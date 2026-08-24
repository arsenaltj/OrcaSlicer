#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

namespace Slic3r::AI::SmartSlicing {

class IOrcaWorkspace
{
public:
    virtual ~IOrcaWorkspace() = default;

    virtual WorkspaceRevision current_revision() const = 0;
    virtual WorkspaceContext capture_context() const   = 0;
};

} // namespace Slic3r::AI::SmartSlicing
