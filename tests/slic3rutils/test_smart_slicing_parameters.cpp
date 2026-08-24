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
    hardware.entries.push_back(change("nozzle_diameter", 0.40, 0.60));
    CHECK(first_rejection(hardware) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal unsafe_flush;
    unsafe_flush.entries.push_back(change("flush_multiplier", 1.0, 0.8));
    CHECK(first_rejection(unsafe_flush) == ParameterRejectionCode::ForbiddenKey);

    ParameterProposal unsafe_tower;
    unsafe_tower.entries.push_back(change("enable_prime_tower", true, false));
    CHECK(first_rejection(unsafe_tower) == ParameterRejectionCode::ForbiddenKey);

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
    CHECK(first_rejection(too_many) == ParameterRejectionCode::TooManyChanges);
}

TEST_CASE("Orca parameter adapter applies only to a matching config clone", "[AI][SmartSlicing][Parameters][Orca]")
{
    DynamicPrintConfig base = DynamicPrintConfig::full_print_config();
    base.set("layer_height", 0.20);
    ParameterProposal proposal;
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

    const ParameterProposal proposal = Slic3r::GUI::OrcaParameterAdvisor(std::move(input)).advise(context);

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
    CHECK(ParameterProposalValidator().validate(proposal).accepted());
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
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(stable)).advise(context).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput capped;
    capped.plate_id = 7;
    capped.current_brim_type = "outer_only";
    capped.current_brim_width = 10.0;
    capped.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(capped)).advise(context).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput missing_target;
    missing_target.current_brim_type = "outer_only";
    missing_target.current_brim_width = 1.0;
    missing_target.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(missing_target)).advise(context).entries.empty());

    Slic3r::GUI::OrcaParameterAdvisorInput missing_workspace;
    missing_workspace.plate_id = 7;
    missing_workspace.current_brim_type = "outer_only";
    missing_workspace.current_brim_width = 1.0;
    missing_workspace.printable_instances.push_back({6.0, 20.0, 40.0});
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(missing_workspace)).advise({}).entries.empty());
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
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(input)).advise(context).entries.empty());
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
    const ParameterProposal proposal = Slic3r::GUI::OrcaParameterAdvisor(std::move(disabled)).advise(context);
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
    CHECK(Slic3r::GUI::OrcaParameterAdvisor(std::move(active)).advise(context).entries.empty());
}
