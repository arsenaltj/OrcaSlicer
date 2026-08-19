#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct SlicingMetrics
{
    double estimated_time_seconds{0.0};
    double filament_volume_mm3{0.0};
    size_t tool_changes{0};
    std::vector<std::string> warning_codes;
};

} // namespace Slic3r::AI::SmartSlicing
