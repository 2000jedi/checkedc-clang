//=--ArrayBoundsInferenceConsumer.cpp ----------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of all the methods of the class ArrayBoundsInferenceConsumer.
//
//===----------------------------------------------------------------------===//

#include "clang/CConv/ArrayBoundsInferenceConsumer.h"
#include "clang/CConv/ConstraintResolver.h"

#include <sstream>

static std::set<std::string> LengthVarNamesPrefixes = {"len", "count",
                                                               "size", "num",
                                                               "siz"};
static std::set<std::string> LengthVarNamesSubstring = {"length"};
#define PREFIXPERCMATCH 50.0
#define COMMONSUBSEQUENCEPERCMATCH 80.0

// Name based heuristics.
static bool hasNameMatch(std::string PtrName, std::string FieldName) {
  // If the name field starts with ptrName?
  if (FieldName.rfind(PtrName, 0) == 0)
    return true;

  return false;
}

std::string commonPrefixUtil(std::string Str1, std::string Str2) {
  auto MRes = std::mismatch(Str1.begin(), Str1.end(), Str2.begin());
  return Str1.substr(0, MRes.first - Str1.begin());
}

static bool prefixNameMatch(std::string PtrName, std::string FieldName) {
    std::string Prefix = commonPrefixUtil(PtrName, FieldName);

    if (Prefix.length() > 0) {
      return ((Prefix.length() * 100.0) / (PtrName.length() * 1.0)) >
             PREFIXPERCMATCH;
    }

    return false;
}

static bool nameSubStringMatch(std::string PtrName, std::string FieldName) {
  // Convert the names to lower case.
  std::transform(PtrName.begin(), PtrName.end(), PtrName.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  std::transform(FieldName.begin(), FieldName.end(), FieldName.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  unsigned SubSeqLen = longestCommonSubsequence(PtrName.c_str(), FieldName.c_str(),
                               PtrName.length(), FieldName.length());
  if (SubSeqLen > 0) {
    // Check if we get 80% match on the common subsequence matching on the
    // variable name of length and the name of array.
    return ((SubSeqLen * 100.0) / (PtrName.length() * 1.0)) >=
           COMMONSUBSEQUENCEPERCMATCH;
  }
  return false;

}

static bool fieldNameMatch(std::string FieldName) {
  // Convert the field name to lower case.
  std::transform(FieldName.begin(), FieldName.end(), FieldName.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  for (auto &PName : LengthVarNamesPrefixes) {
    if (FieldName.rfind(PName, 0) == 0)
      return true;
  }

  for (auto &TmpName : LengthVarNamesSubstring) {
    if (FieldName.find(TmpName) != std::string::npos)
      return true;
  }
  return false;
}

static bool hasLengthKeyword(std::string VarName) {
  // Convert the field name to lower case.
  std::transform(VarName.begin(), VarName.end(), VarName.begin(),
                 [](unsigned char c){ return std::tolower(c); });

  std::set<std::string> LengthKeywords(LengthVarNamesPrefixes);
  LengthKeywords.insert(LengthVarNamesSubstring.begin(),
                           LengthVarNamesSubstring.end());
  for (auto &PName : LengthKeywords) {
    if (VarName.find(PName) != std::string::npos)
      return true;
  }
  return false;
}

// Check if the provided constraint variable is an array and it needs bounds.
static bool needArrayBounds(ConstraintVariable *CV,
                            EnvironmentMap &E) {
  if (CV->hasArr(E)) {
    PVConstraint *PV = dyn_cast<PVConstraint>(CV);
    if (PV && PV->getArrPresent())
      return false;
    return true;
  }
  return false;
}

static bool needNTArrayBounds(ConstraintVariable *CV,
                              EnvironmentMap &E) {
  if (CV->hasNtArr(E)) {
    PVConstraint *PV = dyn_cast<PVConstraint>(CV);
    if (PV && PV->getArrPresent())
      return false;
    return true;
  }
  return false;
}

static bool needArrayBounds(Expr *E, ProgramInfo &Info, ASTContext *C) {
  ConstraintResolver CR(Info, C);
  std::set<ConstraintVariable *> ConsVar = CR.getExprConstraintVars(E);
  for (auto CurrCVar : ConsVar) {
    if (needArrayBounds(CurrCVar, Info.getConstraints().getVariables()))
      return true;
    return false;
  }
  return false;
}

static bool needArrayBounds(Decl *D, ProgramInfo &Info, ASTContext *C,
                            bool IsNtArr = false) {
  std::set<ConstraintVariable *> ConsVar = Info.getVariable(D, C);
  auto &E = Info.getConstraints().getVariables();
  for (auto CurrCVar : ConsVar) {
    if ((!IsNtArr && needArrayBounds(CurrCVar, E)) ||
        (IsNtArr && needNTArrayBounds(CurrCVar, E)))
      return true;
    return false;
  }
  return false;
}

// Map that contains association of allocator functions and indexes of
// parameters that correspond to the size of the object being assigned.
static std::map<std::string, std::set<unsigned>> AllocatorSizeAssoc = {
                                                {"malloc", {0}},
                                                {"calloc", {0, 1}}};


// Get the name of the function called by this call expression.
static std::string getCalledFunctionName(const Expr *E) {
  const CallExpr *CE = dyn_cast<CallExpr>(E);
  assert(CE && "The provided expression should be a call expression.");
  const FunctionDecl *CalleeDecl = dyn_cast<FunctionDecl>(CE->getCalleeDecl());
  if (CalleeDecl && CalleeDecl->getDeclName().isIdentifier())
    return CalleeDecl->getName();
  return "";
}

// Check if the provided expression is a call to one of the known
// memory allocators.
static bool isAllocatorCall(Expr *E, std::string &FName,
                            std::vector<Expr *> &ArgVals) {
  bool RetVal = false;
  if (CallExpr *CE = dyn_cast<CallExpr>(removeAuxillaryCasts(E)))
    if (CE->getCalleeDecl() != nullptr) {
      // Is this a call to a named function?
      FName = getCalledFunctionName(CE);
      // check if the called function is a known allocator?
      if (AllocatorSizeAssoc.find(FName) !=
             AllocatorSizeAssoc.end()) {
        RetVal = true;
        // First get all base expressions.
        std::vector<Expr *> BaseExprs;
        BaseExprs.clear();
        for (auto Pidx : AllocatorSizeAssoc[FName]) {
         Expr *PExpr = CE->getArg(Pidx);
         BinaryOperator *BO = dyn_cast<BinaryOperator>(PExpr);
         UnaryExprOrTypeTraitExpr *UExpr =
             dyn_cast<UnaryExprOrTypeTraitExpr>(PExpr);
         if (BO && BO->isMultiplicativeOp()) {
           BaseExprs.push_back(BO->getLHS());
           BaseExprs.push_back(BO->getRHS());
         } else if(UExpr && UExpr->getKind() == UETT_SizeOf) {
           BaseExprs.push_back(UExpr);
         } else {
           RetVal = false;
           break;
         }
        }

        // Check if each of the expression is either sizeof or a DeclRefExpr
        if (RetVal && !BaseExprs.empty()) {
          for (auto *TmpE : BaseExprs) {
            UnaryExprOrTypeTraitExpr *UExpr =
                dyn_cast<UnaryExprOrTypeTraitExpr>(TmpE);
            if (isa<DeclRefExpr>(TmpE) ||
                (UExpr && UExpr->getKind() == UETT_SizeOf)) {
              ArgVals.push_back(TmpE);
            } else {
              RetVal = false;
              break;
            }
          }
        }
      }
    }
  return RetVal;
}

static void handleAllocatorCall(QualType LHSType, BoundsKey LK, Expr *E,
                                ProgramInfo &Info, ASTContext *Context) {
  auto &AVarBInfo = Info.getABoundsInfo();
  auto &ABStats = AVarBInfo.getBStats();
  ConstraintResolver CR(Info, Context);
  std::string FnName;
  std::vector<Expr *> ArgVals;
  // is the RHS expression a call to allocator function?
  if (isAllocatorCall(E, FnName, ArgVals)) {
    BoundsKey RK;
    bool FoundKey = false;
    // We consider everything as byte_count unless we see a sizeof
    // expression in which case if the type matches we use count bounds.
    bool IsByteBound = true;
    for (auto *TmpE : ArgVals) {
      UnaryExprOrTypeTraitExpr *arg = dyn_cast<UnaryExprOrTypeTraitExpr>(TmpE);
      if (arg && arg->getKind() == UETT_SizeOf) {
        QualType STy = Context->getPointerType(arg->getTypeOfArgument());
        // This is a count bound.
        if (LHSType == STy) {
          IsByteBound = false;
        } else {
          FoundKey = false;
          break;
        }
      } else if (AVarBInfo.getVariable(TmpE, *Context, RK)) {
        // Is this variable?
        if (!FoundKey) {
          FoundKey = true;
        } else {
          // Multiple variables found.
          FoundKey = false;
          break;
        }
      } else {
        // Unrecognized expression.
        FoundKey = false;
        break;
      }
    }

    if (FoundKey) {
      // If we found BoundsKey from the allocate expression?
      auto *PrgLVar = AVarBInfo.getProgramVar(LK);
      auto *PrgRVar = AVarBInfo.getProgramVar(RK);
      // Either both should be in same scope or the RHS should be constant.
      if (PrgLVar->getScope() == PrgRVar->getScope() ||
          PrgRVar->IsNumConstant()) {
        ABounds *LBounds = nullptr;
        if (IsByteBound) {
          LBounds = new ByteBound(RK);
        } else {
          LBounds = new CountBound(RK);
        }
        ABStats.AllocatorMatch.insert(LK);
        if (!AVarBInfo.mergeBounds(LK, LBounds)) {
          delete (LBounds);
        }
      }
    }
  }
}

// Check if expression is a simple local variable i.e., ptr = .if yes, return
// the referenced local variable as the return value of the argument.
bool isExpressionSimpleLocalVar(Expr *ToCheck, VarDecl **TargetDecl) {
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(ToCheck))
    if (DeclaratorDecl *FD = dyn_cast<DeclaratorDecl>(DRE->getDecl()))
      if (!dyn_cast<FieldDecl>(FD) && !dyn_cast<ParmVarDecl>(FD))
        if (VarDecl *VD = dyn_cast<VarDecl>(FD))
          if (!VD->hasGlobalStorage()) {
            *TargetDecl = VD;
            return true;
          }
  return false;
}

bool isExpressionStructField(Expr *ToCheck, FieldDecl **TargetDecl) {
  if (MemberExpr *DRE = dyn_cast<MemberExpr>(ToCheck)) {
    if (FieldDecl *FD = dyn_cast<FieldDecl>(DRE->getMemberDecl())) {
      *TargetDecl = FD;
      return true;
    }
  }
  return false;
}

// This visitor handles the bounds of function local array variables.

void GlobalABVisitor::SetParamHeuristicInfo(LocalVarABVisitor *LAB) {
  this->ParamInfo = LAB;
}

bool GlobalABVisitor::IsPotentialLengthVar(ParmVarDecl *PVD) {
  if (PVD->getType().getTypePtr()->isIntegerType()) {
    return ParamInfo == nullptr || !ParamInfo->isNonLengthParameter(PVD);
  }
  return false;
}

// This handles the length based heuristics for structure fields.
bool GlobalABVisitor::VisitRecordDecl(RecordDecl *RD) {
  // For each of the struct or union types.
  if (RD->isStruct() || RD->isUnion()) {
    // Get fields that are identified as arrays and also fields that could be
    // potential be the length fields
    std::set<std::pair<std::string, BoundsKey>> PotLenFields;
    std::set<std::pair<std::string, BoundsKey>> IdentifiedArrVars;
    const auto &AllFields = RD->fields();
    auto &ABInfo = Info.getABoundsInfo();
    auto &ABStats = ABInfo.getBStats();
    for (auto *Fld : AllFields) {
      FieldDecl *FldDecl = dyn_cast<FieldDecl>(Fld);
      BoundsKey FldKey;
      std::string FldName = FldDecl->getNameAsString();
      // This is an integer field and could be a length field
      if (FldDecl->getType().getTypePtr()->isIntegerType() &&
          ABInfo.getVariable(FldDecl, FldKey))
        PotLenFields.insert(std::make_pair(FldName, FldKey));
      // Is this an array field?
      if (needArrayBounds(FldDecl, Info, Context) &&
          ABInfo.getVariable(FldDecl, FldKey))
        IdentifiedArrVars.insert(std::make_pair(FldName, FldKey));
    }

    if (IdentifiedArrVars.size() > 0 && PotLenFields.size() > 0) {
      // First check for variable name match.
      for (auto &PtrField : IdentifiedArrVars) {
        for (auto &LenField : PotLenFields) {
          if (hasNameMatch(PtrField.first,
                           LenField.first)) {
            ABounds *FldBounds = new CountBound(LenField.second);
            // If we find a field which matches both the pointer name and
            // variable name heuristic lets use it.
            if (hasLengthKeyword(LenField.first)) {
              ABStats.NamePrefixMatch.insert(PtrField.second);
              ABInfo.replaceBounds(PtrField.second, FldBounds);
              break;
            }
            ABStats.VariableNameMatch.insert(PtrField.second);
            ABInfo.replaceBounds(PtrField.second, FldBounds);
          }
        }
        // If the name-correspondence heuristics failed.
        // Then use the named based heuristics.
        /*if (!ArrBInfo.hasBoundsInformation(PtrField)) {
          for (auto LenField : PotLenFields) {
            if (fieldNameMatch(LenField->getNameAsString()))
              ArrBInfo.addBoundsInformation(PtrField, LenField);
          }
        }*/
      }
    }
  }
  return true;
}

bool GlobalABVisitor::VisitFunctionDecl(FunctionDecl *FD) {
  // If we have seen the body of this function? Then try to guess the length
  // of the parameters that are arrays.
  if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
    auto &ABInfo = Info.getABoundsInfo();
    auto &ABStats = ABInfo.getBStats();
    const clang::Type *Ty = FD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
    const FunctionProtoType *FT = Ty->getAs<FunctionProtoType>();
    if (FT != nullptr) {
      std::map<ParmVarDecl *, std::set<ParmVarDecl *>> ArrVarLenMap;
      std::map<unsigned , std::pair<std::string, BoundsKey>> ParamArrays;
      std::map<unsigned, std::pair<std::string, BoundsKey>> ParamNtArrays;
      std::map<unsigned , std::pair<std::string, BoundsKey>> LengthParams;

      for (unsigned i = 0; i < FT->getNumParams(); i++) {
        ParmVarDecl *PVD = FD->getParamDecl(i);
        BoundsKey PK;

        if (ABInfo.getVariable(PVD, PK)) {
          auto PVal = std::make_pair(PVD->getNameAsString(), PK);
          if (needArrayBounds(PVD, Info, Context)) {
            // Is this an array?
            ParamArrays[i] = PVal;
          }
          if (needArrayBounds(PVD, Info, Context, true)) {
            // Is this an NTArray?
            ParamNtArrays[i] = PVal;
          }
          // If this is a length field?
          if (IsPotentialLengthVar(PVD))
            LengthParams[i] = PVal;
        }
      }
      if (!ParamArrays.empty() && !LengthParams.empty()) {
        // We have multiple parameters that are arrays and multiple params
        // that could be potentially length fields.
        for (auto &ArrParamPair : ParamArrays) {
          bool FoundLen = false;

          // If this is right next to the array param?
          // Then most likely this will be a length field.
          unsigned PIdx = ArrParamPair.first;
          BoundsKey PBKey = ArrParamPair.second.second;
          if (LengthParams.find(PIdx +1) != LengthParams.end()) {
            ABounds *PBounds = new CountBound(LengthParams[PIdx+1].second);
            ABInfo.replaceBounds(PBKey, PBounds);
            ABStats.NeighbourParamMatch.insert(PBKey);
            continue;
          }

          for (auto &LenParamPair : LengthParams) {
            // If the name of the length field matches.
            if (hasNameMatch(ArrParamPair.second.first,
                             LenParamPair.second.first)) {
              FoundLen = true;
              ABounds *PBounds = new CountBound(LenParamPair.second.second);
              ABInfo.replaceBounds(PBKey, PBounds);
              ABStats.NamePrefixMatch.insert(PBKey);
              break;
            }

            if (nameSubStringMatch(ArrParamPair.second.first,
                                   LenParamPair.second.first)) {
              FoundLen = true;
              ABounds *PBounds = new CountBound(LenParamPair.second.second);
              ABInfo.replaceBounds(PBKey, PBounds);
              ABStats.NamePrefixMatch.insert(PBKey);
              continue;
            }
          }

          if (!FoundLen) {
            for (auto &currLenParamPair : LengthParams) {
              // Check if the length parameter name matches our heuristics.
              if (fieldNameMatch(currLenParamPair.second.first)) {
                FoundLen = true;
                ABounds *PBounds = new CountBound(currLenParamPair.second.second);
                ABInfo.replaceBounds(PBKey, PBounds);
                ABStats.VariableNameMatch.insert(PBKey);
              }
            }
          }

          if (!FoundLen) {
            llvm::errs() << "[-] Array variable length not found:" <<
                            ArrParamPair.second.first << "\n";
          }

        }
      }

      for (auto &CurrNtArr : ParamNtArrays) {
        unsigned PIdx = CurrNtArr.first;
        BoundsKey PBKey = CurrNtArr.second.second;
        if (LengthParams.find(PIdx +1) != LengthParams.end()) {
          if (fieldNameMatch(LengthParams[PIdx +1].first)) {
            ABounds *PBounds = new CountBound(LengthParams[PIdx +1].second);
            ABInfo.replaceBounds(PBKey, PBounds);
            ABStats.VariableNameMatch.insert(PBKey);
            continue;
          }
        }
      }
    }
  }
  return true;
}


bool LocalVarABVisitor::VisitBinAssign(BinaryOperator *O) {
  Expr *LHS = O->getLHS()->IgnoreParenCasts();
  Expr *RHS = O->getRHS()->IgnoreParenCasts();
  auto &AVarBInfo = Info.getABoundsInfo();
  ConstraintResolver CR(Info, Context);
  std::string FnName;
  std::vector<Expr *> ArgVals;
  BoundsKey LK;
  // is the RHS expression a call to allocator function?
  if (needArrayBounds(LHS, Info, Context) &&
      AVarBInfo.getVariable(LHS, *Context, LK)) {
    handleAllocatorCall(LHS->getType(), LK, RHS, Info, Context);
  }

  // Any parameter directly used as a condition in ternary expression
  // cannot be length.
  if (ConditionalOperator *CO = dyn_cast<ConditionalOperator>(RHS))
    addUsedParmVarDecl(CO->getCond());

  return true;
}

void LocalVarABVisitor::addUsedParmVarDecl(Expr *CE) {
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(CE->IgnoreParenCasts()))
    if (ParmVarDecl *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl()))
      NonLengthParameters.insert(PVD);
}

bool LocalVarABVisitor::VisitIfStmt(IfStmt *IFS) {
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(IFS->getCond())) {
    BinaryOperator::Opcode BOpcode = BO->getOpcode();
    if (BOpcode == BinaryOperator::Opcode::BO_EQ ||
        BOpcode == BinaryOperator::Opcode::BO_NE) {
      addUsedParmVarDecl(BO->getLHS());
      addUsedParmVarDecl(BO->getRHS());
    }
  }
  return true;
}

bool LocalVarABVisitor::VisitDeclStmt(DeclStmt *S) {
  // Build rules based on initializers.
  auto &ABoundsInfo = Info.getABoundsInfo();
  for (const auto &D : S->decls())
    if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      Expr *InitE = VD->getInit();
      BoundsKey DeclKey;
      if (InitE != nullptr) {
        clang::StringLiteral *SL =
            dyn_cast<clang::StringLiteral>(InitE->IgnoreParenCasts());
        if (ABoundsInfo.getVariable(VD, DeclKey)) {
          handleAllocatorCall(VD->getType(), DeclKey, InitE,
                              Info, Context);
          if (SL != nullptr) {
            ABounds *ByBounds =
                new ByteBound(ABoundsInfo.getConstKey(SL->getByteLength()));
            if (!ABoundsInfo.mergeBounds(DeclKey, ByBounds)) {
              delete (ByBounds);
            }
          }
        }
      }
    }

  return true;
}

bool LocalVarABVisitor::VisitSwitchStmt(SwitchStmt *S) {
  VarDecl *CondVar = S->getConditionVariable();

  // If this is a parameter declaration? Then this parameter cannot be length.
  if (CondVar != nullptr)
    if (ParmVarDecl *PD = dyn_cast<ParmVarDecl>(CondVar))
      NonLengthParameters.insert(PD);

  return true;
}

// Check if the provided parameter cannot be a length of an array.
bool LocalVarABVisitor::isNonLengthParameter(ParmVarDecl *PVD) {
  if (PVD->getType().getTypePtr()->isEnumeralType())
    return true;
  return NonLengthParameters.find(PVD) != NonLengthParameters.end();
}

void AddMainFuncHeuristic(ASTContext *C, ProgramInfo &I, FunctionDecl *FD) {
  if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
    // Heuristic: If the function has just a single parameter
    // and we found that it is an array then it must be an Nt_array.
    const clang::Type *Ty = FD->getTypeSourceInfo()->getTypeLoc().getTypePtr();
    const FunctionProtoType *FT = Ty->getAs<FunctionProtoType>();
    auto &ABInfo = I.getABoundsInfo();
    if (FT != nullptr) {
      if (FD->getNameInfo().getAsString() == std::string("main") &&
                 FT->getNumParams() == 2) {
        // If the function is `main` then we know second argument is _Array_ptr.
        ParmVarDecl *Argv = FD->getParamDecl(1);
        assert(Argv != nullptr && "Argument cannot be nullptr");
        BoundsKey ArgvKey;
        BoundsKey ArgcKey;
        if (needArrayBounds(Argv, I, C) &&
            ABInfo.getVariable(Argv, ArgvKey) &&
            ABInfo.getVariable(FD->getParamDecl(0), ArgcKey)) {
          ABounds *ArgcBounds = new CountBound(ArgcKey);
          ABInfo.replaceBounds(ArgvKey, ArgcBounds);
        }
      }
    }
  }

}

void HandleArrayVariablesBoundsDetection(ASTContext *C, ProgramInfo &I) {
  // Run array bounds
  GlobalABVisitor GlobABV(C, I);
  TranslationUnitDecl *TUD = C->getTranslationUnitDecl();
  LocalVarABVisitor LFV = LocalVarABVisitor(C, I);
  bool GlobalTraversed;
  // First visit all the structure members.
  for (const auto &D : TUD->decls()) {
    GlobalTraversed = false;
    if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
      if (FD->hasBody() && FD->isThisDeclarationADefinition()) {
        // Try to guess the bounds information for function locals.
        Stmt *Body = FD->getBody();
        LFV.TraverseStmt(Body);
        // Set information collected after analyzing the function body.
        GlobABV.SetParamHeuristicInfo(&LFV);
        GlobABV.TraverseDecl(D);
        GlobalTraversed = true;
      }
    }
    // If this is not already traversed?
    if (!GlobalTraversed)
      GlobABV.TraverseDecl(D);
    GlobABV.SetParamHeuristicInfo(nullptr);
  }
}
