#include "TrialSliceScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

size_t goal_index(RecommendationGoal goal)
{
    switch (goal) {
    case RecommendationGoal::Balanced: return 0;
    case RecommendationGoal::Speed: return 1;
    case RecommendationGoal::Quality: return 2;
    }
    return RECOMMENDATION_GOALS.size();
}

bool same_identity(const RecommendationTaskIdentity& lhs, const RecommendationTaskIdentity& rhs)
{
    return lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.workspace_revision == rhs.workspace_revision && lhs.candidate_id == rhs.candidate_id &&
           lhs.goal_id == rhs.goal_id;
}

RecommendationTaskResult terminal_result(const RecommendationTaskIdentity& identity,
                                         RecommendationTaskOutcome outcome,
                                         std::string diagnostic)
{
    RecommendationTaskResult result;
    result.identity = identity;
    result.outcome = outcome;
    result.diagnostic_codes.push_back(std::move(diagnostic));
    return result;
}

bool valid_plan(const TrialSliceSchedulerInput& input)
{
    const auto& start = input.session_plan.start_command;
    if (start.workflow_id == 0 || start.attempt_id == 0 || !start.workspace_revision.valid() ||
        start.baseline_candidate_id.empty() || start.strategy_version.empty() ||
        input.search_result.budget_version != start.strategy_version ||
        input.search_result.trial_cost_policy_version != CANDIDATE_TRIAL_COST_POLICY_VERSION ||
        input.session_plan.trial_cost_policy_version != input.search_result.trial_cost_policy_version ||
        input.search_result.baseline.candidate_id != start.baseline_candidate_id ||
        input.search_result.baseline.workspace_revision != start.workspace_revision ||
        input.search_result.baseline.trial_slots != 1 ||
        !same_identity(input.session_plan.baseline_task,
                       {start.workflow_id, start.attempt_id, start.workspace_revision,
                        start.baseline_candidate_id, BASELINE_GOAL_ID}))
        return false;

    size_t total = 1;
    std::set<CandidateId> candidates{start.baseline_candidate_id};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        const GoalCandidateDrafts& drafts = input.search_result.goal(goal);
        if (drafts.goal != goal || drafts.selected_for_trial.empty() ||
            drafts.selected_for_trial.size() > TRIAL_SLICE_PER_GOAL_LIMIT ||
            input.session_plan.goal_tasks[index].workflow_id != start.workflow_id ||
            input.session_plan.goal_tasks[index].attempt_id != start.attempt_id ||
            input.session_plan.goal_tasks[index].workspace_revision != start.workspace_revision ||
            input.session_plan.goal_tasks[index].candidate_id != start.goal_candidate_ids[index] ||
            input.session_plan.goal_tasks[index].goal_id != recommendation_goal_id(goal))
            return false;
        total += drafts.selected_for_trial.size();
        for (const CandidateSearchDraft& draft : drafts.selected_for_trial) {
            if (draft.goal != goal || draft.status != CandidateStatus::Draft ||
                draft.candidate_id.empty() || !std::isfinite(draft.estimated_trial_cost) ||
                draft.estimated_trial_cost < 0.0 || !candidates.insert(draft.candidate_id).second)
                return false;
        }
    }
    return total <= TRIAL_SLICE_TOTAL_LIMIT && input.search_result.total_trial_slots == total;
}

void publish_remaining(const TrialSliceSchedulerInput& input,
                       const std::array<bool, RECOMMENDATION_GOALS.size()>& terminal,
                       RecommendationTaskOutcome outcome,
                       const char* diagnostic,
                       const RecommendationResultPublisher& publish)
{
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index)
        if (!terminal[index])
            publish(terminal_result(input.session_plan.goal_tasks[index], outcome, diagnostic));
}

} // namespace

TrialSliceScheduler::TrialSliceScheduler()
    : TrialSliceScheduler([] { return std::chrono::steady_clock::now(); })
{}

TrialSliceScheduler::TrialSliceScheduler(Clock clock) : m_clock(std::move(clock)) {}

TrialSliceSchedulerReport TrialSliceScheduler::run(const TrialSliceSchedulerInput& input,
                                                   const VersionedTrialExecute& execute,
                                                   const RecommendationResultPublisher& publish) const
{
    TrialSliceSchedulerReport report;
    if (!m_clock || !execute || !publish || !input.cancellation_token || !valid_plan(input)) {
        report.diagnostic_codes.emplace_back("trial_scheduler_input_invalid");
        return report;
    }

    std::array<bool, RECOMMENDATION_GOALS.size()> terminal{};
    if (input.machine_support_status != MachineSupportStatus::Enabled) {
        report.status = TrialSliceSchedulerStatus::Blocked;
        const char* diagnostic = input.machine_support_status == MachineSupportStatus::PendingValidation ?
                                     "machine_capability_pending_validation" : "machine_capability_disabled";
        publish(terminal_result(input.session_plan.baseline_task,
                                RecommendationTaskOutcome::Unavailable, diagnostic));
        publish_remaining(input, terminal, RecommendationTaskOutcome::Unavailable, diagnostic, publish);
        report.diagnostic_codes.emplace_back(diagnostic);
        return report;
    }

    const auto canceled = [&] { return input.cancellation_token->cancellation_requested(); };
    const auto deadline_due = [&] { return m_clock() >= input.deadline; };
    if (canceled()) {
        report.status = TrialSliceSchedulerStatus::Canceled;
        return report;
    }
    if (deadline_due()) {
        report.status = TrialSliceSchedulerStatus::DeadlineExceeded;
        publish(terminal_result(input.session_plan.baseline_task,
                                RecommendationTaskOutcome::Unavailable, "deadline_exceeded"));
        publish_remaining(input, terminal, RecommendationTaskOutcome::Unavailable,
                          "deadline_exceeded", publish);
        return report;
    }

    const auto& start = input.session_plan.start_command;
    TrialSliceTask baseline_task;
    baseline_task.identity = input.session_plan.baseline_task;
    baseline_task.baseline_candidate_id = start.baseline_candidate_id;
    baseline_task.strategy_version = start.strategy_version;
    baseline_task.deadline = input.deadline;
    ++report.total_execution_count;
    ++report.baseline_execution_count;
    report.maximum_concurrency = 1;
    report.execution_order.push_back(baseline_task.identity.candidate_id);
    const VersionedTrialSliceResult baseline_result = execute(baseline_task, input.cancellation_token);
    if (canceled() || baseline_result.status == TrialSliceStatus::Canceled) {
        report.status = TrialSliceSchedulerStatus::Canceled;
        return report;
    }
    if (deadline_due()) {
        report.status = TrialSliceSchedulerStatus::DeadlineExceeded;
        publish(terminal_result(input.session_plan.baseline_task,
                                RecommendationTaskOutcome::Unavailable, "deadline_exceeded"));
        publish_remaining(input, terminal, RecommendationTaskOutcome::Unavailable,
                          "deadline_exceeded", publish);
        return report;
    }
    if (!same_identity(baseline_result.identity, baseline_task.identity) ||
        baseline_result.status != TrialSliceStatus::Succeeded || !baseline_result.metrics ||
        !baseline_result.risks || !normalize_trial_metrics(*baseline_result.metrics).valid() ||
        !normalize_risk_assessment(*baseline_result.risks).valid()) {
        publish(terminal_result(input.session_plan.baseline_task,
                                RecommendationTaskOutcome::Failed, "baseline_trial_failed"));
        publish_remaining(input, terminal, RecommendationTaskOutcome::Failed,
                          "baseline_trial_failed", publish);
        report.status = TrialSliceSchedulerStatus::Completed;
        report.diagnostic_codes.emplace_back("baseline_trial_failed");
        return report;
    }
    publish(terminal_result(input.session_plan.baseline_task,
                            RecommendationTaskOutcome::Ready, "baseline_trial_succeeded"));

    std::vector<TrialSliceTask> tasks;
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        for (const CandidateSearchDraft& draft : input.search_result.goal(goal).selected_for_trial) {
            TrialSliceTask task;
            task.identity = {start.workflow_id, start.attempt_id, start.workspace_revision,
                             draft.candidate_id, recommendation_goal_id(goal)};
            task.baseline_candidate_id = start.baseline_candidate_id;
            task.strategy_version = start.strategy_version;
            task.goal = goal;
            task.estimated_cost = draft.estimated_trial_cost;
            task.placement = draft.placement;
            task.parameters = draft.parameters;
            task.deadline = input.deadline;
            tasks.push_back(std::move(task));
        }
    }
    std::sort(tasks.begin(), tasks.end(), [](const TrialSliceTask& lhs, const TrialSliceTask& rhs) {
        return std::tuple{lhs.estimated_cost, goal_index(*lhs.goal), lhs.identity.candidate_id} <
               std::tuple{rhs.estimated_cost, goal_index(*rhs.goal), rhs.identity.candidate_id};
    });

    std::array<std::vector<EvaluatedCandidate>, RECOMMENDATION_GOALS.size()> evaluated;
    std::array<size_t, RECOMMENDATION_GOALS.size()> remaining{};
    std::array<bool, RECOMMENDATION_GOALS.size()> contract_failure{};
    std::array<bool, RECOMMENDATION_GOALS.size()> execution_failure{};
    for (const TrialSliceTask& task : tasks)
        ++remaining[goal_index(*task.goal)];

    for (const TrialSliceTask& task : tasks) {
        const size_t index = goal_index(*task.goal);
        if (terminal[index]) {
            --remaining[index];
            continue;
        }
        if (canceled()) {
            report.status = TrialSliceSchedulerStatus::Canceled;
            return report;
        }
        if (deadline_due()) {
            report.status = TrialSliceSchedulerStatus::DeadlineExceeded;
            publish_remaining(input, terminal, RecommendationTaskOutcome::Unavailable,
                              "deadline_exceeded", publish);
            return report;
        }

        ++report.total_execution_count;
        ++report.goal_execution_counts[index];
        report.maximum_concurrency = 1;
        report.execution_order.push_back(task.identity.candidate_id);
        const VersionedTrialSliceResult execution = execute(task, input.cancellation_token);
        --remaining[index];
        if (canceled() || execution.status == TrialSliceStatus::Canceled) {
            report.status = TrialSliceSchedulerStatus::Canceled;
            return report;
        }
        if (deadline_due()) {
            report.status = TrialSliceSchedulerStatus::DeadlineExceeded;
            publish_remaining(input, terminal, RecommendationTaskOutcome::Unavailable,
                              "deadline_exceeded", publish);
            return report;
        }
        if (!same_identity(execution.identity, task.identity)) {
            contract_failure[index] = true;
        } else if (execution.status == TrialSliceStatus::Failed) {
            execution_failure[index] = true;
        } else if (execution.status == TrialSliceStatus::Succeeded &&
                   (!execution.metrics || !execution.risks ||
                    !normalize_trial_metrics(*execution.metrics).valid() ||
                    !normalize_risk_assessment(*execution.risks).valid())) {
            contract_failure[index] = true;
        } else if (execution.status == TrialSliceStatus::Succeeded) {
            CandidateEvaluationInput evaluation_input;
            evaluation_input.goal = *task.goal;
            evaluation_input.usage_purpose = input.usage_purpose;
            evaluation_input.baseline_metrics = *baseline_result.metrics;
            evaluation_input.candidate_metrics = *execution.metrics;
            evaluation_input.baseline_risks = *baseline_result.risks;
            evaluation_input.candidate_risks = *execution.risks;
            evaluation_input.manual_intent_preserved = execution.evaluation_facts.manual_intent_preserved;
            evaluation_input.effective_wall_loops = execution.evaluation_facts.effective_wall_loops;
            evaluation_input.effective_infill_percent = execution.evaluation_facts.effective_infill_percent;
            evaluation_input.critical_surface_significantly_improved =
                execution.evaluation_facts.critical_surface_significantly_improved;

            CandidateSelectionBinding binding;
            binding.workflow_id = start.workflow_id;
            binding.attempt_id = start.attempt_id;
            binding.workspace_revision = start.workspace_revision;
            binding.baseline_candidate_id = start.baseline_candidate_id;
            binding.goal_task_candidate_id = input.session_plan.goal_tasks[index].candidate_id;
            binding.strategy_version = start.strategy_version;
            EvaluatedCandidate candidate;
            candidate.binding = binding;
            candidate.candidate_id = task.identity.candidate_id;
            candidate.evaluation_input = std::move(evaluation_input);
            candidate.evaluation = evaluate_candidate(candidate.evaluation_input);
            evaluated[index].push_back(std::move(candidate));

            CandidateSelectionInput selection;
            selection.binding = binding;
            selection.goal = *task.goal;
            selection.baseline_metrics = *baseline_result.metrics;
            selection.baseline_risks = *baseline_result.risks;
            selection.candidates = evaluated[index];
            CandidateSelectionResult selected = select_candidate(selection);
            if (selected.status == CandidateSelectionStatus::Ready) {
                CandidateSelectionTaskContext context{input.session_plan.goal_tasks[index],
                                                      start.baseline_candidate_id,
                                                      start.strategy_version};
                CandidateSelectionTaskMappingResult mapped =
                    map_candidate_selection_task(selected, context);
                if (mapped.valid()) {
                    publish(std::move(*mapped.task_result));
                    terminal[index] = true;
                } else {
                    contract_failure[index] = true;
                }
            }
        }

        if (!terminal[index] && remaining[index] == 0) {
            if (contract_failure[index]) {
                publish(terminal_result(input.session_plan.goal_tasks[index],
                                        RecommendationTaskOutcome::Failed,
                                        "trial_result_contract_invalid"));
            } else if (evaluated[index].empty() && execution_failure[index]) {
                publish(terminal_result(input.session_plan.goal_tasks[index],
                                        RecommendationTaskOutcome::Failed,
                                        "trial_slice_execution_failed"));
            } else {
                CandidateSelectionInput selection;
                selection.binding = {start.workflow_id, start.attempt_id, start.workspace_revision,
                                     start.baseline_candidate_id,
                                     input.session_plan.goal_tasks[index].candidate_id,
                                     start.strategy_version};
                selection.goal = *task.goal;
                selection.baseline_metrics = *baseline_result.metrics;
                selection.baseline_risks = *baseline_result.risks;
                selection.candidates = evaluated[index];
                CandidateSelectionTaskContext context{input.session_plan.goal_tasks[index],
                                                      start.baseline_candidate_id,
                                                      start.strategy_version};
                CandidateSelectionTaskMappingResult mapped =
                    map_candidate_selection_task(select_candidate(selection), context);
                if (mapped.valid())
                    publish(std::move(*mapped.task_result));
                else
                    publish(terminal_result(input.session_plan.goal_tasks[index],
                                            RecommendationTaskOutcome::Failed,
                                            "candidate_selection_mapping_failed"));
            }
            terminal[index] = true;
        }
    }

    report.status = TrialSliceSchedulerStatus::Completed;
    return report;
}

} // namespace Slic3r::AI::SmartSlicing
