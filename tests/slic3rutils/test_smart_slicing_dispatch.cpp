#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/OwnerThreadCallbackGate.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

WorkspaceContext dispatch_context(std::string fingerprint = "revision-a")
{
    WorkspaceContext context;
    context.revision = {1, 2, 3, std::move(fingerprint)};
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

SliceCandidate dispatch_candidate(std::string id, const WorkspaceRevision& revision)
{
    SliceCandidate candidate;
    candidate.id = std::move(id);
    candidate.base_revision = revision;
    return candidate;
}

class DispatchWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceRevision current_revision() const override { return context.revision; }
    WorkspaceContext capture_context() const override { return context; }

    WorkspaceContext context = dispatch_context();
};

class DispatchExecutor final : public ITrialSliceExecutor
{
public:
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        calls.push_back(candidate.id);
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == failed_candidate_id ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
            result.metrics->filament_volume_mm3 = 500.0;
            result.metrics->support_volume_mm3 = 10.0;
        } else {
            result.diagnostic_code = "trial_failed";
        }
        return result;
    }

    void cancel_trial_slice() override { ++cancel_count; }

    CandidateId failed_candidate_id;
    std::vector<CandidateId> calls;
    size_t cancel_count{0};
};

} // namespace

TEST_CASE("trial workers return pure results that only the owner accepts", "[AI][SmartSlicing][Dispatch]")
{
    DispatchWorkspace workspace;
    DispatchExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    const std::thread::id owner_thread = std::this_thread::get_id();
    std::vector<std::thread::id> publication_threads;
    coordinator.set_observer([&](const WorkflowSnapshot&) {
        publication_threads.push_back(std::this_thread::get_id());
    });
    coordinator.start();

    std::optional<CandidateTrialTask> task = coordinator.begin_candidate_trials(
        {dispatch_candidate("alternative", workspace.context.revision)});
    REQUIRE(task);
    CHECK(task->candidate.id == "baseline");
    CHECK(executor.calls.empty());
    REQUIRE(coordinator.snapshot().state == WorkflowState::TrialSlicingBaseline);

    std::optional<TrialSliceResult> worker_result;
    std::thread::id worker_thread;
    std::thread worker([&] {
        worker_thread = std::this_thread::get_id();
        worker_result = executor.execute_trial_slice(task->candidate);
    });
    worker.join();

    REQUIRE(worker_result);
    CHECK(worker_thread != owner_thread);
    CHECK(coordinator.snapshot().state == WorkflowState::TrialSlicingBaseline);
    CandidateTrialAcceptance baseline = coordinator.accept_candidate_trial_result(*task, std::move(*worker_result));
    REQUIRE(baseline.consumed);
    REQUIRE(baseline.accepted);
    REQUIRE(baseline.next_task);
    CHECK(coordinator.snapshot().state == WorkflowState::TrialSlicingCandidates);

    TrialSliceResult alternative = executor.execute_trial_slice(baseline.next_task->candidate);
    CandidateTrialAcceptance completed =
        coordinator.accept_candidate_trial_result(*baseline.next_task, std::move(alternative));
    CHECK(completed.consumed);
    CHECK(completed.accepted);
    CHECK_FALSE(completed.next_task);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(std::all_of(publication_threads.begin(), publication_threads.end(),
                      [&](std::thread::id thread) { return thread == owner_thread; }));
}

TEST_CASE("revision and cancellation reject trial results that arrive late", "[AI][SmartSlicing][Dispatch]")
{
    SECTION("revision changes before owner acceptance") {
        DispatchWorkspace workspace;
        DispatchExecutor executor;
        SmartSlicingCoordinator coordinator(workspace, executor);
        coordinator.start();
        const std::optional<CandidateTrialTask> task = coordinator.begin_candidate_trials();
        REQUIRE(task);
        TrialSliceResult result = executor.execute_trial_slice(task->candidate);
        workspace.context.revision.fingerprint = "revision-b";

        const CandidateTrialAcceptance acceptance =
            coordinator.accept_candidate_trial_result(*task, std::move(result));
        CHECK(acceptance.consumed);
        CHECK_FALSE(acceptance.accepted);
        CHECK(coordinator.snapshot().state == WorkflowState::Stale);
        CHECK(coordinator.snapshot().candidates.front().status == CandidateStatus::Stale);
    }

    SECTION("user cancellation wins over a late result") {
        DispatchWorkspace workspace;
        DispatchExecutor executor;
        SmartSlicingCoordinator coordinator(workspace, executor);
        size_t publications = 0;
        coordinator.set_observer([&](const WorkflowSnapshot&) { ++publications; });
        coordinator.start();
        const std::optional<CandidateTrialTask> task = coordinator.begin_candidate_trials();
        REQUIRE(task);
        TrialSliceResult result = executor.execute_trial_slice(task->candidate);
        coordinator.cancel();
        const size_t after_cancel = publications;

        const CandidateTrialAcceptance acceptance =
            coordinator.accept_candidate_trial_result(*task, std::move(result));
        CHECK_FALSE(acceptance.consumed);
        CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
        CHECK(publications == after_cancel);
    }
}

TEST_CASE("retry trial identity rejects attempt candidate and index mismatches", "[AI][SmartSlicing][Dispatch]")
{
    DispatchWorkspace workspace;
    DispatchExecutor executor;
    executor.failed_candidate_id = "alternative";
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates(
        {dispatch_candidate("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().state == WorkflowState::ReadyToApply);

    executor.failed_candidate_id.clear();
    const std::optional<CandidateTrialTask> task = coordinator.begin_candidate_retry("alternative");
    REQUIRE(task);
    TrialSliceResult result = executor.execute_trial_slice(task->candidate);

    CandidateTrialTask wrong_attempt = *task;
    ++wrong_attempt.attempt_id;
    CHECK_FALSE(coordinator.accept_candidate_trial_result(wrong_attempt, result).consumed);
    CandidateTrialTask wrong_index = *task;
    ++wrong_index.candidate_index;
    CHECK_FALSE(coordinator.accept_candidate_trial_result(wrong_index, result).consumed);
    CandidateTrialTask wrong_candidate = *task;
    wrong_candidate.candidate.id = "different-candidate";
    CHECK_FALSE(coordinator.accept_candidate_trial_result(wrong_candidate, result).consumed);
    CHECK(coordinator.snapshot().state == WorkflowState::TrialSlicingCandidates);

    const CandidateTrialAcceptance accepted =
        coordinator.accept_candidate_trial_result(*task, std::move(result));
    CHECK(accepted.consumed);
    CHECK(accepted.accepted);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
}

TEST_CASE("owner callback gate drops queued window work after shutdown", "[AI][SmartSlicing][Dispatch]")
{
    bool called = false;
    std::function<void()> queued;
    {
        OwnerThreadCallbackGate gate;
        queued = gate.guard([&] { called = true; });
        gate.close();
    }
    queued();
    CHECK_FALSE(called);

    OwnerThreadCallbackGate gate;
    std::function<void()> wrong_thread = gate.guard([&] { called = true; });
    std::atomic<bool> owner_assertion{false};
    std::thread worker([&] {
        try {
            wrong_thread();
        } catch (const std::logic_error&) {
            owner_assertion = true;
        }
    });
    worker.join();
    CHECK(owner_assertion);
    CHECK_FALSE(called);
}

TEST_CASE("presenter publishes on its owner and drops callbacks queued before destruction",
          "[AI][SmartSlicing][Dispatch]")
{
    DispatchWorkspace workspace;
    SmartSlicingCoordinator coordinator(workspace);
    std::vector<std::function<void()>> queued;
    size_t view_changes = 0;
    std::vector<std::thread::id> callback_threads;
    {
        Slic3r::GUI::SmartSlicingPresenter presenter(
            coordinator, [&](std::function<void()> callback) { queued.push_back(std::move(callback)); });
        presenter.set_view_changed([&](const Slic3r::GUI::SmartSlicingViewModel&) {
            ++view_changes;
            callback_threads.push_back(std::this_thread::get_id());
        });
        coordinator.start();
        REQUIRE_FALSE(queued.empty());
        std::function<void()> first = std::move(queued.front());
        queued.erase(queued.begin());
        first();
        CHECK(view_changes == 2);
    }

    const size_t before_late_callbacks = view_changes;
    for (std::function<void()>& callback : queued)
        callback();
    CHECK(view_changes == before_late_callbacks);
    CHECK(std::all_of(callback_threads.begin(), callback_threads.end(), [&](std::thread::id thread) {
        return thread == std::this_thread::get_id();
    }));
}
