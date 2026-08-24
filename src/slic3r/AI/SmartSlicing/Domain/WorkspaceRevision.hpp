#pragma once

#include <cstdint>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct WorkspaceRevision
{
    uint64_t model_revision{0};
    uint64_t config_revision{0};
    uint64_t plate_revision{0};
    std::string fingerprint;

    bool valid() const { return !fingerprint.empty(); }
};

inline bool operator==(const WorkspaceRevision& lhs, const WorkspaceRevision& rhs)
{
    return lhs.model_revision == rhs.model_revision && lhs.config_revision == rhs.config_revision &&
           lhs.plate_revision == rhs.plate_revision && lhs.fingerprint == rhs.fingerprint;
}

inline bool operator!=(const WorkspaceRevision& lhs, const WorkspaceRevision& rhs) { return !(lhs == rhs); }

} // namespace Slic3r::AI::SmartSlicing
