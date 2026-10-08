#include "ParameterProposalValidator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

using Identity = std::tuple<ConfigScope, PresetOwner, int64_t, std::string>;

ParameterValidationResult rejected(ParameterRejectionCode code, size_t index,
                                   const std::string& key, std::string diagnostic = {})
{
    ParameterValidationResult result;
    result.rejections.push_back({code, index, key, std::move(diagnostic)});
    return result;
}

bool has_kind(const ConfigValue& value, ConfigValueKind kind)
{
    switch (kind) {
    case ConfigValueKind::Boolean: return std::holds_alternative<bool>(value);
    case ConfigValueKind::Integer: return std::holds_alternative<int64_t>(value);
    case ConfigValueKind::Floating: return std::holds_alternative<double>(value);
    case ConfigValueKind::Enumeration: return std::holds_alternative<std::string>(value);
    }
    return false;
}

bool numeric_kind(ConfigValueKind kind)
{
    return kind == ConfigValueKind::Integer || kind == ConfigValueKind::Floating;
}

double numeric_value(const ConfigValue& value)
{
    return std::holds_alternative<int64_t>(value) ? static_cast<double>(std::get<int64_t>(value)) :
                                                    std::get<double>(value);
}

template<class T> bool contains(const std::vector<T>& values, T value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

const ParameterValueSnapshot* find_current(const ParameterValidationContext& context,
                                           const ConfigPatchEntry& entry)
{
    const auto found = std::find_if(context.current_values.begin(), context.current_values.end(),
        [&](const ParameterValueSnapshot& value) {
            return value.scope == entry.scope && value.owner == entry.owner &&
                   value.target_id == entry.target_id && value.key == entry.key;
        });
    return found == context.current_values.end() ? nullptr : &*found;
}

const ParameterBoundEvidence* find_bound(const ParameterValidationContext& context,
                                         const ConfigPatchEntry& entry,
                                         BoundEvidenceSource source)
{
    const auto found = std::find_if(context.bounds.begin(), context.bounds.end(),
        [&](const ParameterBoundEvidence& bound) {
            return bound.scope == entry.scope && bound.owner == entry.owner &&
                   bound.target_id == entry.target_id && bound.key == entry.key &&
                   bound.source == source;
        });
    return found == context.bounds.end() ? nullptr : &*found;
}

bool active_intent_conflict(const ParameterPolicyEntry& policy, const ConfigPatchEntry& entry,
                            const IntentConstraintSnapshot& intents)
{
    for (const IntentConstraintRecord& record : intents.records) {
        if (!record.is_hard_constraint())
            continue;
        const bool object_matches = entry.scope != ConfigScope::Object ||
                                    record.object_id == static_cast<uint64_t>(entry.target_id);
        switch (policy.intent_rule) {
        case IntentConflictRule::None: break;
        case IntentConflictRule::PlacementLock:
            if (record.type == IntentConstraintType::PlatePlacementLock ||
                (object_matches && (record.type == IntentConstraintType::ObjectPlacementLock ||
                                    record.type == IntentConstraintType::InstancePlacementLock)))
                return true;
            break;
        case IntentConflictRule::SupportPainting:
            if (object_matches && record.type == IntentConstraintType::SupportPainting)
                return true;
            break;
        case IntentConflictRule::SeamPainting:
            if (object_matches && record.type == IntentConstraintType::SeamPainting)
                return true;
            break;
        case IntentConflictRule::MulticolorPainting:
            if (object_matches && record.type == IntentConstraintType::MulticolorPainting)
                return true;
            break;
        case IntentConflictRule::ExplicitParameterOverride:
            if (entry.scope == ConfigScope::Object && object_matches &&
                (record.type == IntentConstraintType::ObjectConfigOverride ||
                 record.type == IntentConstraintType::LayerHeightRange) &&
                std::find(record.parameter_keys.begin(), record.parameter_keys.end(), entry.key) !=
                    record.parameter_keys.end())
                return true;
            break;
        }
    }
    return false;
}

std::vector<BoundEvidenceSource> required_sources(BoundSource source)
{
    switch (source) {
    case BoundSource::PolicyRange: return {};
    case BoundSource::ProcessProfile: return {BoundEvidenceSource::ProcessProfile};
    case BoundSource::MachineProcessMaterialIntersection:
    case BoundSource::CalibratedProfileIntersection:
        return {BoundEvidenceSource::MachineProfile, BoundEvidenceSource::ProcessProfile,
                BoundEvidenceSource::MaterialProfile};
    case BoundSource::ImmutableFact: return {};
    }
    return {};
}

bool valid_target(const ConfigPatchEntry& entry)
{
    if (entry.target_id < 0)
        return false;
    if (entry.scope == ConfigScope::Workspace)
        return entry.target_id == 0;
    if (entry.scope == ConfigScope::Object || entry.scope == ConfigScope::Material)
        return entry.target_id > 0;
    return true;
}

ParameterValidationContext compatibility_context(const ParameterProposal& proposal)
{
    ParameterValidationContext context;
    context.goal = proposal.goal;
    for (const ConfigPatchEntry& entry : proposal.entries) {
        context.current_values.push_back(
            {entry.scope, entry.owner, entry.target_id, entry.key, entry.expected_value});
        const ParameterPolicyEntry* policy = ParameterPolicyRegistry::current().find(entry.key);
        if (policy != nullptr && policy->risk_class == ParameterRiskClass::StrategyControlled &&
            policy->bound_source == BoundSource::ProcessProfile && policy->safety_bounds) {
            context.bounds.push_back({entry.scope, entry.owner, entry.target_id, entry.key,
                                      BoundEvidenceSource::ProcessProfile, true,
                                      policy->safety_bounds->minimum, policy->safety_bounds->maximum,
                                      {}, "legacy-validator-compatibility/v1"});
        }
    }
    context.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{true, {}};
    };
    return context;
}

} // namespace

ParameterValidationResult ParameterProposalValidator::validate(const ParameterProposal& proposal) const
{
    return validate(proposal, compatibility_context(proposal));
}

ParameterValidationResult ParameterProposalValidator::validate(
    const ParameterProposal& proposal, const ParameterValidationContext& context) const
{
    const ParameterPolicyRegistry& registry = ParameterPolicyRegistry::current();
    if (proposal.entries.empty())
        return rejected(ParameterRejectionCode::EmptyProposal, 0, {});

    // Stage 1: policy identity and key registration.
    if (proposal.policy_version != registry.version())
        return rejected(ParameterRejectionCode::PolicyVersionMismatch, 0, {});
    std::vector<const ParameterPolicyEntry*> policies;
    policies.reserve(proposal.entries.size());
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ParameterPolicyEntry* policy = registry.find(proposal.entries[index].key);
        if (policy == nullptr)
            return rejected(ParameterRejectionCode::UnknownKey, index, proposal.entries[index].key);
        policies.push_back(policy);
    }

    // Stage 2: structural identity, type and duplicate checks.
    std::set<Identity> seen;
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        const ParameterPolicyEntry& policy = *policies[index];
        if (!has_kind(entry.expected_value, policy.value_kind) ||
            !has_kind(entry.new_value, policy.value_kind))
            return rejected(ParameterRejectionCode::TypeMismatch, index, entry.key);
        if (!contains(policy.scopes, entry.scope))
            return rejected(ParameterRejectionCode::ScopeNotAllowed, index, entry.key);
        if (entry.owner != policy.owner)
            return rejected(ParameterRejectionCode::OwnerNotAllowed, index, entry.key);
        if (!valid_target(entry))
            return rejected(ParameterRejectionCode::TargetNotSpecified, index, entry.key);
        if (!seen.emplace(entry.scope, entry.owner, entry.target_id, entry.key).second)
            return rejected(ParameterRejectionCode::DuplicateChange, index, entry.key);
        if (entry.expected_value == entry.new_value)
            return rejected(ParameterRejectionCode::NoEffectiveChange, index, entry.key);
    }

    // Stage 3: optimistic concurrency against the captured configuration.
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        const ParameterValueSnapshot* current = find_current(context, entry);
        if (current == nullptr)
            return rejected(ParameterRejectionCode::ExpectedSnapshotMissing, index, entry.key);
        if (current->value != entry.expected_value)
            return rejected(ParameterRejectionCode::ExpectedValueMismatch, index, entry.key);
    }

    // Stage 4: human intent and immutable calibration facts.
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        const ParameterPolicyEntry& policy = *policies[index];
        if (active_intent_conflict(policy, entry, context.intent_constraints))
            return rejected(ParameterRejectionCode::IntentConflict, index, entry.key,
                            intent_conflict_rule_name(policy.intent_rule));
        if (policy.risk_class == ParameterRiskClass::ImmutableFact)
            return rejected(ParameterRejectionCode::ForbiddenKey, index, entry.key,
                            policy.explanation_code);
    }

    // Stage 5: effective machine/process/material intersection.
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        const ParameterPolicyEntry& policy = *policies[index];
        if (numeric_kind(policy.value_kind)) {
            if (!policy.safety_bounds)
                return rejected(ParameterRejectionCode::EffectiveBoundsUnavailable, index, entry.key);
            double minimum = policy.safety_bounds->minimum;
            double maximum = policy.safety_bounds->maximum;
            for (const BoundEvidenceSource source : required_sources(policy.bound_source)) {
                const ParameterBoundEvidence* evidence = find_bound(context, entry, source);
                if (evidence == nullptr || !evidence->valid || evidence->evidence_version.empty() ||
                    !evidence->minimum || !evidence->maximum ||
                    !std::isfinite(*evidence->minimum) || !std::isfinite(*evidence->maximum))
                    return rejected(ParameterRejectionCode::EffectiveBoundsUnavailable, index, entry.key);
                minimum = std::max(minimum, *evidence->minimum);
                maximum = std::min(maximum, *evidence->maximum);
            }
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum)
                return rejected(ParameterRejectionCode::EffectiveBoundsEmpty, index, entry.key);
            const double replacement = numeric_value(entry.new_value);
            if (!std::isfinite(replacement) || replacement < minimum || replacement > maximum)
                return rejected(ParameterRejectionCode::RangeViolation, index, entry.key);
        } else if (policy.value_kind == ConfigValueKind::Enumeration) {
            const std::string& replacement = std::get<std::string>(entry.new_value);
            const std::string& expected = std::get<std::string>(entry.expected_value);
            if (std::find(policy.allowed_enum_values.begin(), policy.allowed_enum_values.end(), replacement) ==
                    policy.allowed_enum_values.end() ||
                std::find(policy.allowed_enum_values.begin(), policy.allowed_enum_values.end(), expected) ==
                    policy.allowed_enum_values.end())
                return rejected(ParameterRejectionCode::EnumViolation, index, entry.key);
        }
    }

    // Stage 6: goal contract and bounded PatchSet budgets.
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        const ParameterPolicyEntry& policy = *policies[index];
        if (!contains(policy.allowed_goals, context.goal) || proposal.goal != context.goal)
            return rejected(ParameterRejectionCode::GoalNotAllowed, index, entry.key);
        if (policy.maximum_change && numeric_kind(policy.value_kind) &&
            std::abs(numeric_value(entry.new_value) - numeric_value(entry.expected_value)) >
                *policy.maximum_change)
            return rejected(ParameterRejectionCode::ChangeBudgetExceeded, index, entry.key);
    }
    const ParameterPatchSetBudget& budget = registry.budget();
    if (proposal.entries.size() > budget.maximum_registered_changes)
        return rejected(ParameterRejectionCode::TooManyChanges, proposal.entries.size(), {});
    if (parameter_patch_set_serialized_bytes(proposal) > budget.maximum_serialized_bytes)
        return rejected(ParameterRejectionCode::SerializedBudgetExceeded, proposal.entries.size(), {});
    std::array<size_t, 4> scope_counts{};
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const size_t scope = config_scope_index(proposal.entries[index].scope);
        if (scope >= scope_counts.size())
            return rejected(ParameterRejectionCode::ScopeNotAllowed, index, proposal.entries[index].key);
        if (++scope_counts[scope] > budget.maximum_changes_per_scope[scope])
            return rejected(ParameterRejectionCode::ScopeBudgetExceeded, index, proposal.entries[index].key);
    }

    // Stage 7: Orca-native validation against an isolated config copy.
    if (!context.native_validator)
        return rejected(ParameterRejectionCode::NativeValidationUnavailable, 0, {});
    const NativeParameterValidationResult native = context.native_validator(proposal);
    if (!native.accepted)
        return rejected(ParameterRejectionCode::NativeValidationFailed, 0, {}, native.diagnostic_code);
    return {};
}

ParameterValidationResult ParameterProposalValidator::revalidate_for_apply(
    const ParameterProposal& proposal, const ParameterValidationContext& latest_context) const
{
    return validate(proposal, latest_context);
}

const char* parameter_rejection_code_name(ParameterRejectionCode code)
{
    switch (code) {
    case ParameterRejectionCode::EmptyProposal: return "empty_parameter_proposal";
    case ParameterRejectionCode::PolicyVersionMismatch: return "parameter_policy_version_mismatch";
    case ParameterRejectionCode::UnknownKey: return "parameter_key_not_allowed";
    case ParameterRejectionCode::TypeMismatch: return "parameter_type_mismatch";
    case ParameterRejectionCode::ScopeNotAllowed: return "parameter_scope_not_allowed";
    case ParameterRejectionCode::OwnerNotAllowed: return "parameter_owner_not_allowed";
    case ParameterRejectionCode::TargetNotSpecified: return "parameter_target_missing";
    case ParameterRejectionCode::DuplicateChange: return "duplicate_parameter_change";
    case ParameterRejectionCode::NoEffectiveChange: return "parameter_change_is_noop";
    case ParameterRejectionCode::ExpectedSnapshotMissing: return "parameter_expected_snapshot_missing";
    case ParameterRejectionCode::ExpectedValueMismatch: return "parameter_expected_value_changed";
    case ParameterRejectionCode::IntentConflict: return "parameter_conflicts_with_human_intent";
    case ParameterRejectionCode::ForbiddenKey: return "parameter_immutable_fact";
    case ParameterRejectionCode::EffectiveBoundsUnavailable: return "parameter_effective_bounds_unavailable";
    case ParameterRejectionCode::EffectiveBoundsEmpty: return "parameter_effective_bounds_empty";
    case ParameterRejectionCode::RangeViolation: return "parameter_range_violation";
    case ParameterRejectionCode::EnumViolation: return "parameter_enum_violation";
    case ParameterRejectionCode::GoalNotAllowed: return "parameter_goal_not_allowed";
    case ParameterRejectionCode::TooManyChanges: return "parameter_registered_key_budget_exceeded";
    case ParameterRejectionCode::SerializedBudgetExceeded: return "parameter_serialized_budget_exceeded";
    case ParameterRejectionCode::ScopeBudgetExceeded: return "parameter_scope_budget_exceeded";
    case ParameterRejectionCode::ChangeBudgetExceeded: return "parameter_change_budget_exceeded";
    case ParameterRejectionCode::NativeValidationUnavailable: return "parameter_native_validation_unavailable";
    case ParameterRejectionCode::NativeValidationFailed: return "parameter_native_validation_failed";
    }
    return "parameter_validation_failed";
}

} // namespace Slic3r::AI::SmartSlicing
