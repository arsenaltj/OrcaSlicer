#pragma once

#include "WorkspaceRevision.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct WorkspaceObjectSnapshot
{
    uint64_t object_id{0};
    std::string name;
    size_t instance_count{0};
    size_t facet_count{0};
    size_t open_edge_count{0};
    bool outside_build_volume{false};
};

struct MaterialSnapshot
{
    std::string preset_id;
    std::string color;
    std::string filament_type;
    int logical_filament_id{0};
    int physical_slot_id{0};
    int nozzle_temperature{0};
    int nozzle_temperature_range_low{0};
    int nozzle_temperature_range_high{0};
    bool used_on_plate{false};
};

enum class PhysicalSlotCompatibility { NotApplicable, Compatible, Incompatible, InvalidTemperatureRange, Unavailable };

struct LayerToolSequenceSnapshot
{
    int minimum_layer{0};
    int maximum_layer{0};
    std::vector<int> logical_filament_ids;
};

struct MulticolorSnapshot
{
    std::vector<int> used_logical_filament_ids;
    std::vector<int> filament_to_physical_slot;
    std::vector<int> first_layer_tool_sequence;
    std::vector<LayerToolSequenceSnapshot> other_layer_tool_sequences;
    PhysicalSlotCompatibility physical_slot_compatibility{PhysicalSlotCompatibility::NotApplicable};
    bool color_mapping_degraded{false};
    bool prime_tower_enabled{false};
    bool flush_matrix_available{false};
    bool flush_multiplier_available{false};
};

struct WorkspaceContext
{
    WorkspaceRevision revision;
    int plate_index{-1};
    std::string printer_preset_id;
    std::string process_preset_id;
    std::string bed_type;
    std::vector<double> nozzle_diameters;
    std::vector<MaterialSnapshot> materials;
    MulticolorSnapshot multicolor;
    std::vector<WorkspaceObjectSnapshot> objects;
    bool native_validation_available{false};
    std::vector<std::string> validation_errors;
    std::vector<std::string> validation_warnings;
};

} // namespace Slic3r::AI::SmartSlicing
