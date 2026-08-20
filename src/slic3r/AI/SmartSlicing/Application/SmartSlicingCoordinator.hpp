#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"
#include "CandidatePlanningWorkflow.hpp"
#include "PrintabilityInspector.hpp"

#include <functional>

namespace Slic3r::AI::SmartSlicing {

class SmartSlicingCoordinator
{
public:
    using Observer = std::function<void(const WorkflowSnapshot&)>;

    explicit SmartSlicingCoordinator(IOrcaWorkspace& workspace);
    SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor);

    const WorkflowSnapshot& snapshot() const { return m_snapshot; }
    void set_observer(Observer observer);

    void start();
    void cancel();
    bool refresh_revision();
    bool plan_and_slice_candidates(std::vector<SliceCandidate> proposals = {},
                                   CandidateGoal goal = CandidateGoal::Stability);

private:
    void transition(WorkflowState state, std::string detail = {});
    bool workspace_revision_matches() const;

    IOrcaWorkspace& m_workspace;
    ITrialSliceExecutor* m_trial_slice_executor{nullptr};
    PrintabilityInspector m_inspector;
    CandidatePlanningWorkflow m_candidate_planner;
    WorkflowSnapshot m_snapshot;
    Observer m_observer;
    WorkflowId m_last_workflow_id{0};
};

} // namespace Slic3r::AI::SmartSlicing
