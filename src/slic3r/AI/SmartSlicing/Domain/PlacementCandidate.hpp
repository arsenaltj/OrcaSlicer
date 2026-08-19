#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct ObjectTransform
{
    uint64_t object_id{0};
    std::array<double, 16> matrix{};
};

struct PlacementCandidate
{
    std::vector<ObjectTransform> transforms;
};

} // namespace Slic3r::AI::SmartSlicing
