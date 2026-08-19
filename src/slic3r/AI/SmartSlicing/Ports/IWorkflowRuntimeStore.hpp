#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"

namespace Slic3r::AI::SmartSlicing {

class IWorkflowRuntimeStore
{
public:
    virtual ~IWorkflowRuntimeStore()                    = default;
    virtual void save(const WorkflowSnapshot& snapshot) = 0;
    virtual void clear(WorkflowId workflow_id)          = 0;
};

} // namespace Slic3r::AI::SmartSlicing
