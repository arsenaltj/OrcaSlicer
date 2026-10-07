#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "libslic3r/Preset.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;

namespace {

ConfigPatchEntry change(std::string key, ConfigValue expected, ConfigValue replacement,
                        int64_t plate_id = 0, ConfigScope scope = ConfigScope::Plate,
                        PresetOwner owner = PresetOwner::Process)
{
    return {scope, owner, plate_id, std::move(key), std::move(expected), std::move(replacement), "test_reason"};
}

ParameterRejectionCode first_rejection(const ParameterProposal& proposal)
{
    const ParameterValidationResult result = ParameterProposalValidator().validate(proposal);
    REQUIRE_FALSE(result.accepted());
    REQUIRE_FALSE(result.rejections.empty());
    return result.rejections.front().code;
}

} // namespace

TEST_CASE("typed parameter proposals enforce key type range and enum policy", "[AI][SmartSlicing][Parameters]")
{
    ParameterProposal valid;
    valid.entries.push_back(change("layer_height", 0.20, 0.16));
    CHECK(ParameterProposalValidator().validate(valid).accepted());

    ParameterProposal unknown;
    unknown.entries.push_back(change("invented_setting", 1.0, 2.0));
    CHECK(first_rejection(unknown) == ParameterRejectionCode::UnknownKey);

    ParameterProposal wrong_type;
    wrong_type.entries.push_back(change("layer_height", 0.20, std::string("0.16")));
    CHECK(first_rejection(wrong_type) == ParameterRejectionCode::TypeMismatch);

    ParameterProposal out_of_range;
    out_of_range.entries.push_back(change("layer_height", 0.20, 0.80));
    CHECK(first_rejection(out_of_range) == ParameterRejectionCode::RangeViolation);

    ParameterProposal invalid_enum;
    invalid_enum.entries.push_back(change("seam_position", std::string("aligned"), std::string("hidden")));
    CHECK(first_rejection(invalid_enum) == ParameterRejectionCode::EnumViolation);
}

TEST_CASE("typed parameter proposals enforce scope ownership forbidden keys and budgets", "[AI][SmartSlicing][Parameters]")
{
    ParameterProposal wrong_scope;
    wrong_scope.entries.push_back(change("layer_height", 0.20, 0.16, 0, ConfigScope::Object));
    CHECK(first_rejection(wrong_scope) == ParameterRejectionCode::ScopeNotAllowed);

    ParameterProposal wrong_owner;
    wrong_owner.entries.push_back(change("layer_height", 0.20, 0.16, 0, ConfigScope::Plate, PresetOwner::Printer));
    CHECK(first_rejection(wrong_owner) == ParameterRejectionCode::OwnerNotAllowed);

    ParameterProposal hardware;
    hardware.entries.push_back(change("nozzle_diameter", 0.40, 0.60, 0,
                                      ConfigScope::Plate, PresetOwner::Printer));
    CHECK(first_rejection(hardware) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal unsafe_flush;
    unsafe_flush.entries.push_back(change("flush_multiplier", 1.0, 0.8, 0,
                                          ConfigScope::Plate, PresetOwner::Project));
    CHECK(first_rejection(unsafe_flush) == ParameterRejectionCode::EffectiveBoundsUnavailable);

    ParameterProposal unsafe_tower;
    unsafe_tower.entries.push_back(change("enable_prime_tower", true, false));
    CHECK(first_rejection(unsafe_tower) == ParameterRejectionCode::UnknownKey);

    ParameterProposal excessive_delta;
    excessive_delta.entries.push_back(change("brim_width", 0.0, 20.0));
    CHECK(first_rejection(excessive_delta) == ParameterRejectionCode::ChangeBudgetExceeded);

    ParameterProposal duplicate;
    duplicate.entries.push_back(change("wall_loops", int64_t{2}, int64_t{3}));
    duplicate.entries.push_back(change("wall_loops", int64_t{2}, int64_t{4}));
    CHECK(first_rejection(duplicate) == ParameterRejectionCode::DuplicateChange);

    ParameterProposal too_many;
    too_many.entries = {
        change("wall_loops", int64_t{2}, int64_t{3}),
        change("top_shell_layers", int64_t{3}, int64_t{4}),
        change("bottom_shell_layers", int64_t{3}, int64_t{4}),
        change("enable_support", false, true),
        change("brim_width", 0.0, 5.0),
    };
    CHECK(ParameterProposalValidator().validate(too_many).accepted());
}

TEST_CASE("Orca parameter adapter applies only to a matching config clone", "[AI][SmartSlicing][Parameters][Orca]")
{
    DynamicPrintConfig base = DynamicPrintConfig::full_print_config();
    base.set("brim_width", 0.0);
    ParameterProposal proposal;
    proposal.entries.push_back(change("brim_width", 0.0, 5.0, 3));

    DynamicPrintConfig patched;
    const auto legacy = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        proposal, 3, base, patched);
    CHECK_FALSE(legacy.accepted);
    CHECK(legacy.diagnostic_code == "parameter_validation_context_required");
    const Slic3r::GUI::OrcaParameterApplyResult accepted =
        Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(proposal, 3, base, {}, {}, patched);
    REQUIRE(accepted.accepted);
    CHECK(patched.opt_float("brim_width") == Catch::Approx(5.0));
    CHECK(base.opt_float("brim_width") == Catch::Approx(0.0));

    DynamicPrintConfig ignored;
    const auto wrong_plate = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        proposal, 2, base, {}, {}, ignored);
    CHECK_FALSE(wrong_plate.accepted);
    CHECK(wrong_plate.diagnostic_code == "parameter_target_mismatch");

    proposal.entries.front().expected_value = 1.0;
    const auto stale_value = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        proposal, 3, base, {}, {}, ignored);
    CHECK_FALSE(stale_value.accepted);
    CHECK(stale_value.diagnostic_code == "parameter_expected_value_changed");

    base.set("layer_height", 0.20);
    ParameterProposal profiled;
    profiled.entries.push_back(change("layer_height", 0.20, 0.16, 3));
    const auto missing_profile_bound = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        profiled, 3, base, {}, {}, ignored);
    CHECK_FALSE(missing_profile_bound.accepted);
    CHECK(missing_profile_bound.diagnostic_code == "parameter_effective_bounds_unavailable");

    const std::vector<ParameterBoundEvidence> process_bounds{{
        ConfigScope::Plate, PresetOwner::Process, 3, "layer_height",
        BoundEvidenceSource::ProcessProfile, true, 0.12, 0.28, {},
        "fixture-process-profile/v1"}};
    const auto bounded = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        profiled, 3, base, {}, process_bounds, patched);
    REQUIRE(bounded.accepted);
    CHECK(patched.opt_float("layer_height") == Catch::Approx(0.16));
    CHECK(base.opt_float("layer_height") == Catch::Approx(0.20));
}

TEST_CASE("plate process proposals preserve native object overrides and global settings", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(5, 5, 5));
    object->add_instance()->set_offset(Vec3d(50, 50, 0));
    object->config.set("wall_loops", 4);
    const Model before(model);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("brim_width", 5.0);
    ParameterProposal proposal;
    proposal.entries.push_back(change("brim_width", 5.0, 7.0, 3));
    GUI::OrcaParameterProposalAdapter adapter;
    std::vector<GUI::OrcaObjectParameterPatch> patches;
    REQUIRE(adapter.prepare_object_patches(proposal, 3, config, {object}, patches).accepted);
    CHECK(object->config.get() == before.objects.front()->config.get());
    REQUIRE(adapter.apply_object_patches(model, patches).accepted);
    CHECK_THAT(object->config.opt_float("brim_width"), Catch::Matchers::WithinAbs(7.0, 1e-9));
    CHECK(object->config.opt_int("wall_loops") == 4);
    CHECK_THAT(config.opt_float("brim_width"), Catch::Matchers::WithinAbs(5.0, 1e-9));
    Print consumer;
    consumer.apply(model, config);
    REQUIRE(consumer.objects().size() == 1);
    CHECK_THAT(consumer.objects().front()->config().brim_width.value, Catch::Matchers::WithinAbs(7.0, 1e-9));
    // Native undo restores the complete ModelConfig including its timestamp.
    object->config.assign_config(before.objects.front()->config);
    CHECK(object->config.get() == before.objects.front()->config.get());
    CHECK(object->config.timestamp_matches(before.objects.front()->config));
}

TEST_CASE("conflicting object or volume overrides reject the whole plate proposal", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model;
    auto* a = model.add_object(); a->add_volume(make_cube(5, 5, 5)); a->add_instance();
    auto* b = model.add_object(); b->add_volume(make_cube(5, 5, 5)); b->add_instance();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config(); config.set("brim_width", 5.0);
    ParameterProposal proposal; proposal.entries.push_back(change("brim_width", 5.0, 7.0, 3));
    GUI::OrcaParameterProposalAdapter adapter;
    std::vector<GUI::OrcaObjectParameterPatch> patches;
    b->config.set("brim_width", 6.0);
    auto result = adapter.prepare_object_patches(proposal, 3, config, {a,b}, patches);
    CHECK_FALSE(result.accepted);
    CHECK(result.diagnostic_code == "parameter_expected_value_changed");
    CHECK(patches.empty());
    CHECK_FALSE(a->config.has("brim_width"));
    b->config.erase("brim_width");
    b->volumes.front()->config.set("brim_width", 5.0);
    result = adapter.prepare_object_patches(proposal, 3, config, {a,b}, patches);
    CHECK_FALSE(result.accepted);
    CHECK(result.diagnostic_code == "parameter_volume_override_conflict");
    CHECK(patches.empty());
    CHECK_FALSE(a->config.has("brim_width"));
    b->volumes.front()->config.erase("brim_width");
    REQUIRE(adapter.prepare_object_patches(proposal, 3, config, {a,b}, patches).accepted);
    b->config.set("wall_loops", 8); // A later change cannot cause a half-apply.
    CHECK_FALSE(adapter.apply_object_patches(model, patches).accepted);
    CHECK_FALSE(a->config.has("brim_width"));
    CHECK_FALSE(b->config.has("brim_width"));
    b->config.erase("wall_loops");
    REQUIRE(adapter.apply_object_patches(model, patches).accepted);
    CHECK_THAT(a->config.opt_float("brim_width"), Catch::Matchers::WithinAbs(7.0, 1e-9));
    CHECK_THAT(b->config.opt_float("brim_width"), Catch::Matchers::WithinAbs(7.0, 1e-9));
}

TEST_CASE("native object patch preparation rejects missing targets and mismatched plates", "[AI][SmartSlicing][Parameters][Orca]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config(); config.set("brim_width", 5.0);
    ParameterProposal proposal; proposal.entries.push_back(change("brim_width", 5.0, 7.0, 3));
    GUI::OrcaParameterProposalAdapter adapter;
    std::vector<GUI::OrcaObjectParameterPatch> patches;
    CHECK(adapter.prepare_object_patches(proposal, 3, config, {}, patches).diagnostic_code == "parameter_target_objects_missing");
    Model model; auto* object = model.add_object();
    proposal.entries.front().target_id = 4;
    CHECK(adapter.prepare_object_patches(proposal, 3, config, {object}, patches).diagnostic_code == "parameter_target_mismatch");
    CHECK(patches.empty());
}

TEST_CASE("saved native object parameters survive a real 3MF round trip without changing defaults", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model; auto* object = model.add_object(); object->name = "native parameter round trip";
    object->add_volume(make_cube(5,5,5));
    object->add_instance()->set_offset(Vec3d(50,50,0));
    object->config.set("wall_loops", 4);
    auto* neighbor = model.add_object(); neighbor->add_volume(make_cube(8,8,8));
    neighbor->name = "untargeted object"; neighbor->add_instance()->set_offset(Vec3d(80,80,0));
    neighbor->config.set("brim_width", 2.0);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config(); config.set("brim_width", 5.0);
    ParameterProposal proposal; proposal.entries.push_back(change("brim_width", 5.0, 7.0, 3));
    GUI::OrcaParameterProposalAdapter adapter;
    std::vector<GUI::OrcaObjectParameterPatch> patches;
    REQUIRE(adapter.prepare_object_patches(proposal, 3, config, {object}, patches).accepted);
    REQUIRE(adapter.apply_object_patches(model, patches).accepted);
    ScopedTemporaryDir backup("native-parameter-source"); model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf"); const std::string path = file.string();
    PlateData plate; plate.plate_index = 0;
    StoreParams params; params.path = path.c_str(); params.model = &model; params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence; params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored; ScopedTemporaryDir restored_backup("native-parameter-restored"); restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates; std::vector<Preset*> presets; bool is_bbl=false, is_orca=false; Semver version;
    const bool loaded=load_bbs_3mf(path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates); for (auto* preset : presets) delete preset;
    REQUIRE(loaded); REQUIRE(restored.objects.size() == 2);
    CHECK_THAT(restored_config.opt_float("brim_width"), Catch::Matchers::WithinAbs(5.0, 1e-9));
    CHECK_THAT(restored.objects.front()->config.opt_float("brim_width"), Catch::Matchers::WithinAbs(7.0, 1e-9));
    CHECK(restored.objects.front()->config.opt_int("wall_loops") == 4);
    CHECK_THAT(restored.objects.back()->config.opt_float("brim_width"), Catch::Matchers::WithinAbs(2.0, 1e-9));
    // Consume saved object overrides under the same complete native preset
    // snapshot. Raw load_bbs_3mf project options are not a PresetBundle.
    // Complete GUI project/preset reopening is verified separately.
    Print consumer;
    for (auto* reopened : restored.objects) consumer.auto_assign_extruders(reopened);
    consumer.apply(restored, config);
    REQUIRE(consumer.objects().size() == 2);
    CHECK_THAT(consumer.objects().front()->config().brim_width.value, Catch::Matchers::WithinAbs(7.0, 1e-9));
}

TEST_CASE("priority profiles follow the current nozzle and native layer limits", "[AI][SmartSlicing][Parameters][Orca]")
{
    const double nozzle = GENERATE(0.2, 0.4);
    Model model; auto* object = model.add_object();
    object->add_volume(make_cube(5,5,5)); object->add_instance()->set_offset(Vec3d(50,50,0));
    object->config.set("wall_loops", 4);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("layer_height", nozzle / 2.0);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{nozzle});
    config.set_key_value("min_layer_height", new ConfigOptionFloats{0.04});
    config.set_key_value("max_layer_height", new ConfigOptionFloats{nozzle * 0.7});
    const Model before(model);
    GUI::OrcaParameterProposalAdapter adapter;
    const WorkspaceRevision revision{1,2,3,"current"};
    const auto profiles = adapter.priority_candidates(revision, 7, config, {object});
    REQUIRE(profiles.size() == 3);
    const std::vector<double> ratios{0.4,0.7,0.3};
    for (size_t i=0; i<profiles.size(); ++i) {
        REQUIRE(profiles[i].parameters.entries.size() == 1);
        CHECK(profiles[i].base_revision == revision);
        CHECK_THAT(std::get<double>(profiles[i].parameters.entries.front().new_value),
            Catch::Matchers::WithinAbs(nozzle * ratios[i], 1e-9));
        std::vector<GUI::OrcaObjectParameterPatch> patches;
        REQUIRE(adapter.prepare_object_patches(profiles[i].parameters,7,config,{object},patches).accepted);
        Model trial(model); REQUIRE(adapter.apply_object_patches(trial, patches).accepted);
        Print consumer; consumer.apply(trial,config);
        REQUIRE(consumer.objects().size() == 1);
        CHECK_THAT(consumer.objects().front()->config().layer_height.value,
            Catch::Matchers::WithinAbs(nozzle * ratios[i], 1e-9));
        CHECK(trial.objects.front()->config.opt_int("wall_loops") == 4);
    }
    CHECK(object->config.get() == before.objects.front()->config.get());
    CHECK(object->config.timestamp_matches(before.objects.front()->config));
    CHECK_THAT(config.opt_float("layer_height"),Catch::Matchers::WithinAbs(nozzle/2.0,1e-9));
}

TEST_CASE("a priority equal to the current layer height remains a read only trial", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model; auto* object = model.add_object(); object->add_volume(make_cube(5,5,5)); object->add_instance();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set("layer_height",0.16);
    config.set_key_value("nozzle_diameter",new ConfigOptionFloats{0.4});
    config.set_key_value("min_layer_height",new ConfigOptionFloats{0.04});
    config.set_key_value("max_layer_height",new ConfigOptionFloats{0.28});
    const auto profiles = GUI::OrcaParameterProposalAdapter().priority_candidates({1,2,3,"current"},7,config,{object});
    REQUIRE(profiles.size() == 3);
    CHECK(profiles.front().parameters.entries.empty());
    CHECK(profiles.front().explanation == "layer_height_balanced_candidate");
    CHECK_FALSE(object->config.has("layer_height"));
    CHECK_THAT(config.opt_float("layer_height"),Catch::Matchers::WithinAbs(0.16,1e-9));
}

TEST_CASE("priority profiles preserve custom layering and conflicting part overrides", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model; auto* a=model.add_object(); a->add_volume(make_cube(5,5,5)); a->add_instance();
    auto* b=model.add_object(); b->add_volume(make_cube(5,5,5)); b->add_instance();
    DynamicPrintConfig config=DynamicPrintConfig::full_print_config(); config.set("layer_height",0.2);
    config.set_key_value("min_layer_height",new ConfigOptionFloats{0.04});
    GUI::OrcaParameterProposalAdapter adapter;
    const WorkspaceRevision revision{1,2,3,"current"};
    b->config.set("layer_height",0.18);
    CHECK(adapter.priority_candidates(revision,7,config,{a,b}).empty());
    CHECK_FALSE(a->config.has("layer_height"));
    b->config.erase("layer_height"); b->volumes.front()->config.set("layer_height",0.2);
    CHECK(adapter.priority_candidates(revision,7,config,{a,b}).empty());
    b->volumes.front()->config.erase("layer_height");
    b->layer_height_profile.set(std::vector<coordf_t>{0.0,0.1,5.0,0.2});
    const auto profile_before=b->layer_height_profile.get();
    CHECK(adapter.priority_candidates(revision,7,config,{a,b}).empty());
    CHECK(b->layer_height_profile.get() == profile_before);
    CHECK_FALSE(a->config.has("layer_height"));
}

TEST_CASE("priority profiles reject collapsed limits and keep changes within the process budget", "[AI][SmartSlicing][Parameters][Orca]")
{
    Model model; auto* object=model.add_object(); object->add_volume(make_cube(5,5,5)); object->add_instance();
    DynamicPrintConfig config=DynamicPrintConfig::full_print_config(); config.set("layer_height",0.1);
    config.set_key_value("nozzle_diameter",new ConfigOptionFloats{0.2,0.4});
    config.set_key_value("min_layer_height",new ConfigOptionFloats{0.07});
    config.set_key_value("max_layer_height",new ConfigOptionFloats{0.09});
    GUI::OrcaParameterProposalAdapter adapter; const WorkspaceRevision revision{1,2,3,"current"};
    auto profiles=adapter.priority_candidates(revision,7,config,{object});
    REQUIRE(profiles.size() == 3);
    for (const auto& profile : profiles) {
        const double h=std::get<double>(profile.parameters.entries.front().new_value);
        CHECK(h >= 0.07); CHECK(h <= 0.09); CHECK(std::abs(h-0.1) <= 0.12);
    }
    config.set_key_value("max_layer_height",new ConfigOptionFloats{0.07});
    CHECK(adapter.priority_candidates(revision,7,config,{object}).empty());
    config.set_key_value("max_layer_height",new ConfigOptionFloats{0.14});
    config.set("layer_height",0.4);
    CHECK(adapter.priority_candidates(revision,7,config,{object}).empty());
    CHECK(adapter.priority_candidates(revision,7,config,{}).empty());
}

TEST_CASE("the three priority trials measure different native layers without changing the workspace", "[AI][SmartSlicing][Parameters][OrcaTrial]")
{
    GUI::OrcaTrialSliceInput input;
    auto* object=input.model.add_object(); object->add_volume(make_cube(5,5,5));
    object->add_instance()->set_offset(Vec3d(50,50,0)); object->ensure_on_bed();
    input.config=DynamicPrintConfig::full_print_config();
    input.config.set("layer_height",0.2);
    input.config.set("initial_layer_print_height",0.2);
    input.config.set("layer_change_gcode",std::string("G92 E0\n"));
    input.config.set_key_value("min_layer_height",new ConfigOptionFloats{0.04});
    input.plate_id=7; input.plate_name="Priority trial";
    GUI::OrcaParameterProposalAdapter adapter;
    const auto profiles=adapter.priority_candidates({1,2,3,"current"},7,input.config,{object});
    REQUIRE(profiles.size() == 3);
    const Model original(input.model);
    GUI::OrcaTrialSliceExecutor executor([] { return GUI::OrcaTrialSliceInput{}; });
    executor.prepare_session_input(input,profiles);
    std::vector<size_t> layer_counts;
    for (const auto& profile : profiles) {
        std::vector<GUI::OrcaObjectParameterPatch> validation_patches;
        REQUIRE(adapter.prepare_object_patches(profile.parameters,7,input.config,{object},validation_patches).accepted);
        Model validation_model(input.model);
        REQUIRE(adapter.apply_object_patches(validation_model,validation_patches).accepted);
        Print validation_print;
        for (auto* target : validation_model.objects) validation_print.auto_assign_extruders(target);
        validation_print.apply(validation_model,input.config);
        const auto validation_error=validation_print.validate();
        INFO(profile.id << ": " << validation_error.string);
        REQUIRE(validation_error.string.empty());
        const auto result=executor.execute_trial_slice(profile);
        INFO(result.diagnostic_code);
        REQUIRE(result.status == TrialSliceStatus::Succeeded); REQUIRE(result.metrics);
        CHECK(result.metrics->estimated_time_seconds.value_or(0) > 0.0);
        std::vector<GUI::OrcaObjectParameterPatch> patches;
        REQUIRE(adapter.prepare_object_patches(profile.parameters,7,input.config,{object},patches).accepted);
        Model native(input.model); REQUIRE(adapter.apply_object_patches(native,patches).accepted);
        Print consumer; consumer.apply(native,input.config); consumer.process();
        REQUIRE(consumer.objects().size() == 1);
        layer_counts.push_back(consumer.objects().front()->layers().size());
    }
    REQUIRE(layer_counts.size() == 3);
    CHECK(layer_counts[2] > layer_counts[0]);
    CHECK(layer_counts[0] > layer_counts[1]);
    CHECK(input.model.objects.front()->config.get() == original.objects.front()->config.get());
    CHECK(input.model.objects.front()->instances.front()->get_matrix().isApprox(original.objects.front()->instances.front()->get_matrix()));
    CHECK_THAT(input.config.opt_float("layer_height"),Catch::Matchers::WithinAbs(0.2,1e-9));
}
