#pragma once

#include "WorkspaceContext.hpp"

#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct ToolSequenceLayerRange
{
    int minimum_layer{0};
    int maximum_layer{0};
    std::vector<int> logical_filament_ids;
};

inline bool operator==(const ToolSequenceLayerRange& lhs, const ToolSequenceLayerRange& rhs)
{
    return lhs.minimum_layer == rhs.minimum_layer && lhs.maximum_layer == rhs.maximum_layer &&
           lhs.logical_filament_ids == rhs.logical_filament_ids;
}

struct ToolSequenceProposal
{
    std::vector<int> used_logical_filament_ids;
    std::vector<int> expected_filament_to_physical_slot;
    bool expected_prime_tower_enabled{false};
    PhysicalSlotCompatibility expected_physical_slot_compatibility{PhysicalSlotCompatibility::Unavailable};
    bool expected_color_mapping_degraded{false};
    std::vector<int> expected_first_layer_sequence;
    std::vector<int> new_first_layer_sequence;
    std::vector<ToolSequenceLayerRange> expected_other_layer_sequences;
    std::vector<ToolSequenceLayerRange> new_other_layer_sequences;
};

inline bool operator==(const ToolSequenceProposal& lhs, const ToolSequenceProposal& rhs)
{
    return lhs.used_logical_filament_ids == rhs.used_logical_filament_ids &&
           lhs.expected_filament_to_physical_slot == rhs.expected_filament_to_physical_slot &&
           lhs.expected_prime_tower_enabled == rhs.expected_prime_tower_enabled &&
           lhs.expected_physical_slot_compatibility == rhs.expected_physical_slot_compatibility &&
           lhs.expected_color_mapping_degraded == rhs.expected_color_mapping_degraded &&
           lhs.expected_first_layer_sequence == rhs.expected_first_layer_sequence &&
           lhs.new_first_layer_sequence == rhs.new_first_layer_sequence &&
           lhs.expected_other_layer_sequences == rhs.expected_other_layer_sequences &&
           lhs.new_other_layer_sequences == rhs.new_other_layer_sequences;
}

inline bool operator!=(const ToolSequenceProposal& lhs, const ToolSequenceProposal& rhs) { return !(lhs == rhs); }

} // namespace Slic3r::AI::SmartSlicing
