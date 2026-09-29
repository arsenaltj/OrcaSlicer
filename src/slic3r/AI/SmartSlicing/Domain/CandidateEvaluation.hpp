#pragma once

#include "GoalContract.hpp"
#include "RiskAssessment.hpp"
#include "TrialMetrics.hpp"

#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* CANDIDATE_EVALUATION_POLICY_VERSION = "candidate-evaluation-policy/v1";
inline constexpr double CANDIDATE_RATIO_DENOMINATOR_MINIMUM = 1e-9;

enum class CandidateEvaluationStage { CommonHardGates, GoalHardGates, MissingEvidence, Passed };

enum class CandidateRejectionCode {
    InvalidEvaluationPolicy,
    InvalidTrialMetricsContract,
    InvalidRiskAssessmentContract,
    InvalidEvaluationEvidence,
    NativeSlicingError,
    PhysicalSlotEvidenceMissing,
    PhysicalSlotIncompatible,
    MaterialCompatibilityEvidenceMissing,
    MaterialIncompatible,
    ColorMappingDegraded,
    ManualIntentEvidenceMissing,
    ManualIntentConflict,
    StrengthBelowMinimum,
    WallLoopEvidenceMissing,
    WallLoopsBelowMinimum,
    InfillEvidenceMissing,
    InfillBelowMinimum,
    AppearanceWorseThanBaseline,
    ReliabilityUnacceptable,
    ProtectedRegionUnacceptable,
    SpeedTimeReductionBelowMinimum,
    SpeedMaterialIncrease,
    SpeedReliabilityWorseThanBaseline,
    BalancedTimeLimitExceeded,
    BalancedMaterialLimitExceeded,
    QualityTimeLimitExceeded,
    QualityMaterialLimitExceeded,
    MissingEstimatedTimeEvidence,
    MissingMaterialEvidence,
    MissingAppearanceEvidence,
    MissingDimensionalEvidence,
    MissingStrengthEvidence,
    MissingReliabilityEvidence,
    MissingProtectedRegionEvidence,
    MissingMulticolorEvidence,
};

const char* candidate_rejection_code_name(CandidateRejectionCode code);

struct CandidateEvaluationInput
{
    std::string policy_version{CANDIDATE_EVALUATION_POLICY_VERSION};
    RecommendationGoal goal{RecommendationGoal::Balanced};
    UsagePurpose usage_purpose{UsagePurpose::General};
    TrialMetrics baseline_metrics;
    TrialMetrics candidate_metrics;
    RiskAssessment baseline_risks;
    RiskAssessment candidate_risks;
    MetricValue<bool> manual_intent_preserved;
    MetricValue<int> effective_wall_loops;
    MetricValue<double> effective_infill_percent;
    MetricValue<bool> critical_surface_significantly_improved;
};

struct CandidateEvaluationResult
{
    std::string policy_version{CANDIDATE_EVALUATION_POLICY_VERSION};
    CandidateEvaluationStage stage{CandidateEvaluationStage::CommonHardGates};
    std::vector<CandidateRejectionCode> rejection_codes;
    std::vector<std::string> explanation_codes;

    bool accepted() const { return stage == CandidateEvaluationStage::Passed && rejection_codes.empty(); }
};

CandidateEvaluationResult evaluate_candidate(const CandidateEvaluationInput& input);

} // namespace Slic3r::AI::SmartSlicing
