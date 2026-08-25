#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ToolSequenceProposalValidator.hpp"

#include <algorithm>
#include <optional>

namespace Slic3r::GUI {

class OrcaToolSequenceCandidateProvider
{
public:
    std::optional<AI::SmartSlicing::SliceCandidate>
    generate(const AI::SmartSlicing::WorkspaceContext& context,
             AI::SmartSlicing::CandidateGoal goal) const
    {
        using namespace AI::SmartSlicing;
        if (goal != CandidateGoal::MaterialSaving ||
            context.multicolor.used_logical_filament_ids.size() < 2 ||
            context.multicolor.other_layer_tool_sequences.empty())
            return std::nullopt;

        ToolSequenceProposal proposal;
        proposal.used_logical_filament_ids = context.multicolor.used_logical_filament_ids;
        proposal.expected_filament_to_physical_slot = context.multicolor.filament_to_physical_slot;
        proposal.expected_prime_tower_enabled = context.multicolor.prime_tower_enabled;
        proposal.expected_physical_slot_compatibility = context.multicolor.physical_slot_compatibility;
        proposal.expected_color_mapping_degraded = context.multicolor.color_mapping_degraded;
        proposal.expected_first_layer_sequence = context.multicolor.first_layer_tool_sequence;
        proposal.new_first_layer_sequence = context.multicolor.first_layer_tool_sequence;

        int preceding_final_tool = proposal.new_first_layer_sequence.empty() ? 0 :
                                                                     proposal.new_first_layer_sequence.back();
        for (const LayerToolSequenceSnapshot& source : context.multicolor.other_layer_tool_sequences) {
            ToolSequenceLayerRange expected{
                source.minimum_layer, source.maximum_layer, source.logical_filament_ids};
            ToolSequenceLayerRange replacement = expected;
            const auto continuation = std::find(replacement.logical_filament_ids.begin(),
                                                replacement.logical_filament_ids.end(),
                                                preceding_final_tool);
            if (continuation != replacement.logical_filament_ids.end())
                std::rotate(replacement.logical_filament_ids.begin(), continuation,
                            replacement.logical_filament_ids.end());
            if (!replacement.logical_filament_ids.empty())
                preceding_final_tool = replacement.logical_filament_ids.back();
            proposal.expected_other_layer_sequences.push_back(std::move(expected));
            proposal.new_other_layer_sequences.push_back(std::move(replacement));
        }

        if (!ToolSequenceProposalValidator().validate(proposal, context.multicolor).accepted())
            return std::nullopt;

        SliceCandidate candidate;
        candidate.id = "tool-sequence-material-saving-v1";
        candidate.base_revision = context.revision;
        candidate.goal = goal;
        candidate.tool_sequence = std::move(proposal);
        candidate.explanation = "preserve_multicolor_constraints_reorder_tool_sequence";
        return candidate;
    }
};

} // namespace Slic3r::GUI
