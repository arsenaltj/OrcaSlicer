#pragma once

#include "slic3r/AI/SmartSlicing/Domain/ProtectedRegionBinding.hpp"

namespace Slic3r::AI::SmartSlicing {

class ProtectedRegionBinder
{
public:
    ProtectedRegionBindingResult bind(
        const std::vector<AI::ProtectedRegionManifest>& manifests,
        const ProtectedRegionBindingTarget& target) const;
};

} // namespace Slic3r::AI::SmartSlicing
