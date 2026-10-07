#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/SmartSlicing/Application/BrimParameterAdvisor.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

using namespace Slic3r::AI::SmartSlicing;
namespace {
class Workspace final : public IOrcaWorkspace {
public:
    WorkspaceContext context;
    Workspace()
    {
        context.revision = {1, 2, 3, "strategy-fixture"};
        context.plate_index = 0; context.native_validation_available = true;
        context.printer_preset_id = "printer"; context.process_preset_id = "process";
        context.objects.push_back({42, "cube", 1, 12, 0, false});
        context.materials.push_back({"material", "#FFFFFF"});
    }
    WorkspaceRevision current_revision() const override { return context.revision; }
    WorkspaceContext capture_context() const override { return context; }
};
class Trials final : public ITrialSliceExecutor {
public:
    size_t calls {0};
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override
    {
        ++calls;
        TrialSliceResult result;
        result.candidate_id = candidate.id; result.base_revision = candidate.base_revision;
        result.status = TrialSliceStatus::Succeeded; result.metrics = SlicingMetrics{};
        result.metrics->estimated_time_seconds = 100.0;
        return result;
    }
    void cancel_trial_slice() override {}
};
}

TEST_CASE("The baseline parameter advisor preserves brim rules using a native-free snapshot", "[SlicingStrategy]")
{
    WorkspaceContext context;
    context.parameter_plate_id = 17; context.current_brim_width = 2;
    context.printable_instance_sizes_mm = {{20, 20, 10}, {4, 6, 10}};
    BrimParameterAdvisor advisor;
    const auto proposal = advisor.advise(context);
    REQUIRE(proposal.entries.size() == 1);
    CHECK(proposal.entries.front().target_id == 17);
    CHECK(proposal.entries.front().key == "brim_width");
    CHECK(std::get<double>(proposal.entries.front().expected_value) == 2.0);
    CHECK(std::get<double>(proposal.entries.front().new_value) == 5.0);
    CHECK(context.current_brim_width == 2.0);
    context.current_brim_width = 10;
    CHECK(advisor.advise(context).entries.empty());
    context.current_brim_width = 2;
    context.printable_instance_sizes_mm = {{20, 20, 10}};
    CHECK(advisor.advise(context).entries.empty());
}

TEST_CASE("The real coordinator consumes a captured scoring strategy without changing trials or revisions", "[SlicingStrategy]")
{
    Workspace workspace;
    Trials trials;
    size_t rankings = 0;
    CandidateScoringStrategy scoring;
    scoring.algorithm_id = "recording-score"; scoring.algorithm_version = "recording-score-v2";
    scoring.compare = [&](const auto& candidates, auto) {
        ++rankings;
        CHECK(candidates.size() == 2);
        CandidateComparison result;
        result.ordered_candidate_ids = {"alternative", "baseline"};
        result.recommended_candidate_id = "alternative";
        return result;
    };
    SmartSlicingCoordinator coordinator(workspace, trials, scoring);
    scoring.compare = {}; // The coordinator keeps its captured function and version.
    coordinator.start();
    REQUIRE(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
    SliceCandidate alternative;
    alternative.id = "alternative"; alternative.base_revision = workspace.context.revision;
    REQUIRE(coordinator.plan_and_slice_candidates({alternative}));
    CHECK(rankings == 1);
    CHECK(trials.calls == 2);
    CHECK(coordinator.snapshot().selected_candidate_id == "alternative");
    CHECK(coordinator.snapshot().comparison->algorithm_version == "recording-score-v2");
    workspace.context.revision.fingerprint = "changed";
    CHECK_FALSE(coordinator.select_candidate("baseline"));
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
}

TEST_CASE("Replacement scorers cannot rank failed or physically incompatible candidates", "[SlicingStrategy]")
{
    SliceCandidate baseline, incompatible;
    baseline.id = "baseline"; baseline.status = CandidateStatus::Ready; baseline.metrics = SlicingMetrics{};
    incompatible = baseline; incompatible.id = "incompatible";
    incompatible.metrics->physical_slots_compatible = false;
    CandidateScoringStrategy scoring;
    scoring.compare = [](const auto& eligible, auto) {
        CHECK(eligible.size() == 1);
        CandidateComparison result;
        result.ordered_candidate_ids = {"incompatible"}; result.recommended_candidate_id = "incompatible";
        return result;
    };
    CHECK_THROWS(compare_candidates_with_strategy({baseline, incompatible}, CandidateGoal::Stability, scoring));
}
