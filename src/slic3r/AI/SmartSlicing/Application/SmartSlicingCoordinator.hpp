#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "PrintabilityInspector.hpp"

#include <functional>

namespace Slic3r::AI::SmartSlicing {

class SmartSlicingCoordinator
{
public:
    using Observer = std::function<void(const WorkflowSnapshot&)>;

    explicit SmartSlicingCoordinator(IOrcaWorkspace& workspace);

    const WorkflowSnapshot& snapshot() const { return m_snapshot; }
    void set_observer(Observer observer);

    void start();
    void cancel();
    bool refresh_revision();

private:
    void transition(WorkflowState state, std::string detail = {});

    IOrcaWorkspace& m_workspace;
    PrintabilityInspector m_inspector;
    WorkflowSnapshot m_snapshot;
    Observer m_observer;
    WorkflowId m_last_workflow_id{0};
};

} // namespace Slic3r::AI::SmartSlicing
