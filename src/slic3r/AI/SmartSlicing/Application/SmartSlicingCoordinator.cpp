#include "SmartSlicingCoordinator.hpp"

#include <exception>
#include <utility>

namespace Slic3r::AI::SmartSlicing {

SmartSlicingCoordinator::SmartSlicingCoordinator(IOrcaWorkspace& workspace) : m_workspace(workspace) {}

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
    transition(WorkflowState::Canceling, "canceling");
    transition(WorkflowState::Canceled, "canceled");
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
