#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

constexpr double BED_ADHESION_RISK_ATTENTION_THRESHOLD = 1.0;

struct SlicingMetrics
{
    std::optional<double> estimated_time_seconds;
    std::optional<double> filament_volume_mm3;
    std::optional<double> support_volume_mm3;
    std::optional<double> brim_volume_mm3;
    std::optional<double> bed_adhesion_risk_score;
    std::optional<double> flush_volume_mm3;
    std::optional<double> wipe_tower_volume_mm3;
    std::optional<size_t> tool_changes;
    std::optional<bool> physical_slots_compatible;
    std::optional<bool> color_mapping_degraded;
    std::optional<bool> prime_tower_enabled;
    std::vector<int> filament_to_physical_slot;
    std::vector<size_t> filament_change_sequence;
    std::vector<std::vector<size_t>> layer_tool_sequences;
    std::vector<std::string> warning_codes;

    std::optional<double> total_material_volume_mm3() const
    {
        if (!filament_volume_mm3 || !flush_volume_mm3 || !wipe_tower_volume_mm3)
            return std::nullopt;
        return *filament_volume_mm3 + *flush_volume_mm3 + *wipe_tower_volume_mm3;
    }
};

} // namespace Slic3r::AI::SmartSlicing
