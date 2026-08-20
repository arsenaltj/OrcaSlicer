#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp"

using namespace Slic3r::AI::SmartSlicing;

namespace {

SliceCandidate ready_candidate(std::string id,
                               double time_seconds,
                               double material_mm3,
                               double support_mm3,
                               size_t warning_count = 0)
{
    SliceCandidate candidate;
    candidate.id               = std::move(id);
    candidate.base_revision    = {1, 2, 3, "revision-a"};
    candidate.status           = CandidateStatus::Ready;
    candidate.metrics          = SlicingMetrics{};
    candidate.metrics->estimated_time_seconds = time_seconds;
    candidate.metrics->filament_volume_mm3     = material_mm3;
    candidate.metrics->support_volume_mm3      = support_mm3;
    for (size_t index = 0; index < warning_count; ++index)
        candidate.metrics->warning_codes.push_back("warning_" + std::to_string(index));
    return candidate;
}

} // namespace

TEST_CASE("candidate comparison uses goal evidence and stable candidate ids", "[AI][SmartSlicing][Candidate]")
{
    std::vector<SliceCandidate> candidates;
    candidates.push_back(ready_candidate("quality", 120.0, 900.0, 10.0));
    candidates.push_back(ready_candidate("speed", 60.0, 950.0, 20.0));
    candidates.push_back(ready_candidate("material", 100.0, 700.0, 15.0));

    const CandidateComparison speed = compare_candidates(candidates, CandidateGoal::Speed);
    REQUIRE(speed.ordered_candidate_ids.size() == 3);
    CHECK(speed.ordered_candidate_ids.front() == "speed");
    CHECK(speed.recommended_candidate_id == "speed");

    const CandidateComparison material = compare_candidates(candidates, CandidateGoal::MaterialSaving);
    CHECK(material.ordered_candidate_ids.front() == "material");

    const CandidateComparison tied =
        compare_candidates({ready_candidate("b", 60.0, 950.0, 20.0), ready_candidate("a", 60.0, 950.0, 20.0)},
                           CandidateGoal::Speed);
    CHECK(tied.ordered_candidate_ids.front() == "a");
}

TEST_CASE("candidate comparison excludes unusable results and caps cognitive load", "[AI][SmartSlicing][Candidate]")
{
    std::vector<SliceCandidate> candidates;
    candidates.push_back(ready_candidate("d", 40.0, 400.0, 4.0));
    candidates.push_back(ready_candidate("c", 30.0, 300.0, 3.0));
    candidates.push_back(ready_candidate("b", 20.0, 200.0, 2.0));
    candidates.push_back(ready_candidate("a", 10.0, 100.0, 1.0));
    SliceCandidate failed = ready_candidate("failed", 1.0, 1.0, 1.0);
    failed.status          = CandidateStatus::Failed;
    candidates.push_back(std::move(failed));

    const CandidateComparison comparison = compare_candidates(candidates, CandidateGoal::Speed);

    REQUIRE(comparison.ordered_candidate_ids.size() == 3);
    CHECK(comparison.ordered_candidate_ids == std::vector<CandidateId>{"a", "b", "c"});
    CHECK(comparison.excluded_candidate_ids == std::vector<CandidateId>{"failed"});
}

TEST_CASE("candidate comparison keeps unavailable metrics explicit", "[AI][SmartSlicing][Candidate]")
{
    SliceCandidate unavailable;
    unavailable.id            = "unknown";
    unavailable.base_revision = {1, 2, 3, "revision-a"};
    unavailable.status        = CandidateStatus::Ready;
    unavailable.metrics       = SlicingMetrics{};

    const CandidateComparison comparison =
        compare_candidates({unavailable, ready_candidate("measured", 100.0, 500.0, 10.0)}, CandidateGoal::Speed);

    REQUIRE(comparison.ordered_candidate_ids.size() == 2);
    CHECK(comparison.ordered_candidate_ids.front() == "measured");
    CHECK(comparison.missing_metric_candidate_ids == std::vector<CandidateId>{"unknown"});
}

TEST_CASE("stability comparison treats warnings as hard evidence before support cost", "[AI][SmartSlicing][Candidate]")
{
    const SliceCandidate clean = ready_candidate("clean", 120.0, 600.0, 50.0, 0);
    const SliceCandidate risky = ready_candidate("risky", 90.0, 500.0, 1.0, 1);

    const CandidateComparison comparison = compare_candidates({risky, clean}, CandidateGoal::Stability);

    REQUIRE(comparison.ordered_candidate_ids.size() == 2);
    CHECK(comparison.ordered_candidate_ids.front() == "clean");
    CHECK(comparison.recommendation_evidence_codes == std::vector<std::string>{"fewer_slice_warnings"});
}
