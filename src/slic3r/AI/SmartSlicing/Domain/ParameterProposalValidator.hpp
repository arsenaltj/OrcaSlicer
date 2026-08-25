#pragma once

#include "ParameterProposal.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ParameterRejectionCode {
    EmptyProposal,
    TooManyChanges,
    DuplicateChange,
    UnknownKey,
    TypeMismatch,
    RangeViolation,
    EnumViolation,
    ScopeNotAllowed,
    OwnerNotAllowed,
    TargetNotSpecified,
    ForbiddenKey,
    ChangeBudgetExceeded,
    NoEffectiveChange,
    IntentNotSpecified,
    IntentKeyNotAllowed,
    MixedTargets,
    MissingDependency,
    ForbiddenCombination
};

struct ParameterRejection
{
    ParameterRejectionCode code{ParameterRejectionCode::UnknownKey};
    size_t entry_index{0};
    std::string key;
};

struct ParameterValidationResult
{
    std::vector<ParameterRejection> rejections;
    bool accepted() const { return rejections.empty(); }
};

class ParameterProposalValidator
{
public:
    static constexpr size_t MAX_CHANGES = 4;

    ParameterValidationResult validate(const ParameterProposal& proposal) const;
};

const char* parameter_rejection_code_name(ParameterRejectionCode code);

} // namespace Slic3r::AI::SmartSlicing
