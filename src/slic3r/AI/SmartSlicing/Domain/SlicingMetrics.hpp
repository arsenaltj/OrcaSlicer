#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct SlicingMetrics
{
    std::optional<double> estimated_time_seconds;
    std::optional<double> filament_volume_mm3;
    std::optional<double> support_volume_mm3;
    std::optional<double> flush_volume_mm3;
    std::optional<double> wipe_tower_volume_mm3;
    std::optional<size_t> tool_changes;
    std::vector<std::string> warning_codes;
};

} // namespace Slic3r::AI::SmartSlicing
