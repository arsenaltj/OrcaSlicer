#pragma once

#include "ToolSequenceProposal.hpp"

#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ToolSequenceRejectionCode
{
    TooFewUsedFilaments,
    InvalidUsedFilaments,
    SlotCompatibilityUnavailable,
    SlotCompatibilityRejected,
    ColorMappingDegraded,
    PhysicalMappingMismatch,
    PrimeTowerStateMismatch,
    SourceSequenceMismatch,
    InvalidSequencePermutation,
    LayerRangeMismatch,
    NoEffectiveChange
};

struct ToolSequenceValidationResult
{
    std::vector<ToolSequenceRejectionCode> rejections;
    bool accepted() const { return rejections.empty(); }
};

class ToolSequenceProposalValidator
{
public:
    ToolSequenceValidationResult validate(const ToolSequenceProposal& proposal,
                                          const MulticolorSnapshot& current) const;
};

const char* tool_sequence_rejection_code_name(ToolSequenceRejectionCode code);

} // namespace Slic3r::AI::SmartSlicing
