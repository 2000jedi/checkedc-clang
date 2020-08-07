//=--TypeVariableAnalysis.cpp-------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Classes dealing with the processing of type variables for generic functions.
//===----------------------------------------------------------------------===//

#include "clang/CConv/TypeVariableAnalysis.h"

using namespace llvm;
using namespace clang;

std::set<ConstraintVariable *> &TypeVariableEntry::getConstraintVariables() {
  assert("Accessing ConstraintVariable set for inconsistent Type Variable." &&
      IsConsistent);
  return ArgConsVars;
}

void TypeVariableEntry::insertConstraintVariables
    (std::set<ConstraintVariable *> &CVs) {
  assert("Accessing ConstraintVariable set for inconsistent Type Variable." &&
      IsConsistent);
  ArgConsVars.insert(CVs.begin(), CVs.end());
}

void TypeVariableEntry::setTypeParamConsVar(ConstraintVariable *CV) {
  assert("Accessing constraint variable for inconsistent Type Variable." &&
      IsConsistent);
  assert("Setting constraint variable to null" && CV != nullptr);
  assert("Changing already set constraint variable" &&
      TypeParamConsVar == nullptr);
  TypeParamConsVar = CV;
}

void TypeVariableEntry::updateEntry(QualType Ty,
                                    std::set<ConstraintVariable *> &CVs) {
  const clang::Type *PtrTy = Ty->getPointeeOrArrayElementType();
  if (isTypeAnonymous(PtrTy)) {
    // We'll need a name to provide the type arguments during rewriting, so
    // no anonymous things here.
    IsConsistent = false;
  } else if (IsConsistent
      && getType()->getPointeeOrArrayElementType() != PtrTy) {
    // If it has previously been instantiated as a different type, its use
    // is not consistent.
    IsConsistent = false;
  } else if (IsConsistent) {
    // Type variable has been encountered before with the same type. Insert
    // new constraint variables.
    insertConstraintVariables(CVs);
  }
  // If none of the above branches are hit, then this was already inconsistent,
  // so there's no need to update anything.
}

ConstraintVariable *TypeVariableEntry::getTypeParamConsVar() {
  assert("Accessing constraint variable for inconsistent Type Variable." &&
      IsConsistent);
  assert("Accessing null constraint variable" && TypeParamConsVar != nullptr);
  return TypeParamConsVar;
}

QualType TypeVariableEntry::getType() {
  assert("Accessing Type for inconsistent Type Variable." &&
      IsConsistent);
  return TyVarType;
}

bool TypeVariableEntry::getIsConsistent() const {
  return IsConsistent;
}

// Finds cast expression that contain function call to a generic function. If
// the return type of the function uses a type variable, a binding for the
// return is added to the type variable map.
bool TypeVarVisitor::VisitCastExpr(CastExpr *CE) {
  Expr *SubExpr = CE->getSubExpr();
  if (CHKCBindTemporaryExpr *TempE = dyn_cast<CHKCBindTemporaryExpr>(SubExpr))
    SubExpr = TempE->getSubExpr();

  if (auto *Call = dyn_cast<CallExpr>(SubExpr))
    if (auto *FD = dyn_cast_or_null<FunctionDecl>(Call->getCalleeDecl()))
      if (const auto *TyVar = getTypeVariableType(FD)) {
        clang::QualType Ty = CE->getType();
        std::set<ConstraintVariable *>
            CVs = CR.getExprConstraintVars(SubExpr);
        insertBinding(Call, TyVar, Ty, CVs);
      }
  return true;
}

bool TypeVarVisitor::VisitCallExpr(CallExpr *CE) {
  if (FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(CE->getCalleeDecl())) {
    // Visit each function argument, and if it use a type variable, insert it
    // into the type variable binding map.
    unsigned int I = 0;
    for (auto *const A : CE->arguments()) {
      // This can happen with varargs
      if (I >= FD->getNumParams())
        break;
      if (const auto *TyVar = getTypeVariableType(FD->getParamDecl(I))) {
        Expr *Uncast = A->IgnoreImpCasts();
        std::set<ConstraintVariable *> CVs = CR.getExprConstraintVars(Uncast);
        insertBinding(CE, TyVar, Uncast->getType(), CVs);
      }
      ++I;
    }

    // For each type variable added above, make a new constraint variable to
    // remember the solved pointer type.
    for (auto &TVEntry : TVMap[CE])
      if (TVEntry.second.getIsConsistent()) {
        std::string Name =
            FD->getNameAsString() + "_tyarg_" + std::to_string(TVEntry.first);
        PVConstraint *P = new PVConstraint(TVEntry.second.getType(), nullptr,
                                           Name, Info, *Context, nullptr);

        // Constrain this variable GEQ the function arguments using the type
        // variable so if any of them are wild, the type argument will also be
        // an unchecked pointer.
        std::set<ConstraintVariable *> CVs = {P};
        constrainConsVarGeq(CVs, TVEntry.second.getConstraintVariables(),
                            Info.getConstraints(), nullptr, Safe_to_Wild,
                            false, &Info);

        TVEntry.second.setTypeParamConsVar(P);
      }
  }
  return true;
}

// Update the type variable map for a new use of a type variable. For each use
// the exact type variable is identified by the call expression where it is
// used and the index of the type variable type in the function declaration.
void TypeVarVisitor::insertBinding(CallExpr *CE, const TypeVariableType *TyV,
                                   clang::QualType Ty, CVarSet &CVs) {
  assert("Type param must go to pointer or array type."
             && (Ty->isPointerType() || Ty->isArrayType()));

  auto &CallTypeVarMap = TVMap[CE];
  if (CallTypeVarMap.find(TyV->GetIndex()) == CallTypeVarMap.end()){
    // If the type variable hasn't been seen before, add it to the map.
    TypeVariableEntry TVEntry = TypeVariableEntry(Ty, CVs);
    CallTypeVarMap[TyV->GetIndex()] = TVEntry;
  } else {
    // Otherwise, update entry with new type and constraints
    CallTypeVarMap[TyV->GetIndex()].updateEntry(Ty, CVs);
  }
}

// Lookup the of type parameters for a CallExpr that are used consistently.
// Type parameters are identified by their index in the type parameter list and
// consistent parameters are added to the Types set reference.
void TypeVarVisitor::getConsistentTypeParams(CallExpr *CE,
                                             std::set<unsigned int> &Types) {
  // Gather consistent TypeVariables into output set
  auto &CallTypeVarBindings = TVMap[CE];
  for (const auto &TVEntry : CallTypeVarBindings)
    if (TVEntry.second.getIsConsistent())
      Types.insert(TVEntry.first);
}

// Store type param bindings persistently in ProgramInfo so they are available
// during rewriting.
void TypeVarVisitor::setProgramInfoTypeVars() {
  for (const auto &TVEntry : TVMap) {
    bool AllInconsistent = true;
    for (auto TVCallEntry : TVEntry.second)
      AllInconsistent &= !TVCallEntry.second.getIsConsistent();
    // If they're all inconsistent type variables, ignore the call expression
    if (!AllInconsistent) {
      // Add each type variable into the map in ProgramInfo. Inconsistent
      // variables are mapped to null.
      for (auto TVCallEntry : TVEntry.second)
        if (TVCallEntry.second.getIsConsistent())
          Info.setTypeParamBinding(TVEntry.first, TVCallEntry.first,
                                   TVCallEntry.second.getTypeParamConsVar(),
                                   Context);
        else
          Info.setTypeParamBinding(TVEntry.first, TVCallEntry.first, nullptr,
                                   Context);
    }
  }
}