#include "CandidateSearchSessionPlanner.hpp"

#include <set>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

CandidateSearchSessionPlanResult rejected(std::string diagnostic)
{
    CandidateSearchSessionPlanResult result;
    result.diagnostic_code = std::move(diagnostic);
    return result;
}

RecommendationTaskIdentity task_identity(WorkflowId workflow_id, AttemptId attempt_id,
                                         const WorkspaceRevision& revision,
                                         const CandidateId& candidate_id,
                                         std::string goal_id)
{
    return {workflow_id, attempt_id, revision, candidate_id, std::move(goal_id)};
}

} // namespace

CandidateSearchSessionPlanResult CandidateSearchSessionPlanner::plan(
    const CandidateSearchResult& search_result, WorkflowId workflow_id, AttemptId attempt_id) const
{
    if (workflow_id == 0 || attempt_id == 0 ||
        !search_result.baseline.workspace_revision.valid() ||
        search_result.baseline.candidate_id.empty() || search_result.budget_version.empty() ||
        search_result.trial_cost_policy_version != CANDIDATE_TRIAL_COST_POLICY_VERSION)
        return rejected("candidate_session_identity_invalid");

    CandidateSearchSessionPlan plan;
    plan.start_command.workflow_id = workflow_id;
    plan.start_command.attempt_id = attempt_id;
    plan.start_command.workspace_revision = search_result.baseline.workspace_revision;
    plan.start_command.baseline_candidate_id = search_result.baseline.candidate_id;
    plan.start_command.strategy_version = search_result.budget_version;
    plan.trial_cost_policy_version = search_result.trial_cost_policy_version;
    plan.baseline_task = task_identity(workflow_id, attempt_id,
                                       search_result.baseline.workspace_revision,
                                       search_result.baseline.candidate_id,
                                       BASELINE_GOAL_ID);

    std::set<CandidateId> candidate_ids{search_result.baseline.candidate_id};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        const GoalCandidateDrafts& goal_result = search_result.goal(goal);
        if (goal_result.goal != goal || goal_result.selected_for_trial.empty())
            return rejected("candidate_session_goal_draft_unavailable");
        const CandidateSearchDraft& draft = goal_result.selected_for_trial.front();
        if (draft.goal != goal || draft.status != CandidateStatus::Draft ||
            draft.candidate_id.empty())
            return rejected("candidate_session_goal_draft_invalid");
        if (!candidate_ids.insert(draft.candidate_id).second)
            return rejected("candidate_session_candidate_identity_conflict");

        plan.start_command.goal_candidate_ids[index] = draft.candidate_id;
        plan.goal_tasks[index] = task_identity(
            workflow_id, attempt_id, search_result.baseline.workspace_revision,
            draft.candidate_id, recommendation_goal_id(goal));
    }

    CandidateSearchSessionPlanResult result;
    result.plan = std::move(plan);
    return result;
}

bool supersede_active_candidate_search_session(RecommendationSessionCoordinator& coordinator)
{
    const RecommendationSessionSnapshot& current = coordinator.snapshot();
    if (current.state != RecommendationSessionState::Recommending &&
        current.state != RecommendationSessionState::Ready)
        return false;
    const WorkflowId workflow_id = current.workflow_id;
    const AttemptId attempt_id = current.attempt_id;
    coordinator.enqueue(SupersedeRecommendationSessionCommand{workflow_id, attempt_id});
    coordinator.process_all();
    const RecommendationSessionSnapshot& superseded = coordinator.snapshot();
    return superseded.workflow_id == workflow_id && superseded.attempt_id == attempt_id &&
           superseded.state == RecommendationSessionState::Canceled;
}

} // namespace Slic3r::AI::SmartSlicing
