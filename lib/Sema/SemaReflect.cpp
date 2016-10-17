//===--- SemaReflect.cpp - Semantic Analysis for Reflection ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ reflection.
//
//===----------------------------------------------------------------------===//

#include "TreeTransform.h"
#include "TypeLocBuilder.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/PartialDiagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/ParsedTemplate.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/Template.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/PointerSumType.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using namespace clang;
using namespace sema;

/// The expression \c $x returns an object describing the reflected entity \c x.
/// The type of that object depends on the type of the thing reflected.
///
/// The \p E argument is not null.
ExprResult Sema::ActOnCXXReflectExpr(SourceLocation OpLoc, Expr *E) {
  // Try to correct typos.
  if (isa<TypoExpr>(E)) {
    ExprResult Fixed = CorrectDelayedTyposInExpr(E);
    if (!Fixed.isUsable())
      return ExprError();
    E = Fixed.get();
  }

  // If the node is dependent, then preserve the expression until instantiation.
  if (E->isTypeDependent() || E->isValueDependent())
    return new (Context) ReflectionExpr(OpLoc, E, Context.DependentTy);

  if (OverloadExpr *Ovl = dyn_cast<OverloadExpr>(E)) {
    // FIXME: This should be okay. We should be able to provide a limited
    // interface to overloaded functions.
    return ExprError(Diag(OpLoc, diag::err_reflected_overload)
                     << Ovl->getSourceRange());
  }

  if (!isa<DeclRefExpr>(E))
    llvm_unreachable("Expression reflection not implemented");

  // Build the reflection expression.
  return BuildDeclReflection(OpLoc, cast<DeclRefExpr>(E)->getDecl());
}

// \brief Build a reflection for the type wrapped by \p TSI.
ExprResult Sema::ActOnCXXReflectExpr(SourceLocation OpLoc,
                                     TypeSourceInfo *TSI) {
  QualType T = TSI->getType();

  // If the type is dependent, preserve the expression until instantiation.
  if (T->isDependentType())
    return new (Context) ReflectionExpr(OpLoc, TSI, Context.DependentTy);

  return BuildTypeReflection(OpLoc, T);
}

// \brief Build a reflection for the type-id stored in \p D.
ExprResult Sema::ActOnCXXReflectExpr(SourceLocation OpLoc, Declarator &D) {
  return ActOnCXXReflectExpr(OpLoc, GetTypeForDeclarator(D, CurScope));
}

/// Try to construct a reflection for the declaration named by \p II. This will
/// reflect:
///
///   - id-expressions whose unqualified-id is an identifier
///   - type-names that are identifiers, and
///   - namespace-names
///
// TODO: Handle ambiguous and overloaded lookups.
ExprResult Sema::ActOnCXXReflectExpr(SourceLocation OpLoc, CXXScopeSpec &SS,
                                     IdentifierInfo *II, SourceLocation IdLoc) {
  // Perform any declaration having the given name.
  LookupResult R(*this, II, OpLoc, LookupAnyName);
  LookupParsedName(R, CurScope, &SS);
  if (!R.isSingleResult())
    return ExprError(Diag(IdLoc, diag::err_reflected_overload));
  Decl *D = R.getAsSingle<Decl>();
  if (!D)
    return ExprError();

  // If the declaration is a template parameter, defer until instantiation.
  //
  // FIXME: This needs to be adapted for non-type and template template
  // parameters also. Most likely, we need to allow ReflectExprs to contain
  // declarations in addition to expressions and types.
  if (TypeDecl *TD = dyn_cast<TypeDecl>(D)) {
    QualType T = Context.getTypeDeclType(TD);
    if (T->isDependentType()) {
      TypeSourceInfo *TSI = Context.getTrivialTypeSourceInfo(T);
      return new (Context) ReflectionExpr(OpLoc, TSI, Context.DependentTy);
    }
  }

  // If we have a value declaration of dependent type, then make a dependent
  // decl-ref expression to be resolved later.
  if (ValueDecl *VD = dyn_cast<ValueDecl>(D)) {
    QualType T = VD->getType();
    if (T->isDependentType()) {
      Expr *E = new (Context)
          DeclRefExpr(VD, false, Context.DependentTy, VK_LValue, OpLoc);
      return new (Context) ReflectionExpr(OpLoc, E, Context.DependentTy);
    }
  }

  return BuildDeclReflection(OpLoc, D);
}

// Used to encode the kind of entity reflected. This value is packed into
// the low order bits of each reflected pointer. Because we stuff pointer
// values, all must be aligned at 2 bytes (which is generally guaranteed).
//
// TODO: Could we safely use high order bits?
enum ReflectionKind { RK_Decl = 1, RK_Type = 2, RK_Expr = 3 };

using ReflectionValue =
    llvm::PointerSumType<ReflectionKind,
                         llvm::PointerSumTypeMember<RK_Decl, Decl *>,
                         llvm::PointerSumTypeMember<RK_Type, Type *>,
                         llvm::PointerSumTypeMember<RK_Expr, Expr *>>;

static std::pair<ReflectionKind, void *> ExplodeOpaqueValue(std::uintptr_t N) {
  // Look away. I'm totally breaking abstraction.
  using Helper = llvm::detail::PointerSumTypeHelper<
      ReflectionKind, llvm::PointerSumTypeMember<RK_Decl, Decl *>,
      llvm::PointerSumTypeMember<RK_Type, Type *>,
      llvm::PointerSumTypeMember<RK_Expr, Expr *>>;
  ReflectionKind K = (ReflectionKind)(N & Helper::TagMask);
  void *P = (void *)(N & Helper::PointerMask);
  return {K, P};
}

// Returns the name of the class we're going to instantiate.
//
// TODO: Add templates and... other stuff?
//
// TODO: Do we want a more precise set of types for these things?
static char const *GetReflectionClass(Decl *D) {
  switch (D->getKind()) {
  case Decl::Var:
    return "variable";
  case Decl::Field:
    return "member_variable";

  case Decl::ParmVar:
    return "parameter";

  case Decl::Function:
    return "function";
  case Decl::CXXMethod:
    return "member_function";
  case Decl::CXXConstructor:
    return "constructor";
  case Decl::CXXDestructor:
    return "destructor";
  case Decl::CXXConversion:
    return "conversion";

  case Decl::EnumConstant:
    return "enumerator";

  case Decl::TranslationUnit:
    return "tu";

  case Decl::Namespace:
    return "ns";

  default:
    break;
  }
  llvm_unreachable("Unhandled declaration in reflection");
}

/// \brief Return an expression whose type reflects the given node.
ExprResult Sema::BuildDeclReflection(SourceLocation Loc, Decl *D) {
  // Use BuildTypeReflection for type declarations.
  if (TagDecl *TD = dyn_cast<TagDecl>(D))
    return BuildTypeReflection(Loc, Context.getTagDeclType(TD));

  // Get the template name for the instantiation.
  char const *Name = GetReflectionClass(D);
  ClassTemplateDecl *Temp = RequireReflectionType(Loc, Name);
  if (!Temp)
    return ExprError();
  TemplateName TempName(Temp);

  // Get the reflected value for D.
  ReflectionValue RV = ReflectionValue::create<RK_Decl>(D);

  // Build a template specialization, instantiate it, and then complete it.
  QualType IntPtrTy = Context.getIntPtrType();
  llvm::APSInt IntPtrVal = Context.MakeIntValue(RV.getOpaqueValue(), IntPtrTy);
  TemplateArgument Arg(Context, IntPtrVal, IntPtrTy);
  TemplateArgumentLoc ArgLoc(Arg, TemplateArgumentLocInfo());
  TemplateArgumentListInfo TempArgs(Loc, Loc);
  TempArgs.addArgument(ArgLoc);
  QualType TempType = CheckTemplateIdType(TempName, Loc, TempArgs);
  if (RequireCompleteType(Loc, TempType, diag::err_incomplete_type))
    assert(false && "Failed to instantiate reflection type");

  // Produce a value-initialized temporary of the required type.
  SmallVector<Expr *, 1> Args;
  InitializedEntity Entity = InitializedEntity::InitializeTemporary(TempType);
  InitializationKind Kind = InitializationKind::CreateValue(Loc, Loc, Loc);
  InitializationSequence InitSeq(*this, Entity, Kind, Args);
  return InitSeq.Perform(*this, Entity, Kind, Args);
}

// Returns the reflection class name for the type.
//
// TODO: Actually populate this table.
static char const *GetReflectionClass(QualType T) {
  T = T.getCanonicalType();
  switch (T->getTypeClass()) {
  case Type::Record:
    if (T->isUnionType())
      return "union_type";
    else
      return "class_type";
  case Type::Enum:
    return "enum_type";
  default:
    return "type"; // FIXME: Wrong! We should have a class for each type.
  }
}

/// \brief Return an expression whose type reflects the given node.
///
// TODO: Accommodate cv-qualifiers somehow. Perhaps add them as template
// parameters a la:
//
//    template<reflection_t X, bool C, bool V>
//    struct type { ... };
//
// Note that we would need a few alternative traits to combine cv information
// in order to accurately get the type name. I also suspect that we'll need
// noexcept information for function types, and possibly more state flags
// for other types beyond this. Same idea. X is always the value type.
// Additional flags can be added as extensions.
//
// TODO: There's a lot of duplication between this and the BuildDeclReflection
// function above. Surely we can simplify.
ExprResult Sema::BuildTypeReflection(SourceLocation Loc, QualType QT) {
  // See through elaborated and deduced types.
  if (const ElaboratedType *Elab = QT->getAs<ElaboratedType>()) {
    QT = Elab->getNamedType();
  } else if (const AutoType *Auto = QT->getAs<AutoType>()) {
    assert(Auto->isDeduced() && "Reflection of non-deduced type");
    QT = Auto->getDeducedType();
  }

  // Get the wrapper type.
  char const *Name = GetReflectionClass(QT);
  ClassTemplateDecl *Temp = RequireReflectionType(Loc, Name);
  if (!Temp)
    return ExprError();
  TemplateName TempName(Temp);

  Type *T = const_cast<Type *>(QT.getTypePtr());
  ReflectionValue RV = ReflectionValue::create<RK_Type>(T);

  // Build a template specialization, instantiate it, and then complete it.
  QualType IntPtrTy = Context.getIntPtrType();
  llvm::APSInt IntPtrVal = Context.MakeIntValue(RV.getOpaqueValue(), IntPtrTy);
  TemplateArgument Arg(Context, IntPtrVal, IntPtrTy);
  TemplateArgumentLoc ArgLoc(Arg, TemplateArgumentLocInfo());
  TemplateArgumentListInfo TempArgs(Loc, Loc);
  TempArgs.addArgument(ArgLoc);
  QualType TempType = CheckTemplateIdType(TempName, Loc, TempArgs);
  if (RequireCompleteType(Loc, TempType, diag::err_incomplete_type))
    assert(false && "Failed to instantiate reflection type");

  // Produce a value-initialized temporary of the required type.
  SmallVector<Expr *, 1> Args;
  InitializedEntity Entity = InitializedEntity::InitializeTemporary(TempType);
  InitializationKind Kind = InitializationKind::CreateValue(Loc, Loc, Loc);
  InitializationSequence InitSeq(*this, Entity, Kind, Args);
  return InitSeq.Perform(*this, Entity, Kind, Args);
}

/// \brief Returns the cpp3k namespace if a suitable header has been included.
/// If not, a diagnostic is emitted, and \c nullptr is returned.
///
// TODO: We should probably cache this the same way that we do
// with typeid (see CXXTypeInfoDecl in Sema.h).
NamespaceDecl *Sema::RequireCpp3kNamespace(SourceLocation Loc) {
  IdentifierInfo *Cpp3kII = &PP.getIdentifierTable().get("cpp3k");
  LookupResult R(*this, Cpp3kII, Loc, LookupNamespaceName);
  LookupQualifiedName(R, Context.getTranslationUnitDecl());
  if (!R.isSingleResult()) {
    Diag(Loc, diag::err_need_header_before_dollar);
    return nullptr;
  }
  NamespaceDecl *Cpp3k = R.getAsSingle<NamespaceDecl>();
  assert(Cpp3k && "cpp3k is not a namespace");
  return Cpp3k;
}

/// \brief Same as RequireCpp3kNamespace, but requires cpp3k::meta.
NamespaceDecl *Sema::RequireCpp3kMetaNamespace(SourceLocation Loc) {
  NamespaceDecl *Cpp3k = RequireCpp3kNamespace(Loc);
  if (!Cpp3k)
    return nullptr;

  // Get the cpp3k::meta namespace.
  IdentifierInfo *MetaII = &PP.getIdentifierTable().get("meta");
  LookupResult R(*this, MetaII, Loc, LookupNamespaceName);
  LookupQualifiedName(R, Cpp3k);
  if (!R.isSingleResult()) {
    Diag(Loc, diag::err_need_header_before_dollar);
    return nullptr;
  }
  NamespaceDecl *Meta = R.getAsSingle<NamespaceDecl>();
  assert(Meta && "cpp3k::meta is not a namespace");
  return Meta;
}

/// \brief Returns the class with the given name in the
/// std::[experimental::]meta namespace. If no such class can be found, a
/// diagnostic is emitted, and \c nullptr returned.
///
// TODO: Cache these types so we don't keep doing lookup. In on the first
// lookup, cache the names of ALL meta types so that we can easily check
// for appropriate arguments in the reflection traits.
ClassTemplateDecl *Sema::RequireReflectionType(SourceLocation Loc,
                                               char const *Name) {
  NamespaceDecl *Meta = RequireCpp3kMetaNamespace(Loc);
  if (!Meta)
    return nullptr;

  // Get the corresponding reflection class.
  IdentifierInfo *TypeII = &PP.getIdentifierTable().get(Name);
  LookupResult R(*this, TypeII, SourceLocation(), LookupAnyName);
  LookupQualifiedName(R, Meta);
  ClassTemplateDecl *Decl = R.getAsSingle<ClassTemplateDecl>();
  if (!Decl) {
    Diag(Loc, diag::err_need_header_before_dollar);
    return nullptr;
  }
  return Decl;
}

// Information supporting reflection operations.
//
// TODO: Move all of the functions below into this class since it provides
// the context for their evaluation.
struct Reflector {
  Sema &S;
  SourceLocation KWLoc;
  SourceLocation RParenLoc;
  ArrayRef<Expr *> Args;
  ArrayRef<llvm::APSInt> Vals;

  ExprResult Reflect(ReflectionTrait RT, Decl *D);
  ExprResult Reflect(ReflectionTrait RT, Type *T);

  // General entity properties.
  ExprResult ReflectName(Decl *D);
  ExprResult ReflectName(Type *D);
  ExprResult ReflectQualifiedName(Decl *D);
  ExprResult ReflectQualifiedName(Type *D);
  ExprResult ReflectDeclarationContext(Decl *D);
  ExprResult ReflectDeclarationContext(Type *T);
  ExprResult ReflectLexicalContext(Decl *D);
  ExprResult ReflectLexicalContext(Type *T);

  ExprResult ReflectLinkage(Decl *D);

  ExprResult ReflectVariableStorage(VarDecl *D);
  ExprResult ReflectFunctionStorage(FunctionDecl *D);
  ExprResult ReflectStorage(Decl *D);
  ExprResult ReflectPointer(Decl *D);
  ExprResult ReflectType(Decl *D);

  ExprResult ReflectNumParameters(Decl *D);
  ExprResult ReflectParameter(Decl *D, const llvm::APSInt &N);

  template <typename I> ExprResult GetNumMembers(I First, I Limit);

  template <typename I>
  ExprResult GetMember(const llvm::APSInt &N, I First, I Limit);

  ExprResult ReflectNumMembers(Decl *);
  ExprResult ReflectMember(Decl *, const llvm::APSInt &N);
  ExprResult ReflectNumMembers(Type *);
  ExprResult ReflectMember(Type *, const llvm::APSInt &N);
};

ExprResult Sema::ActOnReflectionTrait(SourceLocation KWLoc,
                                      ReflectionTrait Kind,
                                      ArrayRef<Expr *> Args,
                                      SourceLocation RParenLoc) {
  // If any arguments are dependent, then the result is a dependent
  // expression.
  //
  // TODO: What if a argument is type dependent? Or instantiation dependent?
  for (unsigned i = 0; i < Args.size(); ++i) {
    if (Args[i]->isValueDependent()) {
      QualType Ty = Context.DependentTy;
      return new (Context) ReflectionTraitExpr(Context, Kind, Ty, Args,
                                               APValue(), KWLoc, RParenLoc);
    }
  }

  // Ensure that each operand has integral type.
  for (unsigned i = 0; i < Args.size(); ++i) {
    Expr *E = Args[i];
    if (!E->getType()->isIntegerType()) {
      Diag(E->getLocStart(), diag::err_expr_not_ice) << 1;
      return ExprError();
    }
  }

  // Evaluate all of the operands ahead of time. Note that trait arity
  // is checked at parse time.
  SmallVector<llvm::APSInt, 2> Vals;
  Vals.resize(Args.size());
  for (unsigned i = 0; i < Args.size(); ++i) {
    Expr *E = Args[i];
    if (!E->EvaluateAsInt(Vals[i], Context)) {
      Diag(E->getLocStart(), diag::err_expr_not_ice) << 1;
      return ExprError();
    }
  }

  // FIXME: Verify that this is actually a reflected node.
  std::pair<ReflectionKind, void *> Info =
      ExplodeOpaqueValue(Vals[0].getExtValue());

  Reflector R{*this, KWLoc, RParenLoc, Args, Vals};
  if (Info.first == RK_Decl)
    return R.Reflect(Kind, (Decl *)Info.second);
  if (Info.first == RK_Type)
    return R.Reflect(Kind, (Type *)Info.second);

  llvm_unreachable("Unhandled reflection");
}

// Returns a string literal having the given name.
static ExprResult MakeString(ASTContext &C, const std::string &Str) {
  llvm::APSInt Size = C.MakeIntValue(Str.size() + 1, C.getSizeType());
  QualType Elem = C.getConstType(C.CharTy);
  QualType Type = C.getConstantArrayType(Elem, Size, ArrayType::Normal, 0);
  return StringLiteral::Create(C, Str, StringLiteral::Ascii, false, Type,
                               SourceLocation());
}

ExprResult Reflector::Reflect(ReflectionTrait RT, Decl *D) {
  switch (RT) {
  // Name(s) of a declaration.
  case URT_ReflectName:
    return ReflectName(D);
  case URT_ReflectQualifiedName:
    return ReflectQualifiedName(D);

  case URT_ReflectDeclarationContext:
    return ReflectDeclarationContext(D);

  case URT_ReflectLexicalContext:
    return ReflectLexicalContext(D);

  case URT_ReflectLinkage:
    return ReflectLinkage(D);

  case URT_ReflectStorage:
    return ReflectStorage(D);

  case URT_ReflectPointer:
    return ReflectPointer(D);

  case URT_ReflectType:
    return ReflectType(D);

  case URT_ReflectNumParameters:
    return ReflectNumParameters(D);
  case BRT_ReflectParameter:
    return ReflectParameter(D, Vals[1]);

  case URT_ReflectNumMembers:
    return ReflectNumMembers(D);
  case BRT_ReflectMember:
    return ReflectMember(D, Vals[1]);

  default:
    break;
  }

  // FIXME: Improve this error message.
  S.Diag(KWLoc, diag::err_reflection_not_supported);
  return ExprError();
}

ExprResult Reflector::Reflect(ReflectionTrait RT, Type *T) {
  switch (RT) {
  // Type name.
  case URT_ReflectName:
    return ReflectName(T);
  case URT_ReflectQualifiedName:
    return ReflectQualifiedName(T);

  case URT_ReflectDeclarationContext:
    return ReflectDeclarationContext(T);

  case URT_ReflectLexicalContext:
    return ReflectLexicalContext(T);

  case URT_ReflectNumMembers:
    return ReflectNumMembers(T->getAsTagDecl());
  case BRT_ReflectMember:
    return ReflectMember(T->getAsTagDecl(), Vals[1]);

  default:
    break;
  }

  // FIXME: Improve this error message.
  S.Diag(KWLoc, diag::err_reflection_not_supported);
  return ExprError();
}

// Returns a named declaration or emits an error and returns nullptr.
static NamedDecl *RequireNamedDecl(Reflector &R, Decl *D) {
  Sema &S = R.S;
  if (!isa<NamedDecl>(D)) {
    S.Diag(R.Args[0]->getLocStart(), diag::err_reflection_not_named);
    return nullptr;
  }
  return cast<NamedDecl>(D);
}

ExprResult Reflector::ReflectName(Decl *D) {
  if (NamedDecl *ND = RequireNamedDecl(*this, D))
    return MakeString(S.Context, ND->getNameAsString());
  return ExprError();
}

ExprResult Reflector::ReflectName(Type *T) {
  // Use the underlying declaration of tag types for the name. This way,
  // we won't generate "struct or enum" as part of the type.
  if (TagDecl *TD = T->getAsTagDecl())
    return MakeString(S.Context, TD->getNameAsString());
  QualType QT(T, 0);
  return MakeString(S.Context, QT.getAsString());
}

ExprResult Reflector::ReflectQualifiedName(Decl *D) {
  if (NamedDecl *ND = RequireNamedDecl(*this, D))
    return MakeString(S.Context, ND->getQualifiedNameAsString());
  return ExprError();
}

ExprResult Reflector::ReflectQualifiedName(Type *T) {
  if (TagDecl *TD = T->getAsTagDecl())
    return MakeString(S.Context, TD->getQualifiedNameAsString());
  QualType QT(T, 0);
  return MakeString(S.Context, QT.getAsString());
}

// TODO: Currently, this fails to return a declaration context for the
// translation unit and for builtin types (because they aren't declared).
// Perhaps we should return an empty context?

// Reflects the declaration context of D.
ExprResult Reflector::ReflectDeclarationContext(Decl *D) {
  if (isa<TranslationUnitDecl>(D)) {
    S.Diag(KWLoc, diag::err_reflection_not_supported);
    return ExprError();
  }
  return S.BuildDeclReflection(KWLoc, cast<Decl>(D->getDeclContext()));
}

// Reflects the declaration context of a user-defined type T.
//
// TODO: Emit a better error for non-declared types.
ExprResult Reflector::ReflectDeclarationContext(Type *T) {
  if (TagDecl *TD = T->getAsTagDecl()) {
    Decl *D = cast<Decl>(TD->getDeclContext());
    return S.BuildDeclReflection(KWLoc, D);
  }
  S.Diag(KWLoc, diag::err_reflection_not_supported);
  return ExprError();
}

// Reflects the lexical declaration context of D.
ExprResult Reflector::ReflectLexicalContext(Decl *D) {
  if (isa<TranslationUnitDecl>(D)) {
    S.Diag(KWLoc, diag::err_reflection_not_supported);
    return ExprError();
  }
  return S.BuildDeclReflection(KWLoc, cast<Decl>(D->getLexicalDeclContext()));
}

// Reflects the lexical declaration context of a user-defined type T.
//
// TODO: Emit a better error for non-declared types.
ExprResult Reflector::ReflectLexicalContext(Type *T) {
  if (TagDecl *TD = T->getAsTagDecl()) {
    Decl *D = cast<Decl>(TD->getLexicalDeclContext());
    return S.BuildDeclReflection(KWLoc, D);
  }
  S.Diag(KWLoc, diag::err_reflection_not_supported);
  return ExprError();
}

// Reflects the linkage of the declaration D.
ExprResult Reflector::ReflectLinkage(Decl *D) {
  if (NamedDecl *ND = RequireNamedDecl(*this, D)) {
    ASTContext &C = S.Context;
    QualType T = C.IntTy;
    llvm::APSInt N = C.MakeIntValue((int)ND->getFormalLinkage(), T);
    return IntegerLiteral::Create(C, N, T, KWLoc);
  }
  return ExprError();
}

// Reflects the storage class of the variable declaration D.
ExprResult Reflector::ReflectVariableStorage(VarDecl *D) {
  ASTContext &C = S.Context;
  QualType T = C.IntTy;
  llvm::APSInt N = C.MakeIntValue((int)D->getStorageClass(), T);
  return IntegerLiteral::Create(C, N, T, KWLoc);
}

// Reflects a pointer the given declaration. This only applies to global
// variables, member variables, and functions.
//
// TODO: We can actually reflect pointers to local variables by storing
// them within the reflected object. For example:
//
//    void f() {
//      int x = 0;
//      auto p = $x; // constructs a temp with a pointer to x.
//      (void)*p.pointer(); // evaluates to 0.
//
// This would require that local declarations reflect differently than
// global declarations.
ExprResult Reflector::ReflectPointer(Decl *D) {
  // We can only take the address of stored declarations.
  if (!isa<VarDecl>(D) && !isa<FieldDecl>(D) && !isa<FunctionDecl>(D)) {
    S.Diag(KWLoc, diag::err_reflection_not_supported);
    return ExprError();
  }
  ValueDecl *Val = cast<ValueDecl>(D);

  // Don't produce addresses to local variables; they aren't static.
  if (VarDecl *VD = dyn_cast<VarDecl>(Val)) {
    if (VD->hasLocalStorage()) {
      S.Diag(Args[0]->getLocStart(), diag::err_reflection_of_local_reference);
      return ExprError();
    }
  }

  // Determine the type of the declaration pointed at. If it's a member of
  // a class, we need to adjust it to a member pointer type.
  //
  // TODO: This is a subset of what CreateBuiltinUnaryOp does, but for
  // some reason, the result isn't being typed correctly.
  QualType Ty = Val->getType();
  if (FieldDecl *FD = dyn_cast<FieldDecl>(Val)) {
    const Type *Cls = S.Context.getTagDeclType(FD->getParent()).getTypePtr();
    Ty = S.Context.getMemberPointerType(Ty, Cls);
  } else if (isa<CXXConstructorDecl>(Val) || isa<CXXDestructorDecl>(D)) {
    // TODO: Find a better error message to emit.
    S.Diag(KWLoc, diag::err_reflection_not_supported);
    return ExprError();
  } else if (CXXMethodDecl *MD = dyn_cast<CXXMethodDecl>(Val)) {
    const Type *Cls = S.Context.getTagDeclType(MD->getParent()).getTypePtr();
    Ty = S.Context.getMemberPointerType(Ty, Cls);
  } else {
    Ty = S.Context.getPointerType(Ty);
  }

  DeclRefExpr *Ref =
      new (S.Context) DeclRefExpr(Val, false, Val->getType(), VK_LValue, KWLoc);
  S.MarkDeclRefReferenced(Ref);
  Expr *Op = new (S.Context)
      UnaryOperator(Ref, UO_AddrOf, Ty, VK_RValue, OK_Ordinary, KWLoc);
  return Op;
}

// Reflects the storage class of the function declaration D.
ExprResult Reflector::ReflectFunctionStorage(FunctionDecl *D) {
  ASTContext &C = S.Context;
  QualType T = C.IntTy;
  llvm::APSInt N = C.MakeIntValue((int)D->getStorageClass(), T);
  return IntegerLiteral::Create(C, N, T, KWLoc);
}

// Reflects the storage class of the declaration D.
ExprResult Reflector::ReflectStorage(Decl *D) {
  if (VarDecl *Var = dyn_cast<VarDecl>(D))
    return ReflectVariableStorage(Var);
  if (FunctionDecl *Fn = dyn_cast<FunctionDecl>(D))
    return ReflectFunctionStorage(Fn);

  // FIXME: This is the wrong error message. Should be D does not have
  // a storage class.
  S.Diag(Args[0]->getLocStart(), diag::err_reflection_not_named);
  return ExprError();
}

// Reflects the type the typed declaration D.
ExprResult Reflector::ReflectType(Decl *D) {
  if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
    return S.BuildTypeReflection(KWLoc, VD->getType());
  S.Diag(Args[0]->getLocStart(), diag::err_reflection_not_typed);
  return ExprError();
}

// Returns a function declaration or emits a diagnostic and returns null.
static FunctionDecl *RequireFunctionDecl(Reflector &R, Decl *D) {
  if (!isa<FunctionDecl>(D)) {
    R.S.Diag(R.Args[0]->getLocStart(), diag::err_reflection_not_function);
    return nullptr;
  }
  return cast<FunctionDecl>(D);
}

// Reflects the number of parameters of a function declaration.
ExprResult Reflector::ReflectNumParameters(Decl *D) {
  if (FunctionDecl *Fn = RequireFunctionDecl(*this, D)) {
    ASTContext &C = S.Context;
    QualType T = C.UnsignedIntTy;
    llvm::APSInt N = C.MakeIntValue(Fn->getNumParams(), T);
    return IntegerLiteral::Create(C, N, T, KWLoc);
  }
  return ExprError();
}

// Reflects a selected parameter of a function.
ExprResult Reflector::ReflectParameter(Decl *D, const llvm::APSInt &N) {
  if (FunctionDecl *Fn = RequireFunctionDecl(*this, D)) {
    unsigned Num = N.getExtValue();
    if (Num >= Fn->getNumParams()) {
      S.Diag(Args[1]->getLocStart(), diag::err_parameter_out_of_bounds);
      return ExprError();
    }
    return S.BuildDeclReflection(KWLoc, Fn->getParamDecl(Num));
  }
  return ExprError();
}

static NamespaceDecl *RequireNamespace(Reflector &R, Decl *D) {
  if (NamespaceDecl *NS = dyn_cast<NamespaceDecl>(D))
    return NS;
  R.S.Diag(R.Args[0]->getLocStart(), diag::err_reflection_not_supported);
  return nullptr;
}

// Reflects the number of elements in the context.
template <typename I> ExprResult Reflector::GetNumMembers(I First, I Limit) {
  ASTContext &C = S.Context;
  QualType T = C.UnsignedIntTy;
  unsigned D = std::distance(First, Limit);
  llvm::APSInt N = C.MakeIntValue(D, T);
  return IntegerLiteral::Create(C, N, T, KWLoc);
}

// Reflects the selected member from the declaration.
template <typename I>
ExprResult Reflector::GetMember(const llvm::APSInt &N, I First, I Limit) {
  unsigned Ix = N.getExtValue();
  if (Ix >= std::distance(First, Limit)) {
    S.Diag(Args[1]->getLocStart(), diag::err_parameter_out_of_bounds);
    return ExprError();
  }
  std::advance(First, Ix);
  return S.BuildDeclReflection(KWLoc, *First);
}

// TODO: The semantics of this query on namespaces are questionable. Should
// we also include members in the anonymous namespace? If not, how could we
// access those? Another trait perhaps. What about imported declarations?
// We probably also need to walk the namespace backwards through previous
// declarations.
ExprResult Reflector::ReflectNumMembers(Decl *D) {
  if (D) {
    if (TagDecl *TD = dyn_cast<TagDecl>(D))
      return GetNumMembers(TD->decls_begin(), TD->decls_end());
    if (NamespaceDecl *NS = RequireNamespace(*this, D))
      return GetNumMembers(NS->decls_begin(), NS->decls_end());
  }
  S.Diag(Args[0]->getLocStart(), diag::err_reflection_not_supported);
  return ExprError();
}

// Reflects the selected member from the declaration.
ExprResult Reflector::ReflectMember(Decl *D, const llvm::APSInt &N) {
  if (D) {
    if (TagDecl *TD = dyn_cast<TagDecl>(D))
      return GetMember(N, TD->decls_begin(), TD->decls_end());
    if (NamespaceDecl *NS = RequireNamespace(*this, D))
      return GetMember(N, NS->decls_begin(), NS->decls_end());
  }
  S.Diag(Args[0]->getLocStart(), diag::err_reflection_not_supported);
  return ExprError();
}

DeclResult Sema::ActOnMetaclassDefinition(SourceLocation DollarLoc,
                                          IdentifierInfo *II, Stmt *Body) {
  assert(isa<CompoundStmt>(Body));

  // TODO: Create an AST node for the metaclass and actually declare this.
  Body->dump();

  return DeclResult();
}
