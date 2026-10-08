#include "RecommendationWorkerFailure.hpp"

namespace Slic3r::AI::SmartSlicing {
namespace {

bool same_identity(const RecommendationTaskIdentity& lhs, const RecommendationTaskIdentity& rhs)
{
    return lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.workspace_revision == rhs.workspace_revision && lhs.candidate_id == rhs.candidate_id &&
           lhs.goal_id == rhs.goal_id;
}

RecommendationTaskResult failed_result(const RecommendationTaskIdentity& identity)
{
    RecommendationTaskResult result;
    result.identity = identity;
    result.outcome = RecommendationTaskOutcome::Failed;
    result.diagnostic_codes = {RECOMMENDATION_WORKER_EXCEPTION_CODE};
    return result;
}

bool plan_matches_snapshot(const CandidateSearchSessionPlan& plan,
                           const RecommendationSessionSnapshot& snapshot)
{
    const StartRecommendationSessionCommand& start = plan.start_command;
    if ((snapshot.state != RecommendationSessionState::Recommending &&
         snapshot.state != RecommendationSessionState::Ready) ||
        snapshot.cancellation_reason != RecommendationCancellationReason::None ||
        start.workflow_id != snapshot.workflow_id || start.attempt_id != snapshot.attempt_id ||
        start.workspace_revision != snapshot.workspace_revision ||
        start.strategy_version != snapshot.strategy_version ||
        start.baseline_candidate_id != snapshot.recommendation.baseline.candidate_id ||
        !same_identity(plan.baseline_task,
                       {start.workflow_id, start.attempt_id, start.workspace_revision,
                        start.baseline_candidate_id, BASELINE_GOAL_ID}))
        return false;

    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        if (start.goal_candidate_ids[index] != snapshot.recommendation.goal_result(goal).candidate_id ||
            !same_identity(plan.goal_tasks[index],
                           {start.workflow_id, start.attempt_id, start.workspace_revision,
                            start.goal_candidate_ids[index], recommendation_goal_id(goal)}))
            return false;
    }
    return true;
}

} // namespace

size_t settle_recommendation_worker_exception(RecommendationSessionCoordinator& coordinator,
                                               const CandidateSearchSessionPlan& plan)
{
    const RecommendationSessionSnapshot& snapshot = coordinator.snapshot();
    if (!plan_matches_snapshot(plan, snapshot))
        return 0;

    size_t settled = 0;
    if (snapshot.recommendation.baseline.status == GoalResultStatus::Analyzing) {
        coordinator.enqueue(failed_result(plan.baseline_task));
        ++settled;
    }
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        if (snapshot.recommendation.goal_result(RECOMMENDATION_GOALS[index]).status !=
            GoalResultStatus::Analyzing)
            continue;
        coordinator.enqueue(failed_result(plan.goal_tasks[index]));
        ++settled;
    }
    if (settled != 0)
        coordinator.process_all();
    return settled;
}

} // namespace Slic3r::AI::SmartSlicing
