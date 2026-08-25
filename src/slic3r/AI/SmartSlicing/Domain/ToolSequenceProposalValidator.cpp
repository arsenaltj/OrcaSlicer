#include "ToolSequenceProposalValidator.hpp"

#include <algorithm>
#include <set>

namespace Slic3r::AI::SmartSlicing {
namespace {

bool same_permutation(const std::vector<int>& sequence, const std::vector<int>& used)
{
    if (sequence.size() != used.size())
        return false;
    std::vector<int> ordered_sequence = sequence;
    std::vector<int> ordered_used = used;
    std::sort(ordered_sequence.begin(), ordered_sequence.end());
    std::sort(ordered_used.begin(), ordered_used.end());
    return ordered_sequence == ordered_used &&
           std::adjacent_find(ordered_sequence.begin(), ordered_sequence.end()) == ordered_sequence.end();
}

std::vector<ToolSequenceLayerRange> snapshot_ranges(const MulticolorSnapshot& snapshot)
{
    std::vector<ToolSequenceLayerRange> ranges;
    ranges.reserve(snapshot.other_layer_tool_sequences.size());
    for (const LayerToolSequenceSnapshot& sequence : snapshot.other_layer_tool_sequences)
        ranges.push_back({sequence.minimum_layer, sequence.maximum_layer, sequence.logical_filament_ids});
    return ranges;
}

void reject_once(ToolSequenceValidationResult& result, ToolSequenceRejectionCode code)
{
    if (std::find(result.rejections.begin(), result.rejections.end(), code) == result.rejections.end())
        result.rejections.push_back(code);
}

} // namespace

ToolSequenceValidationResult ToolSequenceProposalValidator::validate(
    const ToolSequenceProposal& proposal, const MulticolorSnapshot& current) const
{
    ToolSequenceValidationResult result;
    if (proposal.used_logical_filament_ids.size() < 2)
        reject_once(result, ToolSequenceRejectionCode::TooFewUsedFilaments);
    const std::set<int> used(proposal.used_logical_filament_ids.begin(), proposal.used_logical_filament_ids.end());
    if (used.size() != proposal.used_logical_filament_ids.size() ||
        std::any_of(used.begin(), used.end(), [](int id) { return id <= 0; }))
        reject_once(result, ToolSequenceRejectionCode::InvalidUsedFilaments);

    if (current.physical_slot_compatibility == PhysicalSlotCompatibility::Unavailable ||
        proposal.expected_physical_slot_compatibility == PhysicalSlotCompatibility::Unavailable)
        reject_once(result, ToolSequenceRejectionCode::SlotCompatibilityUnavailable);
    else if (current.physical_slot_compatibility != PhysicalSlotCompatibility::Compatible ||
             proposal.expected_physical_slot_compatibility != PhysicalSlotCompatibility::Compatible)
        reject_once(result, ToolSequenceRejectionCode::SlotCompatibilityRejected);
    if (current.color_mapping_degraded || proposal.expected_color_mapping_degraded)
        reject_once(result, ToolSequenceRejectionCode::ColorMappingDegraded);
    const bool mapping_incomplete = std::any_of(
        proposal.used_logical_filament_ids.begin(), proposal.used_logical_filament_ids.end(),
        [&proposal](int logical_id) {
            const size_t index = logical_id > 0 ? static_cast<size_t>(logical_id - 1) :
                                                  proposal.expected_filament_to_physical_slot.size();
            return index >= proposal.expected_filament_to_physical_slot.size() ||
                   proposal.expected_filament_to_physical_slot[index] <= 0;
        });
    if (mapping_incomplete || proposal.used_logical_filament_ids != current.used_logical_filament_ids ||
        proposal.expected_filament_to_physical_slot != current.filament_to_physical_slot)
        reject_once(result, ToolSequenceRejectionCode::PhysicalMappingMismatch);
    if (proposal.expected_prime_tower_enabled != current.prime_tower_enabled)
        reject_once(result, ToolSequenceRejectionCode::PrimeTowerStateMismatch);

    const std::vector<ToolSequenceLayerRange> current_ranges = snapshot_ranges(current);
    if (proposal.expected_first_layer_sequence != current.first_layer_tool_sequence ||
        proposal.expected_other_layer_sequences != current_ranges)
        reject_once(result, ToolSequenceRejectionCode::SourceSequenceMismatch);
    if (!same_permutation(proposal.expected_first_layer_sequence, proposal.used_logical_filament_ids) ||
        !same_permutation(proposal.new_first_layer_sequence, proposal.used_logical_filament_ids))
        reject_once(result, ToolSequenceRejectionCode::InvalidSequencePermutation);
    if (proposal.expected_other_layer_sequences.size() != proposal.new_other_layer_sequences.size())
        reject_once(result, ToolSequenceRejectionCode::LayerRangeMismatch);

    const size_t paired_count = std::min(proposal.expected_other_layer_sequences.size(),
                                         proposal.new_other_layer_sequences.size());
    for (size_t index = 0; index < paired_count; ++index) {
        const ToolSequenceLayerRange& expected = proposal.expected_other_layer_sequences[index];
        const ToolSequenceLayerRange& replacement = proposal.new_other_layer_sequences[index];
        if (expected.minimum_layer > expected.maximum_layer ||
            expected.minimum_layer != replacement.minimum_layer ||
            expected.maximum_layer != replacement.maximum_layer)
            reject_once(result, ToolSequenceRejectionCode::LayerRangeMismatch);
        if (!same_permutation(expected.logical_filament_ids, proposal.used_logical_filament_ids) ||
            !same_permutation(replacement.logical_filament_ids, proposal.used_logical_filament_ids))
            reject_once(result, ToolSequenceRejectionCode::InvalidSequencePermutation);
    }

    if (proposal.expected_first_layer_sequence == proposal.new_first_layer_sequence &&
        proposal.expected_other_layer_sequences == proposal.new_other_layer_sequences)
        reject_once(result, ToolSequenceRejectionCode::NoEffectiveChange);
    return result;
}

const char* tool_sequence_rejection_code_name(ToolSequenceRejectionCode code)
{
    switch (code) {
    case ToolSequenceRejectionCode::TooFewUsedFilaments: return "too_few_used_filaments";
    case ToolSequenceRejectionCode::InvalidUsedFilaments: return "invalid_used_filaments";
    case ToolSequenceRejectionCode::SlotCompatibilityUnavailable: return "slot_compatibility_unavailable";
    case ToolSequenceRejectionCode::SlotCompatibilityRejected: return "slot_compatibility_rejected";
    case ToolSequenceRejectionCode::ColorMappingDegraded: return "color_mapping_degraded";
    case ToolSequenceRejectionCode::PhysicalMappingMismatch: return "physical_mapping_mismatch";
    case ToolSequenceRejectionCode::PrimeTowerStateMismatch: return "prime_tower_state_mismatch";
    case ToolSequenceRejectionCode::SourceSequenceMismatch: return "source_sequence_mismatch";
    case ToolSequenceRejectionCode::InvalidSequencePermutation: return "invalid_sequence_permutation";
    case ToolSequenceRejectionCode::LayerRangeMismatch: return "layer_range_mismatch";
    case ToolSequenceRejectionCode::NoEffectiveChange: return "no_effective_change";
    }
    return "unknown_tool_sequence_rejection";
}

} // namespace Slic3r::AI::SmartSlicing
