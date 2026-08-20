#include "SmartSlicingCoordinator.hpp"
#include "TrialSlicingWorkflow.hpp"

#include <exception>
#include <utility>

namespace Slic3r::AI::SmartSlicing {

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace) : m_workspace(workspace) {}

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace, ITrialSliceExecutor& trial_slice_executor)
    : m_workspace(workspace), m_trial_slice_executor(&trial_slice_executor)
{}

void SmartSlicingCoordinator::set_observer(Observer observer)
{
    m_observer = std::move(observer);
    if (m_observer)
        m_observer(m_snapshot);
}

void SmartSlicingCoordinator::transition(WorkflowState state, std::string detail)
{
    m_snapshot.state  = state;
    m_snapshot.detail = std::move(detail);
    if (m_observer)
        m_observer(m_snapshot);
}

void SmartSlicingCoordinator::start()
{
    if (!m_snapshot.can_start())
        return;

    m_snapshot             = {};
    m_snapshot.workflow_id = ++m_last_workflow_id;

    try {
        transition(WorkflowState::CapturingContext, "capturing_workspace");
        WorkspaceContext context = m_workspace.capture_context();
        if (!context.revision.valid()) {
            transition(WorkflowState::Failed, "invalid_workspace_revision");
            return;
        }
        m_snapshot.context = std::move(context);

        transition(WorkflowState::Preflighting, "inspecting_printability");
        PrintabilityReport report = m_inspector.inspect(*m_snapshot.context);
        if (report.revision != m_snapshot.context->revision) {
            transition(WorkflowState::Failed, "preflight_revision_mismatch");
            return;
        }
        m_snapshot.report = std::move(report);

        if (m_snapshot.report->has_blocking_issue() || m_snapshot.report->readiness == Readiness::Blocked)
            transition(WorkflowState::AwaitingRiskDecision, "printability_action_required");
        else
            transition(WorkflowState::ReadyForCandidatePlanning, "preflight_complete");
    } catch (const std::exception& error) {
        transition(WorkflowState::Failed, error.what());
    } catch (...) {
        transition(WorkflowState::Failed, "unknown_preflight_error");
    }
}

void SmartSlicingCoordinator::cancel()
{
    if (!m_snapshot.can_cancel())
        return;
    const bool trial_slice_running = m_snapshot.state == WorkflowState::TrialSlicingBaseline ||
                                     m_snapshot.state == WorkflowState::TrialSlicingCandidates;
    transition(WorkflowState::Canceling, "canceling");
    if (trial_slice_running && m_trial_slice_executor != nullptr)
        m_trial_slice_executor->cancel_trial_slice();
    m_snapshot.candidates.clear();
    m_snapshot.comparison.reset();
    transition(WorkflowState::Canceled, "canceled");
}

bool SmartSlicingCoordinator::workspace_revision_matches() const
{
    return m_snapshot.context && m_workspace.current_revision() == m_snapshot.context->revision;
}

bool SmartSlicingCoordinator::plan_and_slice_candidates(std::vector<SliceCandidate> proposals, CandidateGoal goal)
{
    if (m_snapshot.state != WorkflowState::ReadyForCandidatePlanning || !m_snapshot.context ||
        m_trial_slice_executor == nullptr)
        return false;

    try {
        if (!workspace_revision_matches()) {
            transition(WorkflowState::Stale, "workspace_changed");
            return false;
        }

        transition(WorkflowState::PlanningCandidates, "planning_candidates");
        if (m_snapshot.state != WorkflowState::PlanningCandidates)
            return false;
        m_snapshot.goal       = goal;
        m_snapshot.candidates = m_candidate_planner.plan(*m_snapshot.context, std::move(proposals), goal);
        m_snapshot.comparison.reset();
        if (m_snapshot.candidates.empty()) {
            transition(WorkflowState::Failed, "no_candidates");
            return false;
        }

        for (size_t index = 0; index < m_snapshot.candidates.size(); ++index) {
            transition(index == 0 ? WorkflowState::TrialSlicingBaseline : WorkflowState::TrialSlicingCandidates,
                       index == 0 ? "trial_slicing_baseline" : "trial_slicing_candidate");
            const WorkflowState expected_state =
                index == 0 ? WorkflowState::TrialSlicingBaseline : WorkflowState::TrialSlicingCandidates;
            if (m_snapshot.state != expected_state)
                return false;

            SliceCandidate& candidate = m_snapshot.candidates[index];
            candidate.status          = CandidateStatus::TrialSlicing;
            TrialSliceResult result   = m_trial_slice_executor->execute_trial_slice(candidate);

            if (!workspace_revision_matches()) {
                for (SliceCandidate& planned : m_snapshot.candidates)
                    planned.status = CandidateStatus::Stale;
                m_snapshot.comparison.reset();
                transition(WorkflowState::Stale, "workspace_changed");
                return false;
            }
            if (result.status == TrialSliceStatus::Canceled && TrialSlicingWorkflow::result_matches(candidate, result)) {
                m_snapshot.candidates.clear();
                m_snapshot.comparison.reset();
                transition(WorkflowState::Canceled, "trial_slice_canceled");
                return false;
            }

            const bool accepted = TrialSlicingWorkflow::accept_result(candidate, std::move(result));
            if (index == 0 && !accepted) {
                transition(WorkflowState::Failed, "baseline_trial_failed");
                return false;
            }
        }

        m_snapshot.comparison = compare_candidates(m_snapshot.candidates, goal);
        if (m_snapshot.comparison->recommended_candidate_id.empty()) {
            transition(WorkflowState::Failed, "no_comparable_candidate");
            return false;
        }
        transition(WorkflowState::ReadyToApply, "candidates_ready");
        return true;
    } catch (const std::exception& error) {
        transition(WorkflowState::Failed, error.what());
    } catch (...) {
        transition(WorkflowState::Failed, "unknown_candidate_error");
    }
    return false;
}

bool SmartSlicingCoordinator::refresh_revision()
{
    if (!m_snapshot.context || !m_snapshot.can_cancel())
        return false;

    try {
        if (m_workspace.current_revision() == m_snapshot.context->revision)
            return false;
    } catch (...) {
        // A transient capture failure must not turn a previously valid candidate
        // into stale. The next refresh will retry.
        return false;
    }

    transition(WorkflowState::Stale, "workspace_changed");
    return true;
}

} // namespace Slic3r::AI::SmartSlicing
