#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp"

#include <functional>
#include <stdexcept>

using namespace Slic3r::AI::SmartSlicing;

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

class WorkflowWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceContext context = printable_context();

    WorkspaceRevision current_revision() const override { return context.revision; }
    WorkspaceContext capture_context() const override { return context; }
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

} // namespace

TEST_CASE("candidate planning binds drafts to one workspace revision and caps proposals", "[AI][SmartSlicing][Workflow]")
{
    const WorkspaceContext context = printable_context();
    std::vector<SliceCandidate> proposals;
    proposals.push_back(proposal("z", context.revision));
    proposals.push_back(proposal("a", context.revision));
    proposals.push_back(proposal("extra", context.revision));
    proposals.push_back(proposal("stale", WorkspaceRevision{9, 9, 9, "old"}));

    const std::vector<SliceCandidate> planned = CandidatePlanningWorkflow().plan(context, proposals);

    REQUIRE(planned.size() == 3);
    CHECK(planned[0].id == "baseline");
    CHECK(planned[1].id == "a");
    CHECK(planned[2].id == "extra");
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
    CHECK(view.stages[3].status == Slic3r::GUI::SmartSlicingStageStatus::Active);
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
