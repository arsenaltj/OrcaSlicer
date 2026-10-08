#pragma once

#include "IntentConstraintSnapshot.hpp"
#include "ParameterPolicy.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ParameterRejectionCode {
    EmptyProposal,
    PolicyVersionMismatch,
    UnknownKey,
    TypeMismatch,
    ScopeNotAllowed,
    OwnerNotAllowed,
    TargetNotSpecified,
    DuplicateChange,
    NoEffectiveChange,
    ExpectedSnapshotMissing,
    ExpectedValueMismatch,
    IntentConflict,
    ForbiddenKey,
    EffectiveBoundsUnavailable,
    EffectiveBoundsEmpty,
    RangeViolation,
    EnumViolation,
    GoalNotAllowed,
    TooManyChanges,
    SerializedBudgetExceeded,
    ScopeBudgetExceeded,
    ChangeBudgetExceeded,
    NativeValidationUnavailable,
    NativeValidationFailed,
};

struct ParameterRejection
{
    ParameterRejectionCode code{ParameterRejectionCode::UnknownKey};
    size_t entry_index{0};
    std::string key;
    std::string diagnostic_code;
};

struct ParameterValidationResult
{
    std::vector<ParameterRejection> rejections;
    bool accepted() const { return rejections.empty(); }
};

struct ParameterValueSnapshot
{
    ConfigScope scope{ConfigScope::Plate};
    PresetOwner owner{PresetOwner::Process};
    int64_t target_id{-1};
    std::string key;
    ConfigValue value{false};
};

struct ParameterBoundEvidence
{
    ConfigScope scope{ConfigScope::Plate};
    PresetOwner owner{PresetOwner::Process};
    int64_t target_id{-1};
    std::string key;
    BoundEvidenceSource source{BoundEvidenceSource::ProcessProfile};
    bool valid{false};
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::vector<std::string> allowed_enum_values;
    std::string evidence_version;
};

struct NativeParameterValidationResult
{
    bool accepted{false};
    std::string diagnostic_code;
};

struct ParameterValidationContext
{
    RecommendationGoal goal{RecommendationGoal::Balanced};
    std::vector<ParameterValueSnapshot> current_values;
    IntentConstraintSnapshot intent_constraints;
    std::vector<ParameterBoundEvidence> bounds;
    std::function<NativeParameterValidationResult(const ParameterProposal&)> native_validator;
};

class ParameterProposalValidator
{
public:
    // Compatibility entry point for existing callers. Production application
    // must use the context overload so current values and bounds are explicit.
    ParameterValidationResult validate(const ParameterProposal& proposal) const;

    ParameterValidationResult validate(const ParameterProposal& proposal,
                                       const ParameterValidationContext& context) const;
    ParameterValidationResult revalidate_for_apply(
        const ParameterProposal& proposal,
        const ParameterValidationContext& latest_context) const;
};

const char* parameter_rejection_code_name(ParameterRejectionCode code);

} // namespace Slic3r::AI::SmartSlicing
