//===--- AssociatedTypeInference.cpp - Associated Type Inference ---000----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements type witness lookup and associated type inference.
//
//===----------------------------------------------------------------------===//
#include "TypeCheckProtocol.h"
#include "DerivedConformances.h"
#include "TypeAccessScopeChecker.h"
#include "TypeChecker.h"

#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/ClangImporter/ClangModule.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/TinyPtrVector.h"

#define DEBUG_TYPE "Associated type inference"
#include "llvm/Support/Debug.h"

STATISTIC(NumSolutionStates, "# of solution states visited");
STATISTIC(NumSolutionStatesFailedCheck,
          "# of solution states that failed constraints check");
STATISTIC(NumConstrainedExtensionChecks,
          "# of constrained extension checks");
STATISTIC(NumConstrainedExtensionChecksFailed,
          "# of constrained extension checks failed");
STATISTIC(NumDuplicateSolutionStates,
          "# of duplicate solution states ");

using namespace swift;

namespace {

/// Describes the result of checking a type witness.
///
/// This class evaluates true if an error occurred.
class CheckTypeWitnessResult {
public:
  enum Kind {
    Success,

    /// Type witness contains an error type.
    Error,

    /// Type witness does not satisfy a conformance requirement on
    /// the associated type.
    Conformance,

    /// Type witness does not satisfy a superclass requirement on
    /// the associated type.
    Superclass,

    /// Type witness does not satisfy a layout requirement on
    /// the associated type.
    Layout,

    /// Type witness of a tuple conformance does not have the form
    /// (repeat (each Element).A).
    Tuple
  } kind;

private:
  Type reqt;

  CheckTypeWitnessResult() : kind(Success) {}

  CheckTypeWitnessResult(Kind kind, Type reqt)
    : kind(kind), reqt(reqt) {}

public:
  static CheckTypeWitnessResult forSuccess() {
    return CheckTypeWitnessResult(Success, Type());
  }

  static CheckTypeWitnessResult forError() {
    return CheckTypeWitnessResult(Error, Type());
  }

  static CheckTypeWitnessResult forConformance(ProtocolDecl *proto) {
    auto reqt = proto->getDeclaredInterfaceType();
    return CheckTypeWitnessResult(Conformance, reqt);
  }

  static CheckTypeWitnessResult forSuperclass(Type reqt) {
    assert(reqt->getClassOrBoundGenericClass());
    return CheckTypeWitnessResult(Superclass, reqt);
  }

  static CheckTypeWitnessResult forLayout(Type reqt) {
    return CheckTypeWitnessResult(Layout, reqt);
  }

  static CheckTypeWitnessResult forTuple(Type reqt) {
    return CheckTypeWitnessResult(Tuple, reqt);
  }

  Kind getKind() const { return kind; }
  Type getRequirement() const { return reqt; }

  explicit operator bool() const { return kind != Success; }
};

static CheckTypeWitnessResult
checkTypeWitness(Type type, AssociatedTypeDecl *assocType,
                 const NormalProtocolConformance *Conf,
                 SubstOptions options) {
  auto &ctx = assocType->getASTContext();

  if (type->hasError())
    return CheckTypeWitnessResult::forError();

  const auto proto = Conf->getProtocol();
  const auto dc = Conf->getDeclContext();
  const auto sig = proto->getGenericSignature();

  // FIXME: The RequirementMachine will assert on re-entrant construction.
  // We should find a more principled way of breaking this cycle.
  if (ctx.isRecursivelyConstructingRequirementMachine(sig.getCanonicalSignature()) ||
      ctx.isRecursivelyConstructingRequirementMachine(proto) ||
      proto->isComputingRequirementSignature())
    return CheckTypeWitnessResult::forError();

  if (!ctx.LangOpts.hasFeature(Feature::NoncopyableGenerics)
      && type->isNoncopyable()) {
    // No move-only type can witness an associatedtype requirement.
    // Pretend the failure is a lack of Copyable conformance.
    auto *copyable = ctx.getProtocol(KnownProtocolKind::Copyable);
    assert(copyable && "missing Copyable protocol!");
    return CheckTypeWitnessResult::forConformance(copyable);
  }

  const auto depTy = DependentMemberType::get(proto->getSelfInterfaceType(),
                                              assocType);

  Type contextType = type->hasTypeParameter() ? dc->mapTypeIntoContext(type)
                                              : type;

  if (auto superclass = sig->getSuperclassBound(depTy)) {
    if (superclass->hasTypeParameter()) {
      // Replace type parameters with other known or tentative type witnesses.
      superclass = superclass.subst(
          [&](SubstitutableType *type) {
            if (type->isEqual(proto->getSelfInterfaceType()))
              return Conf->getType();

            return Type();
          },
          LookUpConformanceInModule(dc->getParentModule()), options);

      if (superclass->hasTypeParameter())
        superclass = dc->mapTypeIntoContext(superclass);
    }
    if (!superclass->isExactSuperclassOf(contextType))
      return CheckTypeWitnessResult::forSuperclass(superclass);
  }

  auto *module = dc->getParentModule();

  // Check protocol conformances.
  for (const auto reqProto : sig->getRequiredProtocols(depTy)) {
    if (module->lookupConformance(
            contextType, reqProto,
            /*allowMissing=*/reqProto->isSpecificProtocol(
                KnownProtocolKind::Sendable))
            .isInvalid())
      return CheckTypeWitnessResult::forConformance(reqProto);
  }

  if (sig->requiresClass(depTy) &&
      !contextType->satisfiesClassConstraint()) {
    return CheckTypeWitnessResult::forLayout(
        module->getASTContext().getAnyObjectType());
  }

  // Tuple conformances can only witness associated types by projecting them
  // element-wise.
  if (isa<BuiltinTupleDecl>(dc->getSelfNominalTypeDecl())) {
    auto expectedTy = getTupleConformanceTypeWitness(dc, assocType);
    if (!expectedTy->isEqual(type)) {
      return CheckTypeWitnessResult::forTuple(expectedTy);
    }
  }

  // Success!
  return CheckTypeWitnessResult::forSuccess();
}

}

static void recordTypeWitness(NormalProtocolConformance *conformance,
                              AssociatedTypeDecl *assocType,
                              Type type,
                              TypeDecl *typeDecl) {
  // If we already recoded this type witness, there's nothing to do.
  if (conformance->hasTypeWitness(assocType)) {
    assert(conformance->getTypeWitnessUncached(assocType)
               .getWitnessType()
               ->isEqual(type) &&
           "Conflicting type witness deductions");
    return;
  }

  assert(!type->hasArchetype() && "Got a contextual type here?");

  auto *dc = conformance->getDeclContext();
  auto *proto = conformance->getProtocol();
  auto &ctx = dc->getASTContext();

  // If there was no type declaration, synthesize one.
  if (typeDecl == nullptr) {
    auto aliasDecl = new (ctx) TypeAliasDecl(
        SourceLoc(), SourceLoc(), assocType->getName(), SourceLoc(),
        /*genericparams*/ nullptr, dc);
    aliasDecl->setUnderlyingType(type);
    
    aliasDecl->setImplicit();
    aliasDecl->setSynthesized();

    // Inject the typealias into the nominal decl that conforms to the protocol.
    auto nominal = dc->getSelfNominalTypeDecl();
    auto requiredAccessScope = evaluateOrDefault(
        ctx.evaluator, ConformanceAccessScopeRequest{dc, proto},
        std::make_pair(AccessScope::getPublic(), false));

    if (!ctx.isSwiftVersionAtLeast(5) &&
        !dc->getParentModule()->isResilient()) {
      // HACK: In pre-Swift-5, these typealiases were synthesized with the
      // same access level as the conforming type, which might be more
      // visible than the associated type witness. Preserve that behavior
      // when the underlying type has sufficient access, but only in
      // non-resilient modules.
      llvm::Optional<AccessScope> underlyingTypeScope =
          TypeAccessScopeChecker::getAccessScope(type, dc,
                                                 /*usableFromInline*/ false);
      assert(underlyingTypeScope.has_value() &&
             "the type is already invalid and we shouldn't have gotten here");

      AccessScope nominalAccessScope = nominal->getFormalAccessScope(dc);
      llvm::Optional<AccessScope> widestPossibleScope =
          underlyingTypeScope->intersectWith(nominalAccessScope);
      assert(widestPossibleScope.has_value() &&
             "we found the nominal and the type witness, didn't we?");
      requiredAccessScope.first = widestPossibleScope.value();
    }

    // An associated type witness can never be less than fileprivate, since
    // it must always be at least as visible as the enclosing type.
    AccessLevel requiredAccess =
        std::max(requiredAccessScope.first.accessLevelForDiagnostics(),
                 AccessLevel::FilePrivate);

    aliasDecl->setAccess(requiredAccess);
    if (requiredAccessScope.second) {
      auto *attr = new (ctx) UsableFromInlineAttr(/*implicit=*/true);
      aliasDecl->getAttrs().add(attr);
    }

    if (nominal == dc) {
      nominal->addMember(aliasDecl);
    } else {
      auto ext = cast<ExtensionDecl>(dc);
      ext->addMember(aliasDecl);
    }

    typeDecl = aliasDecl;
  }

  // Record the type witness.
  conformance->setTypeWitness(assocType, type, typeDecl);

  // Record type witnesses for any "overridden" associated types.
  llvm::SetVector<AssociatedTypeDecl *> overriddenAssocTypes;
  auto assocOverriddenDecls = assocType->getOverriddenDecls();
  overriddenAssocTypes.insert(assocOverriddenDecls.begin(),
                              assocOverriddenDecls.end());
  for (unsigned idx = 0; idx < overriddenAssocTypes.size(); ++idx) {
    auto overridden = overriddenAssocTypes[idx];

    // Note all of the newly-discovered overridden associated types.
    auto overriddenDecls = overridden->getOverriddenDecls();
    overriddenAssocTypes.insert(overriddenDecls.begin(), overriddenDecls.end());

    // Find the conformance for this overridden protocol.
    auto overriddenConformance =
      dc->getParentModule()->lookupConformance(dc->getSelfInterfaceType(),
                                               overridden->getProtocol(),
                                               /*allowMissing=*/true);
    if (overriddenConformance.isInvalid() ||
        !overriddenConformance.isConcrete())
      continue;

    auto *overriddenRootConformance =
        overriddenConformance.getConcrete()->getRootNormalConformance();
    auto *overriddenRootConformanceDC =
        overriddenRootConformance->getDeclContext();

    // Don't record a type witness for an overridden associated type if the
    // conformance to the corresponding inherited protocol
    // - originates in a superclass
    // - originates in a different module
    // - and the current conformance have mismatching conditional requirements
    // This can turn out badly in two ways:
    // - Foremost, we must not *alter* conformances originating in superclasses
    //   or other modules. In other cases, we may hit an assertion in an attempt
    //   to overwrite an already recorded type witness with a different one.
    //   For example, the recorded type witness may be invalid, whereas the
    //   other one---valid, and vice versa.
    // - If the current conformance is more restrictive, this type witness may
    //   not be a viable candidate for the overridden associated type.
    if (overriddenRootConformanceDC->getSelfNominalTypeDecl() !=
        dc->getSelfNominalTypeDecl())
      continue;

    if (overriddenRootConformanceDC->getParentModule() != dc->getParentModule())
      continue;

    auto currConformanceSig = dc->getGenericSignatureOfContext();
    auto overriddenConformanceSig =
        overriddenRootConformanceDC->getGenericSignatureOfContext();
    if (currConformanceSig.getCanonicalSignature() !=
        overriddenConformanceSig.getCanonicalSignature())
      continue;

    recordTypeWitness(overriddenRootConformance, overridden, type, typeDecl);
  }
}

/// Attempt to resolve a type witness via member name lookup.
static ResolveWitnessResult resolveTypeWitnessViaLookup(
                       NormalProtocolConformance *conformance,
                       AssociatedTypeDecl *assocType) {
  auto *dc = conformance->getDeclContext();
  auto &ctx = dc->getASTContext();

  // Conformances constructed by the ClangImporter should have explicit type
  // witnesses already.
  if (isa<ClangModuleUnit>(dc->getModuleScopeContext())) {
    llvm::errs() << "Cannot look up associated type for imported conformance:\n";
    conformance->getType().dump(llvm::errs());
    assocType->dump(llvm::errs());
    abort();
  }

  NLOptions subOptions = (NL_QualifiedDefault | NL_OnlyTypes | NL_ProtocolMembers);

  // Look for a member type with the same name as the associated type.
  SmallVector<ValueDecl *, 4> candidates;

  dc->lookupQualified(dc->getSelfNominalTypeDecl(), assocType->createNameRef(),
                      dc->getSelfNominalTypeDecl()->getLoc(), subOptions,
                      candidates);

  // If there aren't any candidates, we're done.
  if (candidates.empty()) {
    return ResolveWitnessResult::Missing;
  }

  // Determine which of the candidates is viable.
  SmallVector<LookupTypeResultEntry, 2> viable;
  SmallVector<std::pair<TypeDecl *, CheckTypeWitnessResult>, 2> nonViable;
  SmallPtrSet<CanType, 4> viableTypes;

  for (auto candidate : candidates) {
    auto *typeDecl = cast<TypeDecl>(candidate);

    // Skip other associated types.
    if (isa<AssociatedTypeDecl>(typeDecl))
      continue;

    auto *genericDecl = cast<GenericTypeDecl>(typeDecl);

    // If the declaration has generic parameters, it cannot witness an
    // associated type.
    if (genericDecl->isGeneric())
      continue;

    // As a narrow fix for a source compatibility issue with SwiftUI's
    // swiftinterface, allow a 'typealias' type witness with an underlying type
    // of 'Never' if it is declared in a context that does not satisfy the
    // requirements of the conformance context.
    //
    // FIXME: Remove this eventually.
    bool skipRequirementCheck = false;
    if (auto *typeAliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      if (typeAliasDecl->getParentModule()->getName().is("SwiftUI") &&
          typeAliasDecl->getParentSourceFile() &&
          typeAliasDecl->getParentSourceFile()->Kind == SourceFileKind::Interface) {
        if (typeAliasDecl->getUnderlyingType()->isNever()) {
          if (typeAliasDecl->getDeclContext()->getSelfNominalTypeDecl() ==
              dc->getSelfNominalTypeDecl()) {
            skipRequirementCheck = true;
          }
        }
      }
    }

    // Skip typealiases with an unbound generic type as their underlying type.
    if (auto *typeAliasDecl = dyn_cast<TypeAliasDecl>(typeDecl))
      if (typeAliasDecl->getDeclaredInterfaceType()->is<UnboundGenericType>())
        continue;

    // Skip dependent protocol typealiases.
    //
    // FIXME: This should not be necessary.
    if (auto *typeAliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      if (isa<ProtocolDecl>(typeAliasDecl->getDeclContext()) &&
          typeAliasDecl->getUnderlyingType()->getCanonicalType()
            ->hasTypeParameter()) {
        continue;
      }
    }

    // If the type comes from a constrained extension or has a 'where'
    // clause, check those requirements now.
    if (!skipRequirementCheck &&
        !TypeChecker::checkContextualRequirements(
            genericDecl, dc->getSelfInterfaceType(), SourceLoc(),
            dc->getParentModule(), dc->getGenericSignatureOfContext())) {
      continue;
    }

    auto memberType = TypeChecker::substMemberTypeWithBase(
        dc->getParentModule(), typeDecl, dc->getSelfInterfaceType());

    // Type witnesses that resolve to constraint types are always
    // existential types. This can only happen when the type witness
    // is explicitly written with a type alias. The type alias itself
    // is still a constraint type because it can be used as both a
    // type witness and as a generic constraint.
    //
    // With SE-0335, using a type alias as both a type witness and a generic
    // constraint will be disallowed in Swift 6, because existential types
    // must be explicit, and a generic constraint isn't a valid type witness.
    if (memberType->isConstraintType()) {
      memberType = ExistentialType::get(memberType);
    }

    if (!viableTypes.insert(memberType->getCanonicalType()).second)
      continue;

    // Check this type against the protocol requirements.
    if (auto checkResult =
            checkTypeWitness(memberType, assocType, conformance, llvm::None)) {
      nonViable.push_back({typeDecl, checkResult});
    } else {
      viable.push_back({typeDecl, memberType, nullptr});
    }
  }

  // If there are no viable witnesses, and all nonviable candidates came from
  // protocol extensions, treat this as "missing".
  if (viable.empty() &&
      std::find_if(nonViable.begin(), nonViable.end(),
                   [](const std::pair<TypeDecl *, CheckTypeWitnessResult> &x) {
                     return x.first->getDeclContext()
                        ->getSelfProtocolDecl() == nullptr;
                   }) == nonViable.end())
    return ResolveWitnessResult::Missing;

  // If there is a single viable candidate, form a substitution for it.
  if (viable.size() == 1) {
    auto interfaceType = viable.front().MemberType;
    recordTypeWitness(conformance, assocType, interfaceType,
                      viable.front().Member);
    return ResolveWitnessResult::Success;
  }

  // Record an error.
  recordTypeWitness(conformance, assocType,
                    ErrorType::get(ctx), nullptr);

  // If we had multiple viable types, diagnose the ambiguity.
  if (!viable.empty()) {
    ctx.addDelayedConformanceDiag(conformance, true,
      [assocType, viable](NormalProtocolConformance *conformance) {
        auto &diags = assocType->getASTContext().Diags;
        diags.diagnose(assocType, diag::ambiguous_witnesses_type,
                       assocType->getName());

        for (auto candidate : viable)
          diags.diagnose(candidate.Member, diag::protocol_witness_type);
      });

    return ResolveWitnessResult::ExplicitFailed;
  }
  // Save the missing type witness for later diagnosis.
  ctx.addDelayedMissingWitness(conformance, {assocType, {}});

  // None of the candidates were viable.
  ctx.addDelayedConformanceDiag(conformance, true,
    [nonViable](NormalProtocolConformance *conformance) {
      auto &diags = conformance->getDeclContext()->getASTContext().Diags;
      for (auto candidate : nonViable) {
        if (candidate.first->getDeclaredInterfaceType()->hasError() ||
            candidate.second.getKind() == CheckTypeWitnessResult::Error)
          continue;

        switch (candidate.second.getKind()) {
        case CheckTypeWitnessResult::Success:
        case CheckTypeWitnessResult::Error:
          llvm_unreachable("Should not end up here");

        case CheckTypeWitnessResult::Conformance:
        case CheckTypeWitnessResult::Layout:
          diags.diagnose(
             candidate.first,
             diag::protocol_type_witness_unsatisfied_conformance,
             candidate.first->getDeclaredInterfaceType(),
             candidate.second.getRequirement());
          break;

        case CheckTypeWitnessResult::Superclass:
          diags.diagnose(
             candidate.first,
             diag::protocol_type_witness_unsatisfied_superclass,
             candidate.first->getDeclaredInterfaceType(),
             candidate.second.getRequirement());
          break;

        case CheckTypeWitnessResult::Tuple:
          diags.diagnose(
             candidate.first,
             diag::protocol_type_witness_tuple,
             candidate.first->getDeclaredInterfaceType(),
             candidate.second.getRequirement());
          break;
        }
      }
    });

  return ResolveWitnessResult::ExplicitFailed;
}

namespace {

/// The set of associated types that have been inferred by matching
/// the given value witness to its corresponding requirement.
struct InferredAssociatedTypesByWitness {
  /// The witness we matched.
  ValueDecl *Witness = nullptr;

  /// The associated types inferred from matching this witness.
  SmallVector<std::pair<AssociatedTypeDecl *, Type>, 4> Inferred;

  /// Inferred associated types that don't meet the associated type
  /// requirements.
  SmallVector<std::tuple<AssociatedTypeDecl *, Type, CheckTypeWitnessResult>,
              2> NonViable;

  void dump(llvm::raw_ostream &out, unsigned indent) const;

  SWIFT_DEBUG_DUMP;
};

}

void InferredAssociatedTypesByWitness::dump() const {
  dump(llvm::errs(), 0);
}

void InferredAssociatedTypesByWitness::dump(llvm::raw_ostream &out,
                                            unsigned indent) const {
  out << "\n";
  out.indent(indent) << "(";
  if (Witness) {
    Witness->dumpRef(out);
  }

  for (const auto &inferred : Inferred) {
    out << "\n";
    out.indent(indent + 2);
    out << inferred.first->getName() << " := "
        << inferred.second.getString();
  }

  for (const auto &inferred : NonViable) {
    out << "\n";
    out.indent(indent + 2);
    out << std::get<0>(inferred)->getName() << " := "
        << std::get<1>(inferred).getString();
    auto type = std::get<2>(inferred).getRequirement();
    out << " [failed constraint " << type.getString() << "]";
  }

  out << ")";
}

/// The set of witnesses that were considered when attempting to
/// infer associated types.
using InferredAssociatedTypesByWitnesses =
    SmallVector<InferredAssociatedTypesByWitness, 2>;

/// A mapping from requirements to the set of matches with witnesses.
using InferredAssociatedTypes =
    SmallVector<std::pair<ValueDecl *, InferredAssociatedTypesByWitnesses>, 4>;

namespace {

void dumpInferredAssociatedTypesByWitnesses(
      const InferredAssociatedTypesByWitnesses &inferred,
      llvm::raw_ostream &out,
      unsigned indent) {
  for (const auto &value : inferred) {
    value.dump(out, indent);
  }
}

void dumpInferredAssociatedTypesByWitnesses(
      const InferredAssociatedTypesByWitnesses &inferred) LLVM_ATTRIBUTE_USED;

void dumpInferredAssociatedTypesByWitnesses(
                        const InferredAssociatedTypesByWitnesses &inferred) {
  dumpInferredAssociatedTypesByWitnesses(inferred, llvm::errs(), 0);
}

void dumpInferredAssociatedTypes(const InferredAssociatedTypes &inferred,
                                 llvm::raw_ostream &out,
                                 unsigned indent) {
  for (const auto &value : inferred) {
    out << "\n";
    out.indent(indent) << "(";
    value.first->dumpRef(out);
    dumpInferredAssociatedTypesByWitnesses(value.second, out, indent + 2);
    out << ")";
  }
  out << "\n";
}

void dumpInferredAssociatedTypes(
       const InferredAssociatedTypes &inferred) LLVM_ATTRIBUTE_USED;

void dumpInferredAssociatedTypes(const InferredAssociatedTypes &inferred) {
  dumpInferredAssociatedTypes(inferred, llvm::errs(), 0);
}

/// A conflict between two inferred type witnesses for the same
/// associated type.
struct TypeWitnessConflict {
  /// The associated type.
  AssociatedTypeDecl *AssocType;

  /// The first type.
  Type FirstType;

  /// The requirement to which the first witness was matched.
  ValueDecl *FirstRequirement;

  /// The witness from which the first type witness was inferred.
  ValueDecl *FirstWitness;

  /// The second type.
  Type SecondType;

  /// The requirement to which the second witness was matched.
  ValueDecl *SecondRequirement;

  /// The witness from which the second type witness was inferred.
  ValueDecl *SecondWitness;
};

/// A type witness inferred without the aid of a specific potential
/// value witness.
class AbstractTypeWitness {
  AssociatedTypeDecl *AssocType;
  Type TheType;

  /// The defaulted associated type that was used to infer this type witness.
  /// Need not necessarily match \c AssocType, but their names must.
  AssociatedTypeDecl *DefaultedAssocType;

public:
  AbstractTypeWitness(AssociatedTypeDecl *AssocType, Type TheType,
                      AssociatedTypeDecl *DefaultedAssocType = nullptr)
      : AssocType(AssocType), TheType(TheType),
        DefaultedAssocType(DefaultedAssocType) {
    assert(AssocType && TheType);
    assert(!DefaultedAssocType ||
           (AssocType->getName() == DefaultedAssocType->getName()));
  }

  AssociatedTypeDecl *getAssocType() const { return AssocType; }

  Type getType() const { return TheType; }

  AssociatedTypeDecl *getDefaultedAssocType() const {
    return DefaultedAssocType;
  }
};

/// A potential solution to the set of inferred type witnesses.
struct InferredTypeWitnessesSolution {
  /// The set of type witnesses inferred by this solution, along
  /// with the index into the value witnesses where the type was
  /// inferred.
  llvm::SmallDenseMap<AssociatedTypeDecl *, std::pair<Type, unsigned>, 4>
    TypeWitnesses;

  /// The value witnesses selected by this step of the solution.
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> ValueWitnesses;

  /// The number of value witnesses that occur in protocol
  /// extensions.
  unsigned NumValueWitnessesInProtocolExtensions;

#ifndef NDEBUG
  LLVM_ATTRIBUTE_USED
#endif
  void dump() const;
};

void InferredTypeWitnessesSolution::dump() const {
  const auto numValueWitnesses = ValueWitnesses.size();
  llvm::errs() << "Type Witnesses:\n";
  for (auto &typeWitness : TypeWitnesses) {
    llvm::errs() << "  " << typeWitness.first->getName() << " := ";
    typeWitness.second.first->print(llvm::errs());
    if (typeWitness.second.second == numValueWitnesses) {
      llvm::errs() << ", abstract";
    } else {
      llvm::errs() << ", inferred from $" << typeWitness.second.second;
    }
    llvm::errs() << '\n';
  }
  llvm::errs() << "Value Witnesses:\n";
  for (unsigned i : indices(ValueWitnesses)) {
    const auto &valueWitness = ValueWitnesses[i];
    llvm::errs() << '$' << i << ":\n  ";
    valueWitness.first->dumpRef(llvm::errs());
    llvm::errs() << " ->\n  ";
    valueWitness.second->dumpRef(llvm::errs());
    llvm::errs() << '\n';
  }
}

/// A system for recording and probing the integrity of a type witness solution
/// for a set of unresolved associated type declarations.
///
/// Right now can reason only about abstract type witnesses, i.e., same-type
/// constraints, default type definitions, and bindings to generic parameters.
class TypeWitnessSystem final {
  /// Equivalence classes are used on demand to express equivalences between
  /// witness candidates and reflect changes to resolved types across their
  /// members.
  class EquivalenceClass final {
    /// The pointer:
    /// - The resolved type for witness candidates belonging to this equivalence
    ///   class. The resolved type may be a type parameter, but cannot directly
    ///   pertain to a name variable in the owning system; instead, witness
    ///   candidates that should resolve to the same type share an equivalence
    ///   class.
    /// The int:
    /// - A flag indicating whether the resolved type is ambiguous. When set,
    ///   the resolved type is null.
    llvm::PointerIntPair<Type, 1, bool> ResolvedTyAndIsAmbiguous;

  public:
    EquivalenceClass(Type ty) : ResolvedTyAndIsAmbiguous(ty, false) {}

    EquivalenceClass(const EquivalenceClass &) = delete;
    EquivalenceClass(EquivalenceClass &&) = delete;
    EquivalenceClass &operator=(const EquivalenceClass &) = delete;
    EquivalenceClass &operator=(EquivalenceClass &&) = delete;

    Type getResolvedType() const {
      return ResolvedTyAndIsAmbiguous.getPointer();
    }
    void setResolvedType(Type ty);

    bool isAmbiguous() const {
      return ResolvedTyAndIsAmbiguous.getInt();
    }
    void setAmbiguous() {
      ResolvedTyAndIsAmbiguous = {nullptr, true};
    }
  };

  /// A type witness candidate for a name variable.
  struct TypeWitnessCandidate final {
    /// The defaulted associated type declaration correlating with this
    /// candidate, if present.
    AssociatedTypeDecl *DefaultedAssocType;

    /// The equivalence class of this candidate.
    EquivalenceClass *EquivClass;
  };

  /// The set of equivalence classes in the system.
  llvm::SmallPtrSet<EquivalenceClass *, 4> EquivalenceClasses;

  /// The mapping from name variables (the names of unresolved associated
  /// type declarations) to their corresponding type witness candidates.
  llvm::SmallDenseMap<Identifier, TypeWitnessCandidate, 4> TypeWitnesses;

public:
  TypeWitnessSystem(ArrayRef<AssociatedTypeDecl *> assocTypes);
  ~TypeWitnessSystem();

  TypeWitnessSystem(const TypeWitnessSystem &) = delete;
  TypeWitnessSystem(TypeWitnessSystem &&) = delete;
  TypeWitnessSystem &operator=(const TypeWitnessSystem &) = delete;
  TypeWitnessSystem &operator=(TypeWitnessSystem &&) = delete;

  /// Get the resolved type witness for the associated type with the given name.
  Type getResolvedTypeWitness(Identifier name) const;
  bool hasResolvedTypeWitness(Identifier name) const;

  /// Get the defaulted associated type relating to the resolved type witness
  /// for the associated type with the given name, if present.
  AssociatedTypeDecl *getDefaultedAssocType(Identifier name) const;

  /// Record a type witness for the given associated type name.
  ///
  /// \note This need not lead to the resolution of a type witness, e.g.
  /// an associated type may be defaulted to another.
  void addTypeWitness(Identifier name, Type type);

  /// Record a default type witness.
  ///
  /// \param defaultedAssocType The specific associated type declaration that
  /// defines the given default type.
  ///
  /// \note This need not lead to the resolution of a type witness.
  void addDefaultTypeWitness(Type type, AssociatedTypeDecl *defaultedAssocType);

  /// Record the given same-type requirement, if regarded of interest to
  /// the system.
  ///
  /// \note This need not lead to the resolution of a type witness.
  void addSameTypeRequirement(const Requirement &req);

  void dump(llvm::raw_ostream &out,
            const NormalProtocolConformance *conformance) const;

private:
  /// Form an equivalence between the given name variables.
  void addEquivalence(Identifier name1, Identifier name2);

  /// Merge \p equivClass2 into \p equivClass1.
  ///
  /// \note This will delete \p equivClass2 after migrating its members to
  /// \p equivClass1.
  void mergeEquivalenceClasses(EquivalenceClass *equivClass1,
                               const EquivalenceClass *equivClass2);

  /// The result of comparing two resolved types targeting a single equivalence
  /// class, in terms of their relative impact on solving the system.
  enum class ResolvedTypeComparisonResult {
    /// The first resolved type is a better choice than the second one.
    Better,

    /// The first resolved type is an equivalent or worse choice than the
    /// second one.
    EquivalentOrWorse,

    /// Both resolved types are concrete and mutually exclusive.
    Ambiguity
  };

  /// Compare the given resolved types as targeting a single equivalence class,
  /// in terms of the their relative impact on solving the system.
  static ResolvedTypeComparisonResult compareResolvedTypes(Type ty1, Type ty2);
};

/// Captures the state needed to infer associated types.
class AssociatedTypeInference {
  /// The type checker we'll need to validate declarations etc.
  ASTContext &ctx;

  /// The conformance for which we are inferring associated types.
  NormalProtocolConformance *conformance;

  /// The protocol for which we are inferring associated types.
  ProtocolDecl *proto;

  /// The declaration context in which conformance to the protocol is
  /// declared.
  DeclContext *dc;

  /// The type that is adopting the protocol.
  Type adoptee;

  /// The set of type witnesses inferred from value witnesses.
  InferredAssociatedTypes inferred;

  /// Hash table containing the type witnesses that we've inferred for
  /// each associated type, as well as an indication of how we inferred them.
  llvm::ScopedHashTable<AssociatedTypeDecl *, std::pair<Type, unsigned>>
    typeWitnesses;

  /// Information about a failed, defaulted associated type.
  const AssociatedTypeDecl *failedDefaultedAssocType = nullptr;
  Type failedDefaultedWitness;
  CheckTypeWitnessResult failedDefaultedResult = CheckTypeWitnessResult::forSuccess();

  /// Information about a failed, derived associated type.
  AssociatedTypeDecl *failedDerivedAssocType = nullptr;
  Type failedDerivedWitness;

  // Which type witness was missing?
  AssociatedTypeDecl *missingTypeWitness = nullptr;

  // Was there a conflict in type witness deduction?
  llvm::Optional<TypeWitnessConflict> typeWitnessConflict;
  unsigned numTypeWitnessesBeforeConflict = 0;

public:
  AssociatedTypeInference(ASTContext &ctx,
                          NormalProtocolConformance *conformance);

private:
  /// Retrieve the AST context.
  ASTContext &getASTContext() const { return ctx; }

  /// Infer associated type witnesses for the given tentative
  /// requirement/witness match.
  InferredAssociatedTypesByWitness inferTypeWitnessesViaValueWitness(
                                     ValueDecl *req,
                                     ValueDecl *witness);

  /// Infer associated type witnesses for the given value requirement.
  InferredAssociatedTypesByWitnesses inferTypeWitnessesViaValueWitnesses(
                   const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                   ValueDecl *req);

  /// Infer associated type witnesses for the given associated type.
  InferredAssociatedTypesByWitnesses inferTypeWitnessesViaAssociatedType(
                   const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                   AssociatedTypeDecl *assocType);

  /// Infer associated type witnesses for all relevant value requirements.
  ///
  /// \param assocTypes The set of associated types we're interested in.
  InferredAssociatedTypes
  inferTypeWitnessesViaValueWitnesses(
    const llvm::SetVector<AssociatedTypeDecl *> &assocTypes);

  /// Compute a "fixed" type witness for an associated type, e.g.,
  /// if the refined protocol requires it to be equivalent to some other type.
  Type computeFixedTypeWitness(AssociatedTypeDecl *assocType);

  /// Compute the default type witness from an associated type default,
  /// if there is one.
  llvm::Optional<AbstractTypeWitness>
  computeDefaultTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Compute the "derived" type witness for an associated type that is
  /// known to the compiler.
  std::pair<Type, TypeDecl *>
  computeDerivedTypeWitness(AssociatedTypeDecl *assocType);

  /// Compute a type witness without using a specific potential witness.
  llvm::Optional<AbstractTypeWitness>
  computeAbstractTypeWitness(AssociatedTypeDecl *assocType);

  /// Collect abstract type witnesses and feed them to the given system.
  void collectAbstractTypeWitnesses(
      TypeWitnessSystem &system,
      ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes) const;

  /// Substitute the current type witnesses into the given interface type.
  Type substCurrentTypeWitnesses(Type type);

  /// Retrieve substitution options with a tentative type witness
  /// operation that queries the current set of type witnesses.
  SubstOptions getSubstOptionsWithCurrentTypeWitnesses();

  /// Check whether the current set of type witnesses meets the
  /// requirements of the protocol.
  bool checkCurrentTypeWitnesses(
         const SmallVectorImpl<std::pair<ValueDecl *, ValueDecl *>>
           &valueWitnesses);

  /// Check the current type witnesses against the
  /// requirements of the given constrained extension.
  bool checkConstrainedExtension(ExtensionDecl *ext);

  /// Attempt to infer abstract type witnesses for the given set of associated
  /// types.
  ///
  /// \returns \c nullptr, or the associated type that failed.
  AssociatedTypeDecl *inferAbstractTypeWitnesses(
      ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes, unsigned reqDepth);

  /// Top-level operation to find solutions for the given unresolved
  /// associated types.
  void findSolutions(
                 ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                 SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// Explore the solution space to find both viable and non-viable solutions.
  void findSolutionsRec(
         ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
         SmallVectorImpl<InferredTypeWitnessesSolution> &solutions,
         SmallVectorImpl<InferredTypeWitnessesSolution> &nonViableSolutions,
         SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> &valueWitnesses,
         unsigned numTypeWitnesses,
         unsigned numValueWitnessesInProtocolExtensions,
         unsigned reqDepth);

  /// Determine whether the first solution is better than the second
  /// solution.
  bool isBetterSolution(const InferredTypeWitnessesSolution &first,
                        const InferredTypeWitnessesSolution &second);

  /// Find the best solution.
  ///
  /// \param solutions All of the solutions to consider. On success,
  /// this will contain only the best solution.
  ///
  /// \returns \c false if there was a single best solution,
  /// \c true if no single best solution exists.
  bool findBestSolution(
                SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// Emit a diagnostic for the case where there are no solutions at all
  /// to consider.
  ///
  /// \returns true if a diagnostic was emitted, false otherwise.
  bool diagnoseNoSolutions(
                     ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes);

  /// Emit a diagnostic when there are multiple solutions.
  ///
  /// \returns true if a diagnostic was emitted, false otherwise.
  bool diagnoseAmbiguousSolutions(
                ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                SmallVectorImpl<InferredTypeWitnessesSolution> &solutions);

  /// We may need to determine a type witness, regardless of the existence of a
  /// default value for it, e.g. when a 'distributed actor' is looking up its
  /// 'ID', the default defined in an extension for 'Identifiable' would be
  /// located using the lookup resolve. This would not be correct, since the
  /// type actually must be based on the associated 'ActorSystem'.
  ///
  /// TODO(distributed): perhaps there is a better way to avoid this mixup?
  ///   Note though that this issue seems to only manifest in "real" builds
  ///   involving multiple files/modules, and not in tests within the Swift
  ///   project itself.
  bool canAttemptEagerTypeWitnessDerivation(
      DeclContext *DC, AssociatedTypeDecl *assocType);

public:
  /// Describes a mapping from associated type declarations to their
  /// type witnesses (as interface types).
  using InferredTypeWitnesses =
      std::vector<std::pair<AssociatedTypeDecl *, Type>>;

  /// Perform associated type inference.
  ///
  /// \returns \c true if an error occurred, \c false otherwise
  llvm::Optional<InferredTypeWitnesses> solve();
};

}

AssociatedTypeInference::AssociatedTypeInference(
    ASTContext &ctx, NormalProtocolConformance *conformance)
    : ctx(ctx), conformance(conformance), proto(conformance->getProtocol()),
      dc(conformance->getDeclContext()), adoptee(conformance->getType()) {}

static bool associatedTypesAreSameEquivalenceClass(AssociatedTypeDecl *a,
                                                   AssociatedTypeDecl *b) {
  if (a == b)
    return true;

  // TODO: Do a proper equivalence check here by looking for some relationship
  // between a and b's protocols. In practice today, it's unlikely that
  // two same-named associated types can currently be independent, since we
  // don't have anything like `@implements(P.foo)` to rename witnesses (and
  // we still fall back to name lookup for witnesses in more cases than we
  // should).
  if (a->getName() == b->getName())
    return true;

  return false;
}

namespace {

/// Try to avoid situations where resolving the type of a witness calls back
/// into associated type inference.
struct TypeReprCycleCheckWalker : ASTWalker {
  ASTContext &ctx;
  llvm::SmallDenseSet<Identifier, 2> circularNames;
  ValueDecl *witness;
  bool found;

  TypeReprCycleCheckWalker(
      ASTContext &ctx,
      const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved)
    : ctx(ctx), witness(nullptr), found(false) {
    for (auto *assocType : allUnresolved) {
      circularNames.insert(assocType->getName());
    }
  }

  PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
    // FIXME: Visit generic arguments.

    if (auto *identTyR = dyn_cast<SimpleIdentTypeRepr>(T)) {
      // If we're inferring `Foo`, don't look at a witness mentioning `Foo`.
      if (circularNames.count(identTyR->getNameRef().getBaseIdentifier()) > 0) {
        // If unqualified lookup can find a type with this name without looking
        // into protocol members, don't skip the witness, since this type might
        // be a candidate witness.
        auto desc = UnqualifiedLookupDescriptor(
            identTyR->getNameRef(), witness->getDeclContext(),
            identTyR->getLoc(), UnqualifiedLookupOptions());

        auto results =
            evaluateOrDefault(ctx.evaluator, UnqualifiedLookupRequest{desc}, {});

        // Ok, resolving this name would trigger associated type inference
        // recursively. We're going to skip this witness.
        if (results.allResults().empty()) {
          found = true;
          return Action::Stop();
        }
      }
    }

    if (auto *memberTyR = dyn_cast<MemberTypeRepr>(T)) {
      // If we're looking at a member type`Foo.Bar`, check `Foo` recursively.
      auto *baseTyR = memberTyR->getBaseComponent();
      baseTyR->walk(*this);

      // If we're inferring `Foo`, don't look at a witness mentioning `Self.Foo`.
      if (auto *identTyR = dyn_cast<SimpleIdentTypeRepr>(baseTyR)) {
        if (identTyR->getNameRef().getBaseIdentifier() == ctx.Id_Self &&
            circularNames.count(memberTyR->getNameRef().getBaseIdentifier()) > 0) {
          // But if qualified lookup can find a type with this name without
          // looking into protocol members, don't skip the witness, since this
          // type might be a candidate witness.
          SmallVector<ValueDecl *, 2> results;
          witness->getInnermostDeclContext()->lookupQualified(
              witness->getDeclContext()->getSelfTypeInContext(),
              identTyR->getNameRef(), SourceLoc(), NLOptions(), results);

          // Ok, resolving this member type would trigger associated type
          // inference recursively. We're going to skip this witness.
          if (results.empty()) {
            found = true;
            return Action::Stop();
          }
        }
      }

      return Action::SkipChildren();
    }

    return Action::Continue();
  }

  bool checkForPotentialCycle(ValueDecl *witness) {
    // Don't do this for protocol extension members, because we have a
    // mini "solver" that avoids similar issues instead.
    if (witness->getDeclContext()->getSelfProtocolDecl() != nullptr)
      return false;

    // If we already have an interface type, don't bother trying to
    // avoid a cycle.
    if (witness->hasInterfaceType())
      return false;

    // We call checkForPotentailCycle() multiple times with
    // different witnesses.
    found = false;
    this->witness = witness;

    auto walkInto = [&](TypeRepr *tyR) {
      if (tyR)
        tyR->walk(*this);
      return found;
    };

    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(witness)) {
      for (auto *param : *AFD->getParameters()) {
        if (walkInto(param->getTypeRepr()))
          return true;
      }

      if (auto *FD = dyn_cast<FuncDecl>(witness)) {
        if (walkInto(FD->getResultTypeRepr()))
          return true;
      }

      return false;
    }

    if (auto *SD = dyn_cast<SubscriptDecl>(witness)) {
      for (auto *param : *SD->getIndices()) {
        if (walkInto(param->getTypeRepr()))
          return true;
      }

      if (walkInto(SD->getElementTypeRepr()))
        return true;

      return false;
    }

    if (auto *VD = dyn_cast<VarDecl>(witness)) {
      if (walkInto(VD->getTypeReprOrParentPatternTypeRepr()))
        return true;

      return false;
    }

    if (auto *EED = dyn_cast<EnumElementDecl>(witness)) {
      for (auto *param : *EED->getParameterList()) {
        if (walkInto(param->getTypeRepr()))
          return true;
      }

      return false;
    }

    assert(false && "Should be exhaustive");
    return false;
  }
};

}

static bool isExtensionUsableForInference(const ExtensionDecl *extension,
                                          NormalProtocolConformance *conformance) {
  // The context the conformance being checked is declared on.
  const auto conformanceDC = conformance->getDeclContext();
  if (extension == conformanceDC)
    return true;

  // Invalid case.
  const auto extendedNominal = extension->getExtendedNominal();
  if (extendedNominal == nullptr)
    return true;

  auto *proto = dyn_cast<ProtocolDecl>(extendedNominal);

  // If the extension is bound to the nominal the conformance is
  // declared on, it is viable for inference when its conditional
  // requirements are satisfied by those of the conformance context.
  if (!proto) {
    // Retrieve the generic signature of the extension.
    const auto extensionSig = extension->getGenericSignature();
    return extensionSig
        .requirementsNotSatisfiedBy(
            conformanceDC->getGenericSignatureOfContext())
        .empty();
  }

  // The condition here is a bit more fickle than
  // `isExtensionApplied`. That check would prematurely reject
  // extensions like `P where AssocType == T` if we're relying on a
  // default implementation inside the extension to infer `AssocType == T`
  // in the first place. Only check conformances on the `Self` type,
  // because those have to be explicitly declared on the type somewhere
  // so won't be affected by whatever answer inference comes up with.
  auto *module = conformanceDC->getParentModule();
  auto checkConformance = [&](ProtocolDecl *proto) {
    auto typeInContext = conformanceDC->mapTypeIntoContext(conformance->getType());
    auto otherConf = module->checkConformance(typeInContext, proto);
    return !otherConf.isInvalid();
  };

  // First check the extended protocol itself.
  if (!checkConformance(proto))
    return false;

  // Source file and module file have different ways to get self bounds.
  // Source file extension will have trailing where clause which can avoid
  // computing a generic signature. Module file will not have
  // trailing where clause, so it will compute generic signature to get
  // self bounds which might result in slow performance.
  SelfBounds bounds;
  if (extension->getParentSourceFile() != nullptr)
    bounds = getSelfBoundsFromWhereClause(extension);
  else
    bounds = getSelfBoundsFromGenericSignature(extension);
  for (auto *decl : bounds.decls) {
    if (auto *proto = dyn_cast<ProtocolDecl>(decl)) {
      if (!checkConformance(proto))
        return false;
    }
  }

  return true;
}

InferredAssociatedTypesByWitnesses
AssociatedTypeInference::inferTypeWitnessesViaValueWitnesses(
                    const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                    ValueDecl *req) {
  // Conformances constructed by the ClangImporter should have explicit type
  // witnesses already.
  if (isa<ClangModuleUnit>(conformance->getDeclContext()->getModuleScopeContext())) {
    llvm::errs() << "Cannot infer associated types for imported conformance:\n";
    conformance->getType().dump(llvm::errs());
    for (auto assocTypeDecl : allUnresolved)
      assocTypeDecl->dump(llvm::errs());
    abort();
  }

  TypeReprCycleCheckWalker cycleCheck(dc->getASTContext(), allUnresolved);

  InferredAssociatedTypesByWitnesses result;

  for (auto witness :
       lookupValueWitnesses(dc, req, /*ignoringNames=*/nullptr)) {
    LLVM_DEBUG(llvm::dbgs() << "Inferring associated types from decl:\n";
               witness->dump(llvm::dbgs()));

    // If the potential witness came from an extension, and our `Self`
    // type can't use it regardless of what associated types we end up
    // inferring, skip the witness.
    if (auto extension = dyn_cast<ExtensionDecl>(witness->getDeclContext())) {
      if (!isExtensionUsableForInference(extension, conformance)) {
        LLVM_DEBUG(llvm::dbgs() << "Extension not usable for inference\n");
        continue;
      }
    }

    if (cycleCheck.checkForPotentialCycle(witness)) {
      LLVM_DEBUG(llvm::dbgs() << "Skipping witness to avoid request cycle\n");
      continue;
    }

    // Try to resolve the type witness via this value witness.
    auto witnessResult = inferTypeWitnessesViaValueWitness(req, witness);

    // Filter out duplicated inferred types as well as inferred types
    // that don't meet the requirements placed on the associated type.
    llvm::DenseSet<std::pair<AssociatedTypeDecl *, CanType>> known;
    for (unsigned i = 0; i < witnessResult.Inferred.size(); /*nothing*/) {
#define REJECT {\
  witnessResult.Inferred.erase(witnessResult.Inferred.begin() + i); \
  continue; \
}
      auto &result = witnessResult.Inferred[i];

      LLVM_DEBUG(llvm::dbgs() << "Considering whether "
                              << result.first->getName()
                              << " can infer to:\n";
                 result.second->dump(llvm::dbgs()));

      // Filter out errors.
      if (result.second->hasError()) {
        LLVM_DEBUG(llvm::dbgs() << "-- has error type\n");
        REJECT;
      }

      // Filter out duplicates.
      if (!known.insert({result.first, result.second->getCanonicalType()})
                .second) {
        LLVM_DEBUG(llvm::dbgs() << "-- duplicate\n");
        REJECT;
      }

      // Filter out circular possibilities, e.g. that
      // AssocType == S.AssocType or
      // AssocType == Foo<S.AssocType>.
      bool canInferFromOtherAssociatedType = false;
      bool containsTautologicalType =
        result.second.findIf([&](Type t) -> bool {
          auto dmt = t->getAs<DependentMemberType>();
          if (!dmt)
            return false;
          if (!associatedTypesAreSameEquivalenceClass(dmt->getAssocType(),
                                                      result.first))
            return false;

          auto typeInContext =
            conformance->getDeclContext()->mapTypeIntoContext(conformance->getType());

          if (!dmt->getBase()->isEqual(typeInContext))
            return false;

          // If this associated type is same-typed to another associated type
          // on `Self`, then it may still be an interesting candidate if we find
          // an answer for that other type.
          auto witnessContext = witness->getDeclContext();
          if (witnessContext->getExtendedProtocolDecl()
              && witnessContext->getGenericSignatureOfContext()) {
            auto selfTy = witnessContext->getSelfInterfaceType();
            auto selfAssocTy = DependentMemberType::get(selfTy,
                                                        dmt->getAssocType());
            for (auto &reqt : witnessContext->getGenericSignatureOfContext()
                                            .getRequirements()) {
              switch (reqt.getKind()) {
              case RequirementKind::SameShape:
                llvm_unreachable("Same-shape requirement not supported here");

              case RequirementKind::Conformance:
              case RequirementKind::Superclass:
              case RequirementKind::Layout:
                break;

              case RequirementKind::SameType:
                Type other;
                if (reqt.getFirstType()->isEqual(selfAssocTy)) {
                  other = reqt.getSecondType();
                } else if (reqt.getSecondType()->isEqual(selfAssocTy)) {
                  other = reqt.getFirstType();
                } else {
                  break;
                }

                if (auto otherAssoc = other->getAs<DependentMemberType>()) {
                  if (otherAssoc->getBase()->isEqual(selfTy)) {
                    auto otherDMT = DependentMemberType::get(dmt->getBase(),
                                                    otherAssoc->getAssocType());

                    // We may be able to infer one associated type from the
                    // other.
                    result.second = result.second.transform([&](Type t) -> Type{
                      if (t->isEqual(dmt))
                        return otherDMT;
                      return t;
                    });
                    canInferFromOtherAssociatedType = true;
                    LLVM_DEBUG(llvm::dbgs() << "++ we can same-type to:\n";
                               result.second->dump(llvm::dbgs()));
                    return false;
                  }
                }
                break;
              }
            }
          }

          return true;
        });

      if (containsTautologicalType) {
        LLVM_DEBUG(llvm::dbgs() << "-- tautological\n");
        REJECT;
      }

      // Check that the type witness doesn't contradict an
      // explicitly-given type witness. If it does contradict, throw out the
      // witness completely.
      if (!allUnresolved.count(result.first)) {
        auto existingWitness =
          conformance->getTypeWitness(result.first);
        existingWitness = dc->mapTypeIntoContext(existingWitness);

        // If the deduced type contains an irreducible
        // DependentMemberType, that indicates a dependency
        // on another associated type we haven't deduced,
        // so we can't tell whether there's a contradiction
        // yet.
        auto newWitness = result.second->getCanonicalType();
        if (!newWitness->hasTypeParameter() &&
            !newWitness->hasDependentMember() &&
            !existingWitness->isEqual(newWitness)) {
          LLVM_DEBUG(llvm::dbgs() << "** contradicts explicit type witness, "
                                     "rejecting inference from this decl\n");
          goto next_witness;
        }
      }

      // If we same-typed to another unresolved associated type, we won't
      // be able to check conformances yet.
      if (!canInferFromOtherAssociatedType) {
        // Check that the type witness meets the
        // requirements on the associated type.
        if (auto failed =
                checkTypeWitness(result.second, result.first, conformance,
                                 llvm::None)) {
          witnessResult.NonViable.push_back(
                          std::make_tuple(result.first,result.second,failed));
          LLVM_DEBUG(llvm::dbgs() << "-- doesn't fulfill requirements\n");
          REJECT;
        }
      }

      LLVM_DEBUG(llvm::dbgs() << "++ seems legit\n");
      ++i;
    }
#undef REJECT

    // If no inferred types remain, skip this witness.
    if (witnessResult.Inferred.empty() && witnessResult.NonViable.empty())
      continue;

    // If there were any non-viable inferred associated types, don't
    // infer anything from this witness.
    if (!witnessResult.NonViable.empty())
      witnessResult.Inferred.clear();

    result.push_back(std::move(witnessResult));
next_witness:;
}

  return result;
}

InferredAssociatedTypes
AssociatedTypeInference::inferTypeWitnessesViaValueWitnesses(
  const llvm::SetVector<AssociatedTypeDecl *> &assocTypes) {
  InferredAssociatedTypes result;
  for (auto member : proto->getMembers()) {
    auto req = dyn_cast<ValueDecl>(member);
    if (!req || !req->isProtocolRequirement())
      continue;

    // Infer type witnesses for associated types.
    if (auto assocType = dyn_cast<AssociatedTypeDecl>(req)) {
      // If this is not one of the associated types we are trying to infer,
      // just continue.
      if (assocTypes.count(assocType) == 0)
        continue;

      auto reqInferred = inferTypeWitnessesViaAssociatedType(assocTypes,
                                                             assocType);
      if (!reqInferred.empty())
        result.push_back({req, std::move(reqInferred)});

      continue;
    }

    // Skip operator requirements, because they match globally and
    // therefore tend to cause deduction mismatches.
    // FIXME: If we had some basic sanity checking of Self, we might be able to
    // use these.
    if (auto func = dyn_cast<FuncDecl>(req)) {
      if (func->isOperator() || isa<AccessorDecl>(func))
        continue;
    }

    // Validate the requirement.
    if (req->isInvalid())
      continue;

    // Check whether any of the associated types we care about are
    // referenced in this value requirement.
    {
      auto referenced = evaluateOrDefault(ctx.evaluator,
                                          ReferencedAssociatedTypesRequest{req},
                                          TinyPtrVector<AssociatedTypeDecl *>());
      if (llvm::find_if(referenced, [&](AssociatedTypeDecl *const assocType) {
                          return assocTypes.count(assocType);
                        }) == referenced.end())
        continue;
    }

    // Infer associated types from the potential value witnesses for
    // this requirement.
    auto reqInferred =
      inferTypeWitnessesViaValueWitnesses(assocTypes, req);
    if (reqInferred.empty())
      continue;

    result.push_back({req, std::move(reqInferred)});
  }

  return result;
}

/// Map error types back to their original types.
static Type mapErrorTypeToOriginal(Type type) {
  if (auto errorType = type->getAs<ErrorType>()) {
    if (auto originalType = errorType->getOriginalType())
      return originalType.transform(mapErrorTypeToOriginal);
  }

  return type;
}

/// Produce the type when matching a witness.
static Type getWitnessTypeForMatching(NormalProtocolConformance *conformance,
                                      ValueDecl *witness) {
  if (witness->isRecursiveValidation())
    return Type();

  if (witness->isInvalid())
    return Type();

  if (!witness->getDeclContext()->isTypeContext()) {
    // FIXME: Could we infer from 'Self' to make these work?
    return witness->getInterfaceType();
  }

  // Retrieve the set of substitutions to be applied to the witness.
  Type model =
    conformance->getDeclContext()->mapTypeIntoContext(conformance->getType());
  TypeSubstitutionMap substitutions = model->getMemberSubstitutions(witness);
  Type type = witness->getInterfaceType()->getReferenceStorageReferent();

  if (substitutions.empty())
    return type;

  // Strip off the requirements of a generic function type.
  // FIXME: This doesn't actually break recursion when substitution
  // looks for an inferred type witness, but it makes it far less
  // common, because most of the recursion involves the requirements
  // of the generic type.
  if (auto genericFn = type->getAs<GenericFunctionType>()) {
    type = FunctionType::get(genericFn->getParams(),
                             genericFn->getResult(),
                             genericFn->getExtInfo());
  }

  auto &ctx = conformance->getDeclContext()->getASTContext();

  // Get the reduced type of the witness. This rules our certain tautological
  // inferences below.
  if (ctx.LangOpts.EnableExperimentalAssociatedTypeInference) {
    if (auto genericSig = witness->getInnermostDeclContext()
            ->getGenericSignatureOfContext()) {
      type = genericSig.getReducedType(type);
      type = genericSig->getSugaredType(type);
    }
  }

  // Remap associated types that reference other protocols into this
  // protocol.
  auto proto = conformance->getProtocol();
  type = type.transformRec([proto](TypeBase *type) -> llvm::Optional<Type> {
    if (auto depMemTy = dyn_cast<DependentMemberType>(type)) {
      if (depMemTy->getAssocType() &&
          depMemTy->getAssocType()->getProtocol() != proto) {
        if (auto *assocType = proto->getAssociatedType(depMemTy->getName())) {
          auto origProto = depMemTy->getAssocType()->getProtocol();
          if (proto->inheritsFrom(origProto))
            return Type(DependentMemberType::get(depMemTy->getBase(),
                                                 assocType));
        }
      }
    }

    return llvm::None;
  });

  ModuleDecl *module = conformance->getDeclContext()->getParentModule();
  auto resultType = type.subst(QueryTypeSubstitutionMap{substitutions},
                               LookUpConformanceInModule(module));
  if (!resultType->hasError()) return resultType;

  // Map error types with original types *back* to the original, dependent type.
  return resultType.transform(mapErrorTypeToOriginal);
}

/// Remove the 'self' type from the given type, if it's a method type.
static Type removeSelfParam(ValueDecl *value, Type type) {
  if (value->hasCurriedSelf()) {
    return type->castTo<AnyFunctionType>()->getResult();
  }

  return type;
}

InferredAssociatedTypesByWitnesses
AssociatedTypeInference::inferTypeWitnessesViaAssociatedType(
                   const llvm::SetVector<AssociatedTypeDecl *> &allUnresolved,
                   AssociatedTypeDecl *assocType) {
  // Form the default name _Default_Foo.
  DeclNameRef defaultName;
  {
    SmallString<32> defaultNameStr;
    {
      llvm::raw_svector_ostream out(defaultNameStr);
      out << "_Default_";
      out << assocType->getName().str();
    }

    defaultName = DeclNameRef(getASTContext().getIdentifier(defaultNameStr));
  }

  NLOptions subOptions = (NL_QualifiedDefault |
                          NL_OnlyTypes |
                          NL_ProtocolMembers);

  // Look for types with the given default name that have appropriate
  // @_implements attributes.
  SmallVector<ValueDecl *, 4> lookupResults;
  dc->lookupQualified(dc->getSelfNominalTypeDecl(), defaultName,
                      isa<ExtensionDecl>(dc)
                      ? cast<ExtensionDecl>(dc)->getStartLoc()
                      : cast<NominalTypeDecl>(dc)->getStartLoc(),
                      subOptions, lookupResults);

  InferredAssociatedTypesByWitnesses result;

  for (auto decl : lookupResults) {
    // We want type declarations.
    auto typeDecl = dyn_cast<TypeDecl>(decl);
    if (!typeDecl || isa<AssociatedTypeDecl>(typeDecl))
      continue;

    // We only find these within a protocol extension.
    auto defaultProto = typeDecl->getDeclContext()->getSelfProtocolDecl();
    if (!defaultProto)
      continue;

    // Determine the witness type.
    Type witnessType = getWitnessTypeForMatching(conformance, typeDecl);
    if (!witnessType) continue;

    if (auto witnessMetaType = witnessType->getAs<AnyMetatypeType>())
      witnessType = witnessMetaType->getInstanceType();
    else
      continue;

    // Add this result.
    InferredAssociatedTypesByWitness inferred;
    inferred.Witness = typeDecl;
    inferred.Inferred.push_back({assocType, witnessType});
    result.push_back(std::move(inferred));
  }

  return result;
}

Type swift::adjustInferredAssociatedType(TypeAdjustment adjustment, Type type,
                                         bool &performed) {
  // If we have an optional type, adjust its wrapped type.
  if (auto optionalObjectType = type->getOptionalObjectType()) {
    auto newOptionalObjectType =
      adjustInferredAssociatedType(adjustment, optionalObjectType, performed);
    if (newOptionalObjectType.getPointer() == optionalObjectType.getPointer())
      return type;

    return OptionalType::get(newOptionalObjectType);
  }

  auto needsAdjustment = [=](FunctionType *funcType) -> bool {
    if (adjustment == TypeAdjustment::NoescapeToEscaping)
      return funcType->isNoEscape();
    else
      return !funcType->isSendable();
  };
  auto adjust = [=](const ASTExtInfo &info) -> ASTExtInfo {
    if (adjustment == TypeAdjustment::NoescapeToEscaping)
      return info.withNoEscape(false);
    else
      return info.withConcurrent(true);
  };

  // If we have a noescape function type, make it escaping.
  if (auto funcType = type->getAs<FunctionType>()) {
    performed = needsAdjustment(funcType);
    if (performed)
      return FunctionType::get(funcType->getParams(), funcType->getResult(),
                               adjust(funcType->getExtInfo()));
  }
  return type;
}

static AssociatedTypeDecl *
getReferencedAssocTypeOfProtocol(Type type, ProtocolDecl *proto) {
  if (auto dependentMember = type->getAs<DependentMemberType>()) {
    if (auto assocType = dependentMember->getAssocType()) {
      if (dependentMember->getBase()->isEqual(proto->getSelfInterfaceType())) {
        // Exact match: this is our associated type.
        if (assocType->getProtocol() == proto)
          return assocType;

        // Check whether there is an associated type of the same name in
        // this protocol.
        if (auto *found = proto->getAssociatedType(assocType->getName()))
          return found;
      }
    }
  }

  return nullptr;
}

/// Attempt to resolve a type witness via a specific value witness.
InferredAssociatedTypesByWitness
AssociatedTypeInference::inferTypeWitnessesViaValueWitness(ValueDecl *req,
                                                           ValueDecl *witness) {
  InferredAssociatedTypesByWitness inferred;
  inferred.Witness = witness;

  // Compute the requirement and witness types we'll use for matching.
  Type fullWitnessType = getWitnessTypeForMatching(conformance, witness);
  if (!fullWitnessType) {
    return inferred;
  }

  auto setup =
      [&]() -> std::tuple<llvm::Optional<RequirementMatch>, Type, Type> {
    fullWitnessType = removeSelfParam(witness, fullWitnessType);
    return std::make_tuple(llvm::None,
                           removeSelfParam(req, req->getInterfaceType()),
                           fullWitnessType);
  };

  /// Visits a requirement type to match it to a potential witness for
  /// the purpose of deducing associated types.
  ///
  /// The visitor argument is the witness type. If there are any
  /// obvious conflicts between the structure of the two types,
  /// returns true. The conflict checking is fairly conservative, only
  /// considering rough structure.
  class MatchVisitor : public TypeMatcher<MatchVisitor> {
    NormalProtocolConformance *Conformance;
    InferredAssociatedTypesByWitness &Inferred;

  public:
    MatchVisitor(NormalProtocolConformance *conformance,
                 InferredAssociatedTypesByWitness &inferred)
      : Conformance(conformance), Inferred(inferred) { }

    /// Structural mismatches imply that the witness cannot match.
    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      // If either type hit an error, don't stop yet.
      if (firstType->hasError() || secondType->hasError())
        return true;

      // FIXME: Check whether one of the types is dependent?
      return false;
    }

    /// Deduce associated types from dependent member types in the witness.
    bool mismatch(DependentMemberType *firstDepMember,
                  TypeBase *secondType, Type sugaredFirstType) {
      // If the second type is an error, don't look at it further.
      if (secondType->hasError())
        return true;

      // Adjust the type to a type that can be written explicitly.
      bool noescapeToEscaping = false;
      Type inferredType =
        adjustInferredAssociatedType(TypeAdjustment::NoescapeToEscaping,
                                     secondType, noescapeToEscaping);
      if (!inferredType->isMaterializable())
        return true;

      // If the type contains a type parameter, there is nothing we can infer
      // from it.
      // FIXME: This is a weird state introduced by associated type inference
      // that should not exist.
      if (inferredType->hasTypeParameter())
        return true;

      auto proto = Conformance->getProtocol();
      if (auto assocType = getReferencedAssocTypeOfProtocol(firstDepMember,
                                                            proto)) {
        Inferred.Inferred.push_back({assocType, inferredType});
      }

      // Always allow mismatches here.
      return true;
    }

    /// FIXME: Recheck the type of Self against the second type?
    bool mismatch(GenericTypeParamType *selfParamType,
                  TypeBase *secondType, Type sugaredFirstType) {
      return true;
    }
  };

  // Match a requirement and witness type.
  MatchVisitor matchVisitor(conformance, inferred);
  auto matchTypes = [&](Type reqType,
                        Type witnessType) -> llvm::Optional<RequirementMatch> {
    if (!matchVisitor.match(reqType, witnessType)) {
      return RequirementMatch(witness, MatchKind::TypeConflict,
                              fullWitnessType);
    }

    return llvm::None;
  };

  // Finalization of the checking is pretty trivial; just bundle up a
  // result we can look at.
  auto finalize = [&](bool anyRenaming, ArrayRef<OptionalAdjustment>)
                    -> RequirementMatch {
    return RequirementMatch(witness,
                            anyRenaming ? MatchKind::RenamedMatch
                                        : MatchKind::ExactMatch,
                            fullWitnessType);

  };

  // Match the witness. If we don't succeed, throw away the inference
  // information.
  // FIXME: A renamed match might be useful to retain for the failure case.
  if (!matchWitness(dc, req, witness, setup, matchTypes, finalize)
          .isWellFormed()) {
    inferred.Inferred.clear();
  }

  return inferred;
}

AssociatedTypeDecl *swift::findDefaultedAssociatedType(
                                             DeclContext *dc,
                                             NominalTypeDecl *adoptee,
                                             AssociatedTypeDecl *assocType) {
  // If this associated type has a default, we're done.
  if (assocType->hasDefaultDefinitionType())
    return assocType;

  // Otherwise, look for all associated types with the same name along all the
  // protocols that the adoptee conforms to.
  SmallVector<ValueDecl *, 4> decls;
  auto options = NL_ProtocolMembers | NL_OnlyTypes;
  dc->lookupQualified(adoptee, DeclNameRef(assocType->getName()),
                      SourceLoc(), options, decls);

  SmallPtrSet<CanType, 4> canonicalTypes;
  SmallVector<AssociatedTypeDecl *, 2> results;
  for (auto *decl : decls) {
    if (auto *assocDecl = dyn_cast<AssociatedTypeDecl>(decl)) {
      auto defaultType = assocDecl->getDefaultDefinitionType();
      if (!defaultType) continue;

      CanType key = defaultType->getCanonicalType();
    if (canonicalTypes.insert(key).second)
      results.push_back(assocDecl);
    }
  }

  // If there was a single result, return it.
  // FIXME: We could find *all* of the non-covered, defaulted associated types.
  return results.size() == 1 ? results.front() : nullptr;
}

Type AssociatedTypeInference::computeFixedTypeWitness(
                                            AssociatedTypeDecl *assocType) {
  Type resultType;

  // Look at all of the inherited protocols to determine whether they
  // require a fixed type for this associated type.
  for (auto conformedProto : dc->getSelfNominalTypeDecl()->getAllProtocols()) {
    if (conformedProto != assocType->getProtocol() &&
        !conformedProto->inheritsFrom(assocType->getProtocol()))
      continue;

    auto sig = conformedProto->getGenericSignature();

    // FIXME: The RequirementMachine will assert on re-entrant construction.
    // We should find a more principled way of breaking this cycle.
    if (ctx.isRecursivelyConstructingRequirementMachine(sig.getCanonicalSignature()) ||
        ctx.isRecursivelyConstructingRequirementMachine(conformedProto) ||
        conformedProto->isComputingRequirementSignature())
      continue;

    auto selfTy = conformedProto->getSelfInterfaceType();
    if (!sig->requiresProtocol(selfTy, assocType->getProtocol()))
      continue;

    auto structuralTy = DependentMemberType::get(selfTy, assocType->getName());
    const auto ty = sig.getReducedType(structuralTy);

    // A dependent member type with an identical base and name indicates that
    // the protocol does not same-type constrain it in any way; move on to
    // the next protocol.
    if (auto *const memberTy = ty->getAs<DependentMemberType>()) {
      if (memberTy->getBase()->isEqual(selfTy) &&
          memberTy->getName() == assocType->getName())
        continue;
    }

    if (!resultType) {
      resultType = ty;
      continue;
    }

    // FIXME: Bailing out on ambiguity.
    if (!resultType->isEqual(ty))
      return Type();
  }

  return resultType;
}

llvm::Optional<AbstractTypeWitness>
AssociatedTypeInference::computeDefaultTypeWitness(
    AssociatedTypeDecl *assocType) const {
  // Go find a default definition.
  auto *const defaultedAssocType = findDefaultedAssociatedType(
      dc, dc->getSelfNominalTypeDecl(), assocType);
  if (!defaultedAssocType)
    return llvm::None;

  const Type defaultType = defaultedAssocType->getDefaultDefinitionType();
  // FIXME: Circularity
  if (!defaultType)
    return llvm::None;

  if (defaultType->hasError())
    return llvm::None;

  return AbstractTypeWitness(assocType, defaultType, defaultedAssocType);
}

static std::pair<Type, TypeDecl *>
deriveTypeWitness(DeclContext *DC,
                  NominalTypeDecl *TypeDecl,
                  AssociatedTypeDecl *AssocType) {
  auto *protocol = cast<ProtocolDecl>(AssocType->getDeclContext());

  auto knownKind = protocol->getKnownProtocolKind();
  
  if (!knownKind)
    return std::make_pair(nullptr, nullptr);

  auto Decl = DC->getInnermostDeclarationDeclContext();

  DerivedConformance derived(TypeDecl->getASTContext(), Decl, TypeDecl,
                             protocol);
  switch (*knownKind) {
  case KnownProtocolKind::RawRepresentable:
    return std::make_pair(derived.deriveRawRepresentable(AssocType), nullptr);
  case KnownProtocolKind::CaseIterable:
    return std::make_pair(derived.deriveCaseIterable(AssocType), nullptr);
  case KnownProtocolKind::Differentiable:
    return derived.deriveDifferentiable(AssocType);
  case KnownProtocolKind::DistributedActor:
    return derived.deriveDistributedActor(AssocType);
  case KnownProtocolKind::Identifiable:
    // Identifiable only has derivation logic for distributed actors,
    // because how it depends on the ActorSystem the actor is associated with.
    // If the nominal wasn't a distributed actor, we should not end up here,
    // but either way, then we'd return null (fail derivation).
    return derived.deriveDistributedActor(AssocType);
  default:
    return std::make_pair(nullptr, nullptr);
  }
}

std::pair<Type, TypeDecl *>
AssociatedTypeInference::computeDerivedTypeWitness(
                                              AssociatedTypeDecl *assocType) {
  if (adoptee->hasError())
    return std::make_pair(Type(), nullptr);

  // Can we derive conformances for this protocol and adoptee?
  NominalTypeDecl *derivingTypeDecl = dc->getSelfNominalTypeDecl();
  if (!DerivedConformance::derivesProtocolConformance(dc, derivingTypeDecl,
                                                      proto))
    return std::make_pair(Type(), nullptr);

  // Try to derive the type witness.
  auto result = deriveTypeWitness(dc, derivingTypeDecl, assocType);
  if (!result.first)
    return std::make_pair(Type(), nullptr);

  // Make sure that the derived type satisfies requirements.
  if (checkTypeWitness(result.first, assocType, conformance, llvm::None)) {
    /// FIXME: Diagnose based on this.
    failedDerivedAssocType = assocType;
    failedDerivedWitness = result.first;
    return std::make_pair(Type(), nullptr);
  }

  return result;
}

llvm::Optional<AbstractTypeWitness>
AssociatedTypeInference::computeAbstractTypeWitness(
    AssociatedTypeDecl *assocType) {
  // We don't have a type witness for this associated type, so go
  // looking for more options.
  if (Type concreteType = computeFixedTypeWitness(assocType))
    return AbstractTypeWitness(assocType, concreteType);

  // If we can form a default type, do so.
  if (const auto &typeWitness = computeDefaultTypeWitness(assocType))
    return typeWitness;

  // If there is a generic parameter of the named type, use that.
  if (auto genericSig = dc->getGenericSignatureOfContext()) {
    for (auto gp : genericSig.getInnermostGenericParams()) {
      // Packs cannot witness associated type requirements.
      if (gp->isParameterPack())
        continue;

      if (gp->getName() == assocType->getName())
        return AbstractTypeWitness(assocType, gp);
    }
  }

  return llvm::None;
}

void AssociatedTypeInference::collectAbstractTypeWitnesses(
    TypeWitnessSystem &system,
    ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes) const {
  // Look for suitably-named generic parameters first, before we go digging
  // through same-type requirements of protocols.
  if (auto genericSig = dc->getGenericSignatureOfContext()) {
    for (auto *const assocType : unresolvedAssocTypes) {
      for (auto *gp : genericSig.getInnermostGenericParams()) {
        // Packs cannot witness associated type requirements.
        if (gp->isParameterPack())
          continue;

        if (gp->getName() == assocType->getName()) {
          system.addTypeWitness(assocType->getName(), gp);
        }
      }
    }
  }

  auto considerProtocolRequirements = [&](ProtocolDecl *conformedProto) {
    // FIXME: The RequirementMachine will assert on re-entrant construction.
    // We should find a more principled way of breaking this cycle.
    if (ctx.isRecursivelyConstructingRequirementMachine(
            conformedProto->getGenericSignature().getCanonicalSignature()) ||
        ctx.isRecursivelyConstructingRequirementMachine(conformedProto) ||
        conformedProto->isComputingRequirementSignature())
      return;

    for (const auto &req :
         conformedProto->getRequirementSignature().getRequirements()) {
      if (req.getKind() == RequirementKind::SameType)
        system.addSameTypeRequirement(req);
    }
  };


  // First, look at the conformed protocol for same-type requirements. These
  // are less likely to cause request cycles.
  considerProtocolRequirements(conformance->getProtocol());

  // Also look through all other protocols the conforming type conforms to.
  for (auto *const conformedProto :
       dc->getSelfNominalTypeDecl()->getAllProtocols(/*sorted=*/true)) {
    considerProtocolRequirements(conformedProto);
  }

  // If the same-type constraints weren't enough to resolve an associated type,
  // look for default type witnesses.
  for (auto *const assocType : unresolvedAssocTypes) {
    if (system.hasResolvedTypeWitness(assocType->getName()))
      continue;

    // If we find a default type definition, feed it to the system.
    if (const auto &typeWitness = computeDefaultTypeWitness(assocType)) {
      system.addDefaultTypeWitness(typeWitness->getType(),
                                   typeWitness->getDefaultedAssocType());
    }
  }
}

Type AssociatedTypeInference::substCurrentTypeWitnesses(Type type) {
  auto substOptions = getSubstOptionsWithCurrentTypeWitnesses();

  // Local function that folds dependent member types with non-dependent
  // bases into actual member references.
  std::function<Type(Type)> foldDependentMemberTypes;
  llvm::DenseSet<AssociatedTypeDecl *> recursionCheck;
  foldDependentMemberTypes = [&](Type type) -> Type {
    if (auto depMemTy = type->getAs<DependentMemberType>()) {
      auto baseTy = depMemTy->getBase().transform(foldDependentMemberTypes);
      if (baseTy.isNull() || baseTy->hasTypeParameter())
        return nullptr;

      auto assocType = depMemTy->getAssocType();
      if (!assocType)
        return nullptr;

      if (!recursionCheck.insert(assocType).second)
        return nullptr;

      SWIFT_DEFER { recursionCheck.erase(assocType); };

      auto *module = dc->getParentModule();

      // Try to substitute into the base type.
      Type result = depMemTy->substBaseType(
          baseTy, LookUpConformanceInModule(module), substOptions);
      if (!result->hasError())
        return result;

      // If that failed, check whether it's because of the conformance we're
      // evaluating.
      auto localConformance
        = module->lookupConformance(baseTy, assocType->getProtocol());
      if (localConformance.isInvalid() || localConformance.isAbstract() ||
          (localConformance.getConcrete()->getRootConformance() !=
           conformance)) {
        return nullptr;
      }

      // Find the tentative type witness for this associated type.
      auto known = typeWitnesses.begin(assocType);
      if (known == typeWitnesses.end())
        return nullptr;

      return known->first.transform(foldDependentMemberTypes);
    }

    // The presence of a generic type parameter indicates that we
    // cannot use this type binding.
    if (type->is<GenericTypeParamType>()) {
      return nullptr;
    }

    return type;
  };

  return type.transform(foldDependentMemberTypes);
}

/// "Sanitize" requirements for conformance checking, removing any requirements
/// that unnecessarily refer to associated types of other protocols.
static void sanitizeProtocolRequirements(
                                     ProtocolDecl *proto,
                                     ArrayRef<Requirement> requirements,
                                     SmallVectorImpl<Requirement> &sanitized) {
  std::function<Type(Type)> sanitizeType;
  sanitizeType = [&](Type outerType) {
    return outerType.transformRec([&](TypeBase *type) -> llvm::Optional<Type> {
      if (auto depMemTy = dyn_cast<DependentMemberType>(type)) {
        if (!depMemTy->getAssocType() ||
            depMemTy->getAssocType()->getProtocol() != proto) {

          if (auto *assocType = proto->getAssociatedType(depMemTy->getName())) {
            Type sanitizedBase = sanitizeType(depMemTy->getBase());
            if (!sanitizedBase)
              return Type();
            return Type(DependentMemberType::get(sanitizedBase,
                                                  assocType));
          }

          if (depMemTy->getBase()->is<GenericTypeParamType>())
            return Type();
        }
      }

      return llvm::None;
    });
  };

  for (const auto &req : requirements) {
    switch (req.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement not supported here");

    case RequirementKind::Conformance:
    case RequirementKind::SameType:
    case RequirementKind::Superclass: {
      Type firstType = sanitizeType(req.getFirstType());
      Type secondType = sanitizeType(req.getSecondType());
      if (firstType && secondType) {
        sanitized.push_back({req.getKind(), firstType, secondType});
      }
      break;
    }

    case RequirementKind::Layout: {
      Type firstType = sanitizeType(req.getFirstType());
      if (firstType) {
        sanitized.push_back({req.getKind(), firstType,
                             req.getLayoutConstraint()});
      }
      break;
    }
    }
  }
}

SubstOptions
AssociatedTypeInference::getSubstOptionsWithCurrentTypeWitnesses() {
  SubstOptions options(llvm::None);
  AssociatedTypeInference *self = this;
  options.getTentativeTypeWitness =
    [self](const NormalProtocolConformance *conformance,
           AssociatedTypeDecl *assocType) -> TypeBase * {
      auto thisProto = self->conformance->getProtocol();
      if (conformance == self->conformance) {
        // Okay: we have the associated type we need.
      } else if (conformance->getType()->isEqual(
                   self->conformance->getType()) &&
                 thisProto->inheritsFrom(conformance->getProtocol())) {
        // Find an associated type with the same name in the given
        // protocol.
        auto *foundAssocType = thisProto->getAssociatedType(
            assocType->getName());
        if (!foundAssocType) return nullptr;
        assocType = foundAssocType;
      } else {
        return nullptr;
      }

      Type type = self->typeWitnesses.begin(assocType)->first;

      // FIXME: Get rid of this hack.
      if (auto *aliasTy = dyn_cast<TypeAliasType>(type.getPointer()))
        type = aliasTy->getSinglyDesugaredType();

      if (type->hasArchetype()) {
        type = type.transformRec([&](Type t) -> llvm::Optional<Type> {
          if (auto *archetypeTy = dyn_cast<ArchetypeType>(t.getPointer())) {
            if (!isa<OpaqueTypeArchetypeType>(archetypeTy))
              return archetypeTy->getInterfaceType();
          }
          return llvm::None;
        });
      }

      return type.getPointer();
    };
  return options;
}

bool AssociatedTypeInference::checkCurrentTypeWitnesses(
       const SmallVectorImpl<std::pair<ValueDecl *, ValueDecl *>>
         &valueWitnesses) {
  // Check any same-type requirements in the protocol's requirement signature.
  SubstOptions options = getSubstOptionsWithCurrentTypeWitnesses();

  auto typeInContext = dc->mapTypeIntoContext(adoptee);

  auto substitutions =
    SubstitutionMap::getProtocolSubstitutions(
                                    proto, typeInContext,
                                    ProtocolConformanceRef(conformance));

  SmallVector<Requirement, 4> sanitizedRequirements;
  auto requirements = proto->getRequirementSignature().getRequirements();
  sanitizeProtocolRequirements(proto, requirements,
                               sanitizedRequirements);

  switch (checkRequirements(
      dc->getParentModule(), sanitizedRequirements,
      QuerySubstitutionMap{substitutions}, options)) {
  case CheckRequirementsResult::RequirementFailure:
    ++NumSolutionStatesFailedCheck;
    LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
               << "+ Requirement failure\n";);
    return true;

  case CheckRequirementsResult::Success:
  case CheckRequirementsResult::SubstitutionFailure:
    break;
  }

  // Check for extra requirements in the constrained extensions that supply
  // defaults.
  SmallPtrSet<ExtensionDecl *, 4> checkedExtensions;
  for (const auto &valueWitness : valueWitnesses) {
    // We only perform this additional checking for default associated types.
    if (!isa<TypeDecl>(valueWitness.first)) continue;

    auto witness = valueWitness.second;
    if (!witness) continue;

    auto ext = dyn_cast<ExtensionDecl>(witness->getDeclContext());
    if (!ext) continue;

    if (!ext->isConstrainedExtension()) continue;
    if (!checkedExtensions.insert(ext).second) continue;

    ++NumConstrainedExtensionChecks;
    if (checkConstrainedExtension(ext)) {
      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Constrained extension failed: " <<
                 ext->getExtendedType() << "\n";);
      ++NumConstrainedExtensionChecksFailed;
      return true;
    }
  }

  return false;
}

bool AssociatedTypeInference::checkConstrainedExtension(ExtensionDecl *ext) {
  auto typeInContext = dc->mapTypeIntoContext(adoptee);
  auto subs = typeInContext->getContextSubstitutions(ext);

  SubstOptions options = getSubstOptionsWithCurrentTypeWitnesses();
  switch (checkRequirements(
      dc->getParentModule(), ext->getGenericSignature().getRequirements(),
      QueryTypeSubstitutionMap{subs}, options)) {
  case CheckRequirementsResult::Success:
  case CheckRequirementsResult::SubstitutionFailure:
    return false;

  case CheckRequirementsResult::RequirementFailure:
    return true;
  }
  llvm_unreachable("unhandled result");
}

AssociatedTypeDecl *AssociatedTypeInference::inferAbstractTypeWitnesses(
    ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes, unsigned reqDepth) {
  if (unresolvedAssocTypes.empty()) {
    return nullptr;
  }

  LLVM_DEBUG(llvm::dbgs() << "Inferring abstract type witnesses for "
             << "associated types of " << conformance->getProtocol()->getName()
             << ":\n";);
  for (auto *assocType : unresolvedAssocTypes) {
    LLVM_DEBUG(llvm::dbgs() << "- " << assocType->getName() << "\n";);
  }

  // Attempt to compute abstract type witnesses for associated types that could
  // not resolve otherwise.
  llvm::SmallVector<AbstractTypeWitness, 2> abstractTypeWitnesses;

  if (ctx.LangOpts.EnableExperimentalAssociatedTypeInference) {
    TypeWitnessSystem system(unresolvedAssocTypes);
    collectAbstractTypeWitnesses(system, unresolvedAssocTypes);

    if (ctx.LangOpts.DumpTypeWitnessSystems) {
      system.dump(llvm::dbgs(), conformance);
    }

    // If we couldn't resolve an associated type, bail out.
    for (auto *assocType : unresolvedAssocTypes) {
      if (!system.hasResolvedTypeWitness(assocType->getName())) {
        return assocType;
      }
    }

    // Record the tentative type witnesses to make them available during
    // substitutions.
    for (auto *assocType : unresolvedAssocTypes) {
      auto resolvedTy = system.getResolvedTypeWitness(assocType->getName());

      resolvedTy = resolvedTy.transformRec([&](Type ty) -> llvm::Optional<Type> {
        if (auto *gp = ty->getAs<GenericTypeParamType>()) {
          // FIXME: 'computeFixedTypeWitness' uses 'getReducedType',
          // so if a generic parameter is canonical here, it's 'Self'.
          if (gp->isCanonical() ||
              isa<ProtocolDecl>(gp->getDecl()->getDeclContext()->getAsDecl())) {
            return adoptee;
          }
        }

        return llvm::None;
      });

      LLVM_DEBUG(llvm::dbgs() << "Inserting tentative witness for "
                 << assocType->getName() << ": "; resolvedTy.dump(llvm::dbgs()););
      typeWitnesses.insert(assocType, {resolvedTy, reqDepth});

      if (auto *defaultedAssocType =
              system.getDefaultedAssocType(assocType->getName())) {
        abstractTypeWitnesses.emplace_back(assocType, resolvedTy,
                                           defaultedAssocType);
      } else {
        abstractTypeWitnesses.emplace_back(assocType, resolvedTy);
      }
    }
  } else {
    for (auto *const assocType : unresolvedAssocTypes) {
      // Try to compute the type without the aid of a specific potential
      // witness.
      if (const auto &typeWitness = computeAbstractTypeWitness(assocType)) {
        auto resolvedTy = typeWitness->getType();

        resolvedTy = resolvedTy.transformRec([&](Type ty) -> llvm::Optional<Type> {
          if (auto *gp = ty->getAs<GenericTypeParamType>()) {
            // FIXME: 'computeFixedTypeWitness' uses 'getReducedType',
            // so if a generic parameter is canonical here, it's 'Self'.
            if (gp->isCanonical() ||
                isa<ProtocolDecl>(gp->getDecl()->getDeclContext()->getAsDecl())) {
              return adoptee;
            }
          }

          return llvm::None;
        });

        // Record the type witness immediately to make it available
        // for substitutions into other tentative type witnesses.
        LLVM_DEBUG(llvm::dbgs() << "Inserting tentative witness for "
                   << assocType->getName() << ": "; resolvedTy.dump(llvm::dbgs()););
        typeWitnesses.insert(assocType, {resolvedTy, reqDepth});

        abstractTypeWitnesses.emplace_back(assocType, resolvedTy,
                                           typeWitness->getDefaultedAssocType());
        continue;
      }

      // The solution is incomplete.
      return assocType;
    }
  }

  // Check each abstract type witness against the generic requirements on the
  // corresponding associated type.
  //
  // FIXME: Consider checking non-dependent type witnesses first. Checking in
  // default order can lead to the creation and exposure of malformed types in
  // diagnostics. For example, we would diagnose that 'G<Never>' (!) does not
  // conform to 'Sequence' in the below.
  //
  // struct G<S: Sequence> {}
  // protocol P {
  //   associatedtype A: Sequence = G<B>
  //   associatedtype B: Sequence = Never
  // }
  const auto substOptions = getSubstOptionsWithCurrentTypeWitnesses();
  for (const auto &witness : abstractTypeWitnesses) {
    auto *const assocType = witness.getAssocType();
    Type type = witness.getType();

    LLVM_DEBUG(llvm::dbgs() << "Checking witness for " << assocType->getName()
               << " " << type << "\n";);

    // Replace type parameters with other known or tentative type witnesses.
    if (type->hasDependentMember() || type->hasTypeParameter()) {
      // FIXME: We should find a better way to detect and reason about these
      // cyclic solutions so that we can spot them earlier and express them in
      // diagnostics.
      llvm::SmallPtrSet<AssociatedTypeDecl *, 4> circularityCheck;
      circularityCheck.insert(assocType);

      std::function<Type(Type)> substCurrentTypeWitnesses;
      substCurrentTypeWitnesses = [&](Type ty) -> Type {
        auto *const dmt = ty->getAs<DependentMemberType>();
        if (!dmt) {
          return ty;
        }

        const auto substBase =
            dmt->getBase().transform(substCurrentTypeWitnesses);
        if (!substBase) {
          return nullptr;
        }

        // If the transformed base has the same nominal as the adoptee, we may
        // need to look up a tentative type witness. Otherwise, just substitute
        // the base.
        if (substBase->getAnyNominal() != dc->getSelfNominalTypeDecl()) {
          return dmt->substBaseType(substBase,
                                    LookUpConformanceInModule(dc->getParentModule()),
                                    substOptions);
        }

        auto *assocTy = dmt->getAssocType();
        assert(
            assocTy &&
            "found structural DependentMemberType in tentative type witness");

        // Intercept recursive solutions.
        if (!circularityCheck.insert(assocTy).second) {
          return nullptr;
        }
        SWIFT_DEFER { circularityCheck.erase(dmt->getAssocType()); };

        if (assocTy->getProtocol() == proto) {
          // We have the associated type we need.
        } else if (proto->inheritsFrom(assocTy->getProtocol())) {
          // See if there is an associated type with the same name in our
          // protocol. If there isn't, keep the original associated type:
          // we'll be falling back to a base substitution.
          if (auto *decl = proto->getAssociatedType(assocTy->getName())) {
            assocTy = decl;
          }
        }

        // Find the type witness for this associated type.
        Type tyWitness;
        if (assocTy->getProtocol() == proto && typeWitnesses.count(assocTy)) {
          tyWitness = typeWitnesses.begin(assocTy)->first;

          // A tentative type witness may contain a 'Self'-rooted type
          // parameter,
          // FIXME: or a weird concrete-type-rooted dependent member type
          // coming from inference via a value witness. Make sure we sort these
          // out so that we don't break any subst() invariants.
          if (tyWitness->hasTypeParameter() ||
              tyWitness->hasDependentMember()) {
            tyWitness = tyWitness.transform(substCurrentTypeWitnesses);
          }

          if (tyWitness) {
            // HACK: Those inferred via value witnesses are eagerly mapped into
            // context. For now, do the same for abstract type witnesses and
            // handle archetypes.
            if (tyWitness->hasArchetype()) {
              tyWitness = tyWitness->mapTypeOutOfContext();
            }

            // If the transformed base is specialized, apply substitutions.
            if (tyWitness->hasTypeParameter()) {
              const auto conf = dc->getParentModule()->lookupConformance(
                  substBase, assocTy->getProtocol(), /*allowMissing=*/true);
              if (auto *specialized = dyn_cast<SpecializedProtocolConformance>(
                      conf.getConcrete())) {
                tyWitness = tyWitness.subst(specialized->getSubstitutionMap());
              }
            }
          }
        } else {
          // The associated type has a recorded type witness, or comes from a
          // different, possibly unrelated protocol; fall back to a base
          // substitution to find the type witness.
          tyWitness =
              DependentMemberType::get(proto->getSelfInterfaceType(), assocTy)
                  ->substBaseType(dc->getParentModule(), substBase);
        }

        return tyWitness;
      };

      type = type.transform(substCurrentTypeWitnesses);

      // If substitution failed, give up.
      if (!type || type->hasError())
        return assocType;

      type = dc->mapTypeIntoContext(type);

      LLVM_DEBUG(llvm::dbgs() << "Substituted witness type is " << type << "\n";);
    }

    if (const auto failed =
            checkTypeWitness(type, assocType, conformance, substOptions)) {
      LLVM_DEBUG(llvm::dbgs() << "- Type witness does not satisfy requirements\n";);

      // We failed to satisfy a requirement. If this is a default type
      // witness failure and we haven't seen one already, write it down.
      auto *defaultedAssocType = witness.getDefaultedAssocType();
      if (defaultedAssocType && !failedDefaultedAssocType &&
          failed.getKind() != CheckTypeWitnessResult::Error) {
        failedDefaultedAssocType = defaultedAssocType;
        failedDefaultedWitness = type;
        failedDefaultedResult = failed;
      }

      return assocType;
    }

    // Update the entry for this associated type.
    LLVM_DEBUG(llvm::dbgs() << "Updating tentative witness for "
               << assocType->getName() << ": "; type.dump(llvm::dbgs()););
    typeWitnesses.insert(assocType, {type, reqDepth});
  }

  return nullptr;
}

void AssociatedTypeInference::findSolutions(
                   ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                   SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  SmallVector<InferredTypeWitnessesSolution, 4> nonViableSolutions;
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> valueWitnesses;
  findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                   valueWitnesses, 0, 0, 0);

  for (auto solution : solutions) {
    LLVM_DEBUG(llvm::dbgs() << "=== Valid solution:\n";);
    for (auto pair : solution.TypeWitnesses) {
      LLVM_DEBUG(llvm::dbgs() << pair.first->getName() << " := "
                              << pair.second.first << "\n";);
    }
  }

  for (auto solution : nonViableSolutions) {
    LLVM_DEBUG(llvm::dbgs() << "=== Invalid solution:\n";);
    for (auto pair : solution.TypeWitnesses) {
      LLVM_DEBUG(llvm::dbgs() << pair.first->getName() << " := "
                              << pair.second.first << "\n";);
    }
  }
}

void AssociatedTypeInference::findSolutionsRec(
          ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
          SmallVectorImpl<InferredTypeWitnessesSolution> &solutions,
          SmallVectorImpl<InferredTypeWitnessesSolution> &nonViableSolutions,
          SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> &valueWitnesses,
          unsigned numTypeWitnesses,
          unsigned numValueWitnessesInProtocolExtensions,
          unsigned reqDepth) {
  using TypeWitnessesScope = decltype(typeWitnesses)::ScopeTy;

  // If we hit the last requirement, record and check this solution.
  if (reqDepth == inferred.size()) {
    // Introduce a hash table scope; we may add type witnesses here.
    TypeWitnessesScope typeWitnessesScope(typeWitnesses);

    // Filter out the associated types that remain unresolved.
    SmallVector<AssociatedTypeDecl *, 4> stillUnresolved;
    for (auto *const assocType : unresolvedAssocTypes) {
      const auto typeWitness = typeWitnesses.begin(assocType);
      if (typeWitness == typeWitnesses.end()) {
        stillUnresolved.push_back(assocType);
      } else {
        // If an erroneous type witness has already been recorded for one of
        // the associated types, give up.
        if (typeWitness->first->hasError()) {
          if (!missingTypeWitness)
            missingTypeWitness = assocType;

          LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                     << "+ Recorded an erroneous type witness\n";);
          return;
        }
      }
    }

    // Attempt to infer abstract type witnesses for associated types that
    // could not be resolved otherwise.
    if (auto *const assocType =
            inferAbstractTypeWitnesses(stillUnresolved, reqDepth)) {
      // The solution is decisively incomplete; record the associated type
      // we failed on and bail out.
      if (!missingTypeWitness)
        missingTypeWitness = assocType;

      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Failed to infer abstract witnesses\n";);
      return;
    }

    ++NumSolutionStates;

    // Validate and complete the solution.
    // Fold the dependent member types within this type.
    for (auto assocType : proto->getAssociatedTypeMembers()) {
      if (conformance->hasTypeWitness(assocType))
        continue;

      // If the type binding does not have a type parameter, there's nothing
      // to do.
      auto known = typeWitnesses.begin(assocType);
      assert(known != typeWitnesses.end());
      if (!known->first->hasTypeParameter() &&
          !known->first->hasDependentMember())
        continue;

      Type replaced = substCurrentTypeWitnesses(known->first);
      if (replaced.isNull()) {
        LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                   << "+ Failed substitution of " << known->first << "\n";);
        return;
      }

      known->first = replaced;
    }

    // Check whether our current solution matches the given solution.
    auto matchesSolution =
        [&](const InferredTypeWitnessesSolution &solution) {
      for (const auto &existingTypeWitness : solution.TypeWitnesses) {
        auto typeWitness = typeWitnesses.begin(existingTypeWitness.first);
        if (!typeWitness->first->isEqual(existingTypeWitness.second.first))
          return false;
      }

      return true;
    };

    // If we've seen this solution already, bail out; there's no point in
    // checking further.
    if (llvm::any_of(solutions, matchesSolution)) {
      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Duplicate valid solution found\n";);
      ++NumDuplicateSolutionStates;
      return;
    }
    if (llvm::any_of(nonViableSolutions, matchesSolution)) {
      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Duplicate invalid solution found\n";);
      ++NumDuplicateSolutionStates;
      return;
    }

    /// Check the current set of type witnesses.
    bool invalid = checkCurrentTypeWitnesses(valueWitnesses);

    if (invalid) {
      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Invalid solution found\n";);
    } else {
      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Valid solution found\n";);
    }

    auto &solutionList = invalid ? nonViableSolutions : solutions;
    solutionList.push_back(InferredTypeWitnessesSolution());
    auto &solution = solutionList.back();

    // Copy the type witnesses.
    for (auto assocType : unresolvedAssocTypes) {
      auto typeWitness = typeWitnesses.begin(assocType);
      solution.TypeWitnesses.insert({assocType, *typeWitness});
    }

    // Copy the value witnesses.
    solution.ValueWitnesses = valueWitnesses;
    solution.NumValueWitnessesInProtocolExtensions
      = numValueWitnessesInProtocolExtensions;

    // We're done recording the solution.
    return;
  }

  // Iterate over the potential witnesses for this requirement,
  // looking for solutions involving each one.
  const auto &inferredReq = inferred[reqDepth];
  for (const auto &witnessReq : inferredReq.second) {
    llvm::SaveAndRestore<unsigned> savedNumTypeWitnesses(numTypeWitnesses);

    // If we inferred a type witness via a default, try both with and without
    // the default.
    if (isa<TypeDecl>(inferredReq.first)) {
      // Recurse without considering this type.
      valueWitnesses.push_back({inferredReq.first, nullptr});
      findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                       valueWitnesses, numTypeWitnesses,
                       numValueWitnessesInProtocolExtensions, reqDepth + 1);
      valueWitnesses.pop_back();

      ++numTypeWitnesses;
      for (const auto &typeWitness : witnessReq.Inferred) {
        auto known = typeWitnesses.begin(typeWitness.first);
        if (known != typeWitnesses.end()) continue;

        // Enter a new scope for the type witnesses hash table.
        TypeWitnessesScope typeWitnessesScope(typeWitnesses);

        LLVM_DEBUG(llvm::dbgs() << "Inserting tentative witness for "
                   << typeWitness.first->getName() << ": ";
                   typeWitness.second.dump(llvm::dbgs()););
        typeWitnesses.insert(typeWitness.first, {typeWitness.second, reqDepth});

        valueWitnesses.push_back({inferredReq.first, witnessReq.Witness});
        findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                         valueWitnesses, numTypeWitnesses,
                         numValueWitnessesInProtocolExtensions, reqDepth + 1);
        valueWitnesses.pop_back();
      }

      continue;
    }

    // Enter a new scope for the type witnesses hash table.
    TypeWitnessesScope typeWitnessesScope(typeWitnesses);

    // Record this value witness, popping it when we exit the current scope.
    LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
               << "+ Pushing ";
               inferredReq.first->dumpRef(llvm::dbgs());
               llvm::dbgs() << " := ";
               witnessReq.Witness->dumpRef(llvm::dbgs());
               llvm::dbgs() << "\n";);
    valueWitnesses.push_back({inferredReq.first, witnessReq.Witness});
    if (!isa<TypeDecl>(inferredReq.first) &&
        witnessReq.Witness->getDeclContext()->getExtendedProtocolDecl())
      ++numValueWitnessesInProtocolExtensions;
    SWIFT_DEFER {
      if (!isa<TypeDecl>(inferredReq.first) &&
          witnessReq.Witness->getDeclContext()->getExtendedProtocolDecl())
        --numValueWitnessesInProtocolExtensions;

      valueWitnesses.pop_back();

      LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                 << "+ Popping ";
                 inferredReq.first->dumpRef(llvm::dbgs());
                 llvm::dbgs() << " := ";
                 witnessReq.Witness->dumpRef(llvm::dbgs());
                 llvm::dbgs() << "\n";);
    };

    // Introduce each of the type witnesses into the hash table.
    bool failed = false;
    for (const auto &typeWitness : witnessReq.Inferred) {
      // If we've seen a type witness for this associated type that
      // conflicts, there is no solution.
      auto known = typeWitnesses.begin(typeWitness.first);
      if (known != typeWitnesses.end()) {
        // Don't overwrite a defaulted associated type witness.
        if (isa<TypeDecl>(valueWitnesses[known->second].second))
          continue;

        // If witnesses for two different requirements inferred the same
        // type, we're okay.
        if (known->first->isEqual(typeWitness.second))
          continue;

        // If one has a type parameter remaining but the other does not,
        // drop the one with the type parameter.
        if ((known->first->hasTypeParameter() ||
             known->first->hasDependentMember())
            != (typeWitness.second->hasTypeParameter() ||
                typeWitness.second->hasDependentMember())) {
          if (typeWitness.second->hasTypeParameter() ||
              typeWitness.second->hasDependentMember())
            continue;

          known->first = typeWitness.second;
          continue;
        }

        if (!typeWitnessConflict ||
            numTypeWitnesses > numTypeWitnessesBeforeConflict) {
          typeWitnessConflict = {typeWitness.first,
                                 typeWitness.second,
                                 inferredReq.first,
                                 witnessReq.Witness,
                                 known->first,
                                 valueWitnesses[known->second].first,
                                 valueWitnesses[known->second].second};
          numTypeWitnessesBeforeConflict = numTypeWitnesses;
        }

        LLVM_DEBUG(llvm::dbgs() << std::string(valueWitnesses.size(), '+')
                   << "+ Failed " << typeWitness.first->getName() << ": ";
                   typeWitness.second->dump(llvm::dbgs()););

        failed = true;
        break;
      }

      LLVM_DEBUG(llvm::dbgs() << "Inserting tentative witness for "
                 << typeWitness.first->getName() << ": ";
                 typeWitness.second.dump(llvm::dbgs()););

      // Record the type witness.
      ++numTypeWitnesses;
      typeWitnesses.insert(typeWitness.first, {typeWitness.second, reqDepth});
    }

    if (failed)
      continue;

    // Recurse
    findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                     valueWitnesses, numTypeWitnesses,
                     numValueWitnessesInProtocolExtensions, reqDepth + 1);
  }
}

static Comparison compareDeclsForInference(DeclContext *DC, ValueDecl *decl1,
                                           ValueDecl *decl2) {
  // TypeChecker::compareDeclarations assumes that it's comparing two decls that
  // apply equally well to a call site. We haven't yet inferred the
  // associated types for a type, so the ranking algorithm used by
  // compareDeclarations to score protocol extensions is inappropriate,
  // since we may have potential witnesses from extensions with mutually
  // exclusive associated type constraints, and compareDeclarations will
  // consider these unordered since neither extension's generic signature
  // is a superset of the other.

  // If one of the declarations is null, it implies that we're working with
  // a skipped associated type default. Prefer that default to something
  // that came from a protocol extension.
  if (!decl1 || !decl2) {
    if (!decl1 &&
        decl2 && decl2->getDeclContext()->getExtendedProtocolDecl())
      return Comparison::Worse;

    if (!decl2 &&
        decl1 && decl1->getDeclContext()->getExtendedProtocolDecl())
      return Comparison::Better;

    return Comparison::Unordered;
  }


  // If the witnesses come from the same decl context, score normally.
  auto dc1 = decl1->getDeclContext();
  auto dc2 = decl2->getDeclContext();

  if (dc1 == dc2)
    return TypeChecker::compareDeclarations(DC, decl1, decl2);

  auto isProtocolExt1 = (bool)dc1->getExtendedProtocolDecl();
  auto isProtocolExt2 = (bool)dc2->getExtendedProtocolDecl();

  // If one witness comes from a protocol extension, favor the one
  // from a concrete context.
  if (isProtocolExt1 != isProtocolExt2) {
    return isProtocolExt1 ? Comparison::Worse : Comparison::Better;
  }

  // If both witnesses came from concrete contexts, score normally.
  // Associated type inference shouldn't impact the result.
  // FIXME: It could, if someone constrained to ConcreteType.AssocType...
  if (!isProtocolExt1)
    return TypeChecker::compareDeclarations(DC, decl1, decl2);

  // Compare protocol extensions by which protocols they require Self to
  // conform to. If one extension requires a superset of the other's
  // constraints, it wins.
  auto sig1 = dc1->getGenericSignatureOfContext();
  auto sig2 = dc2->getGenericSignatureOfContext();

  // FIXME: Extensions sometimes have null generic signatures while
  // checking the standard library...
  if (!sig1 || !sig2)
    return TypeChecker::compareDeclarations(DC, decl1, decl2);

  auto selfParam = GenericTypeParamType::get(/*isParameterPack*/ false,
                                             /*depth*/ 0, /*index*/ 0,
                                             decl1->getASTContext());

  // Collect the protocols required by extension 1.
  Type class1;
  SmallPtrSet<ProtocolDecl*, 4> protos1;

  std::function<void (ProtocolDecl*)> insertProtocol;
  insertProtocol = [&](ProtocolDecl *p) {
    if (!protos1.insert(p).second)
      return;

    for (auto parent : p->getInheritedProtocols())
      insertProtocol(parent);
  };

  for (auto &reqt : sig1.getRequirements()) {
    if (!reqt.getFirstType()->isEqual(selfParam))
      continue;
    switch (reqt.getKind()) {
    case RequirementKind::Conformance: {
      insertProtocol(reqt.getProtocolDecl());
      break;
    }
    case RequirementKind::Superclass:
      class1 = reqt.getSecondType();
      break;

    case RequirementKind::SameShape:
    case RequirementKind::SameType:
    case RequirementKind::Layout:
      break;
    }
  }

  // Compare with the protocols required by extension 2.
  Type class2;
  SmallPtrSet<ProtocolDecl*, 4> protos2;
  bool protos2AreSubsetOf1 = true;
  std::function<void (ProtocolDecl*)> removeProtocol;
  removeProtocol = [&](ProtocolDecl *p) {
    if (!protos2.insert(p).second)
      return;

    protos2AreSubsetOf1 &= protos1.erase(p);
    for (auto parent : p->getInheritedProtocols())
      removeProtocol(parent);
  };

  for (auto &reqt : sig2.getRequirements()) {
    if (!reqt.getFirstType()->isEqual(selfParam))
      continue;
    switch (reqt.getKind()) {
    case RequirementKind::Conformance: {
      removeProtocol(reqt.getProtocolDecl());
      break;
    }
    case RequirementKind::Superclass:
      class2 = reqt.getSecondType();
      break;

    case RequirementKind::SameShape:
    case RequirementKind::SameType:
    case RequirementKind::Layout:
      break;
    }
  }

  auto isClassConstraintAsStrict = [&](Type t1, Type t2) -> bool {
    if (!t1)
      return !t2;

    if (!t2)
      return true;

    return t2->isExactSuperclassOf(t1);
  };

  bool protos1AreSubsetOf2 = protos1.empty();
  // If the second extension requires strictly more protocols than the
  // first, it's better.
  if (protos1AreSubsetOf2 > protos2AreSubsetOf1
      && isClassConstraintAsStrict(class2, class1)) {
    return Comparison::Worse;
  // If the first extension requires strictly more protocols than the
  // second, it's better.
  } else if (protos2AreSubsetOf1 > protos1AreSubsetOf2
             && isClassConstraintAsStrict(class1, class2)) {
    return Comparison::Better;
  }

  // If they require the same set of protocols, or non-overlapping
  // sets, judge them normally.
  return TypeChecker::compareDeclarations(DC, decl1, decl2);
}

bool AssociatedTypeInference::isBetterSolution(
                      const InferredTypeWitnessesSolution &first,
                      const InferredTypeWitnessesSolution &second) {
  assert(first.ValueWitnesses.size() == second.ValueWitnesses.size());
  bool firstBetter = false;
  bool secondBetter = false;
  for (unsigned i = 0, n = first.ValueWitnesses.size(); i != n; ++i) {
    assert(first.ValueWitnesses[i].first == second.ValueWitnesses[i].first);
    auto firstWitness = first.ValueWitnesses[i].second;
    auto secondWitness = second.ValueWitnesses[i].second;
    if (firstWitness == secondWitness)
      continue;

    switch (compareDeclsForInference(dc, firstWitness, secondWitness)) {
    case Comparison::Better:
      if (secondBetter)
        return false;

      firstBetter = true;
      break;

    case Comparison::Worse:
      if (firstBetter)
        return false;

      secondBetter = true;
      break;

    case Comparison::Unordered:
      break;
    }
  }

  return firstBetter;
}

bool AssociatedTypeInference::findBestSolution(
                   SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  if (solutions.empty()) return true;
  if (solutions.size() == 1) return false;

  // Find the smallest number of value witnesses found in protocol extensions.
  // FIXME: This is a silly heuristic that should go away.
  unsigned bestNumValueWitnessesInProtocolExtensions
    = solutions.front().NumValueWitnessesInProtocolExtensions;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    bestNumValueWitnessesInProtocolExtensions
      = std::min(bestNumValueWitnessesInProtocolExtensions,
                 solutions[i].NumValueWitnessesInProtocolExtensions);
  }

  // Erase any solutions with more value witnesses in protocol
  // extensions than the best.
  solutions.erase(
    std::remove_if(solutions.begin(), solutions.end(),
                   [&](const InferredTypeWitnessesSolution &solution) {
                     return solution.NumValueWitnessesInProtocolExtensions >
                              bestNumValueWitnessesInProtocolExtensions;
                   }),
    solutions.end());

  // If we're down to one solution, success!
  if (solutions.size() == 1) return false;

  // Find a solution that's at least as good as the solutions that follow it.
  unsigned bestIdx = 0;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    if (isBetterSolution(solutions[i], solutions[bestIdx]))
      bestIdx = i;
  }

  // Make sure that solution is better than any of the other solutions.
  bool ambiguous = false;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    if (i != bestIdx && !isBetterSolution(solutions[bestIdx], solutions[i])) {
      ambiguous = true;
      break;
    }
  }

  // If the result was ambiguous, fail.
  if (ambiguous) {
    assert(solutions.size() != 1 && "should have succeeded somewhere above?");
    return true;

  }
  // Keep the best solution, erasing all others.
  if (bestIdx != 0)
    solutions[0] = std::move(solutions[bestIdx]);
  solutions.erase(solutions.begin() + 1, solutions.end());
  return false;
}

namespace {
  /// A failed type witness binding.
  struct FailedTypeWitness {
    /// The value requirement that triggered inference.
    ValueDecl *Requirement;

    /// The corresponding value witness from which the type witness
    /// was inferred.
    ValueDecl *Witness;

    /// The actual type witness that was inferred.
    Type TypeWitness;

    /// The failed type witness result.
    CheckTypeWitnessResult Result;
  };
} // end anonymous namespace

bool AssociatedTypeInference::diagnoseNoSolutions(
                         ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes) {
  // If a defaulted type witness failed, diagnose it.
  if (failedDefaultedAssocType) {
    auto failedDefaultedAssocType = this->failedDefaultedAssocType;
    auto failedDefaultedWitness = this->failedDefaultedWitness;
    auto failedDefaultedResult = this->failedDefaultedResult;

    ctx.addDelayedConformanceDiag(conformance, true,
      [failedDefaultedAssocType, failedDefaultedWitness,
       failedDefaultedResult](NormalProtocolConformance *conformance) {
        auto proto = conformance->getProtocol();
        auto &diags = proto->getASTContext().Diags;

        switch (failedDefaultedResult.getKind()) {
        case CheckTypeWitnessResult::Success:
        case CheckTypeWitnessResult::Error:
          llvm_unreachable("Should not end up here");

        case CheckTypeWitnessResult::Conformance:
        case CheckTypeWitnessResult::Layout:
          diags.diagnose(
             failedDefaultedAssocType,
             diag::default_associated_type_unsatisfied_conformance,
             failedDefaultedWitness,
             failedDefaultedAssocType,
             proto->getDeclaredInterfaceType(),
             failedDefaultedResult.getRequirement());
          break;

        case CheckTypeWitnessResult::Superclass:
          diags.diagnose(
             failedDefaultedAssocType,
             diag::default_associated_type_unsatisfied_superclass,
             failedDefaultedWitness,
             failedDefaultedAssocType,
             proto->getDeclaredInterfaceType(),
             failedDefaultedResult.getRequirement());
          break;

        case CheckTypeWitnessResult::Tuple:
          diags.diagnose(
             failedDefaultedAssocType,
             diag::default_associated_type_tuple,
             failedDefaultedWitness,
             failedDefaultedAssocType,
             proto->getDeclaredInterfaceType(),
             failedDefaultedResult.getRequirement());
          break;
        }
      });

    return true;
  }

  // Form a mapping from associated type declarations to failed type
  // witnesses.
  llvm::DenseMap<AssociatedTypeDecl *, SmallVector<FailedTypeWitness, 2>>
    failedTypeWitnesses;
  for (const auto &inferredReq : inferred) {
    for (const auto &inferredWitness : inferredReq.second) {
      for (const auto &nonViable : inferredWitness.NonViable) {
        failedTypeWitnesses[std::get<0>(nonViable)]
          .push_back({inferredReq.first, inferredWitness.Witness,
                      std::get<1>(nonViable), std::get<2>(nonViable)});
      }
    }
  }

  // Local function to attempt to diagnose potential type witnesses
  // that failed requirements.
  auto tryDiagnoseTypeWitness = [&](AssociatedTypeDecl *assocType) -> bool {
    auto known = failedTypeWitnesses.find(assocType);
    if (known == failedTypeWitnesses.end())
      return false;

    auto failedSet = std::move(known->second);
    ctx.addDelayedConformanceDiag(conformance, true,
      [assocType, failedSet](NormalProtocolConformance *conformance) {
        auto proto = conformance->getProtocol();
        auto &diags = proto->getASTContext().Diags;
        diags.diagnose(assocType, diag::bad_associated_type_deduction,
                       assocType, proto);
        for (const auto &failed : failedSet) {
          if (failed.Result.getKind() == CheckTypeWitnessResult::Error)
            continue;

          if ((!failed.TypeWitness->getAnyNominal() ||
               failed.TypeWitness->isExistentialType()) &&
              failed.Result.getKind() != CheckTypeWitnessResult::Superclass) {
            Type resultType;
            SourceRange typeRange;
            if (auto *var = dyn_cast<VarDecl>(failed.Witness)) {
              resultType = var->getValueInterfaceType();
              typeRange = var->getTypeSourceRangeForDiagnostics();
            } else if (auto *func = dyn_cast<FuncDecl>(failed.Witness)) {
              resultType = func->getResultInterfaceType();
              typeRange = func->getResultTypeSourceRange();
            } else if (auto *subscript = dyn_cast<SubscriptDecl>(failed.Witness)) {
              resultType = subscript->getElementInterfaceType();
              typeRange = subscript->getElementTypeSourceRange();
            }

            // If the type witness was inferred from an existential
            // result type, suggest an opaque result type instead,
            // which can conform to protocols.
            if (failed.TypeWitness->isExistentialType() &&
                resultType && resultType->isEqual(failed.TypeWitness) &&
                typeRange.isValid()) {
              diags.diagnose(typeRange.Start,
                             diag::suggest_opaque_type_witness,
                             assocType, failed.TypeWitness,
                             failed.Result.getRequirement())
                .highlight(typeRange)
                .fixItInsert(typeRange.Start, "some ");
              continue;
            }

            diags.diagnose(failed.Witness,
                           diag::associated_type_witness_conform_impossible,
                           assocType, failed.TypeWitness,
                           failed.Result.getRequirement());
            continue;
          }
          if (!failed.TypeWitness->getClassOrBoundGenericClass() &&
              failed.Result.getKind() == CheckTypeWitnessResult::Superclass) {
            diags.diagnose(failed.Witness,
                           diag::associated_type_witness_inherit_impossible,
                           assocType, failed.TypeWitness,
                           failed.Result.getRequirement());
            continue;
          }

          switch (failed.Result.getKind()) {
          case CheckTypeWitnessResult::Success:
          case CheckTypeWitnessResult::Error:
            llvm_unreachable("Should not end up here");

          case CheckTypeWitnessResult::Conformance:
          case CheckTypeWitnessResult::Layout:
            diags.diagnose(
               failed.Witness,
               diag::associated_type_deduction_unsatisfied_conformance,
               assocType, failed.TypeWitness,
               failed.Result.getRequirement());
            break;

          case CheckTypeWitnessResult::Superclass:
            diags.diagnose(
               failed.Witness,
               diag::associated_type_deduction_unsatisfied_superclass,
               assocType, failed.TypeWitness,
               failed.Result.getRequirement());
            break;

          case CheckTypeWitnessResult::Tuple:
            diags.diagnose(
               failed.Witness,
               diag::associated_type_deduction_tuple,
               assocType, failed.TypeWitness,
               failed.Result.getRequirement());
            break;
          }
        }
      });

    return true;
  };

  // Try to diagnose the first missing type witness we encountered.
  if (missingTypeWitness && tryDiagnoseTypeWitness(missingTypeWitness))
    return true;

  // Failing that, try to diagnose any type witness that failed a
  // requirement.
  for (auto assocType : unresolvedAssocTypes) {
    if (tryDiagnoseTypeWitness(assocType))
      return true;
  }

  // If we saw a conflict, complain about it.
  if (typeWitnessConflict) {
    auto typeWitnessConflict = this->typeWitnessConflict;

    ctx.addDelayedConformanceDiag(conformance, true,
      [typeWitnessConflict](NormalProtocolConformance *conformance) {
        auto &diags = conformance->getDeclContext()->getASTContext().Diags;
        diags.diagnose(typeWitnessConflict->AssocType,
                       diag::ambiguous_associated_type_deduction,
                       typeWitnessConflict->AssocType,
                       typeWitnessConflict->FirstType,
                       typeWitnessConflict->SecondType);

        diags.diagnose(typeWitnessConflict->FirstWitness,
                       diag::associated_type_deduction_witness,
                       typeWitnessConflict->FirstRequirement,
                       typeWitnessConflict->FirstType);
        diags.diagnose(typeWitnessConflict->SecondWitness,
                       diag::associated_type_deduction_witness,
                       typeWitnessConflict->SecondRequirement,
                       typeWitnessConflict->SecondType);
      });

    return true;
  }

  return false;
}

bool AssociatedTypeInference::diagnoseAmbiguousSolutions(
                  ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                  SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  for (auto assocType : unresolvedAssocTypes) {
    // Find two types that conflict.
    auto &firstSolution = solutions.front();

    // Local function to retrieve the value witness for the current associated
    // type within the given solution.
    auto getValueWitness = [&](InferredTypeWitnessesSolution &solution) {
      unsigned witnessIdx = solution.TypeWitnesses[assocType].second;
      if (witnessIdx < solution.ValueWitnesses.size())
        return solution.ValueWitnesses[witnessIdx];

      return std::pair<ValueDecl *, ValueDecl *>(nullptr, nullptr);
    };

    Type firstType = firstSolution.TypeWitnesses[assocType].first;

    // Extract the value witness used to deduce this associated type, if any.
    auto firstMatch = getValueWitness(firstSolution);

    Type secondType;
    std::pair<ValueDecl *, ValueDecl *> secondMatch;
    for (auto &solution : solutions) {
      Type typeWitness = solution.TypeWitnesses[assocType].first;
      if (!typeWitness->isEqual(firstType)) {
        secondType = typeWitness;
        secondMatch = getValueWitness(solution);
        break;
      }
    }

    if (!secondType)
      continue;

    // We found an ambiguity. diagnose it.
    ctx.addDelayedConformanceDiag(conformance, true,
      [assocType, firstType, firstMatch, secondType, secondMatch](
        NormalProtocolConformance *conformance) {
        auto &diags = assocType->getASTContext().Diags;
        diags.diagnose(assocType, diag::ambiguous_associated_type_deduction,
                       assocType, firstType, secondType);

        auto diagnoseWitness = [&](std::pair<ValueDecl *, ValueDecl *> match,
                                   Type type){
          // If we have a requirement/witness pair, diagnose it.
          if (match.first && match.second) {
            diags.diagnose(match.second,
                           diag::associated_type_deduction_witness,
                           match.first, type);

            return;
          }

          // Otherwise, we have a default.
          auto defaultDiag =
            diags.diagnose(assocType, diag::associated_type_deduction_default,
                           type);
          if (auto defaultTypeRepr = assocType->getDefaultDefinitionTypeRepr())
            defaultDiag.highlight(defaultTypeRepr->getSourceRange());
        };

        diagnoseWitness(firstMatch, firstType);
        diagnoseWitness(secondMatch, secondType);
      });

    return true;
  }

  return false;
}

bool AssociatedTypeInference::canAttemptEagerTypeWitnessDerivation(
    DeclContext *DC, AssociatedTypeDecl *assocType) {

  /// Rather than locating the TypeID via the default implementation of
  /// Identifiable, we need to find the type based on the associated ActorSystem
  if (auto *nominal = DC->getSelfNominalTypeDecl())
    if (nominal->isDistributedActor() &&
        assocType->getProtocol()->isSpecificProtocol(KnownProtocolKind::Identifiable)) {
    return true;
  }

  return false;
}

auto AssociatedTypeInference::solve()
    -> llvm::Optional<InferredTypeWitnesses> {
  LLVM_DEBUG(llvm::dbgs() << "============ Start " << conformance->getType()
                          << ": " << conformance->getProtocol()->getName()
                          << " ============\n";);

  SWIFT_DEFER {
    LLVM_DEBUG(llvm::dbgs() << "============ Finish " << conformance->getType()
                            << ": " << conformance->getProtocol()->getName()
                            << " ============\n";);
  };

  // Try to resolve type witnesses via name lookup.
  llvm::SetVector<AssociatedTypeDecl *> unresolvedAssocTypes;
  for (auto assocType : proto->getAssociatedTypeMembers()) {
    // If we already have a type witness, do nothing.
    if (conformance->hasTypeWitness(assocType))
      continue;

    if (canAttemptEagerTypeWitnessDerivation(dc, assocType)) {
      auto derivedType = computeDerivedTypeWitness(assocType);
      if (derivedType.first) {
        recordTypeWitness(conformance, assocType,
                          derivedType.first->mapTypeOutOfContext(),
                          derivedType.second);
        continue;
      }
    }

    // Try to resolve this type witness via name lookup, which is the
    // most direct mechanism, overriding all others.
    switch (resolveTypeWitnessViaLookup(conformance, assocType)) {
    case ResolveWitnessResult::Success:
      // Success. Move on to the next.
      LLVM_DEBUG(llvm::dbgs() << "Associated type " << assocType->getName()
                              << " has a valid witness\n";);
      continue;

    case ResolveWitnessResult::ExplicitFailed:
      LLVM_DEBUG(llvm::dbgs() << "Associated type " << assocType->getName()
                              << " has an invalid witness\n";);
      continue;

    case ResolveWitnessResult::Missing:
      // We did not find the witness via name lookup. Try to derive
      // it below.
      break;
    }

    // Finally, try to derive the witness if we know how.
    auto derivedType = computeDerivedTypeWitness(assocType);
    if (derivedType.first) {
      recordTypeWitness(conformance, assocType,
                        derivedType.first->mapTypeOutOfContext(),
                        derivedType.second);
      continue;
    }

    // We failed to derive the witness. We're going to go on to try
    // to infer it from potential value witnesses next.
    unresolvedAssocTypes.insert(assocType);
  }

  // Result variable to use for returns so that we get NRVO.
  llvm::Optional<InferredTypeWitnesses> result = InferredTypeWitnesses();

  // If we resolved everything, we're done.
  if (unresolvedAssocTypes.empty())
    return result;

  // Infer potential type witnesses from value witnesses.
  inferred = inferTypeWitnessesViaValueWitnesses(unresolvedAssocTypes);
  LLVM_DEBUG(llvm::dbgs() << "Candidates for inference:\n";
             dumpInferredAssociatedTypes(inferred));

  // Compute the set of solutions.
  SmallVector<InferredTypeWitnessesSolution, 4> solutions;
  findSolutions(unresolvedAssocTypes.getArrayRef(), solutions);

  // Go make sure that type declarations that would act as witnesses
  // did not get injected while we were performing checks above. This
  // can happen when two associated types in different protocols have
  // the same name, and validating a declaration (above) triggers the
  // type-witness generation for that second protocol, introducing a
  // new type declaration.
  // FIXME: This is ridiculous.
  for (auto assocType : unresolvedAssocTypes) {
    switch (resolveTypeWitnessViaLookup(conformance, assocType)) {
    case ResolveWitnessResult::Success:
    case ResolveWitnessResult::ExplicitFailed:
      // A declaration that can become a witness has shown up. Go
      // perform the resolution again now that we have more
      // information.
      LLVM_DEBUG(llvm::dbgs() << "Associated type " << assocType->getName()
                              << " now has a valid witness\n";);
      return solve();

    case ResolveWitnessResult::Missing:
      // The type witness is still missing. Keep going.
      break;
    }
  }

  // Find the best solution.
  if (!findBestSolution(solutions)) {
    assert(solutions.size() == 1 && "Not a unique best solution?");
    // Form the resulting solution.
    auto &typeWitnesses = solutions.front().TypeWitnesses;
    for (auto assocType : unresolvedAssocTypes) {
      assert(typeWitnesses.count(assocType) == 1 && "missing witness");
      auto replacement = typeWitnesses[assocType].first;
      // FIXME: We can end up here with dependent types that were not folded
      // away for some reason.
      if (replacement->hasDependentMember())
        return llvm::None;

      if (replacement->hasArchetype()) {
        replacement = replacement->mapTypeOutOfContext();
      }

      LLVM_DEBUG(llvm::dbgs() << "Best witness for " << assocType->getName()
                              << " is " << replacement->getCanonicalType()
                              << "\n";);
      result->push_back({assocType, replacement});
    }

    return result;
  }

  // Diagnose the complete lack of solutions.
  if (solutions.empty() &&
      diagnoseNoSolutions(unresolvedAssocTypes.getArrayRef()))
    return llvm::None;

  // Diagnose ambiguous solutions.
  if (!solutions.empty() &&
      diagnoseAmbiguousSolutions(unresolvedAssocTypes.getArrayRef(),
                                 solutions))
    return llvm::None;

  // Save the missing type witnesses for later diagnosis.
  for (auto assocType : unresolvedAssocTypes) {
    ctx.addDelayedMissingWitness(conformance, {assocType, {}});
  }

  return llvm::None;
}

void TypeWitnessSystem::EquivalenceClass::setResolvedType(Type ty) {
  assert(ty && "cannot resolve to a null type");
  assert(!isAmbiguous() && "must not set resolved type when ambiguous");

  ResolvedTyAndIsAmbiguous.setPointer(ty);
}

TypeWitnessSystem::TypeWitnessSystem(
    ArrayRef<AssociatedTypeDecl *> assocTypes) {
  for (auto *assocType : assocTypes) {
    this->TypeWitnesses.try_emplace(assocType->getName());
  }
}

TypeWitnessSystem::~TypeWitnessSystem() {
  for (auto *equivClass : this->EquivalenceClasses) {
    delete equivClass;
  }
}

bool TypeWitnessSystem::hasResolvedTypeWitness(Identifier name) const {
  return (bool)getResolvedTypeWitness(name);
}

Type TypeWitnessSystem::getResolvedTypeWitness(Identifier name) const {
  assert(this->TypeWitnesses.count(name));

  if (auto *equivClass = this->TypeWitnesses.lookup(name).EquivClass) {
    return equivClass->getResolvedType();
  }

  return Type();
}

AssociatedTypeDecl *
TypeWitnessSystem::getDefaultedAssocType(Identifier name) const {
  assert(this->TypeWitnesses.count(name));

  return this->TypeWitnesses.lookup(name).DefaultedAssocType;
}

void TypeWitnessSystem::addTypeWitness(Identifier name, Type type) {
  assert(this->TypeWitnesses.count(name));

  if (const auto *depTy = type->getAs<DependentMemberType>()) {
    // If the type corresponds to a name variable in the system, form an
    // equivalence between variables.
    if (depTy->getBase()->is<GenericTypeParamType>()) {
      if (this->TypeWitnesses.count(depTy->getName())) {
        return addEquivalence(name, depTy->getName());
      }
    } else {
      while (depTy->getBase()->is<DependentMemberType>()) {
        depTy = depTy->getBase()->castTo<DependentMemberType>();
      }

      // Equivalences of the form 'Self.X == Self.X.*' do not contribute
      // to solving the system, so just ignore them.
      if (name == depTy->getName()) {
        return;
      }
    }
  }

  auto &tyWitness = this->TypeWitnesses[name];

  // Assume that the type resolves the equivalence class.
  if (tyWitness.EquivClass) {
    // Nothing else to do if the equivalence class had been marked as ambiguous.
    if (tyWitness.EquivClass->isAmbiguous()) {
      return;
    }

    // If we already have a resolved type, keep going only if the new one is
    // a better choice.
    const Type currResolvedTy = tyWitness.EquivClass->getResolvedType();
    if (currResolvedTy) {
      switch (compareResolvedTypes(type, currResolvedTy)) {
      case ResolvedTypeComparisonResult::Better:
        break;
      case ResolvedTypeComparisonResult::EquivalentOrWorse:
        return;
      case ResolvedTypeComparisonResult::Ambiguity:
        // Mark the equivalence class as ambiguous and give up.
        tyWitness.EquivClass->setAmbiguous();
        return;
      }
    }
  }

  // If we can find an existing equivalence class for this type, use it.
  for (auto *const equivClass : this->EquivalenceClasses) {
    if (equivClass->getResolvedType() &&
        equivClass->getResolvedType()->isEqual(type)) {
      if (tyWitness.EquivClass) {
        mergeEquivalenceClasses(equivClass, tyWitness.EquivClass);
      } else {
        tyWitness.EquivClass = equivClass;
      }

      return;
    }
  }

  if (tyWitness.EquivClass) {
    tyWitness.EquivClass->setResolvedType(type);
  } else {
    auto *equivClass = new EquivalenceClass(type);
    this->EquivalenceClasses.insert(equivClass);

    tyWitness.EquivClass = equivClass;
  }
}

void TypeWitnessSystem::addDefaultTypeWitness(
    Type type, AssociatedTypeDecl *defaultedAssocType) {
  const auto name = defaultedAssocType->getName();
  assert(this->TypeWitnesses.count(name));

  auto &tyWitness = this->TypeWitnesses[name];
  assert(!hasResolvedTypeWitness(name) && "already resolved a type witness");
  assert(!tyWitness.DefaultedAssocType &&
         "already recorded a default type witness");

  // Set the defaulted associated type.
  tyWitness.DefaultedAssocType = defaultedAssocType;

  // Record the type witness.
  addTypeWitness(name, type);
}

void TypeWitnessSystem::addSameTypeRequirement(const Requirement &req) {
  assert(req.getKind() == RequirementKind::SameType);

  auto *const depTy1 = req.getFirstType()->getAs<DependentMemberType>();
  auto *const depTy2 = req.getSecondType()->getAs<DependentMemberType>();

  // Equivalences other than 'Self.X == ...' (or '... == Self.X'), where
  // 'X' is a name variable in this system, do not contribute to solving
  // the system.
  if (depTy1 && depTy1->getBase()->is<GenericTypeParamType>() &&
      this->TypeWitnesses.count(depTy1->getName())) {
    addTypeWitness(depTy1->getName(), req.getSecondType());
  } else if (depTy2 && depTy2->getBase()->is<GenericTypeParamType>() &&
             this->TypeWitnesses.count(depTy2->getName())) {
    addTypeWitness(depTy2->getName(), req.getFirstType());
  }
}

void TypeWitnessSystem::dump(
    llvm::raw_ostream &out,
    const NormalProtocolConformance *conformance) const {
  llvm::SmallVector<Identifier, 4> sortedNames;
  sortedNames.reserve(this->TypeWitnesses.size());

  for (const auto &pair : this->TypeWitnesses) {
    sortedNames.push_back(pair.first);
  }

  // Deterministic ordering.
  llvm::array_pod_sort(sortedNames.begin(), sortedNames.end(),
                       [](const Identifier *lhs, const Identifier *rhs) -> int {
                         return lhs->compare(*rhs);
                       });

  out << "Abstract type witness system for conformance"
      << " of " << conformance->getType() << " to "
      << conformance->getProtocol()->getName() << ": {\n";

  for (const auto &name : sortedNames) {
    out.indent(2) << name << " => ";

    const auto *eqClass = this->TypeWitnesses.lookup(name).EquivClass;
    if (eqClass) {
      if (eqClass->getResolvedType()) {
        out << eqClass->getResolvedType();
      } else if (eqClass->isAmbiguous()) {
        out << "(ambiguous)";
      } else {
        out << "(unresolved)";
      }
    } else {
      out << "(unresolved)";
    }

    if (eqClass) {
      out << ", " << eqClass;
    }
    out << "\n";
  }
  out << "}\n";
}

void TypeWitnessSystem::addEquivalence(Identifier name1, Identifier name2) {
  assert(this->TypeWitnesses.count(name1));
  assert(this->TypeWitnesses.count(name2));

  if (name1 == name2) {
    return;
  }

  auto &tyWitness1 = this->TypeWitnesses[name1];
  auto &tyWitness2 = this->TypeWitnesses[name2];

  // If both candidates are associated with existing equivalence classes,
  // merge them.
  if (tyWitness1.EquivClass && tyWitness2.EquivClass) {
    mergeEquivalenceClasses(tyWitness1.EquivClass, tyWitness2.EquivClass);
    return;
  }

  if (tyWitness1.EquivClass) {
    tyWitness2.EquivClass = tyWitness1.EquivClass;
  } else if (tyWitness2.EquivClass) {
    tyWitness1.EquivClass = tyWitness2.EquivClass;
  } else {
    // Neither has an associated equivalence class.
    auto *equivClass = new EquivalenceClass(nullptr);
    this->EquivalenceClasses.insert(equivClass);

    tyWitness1.EquivClass = equivClass;
    tyWitness2.EquivClass = equivClass;
  }
}

void TypeWitnessSystem::mergeEquivalenceClasses(
    EquivalenceClass *equivClass1, const EquivalenceClass *equivClass2) {
  assert(equivClass1 && equivClass2);
  if (equivClass1 == equivClass2) {
    return;
  }

  // Merge the second equivalence class into the first.
  if (equivClass1->getResolvedType() && equivClass2->getResolvedType()) {
    switch (compareResolvedTypes(equivClass2->getResolvedType(),
                                 equivClass1->getResolvedType())) {
    case ResolvedTypeComparisonResult::Better:
      equivClass1->setResolvedType(equivClass2->getResolvedType());
      break;
    case ResolvedTypeComparisonResult::EquivalentOrWorse:
      break;
    case ResolvedTypeComparisonResult::Ambiguity:
      equivClass1->setAmbiguous();
      break;
    }
  } else if (equivClass1->isAmbiguous()) {
    // Ambiguity is retained.
  } else if (equivClass2->getResolvedType()) {
    // Carry over the resolved type.
    equivClass1->setResolvedType(equivClass2->getResolvedType());
  } else if (equivClass2->isAmbiguous()) {
    // Carry over ambiguity.
    equivClass1->setAmbiguous();
  }

  // Migrate members of the second equivalence class to the first.
  for (auto &pair : this->TypeWitnesses) {
    if (pair.second.EquivClass == equivClass2) {
      pair.second.EquivClass = equivClass1;
    }
  }

  // Finally, dispose of the second equivalence class.
  this->EquivalenceClasses.erase(const_cast<EquivalenceClass *>(equivClass2));
  delete equivClass2;
}

TypeWitnessSystem::ResolvedTypeComparisonResult
TypeWitnessSystem::compareResolvedTypes(Type ty1, Type ty2) {
  assert(ty1 && ty2);
  if (!ty1->isTypeParameter()) {
    if (ty2->isTypeParameter()) {
      // A concrete type is better than a type parameter.
      return ResolvedTypeComparisonResult::Better;
    } else if (!ty1->isEqual(ty2)) {
      return ResolvedTypeComparisonResult::Ambiguity;
    }
  }

  // Anything else is either equivalent (i.e. actually equal concrete types or
  // type parameter vs. type parameter), or worse (i.e. type parameter vs.
  // concrete type).
  return ResolvedTypeComparisonResult::EquivalentOrWorse;
}

evaluator::SideEffect
ResolveTypeWitnessesRequest::evaluate(Evaluator &evaluator,
                                      NormalProtocolConformance *conformance) const {
  // Attempt to infer associated type witnesses.
  auto &ctx = conformance->getDeclContext()->getASTContext();

  AssociatedTypeInference inference(ctx, conformance);
  if (auto inferred = inference.solve()) {
    for (const auto &inferredWitness : *inferred) {
      recordTypeWitness(conformance,
                        inferredWitness.first,
                        inferredWitness.second,
                        /*typeDecl=*/nullptr);
    }

    return evaluator::SideEffect();
  }

  // Conformance failed. Record errors for each of the witnesses.
  conformance->setInvalid();

  // We're going to produce an error below. Mark each unresolved
  // associated type witness as erroneous.
  for (auto assocType : conformance->getProtocol()->getAssociatedTypeMembers()) {
    // If we already have a type witness, do nothing.
    if (conformance->hasTypeWitness(assocType))
      continue;

    recordTypeWitness(conformance, assocType, ErrorType::get(ctx), nullptr);
  }

  return evaluator::SideEffect();
}

TypeWitnessAndDecl
TypeWitnessRequest::evaluate(Evaluator &eval,
                             NormalProtocolConformance *conformance,
                             AssociatedTypeDecl *requirement) const {
  switch (resolveTypeWitnessViaLookup(conformance, requirement)) {
  case ResolveWitnessResult::Success:
  case ResolveWitnessResult::ExplicitFailed:
    // We resolved this type witness one way or another.
    break;

  case ResolveWitnessResult::Missing: {
    // The type witness is still missing. Resolve all of the type witnesses.
    auto &ctx = requirement->getASTContext();
    evaluateOrDefault(ctx.evaluator,
                      ResolveTypeWitnessesRequest{conformance},
                      evaluator::SideEffect());
    break;
  }
  }

  // FIXME: resolveTypeWitnessViaLookup() and ResolveTypeWitnessesRequest
  // pre-populate the type witnesses in this manner. This should be cleaned up.
  const auto known = conformance->TypeWitnesses.find(requirement);
  assert(known != conformance->TypeWitnesses.end() &&
         "Didn't resolve witness?");
  return known->second;
}

ProtocolConformanceRef
AssociatedConformanceRequest::evaluate(Evaluator &eval,
                                       NormalProtocolConformance *conformance,
                                       CanType origTy, ProtocolDecl *reqProto,
                                       unsigned index) const {
  auto *module = conformance->getDeclContext()->getParentModule();

  auto subMap = SubstitutionMap::getProtocolSubstitutions(
      conformance->getProtocol(),
      conformance->getType(),
      ProtocolConformanceRef(conformance));
  auto substTy = origTy.subst(subMap);

  // Looking up a conformance for a contextual type and mapping the
  // conformance context produces a more accurate result than looking
  // up a conformance from an interface type.
  //
  // This can happen if the conformance has an associated conformance
  // depending on an associated type that is made concrete in a
  // refining protocol.
  //
  // That is, the conformance of an interface type G<T> : P really
  // depends on the generic signature of the current context, because
  // performing the lookup in a "more" constrained extension than the
  // one where the conformance was defined must produce concrete
  // conformances.
  //
  // FIXME: Eliminate this, perhaps by adding a variant of
  // lookupConformance() taking a generic signature.
  if (substTy->hasTypeParameter())
    substTy = conformance->getDeclContext()->mapTypeIntoContext(substTy);

  return module->lookupConformance(substTy, reqProto, /*allowMissing=*/true)
      .mapConformanceOutOfContext();
}

TinyPtrVector<AssociatedTypeDecl *>
ReferencedAssociatedTypesRequest::evaluate(Evaluator &eval,
                                           ValueDecl *req) const {
  // Collect the set of associated types rooted on Self in the
  // signature. Note that for references to nested types, we only
  // want to consider the outermost dependent member type.
  //
  // For example, a requirement typed '(Iterator.Element) -> ()'
  // is not considered to reference the associated type 'Iterator'.
  TinyPtrVector<AssociatedTypeDecl *> assocTypes;

  class Walker : public TypeWalker {
    ProtocolDecl *Proto;
    llvm::TinyPtrVector<AssociatedTypeDecl *> &assocTypes;
    llvm::SmallPtrSet<AssociatedTypeDecl *, 4> knownAssocTypes;

  public:
    Walker(ProtocolDecl *Proto,
           llvm::TinyPtrVector<AssociatedTypeDecl *> &assocTypes)
      : Proto(Proto), assocTypes(assocTypes) {}

    Action walkToTypePre(Type type) override {
      if (type->is<DependentMemberType>()) {
        if (auto assocType = getReferencedAssocTypeOfProtocol(type, Proto)) {
          if (knownAssocTypes.insert(assocType).second)
            assocTypes.push_back(assocType);
        }

        return Action::SkipChildren;
      }

      return Action::Continue;
    }
  };

  Walker walker(cast<ProtocolDecl>(req->getDeclContext()), assocTypes);

  // This dance below is to avoid calling getCanonicalType() on a
  // GenericFunctionType, which creates a GenericSignatureBuilder, which
  // can in turn trigger associated type inference and cause a cycle.
  auto reqTy = req->getInterfaceType();
  if (auto *funcTy = reqTy->getAs<GenericFunctionType>()) {
    for (auto param : funcTy->getParams())
      param.getPlainType()->getCanonicalType().walk(walker);
    funcTy->getResult()->getCanonicalType().walk(walker);
  } else {
    reqTy->getCanonicalType().walk(walker);
  }

  return assocTypes;
}
