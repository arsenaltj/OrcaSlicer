#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "slic3r/AI/SmartSlicing/Application/RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Application/RecommendationWorkerFailure.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "slic3r/AI/SmartSlicing/Infrastructure/FileRecommendationSessionJournal.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::AI::SmartSlicing;

namespace {

WorkspaceRevision revision(std::string fingerprint)
{
    return {1, 2, 3, std::move(fingerprint)};
}

EvidenceValue<double> available(double value, std::string source)
{
    return {EvidenceAvailability::Available, value, std::move(source)};
}

RecommendationEvidence valid_evidence()
{
    RecommendationEvidence evidence;
    evidence.selection_policy_version = CANDIDATE_SELECTION_POLICY_VERSION;
    evidence.explanation_codes = {"estimated_time_reduced"};
    evidence.estimated_time_ratio = available(0.8, "orca_estimated_time_ratio");
    evidence.material_ratio = available(0.9, "orca_total_material_ratio");
    evidence.appearance_risk = available(0.2, "appearance_risk_assessment");
    evidence.dimensional_risk = available(0.2, "dimensional_risk_assessment");
    evidence.strength_risk = available(0.2, "strength_risk_assessment");
    evidence.retained_strength_ratio = available(0.98, "strength_retention_assessment");
    evidence.reliability_risk = available(0.2, "reliability_risk_assessment");
    evidence.protected_region_risk = available(0.2, "protected_region_risk_assessment");
    return evidence;
}

StartRecommendationSessionCommand start_command(WorkflowId workflow_id = 7,
                                                AttemptId attempt_id = 1,
                                                std::string fingerprint = "revision-a")
{
    return {workflow_id,
            attempt_id,
            revision(std::move(fingerprint)),
            "baseline-1",
            {"balanced-1", "speed-1", "quality-1"},
            "strategy-v1"};
}

RecommendationTaskResult result(const RecommendationSessionSnapshot& snapshot,
                                std::string goal_id,
                                RecommendationTaskOutcome outcome,
                                std::vector<std::string> diagnostics = {})
{
    CandidateId candidate_id;
    if (goal_id == BASELINE_GOAL_ID) {
        candidate_id = snapshot.recommendation.baseline.candidate_id;
    } else {
        const RecommendationGoalParseResult parsed = parse_recommendation_goal_id(goal_id);
        REQUIRE(parsed);
        candidate_id = snapshot.recommendation.goal_result(*parsed.goal).candidate_id;
    }
    RecommendationTaskResult task;
    task.identity = {snapshot.workflow_id, snapshot.attempt_id, snapshot.workspace_revision,
                     std::move(candidate_id), std::move(goal_id)};
    task.outcome = outcome;
    task.diagnostic_codes = std::move(diagnostics);
    if (task.identity.goal_id != BASELINE_GOAL_ID && outcome == RecommendationTaskOutcome::Ready) {
        task.selected_candidate_id = task.identity.goal_id + "-selected";
        task.evidence = valid_evidence();
    }
    return task;
}

std::shared_ptr<const OwnerRiskConfirmationContract> risk_contract_for(
    const RecommendationTaskResult& task, const RecommendationEvidence& evidence)
{
    const RecommendationGoalParseResult parsed =
        parse_recommendation_goal_id(task.identity.goal_id);
    REQUIRE(parsed);
    RiskConfirmationMappingInput input;
    input.publication_identity = {task.identity.workflow_id,
                                  task.identity.attempt_id,
                                  task.identity.workspace_revision,
                                  *parsed.goal,
                                  task.identity.candidate_id,
                                  task.selected_candidate_id};
    input.published_evidence = evidence;
    input.facts = {
        {RiskConfirmationKind::ProtectedRegionSupportContact,
         RiskConfirmationFactAvailability::Known, true,
         "protected_region_support_contact", "session-owner/v1"},
        {RiskConfirmationKind::ProtectedRegionSeam,
         RiskConfirmationFactAvailability::Known, false, "protected_region_seam",
         "session-owner/v1"},
    };
    const RiskConfirmationMappingResult mapped = RiskConfirmationMapper{}.map(input);
    REQUIRE(mapped.accepted());
    return mapped.contract;
}

void start(RecommendationSessionCoordinator& coordinator,
           WorkflowId workflow_id = 7,
           AttemptId attempt_id = 1,
           std::string fingerprint = "revision-a")
{
    coordinator.enqueue(start_command(workflow_id, attempt_id, std::move(fingerprint)));
    REQUIRE(coordinator.process_all() == 1);
    REQUIRE(coordinator.snapshot().state == RecommendationSessionState::Recommending);
}

CandidateSearchSessionPlan worker_plan()
{
    CandidateSearchSessionPlan plan;
    plan.start_command = start_command();
    plan.trial_cost_policy_version = CANDIDATE_TRIAL_COST_POLICY_VERSION;
    plan.baseline_task = {7, 1, revision("revision-a"), "baseline-1", BASELINE_GOAL_ID};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        plan.goal_tasks[index] = {7, 1, revision("revision-a"),
                                  plan.start_command.goal_candidate_ids[index],
                                  recommendation_goal_id(goal)};
    }
    return plan;
}

class MemoryRecommendationSessionJournal final : public IRecommendationSessionJournal
{
public:
    std::optional<RecommendationSessionJournalRecord> load() override
    {
        if (throw_on_load)
            throw std::runtime_error("load_failed");
        return record;
    }

    void save(const RecommendationSessionJournalRecord& value) override
    {
        if (throw_on_save)
            throw std::runtime_error("save_failed");
        record = value;
        ++save_count;
    }

    void clear() override
    {
        if (throw_on_clear)
            throw std::runtime_error("clear_failed");
        record.reset();
        ++clear_count;
    }

    std::optional<RecommendationSessionJournalRecord> record;
    bool throw_on_load{false};
    bool throw_on_save{false};
    bool throw_on_clear{false};
    size_t save_count{0};
    size_t clear_count{0};
};

RecommendationSessionJournalRecord interrupted_record()
{
    RecommendationSessionJournalRecord record;
    record.workflow_id = 99;
    record.attempt_id = 3;
    record.workspace_revision = revision("interrupted-revision");
    record.baseline = {BASELINE_GOAL_ID, GoalResultStatus::Ready, "baseline-old", {"baseline_ready"}};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        record.goals[index] = {recommendation_goal_id(goal), GoalResultStatus::Analyzing,
                               std::string(recommendation_goal_id(goal)) + "-old", {}};
    }
    record.contract_version = RECOMMENDATION_CONTRACT_VERSION;
    record.strategy_version = "strategy-v1";
    record.started_at_epoch_seconds = 1000;
    record.updated_at_epoch_seconds = 1010;
    return record;
}

std::filesystem::path unique_journal_path()
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("orcaslicer-smart-slicing-journal-" + std::to_string(suffix) + ".json");
}

} // namespace

TEST_CASE("recommendation session publishes baseline and goals independently in result order",
          "[AI][SmartSlicing][Session]")
{
    RecommendationSessionCoordinator coordinator;
    std::vector<uint64_t> publications;
    coordinator.set_observer([&](const RecommendationSessionSnapshot& snapshot) {
        publications.push_back(snapshot.publication_revision);
    });
    start(coordinator);

    coordinator.enqueue(result(coordinator.snapshot(), BASELINE_GOAL_ID, RecommendationTaskOutcome::Ready));
    coordinator.enqueue(result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Ready, {"quality-ready"}));
    coordinator.enqueue(result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Failed, {"balanced-failed"}));
    coordinator.enqueue(result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Unavailable, {"speed-unavailable"}));
    REQUIRE(coordinator.process_all() == 4);

    const RecommendationSessionSnapshot& snapshot = coordinator.snapshot();
    CHECK(snapshot.state == RecommendationSessionState::Ready);
    CHECK(snapshot.recommendation.baseline.status == GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Balanced).status == GoalResultStatus::Failed);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Balanced).diagnostic_codes ==
          std::vector<std::string>{"balanced-failed"});
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Speed).status == GoalResultStatus::Unavailable);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Quality).status == GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Quality).selected_candidate_id ==
          "quality-selected");
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Quality).diagnostic_codes ==
          std::vector<std::string>{"quality-ready"});
    CHECK(publications.size() == 6);
}

TEST_CASE("worker exception state is allocation-free and owner settlement preserves terminals",
          "[AI][SmartSlicing][Session][D5T3]")
{
    RecommendationWorkerState worker_state;
    worker_state.mark_started();
    std::thread worker([&]() noexcept {
        try {
            throw std::runtime_error("worker failure");
        } catch (...) {
            worker_state.mark_failed();
        }
    });
    worker.join();
    CHECK_FALSE(worker_state.running());
    REQUIRE(worker_state.consume_failure());
    CHECK_FALSE(worker_state.consume_failure());

    CandidateSearchSessionPlan plan = worker_plan();
    RecommendationSessionCoordinator coordinator;
    coordinator.enqueue(plan.start_command);
    REQUIRE(coordinator.process_all() == 1);
    coordinator.enqueue(result(coordinator.snapshot(), BASELINE_GOAL_ID,
                               RecommendationTaskOutcome::Ready));
    coordinator.enqueue(result(coordinator.snapshot(), "quality",
                               RecommendationTaskOutcome::Ready, {"quality-ready"}));
    REQUIRE(coordinator.process_all() == 2);

    CHECK(settle_recommendation_worker_exception(coordinator, plan) == 2);
    const RecommendationSessionSnapshot& snapshot = coordinator.snapshot();
    CHECK(snapshot.recommendation.baseline.candidate_id == "baseline-1");
    CHECK(snapshot.recommendation.baseline.status == GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Quality).status ==
          GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Quality).selected_candidate_id ==
          "quality-selected");
    for (const RecommendationGoal goal : {RecommendationGoal::Balanced, RecommendationGoal::Speed}) {
        const GoalResult& goal_result = snapshot.recommendation.goal_result(goal);
        CHECK(goal_result.candidate_id == plan.start_command.goal_candidate_ids[
              goal == RecommendationGoal::Balanced ? 0 : 1]);
        CHECK(goal_result.status == GoalResultStatus::Failed);
        CHECK(goal_result.diagnostic_codes ==
              std::vector<std::string>{RECOMMENDATION_WORKER_EXCEPTION_CODE});
    }
    CHECK(coordinator.discarded_result_count() == 0);

    CandidateSearchSessionPlan stale = plan;
    ++stale.start_command.attempt_id;
    ++stale.baseline_task.attempt_id;
    for (RecommendationTaskIdentity& identity : stale.goal_tasks)
        ++identity.attempt_id;
    CHECK(settle_recommendation_worker_exception(coordinator, stale) == 0);
    CHECK(coordinator.discarded_result_count() == 0);
}

TEST_CASE("cancel and revision changes discard late results without publishing", "[AI][SmartSlicing]")
{
    SECTION("cancel") {
        RecommendationSessionCoordinator coordinator;
        std::vector<RecommendationTaskIdentity> discarded;
        coordinator.set_discard_handler([&](const RecommendationTaskResult& value) {
            discarded.push_back(value.identity);
        });
        start(coordinator);
        const RecommendationTaskResult late =
            result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
        coordinator.enqueue(CancelRecommendationSessionCommand{7, 1});
        REQUIRE(coordinator.process_all() == 1);
        const uint64_t publication = coordinator.snapshot().publication_revision;
        coordinator.enqueue(late);
        REQUIRE(coordinator.process_all() == 1);
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Canceled);
        CHECK(coordinator.snapshot().publication_revision == publication);
        CHECK(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Balanced).status ==
              GoalResultStatus::Analyzing);
        CHECK(discarded.size() == 1);
    }

    SECTION("workspace revision") {
        RecommendationSessionCoordinator coordinator;
        start(coordinator);
        const RecommendationTaskResult late =
            result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Ready);
        coordinator.enqueue(WorkspaceRevisionChangedCommand{7, 1, revision("revision-b")});
        REQUIRE(coordinator.process_all() == 1);
        const uint64_t publication = coordinator.snapshot().publication_revision;
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Stale);
        CHECK(coordinator.snapshot().recommendation.baseline.status == GoalResultStatus::Stale);
        for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
            CHECK(coordinator.snapshot().recommendation.goal_result(goal).status == GoalResultStatus::Stale);
        coordinator.enqueue(late);
        REQUIRE(coordinator.process_all() == 1);
        CHECK(coordinator.snapshot().publication_revision == publication);
        CHECK(coordinator.discarded_result_count() == 1);
    }
}

TEST_CASE("task identity rejects attempt candidate and goal mismatches", "[AI][SmartSlicing]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator, 7, 1);
    coordinator.enqueue(start_command(7, 2));
    REQUIRE(coordinator.process_all() == 1);
    REQUIRE(coordinator.snapshot().attempt_id == 2);
    const uint64_t publication = coordinator.snapshot().publication_revision;

    RecommendationTaskResult wrong_workflow = result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
    wrong_workflow.identity.workflow_id = 6;
    RecommendationTaskResult old_attempt = result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
    old_attempt.identity.attempt_id = 1;
    RecommendationTaskResult wrong_revision = result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
    wrong_revision.identity.workspace_revision = revision("revision-b");
    RecommendationTaskResult wrong_candidate = result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Ready);
    wrong_candidate.identity.candidate_id = "different-candidate";
    RecommendationTaskResult wrong_goal = result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Ready);
    wrong_goal.identity.goal_id = "unknown-goal";
    coordinator.enqueue(std::move(wrong_workflow));
    coordinator.enqueue(std::move(old_attempt));
    coordinator.enqueue(std::move(wrong_revision));
    coordinator.enqueue(std::move(wrong_candidate));
    coordinator.enqueue(std::move(wrong_goal));
    REQUIRE(coordinator.process_all() == 5);

    CHECK(coordinator.snapshot().state == RecommendationSessionState::Recommending);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        CHECK(coordinator.snapshot().recommendation.goal_result(goal).status == GoalResultStatus::Analyzing);
    CHECK(coordinator.snapshot().publication_revision == publication);
    CHECK(coordinator.discarded_result_count() == 5);
}

TEST_CASE("published ready goal and evidence are immutable when duplicate results arrive",
          "[AI][SmartSlicing][Session][D5T2]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    RecommendationTaskResult ready =
        result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready, {"first-publication"});
    ready.selected_candidate_id = "balanced-second-candidate";
    coordinator.enqueue(ready);
    REQUIRE(coordinator.process_all() == 1);
    const uint64_t publication = coordinator.snapshot().publication_revision;

    ready.outcome = RecommendationTaskOutcome::Failed;
    ready.diagnostic_codes = {"late-overwrite"};
    ready.selected_candidate_id.clear();
    ready.evidence.reset();
    coordinator.enqueue(std::move(ready));
    REQUIRE(coordinator.process_all() == 1);

    const GoalResult& balanced = coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Balanced);
    CHECK(balanced.status == GoalResultStatus::Ready);
    CHECK(balanced.diagnostic_codes == std::vector<std::string>{"first-publication"});
    CHECK(balanced.selected_candidate_id == "balanced-second-candidate");
    REQUIRE(balanced.evidence);
    REQUIRE(balanced.evidence->estimated_time_ratio.value);
    CHECK(*balanced.evidence->estimated_time_ratio.value == Catch::Approx(0.8));
    CHECK(coordinator.snapshot().publication_revision == publication);
    CHECK(coordinator.discarded_result_count() == 1);
}

TEST_CASE("malformed recommendation publication payloads are discarded fail closed",
          "[AI][SmartSlicing][Session][D5T2]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    const uint64_t publication = coordinator.snapshot().publication_revision;

    RecommendationTaskResult missing_evidence =
        result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
    missing_evidence.evidence.reset();
    RecommendationTaskResult malformed_evidence =
        result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Ready);
    malformed_evidence.evidence->selection_policy_version = "candidate-selection-policy/future";
    RecommendationTaskResult unavailable_with_candidate =
        result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Unavailable);
    unavailable_with_candidate.selected_candidate_id = "quality-forged";
    unavailable_with_candidate.evidence = valid_evidence();

    coordinator.enqueue(std::move(missing_evidence));
    coordinator.enqueue(std::move(malformed_evidence));
    coordinator.enqueue(std::move(unavailable_with_candidate));
    REQUIRE(coordinator.process_all() == 3);

    CHECK(coordinator.snapshot().publication_revision == publication);
    CHECK(coordinator.discarded_result_count() == 3);
    for (RecommendationGoal goal : RECOMMENDATION_GOALS) {
        const GoalResult& goal_result = coordinator.snapshot().recommendation.goal_result(goal);
        CHECK(goal_result.status == GoalResultStatus::Analyzing);
        CHECK(goal_result.selected_candidate_id.empty());
        CHECK_FALSE(goal_result.evidence.has_value());
    }
}

TEST_CASE("owner Ready publication validates and freezes its risk confirmation contract",
          "[AI][SmartSlicing][Session][D6T2]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    RecommendationTaskResult ready =
        result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready,
               {"balanced_ready"});
    ready.risk_confirmation_contract = risk_contract_for(ready, *ready.evidence);
    const auto published_contract = ready.risk_confirmation_contract;

    coordinator.enqueue(std::move(ready));
    REQUIRE(coordinator.process_all() == 1);

    const GoalResult& goal =
        coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Balanced);
    REQUIRE(goal.status == GoalResultStatus::Ready);
    REQUIRE(goal.risk_confirmation_contract);
    CHECK(goal.risk_confirmation_contract == published_contract);
    CHECK(goal.risk_confirmation_contract->required_confirmations() ==
          std::vector<RiskConfirmationKind>{
              RiskConfirmationKind::ProtectedRegionSupportContact});
    CHECK(coordinator.discarded_result_count() == 0);
}

TEST_CASE("Ready publication may omit owner risk contract without synthesizing one",
          "[AI][SmartSlicing][Session][D6T2]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    coordinator.enqueue(
        result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready));
    REQUIRE(coordinator.process_all() == 1);

    const GoalResult& goal =
        coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Balanced);
    CHECK(goal.status == GoalResultStatus::Ready);
    CHECK(goal.evidence.has_value());
    CHECK_FALSE(goal.risk_confirmation_contract);
    CHECK(coordinator.discarded_result_count() == 0);
}

TEST_CASE("owner publication discards mismatched and invalid risk confirmation contracts",
          "[AI][SmartSlicing][Session][D6T2]")
{
    const auto check_rejected = [](
                                    const std::function<void(
                                        RecommendationTaskResult&)>& mutate) {
        RecommendationSessionCoordinator coordinator;
        start(coordinator);
        RecommendationTaskResult ready =
            result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
        ready.risk_confirmation_contract = risk_contract_for(ready, *ready.evidence);
        mutate(ready);
        coordinator.enqueue(std::move(ready));
        REQUIRE(coordinator.process_all() == 1);
        const GoalResult& goal = coordinator.snapshot().recommendation.goal_result(
            RecommendationGoal::Balanced);
        CHECK(goal.status == GoalResultStatus::Analyzing);
        CHECK_FALSE(goal.evidence);
        CHECK_FALSE(goal.risk_confirmation_contract);
        CHECK(coordinator.discarded_result_count() == 1);
    };

    SECTION("publication identity")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            ++data.publication_identity.attempt_id;
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
    SECTION("recommendation evidence")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            RecommendationEvidence other = *ready.evidence;
            other.protected_region_risk.value = 0.4;
            ready.risk_confirmation_contract = risk_contract_for(ready, other);
        });
    }
    SECTION("policy version")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            data.policy_version = "risk-confirmation-policy/future";
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
    SECTION("contract schema")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            data.schema = "orcaslicer.smart-slicing.owner-risk-confirmation.future";
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
    SECTION("contract version")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            data.version = "v2";
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
    SECTION("source evidence digest")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            data.source_evidence_digest.clear();
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
    SECTION("required confirmation set")
    {
        check_rejected([](RecommendationTaskResult& ready) {
            OwnerRiskConfirmationContractData data =
                ready.risk_confirmation_contract->data();
            data.required_confirmations.push_back(
                RiskConfirmationKind::ProtectedRegionSupportContact);
            ready.risk_confirmation_contract =
                OwnerRiskConfirmationContract::from_untrusted_data(std::move(data));
        });
    }
}

TEST_CASE("non-Ready results cannot carry owner risk confirmation contracts",
          "[AI][SmartSlicing][Session][D6T2]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    RecommendationTaskResult source =
        result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Ready);
    RecommendationTaskResult unavailable =
        result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Unavailable,
               {"no_candidate"});
    unavailable.risk_confirmation_contract = risk_contract_for(source, *source.evidence);

    coordinator.enqueue(std::move(unavailable));
    REQUIRE(coordinator.process_all() == 1);
    const GoalResult& goal =
        coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Speed);
    CHECK(goal.status == GoalResultStatus::Analyzing);
    CHECK_FALSE(goal.risk_confirmation_contract);
    CHECK(coordinator.discarded_result_count() == 1);
}

TEST_CASE("session transitions and new attempts do not inherit owner risk contracts",
          "[AI][SmartSlicing][Session][D6T2]")
{
    SECTION("workspace stale")
    {
        RecommendationSessionCoordinator coordinator;
        start(coordinator);
        RecommendationTaskResult ready =
            result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Ready);
        ready.risk_confirmation_contract = risk_contract_for(ready, *ready.evidence);
        coordinator.enqueue(std::move(ready));
        REQUIRE(coordinator.process_all() == 1);
        REQUIRE(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Quality)
                    .risk_confirmation_contract);

        coordinator.enqueue(WorkspaceRevisionChangedCommand{7, 1, revision("revision-b")});
        REQUIRE(coordinator.process_all() == 1);
        CHECK_FALSE(coordinator.snapshot().recommendation.goal_result(
            RecommendationGoal::Quality).risk_confirmation_contract);
    }
    SECTION("cancel and new attempt")
    {
        RecommendationSessionCoordinator coordinator;
        start(coordinator);
        RecommendationTaskResult ready =
            result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready);
        ready.risk_confirmation_contract = risk_contract_for(ready, *ready.evidence);
        coordinator.enqueue(std::move(ready));
        REQUIRE(coordinator.process_all() == 1);

        coordinator.enqueue(CancelRecommendationSessionCommand{7, 1});
        REQUIRE(coordinator.process_all() == 1);
        CHECK_FALSE(coordinator.snapshot().recommendation.goal_result(
            RecommendationGoal::Balanced).risk_confirmation_contract);

        coordinator.enqueue(start_command(7, 2));
        REQUIRE(coordinator.process_all() == 1);
        for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
            CHECK_FALSE(coordinator.snapshot().recommendation.goal_result(goal)
                            .risk_confirmation_contract);
    }
}

TEST_CASE("non-owner threads can enqueue but cannot mutate recommendation state", "[AI][SmartSlicing]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    const uint64_t publication = coordinator.snapshot().publication_revision;
    RecommendationTaskResult worker_result =
        result(coordinator.snapshot(), "quality", RecommendationTaskOutcome::Ready);
    std::atomic<bool> owner_assertion_observed{false};

    std::thread worker([&] {
        coordinator.enqueue(std::move(worker_result));
        try {
            coordinator.process_next();
        } catch (const std::logic_error&) {
            owner_assertion_observed = true;
        }
    });
    worker.join();

    CHECK(owner_assertion_observed);
    CHECK(coordinator.pending_command_count() == 1);
    CHECK(coordinator.snapshot().publication_revision == publication);
    CHECK(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Quality).status ==
          GoalResultStatus::Analyzing);
    REQUIRE(coordinator.process_all() == 1);
    CHECK(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Quality).status ==
          GoalResultStatus::Ready);
}

TEST_CASE("recommendation deadline triggers exactly at ten minutes and preserves completed results",
          "[AI][SmartSlicing]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    RecommendationSessionCoordinator coordinator([&] { return now; }, [] { return int64_t{1000}; });
    std::vector<RecommendationCancellationReason> cleanup_reasons;
    coordinator.set_cleanup_handler([&](RecommendationCancellationReason reason) {
        cleanup_reasons.push_back(reason);
    });
    start(coordinator);
    const std::shared_ptr<const RecommendationCancellationToken> token = coordinator.cancellation_token();

    coordinator.enqueue(result(coordinator.snapshot(), BASELINE_GOAL_ID, RecommendationTaskOutcome::Ready,
                               {"baseline_ready"}));
    coordinator.enqueue(result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Ready,
                               {"balanced_ready"}));
    REQUIRE(coordinator.process_all() == 2);
    const uint64_t before_deadline = coordinator.snapshot().publication_revision;

    now += minutes(9) + seconds(59);
    CHECK_FALSE(coordinator.check_deadline());
    CHECK(coordinator.snapshot().publication_revision == before_deadline);
    CHECK_FALSE(token->cancellation_requested());

    now += seconds(1);
    REQUIRE(coordinator.check_deadline());
    const RecommendationSessionSnapshot& snapshot = coordinator.snapshot();
    CHECK(snapshot.state == RecommendationSessionState::Ready);
    CHECK(snapshot.cancellation_reason == RecommendationCancellationReason::Deadline);
    CHECK(snapshot.recommendation.baseline.status == GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.baseline.diagnostic_codes == std::vector<std::string>{"baseline_ready"});
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Balanced).status == GoalResultStatus::Ready);
    CHECK(snapshot.recommendation.goal_result(RecommendationGoal::Balanced).diagnostic_codes ==
          std::vector<std::string>{"balanced_ready"});
    for (const RecommendationGoal goal : {RecommendationGoal::Speed, RecommendationGoal::Quality}) {
        const GoalResult& result = snapshot.recommendation.goal_result(goal);
        CHECK(result.status == GoalResultStatus::Unavailable);
        CHECK(result.diagnostic_codes == std::vector<std::string>{"deadline_exceeded"});
    }
    CHECK(token->cancellation_requested());
    CHECK(token->reason() == RecommendationCancellationReason::Deadline);
    CHECK(cleanup_reasons == std::vector<RecommendationCancellationReason>{RecommendationCancellationReason::Deadline});

    const uint64_t after_deadline = snapshot.publication_revision;
    CHECK_FALSE(coordinator.check_deadline());
    CHECK(coordinator.snapshot().publication_revision == after_deadline);
    CHECK(cleanup_reasons.size() == 1);

    coordinator.enqueue(result(coordinator.snapshot(), "speed", RecommendationTaskOutcome::Ready));
    REQUIRE(coordinator.process_all() == 1);
    CHECK(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Speed).status ==
          GoalResultStatus::Unavailable);
    CHECK(coordinator.discarded_result_count() == 1);
}

TEST_CASE("recommendation deadline starts when the user command is enqueued", "[AI][SmartSlicing]")
{
    using namespace std::chrono;
    steady_clock::time_point now{};
    RecommendationSessionCoordinator coordinator([&] { return now; }, [] { return int64_t{1000}; });
    coordinator.enqueue(start_command());
    now += minutes(10);

    REQUIRE(coordinator.process_all() == 1);
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Ready);
    CHECK(coordinator.snapshot().cancellation_reason == RecommendationCancellationReason::Deadline);
    CHECK(coordinator.snapshot().recommendation.baseline.status == GoalResultStatus::Unavailable);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        CHECK(coordinator.snapshot().recommendation.goal_result(goal).status == GoalResultStatus::Unavailable);
}

TEST_CASE("recommendation coordinator preserves an explicit shared session start time",
          "[AI][SmartSlicing][Session]")
{
    using namespace std::chrono;
    steady_clock::time_point now = steady_clock::time_point{} + minutes(5);
    RecommendationSessionCoordinator coordinator([&] { return now; }, [] { return int64_t{1000}; });
    StartRecommendationSessionCommand command = start_command();
    command.requested_at = steady_clock::time_point{};
    coordinator.enqueue(command);
    REQUIRE(coordinator.process_all() == 1);

    now = steady_clock::time_point{} + minutes(9);
    CHECK_FALSE(coordinator.check_deadline());
    now = steady_clock::time_point{} + minutes(10);
    CHECK(coordinator.check_deadline());
    CHECK(coordinator.snapshot().cancellation_reason == RecommendationCancellationReason::Deadline);
}

TEST_CASE("recommendation cancellation token distinguishes causes and cleans up once", "[AI][SmartSlicing]")
{
    SECTION("user cancellation") {
        RecommendationSessionCoordinator coordinator;
        std::vector<RecommendationCancellationReason> reasons;
        coordinator.set_cleanup_handler([&](RecommendationCancellationReason reason) { reasons.push_back(reason); });
        start(coordinator);
        const auto token = coordinator.cancellation_token();
        coordinator.enqueue(CancelRecommendationSessionCommand{7, 1});
        coordinator.enqueue(CancelRecommendationSessionCommand{7, 1});
        REQUIRE(coordinator.process_all() == 2);
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Canceled);
        CHECK(coordinator.snapshot().cancellation_reason == RecommendationCancellationReason::User);
        CHECK(token->reason() == RecommendationCancellationReason::User);
        CHECK(reasons == std::vector<RecommendationCancellationReason>{RecommendationCancellationReason::User});
    }

    SECTION("workspace revision change") {
        RecommendationSessionCoordinator coordinator;
        std::vector<RecommendationCancellationReason> reasons;
        coordinator.set_cleanup_handler([&](RecommendationCancellationReason reason) { reasons.push_back(reason); });
        start(coordinator);
        const auto token = coordinator.cancellation_token();
        coordinator.enqueue(WorkspaceRevisionChangedCommand{7, 1, revision("revision-b")});
        coordinator.enqueue(WorkspaceRevisionChangedCommand{7, 1, revision("revision-c")});
        REQUIRE(coordinator.process_all() == 2);
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Stale);
        CHECK(coordinator.snapshot().cancellation_reason == RecommendationCancellationReason::WorkspaceChanged);
        CHECK(token->reason() == RecommendationCancellationReason::WorkspaceChanged);
        CHECK(reasons ==
              std::vector<RecommendationCancellationReason>{RecommendationCancellationReason::WorkspaceChanged});
    }
}

TEST_CASE("a newer valid recommendation start supersedes every active old identity",
          "[AI][SmartSlicing][Session]")
{
    for (const bool complete_old_session : {false, true}) {
        DYNAMIC_SECTION("old state " <<
                        (complete_old_session ? "Ready" : "Recommending")) {
            MemoryRecommendationSessionJournal journal;
            RecommendationSessionCoordinator coordinator;
            coordinator.set_journal(journal);
            std::vector<RecommendationCancellationReason> cleanup_reasons;
            coordinator.set_cleanup_handler([&](RecommendationCancellationReason reason) {
                cleanup_reasons.push_back(reason);
            });
            start(coordinator);
            const auto old_token = coordinator.cancellation_token();
            if (complete_old_session) {
                coordinator.enqueue(result(coordinator.snapshot(), BASELINE_GOAL_ID,
                                           RecommendationTaskOutcome::Ready));
                for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
                    coordinator.enqueue(result(coordinator.snapshot(), recommendation_goal_id(goal),
                                               RecommendationTaskOutcome::Ready));
                REQUIRE(coordinator.process_all() == 4);
                REQUIRE(coordinator.snapshot().state == RecommendationSessionState::Ready);
                REQUIRE_FALSE(old_token->cancellation_requested());
            }

            coordinator.enqueue(start_command(7, 2, "revision-b"));
            REQUIRE(coordinator.process_all() == 1);
            CHECK(old_token->reason() == RecommendationCancellationReason::Superseded);
            CHECK(cleanup_reasons == std::vector<RecommendationCancellationReason>{
                                         RecommendationCancellationReason::Superseded});
            CHECK(coordinator.snapshot().state == RecommendationSessionState::Recommending);
            CHECK(coordinator.snapshot().attempt_id == 2);
            CHECK_FALSE(coordinator.cancellation_token()->cancellation_requested());
            REQUIRE(journal.record);
            CHECK(journal.record->attempt_id == 2);
        }
    }
}

TEST_CASE("generic external cancellation cannot forge a superseded transition",
          "[AI][SmartSlicing][Session]")
{
    RecommendationSessionCoordinator coordinator;
    start(coordinator);
    const auto token = coordinator.cancellation_token();
    coordinator.enqueue(CancelRecommendationSessionCommand{
        7, 1, RecommendationCancellationReason::Superseded});
    REQUIRE(coordinator.process_all() == 1);
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Recommending);
    CHECK(coordinator.snapshot().cancellation_reason == RecommendationCancellationReason::None);
    CHECK_FALSE(token->cancellation_requested());
}

TEST_CASE("runtime journal failures and damaged records do not mutate recommendation state",
          "[AI][SmartSlicing]")
{
    SECTION("invalid diagnostic values are discarded before persistence") {
        MemoryRecommendationSessionJournal journal;
        RecommendationSessionCoordinator coordinator;
        coordinator.set_journal(journal);
        start(coordinator);
        const uint64_t publication = coordinator.snapshot().publication_revision;
        coordinator.enqueue(result(coordinator.snapshot(), "balanced", RecommendationTaskOutcome::Failed,
                                   {"candidate_failed", "C:/private/model.3mf credential=secret"}));
        REQUIRE(coordinator.process_all() == 1);
        REQUIRE(journal.record);
        CHECK(journal.record->goals[0].diagnostic_codes.empty());
        CHECK(coordinator.snapshot().recommendation.goal_result(RecommendationGoal::Balanced).status ==
              GoalResultStatus::Analyzing);
        CHECK(coordinator.snapshot().publication_revision == publication);
        CHECK(coordinator.discarded_result_count() == 1);
    }

    SECTION("save failure") {
        MemoryRecommendationSessionJournal journal;
        journal.throw_on_save = true;
        RecommendationSessionCoordinator coordinator;
        coordinator.set_journal(journal);
        start(coordinator);
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Recommending);
        CHECK(coordinator.journal_error_count() == 1);
    }

    SECTION("load failure") {
        MemoryRecommendationSessionJournal journal;
        journal.throw_on_load = true;
        RecommendationSessionCoordinator coordinator;
        coordinator.set_journal(journal);
        CHECK_FALSE(coordinator.recover_interrupted_session());
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Idle);
        CHECK(coordinator.journal_error_count() == 1);
    }

    SECTION("damaged typed record") {
        MemoryRecommendationSessionJournal journal;
        journal.record = interrupted_record();
        journal.record->goals[1].goal_id = "unknown-goal";
        RecommendationSessionCoordinator coordinator;
        coordinator.set_journal(journal);
        CHECK_FALSE(coordinator.recover_interrupted_session());
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Idle);
        CHECK(coordinator.journal_error_count() == 1);
        CHECK(journal.clear_count == 1);
    }
}

TEST_CASE("runtime journal recovery returns interrupted metadata without applicable candidates",
          "[AI][SmartSlicing]")
{
    MemoryRecommendationSessionJournal journal;
    journal.record = interrupted_record();
    RecommendationSessionCoordinator coordinator;
    coordinator.set_journal(journal);

    const std::optional<InterruptedRecommendationSessionMetadata> recovered =
        coordinator.recover_interrupted_session();
    REQUIRE(recovered);
    CHECK(recovered->workflow_id == 99);
    CHECK(recovered->attempt_id == 3);
    CHECK(recovered->baseline_status == GoalResultStatus::Ready);
    CHECK(recovered->goals[0].goal_id == "balanced");
    CHECK(recovered->goals[0].status == GoalResultStatus::Analyzing);
    CHECK(recovered->strategy_version == "strategy-v1");
    CHECK(recovered->interruption_diagnostic_code == "previous_session_interrupted");
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Idle);
    CHECK(journal.clear_count == 1);
    CHECK_FALSE(journal.record);
}

TEST_CASE("file runtime journal serializes only the approved field whitelist and rejects corruption",
          "[AI][SmartSlicing]")
{
    const std::filesystem::path path = unique_journal_path();
    FileRecommendationSessionJournal journal(path);
    RecommendationSessionJournalRecord record = interrupted_record();
    record.baseline.diagnostic_codes.push_back("credentials=secret model=C:/private/model.3mf");
    journal.save(record);

    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const nlohmann::json encoded = nlohmann::json::parse(input);
    const std::set<std::string> expected_keys{
        "schema", "workflow_id", "attempt_id", "workspace_revision", "baseline", "goals",
        "contract_version", "strategy_version", "started_at_epoch_seconds", "updated_at_epoch_seconds"};
    std::set<std::string> actual_keys;
    for (auto item = encoded.begin(); item != encoded.end(); ++item)
        actual_keys.insert(item.key());
    CHECK(actual_keys == expected_keys);
    const std::string bytes = encoded.dump();
    for (const std::string& forbidden : {"geometry", "configuration", "parameters", "protected_faces",
                                         "gcode", "credentials"})
        CHECK(bytes.find(forbidden) == std::string::npos);

    const std::optional<RecommendationSessionJournalRecord> loaded = journal.load();
    REQUIRE(loaded);
    CHECK(loaded->workflow_id == record.workflow_id);
    CHECK(loaded->goals[2].candidate_id == record.goals[2].candidate_id);
    CHECK(loaded->baseline.diagnostic_codes ==
          std::vector<std::string>{"baseline_ready", "invalid_diagnostic_code"});

    input.close();
    {
        std::ofstream corrupted(path, std::ios::binary | std::ios::trunc);
        REQUIRE(corrupted);
        corrupted << R"({"schema":42})";
    }
    CHECK_FALSE(journal.load());
    journal.clear();
    CHECK_FALSE(std::filesystem::exists(path));
}
