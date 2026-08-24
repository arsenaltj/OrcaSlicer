#pragma once

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposal.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

namespace Slic3r::AI::SmartSlicing {

class IParameterAdvisor
{
public:
    virtual ~IParameterAdvisor()                                      = default;
    virtual ParameterProposal advise(const WorkspaceContext& context) = 0;
};

} // namespace Slic3r::AI::SmartSlicing
