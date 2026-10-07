#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"

#include <algorithm>
#include <numeric>

using namespace Slic3r::AI::SmartSlicing;

namespace {

ConfigPatchEntry patch(std::string key, ConfigValue expected, ConfigValue replacement,
                       int64_t target = 0, ConfigScope scope = ConfigScope::Plate,
                       PresetOwner owner = PresetOwner::Process)
{
    return {scope, owner, target, std::move(key), std::move(expected),
            std::move(replacement), "policy_test"};
}

ParameterProposal proposal(ConfigPatchEntry entry,
                           RecommendationGoal goal = RecommendationGoal::Balanced)
{
    ParameterProposal result;
    result.goal = goal;
    result.entries.push_back(std::move(entry));
    return result;
}

ParameterValidationContext context_for(const ParameterProposal& proposal,
                                       bool native_accepted = true)
{
    ParameterValidationContext context;
    context.goal = proposal.goal;
    for (const ConfigPatchEntry& entry : proposal.entries)
        context.current_values.push_back(
            {entry.scope, entry.owner, entry.target_id, entry.key, entry.expected_value});
    context.native_validator = [native_accepted](const ParameterProposal&) {
        return NativeParameterValidationResult{native_accepted,
                                                native_accepted ? "" : "native_fixture_rejected"};
    };
    return context;
}

void add_bound(ParameterValidationContext& context, const ConfigPatchEntry& entry,
               BoundEvidenceSource source, double minimum, double maximum,
               bool valid = true)
{
    context.bounds.push_back({entry.scope, entry.owner, entry.target_id, entry.key,
                              source, valid, minimum, maximum, {}, "fixture-profile/v1"});
}

ParameterRejectionCode rejection(const ParameterProposal& proposal,
                                 const ParameterValidationContext& context)
{
    const auto result = ParameterProposalValidator().validate(proposal, context);
    REQUIRE_FALSE(result.accepted());
    REQUIRE(result.rejections.size() == 1);
    return result.rejections.front().code;
}

void add_calibrated_bounds(ParameterValidationContext& context,
                           const ConfigPatchEntry& entry,
                           double minimum = 0.1, double maximum = 1.0)
{
    add_bound(context, entry, BoundEvidenceSource::MachineProfile, minimum, maximum);
    add_bound(context, entry, BoundEvidenceSource::ProcessProfile, minimum, maximum);
    add_bound(context, entry, BoundEvidenceSource::MaterialProfile, minimum, maximum);
}

} // namespace

TEST_CASE("parameter policy registry freezes categories risks and bounded PatchSet version",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    const auto& registry = ParameterPolicyRegistry::current();
    CHECK(registry.version() == PARAMETER_POLICY_VERSION);
    CHECK(registry.budget().version == "parameter-patch-budget/v1");
    CHECK(registry.budget().maximum_registered_changes == 26);
    CHECK(registry.budget().maximum_serialized_bytes == 4096);

    const std::vector<std::string> required{
        "object_orientation_strategy", "enable_support", "layer_height", "wall_loops",
        "top_shell_layers", "bottom_shell_layers", "sparse_infill_density",
        "outer_wall_speed", "default_acceleration", "brim_width", "seam_position",
        "wall_sequence", "tool_change_sequence", "line_width", "retraction_length",
        "flush_multiplier", "wipe_tower_width", "nozzle_diameter", "printable_area",
        "nozzle_offset", "filament_max_volumetric_speed", "filament_flow_ratio",
        "pressure_advance", "nozzle_temperature_range_low",
        "nozzle_temperature_range_high"};
    for (const std::string& key : required)
        CHECK(registry.find(key) != nullptr);
    CHECK(registry.find("line_width")->risk_class == ParameterRiskClass::CalibratedRange);
    CHECK(registry.find("fan_max_speed")->value_kind == ConfigValueKind::Floating);
    CHECK(registry.find("nozzle_diameter")->risk_class == ParameterRiskClass::ImmutableFact);
    CHECK(registry.find("nozzle_temperature_range_low")->risk_class ==
          ParameterRiskClass::ImmutableFact);
    CHECK(registry.find("layer_height")->bound_source == BoundSource::ProcessProfile);
}

TEST_CASE("parameter validation stages policy and structural errors before later evidence",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto value = proposal(patch("invented", 1.0, 2.0));
    value.policy_version = "stale-policy";
    CHECK(rejection(value, {}) == ParameterRejectionCode::PolicyVersionMismatch);
    value.policy_version = PARAMETER_POLICY_VERSION;
    CHECK(rejection(value, {}) == ParameterRejectionCode::UnknownKey);

    value = proposal(patch("layer_height", 0.20, std::string("0.16")));
    CHECK(rejection(value, {}) == ParameterRejectionCode::TypeMismatch);
    value = proposal(patch("layer_height", 0.20, 0.16, 1, ConfigScope::Object));
    CHECK(rejection(value, {}) == ParameterRejectionCode::ScopeNotAllowed);
    value = proposal(patch("layer_height", 0.20, 0.16, 0, ConfigScope::Plate,
                           PresetOwner::Printer));
    CHECK(rejection(value, {}) == ParameterRejectionCode::OwnerNotAllowed);
    value = proposal(patch("object_orientation_strategy", std::string("current"),
                           std::string("stable_plane"), 0, ConfigScope::Object,
                           PresetOwner::Project));
    CHECK(rejection(value, {}) == ParameterRejectionCode::TargetNotSpecified);

    value = proposal(patch("wall_loops", int64_t{2}, int64_t{3}));
    value.entries.push_back(value.entries.front());
    CHECK(rejection(value, {}) == ParameterRejectionCode::DuplicateChange);
    value.entries.resize(1);
    value.entries.front().new_value = int64_t{2};
    CHECK(rejection(value, {}) == ParameterRejectionCode::NoEffectiveChange);
}

TEST_CASE("expected snapshots human intent and immutable facts are hard ordered gates",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto value = proposal(patch("enable_support", false, true));
    CHECK(rejection(value, {}) == ParameterRejectionCode::ExpectedSnapshotMissing);
    auto context = context_for(value);
    context.current_values.front().value = true;
    CHECK(rejection(value, context) == ParameterRejectionCode::ExpectedValueMismatch);

    context = context_for(value);
    IntentConstraintRecord painting;
    painting.type = IntentConstraintType::SupportPainting;
    painting.state = IntentConstraintState::Active;
    context.intent_constraints.records.push_back(painting);
    CHECK(rejection(value, context) == ParameterRejectionCode::IntentConflict);

    auto immutable = proposal(patch("nozzle_diameter", 0.4, 0.6, 0,
                                    ConfigScope::Plate, PresetOwner::Printer));
    CHECK(rejection(immutable, context_for(immutable)) == ParameterRejectionCode::ForbiddenKey);

    auto temperature_range = proposal(patch("nozzle_temperature_range_low", int64_t{190},
                                            int64_t{200}, 1, ConfigScope::Material,
                                            PresetOwner::Filament));
    CHECK(rejection(temperature_range, context_for(temperature_range)) ==
          ParameterRejectionCode::ForbiddenKey);

    immutable.entries.front().new_value = std::string("wrong");
    CHECK(rejection(immutable, context_for(immutable)) == ParameterRejectionCode::TypeMismatch);
}

TEST_CASE("placement locks distinguish plate-wide and stable object targets",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto orientation = proposal(patch("object_orientation_strategy", std::string("current"),
                                      std::string("stable_plane"), 7, ConfigScope::Object,
                                      PresetOwner::Project));

    auto plate_locked = context_for(orientation);
    IntentConstraintRecord plate_lock;
    plate_lock.type = IntentConstraintType::PlatePlacementLock;
    plate_lock.state = IntentConstraintState::Active;
    plate_lock.object_id = 0;
    plate_locked.intent_constraints.records.push_back(plate_lock);
    CHECK(rejection(orientation, plate_locked) == ParameterRejectionCode::IntentConflict);

    auto other_object_locked = context_for(orientation);
    IntentConstraintRecord object_lock;
    object_lock.type = IntentConstraintType::ObjectPlacementLock;
    object_lock.state = IntentConstraintState::Active;
    object_lock.object_id = 8;
    other_object_locked.intent_constraints.records.push_back(object_lock);
    CHECK(ParameterProposalValidator().validate(orientation, other_object_locked).accepted());

    auto target_object_locked = context_for(orientation);
    object_lock.object_id = 7;
    target_object_locked.intent_constraints.records.push_back(object_lock);
    CHECK(rejection(orientation, target_object_locked) == ParameterRejectionCode::IntentConflict);

    auto target_instance_locked = context_for(orientation);
    IntentConstraintRecord instance_lock;
    instance_lock.type = IntentConstraintType::InstancePlacementLock;
    instance_lock.state = IntentConstraintState::Active;
    instance_lock.object_id = 7;
    instance_lock.instance_id = 3;
    target_instance_locked.intent_constraints.records.push_back(instance_lock);
    CHECK(rejection(orientation, target_instance_locked) == ParameterRejectionCode::IntentConflict);
}

TEST_CASE("effective profile intersections reject missing empty and out of range evidence",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto layer = proposal(patch("layer_height", 0.20, 0.16));
    auto layer_context = context_for(layer);
    CHECK(rejection(layer, layer_context) == ParameterRejectionCode::EffectiveBoundsUnavailable);
    add_bound(layer_context, layer.entries.front(), BoundEvidenceSource::ProcessProfile, 0.12, 0.28);
    CHECK(ParameterProposalValidator().validate(layer, layer_context).accepted());
    layer.entries.front().new_value = 0.32;
    layer_context.current_values.front().value = 0.20;
    CHECK(rejection(layer, layer_context) == ParameterRejectionCode::RangeViolation);
    layer.entries.front().new_value = 0.16;
    layer_context.bounds.front().minimum = 0.30;
    layer_context.bounds.front().maximum = 0.10;
    CHECK(rejection(layer, layer_context) == ParameterRejectionCode::EffectiveBoundsEmpty);

    auto calibrated = proposal(patch("line_width", 0.40, 0.45));
    auto calibrated_context = context_for(calibrated);
    add_bound(calibrated_context, calibrated.entries.front(), BoundEvidenceSource::MachineProfile, 0.30, 0.60);
    add_bound(calibrated_context, calibrated.entries.front(), BoundEvidenceSource::ProcessProfile, 0.35, 0.50);
    CHECK(rejection(calibrated, calibrated_context) == ParameterRejectionCode::EffectiveBoundsUnavailable);
    add_bound(calibrated_context, calibrated.entries.front(), BoundEvidenceSource::MaterialProfile, 0.38, 0.48);
    CHECK(ParameterProposalValidator().validate(calibrated, calibrated_context).accepted());
    calibrated.entries.front().new_value = 0.49;
    CHECK(rejection(calibrated, calibrated_context) == ParameterRejectionCode::RangeViolation);
}

TEST_CASE("goal budgets and native validation execute after semantic range gates",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto seam = proposal(patch("seam_position", std::string("aligned"), std::string("back")));
    auto seam_context = context_for(seam);
    seam_context.goal = RecommendationGoal::Speed;
    CHECK(rejection(seam, seam_context) == ParameterRejectionCode::GoalNotAllowed);
    seam.entries.front().new_value = std::string("invented");
    CHECK(rejection(seam, seam_context) == ParameterRejectionCode::EnumViolation);

    auto brim = proposal(patch("brim_width", 0.0, 5.0));
    auto brim_context = context_for(brim);
    brim_context.native_validator = {};
    CHECK(rejection(brim, brim_context) == ParameterRejectionCode::NativeValidationUnavailable);
    brim_context = context_for(brim, false);
    const auto native_result = ParameterProposalValidator().validate(brim, brim_context);
    REQUIRE_FALSE(native_result.accepted());
    CHECK(native_result.rejections.front().code == ParameterRejectionCode::NativeValidationFailed);
    CHECK(native_result.rejections.front().diagnostic_code == "native_fixture_rejected");

    brim_context = context_for(brim);
    const size_t maximum = ParameterPolicyRegistry::current().budget().maximum_serialized_bytes;
    const size_t base = parameter_patch_set_serialized_bytes(brim);
    REQUIRE(base < maximum);
    brim.explanation_codes = {std::string(maximum - base, 'x')};
    CHECK(parameter_patch_set_serialized_bytes(brim) == maximum);
    CHECK(ParameterProposalValidator().validate(brim, brim_context).accepted());
    brim.explanation_codes.front().push_back('x');
    CHECK(rejection(brim, brim_context) == ParameterRejectionCode::SerializedBudgetExceeded);
}

TEST_CASE("per-scope and registered-key PatchSet boundaries replace fixed four-change limit",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    ParameterProposal scoped;
    for (int64_t target = 0; target < 16; ++target)
        scoped.entries.push_back(patch("wall_loops", int64_t{2}, int64_t{3}, target));
    auto scoped_context = context_for(scoped);
    CHECK(ParameterProposalValidator().validate(scoped, scoped_context).accepted());
    scoped.entries.push_back(patch("wall_loops", int64_t{2}, int64_t{3}, 16));
    scoped_context = context_for(scoped);
    CHECK(rejection(scoped, scoped_context) == ParameterRejectionCode::ScopeBudgetExceeded);

    ParameterProposal registered;
    for (int64_t target = 0; target < 16; ++target)
        registered.entries.push_back(patch("wall_loops", int64_t{2}, int64_t{3}, target));
    for (int64_t target = 1; target <= 8; ++target)
        registered.entries.push_back(patch("wall_loops", int64_t{2}, int64_t{3}, target,
                                           ConfigScope::Object));
    for (int64_t target = 1; target <= 2; ++target)
        registered.entries.push_back(patch("fan_max_speed", 50.0, 60.0, target,
                                           ConfigScope::Material, PresetOwner::Filament));
    auto registered_context = context_for(registered);
    for (const ConfigPatchEntry& entry : registered.entries)
        if (entry.scope == ConfigScope::Material)
            add_calibrated_bounds(registered_context, entry, 0.0, 100.0);
    REQUIRE(registered.entries.size() ==
            ParameterPolicyRegistry::current().budget().maximum_registered_changes);
    CHECK(ParameterProposalValidator().validate(registered, registered_context).accepted());
    registered.entries.push_back(patch("fan_max_speed", 50.0, 60.0, 3,
                                       ConfigScope::Material, PresetOwner::Filament));
    registered_context = context_for(registered);
    for (const ConfigPatchEntry& entry : registered.entries)
        if (entry.scope == ConfigScope::Material)
            add_calibrated_bounds(registered_context, entry, 0.0, 100.0);
    CHECK(rejection(registered, registered_context) == ParameterRejectionCode::TooManyChanges);
}

TEST_CASE("latest-context revalidation and repeated validation are deterministic",
          "[AI][SmartSlicing][ParameterPolicy]")
{
    auto value = proposal(patch("brim_width", 0.0, 5.0));
    auto context = context_for(value);
    const auto first = ParameterProposalValidator().validate(value, context);
    const auto second = ParameterProposalValidator().validate(value, context);
    CHECK(first.accepted());
    CHECK(second.accepted());

    context.current_values.front().value = 1.0;
    const auto stale = ParameterProposalValidator().revalidate_for_apply(value, context);
    REQUIRE_FALSE(stale.accepted());
    CHECK(stale.rejections.front().code == ParameterRejectionCode::ExpectedValueMismatch);
    const auto repeated = ParameterProposalValidator().revalidate_for_apply(value, context);
    CHECK(repeated.rejections.front().code == stale.rejections.front().code);
    CHECK(repeated.rejections.front().entry_index == stale.rejections.front().entry_index);
    CHECK(repeated.rejections.front().key == stale.rejections.front().key);
}
