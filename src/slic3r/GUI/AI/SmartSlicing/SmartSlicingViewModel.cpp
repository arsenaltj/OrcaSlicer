#include "SmartSlicingViewModel.hpp"

#include <algorithm>

namespace Slic3r::GUI {

SmartSlicingViewModel SmartSlicingViewModel::from_snapshot(const AI::SmartSlicing::WorkflowSnapshot& snapshot)
{
    using AI::SmartSlicing::WorkflowState;
    SmartSlicingViewModel view;
    view.can_start  = snapshot.can_start();
    view.can_cancel = snapshot.can_cancel();
    view.detail     = snapshot.detail;
    if (snapshot.report) {
        view.issue_count = snapshot.report->issues.size();
        view.issues.reserve(snapshot.report->issues.size());
        for (const AI::SmartSlicing::PrintabilityIssue& issue : snapshot.report->issues)
            view.issues.emplace_back(AI::SmartSlicing::issue_code_name(issue.code), issue.evidence);
    }

    auto complete_through = [&view](size_t index) {
        for (size_t i = 0; i <= index && i < view.stages.size(); ++i)
            view.stages[i].status = SmartSlicingStageStatus::Complete;
    };

    switch (snapshot.state) {
    case WorkflowState::Idle: view.summary_key = "ready_to_start"; break;
    case WorkflowState::CapturingContext:
        view.summary_key      = "capturing_workspace";
        view.stages[0].status = SmartSlicingStageStatus::Active;
        view.legacy_steps[0]  = LegacyAIWorkflowStatus::Running;
        break;
    case WorkflowState::Preflighting:
        view.summary_key = "inspecting_printability";
        complete_through(0);
        view.stages[1].status = SmartSlicingStageStatus::Active;
        view.legacy_steps[0]  = LegacyAIWorkflowStatus::Success;
        view.legacy_steps[1]  = LegacyAIWorkflowStatus::Running;
        break;
    case WorkflowState::AwaitingRiskDecision:
        view.summary_key = "printability_action_required";
        complete_through(0);
        view.stages[1].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps     = {LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Warning, LegacyAIWorkflowStatus::Success,
                                 LegacyAIWorkflowStatus::Waiting, LegacyAIWorkflowStatus::Waiting, LegacyAIWorkflowStatus::Waiting};
        break;
    case WorkflowState::ReadyForCandidatePlanning: {
        const bool needs_attention = snapshot.report && snapshot.report->readiness == AI::SmartSlicing::Readiness::NeedsAttention;
        if (needs_attention) {
            view.summary_key = "preflight_complete_with_warnings";
            complete_through(0);
            view.stages[1].status = SmartSlicingStageStatus::NeedsAttention;
        } else {
            view.summary_key = "preflight_complete";
            complete_through(1);
        }
        view.stages[2].status = SmartSlicingStageStatus::Disabled;
        view.stages[3].status = SmartSlicingStageStatus::Disabled;
        view.legacy_steps     = {LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Success,
                                 LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Waiting, LegacyAIWorkflowStatus::Waiting};
        if (needs_attention)
            view.legacy_steps[1] = LegacyAIWorkflowStatus::Warning;
        break;
    }
    case WorkflowState::Canceling: view.summary_key = "canceling"; break;
    case WorkflowState::Canceled:
        view.summary_key = "canceled";
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Warning);
        break;
    case WorkflowState::Stale:
        view.summary_key      = "workspace_changed";
        view.is_stale         = true;
        view.stages[0].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Warning);
        break;
    case WorkflowState::Failed:
        view.summary_key      = "preflight_failed";
        view.stages[0].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Failed);
        break;
    }
    return view;
}

} // namespace Slic3r::GUI
