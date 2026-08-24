#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkflowRuntimeStore.hpp"

#include <boost/filesystem.hpp>

#include <fstream>

using namespace Slic3r::AI::SmartSlicing;

namespace {

WorkspaceContext runtime_context(std::string fingerprint = "revision-a")
{
    WorkspaceContext context;
    context.revision = {1, 2, 3, std::move(fingerprint)};
    context.plate_index = 0;
    context.printer_preset_id = "printer";
    context.process_preset_id = "process";
    context.materials.push_back({"material", "#FFFFFF"});
    context.objects.push_back({42, "cube", 1, 12, 0, false});
    context.native_validation_available = true;
    return context;
}

class RuntimeWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceContext context = runtime_context();
    bool throw_on_revision{false};
    WorkspaceRevision current_revision() const override
    {
        if (throw_on_revision)
            throw std::runtime_error("GUI-thread-only revision access");
        return context.revision;
    }
    WorkspaceContext capture_context() const override { return context; }
};

class RuntimeExecutor final : public ITrialSliceExecutor
{
public:
    std::vector<CandidateId> calls;
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        calls.push_back(candidate.id);
        TrialSliceResult result;
        result.candidate_id = candidate.id;
        result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded;
        result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 1.0;
        result.metrics->filament_volume_mm3 = 1.0;
        result.metrics->support_volume_mm3 = 0.0;
        result.metrics->flush_volume_mm3 = 0.0;
        result.metrics->wipe_tower_volume_mm3 = 0.0;
        result.metrics->tool_changes = 0;
        return result;
    }
    void cancel_trial_slice() override {}
};

class MemoryRuntimeStore final : public IWorkflowRuntimeStore
{
public:
    std::optional<WorkflowRuntimeRecord> record;
    size_t saves{0};
    size_t clears{0};

    std::optional<WorkflowRuntimeRecord> load() override { return record; }
    void save(const WorkflowRuntimeRecord& value) override
    {
        record = value;
        ++saves;
    }
    void clear(WorkflowId) override
    {
        record.reset();
        ++clears;
    }
};

class ScopedRuntimeDirectory
{
public:
    explicit ScopedRuntimeDirectory(boost::filesystem::path path) : m_path(std::move(path)) {}
    ~ScopedRuntimeDirectory()
    {
        boost::system::error_code error;
        boost::filesystem::remove_all(m_path, error);
    }

    const boost::filesystem::path& path() const { return m_path; }

private:
    boost::filesystem::path m_path;
};

SliceCandidate runtime_candidate(std::string id, const WorkspaceRevision& revision)
{
    SliceCandidate candidate;
    candidate.id = std::move(id);
    candidate.base_revision = revision;
    return candidate;
}

} // namespace

TEST_CASE("runtime journal stores descriptors only and clears terminal workflows", "[AI][SmartSlicing][Runtime]")
{
    RuntimeWorkspace workspace;
    RuntimeExecutor executor;
    MemoryRuntimeStore store;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.set_runtime_store(store, false);

    coordinator.start();
    REQUIRE(store.record);
    CHECK(store.record->revision == workspace.context.revision);
    CHECK(store.record->candidates.empty());

    REQUIRE(coordinator.plan_and_slice_candidates({runtime_candidate("alternative", workspace.context.revision)}));
    REQUIRE(store.record);
    REQUIRE(store.record->candidates.size() == 2);
    CHECK(store.record->candidates[0].id == "baseline");
    CHECK(store.record->candidates[1].id == "alternative");
    CHECK(executor.calls == std::vector<CandidateId>{"baseline", "alternative"});

    coordinator.cancel();
    CHECK_FALSE(store.record.has_value());
    CHECK(store.clears > 0);
}

TEST_CASE("runtime recovery keeps a matching summary and discards stale journals", "[AI][SmartSlicing][Runtime]")
{
    RuntimeWorkspace workspace;
    MemoryRuntimeStore matching;
    matching.record = WorkflowRuntimeRecord{7, WorkflowState::TrialSlicingCandidates, workspace.context.revision,
                                             {{"baseline", CandidateGoal::Stability, CandidateStatus::Ready}},
                                             "trial_slicing_candidate", 1};
    SmartSlicingCoordinator recovered(workspace);
    CHECK(recovered.set_runtime_store(matching));
    CHECK(recovered.snapshot().state == WorkflowState::Failed);
    CHECK(recovered.snapshot().detail == "interrupted_workflow_recovered");
    CHECK_FALSE(matching.record.has_value());

    MemoryRuntimeStore stale;
    stale.record = WorkflowRuntimeRecord{8, WorkflowState::PlanningCandidates, {9, 9, 9, "old"}, {}, "planning", 1};
    SmartSlicingCoordinator discarded(workspace);
    CHECK_FALSE(discarded.set_runtime_store(stale));
    CHECK(discarded.snapshot().state == WorkflowState::Idle);
    CHECK_FALSE(stale.record.has_value());
}

TEST_CASE("resource budgets expose candidate timeout memory and disk violations", "[AI][SmartSlicing][Runtime]")
{
    WorkflowResourceBudget budget;
    CHECK(workflow_budget_violation(budget, 4, std::chrono::seconds(0), {}) == "candidate_budget_exceeded");
    CHECK(workflow_budget_violation(budget, 3, std::chrono::minutes(31), {}) == "workflow_timeout");
    CHECK(workflow_budget_violation(budget, 3, std::chrono::seconds(0),
                                    {budget.maximum_memory_bytes + 1, 0}) == "workflow_memory_budget_exceeded");
    CHECK(workflow_budget_violation(budget, 3, std::chrono::seconds(0),
                                    {0, budget.maximum_temporary_disk_bytes + 1}) ==
          "workflow_disk_budget_exceeded");

    RuntimeWorkspace workspace;
    RuntimeExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    budget.maximum_candidates = 2;
    coordinator.set_resource_budget(budget);
    coordinator.start();
    CHECK_FALSE(coordinator.plan_and_slice_candidates({runtime_candidate("a", workspace.context.revision),
                                                       runtime_candidate("b", workspace.context.revision)}));
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "candidate_budget_exceeded");
    CHECK(executor.calls.empty());
}

TEST_CASE("background trials can defer GUI revision reads while keeping final apply guards", "[AI][SmartSlicing][Runtime]")
{
    RuntimeWorkspace workspace;
    RuntimeExecutor executor;
    SmartSlicingCoordinator coordinator(workspace, executor);
    coordinator.start();
    workspace.throw_on_revision = true;

    CHECK(coordinator.plan_and_slice_candidates({}, CandidateGoal::Stability, true));
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyToApply);
    CHECK(executor.calls == std::vector<CandidateId>{"baseline"});
}

TEST_CASE("Orca runtime journal paths isolate data directories and executable instances",
          "[AI][SmartSlicing][Runtime][Orca][Isolation]")
{
    const boost::filesystem::path first_data_dir("first-data-dir");
    const boost::filesystem::path second_data_dir("second-data-dir");
    const boost::filesystem::path first =
        Slic3r::GUI::orca_workflow_runtime_journal_path(first_data_dir, "executable-instance-a");
    const boost::filesystem::path second_instance =
        Slic3r::GUI::orca_workflow_runtime_journal_path(first_data_dir, "executable-instance-b");
    const boost::filesystem::path second_data =
        Slic3r::GUI::orca_workflow_runtime_journal_path(second_data_dir, "executable-instance-a");

    CHECK(first.parent_path() == first_data_dir / "cache");
    CHECK(second_data.parent_path() == second_data_dir / "cache");
    CHECK(first != second_instance);
    CHECK(first != second_data);
    CHECK(first.extension() == ".json");
    CHECK(first.filename().string().find("executable-instance-a") == std::string::npos);
}

TEST_CASE("Orca runtime store round trips bounded metadata without workspace payloads", "[AI][SmartSlicing][Runtime][Orca]")
{
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("orca-smart-runtime-%%%%-%%%%.json");
    Slic3r::GUI::OrcaWorkflowRuntimeStore store(path);
    WorkflowRuntimeRecord record{11, WorkflowState::TrialSlicingBaseline, {1, 2, 3, "revision"},
                                 {{"baseline", CandidateGoal::Stability, CandidateStatus::TrialSlicing}},
                                 "trial_slicing_baseline", 123};
    store.save(record);
    const std::optional<WorkflowRuntimeRecord> loaded = store.load();
    REQUIRE(loaded);
    CHECK(loaded->workflow_id == 11);
    CHECK(loaded->revision == record.revision);
    CHECK(loaded->candidates.size() == 1);

    std::ifstream stream(path.string(), std::ios::binary);
    const std::string serialized((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(serialized.find("gcode") == std::string::npos);
    CHECK(serialized.find("mesh") == std::string::npos);
    CHECK(serialized.find("credential") == std::string::npos);
    stream.close();
    store.clear(11);
    CHECK_FALSE(boost::filesystem::exists(path));
}

TEST_CASE("Orca runtime store recovers an interrupted journal publication and clears every generation",
          "[AI][SmartSlicing][Runtime][Orca][Recovery]")
{
    ScopedRuntimeDirectory directory(
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-smart-runtime-recovery-%%%%-%%%%"));
    const boost::filesystem::path journal_path = directory.path() / "workflow.json";
    boost::filesystem::path backup_path = journal_path;
    backup_path += ".bak";
    boost::filesystem::path temporary_path = journal_path;
    temporary_path += ".tmp";
    Slic3r::GUI::OrcaWorkflowRuntimeStore store(journal_path);
    const WorkflowRuntimeRecord record{13, WorkflowState::TrialSlicingCandidates, {7, 8, 9, "recovery-revision"},
                                       {{"candidate", CandidateGoal::Stability, CandidateStatus::TrialSlicing}},
                                       "trial_slicing_candidate", 789};

    store.save(record);
    boost::filesystem::rename(journal_path, backup_path);
    {
        std::ofstream interrupted_temporary(temporary_path.string(), std::ios::binary | std::ios::trunc);
        interrupted_temporary << "partial";
    }

    const std::optional<WorkflowRuntimeRecord> recovered = store.load();
    CHECK(recovered.has_value());
    if (recovered)
        CHECK(recovered->workflow_id == record.workflow_id);

    WorkflowRuntimeRecord resumed = record;
    resumed.detail = "resumed";
    resumed.updated_at_epoch_seconds = 790;
    store.save(resumed);
    WorkflowRuntimeRecord latest = resumed;
    latest.detail = "latest";
    latest.updated_at_epoch_seconds = 791;
    store.save(latest);
    const std::optional<WorkflowRuntimeRecord> republished = store.load();
    REQUIRE(republished);
    CHECK(republished->detail == "latest");
    CHECK(boost::filesystem::exists(journal_path));
    CHECK_FALSE(boost::filesystem::exists(backup_path));
    CHECK_FALSE(boost::filesystem::exists(temporary_path));

    store.clear(record.workflow_id);
    CHECK_FALSE(boost::filesystem::exists(journal_path));
    CHECK_FALSE(boost::filesystem::exists(backup_path));
    CHECK_FALSE(boost::filesystem::exists(temporary_path));
}

TEST_CASE("Orca runtime store supports Unicode data directories", "[AI][SmartSlicing][Runtime][Orca][Portability]")
{
#if defined(_WIN32)
    const boost::filesystem::path unicode_component(L"\u8def\u5f84-\u0416");
#else
    const boost::filesystem::path unicode_component(u8"路径-Ж");
#endif
    ScopedRuntimeDirectory directory(
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-smart-runtime-%%%%-%%%%") /
        unicode_component);
    const boost::filesystem::path journal_path = directory.path() / "workflow.json";
    Slic3r::GUI::OrcaWorkflowRuntimeStore store(journal_path);
    const WorkflowRuntimeRecord record{12, WorkflowState::PlanningCandidates, {4, 5, 6, "unicode-revision"},
                                       {{"baseline", CandidateGoal::Stability, CandidateStatus::Draft}},
                                       "planning_candidates", 456};

    store.save(record);
    const std::optional<WorkflowRuntimeRecord> loaded = store.load();

    REQUIRE(loaded);
    CHECK(loaded->workflow_id == record.workflow_id);
    CHECK(loaded->revision == record.revision);
    CHECK(loaded->detail == record.detail);
    store.clear(record.workflow_id);
    CHECK_FALSE(boost::filesystem::exists(journal_path));
}
