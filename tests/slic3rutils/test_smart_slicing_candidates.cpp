#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp"
#include "slic3r/GUI/AI/Orca/OrcaCandidateProposalTask.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOrientationCandidateProvider.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlacementCandidateProvider.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Print.hpp"

#include <cmath>

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

ModelInstance* add_box(Model& model, const Vec3d& size, const Vec3d& offset)
{
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(size.x(), size.y(), size.z()));
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

TEST_CASE("native placement candidates preserve sequential print head clearance",
          "[AI][SmartSlicing][Candidate][OrcaPlacement][SequentialPrint]")
{
    GUI::OrcaPlacementCandidateInput input = placement_input(200.0);
    add_cube(input.model, 10.0, Vec3d(80.0, 80.0, 0.0));
    add_cube(input.model, 10.0, Vec3d(82.0, 82.0, 0.0));
    input.arrange_params.is_seq_print            = true;
    input.arrange_params.clearance_radius        = 30.0;
    input.arrange_params.clearance_height_to_rod = 50.0;
    input.arrange_params.clearance_height_to_lid = 50.0;
    input.arrange_params.nozzle_height            = 1.0;

    const std::vector<SliceCandidate> candidates =
        GUI::OrcaPlacementCandidateProvider().generate(std::move(input), {1, 2, 3, "revision-a"});

    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().placement.transforms.size() == 2);
    const ObjectTransform& first  = candidates.front().placement.transforms[0];
    const ObjectTransform& second = candidates.front().placement.transforms[1];
    const double center_distance = std::hypot(first.matrix[3] - second.matrix[3],
                                              first.matrix[7] - second.matrix[7]);
    CHECK(center_distance >= 40.0);
}

TEST_CASE("native orientation candidates are deterministic and keep the input model isolated",
          "[AI][SmartSlicing][Candidate][OrcaOrientation]")
{
    GUI::OrcaOrientationCandidateInput formal;
    formal.config = DynamicPrintConfig::full_print_config();
    ModelInstance* formal_instance = add_box(formal.model, Vec3d(8.0, 12.0, 40.0), Vec3d(30.0, 30.0, 0.0));
    const Transform3d formal_transform = formal_instance->get_matrix();
    GUI::OrcaOrientationCandidateInput first = formal;
    GUI::OrcaOrientationCandidateInput second = formal;
    GUI::OrcaOrientationCandidateProvider provider;
    const WorkspaceRevision revision{1, 2, 3, "revision-a"};

    const std::vector<SliceCandidate> first_result = provider.generate(std::move(first), revision);
    const std::vector<SliceCandidate> second_result = provider.generate(std::move(second), revision);

    REQUIRE(first_result.size() == 1);
    REQUIRE(second_result.size() == 1);
    CHECK(first_result.front().id == "orientation-stability-native-v1");
    CHECK(first_result.front().base_revision == revision);
    REQUIRE(first_result.front().placement.transforms.size() == 1);
    CHECK(first_result.front().placement.transforms.front().matrix ==
          second_result.front().placement.transforms.front().matrix);
    CHECK_FALSE(first_result.front().placement.transforms.front().matrix == ObjectTransform{}.matrix);
    CHECK(formal_instance->get_matrix().isApprox(formal_transform));
}

TEST_CASE("native orientation candidates protect locked and unprintable targets",
          "[AI][SmartSlicing][Candidate][OrcaOrientation]")
{
    GUI::OrcaOrientationCandidateInput input;
    input.config = DynamicPrintConfig::full_print_config();
    ModelInstance* locked = add_box(input.model, Vec3d(8.0, 12.0, 40.0), Vec3d(20.0, 20.0, 0.0));
    ModelInstance* object_locked = add_box(input.model, Vec3d(9.0, 13.0, 45.0), Vec3d(40.0, 20.0, 0.0));
    ModelInstance* movable = add_box(input.model, Vec3d(7.0, 11.0, 35.0), Vec3d(60.0, 60.0, 0.0));
    ModelInstance* unprintable = add_box(input.model, Vec3d(6.0, 10.0, 30.0), Vec3d(80.0, 20.0, 0.0));
    unprintable->printable = false;
    input.locked_instance_ids.insert(locked->id().id);
    input.locked_object_ids.insert(object_locked->get_object()->id().id);
    const uint64_t movable_id = movable->id().id;
    GUI::OrcaOrientationCandidateInput locked_plate_input = input;
    locked_plate_input.plate_locked = true;
    GUI::OrcaOrientationCandidateProvider provider;

    const std::vector<SliceCandidate> candidates = provider.generate(std::move(input), {1, 2, 3, "revision-a"});

    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().placement.transforms.size() == 1);
    CHECK(candidates.front().placement.transforms.front().instance_id == movable_id);
    CHECK(provider.generate(std::move(locked_plate_input), {1, 2, 3, "revision-a"}).empty());
}

TEST_CASE("native candidate providers honor cancellation before expensive planning",
          "[AI][SmartSlicing][Candidate][Cancellation]")
{
    GUI::OrcaPlacementCandidateInput placement = placement_input();
    add_cube(placement.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    placement.arrange_params.stopcondition = [] { return true; };
    CHECK(GUI::OrcaPlacementCandidateProvider().generate(std::move(placement), {1, 2, 3, "revision-a"}).empty());

    GUI::OrcaOrientationCandidateInput orientation;
    orientation.config = DynamicPrintConfig::full_print_config();
    add_box(orientation.model, Vec3d(8.0, 12.0, 40.0), Vec3d(30.0, 30.0, 0.0));
    orientation.stopcondition = [] { return true; };
    CHECK(GUI::OrcaOrientationCandidateProvider().generate(std::move(orientation), {1, 2, 3, "revision-a"}).empty());
}

TEST_CASE("prepared candidate proposal task discards partial work after cancellation",
          "[AI][SmartSlicing][Candidate][Cancellation][ProposalTask]")
{
    GUI::OrcaCandidateProposalInput input;
    input.context.plate_index = 0;
    input.context.revision = {1, 2, 3, "revision-a"};
    input.placement = placement_input();
    add_cube(input.placement.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    input.orientation.config = DynamicPrintConfig::full_print_config();
    add_box(input.orientation.model, Vec3d(8.0, 12.0, 40.0), Vec3d(30.0, 30.0, 0.0));

    size_t cancellation_polls = 0;
    GUI::OrcaCandidateProposalTask task(std::move(input));
    const std::vector<SliceCandidate> candidates = task.execute([&cancellation_polls] {
        return ++cancellation_polls >= 2;
    });

    CHECK(candidates.empty());
    CHECK(cancellation_polls >= 2);
}

TEST_CASE("prepared candidate proposal task composes typed advice into native alternatives",
          "[AI][SmartSlicing][Candidate][ProposalTask][Parameters]")
{
    GUI::OrcaCandidateProposalInput input;
    input.context.plate_index = 0;
    input.context.revision    = {1, 2, 3, "revision-a"};
    input.placement           = placement_input();
    add_cube(input.placement.model, 10.0, Vec3d(75.0, 75.0, 0.0));
    input.orientation.config = DynamicPrintConfig::full_print_config();
    add_box(input.orientation.model, Vec3d(8.0, 12.0, 40.0), Vec3d(30.0, 30.0, 0.0));
    input.parameters.plate_id           = 42;
    input.parameters.current_brim_width = 1.0;
    input.parameters.printable_instances.push_back({6.0, 20.0, 40.0});

    const std::vector<SliceCandidate> candidates =
        GUI::OrcaCandidateProposalTask(std::move(input)).execute();

    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].id == "placement-stability-native-v1");
    CHECK(candidates[1].id == "orientation-stability-native-v1");
    for (const SliceCandidate& candidate : candidates) {
        REQUIRE(candidate.parameters.entries.size() == 1);
        CHECK(candidate.parameters.entries.front().target_id == 42);
        CHECK(candidate.parameters.entries.front().key == "brim_width");
    }
}

TEST_CASE("prepared candidate proposal task keeps typed advice when native alternatives are unavailable",
          "[AI][SmartSlicing][Candidate][ProposalTask][Parameters]")
{
    GUI::OrcaCandidateProposalInput input;
    input.context.plate_index = 0;
    input.context.revision    = {1, 2, 3, "revision-a"};
    input.parameters.plate_id           = 42;
    input.parameters.current_brim_width = 1.0;
    input.parameters.printable_instances.push_back({6.0, 20.0, 40.0});

    const std::vector<SliceCandidate> candidates =
        GUI::OrcaCandidateProposalTask(std::move(input)).execute();

    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().id == "parameter-brim-stability-v1");
    REQUIRE(candidates.front().parameters.entries.size() == 1);
    CHECK(candidates.front().parameters.entries.front().key == "brim_width");
}
