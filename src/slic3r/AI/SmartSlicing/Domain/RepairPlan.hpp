#pragma once

#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct RepairPlan
{
    std::vector<std::string> operation_codes;
    bool changes_geometry_semantics{false};
};

} // namespace Slic3r::AI::SmartSlicing
