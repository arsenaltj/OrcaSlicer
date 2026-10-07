#pragma once

#include "SmartSlicingTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

class OwnerRiskConfirmationContract;

inline constexpr uint32_t RECOMMENDATION_CONTRACT_VERSION = 1;
inline constexpr const char* CANDIDATE_SELECTION_POLICY_VERSION = "candidate-selection-policy/v1";

enum class UsagePurpose { Unknown, Decoration, General, Functional };
enum class RecommendationGoal { Balanced, Speed, Quality };
enum class GoalResultStatus { Analyzing, Ready, Unavailable, Failed, Stale, Applied };
enum class EvidenceAvailability { Available, Unavailable, NotApplicable };
enum class RecommendationErrorCode { UnsupportedGoalId };

inline constexpr std::array<RecommendationGoal, 3> RECOMMENDATION_GOALS{
    RecommendationGoal::Balanced,
    RecommendationGoal::Speed,
    RecommendationGoal::Quality,
};

const char* recommendation_goal_id(RecommendationGoal goal);
const char* recommendation_error_code_name(RecommendationErrorCode code);

struct RecommendationGoalParseResult
{
    std::optional<RecommendationGoal> goal;
    std::optional<RecommendationErrorCode> error;

    explicit operator bool() const { return goal.has_value(); }
};

RecommendationGoalParseResult parse_recommendation_goal_id(std::string_view goal_id);
bool valid_recommendation_code(std::string_view code);

template<class T> struct EvidenceValue
{
    EvidenceAvailability availability{EvidenceAvailability::Unavailable};
    std::optional<T> value;
    std::string source_code;
};

struct RecommendationRequest
{
    uint32_t contract_version{RECOMMENDATION_CONTRACT_VERSION};
    UsagePurpose purpose{UsagePurpose::General};
    std::vector<std::string> requested_goal_ids{"balanced", "speed", "quality"};
};

struct RecommendationEvidence
{
    std::string selection_policy_version;
    std::vector<std::string> explanation_codes;
    EvidenceValue<double> estimated_time_ratio;
    EvidenceValue<double> material_ratio;
    EvidenceValue<double> appearance_risk;
    EvidenceValue<double> dimensional_risk;
    EvidenceValue<double> strength_risk;
    EvidenceValue<double> retained_strength_ratio;
    EvidenceValue<double> reliability_risk;
    EvidenceValue<double> protected_region_risk;
};

std::vector<std::string> validate_recommendation_evidence(const RecommendationEvidence& evidence);

struct GoalResult
{
    CandidateId candidate_id;
    GoalResultStatus status{GoalResultStatus::Analyzing};
    std::vector<std::string> diagnostic_codes;
    std::optional<RecommendationEvidence> evidence;
    CandidateId selected_candidate_id;
    std::shared_ptr<const OwnerRiskConfirmationContract> risk_confirmation_contract;
};

struct BaselineResult
{
    CandidateId candidate_id;
    GoalResultStatus status{GoalResultStatus::Analyzing};
    std::vector<std::string> diagnostic_codes;
};

class RecommendationSnapshot
{
public:
    uint32_t contract_version{RECOMMENDATION_CONTRACT_VERSION};
    BaselineResult baseline;

    GoalResult& goal_result(RecommendationGoal goal);
    const GoalResult& goal_result(RecommendationGoal goal) const;
    const std::array<GoalResult, RECOMMENDATION_GOALS.size()>& goal_results() const { return m_goal_results; }

private:
    std::array<GoalResult, RECOMMENDATION_GOALS.size()> m_goal_results{};
};

} // namespace Slic3r::AI::SmartSlicing
