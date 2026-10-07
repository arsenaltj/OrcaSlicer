#pragma once

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposal.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

namespace Slic3r::AI::SmartSlicing {

class IParameterAdvisor
{
public:
    virtual ~IParameterAdvisor()                                      = default;
    virtual const char* algorithm_id() const noexcept { return "parameter-advisor"; }
    virtual const char* algorithm_version() const noexcept { return "unspecified"; }
    virtual ParameterProposal advise(const WorkspaceContext& context) = 0;
};

} // namespace Slic3r::AI::SmartSlicing
