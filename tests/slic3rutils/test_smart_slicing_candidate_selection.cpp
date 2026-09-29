#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/CandidateSelection.hpp"
#include "slic3r/AI/SmartSlicing/Application/CandidateSelectionTaskMapper.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

RiskComponent known_risk(double value)
{
    RiskComponent risk;
    risk.availability = MetricAvailability::Known;
    risk.evidence = {{"test_proxy", std::to_string(value)}};
    risk.normalized_risk = value;
    risk.hard_gate_acceptable = true;
    return risk;
}

RiskComponent not_applicable_risk()
{
    RiskComponent risk;
    risk.availability = MetricAvailability::NotApplicable;
    risk.evidence = {{"not_declared", "not_applicable"}};
    return risk;
}

RiskAssessment risks(double appearance = 0.5,
                     double dimensional = 0.5,
                     double strength = 0.5,
                     double reliability = 0.5,
                     double protected_region = 0.5)
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

TrialMetrics metrics(double time, double material)
{
    TrialMetrics result;
    result.estimated_time_seconds = MetricValue<double>::known(time, {"orca_time"});
    result.model_material_volume_mm3 = MetricValue<double>::known(material, {"orca_model_material"});
    result.support_material_volume_mm3 = MetricValue<double>::not_applicable({"support_disabled"});
    result.flush_material_volume_mm3 = MetricValue<double>::not_applicable({"single_material"});
    result.wipe_tower_material_volume_mm3 = MetricValue<double>::not_applicable({"single_material"});
    result.total_material_volume_mm3 = MetricValue<double>::known(material, {"orca_total_material"});
    result.tool_change_count = MetricValue<size_t>::known(0, {"single_material"});
    result.tool_change_time_seconds = MetricValue<double>::not_applicable({"single_material"});
    result.layer_tool_sequences = MetricValue<std::vector<LayerToolSequence>>::known({}, {"single_material"});
    result.physical_slots_compatible = MetricValue<bool>::not_applicable({"single_material"});
    result.materials_compatible = MetricValue<bool>::known(true, {"material_registry"});
    result.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});
    result.wipe_tower_enabled = MetricValue<bool>::not_applicable({"single_material"});
    return result;
}

CandidateSelectionBinding binding()
{
    CandidateSelectionBinding result;
    result.workflow_id = 41;
    result.attempt_id = 3;
    result.workspace_revision = {1, 2, 3, "revision-a"};
    result.baseline_candidate_id = "baseline";
    result.goal_task_candidate_id = "goal-selection-task";
    result.strategy_version = "strategy-v1";
    return result;
}

CandidateSelectionInput input(RecommendationGoal goal)
{
    CandidateSelectionInput result;
    result.binding = binding();
    result.goal = goal;
    result.baseline_metrics = metrics(100.0, 100.0);
    result.baseline_risks = risks();
    return result;
}

EvaluatedCandidate candidate(const CandidateSelectionInput& input,
                             std::string id,
                             double time,
                             double material,
                             RiskAssessment assessment)
{
    EvaluatedCandidate result;
    result.binding = input.binding;
    result.candidate_id = std::move(id);
    result.evaluation_input.goal = input.goal;
    result.evaluation_input.baseline_metrics = input.baseline_metrics;
    result.evaluation_input.candidate_metrics = metrics(time, material);
    result.evaluation_input.baseline_risks = input.baseline_risks;
    result.evaluation_input.candidate_risks = std::move(assessment);
    result.evaluation_input.manual_intent_preserved = MetricValue<bool>::known(true, {"proposal_validator"});
    result.evaluation_input.effective_wall_loops = MetricValue<int>::known(2, {"effective_config"});
    result.evaluation_input.effective_infill_percent = MetricValue<double>::known(15.0, {"effective_config"});
    result.evaluation_input.critical_surface_significantly_improved =
        MetricValue<bool>::known(false, {"risk_assessment"});
    result.evaluation = evaluate_candidate(result.evaluation_input);
    return result;
}

} // namespace

TEST_CASE("Pareto removes dominated candidates and keeps tradeoffs deterministically",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Quality);
    selection.candidates = {
        candidate(selection, "dominator", 90.0, 90.0, risks(0.30, 0.30, 0.30, 0.30, 0.30)),
        candidate(selection, "dominated", 95.0, 95.0, risks(0.40, 0.40, 0.40, 0.40, 0.40)),
        candidate(selection, "tradeoff", 80.0, 95.0, risks(0.40, 0.20, 0.20, 0.30, 0.30)),
    };

    const CandidateSelectionResult selected = select_candidate(selection);
    CHECK(selected.status == CandidateSelectionStatus::Ready);
    CHECK(selected.pareto_candidate_ids == std::vector<CandidateId>{"dominator", "tradeoff"});
    CHECK(selected.selected_candidate_id == "dominator");

    std::reverse(selection.candidates.begin(), selection.candidates.end());
    const CandidateSelectionResult permuted = select_candidate(selection);
    CHECK(permuted.pareto_candidate_ids == selected.pareto_candidate_ids);
    CHECK(permuted.selected_candidate_id == selected.selected_candidate_id);
}

TEST_CASE("Rejected and missing-evidence candidates never enter Pareto", "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Balanced);
    EvaluatedCandidate rejected = candidate(selection, "hard-gate", 60.0, 60.0, risks(0.1, 0.1, 0.1, 0.1, 0.1));
    rejected.evaluation.stage = CandidateEvaluationStage::CommonHardGates;
    rejected.evaluation.rejection_codes = {CandidateRejectionCode::ManualIntentConflict};
    EvaluatedCandidate missing = candidate(selection, "unknown-risk", 70.0, 70.0, risks(0.2, 0.2, 0.2, 0.2, 0.2));
    missing.evaluation_input.candidate_risks.dimensional = {};
    selection.candidates = {std::move(rejected), std::move(missing)};

    const CandidateSelectionResult result = select_candidate(selection);
    CHECK(result.status == CandidateSelectionStatus::Unavailable);
    CHECK(result.pareto_candidate_ids.empty());
    CHECK(result.excluded_candidate_ids == std::vector<CandidateId>{"hard-gate", "unknown-risk"});
    CHECK(result.diagnostic_codes == std::vector<std::string>{"no_eligible_candidate"});

    CandidateSelectionInput speed = input(RecommendationGoal::Speed);
    EvaluatedCandidate forged = candidate(speed, "forged-passed", 95.0, 100.0, risks());
    forged.evaluation.stage = CandidateEvaluationStage::Passed;
    forged.evaluation.rejection_codes.clear();
    speed.candidates = {std::move(forged)};
    const CandidateSelectionResult rechecked = select_candidate(speed);
    CHECK(rechecked.status == CandidateSelectionStatus::Unavailable);
    CHECK(rechecked.excluded_candidate_ids == std::vector<CandidateId>{"forged-passed"});
}

TEST_CASE("Not-applicable dimensions are not converted to zero or false improvement",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Balanced);
    selection.baseline_risks.dimensional = not_applicable_risk();
    selection.baseline_risks.protected_region = not_applicable_risk();
    RiskAssessment equal = risks();
    equal.dimensional = not_applicable_risk();
    equal.protected_region = not_applicable_risk();
    selection.candidates = {candidate(selection, "equal", 100.0, 100.0, std::move(equal))};

    const CandidateSelectionResult result = select_candidate(selection);
    CHECK(result.status == CandidateSelectionStatus::Unavailable);
    CHECK(result.diagnostic_codes == std::vector<std::string>{"no_explainable_benefit"});

    RiskAssessment incomparable = risks(0.4, 0.4, 0.4, 0.4, 0.4);
    incomparable.protected_region = known_risk(0.4);
    selection.candidates = {candidate(selection, "incomparable", 90.0, 90.0, std::move(incomparable))};
    const CandidateSelectionResult excluded = select_candidate(selection);
    CHECK(excluded.status == CandidateSelectionStatus::Unavailable);
    CHECK(excluded.excluded_candidate_ids == std::vector<CandidateId>{"incomparable"});
    CHECK(excluded.diagnostic_codes == std::vector<std::string>{"no_eligible_candidate"});
}

TEST_CASE("Balanced uses only frozen weights and requires an explainable net benefit",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Balanced);
    selection.candidates = {
        candidate(selection, "quality", 100.0, 100.0, risks(0.40, 0.40, 0.40, 0.50, 0.50)),
        candidate(selection, "time", 80.0, 100.0, risks()),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    REQUIRE(result.status == CandidateSelectionStatus::Ready);
    CHECK(result.selected_candidate_id == "quality");
    REQUIRE(result.evidence);
    CHECK(result.evidence->selection_policy_version == CANDIDATE_SELECTION_POLICY_VERSION);
    CHECK(result.evidence->explanation_codes ==
          std::vector<std::string>{"appearance_risk_reduced", "dimensional_risk_reduced",
                                   "strength_risk_reduced"});
}

TEST_CASE("Balanced tie candidates must each clear the explainable benefit threshold",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Balanced);
    selection.candidates = {
        candidate(selection, "candidate-a", 100.0 * (1.0 - 1.0e-8),
                  100.0 * (1.0 + 1.2e-8), risks()),
        candidate(selection, "candidate-b", 100.0 * (1.0 - 8.0e-9),
                  100.0, risks()),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    REQUIRE(result.status == CandidateSelectionStatus::Ready);
    CHECK(result.pareto_candidate_ids ==
          std::vector<CandidateId>{"candidate-a", "candidate-b"});
    CHECK(result.selected_candidate_id == "candidate-b");
    REQUIRE(result.evidence);
    CHECK(result.evidence->explanation_codes ==
          std::vector<std::string>{"estimated_time_reduced"});
}

TEST_CASE("Speed uses documented stable lexicographic order without invented weights",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Speed);
    selection.candidates = {
        candidate(selection, "fast", 80.0, 100.0, risks(0.49, 0.49, 0.49, 0.49, 0.49)),
        candidate(selection, "reliable", 85.0, 90.0, risks(0.20, 0.20, 0.20, 0.20, 0.20)),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    REQUIRE(result.status == CandidateSelectionStatus::Ready);
    CHECK(result.selected_candidate_id == "fast");
    REQUIRE(result.evidence);
    REQUIRE(result.evidence->estimated_time_ratio.value);
    CHECK(*result.evidence->estimated_time_ratio.value == Catch::Approx(0.8));
}

TEST_CASE("Quality compares appearance dimensional and strength in stable order",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Quality);
    selection.candidates = {
        candidate(selection, "appearance", 120.0, 105.0, risks(0.20, 0.45, 0.45, 0.40, 0.40)),
        candidate(selection, "dimension", 110.0, 100.0, risks(0.30, 0.10, 0.10, 0.30, 0.30)),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    CHECK(result.status == CandidateSelectionStatus::Ready);
    CHECK(result.selected_candidate_id == "appearance");
    REQUIRE(result.evidence);
    CHECK(result.evidence->explanation_codes ==
          std::vector<std::string>{"appearance_risk_reduced", "dimensional_risk_reduced",
                                   "strength_risk_reduced", "reliability_risk_reduced"});
}

TEST_CASE("Quality requires an explainable quality-component benefit above epsilon",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Quality);
    selection.candidates = {
        candidate(selection, "equal", 100.0, 100.0, risks()),
        candidate(selection, "noise", 100.0, 100.0,
                  risks(0.5 - CANDIDATE_SELECTION_EPSILON * 0.5, 0.6, 0.6, 0.5, 0.5)),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    CHECK(result.status == CandidateSelectionStatus::Unavailable);
    CHECK(result.selected_candidate_id.empty());
    CHECK_FALSE(result.evidence.has_value());
    CHECK(result.diagnostic_codes ==
          std::vector<std::string>{"no_explainable_quality_benefit"});
}

TEST_CASE("Exact equality is resolved by stable candidate id", "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Speed);
    selection.candidates = {
        candidate(selection, "candidate-z", 80.0, 90.0, risks(0.4, 0.4, 0.4, 0.4, 0.4)),
        candidate(selection, "candidate-a", 80.0, 90.0, risks(0.4, 0.4, 0.4, 0.4, 0.4)),
    };

    const CandidateSelectionResult result = select_candidate(selection);
    CHECK(result.pareto_candidate_ids == std::vector<CandidateId>{"candidate-a", "candidate-z"});
    CHECK(result.selected_candidate_id == "candidate-a");
}

TEST_CASE("Lexicographic epsilon chains are stable across input order", "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Speed);
    selection.candidates = {
        candidate(selection, "candidate-a", 80.0 + 1.5 * CANDIDATE_SELECTION_EPSILON,
                  90.0, risks(0.5, 0.5, 0.5, 0.5, 0.5)),
        candidate(selection, "candidate-b", 80.0 + 0.75 * CANDIDATE_SELECTION_EPSILON,
                  95.0, risks(0.5, 0.5, 0.5, 0.4, 0.5)),
        candidate(selection, "candidate-c", 80.0, 100.0,
                  risks(0.5, 0.5, 0.5, 0.4, 0.3)),
    };

    const CandidateSelectionResult selected = select_candidate(selection);
    REQUIRE(selected.status == CandidateSelectionStatus::Ready);
    CHECK(selected.pareto_candidate_ids ==
          std::vector<CandidateId>{"candidate-a", "candidate-b", "candidate-c"});
    CHECK(selected.selected_candidate_id == "candidate-c");

    std::reverse(selection.candidates.begin(), selection.candidates.end());
    const CandidateSelectionResult reversed = select_candidate(selection);
    CHECK(reversed.pareto_candidate_ids == selected.pareto_candidate_ids);
    CHECK(reversed.selected_candidate_id == selected.selected_candidate_id);
}

TEST_CASE("Selection rejects cross-session binding and policy mismatches", "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Balanced);
    selection.candidates = {candidate(selection, "candidate", 90.0, 90.0, risks(0.4, 0.4, 0.4, 0.4, 0.4))};
    selection.candidates.front().binding.attempt_id += 1;
    CHECK(select_candidate(selection).status == CandidateSelectionStatus::InvalidInput);

    selection.candidates.front().binding = selection.binding;
    selection.binding.goal_contract_version = "smart-slicing-goal-contract/future";
    CHECK(select_candidate(selection).status == CandidateSelectionStatus::InvalidInput);

    selection.binding.goal_contract_version = GOAL_CONTRACT_VERSION;
    selection.binding.selection_policy_version = "candidate-selection-policy/future";
    const CandidateSelectionResult policy = select_candidate(selection);
    CHECK(policy.status == CandidateSelectionStatus::InvalidInput);
    CHECK(policy.diagnostic_codes == std::vector<std::string>{"invalid_selection_input"});
}

TEST_CASE("Selection mapper preserves task identity and publishes the actual winning candidate",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Speed);
    selection.candidates = {
        candidate(selection, "first-draft", 88.0, 95.0, risks(0.4, 0.4, 0.4, 0.4, 0.4)),
        candidate(selection, "second-draft", 80.0, 95.0, risks(0.4, 0.4, 0.4, 0.4, 0.4)),
        candidate(selection, "third-draft", 85.0, 90.0, risks(0.3, 0.3, 0.3, 0.3, 0.3)),
    };
    const CandidateSelectionResult selected = select_candidate(selection);
    REQUIRE(selected.status == CandidateSelectionStatus::Ready);
    REQUIRE(selected.selected_candidate_id == "second-draft");

    RecommendationTaskIdentity task_identity{selection.binding.workflow_id,
                                             selection.binding.attempt_id,
                                             selection.binding.workspace_revision,
                                             selection.binding.goal_task_candidate_id,
                                             "speed"};
    StartRecommendationSessionCommand start;
    start.workflow_id = selection.binding.workflow_id;
    start.attempt_id = selection.binding.attempt_id;
    start.workspace_revision = selection.binding.workspace_revision;
    start.baseline_candidate_id = selection.binding.baseline_candidate_id;
    start.goal_candidate_ids = {"balanced-task", selection.binding.goal_task_candidate_id,
                                "quality-task"};
    start.strategy_version = selection.binding.strategy_version;
    CandidateSelectionTaskContext expected_context{task_identity,
                                                   start.baseline_candidate_id,
                                                   start.strategy_version};
    const CandidateSelectionTaskMappingResult mapped =
        map_candidate_selection_task(selected, expected_context);
    REQUIRE(mapped.valid());
    CHECK(mapped.task_result->identity.candidate_id == "goal-selection-task");
    CHECK(mapped.task_result->selected_candidate_id == "second-draft");
    CHECK(mapped.task_result->outcome == RecommendationTaskOutcome::Ready);
    REQUIRE(mapped.task_result->evidence);

    RecommendationSessionCoordinator coordinator;
    coordinator.enqueue(std::move(start));
    REQUIRE(coordinator.process_all() == 1);
    coordinator.enqueue(*mapped.task_result);
    REQUIRE(coordinator.process_all() == 1);
    const GoalResult& published =
        coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Speed);
    CHECK(published.candidate_id == "goal-selection-task");
    CHECK(published.selected_candidate_id == "second-draft");
    CHECK(published.status == GoalResultStatus::Ready);

    CandidateSelectionResult unavailable = selected;
    unavailable.status = CandidateSelectionStatus::Unavailable;
    unavailable.selected_candidate_id.clear();
    unavailable.evidence.reset();
    unavailable.diagnostic_codes = {"no_eligible_candidate"};
    const CandidateSelectionTaskMappingResult unavailable_task =
        map_candidate_selection_task(unavailable, expected_context);
    REQUIRE(unavailable_task.valid());
    CHECK(unavailable_task.task_result->outcome == RecommendationTaskOutcome::Unavailable);
    CHECK(unavailable_task.task_result->selected_candidate_id.empty());
    CHECK_FALSE(unavailable_task.task_result->evidence.has_value());

    expected_context.task_identity.attempt_id += 1;
    const CandidateSelectionTaskMappingResult wrong_binding =
        map_candidate_selection_task(selected, expected_context);
    CHECK_FALSE(wrong_binding.valid());
    CHECK(wrong_binding.diagnostic_codes ==
          std::vector<std::string>{"selection_task_binding_mismatch"});

    CandidateSelectionResult invalid_policy = selected;
    invalid_policy.binding.goal_contract_version = "smart-slicing-goal-contract/future";
    expected_context.task_identity.attempt_id = selection.binding.attempt_id;
    CHECK_FALSE(map_candidate_selection_task(invalid_policy, expected_context).valid());

    CandidateSelectionTaskContext wrong_baseline = expected_context;
    wrong_baseline.baseline_candidate_id = "different-baseline";
    const CandidateSelectionTaskMappingResult rejected_baseline =
        map_candidate_selection_task(selected, wrong_baseline);
    CHECK_FALSE(rejected_baseline.valid());
    CHECK(rejected_baseline.diagnostic_codes ==
          std::vector<std::string>{"selection_task_binding_mismatch"});

    CandidateSelectionTaskContext wrong_strategy = expected_context;
    wrong_strategy.strategy_version = "different-strategy";
    const CandidateSelectionTaskMappingResult rejected_strategy =
        map_candidate_selection_task(selected, wrong_strategy);
    CHECK_FALSE(rejected_strategy.valid());
    CHECK(rejected_strategy.diagnostic_codes ==
          std::vector<std::string>{"selection_task_binding_mismatch"});

    CandidateSelectionResult selected_outside_frontier = selected;
    selected_outside_frontier.selected_candidate_id = "not-on-frontier";
    const CandidateSelectionTaskMappingResult invalid_selected =
        map_candidate_selection_task(selected_outside_frontier, expected_context);
    CHECK_FALSE(invalid_selected.valid());
    CHECK(invalid_selected.diagnostic_codes ==
          std::vector<std::string>{"invalid_ready_selection_result"});
}

TEST_CASE("Published recommendation evidence rejects malformed availability and codes",
          "[AI][SmartSlicing][D5T2]")
{
    CandidateSelectionInput selection = input(RecommendationGoal::Speed);
    selection.candidates = {
        candidate(selection, "ready", 80.0, 90.0, risks(0.4, 0.4, 0.4, 0.4, 0.4)),
    };
    const CandidateSelectionResult selected = select_candidate(selection);
    REQUIRE(selected.evidence);
    CHECK(validate_recommendation_evidence(*selected.evidence).empty());

    RecommendationEvidence malformed = *selected.evidence;
    malformed.estimated_time_ratio.value = std::numeric_limits<double>::infinity();
    CHECK_FALSE(validate_recommendation_evidence(malformed).empty());

    malformed = *selected.evidence;
    malformed.dimensional_risk = {EvidenceAvailability::NotApplicable, 0.0, "not_declared"};
    CHECK_FALSE(validate_recommendation_evidence(malformed).empty());

    malformed = *selected.evidence;
    malformed.protected_region_risk = {EvidenceAvailability::NotApplicable, std::nullopt, ""};
    CHECK_FALSE(validate_recommendation_evidence(malformed).empty());

    malformed = *selected.evidence;
    malformed.explanation_codes = {"Invalid Code"};
    CHECK_FALSE(validate_recommendation_evidence(malformed).empty());
}
