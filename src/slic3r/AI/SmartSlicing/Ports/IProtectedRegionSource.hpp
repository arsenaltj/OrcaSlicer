#pragma once

#include "slic3r/AI/Contracts/ProtectedRegionManifest.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceRevision.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ProtectedRegionSourceStatus { Available, Unknown, Rejected };

struct ProtectedRegionSourceQuery
{
    WorkspaceRevision workspace_revision;
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    uint64_t facet_count{0};
};

struct ProtectedRegionSourceResult
{
    ProtectedRegionSourceStatus status{ProtectedRegionSourceStatus::Unknown};
    std::vector<AI::ProtectedRegionManifest> manifests;
    std::string diagnostic_code;
};

class IProtectedRegionSource
{
public:
    virtual ~IProtectedRegionSource() = default;
    virtual ProtectedRegionSourceResult regions_for(const ProtectedRegionSourceQuery& query) = 0;
};

} // namespace Slic3r::AI::SmartSlicing
