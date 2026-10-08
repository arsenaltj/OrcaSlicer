#include "SmartSlicingViewModel.hpp"

#include <algorithm>

namespace Slic3r::GUI {

namespace {

SmartSlicingGoalState goal_state(AI::SmartSlicing::GoalResultStatus status)
{
    using AI::SmartSlicing::GoalResultStatus;
    switch (status) {
    case GoalResultStatus::Ready: return SmartSlicingGoalState::Ready;
    case GoalResultStatus::Unavailable: return SmartSlicingGoalState::Unavailable;
    case GoalResultStatus::Failed: return SmartSlicingGoalState::Failed;
    case GoalResultStatus::Stale: return SmartSlicingGoalState::Stale;
    case GoalResultStatus::Applied: return SmartSlicingGoalState::Applied;
    case GoalResultStatus::Analyzing: return SmartSlicingGoalState::Analyzing;
    }
    return SmartSlicingGoalState::Analyzing;
}

SmartSlicingGoalState workflow_goal_state(const AI::SmartSlicing::WorkflowSnapshot& snapshot,
                                          SmartSlicingGoalState fallback)
{
    using AI::SmartSlicing::WorkflowState;
    switch (snapshot.state) {
    case WorkflowState::OfficialSlicing: return SmartSlicingGoalState::OfficialSlicing;
    case WorkflowState::ApplyFailed: return SmartSlicingGoalState::ApplyFailed;
    case WorkflowState::Completed: return SmartSlicingGoalState::Applied;
    case WorkflowState::Stale: return SmartSlicingGoalState::Stale;
    case WorkflowState::Failed: return SmartSlicingGoalState::Failed;
    default: return fallback;
    }
}

void fill_goal_actions(SmartSlicingGoalView& view, const AI::SmartSlicing::WorkflowSnapshot& snapshot)
{
    view.actions.can_view_details = !view.diagnostic_codes.empty() || view.evidence.has_value() ||
                                    !view.candidate_id.empty();
    view.actions.can_select = view.state == SmartSlicingGoalState::Ready &&
                              snapshot.state == AI::SmartSlicing::WorkflowState::ReadyToApply;
    view.actions.can_apply = view.actions.can_select && !view.candidate_id.empty();
    view.actions.can_retry_slice = view.state == SmartSlicingGoalState::ApplyFailed;
    view.actions.can_undo = snapshot.can_undo_apply &&
                            (view.state == SmartSlicingGoalState::Applied ||
                             view.state == SmartSlicingGoalState::ApplyFailed);
    view.actions.can_reanalyze = view.state == SmartSlicingGoalState::Unavailable ||
                                 view.state == SmartSlicingGoalState::Failed ||
                                 view.state == SmartSlicingGoalState::Stale;
}

} // namespace

SmartSlicingViewModel SmartSlicingViewModel::from_snapshot(const AI::SmartSlicing::WorkflowSnapshot& snapshot)
{
    using AI::SmartSlicing::WorkflowState;
    SmartSlicingViewModel view;
    view.can_start  = snapshot.can_start();
    view.can_cancel = snapshot.can_cancel();
    view.can_plan_candidates = snapshot.state == WorkflowState::ReadyForCandidatePlanning;
    view.can_apply = snapshot.state == WorkflowState::ReadyToApply && !snapshot.selected_candidate_id.empty();
    view.can_undo_apply = (snapshot.state == WorkflowState::ApplyFailed || snapshot.state == WorkflowState::Completed) && snapshot.can_undo_apply;
    view.can_recheck = snapshot.state == WorkflowState::AwaitingRiskDecision;
    view.has_report = snapshot.report.has_value() && snapshot.state != WorkflowState::Canceled && snapshot.state != WorkflowState::Stale;
    view.can_add_model = snapshot.state == WorkflowState::Idle ||
        (view.can_recheck && snapshot.context && snapshot.context->objects.empty());
    view.needs_polling = snapshot.state == WorkflowState::OfficialSlicing ||
        snapshot.state == WorkflowState::Completed || snapshot.state == WorkflowState::ApplyFailed ||
        (snapshot.state == WorkflowState::Failed && snapshot.context.has_value());
    view.detail     = snapshot.detail;
    view.native_baseline_available = !snapshot.candidates.empty();
    if (view.native_baseline_available) {
        const AI::SmartSlicing::SliceCandidate& baseline = snapshot.candidates.front();
        view.baseline.candidate_id = baseline.id.empty() ? "baseline" : baseline.id;
        view.baseline.summary_key = "native_baseline";
        if (baseline.metrics) {
            view.baseline.estimated_time_seconds = baseline.metrics->estimated_time_seconds;
            view.baseline.filament_volume_mm3 = baseline.metrics->filament_volume_mm3;
        }
    }
    if (snapshot.recommendation) {
        view.has_recommendation_contract = true;
        view.recommendation_contract_version = snapshot.recommendation->contract_version;
        view.baseline.candidate_id = snapshot.recommendation->baseline.candidate_id;
        view.baseline.status = snapshot.recommendation->baseline.status;
        view.baseline.diagnostic_codes = snapshot.recommendation->baseline.diagnostic_codes;
        view.native_baseline_available = !view.baseline.candidate_id.empty() || view.baseline.status !=
            AI::SmartSlicing::GoalResultStatus::Analyzing;
        for (size_t index = 0; index < AI::SmartSlicing::RECOMMENDATION_GOALS.size(); ++index) {
            const AI::SmartSlicing::RecommendationGoal goal = AI::SmartSlicing::RECOMMENDATION_GOALS[index];
            const AI::SmartSlicing::GoalResult& result = snapshot.recommendation->goal_result(goal);
            view.goal_results[index].goal_id = AI::SmartSlicing::recommendation_goal_id(goal);
            view.goal_results[index].task_candidate_id = result.candidate_id;
            view.goal_results[index].candidate_id = result.selected_candidate_id;
            view.goal_results[index].status = result.status;
            view.goal_results[index].state = workflow_goal_state(snapshot, goal_state(result.status));
            view.goal_results[index].diagnostic_codes = result.diagnostic_codes;
            if (result.status == AI::SmartSlicing::GoalResultStatus::Ready ||
                result.status == AI::SmartSlicing::GoalResultStatus::Applied ||
                result.status == AI::SmartSlicing::GoalResultStatus::Stale)
                view.goal_results[index].evidence = result.evidence;
            fill_goal_actions(view.goal_results[index], snapshot);
        }
    } else {
        for (SmartSlicingGoalView& goal : view.goal_results) {
            goal.state = workflow_goal_state(snapshot, SmartSlicingGoalState::Analyzing);
            fill_goal_actions(goal, snapshot);
        }
    }
    if (view.has_report) {
        view.issue_count = snapshot.report->issues.size();
        view.issues.reserve(snapshot.report->issues.size());
        for (const AI::SmartSlicing::PrintabilityIssue& issue : snapshot.report->issues)
            view.issues.emplace_back(AI::SmartSlicing::issue_code_name(issue.code), issue.evidence);
    }
    const AI::SmartSlicing::SlicingMetrics* baseline_metrics =
        !snapshot.candidates.empty() && snapshot.candidates.front().metrics ? &*snapshot.candidates.front().metrics : nullptr;
    view.candidates.reserve(snapshot.candidates.size());
    for (const AI::SmartSlicing::SliceCandidate& candidate : snapshot.candidates) {
        SmartSlicingCandidateView card;
        card.id              = candidate.id;
        card.explanation     = candidate.explanation;
        card.parameter_changes = candidate.parameters.entries;
        card.placement_change_count = candidate.placement.transforms.size();
        card.diagnostic_code = candidate.diagnostic_code;
        card.recommended     = snapshot.comparison && snapshot.comparison->recommended_candidate_id == candidate.id;
        card.selected        = snapshot.selected_candidate_id == candidate.id;
        card.failed          = candidate.status == AI::SmartSlicing::CandidateStatus::Failed;
        if (card.failed)
            card.diagnostic_message = candidate.diagnostic_message;
        card.can_retry       = snapshot.state == WorkflowState::ReadyToApply && card.failed;
        card.can_select      = snapshot.state == WorkflowState::ReadyToApply && !card.failed;
        if (card.recommended && snapshot.comparison)
            card.evidence_codes = snapshot.comparison->recommendation_evidence_codes;
        if (candidate.metrics) {
            const auto& metrics = *candidate.metrics;
            card.estimated_time_seconds = metrics.estimated_time_seconds;
            card.filament_volume_mm3    = metrics.filament_volume_mm3;
            card.support_volume_mm3     = metrics.support_volume_mm3;
            card.flush_volume_mm3       = metrics.flush_volume_mm3;
            card.wipe_tower_volume_mm3  = metrics.wipe_tower_volume_mm3;
            card.tool_changes           = metrics.tool_changes;
            card.physical_slots_compatible = metrics.physical_slots_compatible;
            card.color_mapping_degraded    = metrics.color_mapping_degraded;
            card.prime_tower_enabled       = metrics.prime_tower_enabled;
            card.layer_tool_sequence_count = metrics.layer_tool_sequences.size();
            if (baseline_metrics != nullptr) {
                if (metrics.estimated_time_seconds && baseline_metrics->estimated_time_seconds)
                    card.time_delta_seconds = *metrics.estimated_time_seconds - *baseline_metrics->estimated_time_seconds;
                if (metrics.filament_volume_mm3 && baseline_metrics->filament_volume_mm3)
                    card.filament_delta_mm3 = *metrics.filament_volume_mm3 - *baseline_metrics->filament_volume_mm3;
                if (metrics.support_volume_mm3 && baseline_metrics->support_volume_mm3)
                    card.support_delta_mm3 = *metrics.support_volume_mm3 - *baseline_metrics->support_volume_mm3;
                if (metrics.flush_volume_mm3 && baseline_metrics->flush_volume_mm3)
                    card.flush_delta_mm3 = *metrics.flush_volume_mm3 - *baseline_metrics->flush_volume_mm3;
                if (metrics.wipe_tower_volume_mm3 && baseline_metrics->wipe_tower_volume_mm3)
                    card.wipe_tower_delta_mm3 = *metrics.wipe_tower_volume_mm3 - *baseline_metrics->wipe_tower_volume_mm3;
                if (metrics.tool_changes && baseline_metrics->tool_changes)
                    card.tool_change_delta = static_cast<long long>(*metrics.tool_changes) -
                                             static_cast<long long>(*baseline_metrics->tool_changes);
            }
        }
        view.candidates.push_back(std::move(card));
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
        if (!snapshot.context || snapshot.context->objects.empty() || snapshot.context->printer_preset_id.empty() ||
            snapshot.context->process_preset_id.empty() || snapshot.context->materials.empty())
            view.stages[0].status = SmartSlicingStageStatus::NeedsAttention;
        view.stages[1].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps     = {LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Warning, LegacyAIWorkflowStatus::Success,
                                 LegacyAIWorkflowStatus::Waiting, LegacyAIWorkflowStatus::Waiting, LegacyAIWorkflowStatus::Waiting};
        break;
    case WorkflowState::ReadyForCandidatePlanning: {
        // A missing formal slice is expected at this stage. Keep the deferred
        // native check visible, but do not mark the step as needing user repair.
        const bool needs_attention = snapshot.report &&
            snapshot.report->readiness == AI::SmartSlicing::Readiness::NeedsAttention &&
            std::any_of(snapshot.report->issues.begin(), snapshot.report->issues.end(), [](const auto& issue) {
                return issue.code != AI::SmartSlicing::IssueCode::NativeValidationUnavailable;
            });
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
    case WorkflowState::PlanningCandidates:
        view.summary_key = "planning_candidates";
        complete_through(1);
        view.stages[2].status = SmartSlicingStageStatus::Active;
        break;
    case WorkflowState::TrialSlicingBaseline:
        view.summary_key = "trial_slicing_baseline";
        complete_through(1);
        view.stages[2].status = SmartSlicingStageStatus::Active;
        break;
    case WorkflowState::TrialSlicingCandidates:
        view.summary_key = "trial_slicing_candidates";
        complete_through(1);
        view.stages[2].status = SmartSlicingStageStatus::Active;
        break;
    case WorkflowState::ReadyToApply:
        view.summary_key = "candidates_ready";
        complete_through(2);
        view.stages[3].status = SmartSlicingStageStatus::Waiting;
        break;
    case WorkflowState::Applying:
        view.summary_key = "applying_candidate";
        complete_through(2);
        view.stages[3].status = SmartSlicingStageStatus::Active;
        break;
    case WorkflowState::OfficialSlicing:
        view.summary_key = "official_slicing";
        complete_through(2);
        view.stages[3].status = SmartSlicingStageStatus::Active;
        break;
    case WorkflowState::Completed:
        view.summary_key = "official_slice_complete";
        complete_through(3);
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Success);
        break;
    case WorkflowState::ApplyFailed:
        view.summary_key = "official_slice_failed";
        complete_through(2);
        view.stages[3].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Failed);
        break;
    case WorkflowState::Canceling: view.summary_key = "canceling"; break;
    case WorkflowState::Canceled:
        view.summary_key = "canceled";
        view.candidates.clear();
        view.issue_count = 0;
        view.issues.clear();
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Warning);
        break;
    case WorkflowState::Stale:
        view.candidates.clear();
        view.summary_key = snapshot.detail == "apply_undo_unavailable" ? "apply_undo_unavailable" :
            snapshot.detail == "applied_revision_unavailable" ? "applied_revision_unavailable" : "workspace_changed";
        view.is_stale         = true;
        view.stages[0].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Warning);
        break;
    case WorkflowState::Failed:
        if (snapshot.detail == "baseline_trial_failed") {
            view.summary_key = "baseline_trial_failed";
            complete_through(1);
            view.stages[2].status = SmartSlicingStageStatus::NeedsAttention;
            view.legacy_steps = {LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Success,
                                 LegacyAIWorkflowStatus::Success, LegacyAIWorkflowStatus::Success,
                                 LegacyAIWorkflowStatus::Failed, LegacyAIWorkflowStatus::Waiting};
            break;
        }
        view.summary_key      = snapshot.detail == "interrupted_workflow_recovered" ?
                                    "interrupted_workflow_recovered" : "preflight_failed";
        view.stages[0].status = SmartSlicingStageStatus::NeedsAttention;
        view.legacy_steps.fill(LegacyAIWorkflowStatus::Failed);
        break;
    }
    if (snapshot.detail == "apply_undo_failed")
        view.summary_key = "apply_undo_failed";
    else if (snapshot.detail == "close_active_model_tool")
        view.summary_key = "close_active_model_tool";
    return view;
}

} // namespace Slic3r::GUI
