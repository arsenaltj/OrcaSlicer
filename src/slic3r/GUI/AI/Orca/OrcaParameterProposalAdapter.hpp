#pragma once

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"

#include <string>
#include <vector>

namespace Slic3r::GUI {

struct OrcaParameterApplyResult
{
    bool accepted{false};
    std::string diagnostic_code;
};

class OrcaParameterProposalAdapter
{
public:
    OrcaParameterApplyResult validate_and_apply(const AI::SmartSlicing::ParameterProposal& proposal,
                                                int64_t expected_plate_id,
                                                const DynamicPrintConfig& base_config,
                                                DynamicPrintConfig& patched_config) const;
    OrcaParameterApplyResult validate_and_apply(
        const AI::SmartSlicing::ParameterProposal& proposal,
        int64_t expected_plate_id,
        const DynamicPrintConfig& base_config,
        const AI::SmartSlicing::IntentConstraintSnapshot& intent_constraints,
        const std::vector<AI::SmartSlicing::ParameterBoundEvidence>& profile_bounds,
        DynamicPrintConfig& patched_config) const;
};

} // namespace Slic3r::GUI
