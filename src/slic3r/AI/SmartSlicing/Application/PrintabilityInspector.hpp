#pragma once

#include "slic3r/AI/SmartSlicing/Domain/PrintabilityReport.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

namespace Slic3r::AI::SmartSlicing {

class PrintabilityInspector
{
public:
    PrintabilityReport inspect(const WorkspaceContext& context) const;
};

} // namespace Slic3r::AI::SmartSlicing
