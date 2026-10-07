#include "GoalContract.hpp"

#include <stdexcept>

namespace Slic3r::AI::SmartSlicing {
namespace {

std::vector<std::string> common_gates()
{
    return {
        "native_validation_required",
        "material_and_slot_compatibility_required",
        "human_intent_preserved",
        "profile_bounds_preserved",
        "known_appearance_not_worse",
        "minimum_strength_ratio_0_95",
        "missing_evidence_remains_unknown",
    };
}

} // namespace

const CandidateSearchBudget& candidate_search_budget()
{
    static const CandidateSearchBudget budget;
    return budget;
}

const std::array<GoalContract, RECOMMENDATION_GOALS.size()>& goal_contracts()
{
    static const std::array<GoalContract, RECOMMENDATION_GOALS.size()> contracts = [] {
        std::array<GoalContract, RECOMMENDATION_GOALS.size()> result;

        result[0].goal = RecommendationGoal::Balanced;
        result[0].maximum_time_ratio = 1.15;
        result[0].maximum_material_ratio = 1.10;
        result[0].critical_surface_maximum_time_ratio = 1.25;
        result[0].critical_surface_maximum_material_ratio = 1.15;
        result[0].critical_surface_relaxation_explanation_code =
            "balanced_critical_surface_resource_relaxation";
        result[0].preferred_layer_heights_mm = {0.20, 0.16};
        result[0].weights = {0.45, 0.25, 0.20, 0.10};
        result[0].common_hard_gate_codes = common_gates();
        result[0].required_evidence_codes = {"quality_proxy", "reliability_proxy", "time", "material"};
        result[0].explanation_codes = {"balanced_quality_reliability_time_material"};

        result[1].goal = RecommendationGoal::Speed;
        result[1].minimum_time_reduction_ratio = 0.10;
        result[1].maximum_time_ratio = 0.90;
        result[1].maximum_material_ratio = 1.00;
        result[1].decoration_minimum_infill_percent = 10.0;
        result[1].decoration_maximum_infill_percent = 12.0;
        result[1].preferred_layer_heights_mm = {0.24, 0.28};
        result[1].common_hard_gate_codes = common_gates();
        result[1].required_evidence_codes = {"estimated_time", "known_feature_risk", "strength_proxy"};
        result[1].explanation_codes = {"speed_estimated_time_reduction_at_least_10_percent"};

        result[2].goal = RecommendationGoal::Quality;
        result[2].maximum_time_ratio = 2.00;
        result[2].maximum_material_ratio = 1.20;
        result[2].preferred_layer_heights_mm = {0.12, 0.16};
        result[2].common_hard_gate_codes = common_gates();
        result[2].required_evidence_codes = {"appearance_proxy", "dimension_proxy", "strength_proxy"};
        result[2].explanation_codes = {"quality_appearance_dimension_strength"};

        return result;
    }();
    return contracts;
}

const GoalContract& goal_contract(RecommendationGoal goal)
{
    switch (goal) {
    case RecommendationGoal::Balanced: return goal_contracts()[0];
    case RecommendationGoal::Speed: return goal_contracts()[1];
    case RecommendationGoal::Quality: return goal_contracts()[2];
    }
    throw std::invalid_argument("unsupported recommendation goal");
}

} // namespace Slic3r::AI::SmartSlicing
