//=--ProgramInfo.cpp----------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Implementation of ProgramInfo methods.
//===----------------------------------------------------------------------===//

#include "clang/CConv/ProgramInfo.h"
#include "clang/CConv/CCGlobalOptions.h"
#include "clang/CConv/ConstraintBuilder.h"
#include "clang/CConv/MappingVisitor.h"
#include <sstream>

using namespace clang;

ProgramInfo::ProgramInfo() :
  persisted(true) {
  ArrBoundsInfo = new ArrayBoundsInformation(*this);
  ExternalFunctionDeclFVCons.clear();
  ExternalFunctionDefnFVCons.clear();
  StaticFunctionDeclFVCons.clear();
  StaticFunctionDefnFVCons.clear();
  MultipleRewrites = false;
}


void ProgramInfo::merge_MF(ParameterMap &mf) {
  for (auto kv : mf) {
    MF[kv.first] = kv.second;
  }
}


ParameterMap &ProgramInfo::get_MF() {
  return MF;
}

void dumpExtFuncMap(const ProgramInfo::ExternalFunctionMapType &EMap,
                    raw_ostream &O) {
  for (const auto &DefM : EMap) {
    O << "Func Name:" << DefM.first << " => ";
    for (const auto J : DefM.second) {
      O << "[ ";
      J->print(O);
      O << " ]\n";
    }
    O << "\n";
  }
}

void dumpStaticFuncMap(const ProgramInfo::StaticFunctionMapType &EMap,
                       raw_ostream &O) {
  for (const auto &DefM : EMap) {
    O << "Func Name:" << DefM.first << " => ";
    for (const auto &Tmp : DefM.second) {
      O << " File Name:"<< Tmp.first << " => \n";
      for (const auto J : Tmp.second) {
        O << "[ ";
        J->print(O);
        O << "]\n";
      }
      O << "\n";
    }
    O << "\n";
  }
}

void dumpExtFuncMapJson(const ProgramInfo::ExternalFunctionMapType &EMap,
                        raw_ostream &O) {
  bool AddComma = false;
  for (const auto &DefM : EMap) {
    if (AddComma) {
      O << ",\n";
    }
    O << "{\"FuncName\":\"" << DefM.first << "\", \"Constraints\":[";
    bool AddComma1 = false;
    for (const auto J : DefM.second) {
      if (AddComma1) {
        O << ",";
      }
      J->dump_json(O);
      AddComma1 = true;
    }
    O << "]}";
    AddComma = true;
  }
}

void dumpStaticFuncMapJson(const ProgramInfo::StaticFunctionMapType &EMap,
                           raw_ostream &O) {
  bool AddComma = false;
  for (const auto &DefM : EMap) {
    if (AddComma) {
      O << ",\n";
    }
    O << "{\"FuncName\":\"" << DefM.first << "\", \"Constraints\":[";
    bool AddComma1 = false;
    for (const auto J : DefM.second) {
      if (AddComma1) {
        O << ",";
      }
      O << "{\"FileName\":\"" << J.first << "\", \"FVConstraints\":[";
      bool AddComma2 = false;
      for (const auto FV : J.second) {
        if (AddComma2) {
          O << ",";
        }
        FV->dump_json(O);
        AddComma2 = true;
      }
      O << "]}\n";
      AddComma1 = true;
    }
    O << "]}";
    AddComma = true;
  }
}


void ProgramInfo::print(raw_ostream &O) const {
  CS.print(O);
  O << "\n";

  O << "Constraint Variables\n";
  for ( const auto &I : Variables ) {
    PersistentSourceLoc L = I.first;
    const std::set<ConstraintVariable *> &S = I.second;
    L.print(O);
    O << "=>";
    for (const auto &J : S) {
      O << "[ ";
      J->print(O);
      O << " ]";
    }
    O << "\n";
  }

  O << "External Function Definitions\n";
  dumpExtFuncMap(ExternalFunctionDefnFVCons, O);
  O << "External Function Declarations\n";
  dumpExtFuncMap(ExternalFunctionDeclFVCons, O);
  O << "Static Function Definitions\n";
  dumpStaticFuncMap(StaticFunctionDefnFVCons, O);
  O << "Static Function Declarations\n";
  dumpStaticFuncMap(StaticFunctionDeclFVCons, O);
}

void ProgramInfo::dump_json(llvm::raw_ostream &O) const {
  O << "{\"Setup\":";
  CS.dump_json(O);
  // Dump the constraint variables.
  O << ", \"ConstraintVariables\":[";
  bool AddComma = false;
  for ( const auto &I : Variables ) {
    if (AddComma) {
      O << ",\n";
    }
    PersistentSourceLoc L = I.first;
    const std::set<ConstraintVariable *> &S = I.second;

    O << "{\"line\":\"";
    L.print(O);
    O << "\",";
    O << "\"Variables\":[";
    bool AddComma1 = false;
    for (const auto &J : S) {
      if (AddComma1) {
        O << ",";
      }
      J->dump_json(O);
      AddComma1 = true;
    }
    O << "]";
    O << "}";
    AddComma = true;
  }
  O << "]";
  O << ", \"ExternalFunctionDefinitions\":[";
  dumpExtFuncMapJson(ExternalFunctionDefnFVCons, O);
  O << "], \"ExternalFunctionDeclarations\":[";
  dumpExtFuncMapJson(ExternalFunctionDeclFVCons, O);
  O << "], \"StaticFunctionDefinitions\":[";
  dumpStaticFuncMapJson(StaticFunctionDefnFVCons, O);
  O << "], \"StaticFunctionDeclarations\":[";
  dumpStaticFuncMapJson(StaticFunctionDeclFVCons, O);
  O << "]}";
}

// Given a ConstraintVariable V, retrieve all of the unique
// constraint variables used by V. If V is just a 
// PointerVariableConstraint, then this is just the contents 
// of 'vars'. If it either has a function pointer, or V is
// a function, then recurses on the return and parameter
// constraints.
static
CAtoms getVarsFromConstraint(ConstraintVariable *V, CAtoms T) {
  CAtoms R = T;

  if (PVConstraint *PVC = dyn_cast<PVConstraint>(V)) {
    R.insert(R.begin(), PVC->getCvars().begin(), PVC->getCvars().end());
   if (FVConstraint *FVC = PVC->getFV()) 
     return getVarsFromConstraint(FVC, R);
  } else if (FVConstraint *FVC = dyn_cast<FVConstraint>(V)) {
    for (const auto &C : FVC->getReturnVars()) {
      CAtoms tmp = getVarsFromConstraint(C, R);
      R.insert(R.begin(), tmp.begin(), tmp.end());
    }
    for (unsigned i = 0; i < FVC->numParams(); i++) {
      for (const auto &C : FVC->getParamVar(i)) {
        CAtoms tmp = getVarsFromConstraint(C, R);
        R.insert(R.begin(), tmp.begin(), tmp.end());
      }
    }
  }

  return R;
}

// Print out statistics of constraint variables on a per-file basis.
void ProgramInfo::print_stats(std::set<std::string> &F, raw_ostream &O,
                              bool OnlySummary) {
  if (!OnlySummary) {
    O << "Enable itype propagation:" << EnablePropThruIType << "\n";
    O << "Merge multiple function declaration:" << !SeperateMultipleFuncDecls << "\n";
    O << "Sound handling of var args functions:" << HandleVARARGS << "\n";
  }
  std::map<std::string, std::tuple<int, int, int, int, int>> FilesToVars;
  EnvironmentMap Env = CS.getVariables();
  unsigned int totC, totP, totNt, totA, totWi;
  totC = totP = totNt = totA = totWi = 0;

  // First, build the map and perform the aggregation.
  for (auto &I : Variables) {
    std::string FileName = I.first.getFileName();
    if (F.count(FileName)) {
      int varC = 0;
      int pC = 0;
      int ntAC = 0;
      int aC = 0;
      int wC = 0;

      auto J = FilesToVars.find(FileName);
      if (J != FilesToVars.end())
        std::tie(varC, pC, ntAC, aC, wC) = J->second;

      CAtoms FoundVars;
      for (auto &C : I.second) {
        CAtoms tmp = getVarsFromConstraint(C, FoundVars);
        FoundVars.insert(FoundVars.begin(), tmp.begin(), tmp.end());
      }

      varC += FoundVars.size();
      for (const auto &N : FoundVars) {
        ConstAtom *CA = CS.getAssignment(N);
        switch (CA->getKind()) {
          case Atom::A_Arr:
            aC += 1;
            break;
          case Atom::A_NTArr:
            ntAC += 1;
            break;
          case Atom::A_Ptr:
            pC += 1;
            break;
          case Atom::A_Wild:
            wC += 1;
            break;
          case Atom::A_Var:
          case Atom::A_Const:
            llvm_unreachable("bad constant in environment map");
        }
      }

      FilesToVars[FileName] = std::tuple<int, int, int, int, int>(varC, pC,
                                                                  ntAC, aC, wC);
    }
  }

  // Then, dump the map to output.
  // if not only summary then dump everything.
  if (!OnlySummary) {
    O << "file|#constraints|#ptr|#ntarr|#arr|#wild\n";
  }
  for (const auto &I : FilesToVars) {
    int v, p, nt, a, w;
    std::tie(v, p, nt, a, w) = I.second;

    totC += v;
    totP += p;
    totNt += nt;
    totA += a;
    totWi += w;
    if (!OnlySummary) {
      O << I.first << "|" << v << "|" << p << "|" << nt << "|" << a << "|" << w;
      O << "\n";
    }
  }

  O << "Summary\nTotalConstraints|TotalPtrs|TotalNTArr|TotalArr|TotalWild\n";
  O << totC << "|" << totP << "|" << totNt << "|" << totA << "|" << totWi << "\n";

}

// Check the equality of VTy and UTy. There are some specific rules that
// fire, and a general check is yet to be implemented. 
bool ProgramInfo::checkStructuralEquality(std::set<ConstraintVariable *> V,
                                          std::set<ConstraintVariable *> U,
                                          QualType VTy,
                                          QualType UTy) 
{
  // First specific rule: Are these types directly equal? 
  if (VTy == UTy) {
    return true;
  } else {
    // Further structural checking is TODO.
    return false;
  } 
}

bool ProgramInfo::checkStructuralEquality(QualType D, QualType S) {
  if (D == S)
    return true;

  return D->isPointerType() == S->isPointerType();
}

bool ProgramInfo::isExplicitCastSafe(clang::QualType DstType,
                                     clang::QualType SrcType) {

  // Check if both types are same.
  if (SrcType == DstType)
    return true;

  const clang::Type *SrcTypePtr = SrcType.getTypePtr();
  const clang::Type *DstTypePtr = DstType.getTypePtr();

  const clang::PointerType *SrcPtrTypePtr = dyn_cast<PointerType>(SrcTypePtr);
  const clang::PointerType *DstPtrTypePtr = dyn_cast<PointerType>(DstTypePtr);

  // Both are pointers? check their pointee
  if (SrcPtrTypePtr && DstPtrTypePtr)
    return isExplicitCastSafe(DstPtrTypePtr->getPointeeType(),
                              SrcPtrTypePtr->getPointeeType());
  // Only one of them is pointer?
  if (SrcPtrTypePtr || DstPtrTypePtr)
    return false;

  // If both are not scalar types? Then the types must be exactly same.
  if (!(SrcTypePtr->isScalarType() && DstTypePtr->isScalarType()))
    return SrcTypePtr == DstTypePtr;

  // Check if both types are compatible.
  bool BothNotChar = SrcTypePtr->isCharType() ^ DstTypePtr->isCharType();
  bool BothNotInt =
      SrcTypePtr->isIntegerType() ^ DstTypePtr->isIntegerType();
  bool BothNotFloat =
      SrcTypePtr->isFloatingType() ^ DstTypePtr->isFloatingType();


  return !(BothNotChar || BothNotInt || BothNotFloat);
}

bool ProgramInfo::isExternOkay(std::string Ext) {
  return llvm::StringSwitch<bool>(Ext)
    .Cases("malloc", "free", true)
    .Default(false);
}

bool ProgramInfo::link() {
  // For every global symbol in all the global symbols that we have found
  // go through and apply rules for whether they are functions or variables.
  if (Verbose)
    llvm::errs() << "Linking!\n";

  // Multiple Variables can be at the same PersistentSourceLoc. We should
  // constrain that everything that is at the same location is explicitly
  // equal.
  for (const auto &V : Variables) {
    std::set<ConstraintVariable *> C = V.second;

    if (C.size() > 1) {
      std::set<ConstraintVariable *>::iterator I = C.begin();
      std::set<ConstraintVariable *>::iterator J = C.begin();
      ++J;

      while (J != C.end()) {
        constrainConsVarGeq(*I, *J, CS, nullptr, Same_to_Same, true, this);
        ++I;
        ++J;
      }
    }
  }

  // Equate the constraints for all global variables.
  // This is needed for variables that are defined as extern.
  for (const auto &V : GlobalVariableSymbols) {
    const std::set<PVConstraint *> &C = V.second;

    if (C.size() > 1) {
      std::set<PVConstraint *>::iterator I = C.begin();
      std::set<PVConstraint *>::iterator J = C.begin();
      ++J;
      if (Verbose)
        llvm::errs() << "Global variables:" << V.first << "\n";
      while (J != C.end()) {
        constrainConsVarGeq(*I, *J, CS, nullptr, Same_to_Same, true, this);
        ++I;
        ++J;
      }
    }
  }

  if (!SeperateMultipleFuncDecls) {
    int Gap = 0;
    for (auto &S : ExternalFunctionDeclFVCons) {
      std::string Fname = S.first;
      std::set<FVConstraint *> &P = S.second;

      if (P.size() > 1) {
        std::set<FVConstraint *>::iterator I = P.begin();
        std::set<FVConstraint *>::iterator J = P.begin();
        ++J;

        while (J != P.end()) {
          FVConstraint *P1 = *I;
          FVConstraint *P2 = *J;

          if (P2->hasBody()) { // skip over decl with fun body
            Gap = 1;
            ++J;
            continue;
          }
          // Constrain the return values to be equal.
          if (!P1->hasBody() && !P2->hasBody()) {
            constrainConsVarGeq(P1->getReturnVars(), P2->getReturnVars(), CS,
                                nullptr, Same_to_Same, true, this);

            // Constrain the parameters to be equal, if the parameter arity is
            // the same. If it is not the same, constrain both to be wild.
            if (P1->numParams() == P2->numParams()) {
              for (unsigned i = 0; i < P1->numParams(); i++) {
                constrainConsVarGeq(P1->getParamVar(i), P2->getParamVar(i), CS,
                                    nullptr, Same_to_Same, true, this);
              }

            } else {
              // It could be the case that P1 or P2 is missing a prototype, in
              // which case we don't need to constrain anything.
              if (P1->hasProtoType() && P2->hasProtoType()) {
                // Nope, we have no choice. Constrain everything to wild.
                std::string rsn = "Return value of function:" + P1->getName();
                P1->constrainToWild(CS, rsn, true);
                P2->constrainToWild(CS, rsn, true);
              }
            }
          }
          ++I;
          if (!Gap) {
            ++J;
          } else {
            Gap = 0;
          }
        }
      }
    }
  }


  // For every global function that is an unresolved external, constrain 
  // its parameter types to be wild. Unless it has a bounds-safe annotation. 
  for (const auto &U : ExternFunctions) {
    // If we've seen this symbol, but never seen a body for it, constrain
    // everything about it.
    if (U.second == false && isExternOkay(U.first) == false) {
      // Some global symbols we don't need to constrain to wild, like 
      // malloc and free. Check those here and skip if we find them. 
      std::string FuncName = U.first;
      auto FuncDeclFVIterator =
          ExternalFunctionDeclFVCons.find(FuncName);
      assert(FuncDeclFVIterator != ExternalFunctionDeclFVCons.end());
      const std::set<FVConstraint *> &Gs = (*FuncDeclFVIterator).second;

      for (const auto GIterator : Gs) {
        auto G = GIterator;
        for (const auto &U : G->getReturnVars()) {
          std::string Rsn = "Return value of an external function:" + FuncName;
          U->constrainToWild(CS, Rsn, true);
        }
        std::string rsn = "Inner pointer of a parameter to external function.";
        for (unsigned i = 0; i < G->numParams(); i++)
          for (const auto &PVar : G->getParamVar(i))
            PVar->constrainToWild(CS, rsn, true);
      }
    }
  }

  return true;
}

bool ProgramInfo::isAnExternFunction(const std::string &FName) {
  return !ExternFunctions[FName];
}

void ProgramInfo::seeFunctionDecl(FunctionDecl *F, ASTContext *C) {
  if (!F->isGlobal())
    return;

  // Track if we've seen a body for this function or not.
  std::string Fname = F->getNameAsString();
  if (!ExternFunctions[Fname])
    ExternFunctions[Fname] = (F->isThisDeclarationADefinition() && F->hasBody());

  // Look up the constraint variables for the return type and parameter 
  // declarations of this function, if any.
  /*
  std::set<uint32_t> returnVars;
  std::vector<std::set<uint32_t> > parameterVars(F->getNumParams());
  PersistentSourceLoc PLoc = PersistentSourceLoc::mkPSL(F, *C);
  int i = 0;

  std::set<ConstraintVariable*> FV = getVariable(F, C);
  assert(FV.size() == 1);
  const ConstraintVariable *PFV = (*(FV.begin()));
  assert(PFV != nullptr);
  const FVConstraint *FVC = dyn_cast<FVConstraint>(PFV);
  assert(FVC != nullptr);

  //returnVars = FVC->getReturnVars();
  //unsigned i = 0;
  //for (unsigned i = 0; i < FVC->numParams(); i++) {
  //  parameterVars.push_back(FVC->getParamVar(i));
  //}

  assert(PLoc.valid());
  GlobalFunctionSymbol *GF = 
    new GlobalFunctionSymbol(fn, PLoc, parameterVars, returnVars);

  // Add this to the map of global symbols. 
  std::map<std::string, std::set<GlobalSymbol*> >::iterator it = 
    GlobalFunctionSymbols.find(fn);
  
  if (it == GlobalFunctionSymbols.end()) {
    std::set<GlobalSymbol*> N;
    N.insert(GF);
    GlobalFunctionSymbols.insert(std::pair<std::string, std::set<GlobalSymbol*> >
      (fn, N));
  } else {
    (*it).second.insert(GF);
  }*/
}

void ProgramInfo::seeGlobalDecl(clang::VarDecl *G, ASTContext *C) {
  std::string VarName = G->getName();

  // Add this to the map of global symbols.
  std::set<PVConstraint *> ToAdd;
  // Get the constraint variable directly.
  std::set<ConstraintVariable *> K;
  VariableMap::iterator I = Variables.find(PersistentSourceLoc::mkPSL(G, *C));
  if (I != Variables.end()) {
    K = I->second;
  }
  for (const auto &J : K)
    if (PVConstraint *FJ = dyn_cast<PVConstraint>(J))
      ToAdd.insert(FJ);

  assert(ToAdd.size() > 0);

  if (GlobalVariableSymbols.find(VarName) != GlobalVariableSymbols.end()) {
    GlobalVariableSymbols[VarName].insert(ToAdd.begin(), ToAdd.end());
  } else {
    GlobalVariableSymbols[VarName] = ToAdd;
  }

}

// Populate Variables, VarDeclToStatement, RVariables, and DepthMap with
// AST data structures that correspond do the data stored in PDMap and
// ReversePDMap.
void ProgramInfo::enterCompilationUnit(ASTContext &Context) {
  assert(persisted == true);
  // Get a set of all of the PersistentSourceLoc's we need to fill in.
  std::set<PersistentSourceLoc> P;
  //for (auto I : PersistentVariables)
  //  P.insert(I.first);

  // Resolve the PersistentSourceLoc to one of Decl,Stmt,Type.
  MappingVisitor V(P, Context);
  TranslationUnitDecl *TUD = Context.getTranslationUnitDecl();
  for (const auto &D : TUD->decls())
    V.TraverseDecl(D);

  persisted = false;
  return;
}

// Remove any references we maintain to AST data structure pointers.
// After this, the Variables, VarDeclToStatement, RVariables, and DepthMap
// should all be empty.
void ProgramInfo::exitCompilationUnit() {
  assert(persisted == false);
  persisted = true;
  return;
}

template <typename T>
bool ProgramInfo::hasConstraintType(std::set<ConstraintVariable *> &S) {
  for (const auto &I : S) {
    if (isa<T>(I)) {
      return true;
    }
  }
  return false;
}

bool
ProgramInfo::insertIntoExternalFunctionMap(ExternalFunctionMapType &Map,
                                           const std::string &FuncName,
                                           std::set<FVConstraint *> &ToIns) {
  bool RetVal = false;
  if (Map.find(FuncName) == Map.end()) {
    Map[FuncName] = ToIns;
    RetVal = true;
  } else {
    MultipleRewrites = true;
  }
  return RetVal;
}

bool
ProgramInfo::insertIntoStaticFunctionMap(StaticFunctionMapType &Map,
                                         const std::string &FuncName,
                                         const std::string &FileName,
                                         std::set<FVConstraint *> &ToIns) {
  bool RetVal = false;
  if (Map.find(FuncName) == Map.end()) {
    Map[FuncName][FileName] = ToIns;
    RetVal = true;
  } else if (Map[FuncName].find(FileName) == Map[FuncName].end()) {
    Map[FuncName][FileName] = ToIns;
    RetVal = true;
  } else {
    MultipleRewrites = true;
  }
  return RetVal;
}

void
ProgramInfo::insertNewFVConstraints(FunctionDecl *FD,
                                   std::set<FVConstraint *> &FVcons,
                                   ASTContext *C) {
  std::string FuncName = FD->getNameAsString();
  if (FD->isGlobal()) {
    // external method.
    if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
      // Function definition.
      insertIntoExternalFunctionMap(ExternalFunctionDefnFVCons,
                                    FuncName, FVcons);
    } else {
      insertIntoExternalFunctionMap(ExternalFunctionDeclFVCons,
                                    FuncName, FVcons);
    }
  } else {
    // static method
    auto Psl = PersistentSourceLoc::mkPSL(FD, *C);
    std::string FuncFileName = Psl.getFileName();
    if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
      // Function definition.
      insertIntoStaticFunctionMap(StaticFunctionDefnFVCons, FuncName,
                                  FuncFileName, FVcons);
    } else {
      insertIntoStaticFunctionMap(StaticFunctionDeclFVCons, FuncName,
                                  FuncFileName, FVcons);
    }
  }
}

// For each pointer type in the declaration of D, add a variable to the
// constraint system for that pointer type.
bool ProgramInfo::addVariable(clang::DeclaratorDecl *D,
                              clang::ASTContext *astContext) {
  assert(persisted == false);

  PersistentSourceLoc PLoc = PersistentSourceLoc::mkPSL(D, *astContext);
  assert(PLoc.valid());

  // We only add a PVConstraint or an FVConstraint if the set at
  // Variables[PLoc] does not contain one already. TODO: Explain why would this happen
  std::set<ConstraintVariable *> &S = Variables[PLoc];

  // Function Decls have FVConstraints. Function pointers have PVConstraints;
  // see below
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    const Type *Ty = FD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
    assert(Ty->isFunctionType());

    // Create a function value for the type.
    // process the function constraint only if it doesn't exist
    if (!hasConstraintType<FVConstraint>(S)) {
      FVConstraint *F = new FVConstraint(D, CS, *astContext);
      S.insert(F);

      // If this is a function. Save the created constraint.
      // this needed for resolving function subtypes later.
      // we create a unique key for the declaration and definition
      // of a function.
      // We save the mapping between these unique keys.
      // This is needed so that later when we have to
      // resolve function subtyping. where for each function
      // we need access to teh definition and declaration
      // constraint variables.
      std::string FuncName = FD->getNameAsString();
      // FV Constraints to insert.
      std::set<FVConstraint *> NewFVars;
      NewFVars.insert(F);
      insertNewFVConstraints(FD, NewFVars, astContext);

      // Add mappings from the parameters PLoc to the constraint variables for
      // the parameters.
      // We just created this, so they should be equal.
      assert(FD->getNumParams() == F->numParams());
      for (unsigned i = 0; i < FD->getNumParams(); i++) {
        ParmVarDecl *PVD = FD->getParamDecl(i);
        std::set<ConstraintVariable *> S = F->getParamVar(i);
        if (S.size()) {
          PersistentSourceLoc PSL = PersistentSourceLoc::mkPSL(PVD, *astContext);
          Variables[PSL].insert(S.begin(), S.end());
        }
      }
    }
  } else {
    const Type *Ty = nullptr;
    if (VarDecl *VD = dyn_cast<VarDecl>(D))
      Ty = VD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
    else if (FieldDecl *FD = dyn_cast<FieldDecl>(D))
      Ty = FD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
    else
      llvm_unreachable("unknown decl type");

    // We will add a PVConstraint even for FunPtrs
    if (Ty->isPointerType() || Ty->isArrayType()) {
      // Create a pointer value for the type.
      if (!hasConstraintType<PVConstraint>(S)) {
        PVConstraint *P = new PVConstraint(D, CS, *astContext);
        S.insert(P);
      }
    }
  }

  // The Rewriter won't let us re-write things that are in macros. So, we 
  // should check to see if what we just added was defined within a macro.
  // If it was, we should constrain it to top. This is sad. Hopefully, 
  // someday, the Rewriter will become less lame and let us re-write stuff
  // in macros.
  std::string Rsn = "Pointer in Macro declaration.";
  if (!Rewriter::isRewritable(D->getLocation())) 
    for (const auto &C : S)
      C->constrainToWild(CS, Rsn, false);

  return true;
}

std::string ProgramInfo::getUniqueDeclKey(Decl *D, ASTContext *C) {
  auto Psl = PersistentSourceLoc::mkPSL(D, *C);
  std::string FileName = Psl.getFileName() + ":" +
                         std::to_string(Psl.getLineNo());
  std::string Dname = D->getDeclKindName();
  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    Dname = FD->getNameAsString();
  }
  std::string DeclKey = FileName + ":" + Dname;
  return DeclKey;
}

std::string ProgramInfo::getUniqueFuncKey(FunctionDecl *D,
                                          ASTContext *C) {
  // Get unique key for a function: which is function name,
  // file and line number.
  if (FunctionDecl *FuncDef = getDefinition(D)) {
    D = FuncDef;
  }
  return getUniqueDeclKey(D, C);
}

std::set<FVConstraint *>&
ProgramInfo::getOnDemandFuncDeclarationConstraint(FunctionDecl *D,
                                                  ASTContext *C) {

  std::string FuncName = D->getNameAsString();
  if (D->isGlobal()) {
    // Is this an external function?
    if (ExternalFunctionDeclFVCons.find(FuncName) ==
        ExternalFunctionDeclFVCons.end()) {
      // Create an on demand FVConstraint.
      FVConstraint *F = new FVConstraint(D, CS, *C);
      // Set has body is false, as this is for function declaration.
      F->setHasBody(false);
      ExternalFunctionDeclFVCons[FuncName].insert(F);
    }

    return ExternalFunctionDeclFVCons[FuncName];
  } else {
    // Static function.
    auto Psl = PersistentSourceLoc::mkPSL(D, *C);
    std::string FileName = Psl.getFileName();
    if (StaticFunctionDeclFVCons.find(FuncName) ==
        StaticFunctionDeclFVCons.end() ||
        StaticFunctionDeclFVCons[FuncName].find(FileName) ==
        StaticFunctionDeclFVCons[FuncName].end()) {
      FVConstraint *F = new FVConstraint(D, CS, *C);
      // Set has body is false, as this is for function declaration.
      F->setHasBody(false);
      StaticFunctionDeclFVCons[FuncName][FileName].insert(F);
    }

    return StaticFunctionDeclFVCons[FuncName][FileName];

  }
}

std::set<FVConstraint *> *
ProgramInfo::getFuncDeclConstraints(FunctionDecl *D, ASTContext *C) {
  std::string FuncName = D->getNameAsString();
  if (D->isGlobal()) {
    return getExtFuncDeclConstraintSet(FuncName);
  } else {
    // Static function.
    auto Psl = PersistentSourceLoc::mkPSL(D, *C);
    return getStaticFuncDeclConstraintSet(FuncName, Psl.getFileName());
  }

}

std::set<FVConstraint *> *
ProgramInfo::getFuncDefnConstraints(FunctionDecl *D, ASTContext *C) {

  std::string FuncName = D->getNameAsString();
  if (D->isGlobal()) {
    // Is this an external function?
    if (ExternalFunctionDefnFVCons.find(FuncName) !=
           ExternalFunctionDefnFVCons.end()) {
      return &ExternalFunctionDefnFVCons[FuncName];
    }
  } else {
    // Static function.
    auto Psl = PersistentSourceLoc::mkPSL(D, *C);
    std::string FileName = Psl.getFileName();
    if (StaticFunctionDefnFVCons.find(FuncName) !=
           StaticFunctionDefnFVCons.end() &&
        StaticFunctionDefnFVCons[FuncName].find(FileName) !=
           StaticFunctionDefnFVCons[FuncName].end()) {
      return &StaticFunctionDefnFVCons[FuncName][FileName];
    }
  }
  return nullptr;
}

std::set<ConstraintVariable *>
ProgramInfo::getVariable(clang::Decl *D, clang::ASTContext *C, FunctionDecl *FD,
                         int PIdx) {
  // If this is a parameter.
  if (PIdx >= 0) {
    // Get the parameter index of the requested function declaration.
    D = FD->getParamDecl(PIdx);
  } else {
    // This is the return value of the function.
    D = FD;
  }
  VariableMap::iterator I =
      Variables.find(PersistentSourceLoc::mkPSL(D, *C));
  assert(I != Variables.end());
  return I->second;

}

std::set<ConstraintVariable *>
ProgramInfo::getVariable(clang::Decl *D, clang::ASTContext *C,
                         bool InFuncCtx) {
  // Here, we auto-correct the inFunctionContext flag.
  // If someone is asking for in context variable of a function
  // always give the declaration context.

  // If this a function declaration set in context to false.
  if (dyn_cast<FunctionDecl>(D)) {
    InFuncCtx = false;
  }
  return getVariableOnDemand(D, C, InFuncCtx);
}

std::set<FVConstraint *> *getFuncFVConstraints(FunctionDecl *FD,
                                               ProgramInfo &I,
                                               ASTContext *C,
                                               bool Defn) {
  std::string FuncName = FD->getNameAsString();
  std::set<FVConstraint *> *FunFVars = nullptr;

  if (Defn) {
    // External function definition.
    if (FD->isGlobal()) {
      FunFVars = I.getExtFuncDefnConstraintSet(FuncName);
    } else {
      auto Psl = PersistentSourceLoc::mkPSL(FD, *C);
      std::string FileName = Psl.getFileName();
      FunFVars = I.getStaticFuncDefnConstraintSet(FuncName, FileName);
    }

  }

  if (FunFVars == nullptr) {
    // Try to get declaration constraints.
    FunFVars = &(I.getOnDemandFuncDeclarationConstraint(FD, C));
  }

  return FunFVars;
}


// Given a decl, return the variables for the constraints of the Decl.
std::set<ConstraintVariable *>
ProgramInfo::getVariableOnDemand(Decl *D, ASTContext *C,
                                 bool InFuncCtx) {
  assert(persisted == false);
  // Does this declaration belongs to a function prototype?
  if (dyn_cast<ParmVarDecl>(D) != nullptr ||
      dyn_cast<FunctionDecl>(D) != nullptr) {
    int PIdx = -1;
    FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
    // Is this a parameter? Get the paramter index.
    if (ParmVarDecl *PD = dyn_cast<ParmVarDecl>(D)) {
      // Okay, we got a request for a parameter.
      DeclContext *DC = PD->getParentFunctionOrMethod();
      assert(DC != nullptr);
      FD = dyn_cast<FunctionDecl>(DC);
      // Get the parameter index with in the function.
      for (unsigned i = 0; i < FD->getNumParams(); i++) {
        const ParmVarDecl *tmp = FD->getParamDecl(i);
        if (tmp == D) {
          PIdx = i;
          break;
        }
      }
    }

    // Get corresponding FVConstraint vars.
    std::set<FVConstraint *> *FunFVars = getFuncFVConstraints(FD, *this,
                                                              C, InFuncCtx);

    assert (FunFVars != nullptr && "Unable to find function constraints.");

    if (PIdx != -1) {
      // This is a parameter, get all parameter constraints from FVConstraints.
      std::set<ConstraintVariable *> ParameterCons;
      ParameterCons.clear();
      for (auto fv : *FunFVars) {
        auto currParamConstraint = fv->getParamVar(PIdx);
        ParameterCons.insert(currParamConstraint.begin(),
                             currParamConstraint.end());
      }
      return ParameterCons;
    }

    std::set<ConstraintVariable*> TmpRet;
    TmpRet.insert(FunFVars->begin(), FunFVars->end());
    return TmpRet;
  } else {
    VariableMap::iterator I =
        Variables.find(PersistentSourceLoc::mkPSL(D, *C));
    if (I != Variables.end()) {
      return I->second;
    }
    return std::set<ConstraintVariable *>();
  }
}

VariableMap &ProgramInfo::getVarMap() {
  return Variables;
}

bool ProgramInfo::isAValidPVConstraint(ConstraintVariable *C) {
  if (C != nullptr) {
    if (PVConstraint *PV = dyn_cast<PVConstraint>(C))
      return !PV->getCvars().empty();
  }
  return false;
}


std::set<FVConstraint *> *
    ProgramInfo::getExtFuncDeclConstraintSet(std::string FuncName) {
  if (ExternalFunctionDeclFVCons.find(FuncName) !=
      ExternalFunctionDeclFVCons.end()) {
    return &(ExternalFunctionDeclFVCons[FuncName]);
  }
  return nullptr;
}

std::set<FVConstraint *> *
    ProgramInfo::getExtFuncDefnConstraintSet(std::string FuncName) {
  if (ExternalFunctionDefnFVCons.find(FuncName) !=
      ExternalFunctionDefnFVCons.end()) {
    return &(ExternalFunctionDefnFVCons[FuncName]);
  }
  return nullptr;
}

std::set<FVConstraint *> *
ProgramInfo::getStaticFuncDefnConstraintSet(std::string FuncName,
                                            std::string FileName) {
  if (StaticFunctionDefnFVCons.find(FuncName) !=
      StaticFunctionDefnFVCons.end() &&
      StaticFunctionDefnFVCons[FuncName].find(FileName) !=
          StaticFunctionDefnFVCons[FuncName].end()) {
    return &(StaticFunctionDefnFVCons[FuncName][FileName]);
  }
  return nullptr;
}

std::set<FVConstraint *> *
ProgramInfo::getStaticFuncDeclConstraintSet(std::string FuncName,
                                            std::string FileName) {
  if (StaticFunctionDeclFVCons.find(FuncName) !=
      StaticFunctionDeclFVCons.end() &&
      StaticFunctionDeclFVCons[FuncName].find(FileName) !=
      StaticFunctionDeclFVCons[FuncName].end()) {
    return &(StaticFunctionDeclFVCons[FuncName][FileName]);
  }
  return nullptr;
}

bool
ProgramInfo::applyFunctionDefnDeclsConstraints(std::set<FVConstraint *>
                                                   &DefCVars,
                                               std::set<FVConstraint *>
                                                   &DeclCVars) {
  // We always set inside <: outside for parameters and
  // outside <: inside for returns
  for (auto *DeFV : DefCVars) {
    for (auto *DelFV : DeclCVars) {
      constrainConsVarGeq(DelFV->getReturnVars(), DeFV->getReturnVars(), CS,
                          nullptr, Safe_to_Wild, false, this);

      assert (DeFV->numParams() == DelFV->numParams() &&
             "Definition and Declaration should have same "
             "number of parameters.");
      for (unsigned i=0; i<DeFV->numParams(); i++) {
        constrainConsVarGeq(DeFV->getParamVar(i), DelFV->getParamVar(i), CS,
                            nullptr, Wild_to_Safe, false, this);
      }
    }
  }

  return true;
}

bool ProgramInfo::addFunctionDefDeclConstraints() {
  bool Ret = true;

  for (auto &CurrFDef : ExternalFunctionDefnFVCons) {
    auto FuncName = CurrFDef.first;
    std::set<FVConstraint *> &DefFVCvars = CurrFDef.second;

    // It has declaration?
    if (ExternalFunctionDeclFVCons.find(FuncName) !=
        ExternalFunctionDeclFVCons.end()) {
      applyFunctionDefnDeclsConstraints(DefFVCvars,
                                        ExternalFunctionDeclFVCons[FuncName]);
    }

  }
  for (auto &StFDef : StaticFunctionDefnFVCons) {
    auto FuncName = StFDef.first;
    for (auto &StI : StFDef.second) {
      auto FileName = StI.first;
      std::set<FVConstraint *> &DefFVCvars = StI.second;
      if (StaticFunctionDeclFVCons.find(FuncName) !=
          StaticFunctionDeclFVCons.end() &&
          StaticFunctionDeclFVCons[FuncName].find(FileName) !=
              StaticFunctionDeclFVCons[FuncName].end()) {
        auto &DeclFVs = StaticFunctionDeclFVCons[FuncName][FileName];
        applyFunctionDefnDeclsConstraints(DefFVCvars,
                                          DeclFVs);
      }
    }
  }

  return Ret;
}

bool ProgramInfo::computePointerDisjointSet() {
  ConstraintDisjointSet.Clear();
  CVars WildPtrs;
  WildPtrs.clear();
  auto &WildPtrsReason = ConstraintDisjointSet.RealWildPtrsWithReasons;
  auto &CurrLeaders = ConstraintDisjointSet.Leaders;
  auto &CurrGroups = ConstraintDisjointSet.Groups;
  for (auto currC : CS.getConstraints()) {
    if (Geq *EC = dyn_cast<Geq>(currC)) {
      VarAtom *VLhs = dyn_cast<VarAtom>(EC->getLHS());
      if (dyn_cast<WildAtom>(EC->getRHS())) {
        WildPtrsReason[VLhs->getLoc()].WildPtrReason = EC->getReason();
        if (!EC->FileName.empty() && EC->LineNo != 0) {
          WildPtrsReason[VLhs->getLoc()].IsValid = true;
          WildPtrsReason[VLhs->getLoc()].SourceFileName = EC->FileName;
          WildPtrsReason[VLhs->getLoc()].LineNo = EC->LineNo;
          WildPtrsReason[VLhs->getLoc()].ColStart = EC->ColStart;
        }
        WildPtrs.insert(VLhs->getLoc());
      } else {
        VarAtom *Vrhs = dyn_cast<VarAtom>(EC->getRHS());
        if (Vrhs != nullptr)
          ConstraintDisjointSet.AddElements(VLhs->getLoc(), Vrhs->getLoc());
      }
    }
  }

  // Perform adjustment of group leaders. So that, the real-WILD
  // pointers are the leaders for each group.
  for (auto &RealCp : WildPtrsReason) {
    auto &RealCVar = RealCp.first;
    // check if the leader CVar is a real WILD Ptr
    if (CurrLeaders.find(RealCVar) != CurrLeaders.end()) {
      auto OldGroupLeader = CurrLeaders[RealCVar];
      // If not?
      if (ConstraintDisjointSet.RealWildPtrsWithReasons.find(OldGroupLeader) ==
          ConstraintDisjointSet.RealWildPtrsWithReasons.end()) {
        for (auto &LeadersP : CurrLeaders) {
          if (LeadersP.second == OldGroupLeader) {
            LeadersP.second = RealCVar;
          }
        }

        auto &OldG = CurrGroups[OldGroupLeader];
        CurrGroups[RealCVar].insert(OldG.begin(), OldG.end());
        CurrGroups[RealCVar].insert(RealCVar);
        CurrGroups.erase(OldGroupLeader);
      }
    }
  }

  // Compute non-direct WILD pointers.
  for (auto &Gm : CurrGroups) {
    // Is this group a WILD pointer group?
    if (ConstraintDisjointSet.RealWildPtrsWithReasons.find(Gm.first) !=
        ConstraintDisjointSet.RealWildPtrsWithReasons.end()) {
        ConstraintDisjointSet.TotalNonDirectWildPointers.insert(Gm.second.begin(),
                                                              Gm.second.end());
    }
  }

  CVars TmpCKeys;
  TmpCKeys.clear();
  auto &TotalNDirectWPtrs = ConstraintDisjointSet.TotalNonDirectWildPointers;
  // Remove direct WILD pointers from non-direct wild pointers.
  std::set_difference(TotalNDirectWPtrs.begin(), TotalNDirectWPtrs.end(),
                      WildPtrs.begin(), WildPtrs.end(),
                      std::inserter(TmpCKeys, TmpCKeys.begin()));

  // Update the totalNonDirectWildPointers.
  TotalNDirectWPtrs.clear();
  TotalNDirectWPtrs.insert(TmpCKeys.begin(), TmpCKeys.end());

  for ( const auto &I : Variables ) {
    PersistentSourceLoc L = I.first;
    std::string FilePath = L.getFileName();
    if (canWrite(FilePath)) {
      ConstraintDisjointSet.ValidSourceFiles.insert(FilePath);
    } else {
      continue;
    }
    const std::set<ConstraintVariable *> &S = I.second;
    for (auto *CV : S) {
      if (PVConstraint *PV = dyn_cast<PVConstraint>(CV)) {
        for (auto ck : PV->getCvars()) {
          if (VarAtom *VA = dyn_cast<VarAtom>(ck)) {
            ConstraintDisjointSet.PtrSourceMap[VA->getLoc()] =
                (PersistentSourceLoc*)(&(I.first));
          }
        }
      }
      if (FVConstraint *FV = dyn_cast<FVConstraint>(CV)) {
        for (auto PV : FV->getReturnVars()) {
          if (PVConstraint *RPV = dyn_cast<PVConstraint>(PV)) {
            for (auto ck : RPV->getCvars()) {
              if (VarAtom *VA = dyn_cast<VarAtom>(ck)) {
                ConstraintDisjointSet.PtrSourceMap[VA->getLoc()] =
                    (PersistentSourceLoc*)(&(I.first));
              }
            }
          }
        }
      }
    }
  }


  // Compute all the WILD pointers.
  CVars WildCkeys;
  for (auto &G : CurrGroups) {
    WildCkeys.clear();
    std::set_intersection(G.second.begin(), G.second.end(), WildPtrs.begin(),
                          WildPtrs.end(),
                          std::inserter(WildCkeys, WildCkeys.begin()));

    if (!WildCkeys.empty()) {
      ConstraintDisjointSet.AllWildPtrs.insert(WildCkeys.begin(),
                                               WildCkeys.end());
    }
  }

  return true;
}
