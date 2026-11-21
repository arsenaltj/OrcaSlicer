#include "slic3r/GUI/AI/Orca/SmartSliceApplier.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;
using Slic3r::GUI::SmartSliceApplier;

TEST_CASE("smart slice applier maps recommendation fields to native config", "[SmartSlice][SmartSliceApplier]")
{
    DynamicPrintConfig base;
    base.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    base.set_key_value("wall_loops", new ConfigOptionInt(2));
    base.set_key_value("sparse_infill_density", new ConfigOptionPercent(10.0));
    base.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    base.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.4, false));
    base.set_key_value("outer_wall_speed", new ConfigOptionFloatsNullable(1, 40.0));
    base.set_key_value("enable_support", new ConfigOptionBool(false));
    base.set_key_value("support_threshold_angle", new ConfigOptionInt(45));
    base.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    base.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));

    SliceRecommendation plan;
    plan.layer_height_mm = 0.12;
    plan.wall_loops = 4;
    plan.sparse_infill_density_percent = 18.0;
    plan.sparse_infill_pattern = "gyroid";
    plan.outer_wall_speed_mm_s = 25.0;
    plan.line_width_mm = 0.42;
    plan.enable_support = true;
    plan.support_type = "tree(auto)";
    plan.support_threshold_angle_deg = 46.0;
    plan.arachne_wall_generator = true;

    DynamicPrintConfig output;
    std::string diagnostic;
    REQUIRE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic.empty());
    CHECK(std::abs(output.opt_float("layer_height") - 0.12) < 1e-6);
    CHECK(output.opt_int("wall_loops") == 4);
    CHECK(std::abs(output.opt<ConfigOptionPercent>("sparse_infill_density")->value - 18.0) < 1e-6);
    CHECK(output.opt_enum<InfillPattern>("sparse_infill_pattern") == ipGyroid);
    CHECK(std::abs(output.opt_float_nullable("outer_wall_speed", 0) - 25.0) < 1e-6);
    CHECK(output.opt_bool("enable_support"));
    CHECK(output.opt_enum<SupportType>("support_type") == stTreeAuto);
    CHECK(output.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne);
}

TEST_CASE("smart slice applier rejects unsafe plans before mutation", "[SmartSlice][SmartSliceApplier]")
{
    DynamicPrintConfig base;
    base.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    SliceRecommendation plan;
    plan.layer_height_mm = 2.0;
    std::string diagnostic;
    DynamicPrintConfig output;
    CHECK_FALSE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic == "smart_slice_plan_out_of_range");

    base.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    plan.layer_height_mm = 0.2;
    plan.line_width_mm = 0.8;
    diagnostic.clear();
    CHECK_FALSE(SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
    CHECK(diagnostic == "smart_slice_plan_physical_bounds");
}

TEST_CASE("Smart slice preserves native wall width when the support nozzle is narrower", "[SmartSlice][SmartSliceApplier]")
{
    auto input = DynamicPrintConfig::full_print_config();
    input.set_deserialize_strict("nozzle_diameter", "0.4,0.25");
    input.set_deserialize_strict("outer_wall_line_width", "0.42");
    SliceRecommendation plan;
    plan.layer_height_mm = 0.14;
    plan.line_width_mm = 0.26;
    plan.enable_support = true;
    plan.support_type = "normal(auto)";
    DynamicPrintConfig output;
    std::string diagnostic;
    const bool accepted = GUI::SmartSliceApplier::BuildConfig(input, plan, output, &diagnostic);
    INFO(diagnostic);
    REQUIRE(accepted);
    CHECK(output.option("outer_wall_line_width")->serialize() == "0.42");
    CHECK_THAT(output.opt_float("layer_height"), Catch::Matchers::WithinAbs(plan.layer_height_mm, 1e-8));
    CHECK(output.opt_bool("enable_support"));

    input.set_deserialize_strict("nozzle_diameter", "0.4,0.4");
    CHECK_FALSE(GUI::SmartSliceApplier::BuildConfig(input, plan, output, &diagnostic));
    CHECK(diagnostic == "smart_slice_plan_physical_bounds");
}

TEST_CASE("SmartSlice uses the filament ID density and validates unknown IDs", "[SmartSlice][SmartSliceNativeTrial]")
{
    GCodeProcessorResult result;
    result.filament_densities = {1.20f, 1.25f};
    auto sum = result.material_weight_grams({{0, 1000.}, {1, 2000.}});
    REQUIRE(sum);
    CHECK(std::abs(*sum - 3.70) < 1e-6);
    std::swap(result.filament_densities[0], result.filament_densities[1]);
    sum = result.material_weight_grams({{1, 1000.}, {0, 2000.}});
    REQUIRE(sum);
    CHECK(std::abs(*sum - 3.70) < 1e-6);
    CHECK_FALSE(result.material_weight_grams({{3, 1000.}}));
    CHECK_FALSE(result.material_weight_grams({{1, -10.}}));
}

static GUI::OrcaTrialSliceInput smart_input(Model model)
{
    GUI::OrcaTrialSliceInput input;
    input.model = std::move(model);
    input.config = DynamicPrintConfig::full_print_config();
    input.config.set_deserialize_strict("printable_area", "0x0,300x0,300x270,0x270");
    input.config.set_deserialize_strict("printable_height", "260");
    input.config.set_deserialize_strict("enable_prime_tower", "0");
    input.config.set_deserialize_strict("initial_layer_print_height", "0.2");
    input.config.set_deserialize_strict("wall_loops", "3");
    input.config.set_deserialize_strict("sparse_infill_density", "15%");
    input.config.set_deserialize_strict("enable_support", "1");
    input.config.set_deserialize_strict("support_type", "normal(auto)");
    input.config.set_deserialize_strict("support_threshold_angle", "38");
    input.config.set_deserialize_strict("nozzle_diameter", "0.4");
    input.config.set_deserialize_strict("min_layer_height", "0.08");
    input.config.set_deserialize_strict("max_layer_height", "0.28");
    input.config.set_deserialize_strict("filament_density", "1.24");
    input.config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    input.plate_name = "SmartSlice acceptance";
    input.plate_id = 1;
    input.model.add_default_instances();
    input.model.center_instances_around_point(Vec2d(150.,135.));
    for (auto* object : input.model.objects) object->ensure_on_bed();
    input.model.update_print_volume_state(BuildVolume(
        input.config.option<ConfigOptionPoints>("printable_area")->values, 260., {}, {}));
    return input;
}

TEST_CASE("SmartSlice corrects an automatic tower during preflight without spending a full retry", "[SmartSlice][SmartSliceNativeTrial]")
{
    Model model;
    auto* first = model.add_object("First filament", "", make_cube(12., 12., 8.));
    first->add_instance();
    auto* second = model.add_object("Second filament", "", make_cube(12., 12., 8.));
    second->add_instance()->set_offset(Vec3d(28., 0., 0.));
    second->config.set_key_value("extruder", new ConfigOptionInt(2));
    auto input = smart_input(std::move(model));
    input.config.set_deserialize_strict({
        {"enable_prime_tower", "1"}, {"wipe_tower_x", "165"}, {"wipe_tower_y", "285"},
        {"nozzle_diameter", "0.4,0.4"}, {"printer_extruder_id", "1,2"},
        {"printer_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"extruder_printable_height", "0,0"}, {"single_extruder_multi_material", "0"},
        {"filament_diameter", "1.75,1.75"}, {"filament_density", "1.24,1.24"},
        {"filament_type", "PLA;PLA"}, {"filament_map", "1,2"}, {"filament_map_mode", "Manual"},
        {"flush_volumes_matrix", "0,140,140,0"}, {"filament_colour", "#FF0000;#0000FF"}
    });
    input.automatic_tower = true;
    GUI::OrcaTrialSliceControl control;
    bool expect_relocation = false;
    SECTION("an automatic tower can move during the initial preflight") {
        expect_relocation = true;
    }
    SECTION("a saved tower stays where the user put it") {
        input.automatic_tower = false;
    }
    SECTION("a generated-footprint retry keeps its exact anchor") {
        control.allow_estimated_tower_relocation = false;
    }
    SliceRecommendation plan;
    plan.keep_current_process = true;
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    GUI::OrcaTrialSliceDetails details;
    const auto result = executor.execute_recommendation_slice(input, plan, {}, details, control);
    INFO(result.diagnostic_code << ' ' << result.diagnostic_message);
    CHECK_FALSE(details.tower_retry_position);
    CHECK(input.config.option<ConfigOptionFloats>("wipe_tower_y")->get_at(0) == 285.);
    if (expect_relocation) {
        REQUIRE(result.status == TrialSliceStatus::Succeeded);
        REQUIRE(details.tower_position);
        REQUIRE(details.tower_geometry);
        CHECK(details.tower_position->y() < 270.);
        CHECK(details.tower_geometry->check(*details.tower_position) == WipeTowerPositionStatus::Valid);
    } else {
        CHECK(result.status == TrialSliceStatus::Failed);
        CHECK(result.diagnostic_code == "trial_tower_position");
        CHECK_FALSE(details.tower_position);
    }
}

TEST_CASE("A recommendation preserves the native plate process overrides", "[SmartSlice][SmartSliceNativeTrial]")
{
    Model model;
    model.add_object("Layer height control", "", make_cube(10., 10., 10.));
    auto input = smart_input(std::move(model));
    input.config.set_deserialize_strict("layer_height", "0.2");
    input.plate_overrides.set_deserialize_strict("layer_height", "0.2");
    SliceRecommendation plan;
    plan.layer_height_mm = 0.12;
    plan.adaptive_layer_height = true;
    plan.line_width_mm = 0.42;
    plan.outer_wall_speed_mm_s = 40.;
    GUI::OrcaTrialSliceDetails details;
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    const auto result = executor.execute_recommendation_slice(input, plan, {}, details);
    INFO(result.diagnostic_code << ' ' << result.diagnostic_message);
    REQUIRE(result.status == TrialSliceStatus::Succeeded);
    CHECK_FALSE(details.plate_patch.has("layer_height"));
    CHECK(details.layer_profiles.empty());
    CHECK(details.preserves_manual_settings);
    CHECK(details.effective_layer_height == 0.2);
}

TEST_CASE("SmartSlice generates a native adaptive profile only on a disposable plate", "[SmartSlice][SmartSliceNativeTrial]")
{
    Model model;
    model.add_object("Sphere", "", make_sphere(12., 2. * PI / 90.));
    auto input = smart_input(std::move(model));
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    GUI::OrcaTrialSliceDetails details;
    SliceRecommendation plan;
    plan.layer_height_mm = 0.16;
    plan.enable_support = true;
    plan.support_type = "normal(auto)";
    plan.adaptive_layer_height = true;
    const auto trial = executor.execute_recommendation_slice(input, plan, {}, details);
    INFO(trial.diagnostic_code << ' ' << trial.diagnostic_message);
    REQUIRE(trial.status == TrialSliceStatus::Succeeded);
    REQUIRE(trial.metrics);
    REQUIRE(details.layer_range);
    CHECK(details.layer_range->first < details.layer_range->second);
    CHECK(details.variable_layers);
    REQUIRE(details.layer_profiles.size() == 1);
    const auto variable_profile = details.layer_profiles.front().second;
    CHECK(input.model.objects.front()->layer_height_profile.empty());

    input.intent_constraints.records.push_back({IntentConstraintType::SupportPainting,
        IntentConstraintSource::ModelVolumeAnnotation, IntentConstraintState::Active});
    plan.enable_support = false;
    const auto preserved = executor.execute_recommendation_slice(input, plan, {}, details);
    INFO(preserved.diagnostic_code << ' ' << preserved.diagnostic_message);
    REQUIRE(preserved.status == TrialSliceStatus::Succeeded);
    CHECK(details.support_type == "normal");
    CHECK(details.preserves_manual_settings);
    CHECK_FALSE(details.plate_patch.has("enable_support"));
    auto equivalent = plan;
    equivalent.mode = SliceMode::SPEED;
    equivalent.enable_support = true;
    equivalent.support_type = "tree(auto)";
    equivalent.support_threshold_angle_deg = 35.;
    CHECK(GUI::OrcaTrialSliceExecutor::same_native_recommendation_input(input, plan, equivalent));
    equivalent.wall_loops += 1;
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::same_native_recommendation_input(input, plan, equivalent));

    input.intent_constraints.records.clear();
    input.config.set_deserialize_strict("support_style", "organic");
    plan.enable_support = true;
    plan.support_type = "tree(auto)";
    const auto organic = executor.execute_recommendation_slice(input, plan, {}, details);
    INFO(organic.diagnostic_code << ' ' << organic.diagnostic_message);
    REQUIRE(organic.status == TrialSliceStatus::Succeeded);
    CHECK_FALSE(details.variable_layers);
    CHECK(details.layer_profiles.empty());
    CHECK(details.support_type == "tree");

    input.model.objects.front()->layer_height_profile.set(variable_profile);
    input.intent_constraints.records.push_back({IntentConstraintType::VariableLayerHeight,
        IntentConstraintSource::OrcaPartPlate, IntentConstraintState::Active});
    const auto retained_profile = executor.execute_recommendation_slice(input, plan, {}, details);
    INFO(retained_profile.diagnostic_code << ' ' << retained_profile.diagnostic_message);
    REQUIRE(retained_profile.status == TrialSliceStatus::Succeeded);
    CHECK(details.variable_layers);
    CHECK(details.support_type == "normal");
    CHECK(details.layer_profiles.empty());
    CHECK_FALSE(details.plate_patch.has("layer_height"));
}

TEST_CASE("Plate-only support offers real interface evidence alongside the unrestricted candidate", "[SmartSlice][SmartSliceNativeTrial]")
{
    auto input = smart_input(Model::read_from_file(std::string(TEST_DATA_DIR) + "/overhang.obj"));
    input.config.set_deserialize_strict("support_interface_top_layers", "2");
    input.config.set_deserialize_strict("support_interface_bottom_layers", "0");
    input.config.set_deserialize_strict("support_on_build_plate_only", "0");
    SliceRecommendation reference;
    reference.keep_current_process = true;
    auto only_plate = reference;
    only_plate.support_on_build_plate_only = true;
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    GUI::OrcaTrialSliceDetails ordinary, restricted;
    const auto base = executor.execute_recommendation_slice(input, reference, {}, ordinary);
    const auto candidate = executor.execute_recommendation_slice(input, only_plate, {}, restricted);
    INFO(base.diagnostic_code << ' ' << base.diagnostic_message);
    INFO(candidate.diagnostic_code << ' ' << candidate.diagnostic_message);
    REQUIRE(base.status == TrialSliceStatus::Succeeded);
    REQUIRE(candidate.status == TrialSliceStatus::Succeeded);
    CHECK_FALSE(ordinary.plate_only_support);
    CHECK(restricted.plate_only_support);
    CHECK(ordinary.support_interface_signature.has_value());
    CHECK(restricted.support_interface_signature.has_value());
    CHECK_FALSE(input.config.opt_bool("support_on_build_plate_only"));
}

TEST_CASE("SmartSlice builds a geometric tilt preview without changing the input and respects locks", "[SmartSlice][SmartSliceNativeTrial]")
{
    auto mesh = make_cube(25., 30., 18.);
    mesh.rotate(float(26. * PI / 180.), Vec3d::UnitX());
    Model model;
    model.add_object("Tilted block", "", std::move(mesh));
    auto input = smart_input(std::move(model));
    const auto before = input.model.objects.front()->instances.front()->get_matrix();
    auto copy = input;
    GUI::OrcaTrialSliceDetails preview;
    REQUIRE(GUI::OrcaTrialSliceExecutor::prepare_orientation(copy, preview, [] { return false; }));
    CHECK(preview.orientation_degrees > 1.);
    CHECK(preview.orientation_degrees <= 60.2);
    CHECK(preview.original_preview.size() == 12);
    CHECK(preview.oriented_preview.size() == preview.original_preview.size());
    CHECK(input.model.objects.front()->instances.front()->get_matrix().isApprox(before));
    REQUIRE(preview.placement.transforms.size() == 1);

    input.intent_constraints.records.push_back({IntentConstraintType::PlatePlacementLock,
        IntentConstraintSource::OrcaPartPlate, IntentConstraintState::Active});
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::prepare_orientation(input, preview, [] { return false; }));
    CHECK(input.model.objects.front()->instances.front()->get_matrix().isApprox(before));
}

TEST_CASE("Smart slice orientation excludes marked surfaces and forbidden space", "[SmartSlice][SmartSliceNativeTrial]")
{
    auto mesh = make_cube(25., 30., 18.);
    mesh.rotate(float(26. * PI / 180.), Vec3d::UnitX());
    Model model;
    model.add_object("Tilt protection", "", std::move(mesh));
    const auto input = smart_input(std::move(model));
    auto probe = input;
    GUI::OrcaTrialSliceDetails preview;
    REQUIRE(GUI::OrcaTrialSliceExecutor::prepare_orientation(probe, preview, [] { return false; }));
    const auto before = input.model.objects.front()->instances.front()->get_matrix();

    auto marked = input;
    marked.has_user_marked_surface = true;
    preview = {};
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::prepare_orientation(marked, preview, [] { return false; }));
    CHECK(marked.model.objects.front()->instances.front()->get_matrix().isApprox(before));
    CHECK(preview.placement.transforms.empty());

    auto too_tall = input;
    too_tall.config.set_deserialize_strict("printable_height", "15");
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::prepare_orientation(too_tall, preview, [] { return false; }));
    CHECK(preview.placement.transforms.empty());

    auto keepout = input;
    keepout.config.set_deserialize_strict("bed_exclude_area", "130x115,170x115,170x155,130x155");
    REQUIRE_FALSE(get_bed_excluded_regions(keepout.config).empty());
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::prepare_orientation(keepout, preview, [] { return false; }));
    CHECK(preview.placement.transforms.empty());

    auto manual = input;
    manual.intent_constraints.records.push_back({IntentConstraintType::SupportPainting,
        IntentConstraintSource::ModelVolumeAnnotation, IntentConstraintState::Active});
    CHECK_FALSE(GUI::OrcaTrialSliceExecutor::prepare_orientation(manual, preview, [] { return false; }));
    CHECK(manual.model.objects.front()->instances.front()->get_matrix().isApprox(before));
}

TEST_CASE("Smart slice trials respect the search deadline and can recover with a new attempt", "[SmartSlice][SmartSliceNativeTrial]")
{
    Model model;
    model.add_object("Bounded slice", "", make_cube(10., 12., 8.));
    const auto input = smart_input(std::move(model));
    const auto before = input.model.objects.front()->instances.front()->get_matrix();
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    SliceRecommendation baseline;
    baseline.keep_current_process = true;
    GUI::OrcaTrialSliceDetails details;

    const GUI::OrcaTrialSliceControl expired{std::chrono::steady_clock::now(), {}};
    const auto timed_out = executor.execute_recommendation_slice(input, baseline, {}, details, expired);
    CHECK(timed_out.status == TrialSliceStatus::Canceled);
    CHECK(timed_out.diagnostic_code == "workflow_timeout");
    CHECK_FALSE(timed_out.metrics);

    const GUI::OrcaTrialSliceControl interrupted{std::chrono::steady_clock::time_point::max(), [] { return true; }};
    const auto canceled = executor.execute_recommendation_slice(input, baseline, {}, details, interrupted);
    CHECK(canceled.status == TrialSliceStatus::Canceled);
    CHECK(canceled.diagnostic_code == "trial_slice_canceled");
    CHECK_FALSE(canceled.metrics);
    CHECK(input.model.objects.front()->instances.front()->get_matrix().isApprox(before));

    const auto ready = executor.execute_recommendation_slice(input, baseline, {}, details);
    INFO(ready.diagnostic_code << ' ' << ready.diagnostic_message);
    CHECK(ready.status == TrialSliceStatus::Succeeded);
    CHECK(ready.metrics.has_value());

    executor.set_resource_limits(std::chrono::minutes(1), 1, 1024 * 1024);
    const auto too_large = executor.execute_recommendation_slice(input, baseline, {}, details);
    CHECK(too_large.diagnostic_code == "workflow_memory_budget_exceeded");
    CHECK_FALSE(too_large.metrics);

    executor.set_resource_limits(std::chrono::minutes(1), 1024 * 1024, 1);
    const auto too_much_disk = executor.execute_recommendation_slice(input, baseline, {}, details);
    CHECK(too_much_disk.diagnostic_code == "workflow_disk_budget_exceeded");
    CHECK_FALSE(too_much_disk.metrics);
}

TEST_CASE("SmartSlice professional model matrix", "[.SmartSliceCorpus][SmartSliceCorpus]")
{
    const char* corpus = std::getenv("ORCA_SMARTSLICE_CORPUS");
    REQUIRE(corpus != nullptr);
    const std::array<std::pair<const char*, const char*>, 6> assets{{
        {"person", "01-person-multicolor.obj"}, {"figurine", "02-figurine-robot.obj"},
        {"pet", "03-pet-rabbit.obj"}, {"animal", "04-animal-duck.obj"},
        {"architecture", "05-architecture-clock-tower.obj"}, {"sculpture", "06-sculpture.obj"}}};
    for (const auto& [style, filename] : assets) DYNAMIC_SECTION(style) {
        const auto path = (std::filesystem::path(corpus) / filename).string();
        auto input = smart_input(Model::read_from_file(path));
        const auto mesh = input.model.mesh();
        TriangleMeshSnapshot geometry;
        geometry.vertices.reserve(mesh.its.vertices.size());
        for (const auto& v : mesh.its.vertices) geometry.vertices.push_back({v.x(), v.y(), v.z()});
        geometry.triangles.reserve(mesh.its.indices.size());
        for (const auto& f : mesh.its.indices)
            geometry.triangles.push_back({{uint32_t(f.x()), uint32_t(f.y()), uint32_t(f.z())}});
        const auto recommendations = SmartSlicingEngine().AnalyzeAndPrecompute(geometry, {}, {});
        REQUIRE(recommendations.completed);
        auto baseline = recommendations.recommendations.front();
        baseline.keep_current_process = true;
        baseline.adaptive_layer_height = false;
        std::vector<SliceRecommendation> plans{baseline};
        plans.insert(plans.end(), recommendations.recommendations.begin(), recommendations.recommendations.end());
        for (auto& plan : plans) {
            plan.wall_loops = std::max(plan.wall_loops, input.config.opt_int("wall_loops"));
            plan.sparse_infill_density_percent = std::max(plan.sparse_infill_density_percent,
                input.config.option("sparse_infill_density")->getFloat());
        }
        std::vector<MeasuredSliceOption> observations;
        GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
        executor.set_resource_limits(std::chrono::seconds(180), 2ull << 30, 512ull << 20);
        for (const auto& plan : plans) {
            GUI::OrcaTrialSliceDetails details;
            const auto begin = std::chrono::steady_clock::now();
            const auto result = executor.execute_recommendation_slice(input, plan, {}, details);
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            const auto time = result.metrics ? result.metrics->estimated_time_seconds.value_or(0.) : 0.;
            const auto grams = result.metrics ? result.metrics->material_grams.value_or(0.) : 0.;
            std::cout << "SMARTSLICE_CORPUS " << style << ' ' << (plan.keep_current_process ? "baseline" : slice_mode_id(plan.mode))
                << ' ' << static_cast<int>(result.status) << ' ' << details.effective_layer_height << ' ' << details.variable_layers
                << ' ' << details.support_type << ' ' << details.support_grams.value_or(0.) << ' ' << grams << ' ' << time
                << ' ' << seconds << ' ' << result.diagnostic_code << ' ' << result.diagnostic_message << '\n';
            const double used_layer = details.mean_layer_height > 0. ? details.mean_layer_height : plan.layer_height_mm;
            observations.push_back({time, grams, used_layer * recommendations.metrics.slope_detail_area_ratio *
                (details.variable_layers ? 0.8 : 1.), details.support_grams.value_or(0.), plan.keep_current_process});
            if (result.status == TrialSliceStatus::Succeeded) {
                CHECK(time > 0.);
                CHECK(grams > 0.);
                CHECK(details.layer_count > 0);
                REQUIRE(details.layer_range);
                CHECK(details.layer_range->first > 0.);
                CHECK(details.layer_range->second <= 0.30);
            }
        }
        for (const auto& plan : recommendations.recommendations) {
            const auto choice = select_measured_slice_option(plan.mode, observations);
            REQUIRE(choice.has_value());
            std::cout << "SMARTSLICE_CHOICE " << style << ' ' << slice_mode_id(plan.mode) << ' ' << *choice << '\n';
        }
    }
}
