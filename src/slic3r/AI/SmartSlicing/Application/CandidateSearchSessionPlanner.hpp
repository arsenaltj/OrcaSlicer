#pragma once

#include "CandidateSearchPipeline.hpp"
#include "RecommendationSessionCoordinator.hpp"

#include <array>
#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct CandidateSearchSessionPlan
{
    StartRecommendationSessionCommand start_command;
    std::string trial_cost_policy_version;
    RecommendationTaskIdentity baseline_task;
    std::array<RecommendationTaskIdentity, RECOMMENDATION_GOALS.size()> goal_tasks;
};

struct CandidateSearchSessionPlanResult
{
    std::optional<CandidateSearchSessionPlan> plan;
    std::string diagnostic_code;

    bool accepted() const { return plan.has_value(); }
};

class CandidateSearchSessionPlanner
{
public:
    CandidateSearchSessionPlanResult plan(const CandidateSearchResult& search_result,
                                          WorkflowId workflow_id,
                                          AttemptId attempt_id) const;
};

bool supersede_active_candidate_search_session(RecommendationSessionCoordinator& coordinator);

} // namespace Slic3r::AI::SmartSlicing
