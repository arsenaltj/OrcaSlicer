#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/ApplyWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Application/CachingTrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlacementTransformValidator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp"

#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

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
    context.native_validation_available = true;
    return context;
}

SliceCandidate proposal(std::string id, const WorkspaceRevision& revision)
{
    SliceCandidate candidate;
    candidate.id            = std::move(id);
    candidate.workflow_id   = 1;
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
    bool throw_on_revision{false};

    WorkspaceRevision current_revision() const override
    {
        if (throw_on_revision)
            throw std::runtime_error("revision unavailable");
        return context.revision;
    }
    WorkspaceContext capture_context() const override { return context; }
};

class FakeTrialSliceExecutor final : public ITrialSliceExecutor
{
public:
    std::vector<CandidateId> calls;
    size_t cancel_count{0};
    bool throw_on_cancel{false};
    std::function<TrialSliceResult(const SliceCandidate&, size_t)> result_for;

    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        calls.push_back(candidate.id);
        if (result_for) {
            TrialSliceResult result = result_for(candidate, calls.size() - 1);
            if (result.workflow_id == 0)
                result.workflow_id = candidate.workflow_id;
            return result;
        }

        TrialSliceResult result;
        result.workflow_id = candidate.workflow_id;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
        result.metrics->filament_volume_mm3 = 500.0;
        result.metrics->support_volume_mm3 = 10.0;
        return result;
    }

    void cancel_trial_slice() override
    {
        ++cancel_count;
        if (throw_on_cancel)
            throw std::runtime_error("cancel signal failed");
    }
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
    bool undo_succeeds{true};

    OfficialSliceResult prepare(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++prepare_calls;
        return prepared;
    }
    OfficialSliceResult commit(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++commit_calls;
        return committed;
    }
    OfficialSliceResult poll() override { return polled; }
    bool undo_last_apply() override
    {
        ++undo_calls;
        return undo_succeeds;
    }
};

} // namespace

TEST_CASE("successful trial results are cached by complete candidate content",
          "[AI][SmartSlicing][Workflow][Cache]")
{
    FakeTrialSliceExecutor delegate;
    CachingTrialSliceExecutor cache(delegate);
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.workflow_id = 1;

    const TrialSliceResult first  = cache.execute_trial_slice(candidate);
    const TrialSliceResult second = cache.execute_trial_slice(candidate);

    REQUIRE(first.metrics);
    REQUIRE(second.metrics);
    CHECK(first.metrics->estimated_time_seconds == second.metrics->estimated_time_seconds);
    CHECK(delegate.calls.size() == 1);

    SliceCandidate resumed = candidate;
    resumed.workflow_id = 2;
    const TrialSliceResult rebound = cache.execute_trial_slice(resumed);
    CHECK(rebound.workflow_id == resumed.workflow_id);
    CHECK(delegate.calls.size() == 1);

    SliceCandidate moved = candidate;
    ObjectTransform transform;
    transform.object_id   = 42;
    transform.instance_id = 7;
    transform.matrix[0]   = 1.0;
    moved.placement.transforms.push_back(transform);
    cache.execute_trial_slice(moved);

    SliceCandidate configured = candidate;
    configured.parameters.intent = ParameterIntent::Stability;
    configured.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 4,
                                              "brim_width", 0.0, 3.0, "stability"});
    cache.execute_trial_slice(configured);

    SliceCandidate revised = candidate;
    revised.base_revision.config_revision += 1;
    cache.execute_trial_slice(revised);

    CHECK(delegate.calls.size() == 4);
}

TEST_CASE("failed and canceled trial results are not cached and cancellation delegates",
          "[AI][SmartSlicing][Workflow][Cache]")
{
    FakeTrialSliceExecutor delegate;
    delegate.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = index < 2 ? TrialSliceStatus::Failed : TrialSliceStatus::Canceled;
        return result;
    };
    CachingTrialSliceExecutor cache(delegate);
    const SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    CHECK(cache.execute_trial_slice(candidate).status == TrialSliceStatus::Failed);
    CHECK(cache.execute_trial_slice(candidate).status == TrialSliceStatus::Failed);
    CHECK(cache.execute_trial_slice(candidate).status == TrialSliceStatus::Canceled);
    CHECK(cache.execute_trial_slice(candidate).status == TrialSliceStatus::Canceled);
    CHECK(delegate.calls.size() == 4);

    cache.cancel_trial_slice();
    CHECK(delegate.cancel_count == 1);
}

TEST_CASE("successful transport with invalid metrics is not cached",
          "[AI][SmartSlicing][Workflow][Cache][MetricValidation]")
{
    FakeTrialSliceExecutor delegate;
    delegate.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = index == 0 ? std::numeric_limits<double>::quiet_NaN() : 80.0;
        return result;
    };
    CachingTrialSliceExecutor cache(delegate);
    const SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    const TrialSliceResult invalid = cache.execute_trial_slice(candidate);
    const TrialSliceResult valid   = cache.execute_trial_slice(candidate);
    const TrialSliceResult reused  = cache.execute_trial_slice(candidate);

    REQUIRE(invalid.metrics);
    CHECK_FALSE(invalid.metrics->has_valid_measurements());
    REQUIRE(valid.metrics);
    CHECK(valid.metrics->has_valid_measurements());
    REQUIRE(reused.metrics);
    CHECK(reused.metrics->estimated_time_seconds == valid.metrics->estimated_time_seconds);
    CHECK(delegate.calls.size() == 2);
}

TEST_CASE("trial cache converts execution exceptions and absorbs cancellation exceptions",
          "[AI][SmartSlicing][Workflow][Cache][ExceptionBoundary]")
{
    FakeTrialSliceExecutor delegate;
    delegate.result_for = [](const SliceCandidate&, size_t) -> TrialSliceResult {
        throw std::runtime_error("delegate execution failed");
    };
    delegate.throw_on_cancel = true;
    CachingTrialSliceExecutor cache(delegate);
    const SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    const TrialSliceResult first = cache.execute_trial_slice(candidate);
    CHECK(first.candidate_id == candidate.id);
    CHECK(first.base_revision == candidate.base_revision);
    CHECK(first.status == TrialSliceStatus::Failed);
    CHECK_FALSE(first.metrics.has_value());
    CHECK(first.diagnostic_code == "trial_slice_executor_exception");
    CHECK(cache.execute_trial_slice(candidate).status == TrialSliceStatus::Failed);
    CHECK(delegate.calls.size() == 2);
    CHECK_NOTHROW(cache.cancel_trial_slice());
    CHECK(delegate.cancel_count == 1);
}

TEST_CASE("mismatched successful trial results are not cached",
          "[AI][SmartSlicing][Workflow][Cache]")
{
    FakeTrialSliceExecutor delegate;
    delegate.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = "different-candidate";
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        return result;
    };
    CachingTrialSliceExecutor cache(delegate);
    const SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    cache.execute_trial_slice(candidate);
    cache.execute_trial_slice(candidate);

    CHECK(delegate.calls.size() == 2);
}

TEST_CASE("trial result cache evicts the oldest successful entry",
          "[AI][SmartSlicing][Workflow][Cache]")
{
    FakeTrialSliceExecutor delegate;
    CachingTrialSliceExecutor cache(delegate, 2);
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};

    cache.execute_trial_slice(proposal("a", revision));
    cache.execute_trial_slice(proposal("b", revision));
    cache.execute_trial_slice(proposal("c", revision));
    cache.execute_trial_slice(proposal("a", revision));

    CHECK(delegate.calls == std::vector<CandidateId>{"a", "b", "c", "a"});
}

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

TEST_CASE("invalid numeric trial metrics fail before a candidate becomes ready or retry succeeds",
          "[AI][SmartSlicing][Workflow][MetricValidation]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 :
            std::numeric_limits<double>::quiet_NaN();
        result.metrics->filament_volume_mm3 = 500.0;
        result.metrics->support_volume_mm3 = 10.0;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    REQUIRE(coordinator.plan_and_slice_candidates({proposal("invalid-metrics", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK_FALSE(coordinator.snapshot().candidates[1].metrics);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "invalid_candidate_metrics");
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");

    CHECK_FALSE(coordinator.retry_candidate("invalid-metrics"));
    CHECK(executor.calls == std::vector<CandidateId>{"baseline", "invalid-metrics", "invalid-metrics"});
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "invalid_candidate_metrics");
}

TEST_CASE("invalid baseline metrics fail the workflow before comparison",
          "[AI][SmartSlicing][Workflow][MetricValidation]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = -1.0;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "baseline_trial_failed");
    REQUIRE(coordinator.snapshot().candidates.size() == 1);
    CHECK(coordinator.snapshot().candidates.front().status == CandidateStatus::Failed);
    CHECK_FALSE(coordinator.snapshot().candidates.front().metrics);
    CHECK(coordinator.snapshot().candidates.front().diagnostic_code == "invalid_candidate_metrics");
    CHECK_FALSE(coordinator.snapshot().comparison);
}

TEST_CASE("candidate preparation failure terminates before any trial executor fallback",
          "[AI][SmartSlicing][Workflow][GUIThreadBoundary]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    REQUIRE(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
    CHECK(coordinator.fail_candidate_preparation());
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "candidate_preparation_failed");
    CHECK(executor.calls.empty());
}

TEST_CASE("alternative executor exceptions become explicit failures while the baseline remains available",
          "[AI][SmartSlicing][Workflow][ExceptionBoundary]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        if (candidate.id == "alternative")
            throw std::runtime_error("alternative executor failed");

        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 100.0;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "trial_slice_executor_exception");
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "baseline");
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");
}

TEST_CASE("successful baseline trial resolves only unavailable native validation evidence",
          "[AI][SmartSlicing][Workflow]")
{
    WorkflowWorkspace workspace;
    workspace.context.native_validation_available = false;
    workspace.context.validation_warnings.push_back("Keep this native warning.");
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);

    coordinator.start();
    REQUIRE(coordinator.snapshot().report);
    REQUIRE(coordinator.snapshot().report->issues.size() == 2);
    CHECK(coordinator.snapshot().report->readiness == Readiness::NeedsAttention);

    REQUIRE(coordinator.plan_and_slice_candidates());

    const WorkflowSnapshot& snapshot = coordinator.snapshot();
    REQUIRE(snapshot.report);
    CHECK(snapshot.report->revision == workspace.context.revision);
    CHECK(std::none_of(snapshot.report->issues.begin(), snapshot.report->issues.end(), [](const PrintabilityIssue& issue) {
        return issue.code == IssueCode::NativeValidationUnavailable;
    }));
    CHECK(std::any_of(snapshot.report->issues.begin(), snapshot.report->issues.end(), [](const PrintabilityIssue& issue) {
        return issue.code == IssueCode::ConfigurationValidationWarning &&
               issue.evidence == "Keep this native warning.";
    }));
    CHECK(snapshot.report->readiness == Readiness::NeedsAttention);
    REQUIRE(snapshot.context);
    CHECK_FALSE(snapshot.context->native_validation_available);
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

TEST_CASE("trial results from another workflow are never attached to a current candidate",
          "[AI][SmartSlicing][Workflow][Ownership]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.workflow_id = index == 0 ? candidate.workflow_id : candidate.workflow_id + 1;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = index == 0 ? 100.0 : 80.0;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().candidates[0].workflow_id == coordinator.snapshot().workflow_id);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "trial_result_mismatch");
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

TEST_CASE("cancel reaches a clean terminal state even when the executor cancel signal throws",
          "[AI][SmartSlicing][Workflow][ExceptionBoundary]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.throw_on_cancel = true;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    coordinator.set_observer([&coordinator](const WorkflowSnapshot& snapshot) {
        if (snapshot.state == WorkflowState::TrialSlicingBaseline)
            coordinator.cancel();
    });

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().detail == "canceled");
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

TEST_CASE("candidate workflow exceptions do not expose adapter details",
          "[AI][SmartSlicing][Workflow][ExceptionBoundary]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.set_resource_budget({}, []() -> WorkflowResourceUsage {
        throw std::runtime_error("sensitive local path and adapter details");
    });
    coordinator.start();

    CHECK_FALSE(coordinator.plan_and_slice_candidates());
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "candidate_workflow_exception");
    CHECK(coordinator.snapshot().detail.find("sensitive") == std::string::npos);
}

TEST_CASE("a cancellation requested as the final trial returns wins over candidate publication",
          "[AI][SmartSlicing][Workflow][Cancellation][Background]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    bool cancellation_requested = false;
    executor.result_for = [&cancellation_requested](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 100.0;
        cancellation_requested = true;
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    CHECK_FALSE(coordinator.plan_and_slice_candidates({}, CandidateGoal::Stability, true,
                                                       [&cancellation_requested] {
                                                           return cancellation_requested;
                                                       }));
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().detail == "canceled");
    CHECK(coordinator.snapshot().candidates.empty());
    CHECK_FALSE(coordinator.snapshot().comparison);
    CHECK(executor.cancel_count == 1);
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

TEST_CASE("an alternative trial timeout retains the comparable baseline",
          "[AI][SmartSlicing][Workflow][CandidateFailure][Runtime][Cancellation]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t index) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = index == 0 ? TrialSliceStatus::Succeeded : TrialSliceStatus::Canceled;
        if (index == 0) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        } else {
            result.diagnostic_code = "workflow_timeout";
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "workflow_timeout");
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");
    REQUIRE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "baseline");
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
    REQUIRE(view.candidates.size() == 1);
    CHECK(view.candidates.front().recommended);
    CHECK(view.candidates.front().selected);
    CHECK(view.can_apply);
    CHECK(view.legacy_steps == std::array<Slic3r::GUI::LegacyAIWorkflowStatus, 6>{
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Success,
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Success,
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Success,
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Success,
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Waiting,
                                   Slic3r::GUI::LegacyAIWorkflowStatus::Waiting});
}

TEST_CASE("formal apply states keep the legacy projection aligned with the workbench",
          "[AI][SmartSlicing][Workflow]")
{
    using LegacyStatus = Slic3r::GUI::LegacyAIWorkflowStatus;

    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::PlanningCandidates;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Running, LegacyStatus::Waiting, LegacyStatus::Waiting});

    snapshot.state = WorkflowState::TrialSlicingBaseline;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Running, LegacyStatus::Waiting});

    snapshot.state = WorkflowState::TrialSlicingCandidates;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Running, LegacyStatus::Waiting});

    snapshot.state = WorkflowState::Applying;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Running, LegacyStatus::Waiting});

    snapshot.state = WorkflowState::OfficialSlicing;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Running, LegacyStatus::Waiting});

    snapshot.state = WorkflowState::Completed;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success});

    snapshot.state = WorkflowState::ApplyFailed;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).legacy_steps ==
          std::array<LegacyStatus, 6>{LegacyStatus::Success, LegacyStatus::Success, LegacyStatus::Success,
                                      LegacyStatus::Success, LegacyStatus::Failed, LegacyStatus::Waiting});
}

TEST_CASE("legacy Sidebar uses the workbench summary for candidate and apply phases",
          "[AI][SmartSlicing][GUI][LegacyProjection][Summary]")
{
    const wxString preflight = Slic3r::GUI::smart_slicing_summary_text("inspecting_printability");
    const wxString fallback = Slic3r::GUI::smart_slicing_summary_text("unknown_summary");
    const wxString candidates = Slic3r::GUI::smart_slicing_summary_text("candidates_ready");
    const wxString applying = Slic3r::GUI::smart_slicing_summary_text("applying_candidate");
    const wxString slicing = Slic3r::GUI::smart_slicing_summary_text("official_slicing");

    CHECK_FALSE(candidates == preflight);
    CHECK_FALSE(candidates == fallback);
    CHECK_FALSE(applying == preflight);
    CHECK_FALSE(slicing == preflight);
}

TEST_CASE("apply failure summary only promises recovery when native undo is available",
          "[AI][SmartSlicing][GUI][Apply][Summary]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::ApplyFailed;

    const auto without_recovery = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK_FALSE(without_recovery.can_undo_apply);
    CHECK(without_recovery.summary_key == "official_slice_failed_no_recovery");

    snapshot.can_undo_apply = true;
    const auto with_recovery = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(with_recovery.can_undo_apply);
    CHECK(with_recovery.summary_key == "official_slice_failed");
    CHECK_FALSE(Slic3r::GUI::smart_slicing_summary_text(without_recovery.summary_key) ==
                Slic3r::GUI::smart_slicing_summary_text(with_recovery.summary_key));

    snapshot.can_undo_apply = false;
    snapshot.workspace_mutated = true;
    const auto applied_without_recovery = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK_FALSE(applied_without_recovery.can_undo_apply);
    CHECK(applied_without_recovery.workspace_mutated);
    CHECK(applied_without_recovery.summary_key == "official_slice_failed_applied");
    CHECK_FALSE(Slic3r::GUI::smart_slicing_summary_text(applied_without_recovery.summary_key) ==
                Slic3r::GUI::smart_slicing_summary_text(without_recovery.summary_key));
}

TEST_CASE("failed candidate diagnostics are projected as actionable localized reasons",
          "[AI][SmartSlicing][GUI][CandidateFailure][Diagnostic]")
{
    const wxString fallback = Slic3r::GUI::smart_slicing_candidate_failure_text("unknown_failure");
    const wxString memory =
        Slic3r::GUI::smart_slicing_candidate_failure_text("workflow_memory_budget_exceeded");
    const wxString timeout = Slic3r::GUI::smart_slicing_candidate_failure_text("workflow_timeout");
    const wxString revision =
        Slic3r::GUI::smart_slicing_candidate_failure_text("retry_revision_unavailable");
    const wxString placement =
        Slic3r::GUI::smart_slicing_candidate_failure_text("invalid_candidate_placement");
    const wxString locked_plate =
        Slic3r::GUI::smart_slicing_candidate_failure_text("current_plate_locked");
    const wxString repair =
        Slic3r::GUI::smart_slicing_candidate_failure_text("candidate_repair_unsupported");

    CHECK_FALSE(memory == fallback);
    CHECK_FALSE(timeout == fallback);
    CHECK_FALSE(revision == fallback);
    CHECK_FALSE(placement == fallback);
    CHECK_FALSE(locked_plate == fallback);
    CHECK_FALSE(repair == fallback);
    CHECK(Slic3r::GUI::smart_slicing_candidate_failure_text("") == fallback);
}

TEST_CASE("workbench goal selection maps every visible choice with a stable default",
          "[AI][SmartSlicing][GUI][Goal]")
{
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(0) == CandidateGoal::Stability);
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(1) == CandidateGoal::Quality);
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(2) == CandidateGoal::Speed);
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(3) == CandidateGoal::MaterialSaving);
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(wxNOT_FOUND) == CandidateGoal::Stability);
    CHECK(Slic3r::GUI::smart_slicing_goal_from_selection(4) == CandidateGoal::Stability);
}

TEST_CASE("recommended baseline keeps its measured recommendation explanation",
          "[AI][SmartSlicing][GUI][CandidateEvidence]")
{
    Slic3r::GUI::SmartSlicingCandidateView baseline;
    baseline.id = "baseline";
    baseline.recommended = true;
    baseline.evidence_codes = {"lower_support_volume"};

    const wxString reason = Slic3r::GUI::smart_slicing_candidate_reason_text(baseline);

    CHECK(reason.Find(wxString::FromUTF8(u8"推荐保留")) != wxNOT_FOUND);
    CHECK(reason.Find(wxString::FromUTF8(u8"支撑用量更低")) != wxNOT_FOUND);
}

TEST_CASE("candidate explanation distinguishes complete evidence from a measured advantage",
          "[AI][SmartSlicing][GUI][CandidateEvidence]")
{
    Slic3r::GUI::SmartSlicingCandidateView candidate;
    candidate.recommended = true;
    candidate.evidence_codes = {"more_complete_trial_evidence"};

    const wxString reason = Slic3r::GUI::smart_slicing_candidate_reason_text(candidate);

    CHECK(reason.Find(wxString::FromUTF8(u8"试切证据更完整")) != wxNOT_FOUND);
}

TEST_CASE("hiding the workbench never reads workflow eligibility across a running worker",
          "[AI][SmartSlicing][GUI][Lifecycle][Hide]")
{
    using Slic3r::GUI::smart_slicing_hide_action;
    using Slic3r::GUI::SmartSlicingHideAction;

    CHECK(smart_slicing_hide_action(false, false, true) == SmartSlicingHideAction::CancelDirectly);
    CHECK(smart_slicing_hide_action(false, true, true) == SmartSlicingHideAction::RequestBackgroundCancel);
    CHECK(smart_slicing_hide_action(true, false, true) == SmartSlicingHideAction::None);
    CHECK(smart_slicing_hide_action(true, true, true) == SmartSlicingHideAction::None);
    CHECK(smart_slicing_hide_action(false, false, false) == SmartSlicingHideAction::None);
    CHECK(smart_slicing_hide_action(false, true, false) == SmartSlicingHideAction::RequestBackgroundCancel);
}

TEST_CASE("workflow commands are gated while a smart-slicing worker owns the coordinator",
          "[AI][SmartSlicing][GUI][Lifecycle][WorkerOwnership]")
{
    CHECK(Slic3r::GUI::smart_slicing_workflow_command_allowed(false));
    CHECK_FALSE(Slic3r::GUI::smart_slicing_workflow_command_allowed(true));
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
            result.metrics->bed_adhesion_risk_score = candidate.id == "baseline" ? 1.5 : 0.75;
            result.metrics->brim_volume_mm3         = candidate.id == "baseline" ? 0.0 : 60.0;
            if (candidate.id == "alternative")
                result.metrics->warning_codes = {"native_validation_warning", "gcode_warning"};
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    SliceCandidate alternative = proposal("alternative", workspace.context.revision);
    alternative.repair = RepairPlan{{"repair_open_edges"}, false};
    alternative.placement.transforms.push_back({42, 84, {}});
    alternative.parameters.intent = ParameterIntent::Stability;
    alternative.parameters.entries.push_back({ConfigScope::Plate,
                                                PresetOwner::Process,
                                                3,
                                                "brim_width",
                                                1.0,
                                                5.0,
                                                "improve_small_footprint_adhesion"});
    REQUIRE(coordinator.plan_and_slice_candidates({std::move(alternative)}));

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
    CHECK(ready_view.candidates[1].bed_adhesion_risk_delta == -0.75);
    CHECK(ready_view.candidates[1].brim_volume_delta_mm3 == 60.0);
    CHECK(ready_view.candidates[1].tool_change_delta == -2);
    CHECK(ready_view.candidates[1].repair_operation_count == 1);
    CHECK(ready_view.candidates[1].transformed_instance_count == 1);
    CHECK(ready_view.candidates[1].plate_parameter_change_count == 1);
    CHECK(ready_view.candidates[1].brim_width_before == 1.0);
    CHECK(ready_view.candidates[1].brim_width_after == 5.0);
    CHECK(ready_view.candidates[1].warning_codes ==
          std::vector<std::string>{"native_validation_warning", "gcode_warning"});
    CHECK(workspace.context.revision.fingerprint == "revision-a");
}

TEST_CASE("multicolor candidates excluded by comparison cannot be selected",
          "[AI][SmartSlicing][Workflow][Candidate][Multicolor][Eligibility]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
        result.metrics->physical_slots_compatible = candidate.id != "incompatible";
        result.metrics->color_mapping_degraded = candidate.id == "degraded";
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();

    REQUIRE(coordinator.plan_and_slice_candidates({proposal("incompatible", workspace.context.revision),
                                                   proposal("degraded", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().comparison->excluded_candidate_ids ==
          std::vector<CandidateId>{"degraded", "incompatible"});
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");

    const Slic3r::GUI::SmartSlicingViewModel view =
        Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    REQUIRE(view.candidates.size() == 3);
    const auto incompatible = std::find_if(view.candidates.begin(), view.candidates.end(), [](const auto& candidate) {
        return candidate.id == "incompatible";
    });
    const auto degraded = std::find_if(view.candidates.begin(), view.candidates.end(), [](const auto& candidate) {
        return candidate.id == "degraded";
    });
    REQUIRE(incompatible != view.candidates.end());
    REQUIRE(degraded != view.candidates.end());
    CHECK(incompatible->excluded);
    CHECK(incompatible->exclusion_reason_code == "incompatible_physical_slots");
    CHECK_FALSE(incompatible->can_select);
    CHECK(degraded->excluded);
    CHECK(degraded->exclusion_reason_code == "color_mapping_degraded");
    CHECK_FALSE(degraded->can_select);
    CHECK_FALSE(coordinator.select_candidate("incompatible"));
    CHECK_FALSE(coordinator.select_candidate("degraded"));
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");
}

TEST_CASE("candidate selection keeps the ready comparison when revision capture is temporarily unavailable",
          "[AI][SmartSlicing][Workflow][CandidateFailure]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(coordinator.snapshot().selected_candidate_id == "alternative");

    workspace.throw_on_revision = true;
    CHECK_FALSE(coordinator.select_candidate("baseline"));
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().detail == "candidate_selection_revision_unavailable");
    CHECK(coordinator.snapshot().selected_candidate_id == "alternative");
    CHECK(coordinator.snapshot().candidates.size() == 2);
}

TEST_CASE("retry executor exceptions retain the baseline and failed alternative",
          "[AI][SmartSlicing][Workflow][CandidateFailure]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    executor.result_for = [](const SliceCandidate&, size_t) -> TrialSliceResult {
        throw std::runtime_error("retry executor failed");
    };
    CHECK_FALSE(coordinator.retry_candidate("alternative"));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "retry_executor_exception");
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "baseline");
}

TEST_CASE("candidate retry does not start after the workflow resource budget is exceeded",
          "[AI][SmartSlicing][Workflow][CandidateFailure][Runtime]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    bool fail_alternative = true;
    executor.result_for = [&fail_alternative](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" && fail_alternative ? TrialSliceStatus::Failed :
                                                                            TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = candidate.id == "baseline" ? 100.0 : 80.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    WorkflowResourceBudget budget;
    WorkflowResourceUsage usage;
    coordinator.set_resource_budget(budget, [&usage] { return usage; });
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));
    REQUIRE(executor.calls == std::vector<CandidateId>{"baseline", "alternative"});

    fail_alternative   = false;
    usage.memory_bytes = budget.maximum_memory_bytes + 1;
    CHECK_FALSE(coordinator.retry_candidate("alternative"));
    CHECK(executor.calls == std::vector<CandidateId>{"baseline", "alternative"});
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().detail == "workflow_memory_budget_exceeded");
    CHECK(coordinator.snapshot().selected_candidate_id == "baseline");
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "workflow_memory_budget_exceeded");
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "baseline");
}

TEST_CASE("an explicit cancellation during candidate retry ends the workflow",
          "[AI][SmartSlicing][Workflow][CandidateFailure][Cancellation]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id    = candidate.id;
        result.base_revision   = candidate.base_revision;
        result.status          = TrialSliceStatus::Canceled;
        result.diagnostic_code = "trial_slice_canceled";
        return result;
    };
    CHECK_FALSE(coordinator.retry_candidate("alternative", true));
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().detail == "trial_slice_canceled");
    CHECK(coordinator.snapshot().candidates.empty());
    CHECK_FALSE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().selected_candidate_id.empty());
}

TEST_CASE("a cancellation requested as a retry returns wins over the successful result",
          "[AI][SmartSlicing][Workflow][CandidateFailure][Cancellation][Background]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    bool cancellation_requested = false;
    executor.result_for = [&cancellation_requested](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 80.0;
        cancellation_requested = true;
        return result;
    };

    CHECK_FALSE(coordinator.retry_candidate("alternative", true, [&cancellation_requested] {
        return cancellation_requested;
    }));
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().detail == "canceled");
    CHECK(coordinator.snapshot().candidates.empty());
    CHECK_FALSE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().selected_candidate_id.empty());
    CHECK(executor.cancel_count == 1);
}

TEST_CASE("a cancellation requested as a retry throws wins over the executor failure",
          "[AI][SmartSlicing][Workflow][CandidateFailure][Cancellation][ExceptionBoundary]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    bool cancellation_requested = false;
    executor.result_for = [&cancellation_requested](const SliceCandidate&, size_t) -> TrialSliceResult {
        cancellation_requested = true;
        throw std::runtime_error("retry failed after cancellation");
    };

    CHECK_FALSE(coordinator.retry_candidate("alternative", true, [&cancellation_requested] {
        return cancellation_requested;
    }));
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().detail == "canceled");
    CHECK(coordinator.snapshot().candidates.empty());
    CHECK_FALSE(coordinator.snapshot().comparison);
    CHECK(coordinator.snapshot().selected_candidate_id.empty());
    CHECK(executor.cancel_count == 1);
}

TEST_CASE("retry discards a completed result when final revision capture is unavailable",
          "[AI][SmartSlicing][Workflow][CandidateFailure]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor executor;
    executor.result_for = [](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = candidate.id == "alternative" ? TrialSliceStatus::Failed : TrialSliceStatus::Succeeded;
        if (result.status == TrialSliceStatus::Succeeded) {
            result.metrics = SlicingMetrics{};
            result.metrics->estimated_time_seconds = 100.0;
        }
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates({proposal("alternative", workspace.context.revision)}));

    executor.result_for = [&workspace](const SliceCandidate& candidate, size_t) {
        TrialSliceResult result;
        result.candidate_id  = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status        = TrialSliceStatus::Succeeded;
        result.metrics       = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 80.0;
        workspace.throw_on_revision = true;
        return result;
    };
    CHECK_FALSE(coordinator.retry_candidate("alternative"));
    REQUIRE(coordinator.snapshot().candidates.size() == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(coordinator.snapshot().candidates[0].status == CandidateStatus::Ready);
    CHECK(coordinator.snapshot().candidates[1].status == CandidateStatus::Failed);
    CHECK_FALSE(coordinator.snapshot().candidates[1].metrics.has_value());
    CHECK(coordinator.snapshot().candidates[1].diagnostic_code == "retry_revision_unavailable");
    CHECK(coordinator.snapshot().comparison->recommended_candidate_id == "baseline");
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

TEST_CASE("apply workflow rejects invalid transaction identity before entering the gateway",
          "[AI][SmartSlicing][Apply][TransactionBoundary][Identity]")
{
    FakeOfficialSliceGateway gateway;
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    SliceCandidate candidate = proposal("candidate", revision);

    SECTION("reserved workflow id") {
        candidate.workflow_id = 0;
        const OfficialSliceResult result = ApplyWorkflow().start(candidate, revision, revision, gateway);
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_candidate_identity");
    }

    SECTION("empty candidate id") {
        candidate.id.clear();
        const OfficialSliceResult result = ApplyWorkflow().start(candidate, revision, revision, gateway);
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_candidate_identity");
    }

    SECTION("invalid revision") {
        candidate.base_revision = {};
        const OfficialSliceResult result = ApplyWorkflow().start(candidate, {}, {}, gateway);
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_workspace_revision");
    }

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
    CHECK_FALSE(coordinator.snapshot().workspace_mutated);
    CHECK(official.commit_calls == 0);
}

TEST_CASE("coordinator preserves whether an unrecoverable official failure already mutated the workspace",
          "[AI][SmartSlicing][Apply][TransactionFact]")
{
    WorkflowWorkspace workspace;
    FakeTrialSliceExecutor trial;
    FakeOfficialSliceGateway official;
    official.committed = {OfficialSlicePhase::Slicing, {}, true, false};
    SmartSlicingCoordinator coordinator(workspace, trial, official);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());

    REQUIRE(coordinator.apply_selected_candidate());
    CHECK(coordinator.snapshot().workspace_mutated);
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);

    official.polled = {OfficialSlicePhase::Failed, "official_slice_revision_changed", true, false};
    REQUIRE(coordinator.poll_official_slice());
    CHECK(coordinator.snapshot().state == WorkflowState::ApplyFailed);
    CHECK(coordinator.snapshot().workspace_mutated);
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
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
    CHECK_FALSE(coordinator.snapshot().workspace_mutated);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("an unavailable native recovery is disabled after one safe refusal", "[AI][SmartSlicing][Apply][UndoOwnership]")
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
    official.undo_succeeds = false;

    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(official.undo_calls == 1);
    CHECK(coordinator.snapshot().state == WorkflowState::ApplyFailed);
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK(coordinator.snapshot().workspace_mutated);
    CHECK(coordinator.snapshot().detail == "apply_undo_unavailable");
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
    REQUIRE(gateway.prepare(candidate, current).phase == OfficialSlicePhase::Prepared);
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

TEST_CASE("Orca official gateway rejects completion when revision ownership changes or becomes unavailable",
          "[AI][SmartSlicing][Apply][CompletionOwnership][TransactionBoundary]")
{
    const WorkspaceRevision original{1, 2, 3, "revision-a"};
    WorkspaceRevision current = original;
    bool revision_unavailable = false;
    size_t preview_calls = 0;
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&current, &revision_unavailable] {
            if (revision_unavailable)
                throw std::runtime_error("revision unavailable");
            return current;
        },
        [](const SliceCandidate&) { return std::string{}; },
        [&current](const SliceCandidate&) {
            current = {2, 2, 3, "revision-after-smart-apply"};
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [] { return true; },
        [&preview_calls] {
            ++preview_calls;
            return true;
        },
        [&undo_calls] {
            ++undo_calls;
            return true;
        });
    SliceCandidate candidate = proposal("candidate", original);

    REQUIRE(gateway.prepare(candidate, original).phase == OfficialSlicePhase::Prepared);
    REQUIRE(gateway.commit(candidate, original).phase == OfficialSlicePhase::Slicing);
    revision_unavailable = GENERATE(false, true);
    CAPTURE(revision_unavailable);
    if (!revision_unavailable)
        current = {3, 2, 3, "revision-after-user-edit"};
    gateway.notify_slice_completed(true);

    const OfficialSliceResult result = gateway.poll();
    CHECK(result.phase == OfficialSlicePhase::Failed);
    CHECK(result.diagnostic_code == (revision_unavailable ? "official_slice_revision_unavailable" :
                                                           "official_slice_revision_changed"));
    CHECK(result.workspace_mutated);
    CHECK_FALSE(result.can_undo);
    CHECK(preview_calls == 0);
    CHECK_FALSE(gateway.undo_last_apply());
    CHECK(undo_calls == 0);
}

TEST_CASE("Orca official gateway requires one prepared ready candidate before formal mutation",
          "[AI][SmartSlicing][Apply][TransactionBoundary]")
{
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    size_t compatibility_calls = 0;
    size_t apply_calls = 0;
    size_t slice_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [revision] { return revision; },
        [&compatibility_calls](const SliceCandidate&) {
            ++compatibility_calls;
            return std::string{};
        },
        [&apply_calls](const SliceCandidate&) {
            ++apply_calls;
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [&slice_calls] { ++slice_calls; return true; }, [] { return true; }, [] { return true; });
    SliceCandidate candidate = proposal("candidate", revision);
    candidate.workflow_id = 7;

    const OfficialSliceResult unprepared = gateway.commit(candidate, revision);
    CHECK(unprepared.phase == OfficialSlicePhase::Rejected);
    CHECK(unprepared.diagnostic_code == "candidate_not_prepared");

    candidate.status = CandidateStatus::Failed;
    const OfficialSliceResult not_ready = gateway.prepare(candidate, revision);
    CHECK(not_ready.phase == OfficialSlicePhase::Rejected);
    CHECK(not_ready.diagnostic_code == "candidate_not_ready");

    candidate.status = CandidateStatus::Ready;
    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    const OfficialSliceResult different_candidate = gateway.commit(proposal("other", revision), revision);
    CHECK(different_candidate.phase == OfficialSlicePhase::Rejected);
    CHECK(different_candidate.diagnostic_code == "candidate_not_prepared");

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    SliceCandidate changed_content = candidate;
    ObjectTransform added_transform;
    added_transform.object_id = 42;
    added_transform.instance_id = 43;
    added_transform.matrix = {1.0, 0.0, 0.0, 5.0,
                              0.0, 1.0, 0.0, 0.0,
                              0.0, 0.0, 1.0, 0.0,
                              0.0, 0.0, 0.0, 1.0};
    changed_content.placement.transforms.push_back(added_transform);
    const OfficialSliceResult changed_candidate = gateway.commit(changed_content, revision);
    CHECK(changed_candidate.phase == OfficialSlicePhase::Rejected);
    CHECK(changed_candidate.diagnostic_code == "candidate_not_prepared");

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    SliceCandidate changed_workflow = candidate;
    changed_workflow.workflow_id = 8;
    const OfficialSliceResult changed_owner = gateway.commit(changed_workflow, revision);
    CHECK(changed_owner.phase == OfficialSlicePhase::Rejected);
    CHECK(changed_owner.diagnostic_code == "candidate_not_prepared");

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    REQUIRE(gateway.commit(candidate, revision).phase == OfficialSlicePhase::Slicing);
    const OfficialSliceResult overlapping_prepare = gateway.prepare(proposal("other", revision), revision);
    CHECK(overlapping_prepare.phase == OfficialSlicePhase::Rejected);
    CHECK(overlapping_prepare.diagnostic_code == "official_slice_in_progress");
    const OfficialSliceResult consumed = gateway.commit(candidate, revision);
    CHECK(consumed.phase == OfficialSlicePhase::Rejected);
    CHECK(consumed.diagnostic_code == "official_slice_in_progress");
    CHECK(compatibility_calls == 5);
    CHECK(apply_calls == 1);
    CHECK(slice_calls == 1);
}

TEST_CASE("Orca official gateway rejects invalid transaction identity before compatibility or mutation",
          "[AI][SmartSlicing][Apply][TransactionBoundary][Identity]")
{
    WorkspaceRevision current{1, 2, 3, "revision-a"};
    size_t revision_calls = 0;
    size_t compatibility_calls = 0;
    size_t apply_calls = 0;
    size_t slice_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&current, &revision_calls] {
            ++revision_calls;
            return current;
        },
        [&compatibility_calls](const SliceCandidate&) {
            ++compatibility_calls;
            return std::string{};
        },
        [&apply_calls](const SliceCandidate&) {
            ++apply_calls;
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [&slice_calls] {
            ++slice_calls;
            return true;
        },
        [] { return true; }, [] { return true; });
    SliceCandidate candidate = proposal("candidate", current);

    SECTION("reserved workflow id") {
        candidate.workflow_id = 0;
        const OfficialSliceResult result = gateway.prepare(candidate, current);
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_candidate_identity");
        const OfficialSliceResult committed = gateway.commit(candidate, current);
        CHECK(committed.phase == OfficialSlicePhase::Rejected);
        CHECK(committed.diagnostic_code == "invalid_candidate_identity");
    }

    SECTION("empty candidate id") {
        candidate.id.clear();
        const OfficialSliceResult result = gateway.prepare(candidate, current);
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_candidate_identity");
        const OfficialSliceResult committed = gateway.commit(candidate, current);
        CHECK(committed.phase == OfficialSlicePhase::Rejected);
        CHECK(committed.diagnostic_code == "invalid_candidate_identity");
    }

    SECTION("invalid revision") {
        candidate.base_revision = {};
        current = {};
        const OfficialSliceResult result = gateway.prepare(candidate, {});
        CHECK(result.phase == OfficialSlicePhase::Rejected);
        CHECK(result.diagnostic_code == "invalid_workspace_revision");
        const OfficialSliceResult committed = gateway.commit(candidate, {});
        CHECK(committed.phase == OfficialSlicePhase::Rejected);
        CHECK(committed.diagnostic_code == "invalid_workspace_revision");
    }

    CHECK(revision_calls == 0);
    CHECK(compatibility_calls == 0);
    CHECK(apply_calls == 0);
    CHECK(slice_calls == 0);
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

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    const OfficialSliceResult failed = gateway.commit(candidate, revision);
    CHECK(failed.phase == OfficialSlicePhase::Failed);
    CHECK(failed.diagnostic_code == "official_slice_not_started");
    REQUIRE(failed.can_undo);
    CHECK(gateway.undo_last_apply());
    CHECK(undo_calls == 1);
    CHECK_FALSE(gateway.undo_last_apply());
}

TEST_CASE("Orca official gateway consumes native undo ownership after one safe refusal",
          "[AI][SmartSlicing][Apply][UndoOwnership][TransactionBoundary]")
{
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    const bool throw_on_undo = GENERATE(false, true);
    CAPTURE(throw_on_undo);

    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [revision] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return false; }, [] { return true; },
        [&undo_calls, throw_on_undo] {
            ++undo_calls;
            if (throw_on_undo)
                throw std::runtime_error("native undo failed");
            return false;
        });
    SliceCandidate candidate = proposal("candidate", revision);

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    const OfficialSliceResult failed = gateway.commit(candidate, revision);
    REQUIRE(failed.phase == OfficialSlicePhase::Failed);
    REQUIRE(failed.can_undo);

    CHECK_FALSE(gateway.undo_last_apply());
    CHECK(undo_calls == 1);
    CHECK_FALSE(gateway.poll().can_undo);
    CHECK_FALSE(gateway.undo_last_apply());
    CHECK(undo_calls == 1);
}

TEST_CASE("Orca official gateway protects a failed apply until native recovery is resolved",
          "[AI][SmartSlicing][Apply][UndoOwnership][TransactionBoundary][RecoveryPriority]")
{
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    const bool use_commit = GENERATE(false, true);
    CAPTURE(use_commit);

    size_t apply_calls = 0;
    size_t slice_calls = 0;
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [revision] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [&apply_calls](const SliceCandidate&) {
            ++apply_calls;
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [&slice_calls] { ++slice_calls; return false; }, [] { return true; },
        [&undo_calls] { ++undo_calls; return true; });
    SliceCandidate candidate = proposal("candidate", revision);
    SliceCandidate other = proposal("other", revision);

    REQUIRE(gateway.prepare(candidate, revision).phase == OfficialSlicePhase::Prepared);
    const OfficialSliceResult failed = gateway.commit(candidate, revision);
    REQUIRE(failed.phase == OfficialSlicePhase::Failed);
    REQUIRE(failed.can_undo);

    const OfficialSliceResult blocked = use_commit ? gateway.commit(other, revision) :
                                                     gateway.prepare(other, revision);
    CHECK(blocked.phase == OfficialSlicePhase::Rejected);
    CHECK(blocked.diagnostic_code == "apply_recovery_required");
    CHECK(apply_calls == 1);
    CHECK(slice_calls == 1);
    CHECK(gateway.poll().can_undo);

    REQUIRE(gateway.undo_last_apply());
    CHECK(undo_calls == 1);
    CHECK(gateway.prepare(other, revision).phase == OfficialSlicePhase::Prepared);
}

TEST_CASE("Orca official gateway rejects unsupported repair plans before formal actions",
          "[AI][SmartSlicing][Apply][Repair][Boundary]")
{
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};
    size_t compatibility_calls = 0;
    size_t apply_calls = 0;
    size_t slice_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [revision] { return revision; },
        [&compatibility_calls](const SliceCandidate&) {
            ++compatibility_calls;
            return std::string{};
        },
        [&apply_calls](const SliceCandidate&) {
            ++apply_calls;
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [&slice_calls] { ++slice_calls; return true; }, [] { return true; }, [] { return true; });
    SliceCandidate candidate = proposal("repair", revision);
    candidate.repair = RepairPlan{{"repair_open_edges"}, false};

    const OfficialSliceResult prepared = gateway.prepare(candidate, revision);
    CHECK(prepared.phase == OfficialSlicePhase::Rejected);
    CHECK(prepared.diagnostic_code == "candidate_repair_unsupported");
    const OfficialSliceResult committed = gateway.commit(candidate, revision);
    CHECK(committed.phase == OfficialSlicePhase::Rejected);
    CHECK(committed.diagnostic_code == "candidate_repair_unsupported");
    CHECK(compatibility_calls == 0);
    CHECK(apply_calls == 0);
    CHECK(slice_calls == 0);
}

TEST_CASE("Orca official gateway never undoes a later workspace edit",
          "[AI][SmartSlicing][Apply][UndoOwnership]")
{
    WorkspaceRevision current{1, 2, 3, "revision-a"};
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&current] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&current](const SliceCandidate&) {
            current = {2, 2, 3, "revision-after-smart-apply"};
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        },
        [] { return false; }, [] { return true; }, [&undo_calls] { ++undo_calls; return true; });
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});

    REQUIRE(gateway.prepare(candidate, candidate.base_revision).phase == OfficialSlicePhase::Prepared);
    const OfficialSliceResult failed = gateway.commit(candidate, candidate.base_revision);
    REQUIRE(failed.phase == OfficialSlicePhase::Failed);
    REQUIRE(failed.can_undo);
    current = {3, 2, 3, "revision-after-user-edit"};

    CHECK_FALSE(gateway.undo_last_apply());
    CHECK(undo_calls == 0);
    CHECK_FALSE(gateway.poll().can_undo);
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
    formal_config.set("layer_height", 0.25);
    formal_config.set("layer_change_gcode", std::string("G92 E0\n"));

    const ObjectID object_id = object->id();
    const ObjectID instance_id = instance->id();
    const Transform3d original_transform = instance->get_matrix();
    const std::string original_layer_height = formal_config.opt_serialize("layer_height");
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
    candidate.parameters.intent = ParameterIntent::Quality;
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 7, "layer_height",
                                            0.25, 0.20, "improve_surface_detail"});

    const TrialSliceResult result = executor.execute_trial_slice(candidate);

    INFO("trial diagnostic: " << result.diagnostic_code);
    REQUIRE(result.status == TrialSliceStatus::Succeeded);
    REQUIRE(result.metrics);
    CHECK(result.metrics->estimated_time_seconds.value_or(0.0) > 0.0);
    CHECK(result.metrics->filament_volume_mm3.value_or(0.0) > 0.0);
    CHECK(result.metrics->bed_adhesion_risk_score.value_or(0.0) >= 1.0);
    CHECK(result.metrics->brim_volume_mm3.has_value());
    CHECK_FALSE(result.metrics->physical_slots_compatible.has_value());
    CHECK_FALSE(result.metrics->color_mapping_degraded.has_value());
    REQUIRE(formal_model.objects.size() == 1);
    CHECK(formal_model.objects.front()->id() == object_id);
    REQUIRE(formal_model.objects.front()->instances.size() == 1);
    CHECK(formal_model.objects.front()->instances.front()->id() == instance_id);
    CHECK(formal_model.objects.front()->instances.front()->get_matrix().isApprox(original_transform));
    CHECK(formal_config.opt_serialize("layer_height") == original_layer_height);
}

TEST_CASE("placement candidates cannot change existing geometry semantics",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Placement][GeometryBoundary]")
{
    Transform3d existing = Transform3d::Identity();
    existing.linear() << 2.0, 0.25, 0.0,
                         0.0, -3.0, 0.0,
                         0.0, 0.0, 4.0;
    Transform3d rigidly_moved = existing;
    rigidly_moved.linear() = Eigen::AngleAxisd(0.7, Vec3d::UnitZ()).toRotationMatrix() * existing.linear();
    rigidly_moved.translation() = Vec3d(15.0, 25.0, 5.0);
    CHECK(Slic3r::GUI::orca_placement_transform_preserves_geometry(existing, rigidly_moved));

    const auto execute_changed_transform = [](const std::function<void(Transform3d&)>& change) {
        Slic3r::GUI::OrcaTrialSliceInput input = tiny_trial_input();
        ModelObject* object = input.model.objects.front();
        ModelInstance* instance = object->instances.front();
        Transform3d matrix = instance->get_matrix();
        change(matrix);

        SliceCandidate candidate = proposal("geometry-changing", WorkspaceRevision{1, 2, 3, "revision-a"});
        candidate.status = CandidateStatus::Draft;
        candidate.metrics.reset();
        ObjectTransform transform;
        transform.object_id = object->id().id;
        transform.instance_id = instance->id().id;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
            for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                transform.matrix[static_cast<size_t>(row * matrix.cols() + column)] = matrix(row, column);
        candidate.placement.transforms.push_back(std::move(transform));

        Slic3r::GUI::OrcaTrialSliceExecutor executor(
            [input = std::move(input)]() mutable { return std::move(input); });
        return executor.execute_trial_slice(candidate);
    };

    const TrialSliceResult scaled = execute_changed_transform([](Transform3d& matrix) {
        matrix.linear().col(0) *= 2.0;
    });
    CHECK(scaled.status == TrialSliceStatus::Failed);
    CHECK(scaled.diagnostic_code == "invalid_candidate_placement");

    const TrialSliceResult mirrored = execute_changed_transform([](Transform3d& matrix) {
        matrix.linear().col(0) *= -1.0;
    });
    CHECK(mirrored.status == TrialSliceStatus::Failed);
    CHECK(mirrored.diagnostic_code == "invalid_candidate_placement");
}

TEST_CASE("placement matrices share finite affine and determinant boundaries",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Placement][MatrixBoundary]")
{
    const Transform3d valid = Transform3d::Identity();
    CHECK(Slic3r::GUI::orca_placement_transform_is_valid(valid));

    Transform3d non_finite = valid;
    non_finite.translation().x() = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(Slic3r::GUI::orca_placement_transform_is_valid(non_finite));

    Transform3d non_affine = valid;
    non_affine.matrix()(3, 0) = 0.1;
    CHECK_FALSE(Slic3r::GUI::orca_placement_transform_is_valid(non_affine));

    Transform3d collapsed = valid;
    collapsed.linear()(0, 0) = 1e-12;
    CHECK_FALSE(Slic3r::GUI::orca_placement_transform_is_valid(collapsed));
}

TEST_CASE("plate locks block placement transforms without blocking unchanged slicing",
          "[AI][SmartSlicing][Apply][Orca][Placement][PlateLock]")
{
    CHECK(Slic3r::GUI::orca_placement_respects_plate_lock(false, 1));
    CHECK(Slic3r::GUI::orca_placement_respects_plate_lock(true, 0));
    CHECK_FALSE(Slic3r::GUI::orca_placement_respects_plate_lock(true, 1));
}

TEST_CASE("Orca trial slicing rejects placement transforms on a locked plate",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Placement][PlateLock]")
{
    Slic3r::GUI::OrcaTrialSliceInput input = tiny_trial_input();
    input.plate_locked = true;
    ModelObject* object = input.model.objects.front();
    ModelInstance* instance = object->instances.front();
    Transform3d requested = instance->get_matrix();
    requested.translation().x() += 5.0;

    ObjectTransform transform;
    transform.object_id = object->id().id;
    transform.instance_id = instance->id().id;
    for (Eigen::Index row = 0; row < requested.rows(); ++row)
        for (Eigen::Index column = 0; column < requested.cols(); ++column)
            transform.matrix[static_cast<size_t>(row * requested.cols() + column)] = requested(row, column);

    SliceCandidate candidate = proposal("locked-plate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    candidate.placement.transforms.push_back(std::move(transform));
    Slic3r::GUI::OrcaTrialSliceExecutor executor(
        [input = std::move(input)]() mutable { return std::move(input); });

    const TrialSliceResult result = executor.execute_trial_slice(candidate);

    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_code == "current_plate_locked");
    CHECK_FALSE(result.metrics);
}

TEST_CASE("Orca trial input preserves physical-slot compatibility availability",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Multicolor][EvidenceAvailability]")
{
    CHECK(Slic3r::GUI::orca_physical_slots_compatible(PhysicalSlotCompatibility::NotApplicable) == true);
    CHECK(Slic3r::GUI::orca_physical_slots_compatible(PhysicalSlotCompatibility::Compatible) == true);
    CHECK(Slic3r::GUI::orca_physical_slots_compatible(PhysicalSlotCompatibility::Incompatible) == false);
    CHECK(Slic3r::GUI::orca_physical_slots_compatible(PhysicalSlotCompatibility::InvalidTemperatureRange) == false);
    CHECK_FALSE(Slic3r::GUI::orca_physical_slots_compatible(PhysicalSlotCompatibility::Unavailable).has_value());
}

TEST_CASE("Orca trial slicing rejects forbidden patches and observes early cancellation", "[AI][SmartSlicing][Workflow][OrcaTrial]")
{
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.parameters.intent = ParameterIntent::Stability;
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 0, "nozzle_diameter",
                                            0.4, 0.6, "unsafe_hardware_change"});
    Slic3r::GUI::OrcaTrialSliceExecutor rejected_executor([] {
        Slic3r::GUI::OrcaTrialSliceInput input;
        input.plate_id = 0;
        return input;
    });

    const TrialSliceResult rejected = rejected_executor.execute_trial_slice(candidate);
    CHECK(rejected.status == TrialSliceStatus::Failed);
    CHECK(rejected.diagnostic_code == "parameter_key_forbidden");

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

TEST_CASE("Orca trial slicing rejects repair plans it cannot execute",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Repair][Boundary]")
{
    Slic3r::GUI::OrcaTrialSliceExecutor executor([] { return tiny_trial_input(); });
    SliceCandidate candidate = proposal("repair", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    candidate.repair = RepairPlan{{"repair_open_edges"}, false};

    const TrialSliceResult result = executor.execute_trial_slice(candidate);
    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_code == "candidate_repair_unsupported");
    CHECK_FALSE(result.metrics);
}

TEST_CASE("Orca trial slicing rejects an input without printable objects", "[AI][SmartSlicing][Workflow][OrcaTrial]")
{
    Slic3r::GUI::OrcaTrialSliceInput input = tiny_trial_input();
    REQUIRE(input.model.objects.size() == 1);
    REQUIRE(input.model.objects.front()->instances.size() == 1);
    input.model.objects.front()->instances.front()->printable = false;

    Slic3r::GUI::OrcaTrialSliceExecutor executor([input = std::move(input)]() mutable { return std::move(input); });
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();

    const TrialSliceResult result = executor.execute_trial_slice(candidate);

    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_code == "trial_no_printable_objects");
}

TEST_CASE("Orca trial placement cannot target an unprintable object",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Placement][TargetEligibility]")
{
    Slic3r::GUI::OrcaTrialSliceInput input = tiny_trial_input();
    ModelObject* target_object = input.model.objects.front();
    ModelInstance* target_instance = target_object->instances.front();
    target_object->printable = false;
    ModelObject* printable_object = input.model.add_object();
    printable_object->add_volume(make_cube(5.0, 5.0, 5.0));
    printable_object->add_instance()->set_offset(Vec3d(70.0, 70.0, 0.0));
    printable_object->ensure_on_bed();

    Transform3d requested = target_instance->get_matrix();
    requested.translation().x() += 5.0;
    ObjectTransform transform;
    transform.object_id = target_object->id().id;
    transform.instance_id = target_instance->id().id;
    for (Eigen::Index row = 0; row < requested.rows(); ++row)
        for (Eigen::Index column = 0; column < requested.cols(); ++column)
            transform.matrix[static_cast<size_t>(row * requested.cols() + column)] = requested(row, column);

    SliceCandidate candidate = proposal("unprintable-target", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    candidate.placement.transforms.push_back(std::move(transform));
    Slic3r::GUI::OrcaTrialSliceExecutor executor(
        [input = std::move(input)]() mutable { return std::move(input); });

    const TrialSliceResult result = executor.execute_trial_slice(candidate);
    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_code == "invalid_candidate_placement");
}

TEST_CASE("Orca trial placement rejects duplicate transform targets",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Placement][DuplicateTarget]")
{
    Slic3r::GUI::OrcaTrialSliceInput input = tiny_trial_input();
    ModelObject* object = input.model.objects.front();
    ModelInstance* instance = object->instances.front();
    Transform3d requested = instance->get_matrix();
    requested.translation().x() += 5.0;
    ObjectTransform transform;
    transform.object_id = object->id().id;
    transform.instance_id = instance->id().id;
    for (Eigen::Index row = 0; row < requested.rows(); ++row)
        for (Eigen::Index column = 0; column < requested.cols(); ++column)
            transform.matrix[static_cast<size_t>(row * requested.cols() + column)] = requested(row, column);

    SliceCandidate candidate = proposal("duplicate-target", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();
    candidate.placement.transforms.push_back(transform);
    candidate.placement.transforms.push_back(std::move(transform));
    Slic3r::GUI::OrcaTrialSliceExecutor executor(
        [input = std::move(input)]() mutable { return std::move(input); });

    const TrialSliceResult result = executor.execute_trial_slice(candidate);
    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_code == "invalid_candidate_placement");
}

TEST_CASE("Orca trial executor serializes concurrent slice requests",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Concurrency]")
{
    std::mutex provider_mutex;
    std::condition_variable provider_changed;
    size_t provider_entries = 0;
    bool release_first = false;
    Slic3r::GUI::OrcaTrialSliceExecutor executor([&] {
        std::unique_lock<std::mutex> lock(provider_mutex);
        ++provider_entries;
        provider_changed.notify_all();
        if (provider_entries == 1)
            provider_changed.wait(lock, [&] { return release_first; });
        Slic3r::GUI::OrcaTrialSliceInput input;
        input.config = DynamicPrintConfig::full_print_config();
        return input;
    });
    const SliceCandidate candidate = proposal("serialized-trial", WorkspaceRevision{1, 2, 3, "revision-a"});
    TrialSliceResult first_result;
    TrialSliceResult second_result;

    std::thread first([&] { first_result = executor.execute_trial_slice(candidate); });
    bool first_entered = false;
    {
        std::unique_lock<std::mutex> lock(provider_mutex);
        first_entered = provider_changed.wait_for(
            lock, std::chrono::seconds(5), [&] { return provider_entries == 1; });
    }
    std::promise<void> second_started;
    std::future<void> second_started_future = second_started.get_future();
    std::thread second([&] {
        second_started.set_value();
        second_result = executor.execute_trial_slice(candidate);
    });
    const bool second_invoked =
        second_started_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

    bool entered_concurrently = false;
    {
        std::unique_lock<std::mutex> lock(provider_mutex);
        entered_concurrently = provider_changed.wait_for(
            lock, std::chrono::milliseconds(500), [&] { return provider_entries > 1; });
        release_first = true;
    }
    provider_changed.notify_all();
    first.join();
    second.join();

    CHECK(first_entered);
    CHECK(second_invoked);
    CHECK_FALSE(entered_concurrently);
    CHECK(provider_entries == 2);
    CHECK(first_result.status == TrialSliceStatus::Failed);
    CHECK(second_result.status == TrialSliceStatus::Failed);
}

TEST_CASE("benchmark isolated trial slicing and successful cache hits",
          "[AI][SmartSlicing][Performance][!benchmark]")
{
    if (std::getenv("ORCA_SMART_SLICING_BENCHMARK") == nullptr)
        SKIP("Set ORCA_SMART_SLICING_BENCHMARK=1 to run the release benchmark.");

    SliceCandidate candidate = proposal("benchmark", WorkspaceRevision{1, 2, 3, "benchmark-revision"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();

    BENCHMARK("isolated tiny trial slice") {
        Slic3r::GUI::OrcaTrialSliceExecutor executor([] { return tiny_trial_input(); });
        return executor.execute_trial_slice(candidate);
    };

    Slic3r::GUI::OrcaTrialSliceExecutor native_executor([] { return tiny_trial_input(); });
    CachingTrialSliceExecutor cached_executor(native_executor);
    REQUIRE(cached_executor.execute_trial_slice(candidate).status == TrialSliceStatus::Succeeded);
    BENCHMARK("successful trial cache hit") { return cached_executor.execute_trial_slice(candidate); };
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

TEST_CASE("a timed out prepared trial session can execute a later retry",
          "[AI][SmartSlicing][Workflow][OrcaTrial][Runtime][TimeoutRetry]")
{
    SliceCandidate candidate = proposal("candidate", WorkspaceRevision{1, 2, 3, "revision-a"});
    candidate.status = CandidateStatus::Draft;
    candidate.metrics.reset();

    Slic3r::GUI::OrcaTrialSliceExecutor executor([] { return tiny_trial_input(); });
    executor.prepare_session_input(tiny_trial_input());
    executor.set_resource_limits(std::chrono::seconds(0), 1024 * 1024, 1024 * 1024);
    const TrialSliceResult timed_out = executor.execute_trial_slice(candidate);
    REQUIRE(timed_out.status == TrialSliceStatus::Canceled);
    REQUIRE(timed_out.diagnostic_code == "workflow_timeout");

    executor.set_resource_limits(std::chrono::minutes(1), 1024 * 1024, 1024 * 1024);
    const TrialSliceResult retried = executor.execute_trial_slice(candidate);
    CHECK(retried.status == TrialSliceStatus::Succeeded);
    CHECK(retried.diagnostic_code.empty());
    CHECK(retried.metrics.has_value());
}
