//===--- TypeCheckStmt.cpp - Type Checking for Statements -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

namespace {
/// StmtChecker - This class implements 
class StmtChecker : public StmtVisitor<StmtChecker, Stmt*> {
public:
  TypeChecker &TC;
  
  // TheFunc - This is the current FuncExpr being checked.  This is null for
  // top level code.
  FuncExpr *TheFunc;
  
  /// DC - This is the current DeclContext.
  DeclContext *DC;

  unsigned LoopNestLevel;

  struct AddLoopNest {
    StmtChecker &SC;
    AddLoopNest(StmtChecker &SC) : SC(SC) {
      ++SC.LoopNestLevel;
    }
    ~AddLoopNest() {
      --SC.LoopNestLevel;
    }
  };

  StmtChecker(TypeChecker &TC, FuncExpr *TheFunc)
    : TC(TC), TheFunc(TheFunc), DC(TheFunc), LoopNestLevel(0) { }

  StmtChecker(TypeChecker &TC, DeclContext *DC)
    : TC(TC), TheFunc(nullptr), DC(DC), LoopNestLevel(0) { }

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//
  
  bool typeCheckExpr(Expr *&E, Type DestTy = Type()) {
    return TC.typeCheckExpression(E, DestTy);
  }

  template<typename StmtTy>
  bool typeCheckStmt(StmtTy *&S) {
    StmtTy *S2 = cast_or_null<StmtTy>(visit(S));
    if (S2 == 0) return true;
    S = S2;
    return false;
  }
  
  bool typeCheck(PointerUnion<Expr*, AssignStmt*> &Val) {
    if (Expr *E = Val.dyn_cast<Expr*>()) {
      if (typeCheckExpr(E)) return true;
      Val = E;
    } else if (AssignStmt *S = Val.dyn_cast<AssignStmt*>()) {
      if (typeCheckStmt(S)) return true;
      Val = S;
    }
    return false;
  }
 
  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  Stmt *visitSemiStmt(SemiStmt *S) {
    return S;
  }

  Stmt *visitAssignStmt(AssignStmt *S) {
    Expr *Dest = S->getDest();
    Expr *Src = S->getSrc();
    if (TC.typeCheckAssignment(Dest, S->getEqualLoc(), Src))
      return 0;
    
    S->setDest(Dest);
    S->setSrc(Src);
    return S;
  }
  
  Stmt *visitBraceStmt(BraceStmt *BS);
  
  Stmt *visitReturnStmt(ReturnStmt *RS) {
    if (TheFunc == 0) {
      TC.diagnose(RS->getReturnLoc(), diag::return_invalid_outside_func);
      return 0;
    }

    Type ResultTy = TheFunc->getBodyResultType();
    if (!RS->hasResult()) {
      if (!ResultTy->isEqual(TupleType::getEmpty(TC.Context)))
        TC.diagnose(RS->getReturnLoc(), diag::return_expr_missing);
      return RS;
    }

    Expr *E = RS->getResult();
    if (typeCheckExpr(E, ResultTy))
      return 0;
    RS->setResult(E);

    return RS;
  }
  
  Stmt *visitIfStmt(IfStmt *IS) {
    Expr *E = IS->getCond();
    if (TC.typeCheckCondition(E)) return 0;
    IS->setCond(E);

    Stmt *S = IS->getThenStmt();
    if (typeCheckStmt(S)) return 0;
    IS->setThenStmt(S);

    if ((S = IS->getElseStmt())) {
      if (typeCheckStmt(S)) return 0;
      IS->setElseStmt(S);
    }
    
    return IS;
  }
  
  Stmt *visitWhileStmt(WhileStmt *WS) {
    Expr *E = WS->getCond();
    if (TC.typeCheckCondition(E)) return 0;
    WS->setCond(E);

    AddLoopNest loopNest(*this);
    Stmt *S = WS->getBody();
    if (typeCheckStmt(S)) return 0;
    WS->setBody(S);
    
    return WS;
  }
  Stmt *visitDoWhileStmt(DoWhileStmt *WS) {
    {
      AddLoopNest loopNest(*this);
      Stmt *S = WS->getBody();
      if (typeCheckStmt(S)) return 0;
      WS->setBody(S);
    }
    
    Expr *E = WS->getCond();
    if (TC.typeCheckCondition(E)) return 0;
    WS->setCond(E);
    return WS;
  }
  Stmt *visitForStmt(ForStmt *FS) {
    // Type check any var decls in the initializer.
    for (auto D : FS->getInitializerVarDecls())
      TC.typeCheckDecl(D, /*isFirstPass*/false);

    PointerUnion<Expr*, AssignStmt*> Tmp = FS->getInitializer();
    if (typeCheck(Tmp)) return 0;
    FS->setInitializer(Tmp);
    
    // Type check the condition if present.
    if (FS->getCond().isNonNull()) {
      Expr *E = FS->getCond().get();
      if (TC.typeCheckCondition(E)) return 0;
      FS->setCond(E);
    }
    
    Tmp = FS->getIncrement();
    if (typeCheck(Tmp)) return 0;
    FS->setIncrement(Tmp);

    AddLoopNest loopNest(*this);
    Stmt *S = FS->getBody();
    if (typeCheckStmt(S)) return 0;
    FS->setBody(S);
    
    return FS;
  }
  
  /// callNullaryMethodOf - Form a call (with no arguments) to the given
  /// method of the given base.
  Expr *callNullaryMethodOf(Expr *Base, FuncDecl *Method, SourceLoc Loc) {
    // Form the method reference.
    Expr *Mem = TC.buildMemberRefExpr(Base, Loc, Method, Loc);
    if (!Mem) return nullptr;
    Mem = TC.recheckTypes(Mem);
    if (!Mem) return nullptr;

    // Call the method.
    Expr *EmptyArgs
      = new (TC.Context) TupleExpr(Loc, MutableArrayRef<Expr *>(), 0, Loc,
                                   TupleType::getEmpty(TC.Context));
    ApplyExpr *Call = new (TC.Context) CallExpr(Mem, EmptyArgs);
    Expr *Result = TC.semaApplyExpr(Call);
    if (!Result) return nullptr;
    return TC.convertToRValue(Result);
  }
  
  /// callNullaryMemberOf - Form a call (with no arguments) to a method of the
  /// given base.
  Expr *callNullaryMethodOf(Expr *Base, Identifier Name, SourceLoc Loc,
                            Diag<Type> MissingMember,
                            Diag<Type> NonFuncMember) {
    Type BaseType = Base->getType()->getRValueType();

    // Look for name.
    MemberLookup Lookup(BaseType, Name, TC.TU);
    if (!Lookup.isSuccess()) {
      TC.diagnose(Loc, MissingMember, BaseType)
        << Base->getSourceRange();
      return nullptr;
    }
    
    // Make sure we found a function (which may be overloaded, of course).
    if (!isa<FuncDecl>(Lookup.Results.front().D)) {
      TC.diagnose(Loc, NonFuncMember, BaseType)
        << Base->getSourceRange();
      TC.diagnose(Lookup.Results.front().D->getStartLoc(),
                  diag::decl_declared_here,
                  Lookup.Results.front().D->getName());
      return nullptr;
    }
    
    // Form base.name
    Expr *Mem = TC.buildMemberRefExpr(Base, Loc, Lookup, Loc);
    Mem = TC.recheckTypes(Mem);
    if (!Mem) return nullptr;
    
    // Call base.name()
    Expr *EmptyArgs
      = new (TC.Context) TupleExpr(Loc, MutableArrayRef<Expr *>(), 0, Loc,
                                   TupleType::getEmpty(TC.Context));
    ApplyExpr *Call = new (TC.Context) CallExpr(Mem, EmptyArgs);
    Expr *Result = TC.semaApplyExpr(Call);
    if (!Result) return nullptr;
    return TC.convertToRValue(Result);
  }
  
  Stmt *visitForEachStmt(ForEachStmt *S) {
    // Type-check the container and convert it to an rvalue.
    Expr *Container = S->getContainer();
    if (TC.typeCheckExpression(Container)) return nullptr;
    S->setContainer(Container);

    // Retrieve the 'Enumerable' protocol.
    ProtocolDecl *EnumerableProto = TC.getEnumerableProtocol();
    if (!EnumerableProto) {
      TC.diagnose(S->getForLoc(), diag::foreach_missing_enumerable);
      return nullptr;
    }

    // Retrieve the 'Range' protocol.
    ProtocolDecl *RangeProto = TC.getRangeProtocol();
    if (!RangeProto) {
      TC.diagnose(S->getForLoc(), diag::foreach_missing_range);
      return nullptr;
    }
    
    // Verify that the container conforms to the Enumerable protocol, and
    // invoke getElements() on it container to retrieve the range of elements.
    Type RangeTy;
    VarDecl *Range;
    {
      Type ContainerType = Container->getType()->getRValueType();

      ProtocolConformance *Conformance = nullptr;
      if (!TC.conformsToProtocol(ContainerType, EnumerableProto, &Conformance,
                                 Container->getLoc()))
        return nullptr;

      // Gather the witness from the Enumerable protocol conformance. This are
      // the functions we'll call.
      FuncDecl *getElementsFn = 0;
      
      for (auto Member : EnumerableProto->getMembers()) {
        auto Value = dyn_cast<ValueDecl>(Member);
        if (!Value)
          continue;
        
        StringRef Name = Value->getName().str();
        if (Name.equals("Elements") && isa<TypeDecl>(Value)) {
          if (Conformance) {
            ArchetypeType *Archetype
              = cast<TypeDecl>(Value)->getDeclaredType()->getAs<ArchetypeType>();
            RangeTy = Conformance->TypeMapping[Archetype];
          } else {
            RangeTy = cast<TypeDecl>(Value)->getDeclaredType();
          }
          RangeTy = TC.substMemberTypeWithBase(RangeTy, Value, ContainerType);
        } else if (Name.equals("getElements") && isa<FuncDecl>(Value)) {
          if (Conformance)
            getElementsFn = cast<FuncDecl>(Conformance->Mapping[Value]);
          else
            getElementsFn = cast<FuncDecl>(Value);
        }
      }

      if (!getElementsFn || !RangeTy) {
        TC.diagnose(EnumerableProto->getLoc(), diag::enumerable_protocol_broken);
        return nullptr;
      }
      
      Expr *GetElements = callNullaryMethodOf(Container, getElementsFn,
                                              S->getInLoc());
      if (!GetElements) return nullptr;
      
      // Create a local variable to capture the range.
      // FIXME: Mark declaration as implicit?
      Range = new (TC.Context) VarDecl(S->getInLoc(),
                                       TC.Context.getIdentifier("__range"),
                                       RangeTy, DC);
      
      // Create a pattern binding to initialize the range and wire it into the
      // AST.
      Pattern *RangePat = new (TC.Context) NamedPattern(Range);
      S->setRange(new (TC.Context) PatternBindingDecl(S->getForLoc(),
                                                      RangePat, GetElements,
                                                      DC));
    }
    
    // FIXME: Would like to customize the diagnostic emitted in
    // conformsToProtocol().
    ProtocolConformance *Conformance = nullptr;
    if (!TC.conformsToProtocol(RangeTy, RangeProto, &Conformance,
                               Container->getLoc()))
      return nullptr;
    
    // Gather the witnesses from the Range protocol conformance. These are
    // the functions we'll call.
    FuncDecl *isEmptyFn = 0;
    FuncDecl *getFirstAndAdvanceFn = 0;
    Type ElementTy;
    
    for (auto Member : RangeProto->getMembers()) {
      auto Value = dyn_cast<ValueDecl>(Member);
      if (!Value)
        continue;
      
      StringRef Name = Value->getName().str();
      if (Name.equals("Element") && isa<TypeDecl>(Value)) {
        if (Conformance) {
          ArchetypeType *Archetype
            = cast<TypeDecl>(Value)->getDeclaredType()->getAs<ArchetypeType>();
          ElementTy = Conformance->TypeMapping[Archetype];
        } else {
          ElementTy = cast<TypeDecl>(Value)->getDeclaredType();
        }
        ElementTy = TC.substMemberTypeWithBase(ElementTy, Value, RangeTy);
      } else if (Name.equals("isEmpty") && isa<FuncDecl>(Value)) {
        if (Conformance)
          isEmptyFn = cast<FuncDecl>(Conformance->Mapping[Value]);
        else
          isEmptyFn = cast<FuncDecl>(Value);
      }
      else if (Name.equals("getFirstAndAdvance") && isa<FuncDecl>(Value)) {
        if (Conformance)
          getFirstAndAdvanceFn = cast<FuncDecl>(Conformance->Mapping[Value]);
        else
          getFirstAndAdvanceFn = cast<FuncDecl>(Value);
      }
    }
    
    if (!isEmptyFn || !getFirstAndAdvanceFn || !ElementTy) {
      TC.diagnose(RangeProto->getLoc(), diag::range_protocol_broken);
      return nullptr;
    }
    
    // Compute the expression that determines whether the range is empty.
    Expr *Empty
      = callNullaryMethodOf(
          new (TC.Context) DeclRefExpr(Range, S->getInLoc(),
                                       Range->getTypeOfReference()),
          isEmptyFn, S->getInLoc());
    if (!Empty) return nullptr;
    if (TC.typeCheckCondition(Empty)) return nullptr;
    S->setRangeEmpty(Empty);
    
    // Compute the expression that extracts a value from the range.
    Expr *GetFirstAndAdvance
      = callNullaryMethodOf(
          new (TC.Context) DeclRefExpr(Range, S->getInLoc(),
                                       Range->getTypeOfReference()),
          getFirstAndAdvanceFn,
          S->getInLoc());
    if (!GetFirstAndAdvance) return nullptr;
    
    S->setElementInit(new (TC.Context) PatternBindingDecl(S->getForLoc(),
                                                          S->getPattern(),
                                                          GetFirstAndAdvance,
                                                          DC));

    // Coerce the pattern to the element type, now that we know the element
    // type.
    if (TC.coerceToType(S->getPattern(), ElementTy, /*isFirstPass*/false))
      return nullptr;
    
    // Type-check the body of the loop.
    AddLoopNest loopNest(*this);
    BraceStmt *Body = S->getBody();
    if (typeCheckStmt(Body)) return nullptr;
    S->setBody(Body);
    
    return S;
  }

  Stmt *visitBreakStmt(BreakStmt *S) {
    if (!LoopNestLevel) {
      TC.diagnose(S->getLoc(), diag::break_outside_loop);
      return nullptr;
    }
    return S;
  }

  Stmt *visitContinueStmt(ContinueStmt *S) {
    if (!LoopNestLevel) {
      TC.diagnose(S->getLoc(), diag::continue_outside_loop);
      return nullptr;
    }
    return S;
  }
};
  
} // end anonymous namespace
  
  
Stmt *StmtChecker::visitBraceStmt(BraceStmt *BS) {
  for (unsigned i = 0, e = BS->getNumElements(); i != e; ++i) {
    if (Expr *SubExpr = BS->getElement(i).dyn_cast<Expr*>()) {
      if (typeCheckExpr(SubExpr)) continue;
      TC.typeCheckIgnoredExpr(SubExpr);
      BS->setElement(i, SubExpr);
      continue;
    }
    
    if (Stmt *SubStmt = BS->getElement(i).dyn_cast<Stmt*>()) {
      if (!typeCheckStmt(SubStmt))
        BS->setElement(i, SubStmt);
    } else {
      TC.typeCheckDecl(BS->getElement(i).get<Decl*>(), /*isFirstPass*/false);
    }
  }
  
  return BS;
}

/// Check an expression whose result is not being used at all.
void TypeChecker::typeCheckIgnoredExpr(Expr *E) {
  // Complain about l-values that are neither loaded nor stored.
  if (E->getType()->is<LValueType>()) {
    diagnose(E->getLoc(), diag::expression_unused_lvalue)
      << E->getSourceRange();
    return;
  }

  // Complain about functions that aren't called.
  // TODO: What about tuples which contain functions by-value that are
  // dead?
  if (E->getType()->is<AnyFunctionType>()) {
    diagnose(E->getLoc(), diag::expression_unused_function)
      << E->getSourceRange();
    return;
  }
}

void TypeChecker::typeCheckFunctionBody(FuncExpr *FE) {
  BraceStmt *BS = FE->getBody();
  if (!BS)
    return;

  StmtChecker(*this, FE).typeCheckStmt(BS);
  FE->setBody(BS);
}

/// \brief Check for circular inheritance of protocols.
///
/// \param Path The circular path through the protocol inheritance hierarchy,
/// which will be constructed (backwards) if there is in fact a circular path.
///
/// \returns True if there was circular inheritance, false otherwise.
bool checkProtocolCircularity(TypeChecker &TC, ProtocolDecl *Proto,
                              llvm::SmallPtrSet<ProtocolDecl *, 16> &Visited,
                              llvm::SmallPtrSet<ProtocolDecl *, 16> &Local,
                              llvm::SmallVectorImpl<ProtocolDecl *> &Path) {
  for (auto InheritedTy : Proto->getInherited()) {
    SmallVector<ProtocolDecl *, 4> InheritedProtos;
    if (!InheritedTy.getType()->isExistentialType(InheritedProtos))
      continue;
    
    for (auto InheritedProto : InheritedProtos) {
      if (Visited.count(InheritedProto)) {
        // We've seen this protocol as part of another protocol search;
        // it's not circular.
        continue;
      }

      // Whether we've seen this protocol before in our search or visiting it
      // detects a circularity, record it in the path and abort.
      if (!Local.insert(InheritedProto) ||
          checkProtocolCircularity(TC, InheritedProto, Visited, Local, Path)) {
        Path.push_back(InheritedProto);
        return true;
      }
    }
  }
  
  return false;
}

static unsigned getNumArgs(ValueDecl *value) {
  if (!isa<FuncDecl>(value)) return ~0U;

  Type argTy = cast<AnyFunctionType>(value->getType())->getInput();
  if (auto tuple = argTy->getAs<TupleType>()) {
    return tuple->getFields().size();
  } else {
    return 1;
  }
}

static bool matchesDeclRefKind(ValueDecl *value, DeclRefKind refKind) {
  if (value->getType()->is<ErrorType>())
    return true;

  switch (refKind) {
  // An ordinary reference doesn't ignore anything.
  case DeclRefKind::Ordinary:
    return true;

  // A binary-operator reference only honors FuncDecls with a certain type.
  case DeclRefKind::BinaryOperator:
    return (getNumArgs(value) == 2);

  case DeclRefKind::PrefixOperator:
    return (!value->getAttrs().isPostfix() && getNumArgs(value) == 1);

  case DeclRefKind::PostfixOperator:
    return (value->getAttrs().isPostfix() && getNumArgs(value) == 1);    
  }
  llvm_unreachable("bad declaration reference kind");
}

/// BindName - Bind an UnresolvedDeclRefExpr by performing name lookup and
/// returning the resultant expression.  Context is the DeclContext used
/// for the lookup.
static Expr *BindName(UnresolvedDeclRefExpr *UDRE, DeclContext *Context,
                      TypeChecker &TC) {
  // Process UnresolvedDeclRefExpr by doing an unqualified lookup.
  Identifier Name = UDRE->getName();
  SourceLoc Loc = UDRE->getLoc();

  // Perform standard value name lookup.
  UnqualifiedLookup Lookup(Name, Context);

  if (!Lookup.isSuccess()) {
    TC.diagnose(Loc, diag::use_unresolved_identifier, Name);
    return new (TC.Context) ErrorExpr(Loc);
  }

  // FIXME: Need to refactor the way we build an AST node from a lookup result!

  if (Lookup.Results.size() == 1 &&
      Lookup.Results[0].Kind == UnqualifiedLookupResult::ModuleName) {
    ModuleType *MT = ModuleType::get(Lookup.Results[0].getNamedModule());
    return new (TC.Context) ModuleExpr(Loc, MT);
  }

  bool AllDeclRefs = true;
  SmallVector<ValueDecl*, 4> ResultValues;
  for (auto Result : Lookup.Results) {
    switch (Result.Kind) {
    case UnqualifiedLookupResult::ModuleMember:
    case UnqualifiedLookupResult::LocalDecl: {
      ValueDecl *D = Result.getValueDecl();
      if (matchesDeclRefKind(D, UDRE->getRefKind()))
        ResultValues.push_back(D);
      break;
    }
    case UnqualifiedLookupResult::MemberProperty:
    case UnqualifiedLookupResult::MemberFunction:
    case UnqualifiedLookupResult::MetatypeMember:
    case UnqualifiedLookupResult::ExistentialMember:
    case UnqualifiedLookupResult::ArchetypeMember:
    case UnqualifiedLookupResult::MetaArchetypeMember:
    case UnqualifiedLookupResult::ModuleName:
      AllDeclRefs = false;
      break;
    }
  }
  if (AllDeclRefs) {
    // Diagnose uses of operators that found no matching candidates.
    if (ResultValues.empty()) {
      assert(UDRE->getRefKind() != DeclRefKind::Ordinary);
      TC.diagnose(Loc, diag::use_nonmatching_operator, Name,
                  UDRE->getRefKind() == DeclRefKind::BinaryOperator ? 0 :
                  UDRE->getRefKind() == DeclRefKind::PrefixOperator ? 1 : 2);
      return new (TC.Context) ErrorExpr(Loc);
    }

    return TC.buildRefExpr(ResultValues, Loc);
  }

  ResultValues.clear();
  bool AllMemberRefs = true;
  ValueDecl *Base = 0;
  for (auto Result : Lookup.Results) {
    switch (Result.Kind) {
    case UnqualifiedLookupResult::MemberProperty:
    case UnqualifiedLookupResult::MemberFunction:
    case UnqualifiedLookupResult::MetatypeMember:
    case UnqualifiedLookupResult::ExistentialMember:
      ResultValues.push_back(Result.getValueDecl());
      if (Base && Result.getBaseDecl() != Base) {
        AllMemberRefs = false;
        break;
      }
      Base = Result.getBaseDecl();
      break;
    case UnqualifiedLookupResult::ModuleMember:
    case UnqualifiedLookupResult::LocalDecl:
    case UnqualifiedLookupResult::ModuleName:
      AllMemberRefs = false;
      break;
    case UnqualifiedLookupResult::MetaArchetypeMember:
    case UnqualifiedLookupResult::ArchetypeMember:
      // FIXME: We need to extend OverloadedMemberRefExpr to deal with this.
      llvm_unreachable("Archetype members in overloaded member references");
      break;
    }
  }

  if (AllMemberRefs) {
    Expr *BaseExpr = new (TC.Context) DeclRefExpr(Base, Loc,
                                                  Base->getTypeOfReference());
    if (auto NTD = dyn_cast<NominalTypeDecl>(Base)) {
      if (auto GenericParams = NTD->getGenericParams()) {
        SmallVector<SpecializeExpr::Substitution, 2> Substitutions;
        SmallVector<Type, 4> Types;
        for (auto Param : *GenericParams) {
          Type T = Param.getAsTypeParam()->getDeclaredType();
          Types.push_back(T);
          Substitutions.push_back({ T, nullptr });
        }
        Type BoundTy = BoundGenericType::get(NTD, Types);
        BaseExpr = new (TC.Context) SpecializeExpr(BaseExpr, BoundTy,
                                    TC.Context.AllocateCopy(Substitutions));
      }
    }
    return TC.buildMemberRefExpr(BaseExpr, SourceLoc(), ResultValues, Loc);
  }

  llvm_unreachable("Can't represent lookup result");
}

/// performTypeChecking - Once parsing and namebinding are complete, these
/// walks the AST to resolve types and diagnose problems therein.
///
/// FIXME: This should be moved out to somewhere else.
void swift::performTypeChecking(TranslationUnit *TU, unsigned StartElem) {
  TypeChecker TC(*TU);

  struct ExprPrePassWalker : private ASTWalker {
    TypeChecker &TC;

    ExprPrePassWalker(TypeChecker &TC) : TC(TC) {}
    
    /// CurDeclContexts - This is the stack of DeclContexts that
    /// we're nested in.
    SmallVector<DeclContext*, 4> CurDeclContexts;

    // FuncExprs - This is a list of all the FuncExprs we need to analyze, in
    // an appropriate order.
    SmallVector<llvm::PointerUnion3<FuncExpr*, ConstructorDecl*, DestructorDecl*>, 32> FuncExprs;

    virtual bool walkToDeclPre(Decl *D) {
      if (NominalTypeDecl *NTD = dyn_cast<NominalTypeDecl>(D))
        CurDeclContexts.push_back(NTD);
      else if (ExtensionDecl *ED = dyn_cast<ExtensionDecl>(D))
        CurDeclContexts.push_back(ED);
      else if (ConstructorDecl *CD = dyn_cast<ConstructorDecl>(D))
        CurDeclContexts.push_back(CD);
      else if (DestructorDecl *DD = dyn_cast<DestructorDecl>(D))
        CurDeclContexts.push_back(DD);

      if (FuncDecl *FD = dyn_cast<FuncDecl>(D)) {
        // If this is an instance method with a body, set the type of it's
        // implicit 'this' variable.
        // FIXME: This is only necessary because we do name-binding for
        // DeclRefs too early.
        if (Type ThisTy = FD->computeThisType())
          FD->getImplicitThisDecl()->setType(ThisTy);
      }

      if (ConstructorDecl *CD = dyn_cast<ConstructorDecl>(D))
        FuncExprs.push_back(CD);
      if (DestructorDecl *DD = dyn_cast<DestructorDecl>(D))
        FuncExprs.push_back(DD);
      return true;
    }
    
    virtual bool walkToDeclPost(Decl *D) {
      if (isa<NominalTypeDecl>(D)) {
        assert(CurDeclContexts.back() == cast<NominalTypeDecl>(D) &&
               "Context misbalance");
        CurDeclContexts.pop_back();
      } else if (isa<ExtensionDecl>(D)) {
        assert(CurDeclContexts.back() == cast<ExtensionDecl>(D) &&
               "Context misbalance");
        CurDeclContexts.pop_back();
      } else if (isa<ConstructorDecl>(D)) {
        assert(CurDeclContexts.back() == cast<ConstructorDecl>(D) &&
               "Context misbalance");
        CurDeclContexts.pop_back();
      } else if (isa<DestructorDecl>(D)) {
        assert(CurDeclContexts.back() == cast<DestructorDecl>(D) &&
               "Context misbalance");
        CurDeclContexts.pop_back();
      }

      return true;
    }

    bool walkToExprPre(Expr *E) {
      if (FuncExpr *FE = dyn_cast<FuncExpr>(E))
        FuncExprs.push_back(FE);

      if (CapturingExpr *CE = dyn_cast<CapturingExpr>(E))
        CurDeclContexts.push_back(CE);

      return true;
    }

    Expr *walkToExprPost(Expr *E) {
      if (UnresolvedDeclRefExpr *UDRE = dyn_cast<UnresolvedDeclRefExpr>(E)) {
        return BindName(UDRE, CurDeclContexts.back(),
                        TC);
      }

      if (isa<CapturingExpr>(E)) {
        assert(CurDeclContexts.back() == cast<CapturingExpr>(E) &&
               "Context misbalance");
        CurDeclContexts.pop_back();
      }

      return E;
    }

    Expr *doWalk(Expr *E, DeclContext *DC) {
      CurDeclContexts.push_back(DC);
      E = E->walk(*this);
      CurDeclContexts.pop_back();
      return E;
    }

    void doWalk(Decl *D) {
      CurDeclContexts.push_back(D->getDeclContext());
      D->walk(*this);
      CurDeclContexts.pop_back();
    }
  };
  ExprPrePassWalker prePass(TC);

  // Validate the conformance types of all of the protocols in the translation
  // unit. This includes inherited protocols, associated types with
  // requirements, and (FIXME:) conformance requirements in requires clauses.
  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    Decl *D = TU->Decls[i];
    if (auto Proto = dyn_cast<ProtocolDecl>(D))
      TC.preCheckProtocol(Proto);
  }

  // Type check the top-level elements of the translation unit.
  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    Decl *D = TU->Decls[i];
    if (isa<TopLevelCodeDecl>(D))
      continue;

    TC.typeCheckDecl(D, /*isFirstPass*/true);
  }

  // Check for explicit conformance to protocols and for circularity in
  // protocol definitions.
  {
    // FIXME: This check should be in TypeCheckDecl.
    llvm::SmallPtrSet<ProtocolDecl *, 16> VisitedProtocols;
    for (auto D : TU->Decls) {
      if (auto Protocol = dyn_cast<ProtocolDecl>(D)) {
        // Check for circular protocol definitions.
        llvm::SmallPtrSet<ProtocolDecl *, 16> LocalVisited;
        llvm::SmallVector<ProtocolDecl *, 4> Path;
        if (VisitedProtocols.count(Protocol) == 0) {
          LocalVisited.insert(Protocol);
          if (checkProtocolCircularity(TC, Protocol, VisitedProtocols,
                                       LocalVisited, Path)) {
            llvm::SmallString<128> PathStr;
            PathStr += "'";
            PathStr += Protocol->getName().str();
            PathStr += "'";
            for (unsigned I = Path.size(); I != 0; --I) {
              PathStr += " -> '";
              PathStr += Path[I-1]->getName().str();
              PathStr += "'";
            }
            
            TC.diagnose(Protocol->getLoc(), diag::circular_protocol_def,
                        PathStr);
            for (unsigned I = Path.size(); I != 1; --I) {
              TC.diagnose(Path[I-1]->getLoc(), diag::protocol_here,
                          Path[I-1]->getName());
            }
          }
          
          VisitedProtocols.insert(LocalVisited.begin(), LocalVisited.end());
        }
      }
    }
  }

  // We don't know the types of all the global declarations in the first
  // pass, which means we can't completely analyze everything. Perform the
  // second pass now.

  // Check default arguments in types.
  for (auto TypeAndContext : TU->getTypesWithDefaultValues()) {
    TupleType *TT = TypeAndContext.first;
    for (unsigned i = 0, e = TT->getFields().size(); i != e; ++i) {
      const TupleTypeElt& Elt = TT->getFields()[i];
      if (Elt.hasInit()) {
        // Perform global name-binding etc. for all tuple default values.
        // FIXME: This screws up the FuncExprs list for FuncExprs in a
        // default value; conceptually, we should be appending to the list
        // in source order.
        ExprHandle *init = Elt.getInit();
        Expr *initExpr = prePass.doWalk(init->getExpr(), TypeAndContext.second);
        init->setExpr(initExpr);

        if (TT->hasCanonicalTypeComputed()) {
          // If we already examined a tuple in the first pass, we didn't
          // get a chance to type-check it; do that now.
          if (!TC.typeCheckExpression(initExpr, Elt.getType()))
            init->setExpr(initExpr);
        }
      }
    }
  }
  TU->clearTypesWithDefaultValues();

  // Check default arguments in patterns.
  // FIXME: This is an ugly hack to keep default arguments working for the
  // moment; I don't really want to invest more time into them until we're
  // sure how they are acutally supposed to work.
  struct PatternDefaultArgChecker : public ASTWalker {
    TypeChecker &TC;
    ExprPrePassWalker &PrePass;

    PatternDefaultArgChecker(TypeChecker &TC,
                             ExprPrePassWalker &PrePass)
      : TC(TC), PrePass(PrePass) {}

    void visitPattern(Pattern *P, DeclContext *DC) {
      switch (P->getKind()) {
      case PatternKind::Tuple:
        for (auto &field : cast<TuplePattern>(P)->getFields()) {
          if (field.getInit() && field.getPattern()->hasType()) {
            Expr *e = field.getInit()->getExpr();
            e = PrePass.doWalk(e, DC);
            TC.typeCheckExpression(e, field.getPattern()->getType());
            field.getInit()->setExpr(e);
          }
        }
        return;
      case PatternKind::Paren:
        return visitPattern(cast<ParenPattern>(P)->getSubPattern(), DC);
      case PatternKind::Typed:
      case PatternKind::Named:
      case PatternKind::Any:
        return;
      }
      llvm_unreachable("bad pattern kind!");
    }

    virtual bool walkToDeclPre(Decl *D) {
      if (ConstructorDecl *CD = dyn_cast<ConstructorDecl>(D)) {
        visitPattern(CD->getArguments(), D->getDeclContext());
      } else if (SubscriptDecl *SD = dyn_cast<SubscriptDecl>(D)) {
        visitPattern(SD->getIndices(), D->getDeclContext());
      } else if (FuncDecl *FD = dyn_cast<FuncDecl>(D)) {
        for (Pattern *p : FD->getBody()->getParamPatterns())
          visitPattern(p, D->getDeclContext());
      }
      return true;
    }
  };
  PatternDefaultArgChecker pdac(TC, prePass);
  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i)
    TU->Decls[i]->walk(pdac);

  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    Decl *D = TU->Decls[i];
    if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      // Immediately perform global name-binding etc.
      prePass.doWalk(TLCD);

      auto Elem = TLCD->getBody();
      if (Expr *E = Elem.dyn_cast<Expr*>()) {
        if (TC.typeCheckExpression(E)) continue;
        if (TU->Kind == TranslationUnit::Repl)
          TC.typeCheckTopLevelReplExpr(E, TLCD);
        else
          TC.typeCheckIgnoredExpr(E);
        TLCD->setBody(E);
      } else {
        Stmt *S = Elem.get<Stmt*>();
        if (StmtChecker(TC, TLCD).typeCheckStmt(S)) continue;
        TLCD->setBody(S);
      }
    } else {
      prePass.doWalk(D);
      TC.typeCheckDecl(D, /*isFirstPass*/false);
    }
    if (TU->Kind == TranslationUnit::Repl && !TC.Context.hadError())
      if (PatternBindingDecl *PBD = dyn_cast<PatternBindingDecl>(D))
        TC.REPLCheckPatternBinding(PBD);
  }

  // Check overloaded vars/funcs.
  // FIXME: This is quadratic time for TUs with multiple chunks.
  // FIXME: Can we make this more efficient?
  // FIXME: This check should be earlier to avoid ambiguous overload
  // errors etc.
  llvm::DenseMap<Identifier, TinyPtrVector<ValueDecl*>> CheckOverloads;
  for (unsigned i = 0, e = TU->Decls.size(); i != e; ++i) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(TU->Decls[i])) {
      // FIXME: I'm not sure this check is really correct.
      if (VD->getName().empty())
        continue;
      if (VD->getType()->is<ErrorType>() || VD->getType()->isUnresolvedType())
        continue;
      auto &PrevOv = CheckOverloads[VD->getName()];
      if (i >= StartElem) {
        for (ValueDecl *PrevD : PrevOv) {
          if (PrevD->getType()->isEqual(VD->getType())) {
            TC.diagnose(VD->getStartLoc(), diag::invalid_redecl);
            TC.diagnose(PrevD->getStartLoc(), diag::invalid_redecl_prev,
                        VD->getName());
          }
        }
      }
      PrevOv.push_back(VD);
    }
  }

  // Type check the body of each of the FuncExpr in turn.  Note that outside
  // FuncExprs must be visited before nested FuncExprs for type-checking to
  // work correctly.
  for (auto func : prePass.FuncExprs) {
    if (ConstructorDecl *CD = func.dyn_cast<ConstructorDecl*>()) {
      Stmt *Body = CD->getBody();
      if (Body)
        StmtChecker(TC, CD).typeCheckStmt(Body);
      continue;
    }
    if (DestructorDecl *DD = func.dyn_cast<DestructorDecl*>()) {
      Stmt *Body = DD->getBody();
      StmtChecker(TC, DD).typeCheckStmt(Body);
      continue;
    }
    FuncExpr *FE = func.get<FuncExpr*>();

    PrettyStackTraceExpr StackEntry(TC.Context, "type-checking", FE);

    TC.typeCheckFunctionBody(FE);
  }

  // Verify that we've checked types correctly.
  TU->ASTStage = TranslationUnit::TypeChecked;
  verify(TU);
}
