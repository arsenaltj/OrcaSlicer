#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IWorkflowRuntimeStore.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkflowResourceBudget.hpp"
#include "CandidatePlanningWorkflow.hpp"
#include "PrintabilityInspector.hpp"

#include <functional>
#include <chrono>
#include <cstddef>
#include <optional>

namespace Slic3r::AI::SmartSlicing {

struct CandidateTrialTask
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    CandidateGoal goal{CandidateGoal::Stability};
    size_t candidate_index{0};
    size_t candidate_count{0};
    bool retry{false};
    SliceCandidate candidate;
};

struct CandidateTrialAcceptance
{
    bool consumed{false};
    bool accepted{false};
    std::optional<CandidateTrialTask> next_task;
};

class SmartSlicingCoordinator
{
public:
    using Observer = std::function<void(const WorkflowSnapshot&)>;
    using VersionedApplyFn = std::function<OfficialSliceResult(const WorkflowSnapshot&, const SliceCandidate&)>;
    using VersionedPollFn = std::function<OfficialSliceResult()>;
    using VersionedUndoFn = std::function<OfficialSliceResult()>;

    explicit SmartSlicingCoordinator(IOrcaWorkspace& workspace);
    SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor);
    SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor,
                            IOfficialSliceGateway& official_slice_gateway);

    const WorkflowSnapshot& snapshot() const { return m_snapshot; }
    void set_observer(Observer observer);
    void set_versioned_apply_callbacks(VersionedApplyFn apply, VersionedPollFn poll, VersionedUndoFn undo);
    bool set_runtime_store(IWorkflowRuntimeStore& runtime_store, bool recover = true);
    void set_resource_budget(WorkflowResourceBudget budget,
                             std::function<WorkflowResourceUsage()> usage_probe = {});

    void start();
    void cancel();
    bool refresh_revision();
    bool plan_and_slice_candidates(std::vector<SliceCandidate> proposals = {},
                                   CandidateGoal goal = CandidateGoal::Stability,
                                   bool defer_revision_checks = false);
    std::optional<CandidateTrialTask> begin_candidate_trials(
        std::vector<SliceCandidate> proposals = {}, CandidateGoal goal = CandidateGoal::Stability,
        bool defer_revision_checks = false);
    CandidateTrialAcceptance accept_candidate_trial_result(CandidateTrialTask task,
                                                            TrialSliceResult result,
                                                            bool defer_revision_checks = false);
    bool select_candidate(const CandidateId& candidate_id);
    bool retry_candidate(const CandidateId& candidate_id, bool defer_revision_checks = false);
    std::optional<CandidateTrialTask> begin_candidate_retry(const CandidateId& candidate_id,
                                                            bool defer_revision_checks = false);
    bool apply_selected_candidate();
    bool poll_official_slice();
    bool undo_applied_candidate();

private:
    void transition(WorkflowState state, std::string detail = {});
    bool workspace_revision_matches() const;
    void persist_runtime_state();
    std::string resource_violation(size_t candidate_count) const;
    CandidateTrialTask make_trial_task(size_t candidate_index, bool retry) const;
    void finish_candidate_trials();
    bool active_trial_matches(const CandidateTrialTask& task) const;
    void clear_active_trial();

    IOrcaWorkspace& m_workspace;
    ITrialSliceExecutor* m_trial_slice_executor{nullptr};
    IOfficialSliceGateway* m_official_slice_gateway{nullptr};
    PrintabilityInspector m_inspector;
    CandidatePlanningWorkflow m_candidate_planner;
    WorkflowSnapshot m_snapshot;
    std::optional<WorkspaceRevision> m_applied_revision;
    Observer m_observer;
    WorkflowId m_last_workflow_id{0};
    IWorkflowRuntimeStore* m_runtime_store{nullptr};
    WorkflowResourceBudget m_resource_budget;
    std::function<WorkflowResourceUsage()> m_usage_probe;
    VersionedApplyFn m_versioned_apply;
    VersionedPollFn m_versioned_poll;
    VersionedUndoFn m_versioned_undo;
    std::chrono::steady_clock::time_point m_started_at{std::chrono::steady_clock::now()};
    AttemptId m_last_trial_attempt{0};
    std::optional<AttemptId> m_active_trial_attempt;
    size_t m_active_trial_index{0};
    bool m_active_trial_retry{false};
};

} // namespace Slic3r::AI::SmartSlicing
