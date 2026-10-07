#include "CandidateEvaluation.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::AI::SmartSlicing {
namespace {

constexpr double EPSILON = 1e-9;

bool known_true(const MetricValue<bool>& value)
{
    return value.known() && *value.value;
}

bool known_false(const MetricValue<bool>& value)
{
    return value.known() && !*value.value;
}

template<class T> bool valid_metric_shape(const MetricValue<T>& value)
{
    const bool valid_value = (value.availability == MetricAvailability::Known) == value.value.has_value();
    const bool valid_evidence = value.availability == MetricAvailability::Unknown ||
                                (!value.evidence_codes.empty() &&
                                 std::all_of(value.evidence_codes.begin(), value.evidence_codes.end(),
                                             [](const auto& code) { return !code.empty(); }));
    return valid_value && valid_evidence;
}

bool valid_metric_shape(const MetricValue<double>& value)
{
    return valid_metric_shape<double>(value) && (!value.value || std::isfinite(*value.value));
}

bool required_metric_missing(const MetricValue<double>& value)
{
    return !value.known();
}

bool required_risk_missing(const RiskComponent& value)
{
    return value.availability != MetricAvailability::Known;
}

bool optional_risk_missing(const RiskComponent& value)
{
    return value.availability == MetricAvailability::Unknown;
}

bool invalid_ratio_value(const MetricValue<double>& value)
{
    return !value.known() || *value.value <= CANDIDATE_RATIO_DENOMINATOR_MINIMUM;
}

bool ratio_exceeds(const MetricValue<double>& candidate, const MetricValue<double>& baseline, double maximum)
{
    return !invalid_ratio_value(candidate) && !invalid_ratio_value(baseline) &&
           *candidate.value / *baseline.value > maximum + EPSILON;
}

bool normalized_risk_worse(const RiskComponent& candidate, const RiskComponent& baseline)
{
    return candidate.availability == MetricAvailability::Known &&
           baseline.availability == MetricAvailability::Known && candidate.normalized_risk &&
           baseline.normalized_risk && *candidate.normalized_risk > *baseline.normalized_risk + EPSILON;
}

void append_if(bool condition, CandidateRejectionCode code, std::vector<CandidateRejectionCode>& codes)
{
    if (condition)
        codes.push_back(code);
}

} // namespace

const char* candidate_rejection_code_name(CandidateRejectionCode code)
{
    switch (code) {
    case CandidateRejectionCode::InvalidEvaluationPolicy: return "invalid_candidate_evaluation_policy";
    case CandidateRejectionCode::InvalidTrialMetricsContract: return "invalid_trial_metrics_contract";
    case CandidateRejectionCode::InvalidRiskAssessmentContract: return "invalid_risk_assessment_contract";
    case CandidateRejectionCode::InvalidEvaluationEvidence: return "invalid_candidate_evaluation_evidence";
    case CandidateRejectionCode::NativeSlicingError: return "native_slicing_error";
    case CandidateRejectionCode::PhysicalSlotEvidenceMissing: return "physical_slot_evidence_missing";
    case CandidateRejectionCode::PhysicalSlotIncompatible: return "physical_slot_incompatible";
    case CandidateRejectionCode::MaterialCompatibilityEvidenceMissing: return "material_compatibility_evidence_missing";
    case CandidateRejectionCode::MaterialIncompatible: return "material_incompatible";
    case CandidateRejectionCode::ColorMappingDegraded: return "color_mapping_degraded";
    case CandidateRejectionCode::ManualIntentEvidenceMissing: return "manual_intent_evidence_missing";
    case CandidateRejectionCode::ManualIntentConflict: return "manual_intent_conflict";
    case CandidateRejectionCode::StrengthBelowMinimum: return "strength_below_minimum";
    case CandidateRejectionCode::WallLoopEvidenceMissing: return "wall_loop_evidence_missing";
    case CandidateRejectionCode::WallLoopsBelowMinimum: return "wall_loops_below_minimum";
    case CandidateRejectionCode::InfillEvidenceMissing: return "infill_evidence_missing";
    case CandidateRejectionCode::InfillBelowMinimum: return "infill_below_minimum";
    case CandidateRejectionCode::AppearanceWorseThanBaseline: return "appearance_worse_than_baseline";
    case CandidateRejectionCode::ReliabilityUnacceptable: return "reliability_unacceptable";
    case CandidateRejectionCode::ProtectedRegionUnacceptable: return "protected_region_unacceptable";
    case CandidateRejectionCode::SpeedTimeReductionBelowMinimum: return "speed_time_reduction_below_minimum";
    case CandidateRejectionCode::SpeedMaterialIncrease: return "speed_material_increase";
    case CandidateRejectionCode::SpeedReliabilityWorseThanBaseline: return "speed_reliability_worse_than_baseline";
    case CandidateRejectionCode::BalancedTimeLimitExceeded: return "balanced_time_limit_exceeded";
    case CandidateRejectionCode::BalancedMaterialLimitExceeded: return "balanced_material_limit_exceeded";
    case CandidateRejectionCode::QualityTimeLimitExceeded: return "quality_time_limit_exceeded";
    case CandidateRejectionCode::QualityMaterialLimitExceeded: return "quality_material_limit_exceeded";
    case CandidateRejectionCode::MissingEstimatedTimeEvidence: return "missing_estimated_time_evidence";
    case CandidateRejectionCode::MissingMaterialEvidence: return "missing_material_evidence";
    case CandidateRejectionCode::MissingAppearanceEvidence: return "missing_appearance_evidence";
    case CandidateRejectionCode::MissingDimensionalEvidence: return "missing_dimensional_evidence";
    case CandidateRejectionCode::MissingStrengthEvidence: return "missing_strength_evidence";
    case CandidateRejectionCode::MissingReliabilityEvidence: return "missing_reliability_evidence";
    case CandidateRejectionCode::MissingProtectedRegionEvidence: return "missing_protected_region_evidence";
    case CandidateRejectionCode::MissingMulticolorEvidence: return "missing_multicolor_evidence";
    }
    return "candidate_rejected";
}

CandidateEvaluationResult evaluate_candidate(const CandidateEvaluationInput& input)
{
    CandidateEvaluationResult result;
    result.policy_version = input.policy_version;
    const GoalContract& contract = goal_contract(input.goal);
    const TrialMetricsNormalizationResult baseline_metrics = normalize_trial_metrics(input.baseline_metrics);
    const TrialMetricsNormalizationResult candidate_metrics = normalize_trial_metrics(input.candidate_metrics);
    const RiskAssessmentNormalizationResult baseline_risks = normalize_risk_assessment(input.baseline_risks);
    const RiskAssessmentNormalizationResult candidate_risks = normalize_risk_assessment(input.candidate_risks);

    // Common hard gates are deliberately evaluated in enum order.
    append_if(input.policy_version != CANDIDATE_EVALUATION_POLICY_VERSION,
              CandidateRejectionCode::InvalidEvaluationPolicy, result.rejection_codes);
    append_if(!baseline_metrics.valid() || !candidate_metrics.valid(),
              CandidateRejectionCode::InvalidTrialMetricsContract, result.rejection_codes);
    append_if(!baseline_risks.valid() || !candidate_risks.valid(),
              CandidateRejectionCode::InvalidRiskAssessmentContract, result.rejection_codes);
    append_if(!valid_metric_shape(input.manual_intent_preserved) ||
                  !valid_metric_shape(input.effective_wall_loops) ||
                  !valid_metric_shape(input.effective_infill_percent) ||
                  !valid_metric_shape(input.critical_surface_significantly_improved),
              CandidateRejectionCode::InvalidEvaluationEvidence, result.rejection_codes);
    append_if(std::any_of(candidate_metrics.metrics.native_diagnostics.begin(),
                          candidate_metrics.metrics.native_diagnostics.end(), [](const auto& diagnostic) {
                              return diagnostic.severity == NativeDiagnosticSeverity::Error;
                          }),
              CandidateRejectionCode::NativeSlicingError, result.rejection_codes);
    append_if(candidate_metrics.metrics.physical_slots_compatible.availability == MetricAvailability::Unknown,
              CandidateRejectionCode::PhysicalSlotEvidenceMissing, result.rejection_codes);
    append_if(known_false(candidate_metrics.metrics.physical_slots_compatible),
              CandidateRejectionCode::PhysicalSlotIncompatible, result.rejection_codes);
    append_if(!candidate_metrics.metrics.materials_compatible.known(),
              CandidateRejectionCode::MaterialCompatibilityEvidenceMissing, result.rejection_codes);
    append_if(known_false(candidate_metrics.metrics.materials_compatible),
              CandidateRejectionCode::MaterialIncompatible, result.rejection_codes);
    append_if(known_true(candidate_metrics.metrics.color_mapping_degraded),
              CandidateRejectionCode::ColorMappingDegraded, result.rejection_codes);
    append_if(!input.manual_intent_preserved.known(), CandidateRejectionCode::ManualIntentEvidenceMissing,
              result.rejection_codes);
    append_if(known_false(input.manual_intent_preserved), CandidateRejectionCode::ManualIntentConflict,
              result.rejection_codes);
    append_if(candidate_risks.assessment.strength.retained_strength_ratio &&
                  *candidate_risks.assessment.strength.retained_strength_ratio + EPSILON <
                      contract.minimum_strength_ratio,
              CandidateRejectionCode::StrengthBelowMinimum, result.rejection_codes);
    append_if(!input.effective_wall_loops.known(), CandidateRejectionCode::WallLoopEvidenceMissing,
              result.rejection_codes);
    append_if(input.effective_wall_loops.known() &&
                  *input.effective_wall_loops.value < contract.general_minimum_wall_loops,
              CandidateRejectionCode::WallLoopsBelowMinimum, result.rejection_codes);
    append_if(!input.effective_infill_percent.known(), CandidateRejectionCode::InfillEvidenceMissing,
              result.rejection_codes);
    double minimum_infill = contract.general_minimum_infill_percent;
    if (input.goal == RecommendationGoal::Speed && input.usage_purpose == UsagePurpose::Decoration &&
        contract.decoration_minimum_infill_percent)
        minimum_infill = *contract.decoration_minimum_infill_percent;
    append_if(input.effective_infill_percent.known() &&
                  *input.effective_infill_percent.value + EPSILON < minimum_infill,
              CandidateRejectionCode::InfillBelowMinimum, result.rejection_codes);
    append_if(normalized_risk_worse(candidate_risks.assessment.appearance,
                                    baseline_risks.assessment.appearance),
              CandidateRejectionCode::AppearanceWorseThanBaseline, result.rejection_codes);
    append_if(candidate_risks.assessment.reliability.availability == MetricAvailability::Known &&
                  candidate_risks.assessment.reliability.hard_gate_acceptable &&
                  !*candidate_risks.assessment.reliability.hard_gate_acceptable,
              CandidateRejectionCode::ReliabilityUnacceptable, result.rejection_codes);
    append_if(candidate_risks.assessment.protected_region.availability == MetricAvailability::Known &&
                  candidate_risks.assessment.protected_region.hard_gate_acceptable &&
                  !*candidate_risks.assessment.protected_region.hard_gate_acceptable,
              CandidateRejectionCode::ProtectedRegionUnacceptable, result.rejection_codes);
    if (!result.rejection_codes.empty()) {
        result.stage = CandidateEvaluationStage::CommonHardGates;
        return result;
    }

    const auto& baseline_time = baseline_metrics.metrics.estimated_time_seconds;
    const auto& candidate_time = candidate_metrics.metrics.estimated_time_seconds;
    const auto& baseline_material = baseline_metrics.metrics.total_material_volume_mm3;
    const auto& candidate_material = candidate_metrics.metrics.total_material_volume_mm3;

    if (input.goal == RecommendationGoal::Speed) {
        if (!invalid_ratio_value(baseline_time) && !invalid_ratio_value(candidate_time)) {
            const double reduction = 1.0 - *candidate_time.value / *baseline_time.value;
            append_if(reduction + EPSILON < *contract.minimum_time_reduction_ratio,
                      CandidateRejectionCode::SpeedTimeReductionBelowMinimum, result.rejection_codes);
        }
        append_if(ratio_exceeds(candidate_material, baseline_material, contract.maximum_material_ratio),
                  CandidateRejectionCode::SpeedMaterialIncrease, result.rejection_codes);
        append_if(normalized_risk_worse(candidate_risks.assessment.reliability,
                                        baseline_risks.assessment.reliability),
                  CandidateRejectionCode::SpeedReliabilityWorseThanBaseline, result.rejection_codes);
    } else if (input.goal == RecommendationGoal::Balanced) {
        const bool relaxed = known_true(input.critical_surface_significantly_improved);
        const double maximum_time = relaxed && contract.critical_surface_maximum_time_ratio ?
                                        *contract.critical_surface_maximum_time_ratio : contract.maximum_time_ratio;
        const double maximum_material = relaxed && contract.critical_surface_maximum_material_ratio ?
                                            *contract.critical_surface_maximum_material_ratio : contract.maximum_material_ratio;
        append_if(ratio_exceeds(candidate_time, baseline_time, maximum_time),
                  CandidateRejectionCode::BalancedTimeLimitExceeded, result.rejection_codes);
        append_if(ratio_exceeds(candidate_material, baseline_material, maximum_material),
                  CandidateRejectionCode::BalancedMaterialLimitExceeded, result.rejection_codes);
        if (relaxed && contract.critical_surface_relaxation_explanation_code)
            result.explanation_codes.push_back(*contract.critical_surface_relaxation_explanation_code);
    } else {
        append_if(ratio_exceeds(candidate_time, baseline_time, contract.maximum_time_ratio),
                  CandidateRejectionCode::QualityTimeLimitExceeded, result.rejection_codes);
        append_if(ratio_exceeds(candidate_material, baseline_material, contract.maximum_material_ratio),
                  CandidateRejectionCode::QualityMaterialLimitExceeded, result.rejection_codes);
    }
    if (!result.rejection_codes.empty()) {
        result.stage = CandidateEvaluationStage::GoalHardGates;
        return result;
    }

    append_if(required_metric_missing(baseline_time) || required_metric_missing(candidate_time) ||
                  invalid_ratio_value(baseline_time) || invalid_ratio_value(candidate_time),
              CandidateRejectionCode::MissingEstimatedTimeEvidence, result.rejection_codes);
    append_if(required_metric_missing(baseline_material) || required_metric_missing(candidate_material) ||
                  invalid_ratio_value(baseline_material) || invalid_ratio_value(candidate_material),
              CandidateRejectionCode::MissingMaterialEvidence, result.rejection_codes);
    append_if(required_risk_missing(baseline_risks.assessment.appearance) ||
                  required_risk_missing(candidate_risks.assessment.appearance),
              CandidateRejectionCode::MissingAppearanceEvidence, result.rejection_codes);
    append_if(optional_risk_missing(baseline_risks.assessment.dimensional) ||
                  optional_risk_missing(candidate_risks.assessment.dimensional),
              CandidateRejectionCode::MissingDimensionalEvidence, result.rejection_codes);
    append_if(required_risk_missing(baseline_risks.assessment.strength) ||
                  required_risk_missing(candidate_risks.assessment.strength) ||
                  !candidate_risks.assessment.strength.retained_strength_ratio,
              CandidateRejectionCode::MissingStrengthEvidence, result.rejection_codes);
    append_if(required_risk_missing(baseline_risks.assessment.reliability) ||
                  required_risk_missing(candidate_risks.assessment.reliability) ||
                  (candidate_risks.assessment.reliability.availability == MetricAvailability::Known &&
                   !candidate_risks.assessment.reliability.hard_gate_acceptable),
              CandidateRejectionCode::MissingReliabilityEvidence, result.rejection_codes);
    append_if(optional_risk_missing(baseline_risks.assessment.protected_region) ||
                  optional_risk_missing(candidate_risks.assessment.protected_region) ||
                  (candidate_risks.assessment.protected_region.availability == MetricAvailability::Known &&
                   !candidate_risks.assessment.protected_region.hard_gate_acceptable),
              CandidateRejectionCode::MissingProtectedRegionEvidence, result.rejection_codes);
    append_if(baseline_metrics.metrics.color_mapping_degraded.availability == MetricAvailability::Unknown ||
                  candidate_metrics.metrics.color_mapping_degraded.availability == MetricAvailability::Unknown,
              CandidateRejectionCode::MissingMulticolorEvidence, result.rejection_codes);
    if (!result.rejection_codes.empty()) {
        result.stage = CandidateEvaluationStage::MissingEvidence;
        return result;
    }

    result.stage = CandidateEvaluationStage::Passed;
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
