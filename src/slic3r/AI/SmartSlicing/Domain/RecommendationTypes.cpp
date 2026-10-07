#include "RecommendationTypes.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r::AI::SmartSlicing {
namespace {

size_t goal_index(RecommendationGoal goal)
{
    switch (goal) {
    case RecommendationGoal::Balanced: return 0;
    case RecommendationGoal::Speed: return 1;
    case RecommendationGoal::Quality: return 2;
    }
    throw std::invalid_argument("unsupported recommendation goal");
}

} // namespace

bool valid_recommendation_code(std::string_view code)
{
    return !code.empty() && code.size() <= 128 &&
           std::all_of(code.begin(), code.end(), [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_' ||
                      character == '-' || character == '.';
           });
}

std::vector<std::string> validate_recommendation_evidence(const RecommendationEvidence& evidence)
{
    std::vector<std::string> errors;
    if (evidence.selection_policy_version != CANDIDATE_SELECTION_POLICY_VERSION)
        errors.emplace_back("invalid_selection_policy_version");
    if (evidence.explanation_codes.empty() ||
        !std::all_of(evidence.explanation_codes.begin(), evidence.explanation_codes.end(),
                     [](const std::string& code) { return valid_recommendation_code(code); }))
        errors.emplace_back("invalid_explanation_codes");

    const auto validate_value = [&](const EvidenceValue<double>& value, const char* name,
                                    bool required, bool unit_interval) {
        switch (value.availability) {
        case EvidenceAvailability::Available:
            if (!value.value || !std::isfinite(*value.value) ||
                !valid_recommendation_code(value.source_code) ||
                (unit_interval && (*value.value < 0.0 || *value.value > 1.0)))
                errors.emplace_back(std::string("invalid_evidence_value:") + name);
            break;
        case EvidenceAvailability::Unavailable:
            if (required || value.value || !value.source_code.empty())
                errors.emplace_back(std::string("invalid_unavailable_evidence:") + name);
            break;
        case EvidenceAvailability::NotApplicable:
            if (required || value.value || !valid_recommendation_code(value.source_code))
                errors.emplace_back(std::string("invalid_not_applicable_evidence:") + name);
            break;
        default: errors.emplace_back(std::string("invalid_evidence_availability:") + name); break;
        }
    };

    validate_value(evidence.estimated_time_ratio, "estimated_time_ratio", true, false);
    validate_value(evidence.material_ratio, "material_ratio", true, false);
    validate_value(evidence.appearance_risk, "appearance_risk", true, true);
    validate_value(evidence.dimensional_risk, "dimensional_risk", false, true);
    validate_value(evidence.strength_risk, "strength_risk", true, true);
    validate_value(evidence.retained_strength_ratio, "retained_strength_ratio", true, false);
    validate_value(evidence.reliability_risk, "reliability_risk", true, true);
    validate_value(evidence.protected_region_risk, "protected_region_risk", false, true);
    if (evidence.estimated_time_ratio.value && *evidence.estimated_time_ratio.value <= 0.0)
        errors.emplace_back("invalid_estimated_time_ratio");
    if (evidence.material_ratio.value && *evidence.material_ratio.value <= 0.0)
        errors.emplace_back("invalid_material_ratio");
    if (evidence.retained_strength_ratio.value && *evidence.retained_strength_ratio.value < 0.0)
        errors.emplace_back("invalid_retained_strength_ratio");
    std::sort(errors.begin(), errors.end());
    errors.erase(std::unique(errors.begin(), errors.end()), errors.end());
    return errors;
}

const char* recommendation_goal_id(RecommendationGoal goal)
{
    switch (goal) {
    case RecommendationGoal::Balanced: return "balanced";
    case RecommendationGoal::Speed: return "speed";
    case RecommendationGoal::Quality: return "quality";
    }
    return "";
}

RecommendationGoalParseResult parse_recommendation_goal_id(std::string_view goal_id)
{
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        if (goal_id == recommendation_goal_id(goal))
            return {goal, std::nullopt};
    }
    return {std::nullopt, RecommendationErrorCode::UnsupportedGoalId};
}

const char* recommendation_error_code_name(RecommendationErrorCode code)
{
    switch (code) {
    case RecommendationErrorCode::UnsupportedGoalId: return "unsupported_goal_id";
    }
    return "unknown";
}

GoalResult& RecommendationSnapshot::goal_result(RecommendationGoal goal)
{
    return m_goal_results.at(goal_index(goal));
}

const GoalResult& RecommendationSnapshot::goal_result(RecommendationGoal goal) const
{
    return m_goal_results.at(goal_index(goal));
}

} // namespace Slic3r::AI::SmartSlicing
