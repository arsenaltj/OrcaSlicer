#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/CandidateEvaluation.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

RiskComponent known_risk(double normalized, bool acceptable = true)
{
    RiskComponent result;
    result.availability = MetricAvailability::Known;
    result.evidence = {{"native_proxy", std::to_string(normalized)}};
    result.normalized_risk = normalized;
    result.hard_gate_acceptable = acceptable;
    return result;
}

RiskComponent not_applicable_risk(const std::string& reason)
{
    RiskComponent result;
    result.availability = MetricAvailability::NotApplicable;
    result.evidence = {{reason, "not_applicable"}};
    return result;
}

RiskAssessment complete_risks(double appearance = 0.20, double dimensional = 0.20,
                              double strength = 0.20, double reliability = 0.20,
                              double protected_region = 0.20)
{
    RiskAssessment result;
    result.appearance = known_risk(appearance);
    result.dimensional = known_risk(dimensional);
    result.strength = known_risk(strength);
    result.strength.retained_strength_ratio = 1.0;
    result.reliability = known_risk(reliability);
    result.protected_region = known_risk(protected_region);
    return result;
}

TrialMetrics complete_metrics(double time_seconds = 100.0, double material_mm3 = 100.0)
{
    TrialMetrics result;
    result.estimated_time_seconds = MetricValue<double>::known(time_seconds, {"orca_estimate"});
    result.model_material_volume_mm3 = MetricValue<double>::known(material_mm3, {"orca_model_volume"});
    result.support_material_volume_mm3 = MetricValue<double>::not_applicable({"support_disabled"});
    result.flush_material_volume_mm3 = MetricValue<double>::not_applicable({"single_material"});
    result.wipe_tower_material_volume_mm3 = MetricValue<double>::not_applicable({"single_material"});
    result.total_material_volume_mm3 = MetricValue<double>::known(material_mm3, {"orca_total_volume"});
    result.tool_change_count = MetricValue<size_t>::known(0, {"orca_tool_sequence"});
    result.tool_change_time_seconds = MetricValue<double>::not_applicable({"single_material"});
    result.layer_tool_sequences = MetricValue<std::vector<LayerToolSequence>>::known({}, {"orca_tool_sequence"});
    result.physical_slots_compatible = MetricValue<bool>::not_applicable({"single_material"});
    result.materials_compatible = MetricValue<bool>::known(true, {"orca_material_validation"});
    result.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});
    result.wipe_tower_enabled = MetricValue<bool>::not_applicable({"single_material"});
    return result;
}

void set_material_total(TrialMetrics& metrics, double material_mm3)
{
    metrics.model_material_volume_mm3 = MetricValue<double>::known(material_mm3, {"orca_model_volume"});
    metrics.total_material_volume_mm3 = MetricValue<double>::known(material_mm3, {"orca_total_volume"});
}

CandidateEvaluationInput valid_input(RecommendationGoal goal = RecommendationGoal::Balanced)
{
    CandidateEvaluationInput input;
    input.goal = goal;
    input.baseline_metrics = complete_metrics();
    input.candidate_metrics = complete_metrics(goal == RecommendationGoal::Speed ? 90.0 : 100.0);
    input.baseline_risks = complete_risks();
    input.candidate_risks = complete_risks();
    input.candidate_risks.strength.retained_strength_ratio = 0.95;
    input.manual_intent_preserved = MetricValue<bool>::known(true, {"proposal_validator"});
    input.effective_wall_loops = MetricValue<int>::known(2, {"effective_config"});
    input.effective_infill_percent = MetricValue<double>::known(15.0, {"effective_config"});
    input.critical_surface_significantly_improved = MetricValue<bool>::known(false, {"risk_assessment"});
    return input;
}

std::vector<std::string> names(const CandidateEvaluationResult& result)
{
    std::vector<std::string> output;
    for (CandidateRejectionCode code : result.rejection_codes)
        output.emplace_back(candidate_rejection_code_name(code));
    return output;
}

} // namespace

TEST_CASE("Trial metrics normalize availability, summaries, and material totals", "[AI][SmartSlicing][D5T1][TrialMetrics]")
{
    TrialMetrics metrics;
    metrics.model_material_volume_mm3 = MetricValue<double>::known(80.0, {"z", "a", "z"});
    metrics.support_material_volume_mm3 = MetricValue<double>::known(10.0, {"support"});
    metrics.flush_material_volume_mm3 = MetricValue<double>::not_applicable({"single_material"});
    metrics.wipe_tower_material_volume_mm3 = MetricValue<double>::known(5.0, {"tower"});
    metrics.native_diagnostics = {
        {NativeDiagnosticSeverity::Warning, "z_warning", "z"},
        {NativeDiagnosticSeverity::Error, "a_error", "a"},
    };
    metrics.effective_parameters = {
        {"plate", "wall_loops", "3", "process"},
        {"object:1", "layer_height", "0.16", "object"},
    };
    ObjectTransformSummary second;
    second.object_id = 2;
    second.instance_id = 2;
    ObjectTransformSummary first;
    first.object_id = 1;
    first.instance_id = 1;
    metrics.object_transforms = {second, first};
    metrics.layer_tool_sequences = MetricValue<std::vector<LayerToolSequence>>::known(
        {{2, {3, 1}}, {1, {2, 1}}}, {"orca_tool_sequence"});

    const auto normalized = normalize_trial_metrics(metrics);
    REQUIRE(normalized.valid());
    REQUIRE(normalized.metrics.total_material_volume_mm3.known());
    CHECK(*normalized.metrics.total_material_volume_mm3.value == Catch::Approx(95.0));
    CHECK(normalized.metrics.model_material_volume_mm3.evidence_codes == std::vector<std::string>{"a", "z"});
    CHECK(normalized.metrics.native_diagnostics.front().code == "z_warning");
    CHECK(normalized.metrics.effective_parameters.front().scope_id == "object:1");
    CHECK(normalized.metrics.object_transforms.front().object_id == 1);
    REQUIRE(normalized.metrics.layer_tool_sequences.value);
    CHECK(normalized.metrics.layer_tool_sequences.value->front().layer_index == 1);
    CHECK(normalized.metrics.layer_tool_sequences.value->back().tool_ids == std::vector<size_t>{3, 1});
    CHECK(std::string(metric_availability_name(MetricAvailability::NotApplicable)) == "not_applicable");
}

TEST_CASE("Known total material must agree with computable components", "[AI][SmartSlicing][D5T1][TrialMetrics]")
{
    TrialMetrics within_tolerance = complete_metrics();
    within_tolerance.total_material_volume_mm3 = MetricValue<double>::known(
        100.0 + TRIAL_MATERIAL_ABSOLUTE_TOLERANCE_MM3 * 0.5, {"orca_total_volume"});
    CHECK(normalize_trial_metrics(within_tolerance).valid());

    TrialMetrics outside_tolerance = complete_metrics();
    outside_tolerance.total_material_volume_mm3 = MetricValue<double>::known(
        100.0 + TRIAL_MATERIAL_ABSOLUTE_TOLERANCE_MM3 * 2.0, {"orca_total_volume"});
    const auto mismatch = normalize_trial_metrics(outside_tolerance);
    CHECK_FALSE(mismatch.valid());
    CHECK(std::find(mismatch.validation_codes.begin(), mismatch.validation_codes.end(),
                    "total_material_volume_mismatch") != mismatch.validation_codes.end());

    CandidateEvaluationInput bypass = valid_input();
    bypass.candidate_metrics.total_material_volume_mm3 = MetricValue<double>::known(1.0, {"forged_total"});
    const auto rejected = evaluate_candidate(bypass);
    CHECK(rejected.stage == CandidateEvaluationStage::CommonHardGates);
    CHECK(rejected.rejection_codes ==
          std::vector<CandidateRejectionCode>{CandidateRejectionCode::InvalidTrialMetricsContract});

    TrialMetrics unknown_component = complete_metrics();
    unknown_component.support_material_volume_mm3 = MetricValue<double>::unknown({"not_extracted"});
    unknown_component.total_material_volume_mm3 = MetricValue<double>::unknown({"not_computable"});
    const auto unknown = normalize_trial_metrics(unknown_component);
    CHECK(unknown.valid());
    CHECK(unknown.metrics.total_material_volume_mm3.availability == MetricAvailability::Unknown);
}

TEST_CASE("Trial metrics and risk contracts reject inconsistent availability", "[AI][SmartSlicing][D5T1][TrialMetrics]")
{
    TrialMetrics metrics = complete_metrics();
    metrics.estimated_time_seconds = MetricValue<double>::unknown({"not_extracted"});
    metrics.estimated_time_seconds.value = 10.0;
    CHECK_FALSE(normalize_trial_metrics(metrics).valid());

    metrics = complete_metrics();
    metrics.policy_version = "trial-metrics-policy/future";
    CHECK_FALSE(normalize_trial_metrics(metrics).valid());

    RiskAssessment risks = complete_risks();
    risks.dimensional = {};
    risks.dimensional.normalized_risk = 0.0;
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    risks = complete_risks();
    risks.schema = "future";
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    risks = complete_risks();
    risks.policy_version = "risk-assessment-policy/future";
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    risks = complete_risks();
    risks.appearance.evidence.front().raw_value.clear();
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    metrics = complete_metrics();
    metrics.object_transforms = {ObjectTransformSummary{}, ObjectTransformSummary{}};
    auto invalid_transforms = normalize_trial_metrics(metrics);
    CHECK_FALSE(invalid_transforms.valid());
    CHECK(std::find(invalid_transforms.validation_codes.begin(), invalid_transforms.validation_codes.end(),
                    "invalid_object_transform_identity") != invalid_transforms.validation_codes.end());

    ObjectTransformSummary duplicate;
    duplicate.object_id = 1;
    duplicate.instance_id = 1;
    metrics = complete_metrics();
    metrics.object_transforms = {duplicate, duplicate};
    invalid_transforms = normalize_trial_metrics(metrics);
    CHECK_FALSE(invalid_transforms.valid());
    CHECK(std::find(invalid_transforms.validation_codes.begin(), invalid_transforms.validation_codes.end(),
                    "duplicate_object_transform_identity") != invalid_transforms.validation_codes.end());
}

TEST_CASE("Known and not-applicable values require source evidence", "[AI][SmartSlicing][D5T1][TrialMetrics]")
{
    TrialMetrics metrics = complete_metrics();
    metrics.estimated_time_seconds = MetricValue<double>::known(100.0);
    CHECK_FALSE(normalize_trial_metrics(metrics).valid());

    metrics = complete_metrics();
    metrics.physical_slots_compatible = MetricValue<bool>::not_applicable();
    CHECK_FALSE(normalize_trial_metrics(metrics).valid());

    metrics = complete_metrics();
    metrics.color_mapping_degraded = MetricValue<bool>::not_applicable();
    CHECK_FALSE(normalize_trial_metrics(metrics).valid());

    RiskAssessment risks = complete_risks();
    risks.dimensional = {};
    risks.dimensional.availability = MetricAvailability::NotApplicable;
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    risks = complete_risks();
    risks.appearance.evidence.clear();
    CHECK_FALSE(normalize_risk_assessment(risks).valid());

    const std::vector<std::function<void(CandidateEvaluationInput&)>> bare_gate_evidence{
        [](auto& input) { input.manual_intent_preserved = MetricValue<bool>::known(true); },
        [](auto& input) { input.effective_wall_loops = MetricValue<int>::known(2); },
        [](auto& input) { input.effective_infill_percent = MetricValue<double>::known(15.0); },
        [](auto& input) { input.critical_surface_significantly_improved = MetricValue<bool>::known(false); },
        [](auto& input) { input.critical_surface_significantly_improved = MetricValue<bool>::not_applicable(); },
    };
    for (const auto& mutate : bare_gate_evidence) {
        CandidateEvaluationInput input = valid_input();
        mutate(input);
        const auto result = evaluate_candidate(input);
        CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
        CHECK(std::find(result.rejection_codes.begin(), result.rejection_codes.end(),
                        CandidateRejectionCode::InvalidEvaluationEvidence) != result.rejection_codes.end());
    }
}

TEST_CASE("Common gates fail closed before goal or missing evidence", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input(RecommendationGoal::Speed);
    input.candidate_metrics.native_diagnostics = {
        {NativeDiagnosticSeverity::Warning, "warning", "does not reject"},
        {NativeDiagnosticSeverity::Error, "native_error", "rejects"},
    };
    input.candidate_metrics.physical_slots_compatible = MetricValue<bool>::unknown({"not_extracted"});
    input.candidate_metrics.materials_compatible = MetricValue<bool>::known(false, {"native_validation"});
    input.manual_intent_preserved = MetricValue<bool>::known(false, {"proposal_validator"});
    input.candidate_risks.strength.retained_strength_ratio = 0.9499;
    input.effective_wall_loops = MetricValue<int>::known(1, {"effective_config"});
    input.effective_infill_percent = MetricValue<double>::known(14.99, {"effective_config"});
    input.candidate_risks.appearance.normalized_risk = 0.21;
    input.candidate_risks.reliability.hard_gate_acceptable = false;
    input.candidate_risks.protected_region.hard_gate_acceptable = false;
    input.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(99.0, {"orca_estimate"});

    const CandidateEvaluationResult result = evaluate_candidate(input);
    CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
    CHECK(names(result) == std::vector<std::string>{
        "native_slicing_error",
        "physical_slot_evidence_missing",
        "material_incompatible",
        "manual_intent_conflict",
        "strength_below_minimum",
        "wall_loops_below_minimum",
        "infill_below_minimum",
        "appearance_worse_than_baseline",
        "reliability_unacceptable",
        "protected_region_unacceptable",
    });
}

TEST_CASE("Native warnings do not fail common gates", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input();
    input.candidate_metrics.native_diagnostics = {
        {NativeDiagnosticSeverity::Warning, "thin_wall", "warning only"},
    };
    CHECK(evaluate_candidate(input).accepted());
}

TEST_CASE("Every common hard gate rejects independently", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    using Mutation = std::function<void(CandidateEvaluationInput&)>;
    const std::vector<std::pair<Mutation, CandidateRejectionCode>> cases{
        {[](auto& value) { value.candidate_metrics.version = "future"; },
         CandidateRejectionCode::InvalidTrialMetricsContract},
        {[](auto& value) { value.candidate_risks.version = "future"; },
         CandidateRejectionCode::InvalidRiskAssessmentContract},
        {[](auto& value) {
             value.candidate_metrics.native_diagnostics.push_back(
                 {NativeDiagnosticSeverity::Error, "native", "error"});
         }, CandidateRejectionCode::NativeSlicingError},
        {[](auto& value) {
             value.candidate_metrics.physical_slots_compatible =
                 MetricValue<bool>::unknown({"not_extracted"});
         }, CandidateRejectionCode::PhysicalSlotEvidenceMissing},
        {[](auto& value) {
             value.candidate_metrics.physical_slots_compatible =
                 MetricValue<bool>::known(false, {"slot_validation"});
         }, CandidateRejectionCode::PhysicalSlotIncompatible},
        {[](auto& value) {
             value.candidate_metrics.materials_compatible =
                 MetricValue<bool>::unknown({"not_extracted"});
         }, CandidateRejectionCode::MaterialCompatibilityEvidenceMissing},
        {[](auto& value) {
             value.candidate_metrics.materials_compatible =
                 MetricValue<bool>::known(false, {"material_validation"});
         }, CandidateRejectionCode::MaterialIncompatible},
        {[](auto& value) {
             value.candidate_metrics.color_mapping_degraded =
                 MetricValue<bool>::known(true, {"mapping_validation"});
         }, CandidateRejectionCode::ColorMappingDegraded},
        {[](auto& value) { value.manual_intent_preserved = MetricValue<bool>::unknown({"not_checked"}); },
         CandidateRejectionCode::ManualIntentEvidenceMissing},
        {[](auto& value) { value.manual_intent_preserved = MetricValue<bool>::known(false, {"validator"}); },
         CandidateRejectionCode::ManualIntentConflict},
        {[](auto& value) { value.candidate_risks.strength.retained_strength_ratio = 0.94; },
         CandidateRejectionCode::StrengthBelowMinimum},
        {[](auto& value) { value.effective_wall_loops = MetricValue<int>::unknown({"not_extracted"}); },
         CandidateRejectionCode::WallLoopEvidenceMissing},
        {[](auto& value) { value.effective_wall_loops = MetricValue<int>::known(1, {"effective_config"}); },
         CandidateRejectionCode::WallLoopsBelowMinimum},
        {[](auto& value) { value.effective_infill_percent = MetricValue<double>::unknown({"not_extracted"}); },
         CandidateRejectionCode::InfillEvidenceMissing},
        {[](auto& value) { value.effective_infill_percent = MetricValue<double>::known(14.0, {"effective_config"}); },
         CandidateRejectionCode::InfillBelowMinimum},
        {[](auto& value) { value.candidate_risks.appearance.normalized_risk = 0.3; },
         CandidateRejectionCode::AppearanceWorseThanBaseline},
        {[](auto& value) { value.candidate_risks.reliability.hard_gate_acceptable = false; },
         CandidateRejectionCode::ReliabilityUnacceptable},
        {[](auto& value) { value.candidate_risks.protected_region.hard_gate_acceptable = false; },
         CandidateRejectionCode::ProtectedRegionUnacceptable},
    };

    for (const auto& [mutate, expected] : cases) {
        CandidateEvaluationInput input = valid_input();
        mutate(input);
        const auto result = evaluate_candidate(input);
        INFO(candidate_rejection_code_name(expected));
        CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
        CHECK(result.rejection_codes == std::vector<CandidateRejectionCode>{expected});
    }
}

TEST_CASE("Evaluation policy version participates in stable rejection", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input();
    input.policy_version = "candidate-evaluation-policy/future";
    const auto result = evaluate_candidate(input);
    CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
    CHECK(result.rejection_codes ==
          std::vector<CandidateRejectionCode>{CandidateRejectionCode::InvalidEvaluationPolicy});
}

TEST_CASE("Non-finite or structurally inconsistent gate evidence fails closed", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        CandidateEvaluationInput input = valid_input();
        input.effective_infill_percent = MetricValue<double>::known(invalid, {"effective_config"});
        const auto result = evaluate_candidate(input);
        CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::InvalidEvaluationEvidence});
    }

    CandidateEvaluationInput inconsistent = valid_input();
    inconsistent.critical_surface_significantly_improved =
        MetricValue<bool>::unknown({"not_assessed"});
    inconsistent.critical_surface_significantly_improved.value = true;
    const auto result = evaluate_candidate(inconsistent);
    CHECK(result.stage == CandidateEvaluationStage::CommonHardGates);
    CHECK(result.rejection_codes ==
          std::vector<CandidateRejectionCode>{CandidateRejectionCode::InvalidEvaluationEvidence});
}

TEST_CASE("Strength boundary accepts exactly ninety five percent", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input();
    input.candidate_risks.strength.retained_strength_ratio = 0.95;
    CHECK(evaluate_candidate(input).accepted());

    input.candidate_risks.strength.retained_strength_ratio = 0.949999;
    const auto rejected = evaluate_candidate(input);
    CHECK(rejected.stage == CandidateEvaluationStage::CommonHardGates);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{CandidateRejectionCode::StrengthBelowMinimum});
}

TEST_CASE("Speed accepts ten percent but rejects 9.99 percent and material growth", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput exact = valid_input(RecommendationGoal::Speed);
    exact.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(90.0, {"orca_estimate"});
    CHECK(evaluate_candidate(exact).accepted());

    CandidateEvaluationInput shortfall = exact;
    shortfall.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(90.01, {"orca_estimate"});
    auto rejected = evaluate_candidate(shortfall);
    CHECK(rejected.stage == CandidateEvaluationStage::GoalHardGates);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::SpeedTimeReductionBelowMinimum,
    });

    set_material_total(shortfall.candidate_metrics, 100.01);
    rejected = evaluate_candidate(shortfall);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::SpeedTimeReductionBelowMinimum,
        CandidateRejectionCode::SpeedMaterialIncrease,
    });

    CandidateEvaluationInput reliability = exact;
    reliability.candidate_risks.reliability.normalized_risk = 0.21;
    rejected = evaluate_candidate(reliability);
    CHECK(rejected.stage == CandidateEvaluationStage::GoalHardGates);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::SpeedReliabilityWorseThanBaseline,
    });
}

TEST_CASE("Usage floors follow the goal contract", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput decoration = valid_input(RecommendationGoal::Speed);
    decoration.usage_purpose = UsagePurpose::Decoration;
    decoration.effective_infill_percent = MetricValue<double>::known(10.0, {"effective_config"});
    CHECK(evaluate_candidate(decoration).accepted());

    decoration.effective_infill_percent = MetricValue<double>::known(9.99, {"effective_config"});
    CHECK(evaluate_candidate(decoration).rejection_codes ==
          std::vector<CandidateRejectionCode>{CandidateRejectionCode::InfillBelowMinimum});

    CandidateEvaluationInput unknown = valid_input(RecommendationGoal::Speed);
    unknown.usage_purpose = UsagePurpose::Unknown;
    unknown.effective_infill_percent = MetricValue<double>::known(14.99, {"effective_config"});
    CHECK(evaluate_candidate(unknown).rejection_codes ==
          std::vector<CandidateRejectionCode>{CandidateRejectionCode::InfillBelowMinimum});
}

TEST_CASE("Balanced and quality boundaries use GoalContract ratios", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput balanced = valid_input(RecommendationGoal::Balanced);
    balanced.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(115.0, {"orca_estimate"});
    set_material_total(balanced.candidate_metrics, 110.0);
    CHECK(evaluate_candidate(balanced).accepted());

    balanced.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(115.01, {"orca_estimate"});
    set_material_total(balanced.candidate_metrics, 110.01);
    auto rejected = evaluate_candidate(balanced);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::BalancedTimeLimitExceeded,
        CandidateRejectionCode::BalancedMaterialLimitExceeded,
    });

    balanced.critical_surface_significantly_improved = MetricValue<bool>::known(true, {"protected_surface"});
    balanced.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(125.0, {"orca_estimate"});
    set_material_total(balanced.candidate_metrics, 115.0);
    const auto relaxed = evaluate_candidate(balanced);
    CHECK(relaxed.accepted());
    CHECK(relaxed.explanation_codes ==
          std::vector<std::string>{"balanced_critical_surface_resource_relaxation"});
    balanced.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(125.01, {"orca_estimate"});
    set_material_total(balanced.candidate_metrics, 115.01);
    rejected = evaluate_candidate(balanced);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::BalancedTimeLimitExceeded,
        CandidateRejectionCode::BalancedMaterialLimitExceeded,
    });

    CandidateEvaluationInput quality = valid_input(RecommendationGoal::Quality);
    quality.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(200.0, {"orca_estimate"});
    set_material_total(quality.candidate_metrics, 120.0);
    CHECK(evaluate_candidate(quality).accepted());
    quality.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(200.01, {"orca_estimate"});
    set_material_total(quality.candidate_metrics, 120.01);
    rejected = evaluate_candidate(quality);
    CHECK(rejected.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::QualityTimeLimitExceeded,
        CandidateRejectionCode::QualityMaterialLimitExceeded,
    });
}

TEST_CASE("Unknown evidence is rejected after hard gates while not applicable remains distinct", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input();
    input.candidate_metrics.estimated_time_seconds = MetricValue<double>::unknown({"not_extracted"});
    input.candidate_metrics.total_material_volume_mm3 = MetricValue<double>::unknown({"not_computable"});
    input.candidate_metrics.model_material_volume_mm3 = MetricValue<double>::unknown({"not_extracted"});
    input.candidate_risks.appearance = {};
    input.candidate_risks.dimensional = {};
    input.candidate_risks.strength = {};
    input.candidate_risks.reliability = {};
    input.candidate_risks.protected_region = {};
    input.candidate_metrics.color_mapping_degraded = MetricValue<bool>::unknown({"not_extracted"});

    const auto result = evaluate_candidate(input);
    CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
    CHECK(names(result) == std::vector<std::string>{
        "missing_estimated_time_evidence",
        "missing_material_evidence",
        "missing_appearance_evidence",
        "missing_dimensional_evidence",
        "missing_strength_evidence",
        "missing_reliability_evidence",
        "missing_protected_region_evidence",
        "missing_multicolor_evidence",
    });

    input.candidate_risks.dimensional = not_applicable_risk("no_declared_dimension");
    input.candidate_risks.protected_region = not_applicable_risk("no_protected_region");
    input.candidate_metrics.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});
    const auto distinct = evaluate_candidate(input);
    CHECK(std::find(distinct.rejection_codes.begin(), distinct.rejection_codes.end(),
                    CandidateRejectionCode::MissingDimensionalEvidence) == distinct.rejection_codes.end());
    CHECK(std::find(distinct.rejection_codes.begin(), distinct.rejection_codes.end(),
                    CandidateRejectionCode::MissingProtectedRegionEvidence) == distinct.rejection_codes.end());
    CHECK(std::find(distinct.rejection_codes.begin(), distinct.rejection_codes.end(),
                    CandidateRejectionCode::MissingMulticolorEvidence) == distinct.rejection_codes.end());
}

TEST_CASE("Not applicable is rejected for required metrics and risks", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput allowed = valid_input();
    allowed.baseline_risks.dimensional = not_applicable_risk("no_declared_dimension");
    allowed.candidate_risks.dimensional = allowed.baseline_risks.dimensional;
    allowed.baseline_risks.protected_region = not_applicable_risk("no_protected_region");
    allowed.candidate_risks.protected_region = allowed.baseline_risks.protected_region;
    CHECK(evaluate_candidate(allowed).accepted());

    CandidateEvaluationInput input = valid_input();
    input.candidate_metrics.estimated_time_seconds = MetricValue<double>::not_applicable({"invalid_na"});
    input.candidate_metrics.total_material_volume_mm3 = MetricValue<double>::not_applicable({"invalid_na"});
    input.candidate_metrics.model_material_volume_mm3 = MetricValue<double>::unknown({"not_extracted"});
    input.candidate_risks.appearance = not_applicable_risk("invalid_na");
    input.candidate_risks.strength = not_applicable_risk("invalid_na");
    input.candidate_risks.reliability = not_applicable_risk("invalid_na");
    input.candidate_risks.dimensional = not_applicable_risk("no_declared_dimension");
    input.candidate_risks.protected_region = not_applicable_risk("no_protected_region");
    input.candidate_metrics.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});

    const auto result = evaluate_candidate(input);
    CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
    CHECK(result.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::MissingEstimatedTimeEvidence,
        CandidateRejectionCode::MissingMaterialEvidence,
        CandidateRejectionCode::MissingAppearanceEvidence,
        CandidateRejectionCode::MissingStrengthEvidence,
        CandidateRejectionCode::MissingReliabilityEvidence,
    });
}

TEST_CASE("Ratio baselines must exceed the frozen positive minimum", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    for (RecommendationGoal goal : RECOMMENDATION_GOALS) {
        CandidateEvaluationInput zero_time = valid_input(goal);
        zero_time.baseline_metrics.estimated_time_seconds = MetricValue<double>::known(0.0, {"orca_estimate"});
        auto result = evaluate_candidate(zero_time);
        INFO(recommendation_goal_id(goal));
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingEstimatedTimeEvidence});

        CandidateEvaluationInput near_zero_time = valid_input(goal);
        near_zero_time.baseline_metrics.estimated_time_seconds =
            MetricValue<double>::known(CANDIDATE_RATIO_DENOMINATOR_MINIMUM, {"orca_estimate"});
        near_zero_time.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(
            goal == RecommendationGoal::Speed ? CANDIDATE_RATIO_DENOMINATOR_MINIMUM * 0.9 :
                                                CANDIDATE_RATIO_DENOMINATOR_MINIMUM,
            {"orca_estimate"});
        result = evaluate_candidate(near_zero_time);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingEstimatedTimeEvidence});

        CandidateEvaluationInput zero_material = valid_input(goal);
        zero_material.baseline_metrics.model_material_volume_mm3 = MetricValue<double>::known(0.0, {"model"});
        zero_material.baseline_metrics.total_material_volume_mm3 = MetricValue<double>::known(0.0, {"total"});
        result = evaluate_candidate(zero_material);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingMaterialEvidence});

        CandidateEvaluationInput near_zero_material = valid_input(goal);
        near_zero_material.baseline_metrics.model_material_volume_mm3 =
            MetricValue<double>::known(CANDIDATE_RATIO_DENOMINATOR_MINIMUM, {"model"});
        near_zero_material.baseline_metrics.total_material_volume_mm3 =
            MetricValue<double>::known(CANDIDATE_RATIO_DENOMINATOR_MINIMUM, {"total"});
        result = evaluate_candidate(near_zero_material);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingMaterialEvidence});

        CandidateEvaluationInput zero_candidate_time = valid_input(goal);
        zero_candidate_time.candidate_metrics.estimated_time_seconds =
            MetricValue<double>::known(0.0, {"orca_estimate"});
        result = evaluate_candidate(zero_candidate_time);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingEstimatedTimeEvidence});

        CandidateEvaluationInput near_zero_candidate_time = valid_input(goal);
        near_zero_candidate_time.candidate_metrics.estimated_time_seconds =
            MetricValue<double>::known(CANDIDATE_RATIO_DENOMINATOR_MINIMUM, {"orca_estimate"});
        result = evaluate_candidate(near_zero_candidate_time);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingEstimatedTimeEvidence});

        CandidateEvaluationInput zero_candidate_material = valid_input(goal);
        set_material_total(zero_candidate_material.candidate_metrics, 0.0);
        result = evaluate_candidate(zero_candidate_material);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingMaterialEvidence});

        CandidateEvaluationInput near_zero_candidate_material = valid_input(goal);
        set_material_total(near_zero_candidate_material.candidate_metrics,
                           CANDIDATE_RATIO_DENOMINATOR_MINIMUM);
        result = evaluate_candidate(near_zero_candidate_material);
        CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
        CHECK(result.rejection_codes ==
              std::vector<CandidateRejectionCode>{CandidateRejectionCode::MissingMaterialEvidence});
    }

    CandidateEvaluationInput above_minimum = valid_input(RecommendationGoal::Speed);
    above_minimum.baseline_metrics.estimated_time_seconds = MetricValue<double>::known(
        CANDIDATE_RATIO_DENOMINATOR_MINIMUM * 2.0, {"orca_estimate"});
    above_minimum.candidate_metrics.estimated_time_seconds = MetricValue<double>::known(
        CANDIDATE_RATIO_DENOMINATOR_MINIMUM * 1.8, {"orca_estimate"});
    CHECK(evaluate_candidate(above_minimum).accepted());
}

TEST_CASE("Known reliability and protected-region risk still require gate evidence", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput input = valid_input();
    input.candidate_risks.reliability.hard_gate_acceptable.reset();
    input.candidate_risks.protected_region.hard_gate_acceptable.reset();
    const auto result = evaluate_candidate(input);
    CHECK(result.stage == CandidateEvaluationStage::MissingEvidence);
    CHECK(result.rejection_codes == std::vector<CandidateRejectionCode>{
        CandidateRejectionCode::MissingReliabilityEvidence,
        CandidateRejectionCode::MissingProtectedRegionEvidence,
    });
}

TEST_CASE("Normalization and rejection order are independent of input order", "[AI][SmartSlicing][D5T1][CandidateEvaluation]")
{
    CandidateEvaluationInput first = valid_input();
    first.candidate_metrics.native_diagnostics = {
        {NativeDiagnosticSeverity::Error, "z", "z"},
        {NativeDiagnosticSeverity::Error, "a", "a"},
    };
    first.candidate_metrics.effective_parameters = {
        {"plate", "z", "1", "profile"},
        {"plate", "a", "1", "profile"},
    };
    first.candidate_risks.appearance.evidence = {{"z", "2"}, {"a", "1"}};
    first.candidate_metrics.materials_compatible = MetricValue<bool>::known(false, {"z", "a"});

    CandidateEvaluationInput second = first;
    std::reverse(second.candidate_metrics.native_diagnostics.begin(), second.candidate_metrics.native_diagnostics.end());
    std::reverse(second.candidate_metrics.effective_parameters.begin(), second.candidate_metrics.effective_parameters.end());
    std::reverse(second.candidate_risks.appearance.evidence.begin(), second.candidate_risks.appearance.evidence.end());
    std::reverse(second.candidate_metrics.materials_compatible.evidence_codes.begin(),
                 second.candidate_metrics.materials_compatible.evidence_codes.end());

    CHECK(evaluate_candidate(first).rejection_codes == evaluate_candidate(second).rejection_codes);
    CHECK(normalize_trial_metrics(first.candidate_metrics).metrics.native_diagnostics.front().code == "a");
    CHECK(normalize_risk_assessment(first.candidate_risks).assessment.appearance.evidence.front().code == "a");
}

TEST_CASE("Goal contract values used by evaluation remain frozen", "[AI][SmartSlicing][D5T1][GoalContract]")
{
    const GoalContract& speed = goal_contract(RecommendationGoal::Speed);
    REQUIRE(speed.minimum_time_reduction_ratio);
    CHECK(*speed.minimum_time_reduction_ratio == Catch::Approx(0.10));
    CHECK(speed.maximum_material_ratio == Catch::Approx(1.0));
    CHECK(speed.minimum_strength_ratio == Catch::Approx(0.95));
    CHECK(speed.general_minimum_wall_loops == 2);
    CHECK(speed.general_minimum_infill_percent == Catch::Approx(15.0));
}
