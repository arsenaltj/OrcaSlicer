#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/ApplyWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/TrialSlicingWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaModelPreparation.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <functional>
#include <stdexcept>

using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r;

namespace {

WorkspaceContext printable_context(std::string fingerprint = "revision-a")
{
    WorkspaceContext context;
    context.revision          = {1, 2, 3, std::move(fingerprint)};
    context.plate_index       = 0;
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

SliceCandidate proposal(std::string id, const WorkspaceRevision& revision)
{
    SliceCandidate candidate;
    candidate.id            = std::move(id);
    candidate.base_revision = revision;
    candidate.status        = CandidateStatus::Ready;
    candidate.metrics       = SlicingMetrics{};
    candidate.metrics->estimated_time_seconds = 1.0;
    return candidate;
}

Slic3r::GUI::OrcaTrialSliceInput tiny_trial_input()
{
    Slic3r::GUI::OrcaTrialSliceInput input;
    ModelObject* object = input.model.add_object();
    object->name = "budget cube";
    object->add_volume(make_cube(5.0, 5.0, 5.0));
    object->add_instance()->set_offset(Vec3d(50.0, 50.0, 0.0));
    object->ensure_on_bed();
    input.config = DynamicPrintConfig::full_print_config();
    input.config.set("layer_height", 0.25);
    input.config.set("layer_change_gcode", std::string("G92 E0\n"));
    input.plate_index = 0;
    input.plate_id = 7;
    input.plate_name = "Budget Trial";
    return input;
}

class WorkflowWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceContext context = printable_context();
    bool revision_unavailable{false};
    std::function<WorkspaceContext()> capture_override;

    WorkspaceRevision current_revision() const override {
        if (revision_unavailable) throw std::runtime_error("revision unavailable");
        return context.revision;
    }
    WorkspaceContext capture_context() const override { return capture_override ? capture_override() : context; }
};

class FakeTrialSliceExecutor final : public ITrialSliceExecutor
{
public:
    std::vector<CandidateId> calls;
    size_t cancel_count{0};
    std::function<TrialSliceResult(const SliceCandidate&, size_t)> result_for;

    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        calls.push_back(candidate.id);
        if (result_for)
            return result_for(candidate, calls.size() - 1);

        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
        result.metrics->filament_volume_mm3 = 500.0;
        result.metrics->support_volume_mm3 = 10.0;
        return result;
    }

    void cancel_trial_slice() override { ++cancel_count; }
};

class FakeOfficialSliceGateway final : public IOfficialSliceGateway
{
public:
    OfficialSliceResult prepared{OfficialSlicePhase::Prepared, {}, false, false};
    OfficialSliceResult committed{OfficialSlicePhase::Slicing, {}, true, true};
    OfficialSliceResult polled{OfficialSlicePhase::Slicing, {}, true, true};
    size_t prepare_calls{0};
    size_t commit_calls{0};
    size_t undo_calls{0};
    bool throw_on_undo{false};
    std::function<void()> on_commit;

    OfficialSliceResult prepare(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++prepare_calls;
        return prepared;
    }
    OfficialSliceResult commit(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++commit_calls;
        if (on_commit) on_commit();
        return committed;
    }
    OfficialSliceResult poll() override { return polled; }
    bool undo_last_apply() override
    {
        if (throw_on_undo)
            throw std::runtime_error("native undo temporarily unavailable");
        ++undo_calls;
        return true;
    }
};

} // namespace

TEST_CASE("candidate planning binds drafts to one workspace revision and caps proposals", "[AI][SmartSlicing][Workflow]")
{
    const WorkspaceContext context = printable_context();
    std::vector<SliceCandidate> proposals;
    proposals.push_back(proposal("z", context.revision));
    proposals.push_back(proposal("a", context.revision));
    proposals.push_back(proposal("extra", context.revision));
    proposals.push_back(proposal("zz-overflow", context.revision));
    proposals.push_back(proposal("stale", WorkspaceRevision{9, 9, 9, "old"}));

    const std::vector<SliceCandidate> planned = CandidatePlanningWorkflow().plan(context, proposals);

    REQUIRE(planned.size() == 4);
    CHECK(planned[0].id == "baseline");
    CHECK(planned[1].id == "a");
    CHECK(planned[2].id == "extra");
    CHECK(planned[3].id == "z");
    for (const SliceCandidate& candidate : planned) {
        CHECK(candidate.base_revision == context.revision);
        CHECK(candidate.status == CandidateStatus::Draft);
        CHECK_FALSE(candidate.metrics);
    }
}

TEST_CASE("coordinator trial slices baseline first and retains it after an alternative fails", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status         = index == 1 ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        result.diagnostic_code = index == 1 ? "trial_failed" : "";
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = index == 0 ? 100.0 : 75.0;
            result.metrics->filament_volume_mm3 = 500.0;
            result.metrics->support_volume_mm3 = 10.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    const bool ready = coordinator.plan_and_slice_candidates(
        {proposal("failed-alternative", workspace.context.revision), proposal("good-alternative", workspace.context.revision)});

    CHECK(ready);
    CHECK(executor.calls == std::vector<CandidateId>{"baseline", "failed-alternative", "good-alternative"});
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    REQUIRE(coordinator.snapshot().candidates.size() == 3);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[2].status == CandidateStatus::Ready);
    REQUIRE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "good-alternative");
}

TEST_CASE("mismatched trial results are never attached to a candidate", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = index == 0 ? candidate.id : "late-from-old-workflow";
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 50.0;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK_FALSE(coordinator.snapshot().candidates[1].metrics);
    CHECK(coordinator.snapshot().comparison->ordered_candidate_ids == std::vector<CandidateId>{"baseline"});
}

TEST_CASE("cancel during a trial state propagates before executor work starts", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    coordinator.set_observer([&coordinator](const WorkflowSnapshot& snapshot) {
        if (snapshot.state == WorkflowState::TrialSlicingBaseline)
            coordinator.cancel();
    });

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(executor.cancel_count == 1);
    CHECK(executor.calls.empty());
    CHECK(coordinator.snapshot().candidates.empty());
    CHECK_FALSE(coordinator.snapshot().comparison);
}

TEST_CASE("cancel during candidate planning cannot be overwritten by a later trial state", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    coordinator.set_observer([&coordinator](const WorkflowSnapshot& snapshot) {
        if (snapshot.state == WorkflowState::PlanningCandidates)
            coordinator.cancel();
    });

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(executor.calls.empty());
    CHECK(executor.cancel_count == 0);
}

TEST_CASE("late cancellation results cannot cancel the current workflow", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = index == 0 ? candidate.id : "old-candidate";
        result.base_revision = candidate.base_revision;
        result.status        = index == 0 ? TrialSliceStatus::Succeeded : TrialSliceStatus::Canceled;
        if (index == 0) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
}

TEST_CASE("ready candidate workflow projects into optimization and apply stages", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());

    const Slic3r::GUI::SmartSlicingViewModel view =
        Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());

    CHECK(view.summary_key == "candidates_ready");
    CHECK(view.stages[2].status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.stages[3].status == Slic3r::GUI::SmartSlicingStageStatus::Waiting);
    REQUIRE(view.candidates.size() == 1);
    CHECK(view.candidates.front().recommended);
    CHECK(view.candidates.front().selected);
    CHECK(view.can_apply);
}

TEST_CASE("candidate cards expose baseline deltas selection and retry without workspace mutation", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    bool fail_alternative = true;
    executor.result_for = [&fail_alternative](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" && fail_alternative ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
            result.metrics->filament_volume_mm3     = candidate.id == "baseline" ? 500.0 : 450.0;
            result.metrics->support_volume_mm3      = candidate.id == "baseline" ? 20.0 : 10.0;
            result.metrics->tool_changes            = candidate.id == "baseline" ? 4 : 2;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    Slic3r::GUI::SmartSlicingViewModel failed_view =
        Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(failed_view.candidates.size() == 2);
    CHECK(failed_view.candidates[1].failed);
    CHECK(failed_view.candidates[1].can_retry);
    CHECK(failed_view.candidates[0].can_select);
    CHECK_FALSE(failed_view.candidates[1].can_select);
    CHECK_FALSE(coordinator.select_candidate("alternative"));
    CHECK(coordinator.select_candidate("baseline"));

    fail_alternative = false;
    REQUIRE(coordinator.retry_candidate("alternative"));
    REQUIRE(coordinator.select_candidate("alternative"));
    const Slic3r::GUI::SmartSlicingViewModel ready_view =
        Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(ready_view.candidates.size() == 2);
    CHECK(ready_view.candidates[1].selected);
    CHECK(ready_view.candidates[1].can_select);
    CHECK(ready_view.candidates[1].time_delta_seconds == -20.0);
    CHECK(ready_view.candidates[1].filament_delta_mm3 == -50.0);
    CHECK(ready_view.candidates[1].support_delta_mm3 == -10.0);
    CHECK(ready_view.candidates[1].tool_change_delta == -2);
    CHECK(workspace.context.revision.fingerprint == "revision-a");
}

TEST_CASE("workspace edits during trial slicing make all results stale", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [&workspace](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 50.0;
        workspace.context.revision.fingerprint = "revision-b";
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    CHECK(coordinator.snapshot().comparison == std::nullopt);
}

TEST_CASE("apply workflow rejects stale candidates before entering the transaction gateway", "[AI][SmartSlicing][Apply]")
{
    FakeOfficialSliceGateway gateway;
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    const OfficialSliceResult result = ApplyWorkflow().start(
        candidate, candidate.base_revision, WorkspaceRevision{1, 2, 3, "revision-b"}, gateway);

    CHECK(result.phase == OfficialSlicePhase::Rejected);
    CHECK(result.diagnostic_code == "stale_revision");
    CHECK(gateway.prepare_calls == 0);
    CHECK(gateway.commit_calls == 0);
}

TEST_CASE("coordinator applies once then waits for official slice completion", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());

    REQUIRE(coordinator.apply_selected_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::OfficialSlicing);
    CHECK(official.prepare_calls == 1);
    CHECK(official.commit_calls == 1);

    official.polled = {OfficialSlicePhase::Completed, {}, true, true};
    CHECK(coordinator.poll_official_slice());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().can_start());
    official.throw_on_undo = true;
    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().detail == "apply_undo_failed");
    official.throw_on_undo = false;
    CHECK(coordinator.undo_applied_candidate());
    CHECK(official.undo_calls == 1);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("compatibility rejection and apply failure never claim an official slice", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.prepared = {OfficialSlicePhase::Rejected, "compatibility_revalidation_failed", false, false};
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());

    CHECK_FALSE(coordinator.apply_selected_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::ApplyFailed);
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK(official.commit_calls == 0);
}

TEST_CASE("official slice failure exposes exactly one native undo recovery", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());

    official.polled = {OfficialSlicePhase::Failed, "official_slice_failed", true, true};
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().state == WorkflowState::ApplyFailed);
    REQUIRE(coordinator.snapshot().can_undo_apply);
    CHECK(coordinator.undo_applied_candidate());
    CHECK(official.undo_calls == 1);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("Orca official gateway double checks revision and enters Preview only after success", "[AI][SmartSlicing][Apply]")
{
    WorkspaceRevision current{1, 2, 3, "revision-a"};
    size_t apply_calls = 0;
    size_t slice_calls = 0;
    size_t preview_calls = 0;
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&apply_calls](const SliceCandidate&) {
            ++apply_calls;
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [&slice_calls] { ++slice_calls; return true; },
        [&preview_calls] { ++preview_calls; return true; },
        [&undo_calls] { ++undo_calls; return true; });
    SliceCandidate candidate = proposal("candidate", current);

    CHECK(gateway.prepare(candidate, current).phase == OfficialSlicePhase::Prepared);
    current.fingerprint = "revision-b";
    CHECK(gateway.commit(candidate, candidate.base_revision).phase == OfficialSlicePhase::Rejected);
    CHECK(apply_calls == 0);

    current.fingerprint = "revision-a";
    CHECK(gateway.commit(candidate, current).phase == OfficialSlicePhase::Slicing);
    CHECK(apply_calls == 1);
    CHECK(slice_calls == 1);
    CHECK(preview_calls == 0);
    gateway.notify_slice_completed(true);
    CHECK(gateway.poll().phase == OfficialSlicePhase::Completed);
    CHECK(preview_calls == 1);
    CHECK(gateway.poll().phase == OfficialSlicePhase::Completed);
    CHECK(preview_calls == 1);

    CHECK(gateway.prepare(candidate, current).phase == OfficialSlicePhase::Prepared);
    CHECK(gateway.commit(candidate, current).phase == OfficialSlicePhase::Slicing);
    gateway.notify_slice_completed(true);
    CHECK(gateway.poll().phase == OfficialSlicePhase::Completed);
    CHECK(preview_calls == 2);
}

TEST_CASE("Orca official gateway preserves native undo recovery when slicing cannot start", "[AI][SmartSlicing][Apply]")
{
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [revision] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return false; }, [] { return true; }, [&undo_calls] { ++undo_calls; return true; });
    SliceCandidate candidate = proposal("candidate", revision);

    const OfficialSliceResult failed = gateway.commit(candidate, revision);
    CHECK(failed.phase == OfficialSlicePhase::Failed);
    CHECK(failed.diagnostic_code == "official_slice_not_started");
    REQUIRE(failed.can_undo);
    CHECK(gateway.undo_last_apply());
    CHECK(undo_calls == 1);
    CHECK_FALSE(gateway.undo_last_apply());
}

TEST_CASE("Orca trial slicing owns model config print and gcode copies", "[AI][SmartSlicing][Workflow][OrcaTrial]")
{
    Model formal_model;
    ModelObject* object = formal_model.add_object();
    object->name        = "trial cube";
    object->add_volume(make_cube(5.0, 5.0, 5.0));
    ModelInstance* instance = object->add_instance();
    instance->set_offset(Vec3d(50.0, 50.0, 0.0));
    object->ensure_on_bed();
    DynamicPrintConfig formal_config = DynamicPrintConfig::full_print_config();
    formal_config.set("brim_width", 0.0);
    formal_config.set("layer_change_gcode", std::string("G92 E0\n"));

    const ObjectID object_id = object->id();
    const ObjectID instance_id = instance->id();
    const Transform3d original_transform = instance->get_matrix();
    const std::string original_brim_width = formal_config.opt_serialize("brim_width");
    Slic3r::GUI::OrcaTrialSliceExecutor executor([&formal_model, &formal_config] {
        Slic3r::GUI::OrcaTrialSliceInput input;
        input.model       = formal_model;
        input.config      = formal_config;
        input.plate_index = 0;
        input.plate_id    = 7;
        input.plate_name  = "Trial";
        return input;
    });
    SliceCandidate candidate = proposal("baseline", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    ObjectTransform cloned_transform;
    cloned_transform.object_id   = object_id.id;
    cloned_transform.instance_id = instance_id.id;
    Transform3d candidate_transform = original_transform;
    candidate_transform.translation().x() += 10.0;
    for (Eigen::Index row = 0; row < candidate_transform.rows(); ++row)
        for (Eigen::Index column = 0; column < candidate_transform.cols(); ++column)
            cloned_transform.matrix[static_cast<size_t>(row * candidate_transform.cols() + column)] =
                candidate_transform(row, column);
    candidate.placement.transforms.push_back(cloned_transform);
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 7, "brim_width",
                                            0.0, 5.0, "improve_bed_adhesion"});

    Slic3r::GUI::OrcaTrialSliceInput prepared;
    prepared.model = formal_model; prepared.config = formal_config;
    prepared.plate_id = 7; prepared.plate_name = "Trial";
    executor.prepare_session_input(std::move(prepared), {candidate});
    auto altered = candidate;
    altered.parameters.entries.front().new_value = 0.22;
    CHECK(executor.execute_trial_slice(altered).diagnostic_code == "parameter_trial_input_not_prepared");
    const TrialSliceResult result = executor.execute_trial_slice(candidate);

    INFO("trial diagnostic: " << result.diagnostic_code);
    REQUIRE(result.status == TrialSliceStatus::Succeeded);
    REQUIRE(result.metrics);
    CHECK(result.metrics->estimated_time_seconds.value_or(0.0) > 0.0);
    CHECK(result.metrics->filament_volume_mm3.value_or(0.0) > 0.0);
    REQUIRE(formal_model.objects.size() == 1);
    CHECK(formal_model.objects.front()->id() == object_id);
    REQUIRE(formal_model.objects.front()->instances.size() == 1);
    CHECK(formal_model.objects.front()->instances.front()->id() == instance_id);
    CHECK(formal_model.objects.front()->instances.front()->get_matrix().isApprox(original_transform));
    CHECK(formal_config.opt_serialize("brim_width") == original_brim_width);
}

TEST_CASE("Completed and failed slice results become stale after a later workspace edit", "[AI][SmartSlicing][Apply]")
{
    const auto final_phase = GENERATE(OfficialSlicePhase::Completed, OfficialSlicePhase::Failed);
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.on_commit = [&workspace] { workspace.context.revision.fingerprint = "applied-version"; };
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    official.polled = {final_phase, {}, true, true};
    REQUIRE(coordinator.poll_official_slice());
    CHECK_FALSE(coordinator.refresh_revision()); // Applying its own proposal is not a later edit.
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot()).needs_polling);

    workspace.context.revision.fingerprint = "edited-model-or-printer";
    REQUIRE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK_FALSE(coordinator.snapshot().comparison);
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK_FALSE(view.has_report);
    CHECK(view.candidates.empty());
    CHECK_FALSE(view.can_apply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(official.undo_calls == 0);
    coordinator.start();
    REQUIRE(coordinator.snapshot().context);
    CHECK(coordinator.snapshot().context->revision == workspace.context.revision);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("An unavailable applied revision is not reported as a confirmed workspace edit", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.on_commit = [&workspace] { workspace.revision_unavailable = true; };
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    official.polled = {OfficialSlicePhase::Completed, {}, true, true};
    REQUIRE(coordinator.poll_official_slice());
    workspace.revision_unavailable = false;
    REQUIRE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().detail == "applied_revision_unavailable");
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.summary_key == "applied_revision_unavailable");
    CHECK_FALSE(view.has_report);
    CHECK(view.candidates.empty());
    CHECK(view.can_start);
}

TEST_CASE("Native trial slicing checks device height for a prepared 120 mm model", "[AI][SmartSlicing][OrcaModelPreparation][OrcaTrial]")
{
    const double device_height = GENERATE(100.0, 256.0);
    Model formal_model;
    auto* object = formal_model.add_object();
    object->name = "fixed preparation sample";
    object->add_volume(make_cube(8, 8, 40));
    object->add_instance()->set_offset(Vec3d(70, 70, 0));
    object->ensure_on_bed();
    Slic3r::GUI::apply_model_preparation(*object, Slic3r::GUI::prepare_model(*object, {120, true, 3}));
    const auto prepared_transform = object->instances.front()->get_matrix();
    const auto prepared_id = object->id();
    Slic3r::GUI::OrcaTrialSliceExecutor executor([&formal_model, device_height] {
        Slic3r::GUI::OrcaTrialSliceInput input;
        input.model = formal_model;
        input.config = DynamicPrintConfig::full_print_config();
        input.config.set("layer_height", 0.25);
        input.config.set("printable_height", device_height);
        input.config.set("layer_change_gcode", std::string("G92 E0\n"));
        input.plate_index = 0;
        input.plate_id = 7;
        input.plate_name = "Prepared sample";
        return input;
    });
    auto candidate = proposal("baseline", WorkspaceRevision{1, 2, 3, "prepared-120-base"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    const auto result = executor.execute_trial_slice(candidate);
    INFO("trial diagnostic: " << result.diagnostic_code);
    if (device_height < 120) {
        CHECK(result.status == TrialSliceStatus::Failed);
        CHECK(result.diagnostic_code == "trial_validation_failed");
        CHECK_FALSE(result.diagnostic_message.empty());
        CHECK(result.diagnostic_message.find(object->name) != std::string::npos);
        CHECK_FALSE(result.metrics);
        auto failed = candidate;
        CHECK_FALSE(TrialSlicingWorkflow::accept_result(failed, result));
        WorkflowSnapshot snapshot;
        snapshot.state = WorkflowState::Failed;
        snapshot.detail = "baseline_trial_failed";
        snapshot.candidates.push_back(failed);
        const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
        REQUIRE(view.candidates.size() == 1);
        CHECK(view.candidates.front().diagnostic_message == result.diagnostic_message);
        CHECK_FALSE(view.can_apply);
    } else {
        REQUIRE(result.status == TrialSliceStatus::Succeeded);
        REQUIRE(result.metrics);
        CHECK(result.metrics->estimated_time_seconds.value_or(0) > 0);
        CHECK(result.metrics->filament_volume_mm3.value_or(0) > 0);
    }
    CHECK(object->id() == prepared_id);
    REQUIRE(object->volumes.size() == 2);
    CHECK(object->instances.front()->get_matrix().isApprox(prepared_transform));
    CHECK_THAT(object->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(120, 0.001));
}

TEST_CASE("Orca trial slicing rejects forbidden patches and observes early cancellation", "[AI][SmartSlicing][Workflow][OrcaTrial]")
{
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Printer, 0, "nozzle_diameter",
                                            0.4, 0.6, "unsafe_hardware_change"});
    Slic3r::GUI::OrcaTrialSliceExecutor rejected_executor([] {
        Slic3r::GUI::OrcaTrialSliceInput input;
        input.plate_id = 0;
        input.config = DynamicPrintConfig::full_print_config();
        return input;
    });

    const TrialSliceResult rejected = rejected_executor.execute_trial_slice(candidate);
    CHECK(rejected.status == TrialSliceStatus::Failed);
    CHECK(rejected.diagnostic_code == "parameter_immutable_fact");

    Slic3r::GUI::OrcaTrialSliceExecutor* executor_ptr = nullptr;
    Slic3r::GUI::OrcaTrialSliceExecutor canceled_executor([&executor_ptr] {
        executor_ptr->cancel_trial_slice();
        return Slic3r::GUI::OrcaTrialSliceInput{};
    });
    executor_ptr = &canceled_executor;
    candidate.parameters.entries.clear();

    const TrialSliceResult canceled = canceled_executor.execute_trial_slice(candidate);
    CHECK(canceled.status == TrialSliceStatus::Canceled);
    CHECK(canceled.diagnostic_code == "trial_slice_canceled");
}

TEST_CASE("Orca trial slicing enforces execution memory timeout and disk budgets", "[AI][SmartSlicing][Workflow][OrcaTrial][Runtime]")
{
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();

    Slic3r::GUI::OrcaTrialSliceExecutor memory_limited([] { return tiny_trial_input(); });
    memory_limited.set_resource_limits(std::chrono::minutes(1), 1, 1024 * 1024);
    const TrialSliceResult memory_result = memory_limited.execute_trial_slice(candidate);
    CHECK(memory_result.status == TrialSliceStatus::Failed);
    CHECK(memory_result.diagnostic_code == "workflow_memory_budget_exceeded");

    Slic3r::GUI::OrcaTrialSliceExecutor timed_out([] { return tiny_trial_input(); });
    timed_out.set_resource_limits(std::chrono::seconds(0), 1024 * 1024, 1024 * 1024);
    const TrialSliceResult timeout_result = timed_out.execute_trial_slice(candidate);
    CHECK(timeout_result.status == TrialSliceStatus::Canceled);
    CHECK(timeout_result.diagnostic_code == "workflow_timeout");

    Slic3r::GUI::OrcaTrialSliceExecutor disk_limited([] { return tiny_trial_input(); });
    disk_limited.set_resource_limits(std::chrono::minutes(1), 1024 * 1024, 0);
    const TrialSliceResult disk_result = disk_limited.execute_trial_slice(candidate);
    CHECK(disk_result.status == TrialSliceStatus::Failed);
    CHECK(disk_result.diagnostic_code == "workflow_disk_budget_exceeded");
}

TEST_CASE("failed trial reports expire after a model config or plate change", "[AI][SmartSlicing][Workflow]")
{
    const int changed_component = GENERATE(0, 1, 2);
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Failed;
        result.diagnostic_code = "native_trial_failed";
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.snapshot().state == WorkflowState::Failed);
    REQUIRE(coordinator.snapshot().report);
    REQUIRE(coordinator.snapshot().context);
    const WorkspaceRevision original = coordinator.snapshot().context->revision;
    const auto failed_view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(failed_view.has_report);
    CHECK(failed_view.needs_polling);
    CHECK_FALSE(failed_view.can_apply);
    CHECK_FALSE(coordinator.refresh_revision());

    if (changed_component == 0) ++workspace.context.revision.model_revision;
    if (changed_component == 1) ++workspace.context.revision.config_revision;
    if (changed_component == 2) ++workspace.context.revision.plate_revision;
    workspace.context.revision.fingerprint = "revision-b";
    workspace.revision_unavailable = true;
    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().context->revision == original);
    workspace.revision_unavailable = false;
    REQUIRE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    const auto stale_view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK_FALSE(stale_view.has_report);
    CHECK(stale_view.issues.empty());
    CHECK(stale_view.candidates.empty());
    CHECK_FALSE(stale_view.can_apply);
    CHECK(stale_view.can_start);
    CHECK_FALSE(coordinator.select_candidate("baseline"));
    CHECK(executor.calls.size() == 1);

    executor.result_for = {};
    coordinator.start();
    REQUIRE(coordinator.snapshot().context);
    CHECK(coordinator.snapshot().context->revision == workspace.context.revision);
    CHECK(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(executor.calls.size() == 2);
}

TEST_CASE("Completed slicing refreshes native issues and Undo restores the original preflight", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    const auto baseline = workspace.context;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.on_commit = [&] { workspace.context.revision.config_revision++; };
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.snapshot().report->issues.front().code == IssueCode::NativeValidationUnavailable);
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());

    workspace.context.native_validation_available = true;
    const auto diagnostic = GENERATE(0, 1, 2);
    if (diagnostic == 1) workspace.context.validation_warnings.push_back("current native warning");
    if (diagnostic == 2) workspace.context.validation_errors.push_back("current native error");
    official.polled = {OfficialSlicePhase::Completed, {}, true, true};
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().report->revision == workspace.context.revision);
    CHECK(coordinator.snapshot().context->revision == baseline.revision);
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    if (diagnostic == 0) {
        CHECK(view.issues.empty());
    } else {
        REQUIRE(view.issues.size() == 1);
        CHECK(view.issues.front().first == (diagnostic == 1 ? "configuration_validation_warning" : "configuration_validation_error"));
        CHECK(view.issues.front().second == (diagnostic == 1 ? "current native warning" : "current native error"));
    }
    CHECK_FALSE(coordinator.refresh_revision());
    workspace.context = baseline; // Native Undo restores the captured project.
    REQUIRE(coordinator.undo_applied_candidate());
    REQUIRE(coordinator.snapshot().report->issues.size() == 1);
    CHECK(coordinator.snapshot().report->revision == baseline.revision);
    CHECK(coordinator.snapshot().report->issues.front().code == IssueCode::NativeValidationUnavailable);
}

TEST_CASE("Completed native reports wait for a usable capture of the applied revision", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    const auto baseline = workspace.context;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.on_commit = [&] { workspace.context.revision.config_revision++; };
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    workspace.context.native_validation_available = true;
    const auto failure = GENERATE(0, 1, 2, 3, 4, 5);
    workspace.capture_override = [&]() -> WorkspaceContext {
        if (failure == 4) throw std::runtime_error("temporary capture failure");
        auto current = workspace.context;
        if (failure == 0) current.native_validation_available = false;
        if (failure == 1) current.revision.model_revision++;
        if (failure == 2) current.revision.config_revision++;
        if (failure == 3) current.revision.plate_revision++;
        if (failure == 5) workspace.revision_unavailable = true;
        return current;
    };
    official.polled = {OfficialSlicePhase::Completed, {}, true, true};
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().report->issues.size() == 1);
    CHECK(coordinator.snapshot().report->revision == baseline.revision);
    CHECK(coordinator.snapshot().report->issues.front().code == IssueCode::NativeValidationUnavailable);
    workspace.capture_override = {};
    workspace.revision_unavailable = false;
    REQUIRE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().report->revision == workspace.context.revision);
    CHECK(coordinator.snapshot().report->issues.empty());
    CHECK_FALSE(coordinator.refresh_revision());
}

TEST_CASE("Failed formal slicing keeps its original preflight report", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    const auto baseline = workspace.context;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    workspace.context.native_validation_available = true;
    workspace.capture_override = []() -> WorkspaceContext { FAIL("Failed slicing must not publish a completed report"); return {}; };
    official.polled = {OfficialSlicePhase::Failed, "native slice failed", true, true};
    REQUIRE(coordinator.poll_official_slice());
    CHECK(coordinator.snapshot().state == WorkflowState::ApplyFailed);
    CHECK(coordinator.snapshot().report->revision == baseline.revision);
    CHECK(coordinator.snapshot().report->issues.front().code == IssueCode::NativeValidationUnavailable);
    CHECK_FALSE(coordinator.refresh_revision());
}

TEST_CASE("Immediate native completion refreshes its report without changing trial context", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.committed = {OfficialSlicePhase::Completed, {}, false, false};
    official.on_commit = [&] { workspace.context.native_validation_available = true; };
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().report->issues.empty());
    CHECK_FALSE(coordinator.snapshot().context->native_validation_available);
}

TEST_CASE("Manual slicing refreshes native validation without applying the selected suggestion", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    const auto before = coordinator.snapshot();
    REQUIRE(before.state == WorkflowState::ReadyToApply);
    REQUIRE(before.report->issues.front().code == IssueCode::NativeValidationUnavailable);
    workspace.context.native_validation_available = true;
    const auto diagnostic = GENERATE(0, 1, 2);
    if (diagnostic == 1) workspace.context.validation_warnings.push_back("manual native warning");
    if (diagnostic == 2) workspace.context.validation_errors.push_back("manual native error");
    REQUIRE(coordinator.refresh_revision());
    const auto& after = coordinator.snapshot();
    CHECK(after.state == before.state);
    CHECK(after.detail == before.detail);
    CHECK(after.selected_candidate_id == before.selected_candidate_id);
    CHECK(after.candidates.size() == before.candidates.size());
    CHECK_FALSE(after.can_undo_apply);
    CHECK_FALSE(after.context->native_validation_available);
    CHECK(after.context->revision == before.context->revision);
    CHECK(after.report->revision == workspace.context.revision);
    if (diagnostic == 0) {
        CHECK(after.report->issues.empty());
    } else {
        REQUIRE(after.report->issues.size() == 1);
        CHECK(after.report->issues.front().code == (diagnostic == 1 ? IssueCode::ConfigurationValidationWarning : IssueCode::ConfigurationValidationError));
        CHECK(after.report->issues.front().evidence == (diagnostic == 1 ? "manual native warning" : "manual native error"));
    }
    CHECK_FALSE(coordinator.refresh_revision());
}

TEST_CASE("Manual native reports keep the preflight until the same revision is available", "[AI][SmartSlicing][Apply]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    const auto before = coordinator.snapshot();
    workspace.context.native_validation_available = true;
    const auto failure = GENERATE(0, 1, 2, 3, 4, 5);
    workspace.capture_override = [&]() {
        if (failure == 4) throw std::runtime_error("manual capture unavailable");
        auto current = workspace.context;
        if (failure == 0) current.native_validation_available = false;
        if (failure == 1) current.revision.model_revision++;
        if (failure == 2) current.revision.config_revision++;
        if (failure == 3) current.revision.plate_revision++;
        if (failure == 5) workspace.revision_unavailable = true;
        return current;
    };
    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == before.state);
    CHECK(coordinator.snapshot().selected_candidate_id == before.selected_candidate_id);
    REQUIRE(coordinator.snapshot().report->issues.size() == before.report->issues.size());
    CHECK(coordinator.snapshot().report->issues.front().code == IssueCode::NativeValidationUnavailable);
    workspace.capture_override = {};
    workspace.revision_unavailable = false;
    REQUIRE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().report->issues.empty());
    CHECK(coordinator.snapshot().selected_candidate_id == before.selected_candidate_id);
}

TEST_CASE("Trial failure reasons belong to the matching candidate and clear on success", "[AI][SmartSlicing][Workflow]")
{
    const int outcome = GENERATE(0, 1, 2, 3, 4);
    auto candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "current"});
    candidate.diagnostic_message = "previous native failure";
    TrialSliceResult result;
    result.candidate_id = candidate.id;
    result.base_revision = candidate.base_revision;
    result.diagnostic_code = "trial_validation_failed";
    result.diagnostic_message = "current native failure";
    if (outcome == 1) result.candidate_id = "other-candidate";
    if (outcome == 2) result.base_revision.fingerprint = "other-revision";
    if (outcome == 3) result.status = TrialSliceStatus::Canceled;
    if (outcome == 4) {
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.diagnostic_code.clear();
    }
    CHECK(TrialSlicingWorkflow::accept_result(candidate, result) == (outcome == 4));
    CHECK(candidate.diagnostic_message == (outcome == 0 ? "current native failure" : ""));
    if (outcome == 1 || outcome == 2)
        CHECK(candidate.diagnostic_code == "trial_result_mismatch");
}

TEST_CASE("A failed baseline exposes the native reason and a fresh successful check clears it", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.diagnostic_code = "trial_validation_failed";
        result.diagnostic_message = "Prime Tower is partially outside the printable area.";
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    const auto failed = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(failed.candidates.size() == 1);
    CHECK(failed.candidates.front().diagnostic_message == "Prime Tower is partially outside the printable area.");
    CHECK(failed.summary_key == "baseline_trial_failed");
    CHECK_FALSE(failed.can_apply);
    executor.result_for = {};
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    const auto recovered = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(recovered.candidates.size() == 1);
    CHECK(recovered.candidates.front().diagnostic_message.empty());
    CHECK(recovered.summary_key == "candidates_ready");
    CHECK(recovered.can_apply);
}

TEST_CASE("Canceling an alternative retry does not present its previous native error as the current reason", "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t call) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        if (call == 0) {
            result.status = TrialSliceStatus::Succeeded;
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        } else if (call == 1) {
            result.diagnostic_code = "trial_validation_failed";
            result.diagnostic_message = "Previous native failure before retry.";
        } else {
            result.status = TrialSliceStatus::Canceled;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    REQUIRE_FALSE(coordinator.snapshot().candidates[1].diagnostic_message.empty());
    CHECK_FALSE(coordinator.retry_candidate("alternative"));
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(view.candidates.size() == 2);
    CHECK(view.summary_key == "candidates_ready");
    CHECK(view.candidates[1].diagnostic_code == "retry_canceled");
    CHECK(view.candidates[1].diagnostic_message.empty());
    CHECK(view.candidates[0].can_select);
    CHECK(view.can_apply);
}
