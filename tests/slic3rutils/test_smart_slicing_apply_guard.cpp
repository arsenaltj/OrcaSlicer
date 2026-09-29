#include <catch2/catch_all.hpp>
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;
using Slic3r::GUI::OrcaApplyMutationResult;
using Slic3r::GUI::OrcaOfficialSliceGateway;
using Slic3r::GUI::OrcaVersionedApplyResult;

namespace {
struct ApplyTestWorkspace final : IOrcaWorkspace {
    WorkspaceRevision current_revision() const override { return {1, 2, 3, "before"}; }
    WorkspaceContext capture_context() const override {
        WorkspaceContext context;
        context.revision = current_revision();
        context.plate_index = 0;
        context.printer_preset_id = "printer";
        context.process_preset_id = "process";
        context.materials.push_back({"material", "#FFFFFF"});
        context.objects.push_back({42, "cube", 1, 12, 0, false});
        context.machine_capability.registry_version = "test-machine-registry";
        context.machine_capability.support_status = MachineSupportStatus::Enabled;
        context.material_compatibility.registry_version = "test-material-registry";
        context.material_compatibility.combination_status = MaterialCombinationStatus::Compatible;
        context.material_compatibility.common_family = MaterialFamily::PLA;
        context.native_validation_available = true;
        return context;
    }
};
struct ApplyTestTrial final : ITrialSliceExecutor {
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override {
        return {candidate.id, candidate.base_revision, TrialSliceStatus::Succeeded, SlicingMetrics{}, {}};
    }
    void cancel_trial_slice() override {}
};
} // namespace

TEST_CASE("completed applications recover from undo errors and reject obsolete undo", "[SmartSlicing][Apply]")
{
    ApplyTestWorkspace workspace;
    ApplyTestTrial trial;
    bool throw_on_undo = true;
    const bool can_undo = GENERATE(false, true);
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return workspace.current_revision(); }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return true; }, [] { return true; }, [&] {
            if (throw_on_undo) throw std::runtime_error("temporary undo failure");
            return can_undo;
        });
    SmartSlicingCoordinator coordinator(workspace, trial, gateway);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    gateway.notify_slice_completed(true);
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().detail == "apply_undo_failed");
    CHECK(coordinator.snapshot().can_undo_apply);
    throw_on_undo = false;
    CHECK(coordinator.undo_applied_candidate() == can_undo);
    CHECK(coordinator.snapshot().state == (can_undo ? WorkflowState::ReadyToApply : WorkflowState::Stale));
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("undo targets the applied revision and leaves subsequent edits untouched", "[SmartSlicing][Apply]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&](const SliceCandidate&) {
            current.fingerprint = "applied";
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        }, [] { return true; }, [] { return true; }, [&] { ++undo_calls; return true; });
    SliceCandidate candidate;
    candidate.base_revision = current;
    REQUIRE(gateway.commit(candidate, candidate.base_revision).phase == OfficialSlicePhase::Slicing);
    gateway.notify_slice_completed(true);
    REQUIRE(gateway.poll().phase == OfficialSlicePhase::Completed);
    const bool subsequently_edited = GENERATE(false, true);
    if (subsequently_edited) current.fingerprint = "user-edit";
    CHECK(gateway.undo_last_apply() == !subsequently_edited);
    CHECK(undo_calls == (subsequently_edited ? 0 : 1));
    CHECK_FALSE(gateway.undo_last_apply());
}

TEST_CASE("native undo rejection preserves later history even when the content matches", "[SmartSlicing][Apply]")
{
    const WorkspaceRevision revision{1, 2, 3, "same-content"};
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [=] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return false; }, [] { return true; }, [] { return false; });
    SliceCandidate candidate;
    candidate.base_revision = revision;
    REQUIRE(gateway.commit(candidate, revision).phase == OfficialSlicePhase::Failed);
    CHECK_FALSE(gateway.undo_last_apply());
}

namespace {

enum class TransactionFailureStage { None, Transform, Config, Invalidation, Dirty };

AtomicApplyPlan versioned_plan(const WorkspaceRevision& revision, size_t transform_count,
                               size_t config_count, std::string plan_id = "apply-plan:d6t3")
{
    AtomicApplyPlan plan;
    plan.plan_id = std::move(plan_id);
    plan.confirmation_token_id = "risk-confirmation-d6t3";
    plan.binding.workflow_id = 17;
    plan.binding.attempt_id = 4;
    plan.binding.base_revision = revision;
    plan.binding.goal = RecommendationGoal::Balanced;
    plan.binding.goal_task_candidate_id = "balanced-task";
    plan.binding.selected_candidate_id = "balanced-selected";
    plan.binding.strategy_version = "candidate-search/v1";
    plan.binding.selection_policy_version = "candidate-selection/v1";
    plan.binding.parameter_policy_version = PARAMETER_POLICY_VERSION;
    plan.binding.risk_evidence_digest = "risk-evidence";
    plan.binding.risk_confirmation_policy_version = RISK_CONFIRMATION_POLICY_VERSION;
    plan.binding.risk_confirmation_evidence_digest = "confirmation-evidence";
    plan.binding.required_confirmations = {RiskConfirmationKind::ProtectedRegionSeam};
    plan.binding.intent_evidence_revision = 3;
    plan.binding.machine_registry_version = "machine-registry/v1";
    plan.binding.material_registry_version = "material-registry/v1";
    plan.binding.machine_capability_evidence_digest = "machine-evidence";
    plan.binding.material_compatibility_evidence_digest = "material-evidence";
    plan.candidate.id = plan.binding.selected_candidate_id;
    plan.candidate.base_revision = revision;
    plan.candidate.goal = CandidateGoal::Stability;
    plan.candidate.status = CandidateStatus::Ready;
    plan.candidate.parameters.goal = plan.binding.goal;
    plan.candidate.parameters.policy_version = plan.binding.parameter_policy_version;
    for (size_t index = 0; index < transform_count; ++index) {
        ObjectTransform transform;
        transform.object_id = 100 + index / 2;
        transform.instance_id = 200 + index;
        transform.matrix = {1.0, 0.0, 0.0, static_cast<double>(index + 1),
                            0.0, 1.0, 0.0, 0.0,
                            0.0, 0.0, 1.0, 0.0,
                            0.0, 0.0, 0.0, 1.0};
        plan.candidate.placement.transforms.push_back(transform);
    }
    for (size_t index = 0; index < config_count; ++index) {
        ConfigPatchEntry entry;
        entry.scope = ConfigScope::Plate;
        entry.owner = PresetOwner::Process;
        entry.target_id = 9;
        entry.key = "parameter-" + std::to_string(index);
        entry.expected_value = int64_t(index);
        entry.new_value = int64_t(index + 1);
        entry.reason_code = "balanced";
        plan.candidate.parameters.entries.push_back(entry);
    }
    plan.binding.candidate_payload_digest = apply_candidate_payload_digest(plan.candidate);
    return plan;
}

struct RecordingVersionedTransaction
{
    OrcaVersionedApplyResult apply(const AtomicApplyPlan& plan)
    {
        ++calls;
        ++snapshot_count;
        bool mutated = false;
        for (size_t index = 0; index < plan.candidate.placement.transforms.size(); ++index) {
            ++transform_mutations;
            mutated = true;
            if (failure == TransactionFailureStage::Transform)
                return failure_result(mutated, "transform_apply_failed");
        }
        for (size_t index = 0; index < plan.candidate.parameters.entries.size(); ++index) {
            ++config_mutations;
            mutated = true;
            if (failure == TransactionFailureStage::Config)
                return failure_result(mutated, "config_apply_failed");
        }
        ++invalidation_calls;
        if (failure == TransactionFailureStage::Invalidation)
            return failure_result(mutated, "invalidation_failed");
        ++dirty_calls;
        if (failure == TransactionFailureStage::Dirty)
            return failure_result(mutated, "dirty_update_failed");

        OfficialApplyTransactionIdentity transaction;
        transaction.plan_id = plan.plan_id;
        transaction.candidate_id = plan.candidate.id;
        transaction.applied_revision = {2, 3, 4, "applied"};
        transaction.workflow_id = plan.binding.workflow_id;
        transaction.attempt_id = plan.binding.attempt_id;
        transaction.snapshot = {40, 41, "Apply Smart Slicing Candidate"};
        return {true, true, false, false, {}, std::move(transaction)};
    }

    OrcaVersionedApplyResult failure_result(bool mutated, std::string diagnostic)
    {
        ++rollback_calls;
        if (rollback_succeeds)
            return {false, false, true, true, "versioned_apply_rolled_back", std::nullopt};
        return {false, mutated, true, false, std::move(diagnostic), std::nullopt};
    }

    TransactionFailureStage failure{TransactionFailureStage::None};
    bool rollback_succeeds{true};
    size_t calls{0};
    size_t snapshot_count{0};
    size_t transform_mutations{0};
    size_t config_mutations{0};
    size_t invalidation_calls{0};
    size_t dirty_calls{0};
    size_t rollback_calls{0};
};

OrcaOfficialSliceGateway versioned_gateway(WorkspaceRevision& current,
                                           RecordingVersionedTransaction& transaction,
                                           bool& owner_thread,
                                           size_t& legacy_apply_calls,
                                           size_t& official_slice_calls,
                                           size_t& preview_calls)
{
    return OrcaOfficialSliceGateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&legacy_apply_calls](const SliceCandidate&) {
            ++legacy_apply_calls;
            return OrcaApplyMutationResult{true, true, {}};
        },
        [&official_slice_calls] { ++official_slice_calls; return true; },
        [&preview_calls] { ++preview_calls; return true; }, [] { return true; },
        [&transaction, &current](const AtomicApplyPlan& plan) {
            OrcaVersionedApplyResult result = transaction.apply(plan);
            if (result.success && result.transaction)
                current = result.transaction->applied_revision;
            return result;
        },
        [&owner_thread] { return owner_thread; });
}

} // namespace

TEST_CASE("versioned apply commits transform config and combined plans in one transaction",
          "[AI][SmartSlicing][D6T3]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    RecordingVersionedTransaction transaction;
    bool owner_thread = true;
    size_t legacy_apply_calls = 0;
    size_t official_slice_calls = 0;
    size_t preview_calls = 0;
    OrcaOfficialSliceGateway gateway = versioned_gateway(
        current, transaction, owner_thread, legacy_apply_calls, official_slice_calls, preview_calls);
    const auto counts = GENERATE(table<size_t, size_t>({{3, 0}, {0, 2}, {4, 3}}));
    const size_t transforms = std::get<0>(counts);
    const size_t configs = std::get<1>(counts);
    AtomicApplyPlan plan = versioned_plan(
        current, transforms, configs, "apply-plan:" + std::to_string(transforms) + ":" + std::to_string(configs));

    const OfficialSliceResult result = gateway.commit_plan(plan);

    REQUIRE(result.phase == OfficialSlicePhase::Applied);
    REQUIRE(result.apply_transaction);
    CHECK(result.workspace_mutated);
    CHECK(result.can_undo);
    CHECK(result.apply_transaction->plan_id == plan.plan_id);
    CHECK(result.apply_transaction->candidate_id == plan.candidate.id);
    CHECK(result.apply_transaction->applied_revision.fingerprint == "applied");
    CHECK(result.apply_transaction->snapshot.action_snapshot_time == 40);
    CHECK(result.apply_transaction->snapshot.applied_snapshot_time == 41);
    CHECK(transaction.snapshot_count == 1);
    CHECK(transaction.transform_mutations == transforms);
    CHECK(transaction.config_mutations == configs);
    CHECK(transaction.invalidation_calls == 1);
    CHECK(transaction.dirty_calls == 1);
    CHECK(legacy_apply_calls == 0);
    CHECK(official_slice_calls == 0);
    CHECK(preview_calls == 0);
}

TEST_CASE("versioned apply rejects invalid stale replayed and non-owner plans before snapshot",
          "[AI][SmartSlicing][D6T3]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    RecordingVersionedTransaction transaction;
    bool owner_thread = true;
    size_t legacy_apply_calls = 0;
    size_t official_slice_calls = 0;
    size_t preview_calls = 0;
    OrcaOfficialSliceGateway gateway = versioned_gateway(
        current, transaction, owner_thread, legacy_apply_calls, official_slice_calls, preview_calls);
    AtomicApplyPlan valid = versioned_plan(current, 1, 1);

    SECTION("schema") { valid.schema = "wrong"; }
    SECTION("version") { valid.version = "v0"; }
    SECTION("binding identity") { valid.binding.selected_candidate_id = "other"; }
    SECTION("candidate digest") { valid.binding.candidate_payload_digest = "tampered"; }
    SECTION("candidate goal mismatch with recomputed digest") {
        valid.candidate.goal = CandidateGoal::Speed;
        valid.binding.candidate_payload_digest = apply_candidate_payload_digest(valid.candidate);
    }
    SECTION("all goal fields invalid with recomputed digest") {
        valid.binding.goal = static_cast<RecommendationGoal>(999);
        valid.candidate.parameters.goal = static_cast<RecommendationGoal>(999);
        valid.candidate.goal = static_cast<CandidateGoal>(999);
        valid.binding.candidate_payload_digest = apply_candidate_payload_digest(valid.candidate);
    }
    SECTION("missing intent evidence revision") { valid.binding.intent_evidence_revision = 0; }
    SECTION("stale revision") { current.fingerprint = "later"; }
    SECTION("non-owner thread") { owner_thread = false; }
    SECTION("replay") {
        REQUIRE(gateway.commit_plan(valid).phase == OfficialSlicePhase::Applied);
        const size_t calls_after_first = transaction.calls;
        const OfficialSliceResult replayed = gateway.commit_plan(valid);
        CHECK(replayed.phase == OfficialSlicePhase::Rejected);
        CHECK(replayed.diagnostic_code == "apply_plan_already_consumed");
        CHECK(transaction.calls == calls_after_first);
        AtomicApplyPlan renamed = valid;
        renamed.plan_id = "apply-plan:renamed-replay";
        const OfficialSliceResult renamed_replay = gateway.commit_plan(renamed);
        CHECK(renamed_replay.phase == OfficialSlicePhase::Rejected);
        CHECK(renamed_replay.diagnostic_code == "apply_plan_already_consumed");
        CHECK(transaction.calls == calls_after_first);
        return;
    }

    const OfficialSliceResult rejected = gateway.commit_plan(valid);
    CHECK(rejected.phase == OfficialSlicePhase::Rejected);
    CHECK_FALSE(rejected.workspace_mutated);
    CHECK(transaction.calls == 0);
    CHECK(transaction.snapshot_count == 0);
    CHECK(legacy_apply_calls == 0);
    CHECK(official_slice_calls == 0);
    CHECK(preview_calls == 0);
}

TEST_CASE("versioned apply rejects a nested official transaction before another snapshot",
          "[AI][SmartSlicing][D6T3]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    bool owner_thread = true;
    OfficialSliceResult nested;
    size_t snapshots = 0;
    OrcaOfficialSliceGateway* gateway_ptr = nullptr;
    OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return OrcaApplyMutationResult{}; }, [] { return true; },
        [] { return true; }, [] { return true; },
        [&](const AtomicApplyPlan& plan) {
            ++snapshots;
            AtomicApplyPlan second = versioned_plan(current, 2, 0, "apply-plan:nested");
            nested = gateway_ptr->commit_plan(second);
            OfficialApplyTransactionIdentity identity{
                plan.plan_id, plan.candidate.id, {2, 3, 4, "applied"},
                {70, 71, "Apply Smart Slicing Candidate"}, plan.binding.workflow_id,
                plan.binding.attempt_id};
            current = identity.applied_revision;
            return OrcaVersionedApplyResult{true, true, false, false, {}, std::move(identity)};
        }, [&owner_thread] { return owner_thread; });
    gateway_ptr = &gateway;

    REQUIRE(gateway.commit_plan(versioned_plan(current, 1, 0)).phase == OfficialSlicePhase::Applied);
    CHECK(nested.phase == OfficialSlicePhase::Rejected);
    CHECK(nested.diagnostic_code == "official_transaction_active");
    CHECK(snapshots == 1);
}

TEST_CASE("versioned transaction rolls back every mutation stage and reports rollback failure",
          "[AI][SmartSlicing][D6T3]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    RecordingVersionedTransaction transaction;
    transaction.failure = GENERATE(TransactionFailureStage::Transform,
                                   TransactionFailureStage::Config,
                                   TransactionFailureStage::Invalidation,
                                   TransactionFailureStage::Dirty);
    const bool rollback_succeeds = GENERATE(false, true);
    transaction.rollback_succeeds = rollback_succeeds;
    bool owner_thread = true;
    size_t legacy_apply_calls = 0;
    size_t official_slice_calls = 0;
    size_t preview_calls = 0;
    OrcaOfficialSliceGateway gateway = versioned_gateway(
        current, transaction, owner_thread, legacy_apply_calls, official_slice_calls, preview_calls);

    const OfficialSliceResult result = gateway.commit_plan(versioned_plan(current, 2, 2));

    CHECK(result.phase == OfficialSlicePhase::Failed);
    CHECK(result.workspace_mutated == !rollback_succeeds);
    CHECK_FALSE(result.can_undo);
    CHECK(transaction.snapshot_count == 1);
    CHECK(transaction.rollback_calls == 1);
    if (rollback_succeeds)
        CHECK(result.diagnostic_code == "versioned_apply_rolled_back");
    else
        CHECK_FALSE(result.diagnostic_code.empty());
    CHECK(legacy_apply_calls == 0);
    CHECK(official_slice_calls == 0);
    CHECK(preview_calls == 0);
}

TEST_CASE("versioned slice failure retries without reapplying and enables print only after preview",
          "[AI][SmartSlicing][D6T4]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    RecordingVersionedTransaction transaction;
    bool owner_thread = true;
    size_t official_slice_calls = 0;
    size_t preview_calls = 0;
    bool preview_succeeds = true;
    OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return OrcaApplyMutationResult{true, true, {}}; },
        [&official_slice_calls] { ++official_slice_calls; return true; },
        [&preview_calls, &preview_succeeds] { ++preview_calls; return preview_succeeds; }, [] { return true; },
        [&transaction, &current](const AtomicApplyPlan& plan) {
            OrcaVersionedApplyResult result = transaction.apply(plan);
            if (result.success && result.transaction)
                current = result.transaction->applied_revision;
            return result;
        }, [&owner_thread] { return owner_thread; });
    AtomicApplyPlan plan = versioned_plan(current, 1, 0, "apply-plan:d6t4");

    const OfficialSliceResult applied = gateway.commit_plan(plan);
    REQUIRE(applied.phase == OfficialSlicePhase::Applied);
    REQUIRE(applied.apply_transaction);
    const OfficialApplyTransactionIdentity identity = *applied.apply_transaction;
    CHECK(gateway.start_committed_plan_slice(identity).phase == OfficialSlicePhase::Slicing);
    CHECK(official_slice_calls == 1);
    CHECK(gateway.complete_official_slice(identity, false, "official_slice_failed").phase ==
          OfficialSlicePhase::Failed);
    CHECK(gateway.retry_official_slice(identity).phase == OfficialSlicePhase::Slicing);
    CHECK(official_slice_calls == 2);
    CHECK(gateway.complete_official_slice(identity, true).phase == OfficialSlicePhase::Completed);
    const OfficialSliceResult preview = gateway.poll_committed_plan(identity);
    CHECK(preview.phase == OfficialSlicePhase::Completed);
    CHECK(preview.can_print);
    CHECK(preview.can_retry_slice == false);
    CHECK(preview_calls == 1);
    CHECK(transaction.calls == 1);
}

TEST_CASE("versioned preview failure retains applied transaction and only retries slicing",
          "[AI][SmartSlicing][D6T4]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    RecordingVersionedTransaction transaction;
    bool owner_thread = true;
    size_t official_slice_calls = 0;
    size_t preview_calls = 0;
    OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return OrcaApplyMutationResult{true, true, {}}; },
        [&official_slice_calls] { ++official_slice_calls; return true; },
        [&preview_calls] { ++preview_calls; return false; }, [] { return true; },
        [&transaction, &current](const AtomicApplyPlan& plan) {
            OrcaVersionedApplyResult result = transaction.apply(plan);
            if (result.success && result.transaction)
                current = result.transaction->applied_revision;
            return result;
        }, [&owner_thread] { return owner_thread; });
    AtomicApplyPlan plan = versioned_plan(current, 1, 0, "apply-plan:d6t4-preview");
    const OfficialSliceResult applied = gateway.commit_plan(plan);
    REQUIRE(applied.apply_transaction);
    const OfficialApplyTransactionIdentity identity = *applied.apply_transaction;
    REQUIRE(gateway.start_committed_plan_slice(identity).phase == OfficialSlicePhase::Slicing);
    REQUIRE(gateway.complete_official_slice(identity, true).phase == OfficialSlicePhase::Completed);
    const OfficialSliceResult preview = gateway.poll_committed_plan(identity);
    CHECK(preview.phase == OfficialSlicePhase::Failed);
    CHECK(preview.diagnostic_code == "preview_navigation_failed");
    CHECK(preview.can_print == false);
    CHECK(preview.can_retry_slice);
    CHECK(gateway.retry_official_slice(identity).phase == OfficialSlicePhase::Slicing);
    CHECK(official_slice_calls == 2);
    CHECK(transaction.calls == 1);
    CHECK(preview_calls == 1);
}

TEST_CASE("versioned AI undo is identity guarded and succeeds once",
          "[AI][SmartSlicing][D6T4]")
{
    WorkspaceRevision base{1, 2, 3, "before"};
    WorkspaceRevision current = base;
    RecordingVersionedTransaction transaction;
    bool owner_thread = true;
    size_t undo_calls = 0;
    bool undo_available = true;
    std::optional<OfficialApplyTransactionIdentity> seen;
    OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return OrcaApplyMutationResult{true, true, {}}; },
        [] { return true; }, [] { return true; }, [] { return false; },
        [&transaction, &current](const AtomicApplyPlan& plan) {
            OrcaVersionedApplyResult result = transaction.apply(plan);
            if (result.success && result.transaction)
                current = result.transaction->applied_revision;
            return result;
        }, [&owner_thread] { return owner_thread; },
        [&undo_available](const OfficialApplyTransactionIdentity&) { return undo_available; },
        [&current, &base, &undo_calls, &seen](const OfficialApplyTransactionIdentity& identity,
                                             std::string&) {
            ++undo_calls;
            seen = identity;
            current = base;
            return true;
        });
    const OfficialSliceResult applied = gateway.commit_plan(versioned_plan(base, 1, 0, "apply-plan:d6t4-undo"));
    REQUIRE(applied.apply_transaction);
    const OfficialApplyTransactionIdentity identity = *applied.apply_transaction;
    CHECK(gateway.undo_committed_plan(identity).diagnostic_code == "apply_undone");
    CHECK(undo_calls == 1);
    CHECK(seen.has_value());
    CHECK(*seen == identity);
    CHECK(gateway.undo_committed_plan(identity).diagnostic_code == "apply_undo_unavailable");
    CHECK(undo_calls == 1);

    AtomicApplyPlan reapplied_plan = versioned_plan(base, 1, 0, "apply-plan:d6t4-undo-2");
    reapplied_plan.confirmation_token_id = "risk-confirmation-d6t4-undo-2";
    const OfficialSliceResult reapplied = gateway.commit_plan(reapplied_plan);
    REQUIRE(reapplied.apply_transaction);
    const OfficialApplyTransactionIdentity identity2 = *reapplied.apply_transaction;
    current.fingerprint = "user-edit";
    CHECK(gateway.undo_committed_plan(identity2).diagnostic_code == "apply_undo_unavailable");
    CHECK(undo_calls == 1);
}

TEST_CASE("failed smart slicing action is removed from undo history after exact restore",
          "[AI][SmartSlicing][D6T3][UndoRedo]")
{
    using namespace Slic3r::UndoRedo;
    SnapshotData action_data;
    action_data.snapshot_type = SnapshotType::Action;
    std::vector<Snapshot> history;
    history.emplace_back("Previous User Operation", 10, 11, action_data);
    history.emplace_back("Apply Smart Slicing Candidate", 20, 12, action_data);
    history.emplace_back("@@@ Topmost @@@", 21, 13, action_data);
    size_t active_time = 20;

    REQUIRE(detail::abort_top_action_history(
        history, active_time, {20, 21, "Apply Smart Slicing Candidate"}));

    REQUIRE(history.size() == 2);
    CHECK(history.front().name == "Previous User Operation");
    CHECK(snapshot_modifies_project(history.front()));
    CHECK(history.front().timestamp < active_time);
    CHECK(active_time == 20);
    CHECK(history.back().timestamp == active_time);
    CHECK(history.back().is_topmost());
    CHECK_FALSE(history.back().is_topmost_captured());
    CHECK(std::none_of(history.begin(), history.end(), [](const Snapshot& snapshot) {
        return snapshot.name == "Apply Smart Slicing Candidate";
    }));
    CHECK(std::none_of(history.begin(), history.end(), [active_time](const Snapshot& snapshot) {
        return snapshot.timestamp > active_time;
    }));
}

TEST_CASE("undo abort refuses a changed transaction identity without editing history",
          "[AI][SmartSlicing][D6T3][UndoRedo]")
{
    using namespace Slic3r::UndoRedo;
    SnapshotData action_data;
    action_data.snapshot_type = SnapshotType::Action;
    std::vector<Snapshot> history;
    history.emplace_back("Previous User Operation", 10, 11, action_data);
    history.emplace_back("Apply Smart Slicing Candidate", 20, 12, action_data);
    history.emplace_back("@@@ Topmost @@@", 22, 13, action_data);
    const std::vector<Snapshot> before = history;
    size_t active_time = 20;

    CHECK_FALSE(detail::abort_top_action_history(
        history, active_time, {20, 21, "Apply Smart Slicing Candidate"}));
    REQUIRE(history.size() == before.size());
    for (size_t index = 0; index < history.size(); ++index) {
        CHECK(history[index].name == before[index].name);
        CHECK(history[index].timestamp == before[index].timestamp);
        CHECK(history[index].model_id == before[index].model_id);
    }
    CHECK(active_time == 20);
}
