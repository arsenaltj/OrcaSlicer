#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlacementCandidateProvider.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r;

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
    candidate.metrics->flush_volume_mm3        = 0.0;
    candidate.metrics->wipe_tower_volume_mm3   = 0.0;
    candidate.metrics->tool_changes            = 0;
    for (size_t index = 0; index < warning_count; ++index)
        candidate.metrics->warning_codes.push_back("warning_" + std::to_string(index));
    return candidate;
}

GUI::OrcaPlacementCandidateInput placement_input(double bed_size = 100.0)
{
    GUI::OrcaPlacementCandidateInput input;
    input.config = DynamicPrintConfig::full_print_config();
    input.config.set_key_value("printable_area", new ConfigOptionPoints{
        {0.0, 0.0}, {bed_size, 0.0}, {bed_size, bed_size}, {0.0, bed_size}});
    input.arrange_params.allow_rotations = false;
    input.arrange_params.accuracy        = 0.25f;
    input.arrange_params.min_obj_distance = scaled(5.0);
    return input;
}

ModelInstance* add_cube(Model& model, double size, const Vec3d& offset)
{
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(size, size, size));
    ModelInstance* instance = object->add_instance();
    instance->set_offset(offset);
    object->ensure_on_bed();
    return instance;
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

TEST_CASE("multicolor comparison includes flush wipe tower and tool-change evidence", "[AI][SmartSlicing][Candidate][Multicolor]")
{
    SliceCandidate lower_flush = ready_candidate("lower-flush", 100.0, 500.0, 10.0);
    lower_flush.metrics->flush_volume_mm3 = 40.0;
    lower_flush.metrics->wipe_tower_volume_mm3 = 20.0;
    lower_flush.metrics->tool_changes = 12;
    lower_flush.metrics->filament_change_sequence = {0, 1, 0};
    lower_flush.metrics->layer_tool_sequences = {{0}, {0, 1}};

    SliceCandidate higher_flush = ready_candidate("higher-flush", 100.0, 490.0, 10.0);
    higher_flush.metrics->flush_volume_mm3 = 80.0;
    higher_flush.metrics->wipe_tower_volume_mm3 = 30.0;
    higher_flush.metrics->tool_changes = 18;

    const CandidateComparison comparison =
        compare_candidates({higher_flush, lower_flush}, CandidateGoal::MaterialSaving);

    REQUIRE(comparison.ordered_candidate_ids.size() == 2);
    CHECK(comparison.ordered_candidate_ids.front() == "lower-flush");
    CHECK(comparison.recommendation_evidence_codes ==
          std::vector<std::string>{"less_total_material_including_multicolor_waste"});
    CHECK(lower_flush.metrics->total_material_volume_mm3() == Catch::Approx(560.0));
    CHECK(lower_flush.metrics->filament_change_sequence == std::vector<size_t>{0, 1, 0});
    CHECK(lower_flush.metrics->layer_tool_sequences.size() == 2);
}

TEST_CASE("degraded color mappings are excluded and incomplete multicolor cost stays unavailable",
          "[AI][SmartSlicing][Candidate][Multicolor]")
{
    SliceCandidate degraded = ready_candidate("degraded", 80.0, 400.0, 10.0);
    degraded.metrics->color_mapping_degraded = true;

    SliceCandidate incomplete = ready_candidate("incomplete", 90.0, 450.0, 10.0);
    incomplete.metrics->flush_volume_mm3.reset();

    const CandidateComparison comparison =
        compare_candidates({degraded, incomplete}, CandidateGoal::MaterialSaving);

    CHECK(comparison.excluded_candidate_ids == std::vector<CandidateId>{"degraded"});
    CHECK(comparison.missing_metric_candidate_ids == std::vector<CandidateId>{"incomplete"});
    CHECK_FALSE(incomplete.metrics->total_material_volume_mm3().has_value());
}

TEST_CASE("native physical-slot compatibility distinguishes compatible invalid and mixed ranges",
          "[AI][SmartSlicing][Candidate][Multicolor]")
{
    CHECK(Print::check_multi_filaments_compatibility(
              {"PLA", "PLA"}, {220, 215}, {190, 190}, {240, 240}) == FilamentCompatibilityType::Compatible);
    CHECK(Print::check_multi_filaments_compatibility(
              {"PLA", "PETG"}, {220, 260}, {190, 240}, {230, 280}) == FilamentCompatibilityType::HighLowMixed);
    CHECK(Print::check_multi_filaments_compatibility(
              {"PLA", "PLA"}, {220, 220}, {240, 190}, {230, 240}) ==
          FilamentCompatibilityType::InvalidTemperatureRange);
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

TEST_CASE("native placement candidates are deterministic and keep baseline isolated", "[AI][SmartSlicing][Candidate][OrcaPlacement]")
{
    GUI::OrcaPlacementCandidateInput formal = placement_input();
    ModelInstance* formal_instance = add_cube(formal.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    const Transform3d formal_transform = formal_instance->get_matrix();
    GUI::OrcaPlacementCandidateInput first  = formal;
    GUI::OrcaPlacementCandidateInput second = formal;
    GUI::OrcaPlacementCandidateProvider provider;
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};

    const std::vector<SliceCandidate> first_result  = provider.generate(std::move(first), revision);
    const std::vector<SliceCandidate> second_result = provider.generate(std::move(second), revision);

    REQUIRE(first_result.size() == 1);
    REQUIRE(second_result.size() == 1);
    CHECK(first_result.front().id == "placement-stability-native-v1");
    CHECK(first_result.front().base_revision == revision);
    CHECK(first_result.front().placement.transforms.size() == 1);
    CHECK(first_result.front().placement.transforms.front().matrix == second_result.front().placement.transforms.front().matrix);
    CHECK(formal_instance->get_matrix().isApprox(formal_transform));
}

TEST_CASE("native placement candidates protect locked targets and locked plates", "[AI][SmartSlicing][Candidate][OrcaPlacement]")
{
    GUI::OrcaPlacementCandidateInput input = placement_input();
    ModelInstance* locked = add_cube(input.model, 10.0, Vec3d(20.0, 20.0, 0.0));
    add_cube(input.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    const uint64_t locked_instance_id = locked->id().id;
    input.locked_instance_ids.insert(locked_instance_id);
    GUI::OrcaPlacementCandidateInput locked_plate_input = input;
    locked_plate_input.plate_locked = true;
    GUI::OrcaPlacementCandidateProvider provider;

    const std::vector<SliceCandidate> candidates = provider.generate(std::move(input), {1, 2, 3, "revision-a"});

    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().placement.transforms.size() == 1);
    CHECK(candidates.front().placement.transforms.front().instance_id != locked_instance_id);
    CHECK(provider.generate(std::move(locked_plate_input), {1, 2, 3, "revision-a"}).empty());
}

TEST_CASE("native placement candidates reject objects that cannot fit", "[AI][SmartSlicing][Candidate][OrcaPlacement]")
{
    GUI::OrcaPlacementCandidateInput input = placement_input(10.0);
    add_cube(input.model, 30.0, Vec3d(0.0, 0.0, 0.0));
    GUI::OrcaPlacementCandidateProvider provider;

    CHECK(provider.generate(std::move(input), {1, 2, 3, "revision-a"}).empty());
}

TEST_CASE("native placement candidates honor native excluded regions", "[AI][SmartSlicing][Candidate][OrcaPlacement]")
{
    GUI::OrcaPlacementCandidateInput input = placement_input();
    add_cube(input.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    arrangement::ArrangePolygon exclusion;
    exclusion.poly.contour.points = {
        Point{scaled(0.0), scaled(0.0)}, Point{scaled(100.0), scaled(0.0)},
        Point{scaled(100.0), scaled(100.0)}, Point{scaled(0.0), scaled(100.0)}};
    exclusion.bed_idx        = 0;
    exclusion.is_virt_object = true;
    input.arrange_params.excluded_regions.push_back(std::move(exclusion));
    GUI::OrcaPlacementCandidateProvider provider;

    CHECK(provider.generate(std::move(input), {1, 2, 3, "revision-a"}).empty());
}
