#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"

#include "test_helpers.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r::Test;

namespace {

GUI::OrcaTrialSliceInput trial_input()
{
    GUI::OrcaTrialSliceInput input;
    ModelObject* object = input.model.add_object();
    object->name = "versioned trial cube";
    object->add_volume(cube(5.0));
    object->add_instance()->set_offset(Vec3d(50.0, 50.0, 0.0));
    object->ensure_on_bed();
    input.config = multifilament_config(2, {
        {"layer_height", 0.25},
        {"initial_layer_print_height", 0.25},
        {"outer_wall_filament_id", 2},
        {"inner_wall_filament_id", 1},
        {"sparse_infill_filament_id", 1},
        {"internal_solid_filament_id", 1},
        {"top_surface_filament_id", 1},
        {"bottom_surface_filament_id", 1},
        {"enable_prime_tower", 0},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    input.config.set("layer_change_gcode", std::string("G92 E0\n"));
    input.plate_index = 0;
    input.plate_id = 7;
    input.plate_name = "Versioned Trial";
    return input;
}

TrialSliceTask task_for(const GUI::OrcaTrialSliceInput& input, std::string candidate_id)
{
    TrialSliceTask task;
    task.identity = {41, 3, {1, 2, 3, "fff-versioned-trial"},
                     std::move(candidate_id), BASELINE_GOAL_ID};
    task.baseline_candidate_id = "baseline";
    task.strategy_version = "strategy-v1";
    task.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    return task;
}

std::set<std::string> trial_temp_files()
{
    std::set<std::string> result;
    boost::system::error_code error;
    const boost::filesystem::path directory = boost::filesystem::temp_directory_path(error);
    if (error)
        return result;
    for (boost::filesystem::directory_iterator item(directory, error), end; !error && item != end;
         item.increment(error)) {
        const std::string filename = item->path().filename().string();
        if (filename.rfind("orca-trial-", 0) == 0)
            result.insert(item->path().string());
    }
    return result;
}

std::vector<LayerToolSequence> native_layer_sequences(GUI::OrcaTrialSliceInput input)
{
    Print print;
    print.set_status_silent();
    print.set_plate_index(input.plate_index);
    print.set_plate_name(input.plate_name);
    print.set_extruder_filament_info(input.extruder_filament_info);
    for (ModelObject* object : input.model.objects)
        if (object != nullptr)
            print.auto_assign_extruders(object);
    print.apply(input.model, input.config);
    REQUIRE(print.validate().string.empty());
    print.process();

    const boost::filesystem::path requested =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("d5t3-native-%%%%-%%%%.gcode");
    GCodeProcessorResult gcode_result;
    const boost::filesystem::path actual = print.export_gcode(requested.string(), &gcode_result, nullptr);
    boost::system::error_code error;
    boost::filesystem::remove(actual, error);
    error.clear();
    boost::filesystem::remove(requested, error);

    std::map<size_t, std::vector<size_t>> by_layer;
    for (const GCodeProcessorResult::MoveVertex& move : gcode_result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        std::vector<size_t>& sequence = by_layer[move.layer_id];
        const size_t tool = move.extruder_id;
        if (sequence.empty() || sequence.back() != tool)
            sequence.push_back(tool);
    }
    std::vector<LayerToolSequence> result;
    for (auto& [layer_index, tool_ids] : by_layer)
        result.push_back({layer_index, std::move(tool_ids)});
    return result;
}

} // namespace

TEST_CASE("versioned Orca trial metrics come from isolated Print and G-code evidence",
          "[AI][SmartSlicing][D5T3][FFFPrint]")
{
    const std::set<std::string> before_files = trial_temp_files();
    GUI::OrcaTrialSliceInput formal = trial_input();
    REQUIRE(formal.model.objects.size() == 1);
    REQUIRE(formal.model.objects.front()->instances.size() == 1);
    const ObjectID object_id = formal.model.objects.front()->id();
    const ObjectID instance_id = formal.model.objects.front()->instances.front()->id();
    const Transform3d original_transform = formal.model.objects.front()->instances.front()->get_matrix();
    const std::string original_layer_height = formal.config.opt_serialize("layer_height");
    const std::vector<LayerToolSequence> expected_sequences = native_layer_sequences(formal);

    GUI::OrcaTrialSliceExecutor executor([&] { return formal; });
    executor.prepare_session_input(formal);
    RecommendationSessionCoordinator coordinator;
    TrialSliceTask task = task_for(formal, "baseline");
    const VersionedTrialSliceResult result =
        executor.execute_versioned_trial_slice(task, coordinator.cancellation_token());

    INFO("diagnostics: " << (result.diagnostic_codes.empty() ? "none" : result.diagnostic_codes.front()));
    REQUIRE(result.status == TrialSliceStatus::Succeeded);
    REQUIRE(result.metrics);
    CHECK(normalize_trial_metrics(*result.metrics).valid());
    CHECK(result.metrics->estimated_time_seconds.known());
    CHECK(*result.metrics->estimated_time_seconds.value > 0.0);
    CHECK(result.metrics->model_material_volume_mm3.known());
    CHECK(*result.metrics->model_material_volume_mm3.value > 0.0);
    CHECK(result.metrics->total_material_volume_mm3.known());
    REQUIRE(result.metrics->layer_tool_sequences.known());
    REQUIRE(result.metrics->layer_tool_sequences.value->size() == expected_sequences.size());
    for (size_t index = 0; index < expected_sequences.size(); ++index) {
        CHECK(result.metrics->layer_tool_sequences.value->at(index).layer_index ==
              expected_sequences[index].layer_index);
        CHECK(result.metrics->layer_tool_sequences.value->at(index).tool_ids ==
              expected_sequences[index].tool_ids);
    }
    CHECK(std::any_of(expected_sequences.begin(), expected_sequences.end(), [](const auto& layer) {
        return std::find(layer.tool_ids.begin(), layer.tool_ids.end(), 0) != layer.tool_ids.end() &&
               std::find(layer.tool_ids.begin(), layer.tool_ids.end(), 1) != layer.tool_ids.end();
    }));
    CHECK(result.metrics->materials_compatible.availability == MetricAvailability::Unknown);
    REQUIRE(result.metrics->object_transforms.size() == 1);
    CHECK(result.metrics->object_transforms.front().object_id == object_id.id);
    CHECK(result.metrics->object_transforms.front().instance_id == instance_id.id);
    CHECK(formal.model.objects.front()->instances.front()->get_matrix().isApprox(original_transform));
    CHECK(formal.config.opt_serialize("layer_height") == original_layer_height);
    CHECK(trial_temp_files() == before_files);
}

TEST_CASE("versioned Orca trial cleans temporary G-code after failure and cancellation",
          "[AI][SmartSlicing][D5T3][FFFPrint]")
{
    const std::set<std::string> before_files = trial_temp_files();
    const GUI::OrcaTrialSliceInput input = trial_input();
    RecommendationSessionCoordinator coordinator;

    GUI::OrcaTrialSliceExecutor disk_limited([&] { return input; });
    disk_limited.prepare_session_input(input);
    disk_limited.set_resource_limits(std::chrono::minutes(2), 1024ull * 1024ull * 1024ull, 0);
    const VersionedTrialSliceResult failed = disk_limited.execute_versioned_trial_slice(
        task_for(input, "disk-failure"), coordinator.cancellation_token());
    CHECK(failed.status == TrialSliceStatus::Failed);
    CHECK(failed.diagnostic_codes == std::vector<std::string>{"workflow_disk_budget_exceeded"});
    CHECK(trial_temp_files() == before_files);

    GUI::OrcaTrialSliceExecutor timed_out([&] { return input; });
    timed_out.prepare_session_input(input);
    timed_out.set_resource_limits(std::chrono::seconds(0), 1024ull * 1024ull * 1024ull,
                                  1024ull * 1024ull * 1024ull);
    const VersionedTrialSliceResult canceled = timed_out.execute_versioned_trial_slice(
        task_for(input, "deadline-cancel"), coordinator.cancellation_token());
    CHECK(canceled.status == TrialSliceStatus::Canceled);
    CHECK(canceled.diagnostic_codes == std::vector<std::string>{"workflow_timeout"});
    CHECK(trial_temp_files() == before_files);
}

TEST_CASE("versioned Orca trial requires captured Profile bounds and placement intent evidence",
          "[AI][SmartSlicing][D5T3][FFFPrint]")
{
    GUI::OrcaTrialSliceInput bounded = trial_input();
    bounded.profile_bounds.push_back({
        ConfigScope::Plate, PresetOwner::Process, 7, "layer_height",
        BoundEvidenceSource::ProcessProfile, true, 0.12, 0.28, {}, "fixture-process/v1"});
    TrialSliceTask bounded_task = task_for(bounded, "bounded-layer-height");
    bounded.evidence_revision = bounded_task.identity.workspace_revision;
    bounded.materials_compatible = MetricValue<bool>::known(true, {"fixture_material_registry"});
    bounded.physical_slots_compatible = MetricValue<bool>::known(true, {"fixture_slot_binding"});
    bounded.color_mapping_degraded = MetricValue<bool>::known(false, {"fixture_color_mapping"});
    bounded_task.parameters.entries.push_back({
        ConfigScope::Plate, PresetOwner::Process, 7, "layer_height",
        0.25, 0.20, "improve_surface_quality"});
    RecommendationSessionCoordinator coordinator;

    GUI::OrcaTrialSliceExecutor accepted([&] { return bounded; });
    accepted.prepare_session_input(bounded);
    const VersionedTrialSliceResult accepted_result = accepted.execute_versioned_trial_slice(
        bounded_task, coordinator.cancellation_token());
    INFO("accepted diagnostic: " << (accepted_result.diagnostic_codes.empty() ?
                                      "none" : accepted_result.diagnostic_codes.front()));
    REQUIRE(accepted_result.status == TrialSliceStatus::Succeeded);
    REQUIRE(accepted_result.metrics);
    REQUIRE(accepted_result.metrics->effective_parameters.size() == 1);
    CHECK(accepted_result.metrics->effective_parameters.front().key == "layer_height");
    CHECK(accepted_result.metrics->effective_parameters.front().effective_value == "0.2");
    REQUIRE(accepted_result.metrics->materials_compatible.known());
    CHECK(*accepted_result.metrics->materials_compatible.value);

    GUI::OrcaTrialSliceInput stale_evidence = bounded;
    stale_evidence.evidence_revision.fingerprint = "different-revision";
    TrialSliceTask stale_task = task_for(stale_evidence, "stale-evidence");
    stale_task.parameters = bounded_task.parameters;
    GUI::OrcaTrialSliceExecutor stale_executor([&] { return stale_evidence; });
    stale_executor.prepare_session_input(stale_evidence);
    const VersionedTrialSliceResult stale_result = stale_executor.execute_versioned_trial_slice(
        stale_task, coordinator.cancellation_token());
    REQUIRE(stale_result.status == TrialSliceStatus::Succeeded);
    REQUIRE(stale_result.metrics);
    CHECK(stale_result.metrics->materials_compatible.availability == MetricAvailability::Unknown);

    GUI::OrcaTrialSliceInput missing = bounded;
    missing.profile_bounds.clear();
    GUI::OrcaTrialSliceExecutor missing_executor([&] { return missing; });
    missing_executor.prepare_session_input(missing);
    const VersionedTrialSliceResult missing_result = missing_executor.execute_versioned_trial_slice(
        bounded_task, coordinator.cancellation_token());
    CHECK(missing_result.status == TrialSliceStatus::Failed);
    CHECK(missing_result.diagnostic_codes ==
          std::vector<std::string>{"parameter_effective_bounds_unavailable"});

    GUI::OrcaTrialSliceInput wrong = bounded;
    wrong.profile_bounds.front().minimum = 0.22;
    GUI::OrcaTrialSliceExecutor wrong_executor([&] { return wrong; });
    wrong_executor.prepare_session_input(wrong);
    const VersionedTrialSliceResult wrong_result = wrong_executor.execute_versioned_trial_slice(
        bounded_task, coordinator.cancellation_token());
    CHECK(wrong_result.status == TrialSliceStatus::Failed);
    CHECK(wrong_result.diagnostic_codes == std::vector<std::string>{"parameter_range_violation"});

    GUI::OrcaTrialSliceInput locked = trial_input();
    const ModelObject* object = locked.model.objects.front();
    const ModelInstance* instance = object->instances.front();
    locked.intent_constraints.records.push_back({
        IntentConstraintType::PlatePlacementLock,
        IntentConstraintSource::OrcaPartPlate,
        IntentConstraintState::Inactive});
    IntentConstraintRecord instance_lock;
    instance_lock.type = IntentConstraintType::InstancePlacementLock;
    instance_lock.source = IntentConstraintSource::OrcaPartPlate;
    instance_lock.state = IntentConstraintState::Active;
    instance_lock.object_id = object->id().id;
    instance_lock.instance_id = instance->id().id;
    locked.intent_constraints.records.push_back(instance_lock);
    TrialSliceTask locked_task = task_for(locked, "locked-placement");
    ObjectTransform transform;
    transform.object_id = object->id().id;
    transform.instance_id = instance->id().id;
    Transform3d moved = instance->get_matrix();
    moved.translation().x() += 10.0;
    for (Eigen::Index row = 0; row < moved.rows(); ++row)
        for (Eigen::Index column = 0; column < moved.cols(); ++column)
            transform.matrix[static_cast<size_t>(row * moved.cols() + column)] = moved(row, column);
    locked_task.placement.transforms.push_back(transform);

    GUI::OrcaTrialSliceExecutor locked_executor([&] { return locked; });
    locked_executor.prepare_session_input(locked);
    const VersionedTrialSliceResult locked_result = locked_executor.execute_versioned_trial_slice(
        locked_task, coordinator.cancellation_token());
    CHECK(locked_result.status == TrialSliceStatus::Failed);
    CHECK(locked_result.diagnostic_codes == std::vector<std::string>{"placement_intent_conflict"});
    CHECK_FALSE(locked_result.evaluation_facts.manual_intent_preserved.known());
}

TEST_CASE("versioned Orca trial never falls back to the GUI input provider",
          "[AI][SmartSlicing][D5T3][FFFPrint]")
{
    const GUI::OrcaTrialSliceInput input = trial_input();
    size_t provider_calls = 0;
    GUI::OrcaTrialSliceExecutor executor([&] {
        ++provider_calls;
        return input;
    });
    executor.prepare_session_input(input);
    executor.clear_session_input();
    RecommendationSessionCoordinator coordinator;

    const VersionedTrialSliceResult result = executor.execute_versioned_trial_slice(
        task_for(input, "cleared-session"), coordinator.cancellation_token());

    CHECK(result.status == TrialSliceStatus::Failed);
    CHECK(result.diagnostic_codes ==
          std::vector<std::string>{"versioned_trial_session_input_unavailable"});
    CHECK(provider_calls == 0);
}

TEST_CASE("Orca layer filament sets are not reported as an ordered tool sequence",
          "[AI][SmartSlicing][D5T3][FFFPrint]")
{
    GCodeProcessorResult result;
    result.layer_filaments[std::vector<unsigned int>{1, 0}] = {{0, 3}};

    const MetricValue<std::vector<LayerToolSequence>> sequences =
        GUI::extract_orca_layer_tool_sequences(result);

    CHECK(sequences.availability == MetricAvailability::Unknown);
    CHECK_FALSE(sequences.value.has_value());
    CHECK(sequences.evidence_codes ==
          std::vector<std::string>{"orca_gcode_extrusion_moves_unavailable"});
}
