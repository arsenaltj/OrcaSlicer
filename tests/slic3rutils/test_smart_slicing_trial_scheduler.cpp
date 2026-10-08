#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/TrialSliceScheduler.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

RiskComponent known_risk(double value)
{
    RiskComponent result;
    result.availability = MetricAvailability::Known;
    result.evidence = {{"trial_fixture", std::to_string(value)}};
    result.normalized_risk = value;
    result.retained_strength_ratio = 1.0;
    result.hard_gate_acceptable = true;
    return result;
}

RiskAssessment risks(double value)
{
    RiskAssessment result;
    result.appearance = known_risk(value);
    result.dimensional = known_risk(value);
    result.strength = known_risk(value);
    result.reliability = known_risk(value);
    result.protected_region = known_risk(value);
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
    result.layer_tool_sequences =
        MetricValue<std::vector<LayerToolSequence>>::known({}, {"single_material"});
    result.physical_slots_compatible = MetricValue<bool>::not_applicable({"single_material"});
    result.materials_compatible = MetricValue<bool>::known(true, {"material_registry"});
    result.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});
    result.wipe_tower_enabled = MetricValue<bool>::not_applicable({"single_material"});
    return result;
}

VersionedTrialSliceResult succeeded(const TrialSliceTask& task, bool baseline = false)
{
    VersionedTrialSliceResult result;
    result.identity = task.identity;
    result.status = TrialSliceStatus::Succeeded;
    result.metrics = metrics(baseline ? 100.0 : 80.0, baseline ? 100.0 : 80.0);
    result.risks = risks(baseline ? 0.5 : 0.2);
    result.evaluation_facts.manual_intent_preserved =
        MetricValue<bool>::known(true, {"proposal_validator"});
    result.evaluation_facts.effective_wall_loops =
        MetricValue<int>::known(2, {"effective_config"});
    result.evaluation_facts.effective_infill_percent =
        MetricValue<double>::known(20.0, {"effective_config"});
    result.evaluation_facts.critical_surface_significantly_improved =
        MetricValue<bool>::known(false, {"risk_assessment"});
    return result;
}

CandidateSearchDraft draft(RecommendationGoal goal, std::string id, double cost)
{
    CandidateSearchDraft result;
    result.goal = goal;
    result.candidate_id = std::move(id);
    result.estimated_trial_cost = cost;
    result.status = CandidateStatus::Draft;
    return result;
}

CandidateSearchResult search_result(size_t candidates_per_goal = 2)
{
    CandidateSearchResult result;
    result.budget_version = "strategy-v1";
    result.trial_cost_policy_version = CANDIDATE_TRIAL_COST_POLICY_VERSION;
    result.baseline.candidate_id = "baseline";
    result.baseline.workspace_revision = {1, 2, 3, "scheduler-fixture"};
    result.baseline.trial_slots = 1;
    result.total_trial_slots = 1;
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        GoalCandidateDrafts& goal_result = result.goal(goal);
        goal_result.goal = goal;
        const std::string prefix = recommendation_goal_id(goal);
        for (size_t index = 0; index < candidates_per_goal; ++index) {
            const double cost = index == 0 ? 1.0 : 2.0;
            goal_result.selected_for_trial.push_back(
                draft(goal, prefix + (index == 0 ? "-a" : "-z"), cost));
            ++result.total_trial_slots;
        }
    }
    return result;
}

TrialSliceSchedulerInput scheduler_input(size_t candidates_per_goal = 2)
{
    TrialSliceSchedulerInput input;
    input.search_result = search_result(candidates_per_goal);
    const CandidateSearchSessionPlanResult planned =
        CandidateSearchSessionPlanner().plan(input.search_result, 41, 3);
    REQUIRE(planned.accepted());
    input.session_plan = *planned.plan;
    input.machine_support_status = MachineSupportStatus::Enabled;
    input.cancellation_token = RecommendationSessionCoordinator().cancellation_token();
    input.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    return input;
}

} // namespace

TEST_CASE("trial scheduler runs baseline once and freezes each goal after first Ready",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input();
    std::vector<RecommendationTaskResult> published;
    size_t active = 0;
    size_t maximum_active = 0;
    const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [&](const TrialSliceTask& task,
            const std::shared_ptr<const RecommendationCancellationToken>& token) {
            CHECK(token == input.cancellation_token);
            ++active;
            maximum_active = std::max(maximum_active, active);
            VersionedTrialSliceResult result = succeeded(task, task.baseline());
            --active;
            return result;
        },
        [&](RecommendationTaskResult result) { published.push_back(std::move(result)); });

    CHECK(report.status == TrialSliceSchedulerStatus::Completed);
    CHECK(report.baseline_execution_count == 1);
    CHECK(report.total_execution_count == 4);
    CHECK(report.goal_execution_counts == std::array<size_t, 3>{1, 1, 1});
    CHECK(report.maximum_concurrency == 1);
    CHECK(maximum_active == 1);
    CHECK(report.execution_order ==
          std::vector<CandidateId>{"baseline", "balanced-a", "speed-a", "quality-a"});
    REQUIRE(published.size() == 4);
    CHECK(published[0].identity.goal_id == BASELINE_GOAL_ID);
    CHECK(published[0].outcome == RecommendationTaskOutcome::Ready);
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        CHECK(published[index + 1].identity.goal_id == recommendation_goal_id(RECOMMENDATION_GOALS[index]));
        CHECK(published[index + 1].outcome == RecommendationTaskOutcome::Ready);
    }
}

TEST_CASE("trial scheduler uses the versioned cost proxy across shuffled goals",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input();
    auto& balanced = input.search_result.goal(RecommendationGoal::Balanced).selected_for_trial;
    balanced = {draft(RecommendationGoal::Balanced, "balanced-high", 9.0),
                draft(RecommendationGoal::Balanced, "balanced-tie", 4.0)};
    auto& speed = input.search_result.goal(RecommendationGoal::Speed).selected_for_trial;
    speed = {draft(RecommendationGoal::Speed, "speed-high", 8.0),
             draft(RecommendationGoal::Speed, "speed-low", 1.0)};
    auto& quality = input.search_result.goal(RecommendationGoal::Quality).selected_for_trial;
    quality = {draft(RecommendationGoal::Quality, "quality-high", 7.0),
               draft(RecommendationGoal::Quality, "quality-tie", 4.0)};
    const CandidateSearchSessionPlanResult planned =
        CandidateSearchSessionPlanner().plan(input.search_result, 41, 3);
    REQUIRE(planned.accepted());
    input.session_plan = *planned.plan;

    const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [](const TrialSliceTask& task,
           const std::shared_ptr<const RecommendationCancellationToken>&) {
            return succeeded(task, task.baseline());
        },
        [](RecommendationTaskResult) {});

    CHECK(report.status == TrialSliceSchedulerStatus::Completed);
    CHECK(report.execution_order ==
          std::vector<CandidateId>{"baseline", "speed-low", "balanced-tie", "quality-tie"});
}

TEST_CASE("trial scheduler rejects inconsistent and over-budget plans before execution",
          "[AI][SmartSlicing][D5T3]")
{
    for (const bool inconsistent_count : {false, true}) {
        TrialSliceSchedulerInput input = scheduler_input(inconsistent_count ? 2 : 3);
        if (inconsistent_count)
            --input.search_result.total_trial_slots;
        else
            input.search_result.goal(RecommendationGoal::Balanced).selected_for_trial.push_back(
                draft(RecommendationGoal::Balanced, "balanced-over-budget", 3.0));

        size_t execution_count = 0;
        size_t publication_count = 0;
        const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
            input,
            [&](const TrialSliceTask& task,
                const std::shared_ptr<const RecommendationCancellationToken>&) {
                ++execution_count;
                return succeeded(task, task.baseline());
            },
            [&](RecommendationTaskResult) { ++publication_count; });
        CHECK(report.status == TrialSliceSchedulerStatus::InvalidInput);
        CHECK(execution_count == 0);
        CHECK(publication_count == 0);
    }
}

TEST_CASE("trial scheduler rejects an unbound trial cost policy before execution",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input(1);
    input.search_result.trial_cost_policy_version = "candidate-trial-cost/v2";
    size_t execution_count = 0;
    size_t publication_count = 0;

    const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [&](const TrialSliceTask& task,
            const std::shared_ptr<const RecommendationCancellationToken>&) {
            ++execution_count;
            return succeeded(task, task.baseline());
        },
        [&](RecommendationTaskResult) { ++publication_count; });

    CHECK(report.status == TrialSliceSchedulerStatus::InvalidInput);
    CHECK(execution_count == 0);
    CHECK(publication_count == 0);
}

TEST_CASE("trial scheduler fails only the goal whose versioned result identity mismatches",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input(1);
    std::vector<RecommendationTaskResult> published;
    TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [](const TrialSliceTask& task,
           const std::shared_ptr<const RecommendationCancellationToken>&) {
            VersionedTrialSliceResult result = succeeded(task, task.baseline());
            if (task.goal == RecommendationGoal::Balanced)
                ++result.identity.attempt_id;
            return result;
        },
        [&](RecommendationTaskResult result) { published.push_back(std::move(result)); });

    CHECK(report.status == TrialSliceSchedulerStatus::Completed);
    const auto balanced = std::find_if(published.begin(), published.end(), [](const auto& result) {
        return result.identity.goal_id == recommendation_goal_id(RecommendationGoal::Balanced);
    });
    REQUIRE(balanced != published.end());
    CHECK(balanced->outcome == RecommendationTaskOutcome::Failed);
    CHECK(balanced->diagnostic_codes == std::vector<std::string>{"trial_result_contract_invalid"});
}

TEST_CASE("pending machine validation publishes conservative terminals without slicing",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input(1);
    input.machine_support_status = MachineSupportStatus::PendingValidation;
    size_t execution_count = 0;
    std::vector<RecommendationTaskResult> published;
    const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [&](const TrialSliceTask& task,
            const std::shared_ptr<const RecommendationCancellationToken>&) {
            ++execution_count;
            return succeeded(task, task.baseline());
        },
        [&](RecommendationTaskResult result) { published.push_back(std::move(result)); });

    CHECK(report.status == TrialSliceSchedulerStatus::Blocked);
    CHECK(execution_count == 0);
    REQUIRE(published.size() == 4);
    CHECK(std::all_of(published.begin(), published.end(), [](const auto& result) {
        return result.outcome == RecommendationTaskOutcome::Unavailable &&
               result.diagnostic_codes ==
                   std::vector<std::string>{"machine_capability_pending_validation"};
    }));
}

TEST_CASE("technical trial failure is not reported as a product-level unavailable result",
          "[AI][SmartSlicing][D5T3]")
{
    TrialSliceSchedulerInput input = scheduler_input(1);
    std::vector<RecommendationTaskResult> published;
    const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
        input,
        [](const TrialSliceTask& task,
           const std::shared_ptr<const RecommendationCancellationToken>&) {
            VersionedTrialSliceResult result = succeeded(task, task.baseline());
            if (task.goal == RecommendationGoal::Balanced) {
                result.status = TrialSliceStatus::Failed;
                result.metrics.reset();
                result.risks.reset();
            }
            return result;
        },
        [&](RecommendationTaskResult result) { published.push_back(std::move(result)); });

    CHECK(report.status == TrialSliceSchedulerStatus::Completed);
    const auto balanced = std::find_if(published.begin(), published.end(), [](const auto& result) {
        return result.identity.goal_id == recommendation_goal_id(RecommendationGoal::Balanced);
    });
    REQUIRE(balanced != published.end());
    CHECK(balanced->outcome == RecommendationTaskOutcome::Failed);
    CHECK(balanced->diagnostic_codes == std::vector<std::string>{"trial_slice_execution_failed"});
}

TEST_CASE("trial scheduler shares cancellation and enforces the workflow deadline",
          "[AI][SmartSlicing][D5T3]")
{
    SECTION("deadline before baseline")
    {
        const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{50}};
        TrialSliceSchedulerInput input = scheduler_input(1);
        input.deadline = now;
        size_t execution_count = 0;
        size_t publication_count = 0;
        const TrialSliceSchedulerReport report = TrialSliceScheduler([&] { return now; }).run(
            input,
            [&](const TrialSliceTask& task,
                const std::shared_ptr<const RecommendationCancellationToken>&) {
                ++execution_count;
                return succeeded(task, task.baseline());
            },
            [&](RecommendationTaskResult) { ++publication_count; });
        CHECK(report.status == TrialSliceSchedulerStatus::DeadlineExceeded);
        CHECK(execution_count == 0);
        CHECK(publication_count == 4);
    }

    SECTION("shared token canceled after baseline")
    {
        TrialSliceSchedulerInput input = scheduler_input(1);
        RecommendationSessionCoordinator coordinator;
        coordinator.enqueue(input.session_plan.start_command);
        REQUIRE(coordinator.process_all() == 1);
        input.cancellation_token = coordinator.cancellation_token();
        size_t execution_count = 0;
        const TrialSliceSchedulerReport report = TrialSliceScheduler().run(
            input,
            [&](const TrialSliceTask& task,
                const std::shared_ptr<const RecommendationCancellationToken>& token) {
                CHECK(token == input.cancellation_token);
                ++execution_count;
                VersionedTrialSliceResult result = succeeded(task, task.baseline());
                if (task.baseline()) {
                    coordinator.enqueue(CancelRecommendationSessionCommand{
                        input.session_plan.start_command.workflow_id,
                        input.session_plan.start_command.attempt_id,
                        RecommendationCancellationReason::User});
                    coordinator.process_all();
                }
                return result;
            },
            [](RecommendationTaskResult) {});
        CHECK(report.status == TrialSliceSchedulerStatus::Canceled);
        CHECK(execution_count == 1);
    }

    SECTION("candidate result finishing after deadline is never published Ready")
    {
        auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{50}};
        TrialSliceSchedulerInput input = scheduler_input(1);
        input.deadline = now + std::chrono::seconds{10};
        std::vector<RecommendationTaskResult> published;
        const TrialSliceSchedulerReport report = TrialSliceScheduler([&] { return now; }).run(
            input,
            [&](const TrialSliceTask& task,
                const std::shared_ptr<const RecommendationCancellationToken>&) {
                VersionedTrialSliceResult result = succeeded(task, task.baseline());
                if (!task.baseline())
                    now = input.deadline;
                return result;
            },
            [&](RecommendationTaskResult result) { published.push_back(std::move(result)); });

        CHECK(report.status == TrialSliceSchedulerStatus::DeadlineExceeded);
        REQUIRE(published.size() == 4);
        CHECK(published.front().identity.goal_id == BASELINE_GOAL_ID);
        CHECK(published.front().outcome == RecommendationTaskOutcome::Ready);
        CHECK(std::none_of(published.begin() + 1, published.end(), [](const auto& result) {
            return result.outcome == RecommendationTaskOutcome::Ready;
        }));
        CHECK(std::all_of(published.begin() + 1, published.end(), [](const auto& result) {
            return result.outcome == RecommendationTaskOutcome::Unavailable &&
                   result.diagnostic_codes == std::vector<std::string>{"deadline_exceeded"};
        }));
    }
}
