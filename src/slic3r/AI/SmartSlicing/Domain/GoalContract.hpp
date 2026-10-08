#pragma once

#include "RecommendationTypes.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* GOAL_CONTRACT_VERSION = "smart-slicing-goal-contract/v1";
inline constexpr const char* CANDIDATE_SEARCH_BUDGET_VERSION = "candidate-search-budget/v1";

struct CandidateSearchBudget
{
    std::string version{CANDIDATE_SEARCH_BUDGET_VERSION};
    size_t maximum_orientations_per_object{4};
    size_t beam_width{8};
    size_t maximum_static_drafts_per_goal{6};
    size_t maximum_trial_selected_per_goal{3};
    size_t baseline_trial_slots{1};
    size_t maximum_total_trial_slots{10};
};

struct GoalWeights
{
    double quality{0.0};
    double reliability{0.0};
    double time{0.0};
    double material_and_multicolor_waste{0.0};
};

struct GoalContract
{
    std::string version{GOAL_CONTRACT_VERSION};
    RecommendationGoal goal{RecommendationGoal::Balanced};
    double minimum_strength_ratio{0.95};
    std::optional<double> minimum_time_reduction_ratio;
    double maximum_time_ratio{1.0};
    double maximum_material_ratio{1.0};
    std::optional<double> critical_surface_maximum_time_ratio;
    std::optional<double> critical_surface_maximum_material_ratio;
    std::optional<std::string> critical_surface_relaxation_explanation_code;
    int general_minimum_wall_loops{2};
    double general_minimum_infill_percent{15.0};
    std::optional<double> decoration_minimum_infill_percent;
    std::optional<double> decoration_maximum_infill_percent;
    std::vector<double> preferred_layer_heights_mm;
    GoalWeights weights;
    std::vector<std::string> common_hard_gate_codes;
    std::vector<std::string> required_evidence_codes;
    std::vector<std::string> explanation_codes;
};

const CandidateSearchBudget& candidate_search_budget();
const GoalContract& goal_contract(RecommendationGoal goal);
const std::array<GoalContract, RECOMMENDATION_GOALS.size()>& goal_contracts();

} // namespace Slic3r::AI::SmartSlicing
