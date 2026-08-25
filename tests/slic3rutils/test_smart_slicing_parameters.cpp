#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterAdvisor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <limits>
#include <type_traits>

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

ParameterProposal intended(ParameterIntent intent, std::initializer_list<ConfigPatchEntry> entries)
{
    ParameterProposal proposal;
    proposal.intent  = intent;
    proposal.entries = entries;
    return proposal;
}

} // namespace

static_assert(std::is_base_of_v<IParameterAdvisor, Slic3r::GUI::OrcaParameterAdvisor>);

TEST_CASE("Orca parameter geometry excludes unprintable objects and instances",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor][TargetEligibility]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(6.0, 20.0, 40.0));
    ModelInstance* instance = object->add_instance();
    object->ensure_on_bed();

    const auto printable = Slic3r::GUI::orca_printable_instance_geometry(object, instance);
    REQUIRE(printable);
    CHECK(printable->width_mm == Catch::Approx(6.0));
    CHECK(printable->depth_mm == Catch::Approx(20.0));
    CHECK(printable->height_mm == Catch::Approx(40.0));

    object->printable = false;
    CHECK_FALSE(Slic3r::GUI::orca_printable_instance_geometry(object, instance));
    object->printable = true;
    instance->printable = false;
    CHECK_FALSE(Slic3r::GUI::orca_printable_instance_geometry(object, instance));
    CHECK_FALSE(Slic3r::GUI::orca_printable_instance_geometry(nullptr, instance));
    CHECK_FALSE(Slic3r::GUI::orca_printable_instance_geometry(object, nullptr));
}

TEST_CASE("typed parameter proposals enforce key type range and enum policy", "[AI][SmartSlicing][Parameters]")
{
    ParameterProposal valid;
    valid.intent = ParameterIntent::Quality;
    valid.entries.push_back(change("layer_height", 0.20, 0.16));
    CHECK(ParameterProposalValidator().validate(valid).accepted());

    ParameterProposal unknown;
    unknown.intent = ParameterIntent::Quality;
    unknown.entries.push_back(change("invented_setting", 1.0, 2.0));
    CHECK(first_rejection(unknown) == ParameterRejectionCode::UnknownKey);

    ParameterProposal wrong_type;
    wrong_type.intent = ParameterIntent::Quality;
    wrong_type.entries.push_back(change("layer_height", 0.20, std::string("0.16")));
    CHECK(first_rejection(wrong_type) == ParameterRejectionCode::TypeMismatch);

    ParameterProposal out_of_range;
    out_of_range.intent = ParameterIntent::Quality;
    out_of_range.entries.push_back(change("layer_height", 0.20, 0.80));
    CHECK(first_rejection(out_of_range) == ParameterRejectionCode::RangeViolation);

    ParameterProposal invalid_enum;
    invalid_enum.intent = ParameterIntent::Quality;
    invalid_enum.entries.push_back(change("seam_position", std::string("aligned"), std::string("hidden")));
    CHECK(first_rejection(invalid_enum) == ParameterRejectionCode::EnumViolation);
}

TEST_CASE("typed parameter proposals require one coherent intent and target",
          "[AI][SmartSlicing][Parameters][Intent]")
{
    ParameterProposal unspecified;
    unspecified.entries.push_back(change("layer_height", 0.20, 0.16));
    CHECK(first_rejection(unspecified) == ParameterRejectionCode::IntentNotSpecified);

    const ParameterProposal quality = intended(
        ParameterIntent::Quality, {change("layer_height", 0.20, 0.16)});
    CHECK(ParameterProposalValidator().validate(quality).accepted());

    const ParameterProposal wrong_family = intended(
        ParameterIntent::Stability, {change("layer_height", 0.20, 0.16)});
    CHECK(first_rejection(wrong_family) == ParameterRejectionCode::IntentKeyNotAllowed);

    const ParameterProposal mixed_targets = intended(
        ParameterIntent::Quality,
        {change("wall_loops", int64_t{2}, int64_t{3}, 4),
         change("layer_height", 0.20, 0.16, 5)});
    CHECK(first_rejection(mixed_targets) == ParameterRejectionCode::MixedTargets);
}

TEST_CASE("typed parameter proposal owner and scope matrix stays plate process only",
          "[AI][SmartSlicing][Parameters][ScopeMatrix]")
{
    const ConfigScope scope = GENERATE(ConfigScope::Object, ConfigScope::Material, ConfigScope::Workspace);
    CAPTURE(scope);
    const ParameterProposal proposal = intended(
        ParameterIntent::Quality,
        {change("layer_height", 0.20, 0.16, 0, scope, PresetOwner::Process)});
    CHECK(first_rejection(proposal) == ParameterRejectionCode::ScopeNotAllowed);

    const PresetOwner owner = GENERATE(PresetOwner::Filament, PresetOwner::Printer, PresetOwner::Project);
    CAPTURE(owner);
    const ParameterProposal wrong_owner = intended(
        ParameterIntent::Quality,
        {change("layer_height", 0.20, 0.16, 0, ConfigScope::Plate, owner)});
    CHECK(first_rejection(wrong_owner) == ParameterRejectionCode::OwnerNotAllowed);
}

TEST_CASE("typed parameter proposals enforce dependent keys and forbidden combinations",
          "[AI][SmartSlicing][Parameters][Coherence]")
{
    const ParameterProposal missing_shell_dependency = intended(
        ParameterIntent::Quality,
        {change("top_shell_layers", int64_t{3}, int64_t{4})});
    CHECK(first_rejection(missing_shell_dependency) == ParameterRejectionCode::MissingDependency);

    const ParameterProposal coherent_shells = intended(
        ParameterIntent::Quality,
        {change("top_shell_layers", int64_t{3}, int64_t{4}),
         change("bottom_shell_layers", int64_t{3}, int64_t{4})});
    CHECK(ParameterProposalValidator().validate(coherent_shells).accepted());

    const ParameterProposal disabled_support_interface = intended(
        ParameterIntent::MaterialSaving,
        {change("enable_support", true, false),
         change("support_interface_top_layers", int64_t{2}, int64_t{1})});
    CHECK(first_rejection(disabled_support_interface) == ParameterRejectionCode::ForbiddenCombination);

    const ParameterProposal disabled_brim_with_growth = intended(
        ParameterIntent::Stability,
        {change("brim_type", std::string("outer_only"), std::string("no_brim")),
         change("brim_width", 1.0, 5.0)});
    CHECK(first_rejection(disabled_brim_with_growth) == ParameterRejectionCode::ForbiddenCombination);

    const ParameterProposal quality_wrong_direction = intended(
        ParameterIntent::Quality, {change("layer_height", 0.20, 0.24)});
    CHECK(first_rejection(quality_wrong_direction) == ParameterRejectionCode::ForbiddenCombination);

    const ParameterProposal speed_wrong_direction = intended(
        ParameterIntent::Speed, {change("layer_height", 0.20, 0.16)});
    CHECK(first_rejection(speed_wrong_direction) == ParameterRejectionCode::ForbiddenCombination);
}

TEST_CASE("typed parameter proposals reject raw sequence flushing hardware and calibration keys",
          "[AI][SmartSlicing][Parameters][Forbidden]")
{
    const std::string key = GENERATE(
        std::string("first_layer_print_sequence"),
        std::string("other_layers_print_sequence"),
        std::string("other_layers_print_sequence_nums"),
        std::string("flush_volumes_matrix"),
        std::string("flush_multiplier"),
        std::string("enable_prime_tower"),
        std::string("nozzle_diameter"),
        std::string("printable_area"),
        std::string("machine_max_acceleration_x"),
        std::string("nozzle_temperature"),
        std::string("bed_temperature"),
        std::string("filament_flow_ratio"),
        std::string("pressure_advance"));
    CAPTURE(key);
    const ParameterProposal proposal = intended(
        ParameterIntent::Stability, {change(key, std::string("before"), std::string("after"))});
    CHECK(first_rejection(proposal) == ParameterRejectionCode::ForbiddenKey);
}

TEST_CASE("typed parameter proposals enforce scope ownership forbidden keys and budgets", "[AI][SmartSlicing][Parameters]")
{
    ParameterProposal wrong_scope;
    wrong_scope.intent = ParameterIntent::Quality;
    wrong_scope.entries.push_back(change("layer_height", 0.20, 0.16, 0, ConfigScope::Object));
    CHECK(first_rejection(wrong_scope) == ParameterRejectionCode::ScopeNotAllowed);

    ParameterProposal wrong_owner;
    wrong_owner.intent = ParameterIntent::Quality;
    wrong_owner.entries.push_back(change("layer_height", 0.20, 0.16, 0, ConfigScope::Plate, PresetOwner::Printer));
    CHECK(first_rejection(wrong_owner) == ParameterRejectionCode::OwnerNotAllowed);

    ParameterProposal hardware;
    hardware.intent = ParameterIntent::Stability;
    hardware.entries.push_back(change("nozzle_diameter", 0.40, 0.60));
    CHECK(first_rejection(hardware) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal unsafe_flush;
    unsafe_flush.intent = ParameterIntent::MaterialSaving;
    unsafe_flush.entries.push_back(change("flush_multiplier", 1.0, 0.8));
    CHECK(first_rejection(unsafe_flush) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal unsafe_tower;
    unsafe_tower.intent = ParameterIntent::MaterialSaving;
    unsafe_tower.entries.push_back(change("enable_prime_tower", true, false));
    CHECK(first_rejection(unsafe_tower) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal excessive_delta;
    excessive_delta.intent = ParameterIntent::Stability;
    excessive_delta.entries.push_back(change("brim_width", 0.0, 20.0));
    CHECK(first_rejection(excessive_delta) == ParameterRejectionCode::ChangeBudgetExceeded);

    ParameterProposal duplicate;
    duplicate.intent = ParameterIntent::Quality;
    duplicate.entries.push_back(change("wall_loops", int64_t{2}, int64_t{3}));
    duplicate.entries.push_back(change("wall_loops", int64_t{2}, int64_t{4}));
    CHECK(first_rejection(duplicate) == ParameterRejectionCode::DuplicateChange);

    ParameterProposal too_many;
    too_many.intent = ParameterIntent::Quality;
    too_many.entries = {
        change("wall_loops", int64_t{2}, int64_t{3}),
        change("top_shell_layers", int64_t{3}, int64_t{4}),
        change("bottom_shell_layers", int64_t{3}, int64_t{4}),
        change("enable_support", false, true),
        change("brim_width", 0.0, 5.0),
    };
    CHECK(first_rejection(too_many) == ParameterRejectionCode::TooManyChanges);
}

TEST_CASE("Orca parameter adapter applies only to a matching config clone", "[AI][SmartSlicing][Parameters][Orca]")
{
    DynamicPrintConfig base = DynamicPrintConfig::full_print_config();
    base.set("layer_height", 0.20);
    ParameterProposal proposal;
    proposal.intent = ParameterIntent::Quality;
    proposal.entries.push_back(change("layer_height", 0.20, 0.16, 3));

    DynamicPrintConfig patched;
    const Slic3r::GUI::OrcaParameterApplyResult accepted =
        Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(proposal, 3, base, patched);
    REQUIRE(accepted.accepted);
    CHECK(patched.opt_float("layer_height") == Catch::Approx(0.16));
    CHECK(base.opt_float("layer_height") == Catch::Approx(0.20));

    DynamicPrintConfig ignored;
    const auto wrong_plate = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(proposal, 2, base, ignored);
    CHECK_FALSE(wrong_plate.accepted);
    CHECK(wrong_plate.diagnostic_code == "parameter_target_mismatch");

    proposal.entries.front().expected_value = 0.24;
    const auto stale_value = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(proposal, 3, base, ignored);
    CHECK_FALSE(stale_value.accepted);
    CHECK(stale_value.diagnostic_code == "parameter_expected_value_changed");

    base.set_deserialize_strict("brim_type", "no_brim");
    ParameterProposal native_auto_brim;
    native_auto_brim.intent = ParameterIntent::Stability;
    native_auto_brim.entries.push_back(
        change("brim_type", std::string("no_brim"), std::string("auto_brim"), 3));
    DynamicPrintConfig native_brim_config;
    const auto native_brim = Slic3r::GUI::OrcaParameterProposalAdapter().validate_and_apply(
        native_auto_brim, 3, base, native_brim_config);
    REQUIRE(native_brim.accepted);
    CHECK(native_brim_config.opt_serialize("brim_type") == "auto_brim");
    CHECK(base.opt_serialize("brim_type") == "no_brim");
}

TEST_CASE("Orca parameter advisor proposes one bounded brim change for fragile geometry",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor]")
{
    Slic3r::GUI::OrcaParameterAdvisorInput input;
    input.plate_id = 7;
    input.current_brim_type = "outer_only";
    input.current_brim_width = 1.0;
    input.printable_instances.push_back({6.0, 20.0, 40.0});
    WorkspaceContext context;
    context.plate_index = 0;
    context.objects.push_back({1, "slender", 1, 12, 0, false});

    const ParameterProposal proposal = Slic3r::GUI::OrcaParameterAdvisor(std::move(input)).advise(
        context, CandidateGoal::Stability);

    REQUIRE(proposal.entries.size() == 1);
    const ConfigPatchEntry& entry = proposal.entries.front();
    CHECK(entry.scope == ConfigScope::Plate);
    CHECK(entry.owner == PresetOwner::Process);
    CHECK(entry.target_id == 7);
    CHECK(entry.key == "brim_width");
    CHECK(std::get<double>(entry.expected_value) == Catch::Approx(1.0));
    CHECK(std::get<double>(entry.new_value) == Catch::Approx(5.0));
    CHECK(entry.reason_code == "improve_small_footprint_adhesion");
    CHECK(proposal.explanation_codes == std::vector<std::string>{"small_or_slender_footprint"});
    CHECK(proposal.intent == ParameterIntent::Stability);
    CHECK(ParameterProposalValidator().validate(proposal).accepted());
}

TEST_CASE("Orca parameter advisor emits bounded goal-specific layer height intent",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor][Goal]")
{
    WorkspaceContext context;
    context.plate_index = 0;
    context.nozzle_diameters = {0.4};

    Slic3r::GUI::OrcaParameterAdvisorInput input;
    input.plate_id = 7;
    input.current_layer_height = 0.20;

    const ParameterProposal quality = Slic3r::GUI::OrcaParameterAdvisor(input).advise(
        context, CandidateGoal::Quality);
    REQUIRE(quality.entries.size() == 1);
    CHECK(quality.intent == ParameterIntent::Quality);
    CHECK(quality.entries.front().key == "layer_height");
    CHECK(std::get<double>(quality.entries.front().expected_value) == Catch::Approx(0.20));
    CHECK(std::get<double>(quality.entries.front().new_value) == Catch::Approx(0.16));
    CHECK(quality.entries.front().reason_code == "use_finer_validated_layer_height");
    CHECK(ParameterProposalValidator().validate(quality).accepted());

    const ParameterProposal speed = Slic3r::GUI::OrcaParameterAdvisor(input).advise(
        context, CandidateGoal::Speed);
    REQUIRE(speed.entries.size() == 1);
    CHECK(speed.intent == ParameterIntent::Speed);
    CHECK(speed.entries.front().key == "layer_height");
    CHECK(std::get<double>(speed.entries.front().expected_value) == Catch::Approx(0.20));
    CHECK(std::get<double>(speed.entries.front().new_value) == Catch::Approx(0.24));
    CHECK(speed.entries.front().reason_code == "use_coarser_validated_layer_height");
    CHECK(ParameterProposalValidator().validate(speed).accepted());

    CHECK(Slic3r::GUI::OrcaParameterAdvisor(input).advise(
        context, CandidateGoal::MaterialSaving).entries.empty());
}

TEST_CASE("Orca parameter advisor stays empty without actionable bounded evidence",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor]")
{
    WorkspaceContext context;
    context.plate_index = 0;
    context.objects.push_back({1, "stable", 1, 12, 0, false});

    Slic3r::GUI::OrcaParameterAdvisorInput stable;
    stable.plate_id = 7;
    stable.current_brim_type = "outer_only";
    stable.current_brim_width = 1.0;
    stable.printable_instances.push_back({30.0, 30.0, 10.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(stable)).advise(
        context, CandidateGoal::Stability).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput capped;
    capped.plate_id = 7;
    capped.current_brim_type = "outer_only";
    capped.current_brim_width = 10.0;
    capped.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(capped)).advise(
        context, CandidateGoal::Stability).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput missing_target;
    missing_target.current_brim_type = "outer_only";
    missing_target.current_brim_width = 1.0;
    missing_target.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(missing_target)).advise(
        context, CandidateGoal::Stability).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput missing_workspace;
    missing_workspace.plate_id = 7;
    missing_workspace.current_brim_type = "outer_only";
    missing_workspace.current_brim_width = 1.0;
    missing_workspace.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(missing_workspace)).advise(
        {}, CandidateGoal::Stability).entries.empty());
}

TEST_CASE("bed adhesion evidence degrades safely when finite geometry overflows the derived risk",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor][MetricValidation]")
{
    const double extreme_footprint = std::numeric_limits<double>::min();
    CHECK_FALSE(Slic3r::GUI::orca_bed_adhesion_risk_score(
        {{extreme_footprint, 1.0, 1.0}}).has_value());

    const std::optional<double> mixed = Slic3r::GUI::orca_bed_adhesion_risk_score(
        {{extreme_footprint, 1.0, 1.0}, {10.0, 20.0, 30.0}});
    REQUIRE(mixed);
    CHECK(*mixed == Catch::Approx(1.5));

    WorkspaceContext context;
    context.plate_index = 0;
    context.objects.push_back({1, "extreme", 1, 12, 0, false});
    Slic3r::GUI::OrcaParameterAdvisorInput input;
    input.plate_id = 7;
    input.current_brim_type = "no_brim";
    input.printable_instances.push_back({extreme_footprint, 1.0, 1.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(input)).advise(
        context, CandidateGoal::Stability).entries.empty());
}

TEST_CASE("Orca parameter advisor uses native auto brim without duplicating an active native policy",
          "[AI][SmartSlicing][Parameters][OrcaAdvisor][BedAdhesion]")
{
    WorkspaceContext context;
    context.plate_index = 0;
    context.objects.push_back({1, "slender", 1, 12, 0, false});

    Slic3r::GUI::OrcaParameterAdvisorInput disabled;
    disabled.plate_id = 7;
    disabled.current_brim_type = "no_brim";
    disabled.current_brim_width = 0.0;
    disabled.printable_instances.push_back({6.0, 20.0, 40.0});
    const ParameterProposal proposal = Slic3r::GUI::OrcaParameterAdvisor(std::move(disabled)).advise(
        context, CandidateGoal::Stability);
    REQUIRE(proposal.entries.size() == 1);
    CHECK(proposal.entries.front().key == "brim_type");
    CHECK(std::get<std::string>(proposal.entries.front().expected_value) == "no_brim");
    CHECK(std::get<std::string>(proposal.entries.front().new_value) == "auto_brim");
    CHECK(ParameterProposalValidator().validate(proposal).accepted());

    Slic3r::GUI::OrcaParameterAdvisorInput active;
    active.plate_id = 7;
    active.current_brim_type = "auto_brim";
    active.current_brim_width = 0.0;
    active.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(active)).advise(
        context, CandidateGoal::Stability).entries.empty());
}
