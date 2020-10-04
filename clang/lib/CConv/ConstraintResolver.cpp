//=--ConstraintResolver.cpp---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Implementation of methods in ConstraintResolver.h that help in fetching
// constraints for a given expression.
//===----------------------------------------------------------------------===//

#include "clang/CConv/ConstraintResolver.h"
#include "clang/CConv/CCGlobalOptions.h"

using namespace llvm;
using namespace clang;


ConstraintResolver::~ConstraintResolver() { }

// Force all ConstraintVariables in this set to be WILD
void ConstraintResolver::constraintAllCVarsToWild(const CVarSet &CSet,
                                                  const std::string &rsn,
                                                  Expr *AtExpr) {
  PersistentSourceLoc Psl;
  PersistentSourceLoc *PslP = nullptr;
  if (AtExpr != nullptr) {
    Psl = PersistentSourceLoc::mkPSL(AtExpr, *Context);
    PslP = &Psl;
  }
  auto &CS = Info.getConstraints();

  for (const auto &A : CSet) {
    if (PVConstraint *PVC = dyn_cast<PVConstraint>(A))
      PVC->constrainToWild(CS, rsn, PslP);
    else {
      FVConstraint *FVC = dyn_cast<FVConstraint>(A);
      assert(FVC != nullptr);
      FVC->constrainToWild(CS, rsn, PslP);
    }
  }
}

void ConstraintResolver::constraintCVarToWild(CVarOption CVar,
                                              const std::string &Rsn,
                                              Expr *AtExpr) {
  if (CVar.hasValue()) {
    ConstraintVariable &T = CVar.getValue();
    constraintAllCVarsToWild({&T}, Rsn, AtExpr);
  }
}

// Return a set of PVConstraints equivalent to the set given,
// but dereferenced one level down
CVarSet
    ConstraintResolver::handleDeref(CVarSet T) {
  CVarSet tmp;
  for (const auto &CV : T) {
    PVConstraint *PVC = dyn_cast<PVConstraint>(CV);
    assert (PVC != nullptr); // Shouldn't be dereferencing FPs
    // Subtract one from this constraint. If that generates an empty
    // constraint, then, don't add it
    CAtoms C = PVC->getCvars();
    if (C.size() > 0) {
      C.erase(C.begin());
      if (C.size() > 0) {
        bool a = PVC->getArrPresent();
        bool c = PVC->hasItype();
        std::string d = PVC->getItype();
        FVConstraint *b = PVC->getFV();
        PVConstraint *TmpPV = new PVConstraint(C, PVC->getTy(), PVC->getName(),
                                               b, a, c, d);
        tmp.insert(TmpPV);
      }
    }
  }
  return tmp;
}

// For each constraint variable either invoke addAtom to add an additional level
// of indirection (when the constraint is PVConstraint), or return the constraint
// unchanged (when the constraint is a function constraint).
CVarSet ConstraintResolver::addAtomAll(CVarSet CVS,
                                       ConstAtom *PtrTyp, Constraints &CS) {
  CVarSet Result;
  for (auto *CV : CVS) {
    if (PVConstraint *PVC = dyn_cast<PVConstraint>(CV)) {
      PVConstraint *temp = addAtom(PVC, PtrTyp, CS);
      Result.insert(temp);
    } else {
      Result.insert(CV);
    }
  }
  return Result;
}

// Add to a PVConstraint one additional level of indirection
// The pointer type of the new atom is constrained >= PtrTyp.
PVConstraint *ConstraintResolver::addAtom(PVConstraint *PVC, ConstAtom *PtrTyp,
                                          Constraints &CS) {
  Atom *NewA = CS.getFreshVar("&"+(PVC->getName()), VarAtom::V_Other);
  CAtoms C = PVC->getCvars();
  if (!C.empty()) {
    Atom *A = *C.begin();
    // If PVC is already a pointer, add implication forcing outermost
    //   one to be wild if this added one is
    if (VarAtom *VA = dyn_cast<VarAtom>(A)) {
      auto *Prem = CS.createGeq(NewA, CS.getWild());
      auto *Conc = CS.createGeq(VA, CS.getWild());
      CS.addConstraint(CS.createImplies(Prem, Conc));
    }
  }

  C.insert(C.begin(), NewA);
  bool a = PVC->getArrPresent();
  FVConstraint *b = PVC->getFV();
  bool c = PVC->hasItype();
  std::string d = PVC->getItype();
  PVConstraint *TmpPV = new PVConstraint(C, PVC->getTy(), PVC->getName(),
                                         b, a, c, d);
  TmpPV->constrainOuterTo(CS, PtrTyp, true);
  return TmpPV;
}

static bool getSizeOfArg(Expr *Arg, QualType &ArgTy) {
  if (auto *SizeOf = dyn_cast<UnaryExprOrTypeTraitExpr>(Arg))
    if (SizeOf->getKind() == UETT_SizeOf){
      ArgTy = SizeOf->getTypeOfArgument();
      return true;
    }
  return false;
}

// Processes E from malloc(E) to discern the pointer type this will be
static ConstAtom *analyzeAllocExpr(CallExpr *CE, Constraints &CS,
                                   QualType &ArgTy, std::string FuncName,
                                   ASTContext *Context) {
  if (FuncName.compare("calloc") == 0) {
    if (!getSizeOfArg(CE->getArg(1), ArgTy))
      return nullptr;
    // Check if first argument to calloc is 1
    int Result;
    if (evaluateToInt(CE->getArg(0), *Context, Result) && Result == 1)
      return CS.getPtr();
    else {
      // While calloc can be thought of as returning NT_ARR because it
      // initializes the allocated memory to zero, its type in the checked
      // header file is ARR so, we cannot safely return NT_ARR here.
      return CS.getArr();
    }
  }

  ConstAtom *Ret = CS.getPtr();
  Expr *E;
  if (std::find(AllocatorFunctions.begin(), AllocatorFunctions.end(),
                FuncName) != AllocatorFunctions.end()
      || FuncName.compare("malloc") == 0)
    E = CE->getArg(0);
  else {
    assert(FuncName.compare("realloc") == 0);
    E = CE->getArg(1);
  }
  E = E->IgnoreParenImpCasts();
  BinaryOperator *B = dyn_cast<BinaryOperator>(E);
  std::set<Expr *> Exprs;

  // Looking for X*Y -- could be an array
  if (B && B->isMultiplicativeOp()) {
    Ret = CS.getArr();
    Exprs.insert(B->getLHS());
    Exprs.insert(B->getRHS());
  }
  else
    Exprs.insert(E);

  // Look for sizeof(X); return Arr or Ptr if found
  for (Expr *Ex: Exprs)
    if (getSizeOfArg(Ex, ArgTy))
      return Ret;
  return nullptr;
}

CVarSet ConstraintResolver::getInvalidCastPVCons(Expr *E) {
  CVarSet Ret;
  QualType SrcType, DstType;
  // As getInvalidCastPVCons could be called from non-persistent expressions
  // we need to explicitly store the generated PVConstraints into persistent
  // constraints.
  if (Info.hasPersistentConstraints(E, Context))
    return Info.getPersistentConstraintsSet(E, Context);

  DstType = E->getType();
  SrcType = E->getType();
  if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E))
    SrcType = ICE->getSubExpr()->getType();

  if (ExplicitCastExpr *ECE = dyn_cast<ExplicitCastExpr>(E))
    SrcType = ECE->getSubExpr()->getType();

  auto *P = new PVConstraint(DstType, nullptr, "Invalid cast", Info, *Context);

  PersistentSourceLoc PL = PersistentSourceLoc::mkPSL(E, *Context);
  std::string Rsn =
      "Cast from " + SrcType.getAsString() + " to " + DstType.getAsString();
  P->constrainToWild(Info.getConstraints(), Rsn, &PL);
  Ret = {P};
  Info.storePersistentConstraints(E, Ret, Context);
  return Ret;
}

inline CSetBkeyPair convertToCSetBKeyPair(const CVarSet &Vars) {
  BKeySet EmptyBSet;
  EmptyBSet.clear();
  return std::make_pair(Vars, EmptyBSet);
}

// Returns a set of ConstraintVariables which represent the result of
// evaluating the expression E. Will explore E recursively, but will
// ignore parts of it that do not contribute to the final result
CSetBkeyPair
    ConstraintResolver::getExprConstraintVars(Expr *E) {
  CSetBkeyPair EmptyCSBKeySet;
  BKeySet EmptyBSet;
  if (E != nullptr) {
    auto &CS = Info.getConstraints();
    QualType TypE = E->getType();
    E = E->IgnoreParens();

    // Non-pointer (int, char, etc.) types have a special base PVConstraint
    if (TypE->isRecordType() || TypE->isArithmeticType()) {
      if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
        // If we have a DeclRef, the PVC can get a meaningful name
        return std::make_pair(getBaseVarPVConstraint(DRE), EmptyBSet);
      } else {
        return std::make_pair(PVConstraintFromType(TypE), EmptyBSet);
      }
      // NULL
      // Special handling for casts of null is required to enable rewriting
      // statements such as int *x = (int*) 0. If this was handled as a
      // normal null expression, the cast would never be visited.
    } else if (!isa<ExplicitCastExpr>(E) && isNULLExpression(E, *Context)) {
      return EmptyCSBKeySet;
      // Implicit cast, e.g., T* from T[] or int (*)(int) from int (int),
      //   but also weird int->int * conversions (and back)
    } else if (ImplicitCastExpr *IE = dyn_cast<ImplicitCastExpr>(E)) {
      // We should not use persistent source location for compiler
      // generated constructs.
      QualType SubTypE = IE->getSubExpr()->getType();
      auto CVs = getExprConstraintVars(IE->getSubExpr());
      // if TypE is a pointer type, and the cast is unsafe, return WildPtr
      if (TypE->isPointerType() &&
          !(SubTypE->isFunctionType() || SubTypE->isArrayType() ||
            SubTypE->isVoidPointerType()) &&
          !isCastSafe(TypE, SubTypE)) {
        std::string Rsn = "Cast from " + SubTypE.getAsString() +  " to " +
                          TypE.getAsString();
        constraintAllCVarsToWild(CVs.first, Rsn, IE);
        return std::make_pair(getInvalidCastPVCons(E), EmptyBSet);
      }
      // else, return sub-expression's result
      return CVs;
      // variable (x)
    } else if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
      CVarOption CV = Info.getVariable(DRE->getDecl(), Context);
      assert("Declaration without constraint variable?" && CV.hasValue());
      return convertToCSetBKeyPair({&CV.getValue()});
      // x.f
    } else if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
      CVarOption CV = Info.getVariable(ME->getMemberDecl(), Context);
      assert("Declaration without constraint variable?" && CV.hasValue());
      return convertToCSetBKeyPair({&CV.getValue()});
      // Checked-C temporary
    } else if (CHKCBindTemporaryExpr *CE = dyn_cast<CHKCBindTemporaryExpr>(E)) {
      return getExprConstraintVars(CE->getSubExpr());
    } else {

      // Apart from the above expressions constraints for all the other
      // expressions can be cached.
      // First, check if the expression has constraints that are cached?
      if (Info.hasPersistentConstraints(E, Context))
        return Info.getPersistentConstraints(E, Context);

      CSetBkeyPair Ret = EmptyCSBKeySet;
      if (ExplicitCastExpr *ECE = dyn_cast<ExplicitCastExpr>(E)) {
        assert(ECE->getType() == TypE);
        Expr *TmpE = ECE->getSubExpr();
        // Is cast internally safe? Return WILD if not.
        // If the cast is NULL, it will otherwise seem invalid, but we want to
        // handle it as usual so the type in the cast can be rewritten.
        if (!isNULLExpression(ECE, *Context) && TypE->isPointerType()
            && !isCastSafe(TypE, TmpE->getType())) {
          Ret = convertToCSetBKeyPair(getInvalidCastPVCons(E));
          // NB: Expression ECE itself handled in
          // ConstraintBuilder::FunctionVisitor
        } else {
          CVarSet Vars = getExprConstraintVars(TmpE).first;
          // PVConstraint introduced for explicit cast so they can be rewritten.
          // Pretty much the same idea as CompoundLiteralExpr.
          PVConstraint *P = getRewritablePVConstraint(ECE);
          Ret = convertToCSetBKeyPair({P});
          // ConstraintVars for TmpE when ECE is NULL will be WILD, so
          // constraining GEQ these vars would be the cast always be WILD.
          if (!isNULLExpression(ECE, *Context)) {
            PersistentSourceLoc PL = PersistentSourceLoc::mkPSL(ECE, *Context);
            constrainConsVarGeq(P, Vars, Info.getConstraints(), &PL,
                                Same_to_Same, false, &Info);
          }
        }
      }
        // x = y, x+y, x+=y, etc.
      else if (BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
        switch (BO->getOpcode()) {
          // Assignment, comma operators; only care about LHS.
        case BO_Assign:
        case BO_AddAssign:
        case BO_SubAssign:
          Ret = getExprConstraintVars(BO->getLHS());
          break;
        case BO_Comma:
          Ret = getExprConstraintVars(BO->getRHS());
          break;
          // Possible pointer arithmetic: Could be LHS or RHS.
        case BO_Add:
        case BO_Sub:
          if (BO->getLHS()->getType()->isPointerType())
            Ret = getExprConstraintVars(BO->getLHS());
          else if (BO->getRHS()->getType()->isPointerType())
            Ret = getExprConstraintVars(BO->getRHS());
          else
            Ret = convertToCSetBKeyPair(PVConstraintFromType(TypE));
          break;
          // Pointer-to-member ops unsupported.
        case BO_PtrMemD:
        case BO_PtrMemI:
          assert(false && "Bogus pointer-to-member operator");
          break;
          // Bit-shift/arithmetic/assign/comp operators
          // Ret = ints; do nothing.
        case BO_ShlAssign:
        case BO_ShrAssign:
        case BO_AndAssign:
        case BO_XorAssign:
        case BO_OrAssign:
        case BO_MulAssign:
        case BO_DivAssign:
        case BO_RemAssign:
        case BO_And:
        case BO_Or:
        case BO_Mul:
        case BO_Div:
        case BO_Rem:
        case BO_Xor:
        case BO_Cmp:
        case BO_EQ:
        case BO_NE:
        case BO_GE:
        case BO_GT:
        case BO_LE:
        case BO_LT:
        case BO_LAnd:
        case BO_LOr:
        case BO_Shl:
        case BO_Shr:
          Ret = convertToCSetBKeyPair(PVConstraintFromType(TypE));
          break;
        }
        // x[e]
      } else if (ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
        CSetBkeyPair T = getExprConstraintVars(ASE->getBase());
        CVarSet tmp = handleDeref(T.first);
        T.first.swap(tmp);
        Ret = T;
        // ++e, &e, *e, etc.
      } else if (UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
        Expr *UOExpr = UO->getSubExpr();
        switch (UO->getOpcode()) {
          // &e
          // C99 6.5.3.2: "The operand of the unary & operator shall be
          // either a function designator, the result of a [] or
          // unary * operator, or an lvalue that designates an object that is
          // not a bit-field and is not declared with the register
          // storage-class specifier."
        case UO_AddrOf: {

          UOExpr = UOExpr->IgnoreParenImpCasts();
          // Taking the address of a dereference is a NoOp, so the constraint
          // vars for the subexpression can be passed through.
          // FIXME: We've dumped implicit casts on UOEXpr; restore?
          if (UnaryOperator *SubUO = dyn_cast<UnaryOperator>(UOExpr)) {
            if (SubUO->getOpcode() == UO_Deref)
              Ret = getExprConstraintVars(SubUO->getSubExpr());
            // else, fall through
          } else if (ArraySubscriptExpr *ASE =
              dyn_cast<ArraySubscriptExpr>(UOExpr)) {
            Ret = getExprConstraintVars(ASE->getBase());
          } else {
            // add a VarAtom to UOExpr's PVConstraint, for &
            CSetBkeyPair T = getExprConstraintVars(UOExpr);
            assert("Empty constraint vars in AddrOf!" && !T.first.empty());
            Ret = std::make_pair(addAtomAll(T.first, CS.getPtr(), CS), T.second);
          }
          break;
        }
          // *e
        case UO_Deref: {
          // We are dereferencing, so don't assign to LHS
          CSetBkeyPair T = getExprConstraintVars(UOExpr);
          Ret = std::make_pair(handleDeref(T.first), T.second);
          break;
        }
          /* Operations on lval; if pointer, just process that */
          // e++, e--, ++e, --e
        case UO_PostInc:
        case UO_PostDec:
        case UO_PreInc:
        case UO_PreDec:
          Ret = getExprConstraintVars(UOExpr);
          break;
          /* Integer operators */
          // +e, -e, ~e
        case UO_Plus:
        case UO_Minus:
        case UO_LNot:
        case UO_Not:
          Ret = convertToCSetBKeyPair(PVConstraintFromType(TypE));
          break;
        case UO_Coawait:
        case UO_Real:
        case UO_Imag:
        case UO_Extension:
          assert(false && "Unsupported unary operator");
          break;
        }
        // f(e1,e2, ...)
      } else if (CallExpr *CE = dyn_cast<CallExpr>(E)) {
        // Call expression should always get out-of context constraint variable.
        CVarSet ReturnCVs;
        BKeySet ReturnBSet = EmptyBSet;

        // Here, we need to look up the target of the call and return the
        // constraints for the return value of that function.
        QualType ExprType = E->getType();
        Decl *D = CE->getCalleeDecl();
        CVarSet ReallocFlow;
        if (D == nullptr) {
          // There are a few reasons that we couldn't get a decl. For example,
          // the call could be done through an array subscript.
          Expr *CalledExpr = CE->getCallee();
          CSetBkeyPair tmp = getExprConstraintVars(CalledExpr);
          ReturnBSet = tmp.second;

          for (ConstraintVariable *C : tmp.first) {
            if (FVConstraint *FV = dyn_cast<FVConstraint>(C)) {
              ReturnCVs.insert(FV->getReturnVar());
            } else if (PVConstraint *PV = dyn_cast<PVConstraint>(C)) {
              if (FVConstraint *FV = PV->getFV())
                ReturnCVs.insert(FV->getReturnVar());
            }
          }
        } else if (DeclaratorDecl *FD = dyn_cast<DeclaratorDecl>(D)) {
          /* Allocator call */
          if (isFunctionAllocator(FD->getName())) {
            bool didInsert = false;
            if (CE->getNumArgs() > 0) {
              QualType ArgTy;
              std::string FuncName = FD->getNameAsString();
              ConstAtom *A;
              A = analyzeAllocExpr(CE, CS, ArgTy, FuncName, Context);
              if (A) {
                std::string N = FD->getName();
                N = "&" + N;
                ExprType = Context->getPointerType(ArgTy);
                PVConstraint *PVC =
                    new PVConstraint(ExprType, nullptr, N, Info, *Context, nullptr, true);
                PVC->constrainOuterTo(CS, A, true);
                ReturnCVs.insert(PVC);
                didInsert = true;
                if (FuncName.compare("realloc") == 0) {
                  // We will constrain the first arg to the return of
                  // realloc, below
                  ReallocFlow = getExprConstraintVars(
                      CE->getArg(0)->IgnoreParenImpCasts()).first;
                }
              }
            }
            if (!didInsert)
              ReturnCVs.insert(
                  PVConstraint::getWildPVConstraint(Info.getConstraints()));

            /* Normal function call */
          } else {
            CVarOption CV = Info.getVariable(FD, Context);
            assert(CV.hasValue() && "Function without constraint variable.");
            /* Direct function call */
            if (FVConstraint *FVC = dyn_cast<FVConstraint>(&CV.getValue()))
              ReturnCVs.insert(FVC->getReturnVar());
              /* Call via function pointer */
            else {
              PVConstraint *tmp = dyn_cast<PVConstraint>(&CV.getValue());
              assert(tmp != nullptr);
              if (FVConstraint *FVC = tmp->getFV())
                ReturnCVs.insert(FVC->getReturnVar());
              else {
                // No FVConstraint -- make WILD
                auto *TmpFV = new FVConstraint();
                ReturnCVs.insert(TmpFV);
              }
            }
          }
        } else {
          // If it ISN'T, though... what to do? How could this happen?
          llvm_unreachable("TODO");
        }

        // This is R-Value, we need to make a copy of the resulting
        // ConstraintVariables.
        CVarSet TmpCVs;
        for (ConstraintVariable *CV : ReturnCVs) {
          ConstraintVariable *NewCV;
          auto *PCV = dyn_cast<PVConstraint>(CV);
          if (PCV && PCV->getIsOriginallyChecked()) {
            // Copying needs to be done differently if the constraint variable
            // had a checked type in the input program because these constraint
            // variables contain constant atoms that are reused by the copy
            // constructor.
            NewCV = new PVConstraint(CE->getType(), nullptr, PCV->getName(),
                                     Info, *Context, nullptr,
                                     PCV->getIsGeneric());
            if (PCV->hasBoundsKey())
              NewCV->setBoundsKey(PCV->getBoundsKey());
              
          } else {
            NewCV = CV->getCopy(CS);
          }
          
          // Make the bounds key context sensitive.
          if (NewCV->hasBoundsKey()) {
            auto &ABInfo = Info.getABoundsInfo();
            auto CSensBKey =
                ABInfo.getContextSensitiveBoundsKey(CE,
                                                    NewCV->getBoundsKey());
            NewCV->setBoundsKey(CSensBKey);
          }
          
          // Important: Do Safe_to_Wild from returnvar in this copy, which then
          //   might be assigned otherwise (Same_to_Same) to LHS
          constrainConsVarGeq(NewCV, CV, CS, nullptr, Safe_to_Wild, false,
                              &Info);
          TmpCVs.insert(NewCV);
          // If this is realloc, constrain the first arg to flow to the return
          if (!ReallocFlow.empty()) {
            constrainConsVarGeq(NewCV, ReallocFlow, Info.getConstraints(),
                                nullptr, Wild_to_Safe, false, &Info);
          }
        }
        Ret = std::make_pair(TmpCVs, ReturnBSet);
        // e1 ? e2 : e3
      } else if (ConditionalOperator *CO = dyn_cast<ConditionalOperator>(E)) {
        std::vector<Expr *> SubExprs;
        SubExprs.push_back(CO->getLHS());
        SubExprs.push_back(CO->getRHS());
        Ret = getAllSubExprConstraintVars(SubExprs);
        // { e1, e2, e3, ... }
      } else if (InitListExpr *ILE = dyn_cast<InitListExpr>(E)) {
        std::vector<Expr *> SubExprs = ILE->inits().vec();
        CSetBkeyPair CVars = getAllSubExprConstraintVars(SubExprs);
        if (ILE->getType()->isArrayType()) {
          // Array initialization is similar AddrOf, so the same pattern is
          // used where a new indirection is added to constraint variables.
          Ret = std::make_pair(addAtomAll(CVars.first, CS.getArr(), CS),
                               CVars.second);
        } else {
          // This branch should only be taken on compound literal expressions
          // with pointer type (e.g. int *a = (int*){(int*) 1}).
          // In particular, structure initialization should not reach here,
          // as that caught by the non-pointer check at the top of this
          // method.
          assert("InitlistExpr of type other than array or pointer in "
                 "getExprConstraintVars" &&
              ILE->getType()->isPointerType());
          Ret = CVars;
        }
        // (int[]){e1, e2, e3, ... }
      } else if (CompoundLiteralExpr *CLE =
          dyn_cast<CompoundLiteralExpr>(E)) {
        CSetBkeyPair Vars = getExprConstraintVars(CLE->getInitializer());

        PVConstraint *P = getRewritablePVConstraint(CLE);

        PersistentSourceLoc PL = PersistentSourceLoc::mkPSL(CLE, *Context);
        constrainConsVarGeq(P, Vars.first, Info.getConstraints(), &PL, Same_to_Same,
                            false, &Info);
        CVarSet T = {P};
        Ret = std::make_pair(T, Vars.second);
        // "foo"
      } else if (clang::StringLiteral *Str =
          dyn_cast<clang::StringLiteral>(E)) {
        CVarSet T;
        // If this is a string literal. i.e., "foo".
        // We create a new constraint variable and constraint it to an Nt_array.

        PVConstraint *P =
            new PVConstraint(Str->getType(), nullptr, Str->getStmtClassName(),
                             Info, *Context, nullptr);
        P->constrainOuterTo(CS, CS.getNTArr()); // NB: ARR already there
        T = {P};

        Ret = convertToCSetBKeyPair(T);
      } else if (StmtExpr *SE = dyn_cast<StmtExpr>(E)) {
        CVarSet T;
        // retrieve the last "thing" returned by the block
        Stmt *Res = SE->getSubStmt()->getStmtExprResult();
        if(Expr *ESE = dyn_cast<Expr>(Res)) {
          return getExprConstraintVars(ESE);
        }
      } else {
        if (Verbose) {
          llvm::errs() << "WARNING! Initialization expression ignored: ";
          E->dump(llvm::errs());
          llvm::errs() << "\n";
        }
      }
      Info.storePersistentConstraints(E, Ret, Context);
      return Ret;
    }
  }
  return EmptyCSBKeySet;
}

CVarSet ConstraintResolver::getExprConstraintVarsSet(Expr *E) {
  return getExprConstraintVars(E).first;
}

// Collect constraint variables for Exprs int a set
CSetBkeyPair ConstraintResolver::getAllSubExprConstraintVars(
    std::vector<Expr *> &Exprs) {

  CVarSet AggregateCons;
  BKeySet AggregateBKeys;
  for (const auto &E : Exprs) {
    CSetBkeyPair ECons;
    ECons = getExprConstraintVars(E);
    AggregateCons.insert(ECons.first.begin(), ECons.first.end());
    AggregateBKeys.insert(ECons.second.begin(), ECons.second.end());
  }

  return std::make_pair(AggregateCons, AggregateBKeys);
}

void ConstraintResolver::constrainLocalAssign(Stmt *TSt, Expr *LHS, Expr *RHS,
                                              ConsAction CAction) {
  PersistentSourceLoc PL = PersistentSourceLoc::mkPSL(TSt, *Context);
  CSetBkeyPair L = getExprConstraintVars(LHS);
  CSetBkeyPair R = getExprConstraintVars(RHS);
  constrainConsVarGeq(L.first, R.first, Info.getConstraints(), &PL,
                      CAction, false, &Info);

  // Handle pointer arithmetic.
  auto &ABI = Info.getABoundsInfo();
  ABI.handlePointerAssignment(TSt, LHS, RHS, Context,this);

  // Only if all types are enabled and these are not pointers, then track
  // the assignment.
  if (AllTypes && !containsValidCons(L.first) &&
      !containsValidCons(R.first)) {
    ABI.handleAssignment(LHS, L.first, RHS, R.first, Context, this);
  }
}

void ConstraintResolver::constrainLocalAssign(Stmt *TSt, DeclaratorDecl *D,
                                              Expr *RHS, ConsAction CAction) {
  PersistentSourceLoc PL, *PLPtr = nullptr;
  if (TSt != nullptr) {
   PL = PersistentSourceLoc::mkPSL(TSt, *Context);
   PLPtr = &PL;
  }
  // Get the in-context local constraints.
  CVarOption V = Info.getVariable(D, Context);
  auto RHSCons = getExprConstraintVars(RHS);

  if (V.hasValue())
    constrainConsVarGeq(&V.getValue(), RHSCons.first, Info.getConstraints(), PLPtr,
                        CAction, false, &Info);
  if (AllTypes && !(V.hasValue() && isValidCons(&V.getValue()))
      && !containsValidCons(RHSCons.first)) {
    auto &ABI = Info.getABoundsInfo();
    ABI.handleAssignment(D, V, RHS, RHSCons.first, Context, this);
  }
}

CVarSet ConstraintResolver::getWildPVConstraint() {
  CVarSet Ret;
  Ret.insert(PVConstraint::getWildPVConstraint(Info.getConstraints()));
  return Ret;
}

CVarSet ConstraintResolver::PVConstraintFromType(QualType TypE) {
  CVarSet Ret;
  if (TypE->isRecordType() || TypE->isArithmeticType())
    Ret.insert(PVConstraint::getNonPtrPVConstraint(Info.getConstraints()));
  else if (TypE->isPointerType())
    Ret.insert(PVConstraint::getWildPVConstraint(Info.getConstraints()));
  else
    llvm::errs() << "Warning: Returning non-base, non-wild type";
  return Ret;
}

CVarSet ConstraintResolver::getBaseVarPVConstraint(DeclRefExpr *Decl) {
  assert(Decl->getType()->isRecordType() || Decl->getType()->isArithmeticType());
  CVarSet Ret;
  auto DN = Decl->getDecl()->getName();
  Ret.insert(PVConstraint::getNamedNonPtrPVConstraint(DN,
                                                      Info.getConstraints()));
  return Ret;
}

// Construct a PVConstraint for an expression that can safely be used when
// rewriting the expression later on. This is done by making the constraint WILD
// if the expression is inside a macro.
PVConstraint *ConstraintResolver::getRewritablePVConstraint(Expr *E) {
  PVConstraint *P = new PVConstraint(E->getType(), nullptr,
                                     E->getStmtClassName(), Info, *Context,
                                     nullptr);
  Info.constrainWildIfMacro(P, E->getExprLoc());
  return P;
}

bool ConstraintResolver::containsValidCons(const CVarSet &CVs) {
  for (auto *ConsVar : CVs)
    if (isValidCons(ConsVar))
      return true;
  return false;
}

bool ConstraintResolver::isValidCons(ConstraintVariable *CV) {
  if (PVConstraint *PV = dyn_cast<PVConstraint>(CV))
    return !PV->getCvars().empty();
  return false;
}

bool ConstraintResolver::resolveBoundsKey(const CVarSet &CVs, BoundsKey &BK) {
  if (CVs.size() == 1) {
    auto *OCons = getOnly(CVs);
    return resolveBoundsKey(*OCons, BK);
  }
  return false;
}

bool ConstraintResolver::resolveBoundsKey(CVarOption CVOpt,
                                          BoundsKey &BK) {
  if (CVOpt.hasValue()) {
    ConstraintVariable &CV = CVOpt.getValue();
    if (PVConstraint *PV = dyn_cast<PVConstraint>(&CV))
      if (PV->hasBoundsKey()) {
        BK = PV->getBoundsKey();
        return true;
      }
  }
  return false;
}

bool ConstraintResolver::canFunctionBeSkipped(const std::string &FN) {
  return FN == "realloc";
}