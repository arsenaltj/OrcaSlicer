#include "ParameterPolicy.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace Slic3r::AI::SmartSlicing {
namespace {

using Goals = std::vector<RecommendationGoal>;

Goals all_goals()
{
    return {RecommendationGoal::Balanced, RecommendationGoal::Speed,
            RecommendationGoal::Quality};
}

ParameterPolicyEntry numeric(std::string key, ConfigValueKind kind,
                             std::vector<ConfigScope> scopes, PresetOwner owner,
                             ParameterRiskClass risk, BoundSource bound,
                             NumericPolicyBounds safety, double maximum_change,
                             Goals goals, IntentConflictRule intent,
                             std::string explanation)
{
    ParameterPolicyEntry entry;
    entry.key = std::move(key);
    entry.value_kind = kind;
    entry.scopes = std::move(scopes);
    entry.owner = owner;
    entry.risk_class = risk;
    entry.bound_source = bound;
    entry.allowed_goals = std::move(goals);
    entry.intent_rule = intent;
    entry.explanation_code = std::move(explanation);
    entry.safety_bounds = safety;
    entry.maximum_change = maximum_change;
    return entry;
}

ParameterPolicyEntry boolean(std::string key, std::vector<ConfigScope> scopes,
                             PresetOwner owner, Goals goals,
                             IntentConflictRule intent, std::string explanation)
{
    ParameterPolicyEntry entry;
    entry.key = std::move(key);
    entry.value_kind = ConfigValueKind::Boolean;
    entry.scopes = std::move(scopes);
    entry.owner = owner;
    entry.risk_class = ParameterRiskClass::StrategyControlled;
    entry.bound_source = BoundSource::PolicyRange;
    entry.allowed_goals = std::move(goals);
    entry.intent_rule = intent;
    entry.explanation_code = std::move(explanation);
    return entry;
}

ParameterPolicyEntry enumeration(std::string key, std::vector<ConfigScope> scopes,
                                 PresetOwner owner, std::vector<std::string> values,
                                 Goals goals, IntentConflictRule intent,
                                 std::string explanation)
{
    ParameterPolicyEntry entry;
    entry.key = std::move(key);
    entry.value_kind = ConfigValueKind::Enumeration;
    entry.scopes = std::move(scopes);
    entry.owner = owner;
    entry.risk_class = ParameterRiskClass::StrategyControlled;
    entry.bound_source = BoundSource::PolicyRange;
    entry.allowed_enum_values = std::move(values);
    entry.allowed_goals = std::move(goals);
    entry.intent_rule = intent;
    entry.explanation_code = std::move(explanation);
    return entry;
}

ParameterPolicyEntry immutable(std::string key, ConfigValueKind kind, PresetOwner owner)
{
    ParameterPolicyEntry entry;
    entry.key = std::move(key);
    entry.value_kind = kind;
    entry.scopes = {ConfigScope::Plate, ConfigScope::Material, ConfigScope::Workspace};
    entry.owner = owner;
    entry.risk_class = ParameterRiskClass::ImmutableFact;
    entry.bound_source = BoundSource::ImmutableFact;
    entry.explanation_code = "immutable_calibration_fact";
    return entry;
}

size_t saturating_add(size_t lhs, size_t rhs)
{
    return rhs > std::numeric_limits<size_t>::max() - lhs ?
               std::numeric_limits<size_t>::max() : lhs + rhs;
}

size_t value_bytes(const ConfigValue& value)
{
    if (std::holds_alternative<std::string>(value))
        return saturating_add(1, std::get<std::string>(value).size());
    return 1 + sizeof(uint64_t);
}

} // namespace

ParameterPolicyRegistry::ParameterPolicyRegistry()
    : m_version(PARAMETER_POLICY_VERSION)
{
    const Goals all = all_goals();
    const Goals quality_speed = all;

    m_entries = {
        enumeration("object_orientation_strategy", {ConfigScope::Object}, PresetOwner::Project,
                    {"current", "stable_plane", "min_support", "protect_surface"}, all,
                    IntentConflictRule::PlacementLock, "orientation_strategy"),
        numeric("layer_height", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::ProcessProfile,
                {0.04, 0.40}, 0.12, all, IntentConflictRule::ExplicitParameterOverride,
                "layer_height_strategy"),
        numeric("wall_loops", ConfigValueKind::Integer, {ConfigScope::Plate, ConfigScope::Object},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {1.0, 20.0}, 4.0, all, IntentConflictRule::ExplicitParameterOverride, "wall_strategy"),
        numeric("top_shell_layers", ConfigValueKind::Integer, {ConfigScope::Plate, ConfigScope::Object},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {0.0, 20.0}, 5.0, all, IntentConflictRule::ExplicitParameterOverride, "top_shell_strategy"),
        numeric("bottom_shell_layers", ConfigValueKind::Integer, {ConfigScope::Plate, ConfigScope::Object},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {0.0, 20.0}, 5.0, all, IntentConflictRule::ExplicitParameterOverride, "bottom_shell_strategy"),
        numeric("sparse_infill_density", ConfigValueKind::Floating, {ConfigScope::Plate, ConfigScope::Object},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {0.0, 100.0}, 20.0, all, IntentConflictRule::ExplicitParameterOverride, "infill_density_strategy"),
        enumeration("sparse_infill_pattern", {ConfigScope::Plate, ConfigScope::Object}, PresetOwner::Process,
                    {"grid", "gyroid", "honeycomb", "adaptivecubic", "lines"}, all,
                    IntentConflictRule::ExplicitParameterOverride, "infill_pattern_strategy"),
        boolean("enable_support", {ConfigScope::Plate}, PresetOwner::Process, all,
                IntentConflictRule::SupportPainting, "support_strategy"),
        enumeration("support_type", {ConfigScope::Plate}, PresetOwner::Process,
                    {"normal(auto)", "tree(auto)", "normal(manual)", "tree(manual)"}, all,
                    IntentConflictRule::SupportPainting, "support_type_strategy"),
        numeric("support_interface_top_layers", ConfigValueKind::Integer, {ConfigScope::Plate},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {0.0, 10.0}, 4.0, all, IntentConflictRule::SupportPainting, "support_interface_strategy"),
        numeric("brim_width", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::PolicyRange,
                {0.0, 30.0}, 10.0, all, IntentConflictRule::ExplicitParameterOverride, "adhesion_strategy"),
        numeric("initial_layer_print_height", ConfigValueKind::Floating, {ConfigScope::Plate},
                PresetOwner::Process, ParameterRiskClass::StrategyControlled, BoundSource::ProcessProfile,
                {0.04, 1.0}, 0.30, all, IntentConflictRule::ExplicitParameterOverride,
                "initial_layer_strategy"),
        enumeration("seam_position", {ConfigScope::Plate}, PresetOwner::Process,
                    {"nearest", "aligned", "aligned_back", "back", "random"}, quality_speed,
                    IntentConflictRule::SeamPainting, "seam_strategy"),
        enumeration("wall_sequence", {ConfigScope::Plate}, PresetOwner::Process,
                    {"inner wall/outer wall", "outer wall/inner wall", "inner-outer-inner wall"}, all,
                    IntentConflictRule::ExplicitParameterOverride, "wall_path_strategy"),
        enumeration("print_sequence", {ConfigScope::Plate}, PresetOwner::Process,
                    {"by layer", "by object"}, all, IntentConflictRule::ExplicitParameterOverride,
                    "print_sequence_strategy"),
        enumeration("tool_change_sequence", {ConfigScope::Plate}, PresetOwner::Project,
                    {"current", "minimize_changes", "stable_order"}, all,
                    IntentConflictRule::MulticolorPainting, "tool_change_sequence_strategy"),
        numeric("outer_wall_speed", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::MachineProcessMaterialIntersection,
                {1.0, 1000.0}, 200.0, quality_speed, IntentConflictRule::ExplicitParameterOverride,
                "outer_wall_speed_strategy"),
        numeric("inner_wall_speed", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::MachineProcessMaterialIntersection,
                {1.0, 1000.0}, 300.0, quality_speed, IntentConflictRule::ExplicitParameterOverride,
                "inner_wall_speed_strategy"),
        numeric("sparse_infill_speed", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::MachineProcessMaterialIntersection,
                {1.0, 1000.0}, 400.0, quality_speed, IntentConflictRule::ExplicitParameterOverride,
                "infill_speed_strategy"),
        numeric("default_acceleration", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::StrategyControlled, BoundSource::MachineProcessMaterialIntersection,
                {1.0, 50000.0}, 10000.0, quality_speed, IntentConflictRule::ExplicitParameterOverride,
                "acceleration_strategy"),
        numeric("line_width", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Process,
                ParameterRiskClass::CalibratedRange, BoundSource::CalibratedProfileIntersection,
                {0.05, 2.0}, 0.4, all, IntentConflictRule::ExplicitParameterOverride,
                "calibrated_line_width"),
        numeric("support_base_pattern_spacing", ConfigValueKind::Floating, {ConfigScope::Plate},
                PresetOwner::Process, ParameterRiskClass::CalibratedRange,
                BoundSource::CalibratedProfileIntersection, {0.1, 20.0}, 5.0, all,
                IntentConflictRule::SupportPainting, "calibrated_support_spacing"),
        numeric("fan_max_speed", ConfigValueKind::Floating, {ConfigScope::Material}, PresetOwner::Filament,
                ParameterRiskClass::CalibratedRange, BoundSource::CalibratedProfileIntersection,
                {0.0, 100.0}, 30.0, quality_speed, IntentConflictRule::ExplicitParameterOverride,
                "calibrated_cooling"),
        numeric("retraction_length", ConfigValueKind::Floating, {ConfigScope::Material}, PresetOwner::Filament,
                ParameterRiskClass::CalibratedRange, BoundSource::CalibratedProfileIntersection,
                {0.0, 20.0}, 5.0, all, IntentConflictRule::ExplicitParameterOverride,
                "calibrated_retraction"),
        numeric("flush_multiplier", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Project,
                ParameterRiskClass::CalibratedRange, BoundSource::CalibratedProfileIntersection,
                {0.1, 3.0}, 0.5, all, IntentConflictRule::MulticolorPainting,
                "calibrated_flush_volume"),
        numeric("wipe_tower_width", ConfigValueKind::Floating, {ConfigScope::Plate}, PresetOwner::Project,
                ParameterRiskClass::CalibratedRange, BoundSource::CalibratedProfileIntersection,
                {5.0, 200.0}, 30.0, all, IntentConflictRule::MulticolorPainting,
                "calibrated_wipe_tower"),
        immutable("nozzle_diameter", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("printable_area", ConfigValueKind::Enumeration, PresetOwner::Printer),
        immutable("printable_height", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("nozzle_offset", ConfigValueKind::Enumeration, PresetOwner::Printer),
        immutable("machine_max_acceleration_x", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("machine_max_acceleration_y", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("machine_max_speed_x", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("machine_max_speed_y", ConfigValueKind::Floating, PresetOwner::Printer),
        immutable("nozzle_temperature", ConfigValueKind::Integer, PresetOwner::Filament),
        immutable("nozzle_temperature_range_low", ConfigValueKind::Integer, PresetOwner::Filament),
        immutable("nozzle_temperature_range_high", ConfigValueKind::Integer, PresetOwner::Filament),
        immutable("bed_temperature", ConfigValueKind::Integer, PresetOwner::Filament),
        immutable("filament_max_volumetric_speed", ConfigValueKind::Floating, PresetOwner::Filament),
        immutable("filament_flow_ratio", ConfigValueKind::Floating, PresetOwner::Filament),
        immutable("pressure_advance", ConfigValueKind::Floating, PresetOwner::Filament),
    };

    size_t mutable_count = 0;
    for (const ParameterPolicyEntry& entry : m_entries)
        if (entry.risk_class != ParameterRiskClass::ImmutableFact)
            ++mutable_count;
    m_budget.version = "parameter-patch-budget/v1";
    m_budget.maximum_registered_changes = mutable_count;
}

const ParameterPolicyRegistry& ParameterPolicyRegistry::current()
{
    static const ParameterPolicyRegistry registry;
    return registry;
}

const ParameterPolicyEntry* ParameterPolicyRegistry::find(const std::string& key) const
{
    const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                    [&](const ParameterPolicyEntry& entry) { return entry.key == key; });
    return found == m_entries.end() ? nullptr : &*found;
}

const char* config_value_kind_name(ConfigValueKind kind)
{
    switch (kind) {
    case ConfigValueKind::Boolean: return "boolean";
    case ConfigValueKind::Integer: return "integer";
    case ConfigValueKind::Floating: return "floating";
    case ConfigValueKind::Enumeration: return "enumeration";
    }
    return "unknown";
}

const char* parameter_risk_class_name(ParameterRiskClass risk_class)
{
    switch (risk_class) {
    case ParameterRiskClass::StrategyControlled: return "strategy_controlled";
    case ParameterRiskClass::CalibratedRange: return "calibrated_range";
    case ParameterRiskClass::ImmutableFact: return "immutable_fact";
    }
    return "unknown";
}

const char* bound_source_name(BoundSource source)
{
    switch (source) {
    case BoundSource::PolicyRange: return "policy_range";
    case BoundSource::ProcessProfile: return "process_profile";
    case BoundSource::MachineProcessMaterialIntersection: return "machine_process_material_intersection";
    case BoundSource::CalibratedProfileIntersection: return "calibrated_profile_intersection";
    case BoundSource::ImmutableFact: return "immutable_fact";
    }
    return "unknown";
}

const char* intent_conflict_rule_name(IntentConflictRule rule)
{
    switch (rule) {
    case IntentConflictRule::None: return "none";
    case IntentConflictRule::PlacementLock: return "placement_lock";
    case IntentConflictRule::SupportPainting: return "support_painting";
    case IntentConflictRule::SeamPainting: return "seam_painting";
    case IntentConflictRule::MulticolorPainting: return "multicolor_painting";
    case IntentConflictRule::ExplicitParameterOverride: return "explicit_parameter_override";
    }
    return "unknown";
}

size_t config_scope_index(ConfigScope scope)
{
    switch (scope) {
    case ConfigScope::Plate: return 0;
    case ConfigScope::Object: return 1;
    case ConfigScope::Material: return 2;
    case ConfigScope::Workspace: return 3;
    }
    return 4;
}

size_t parameter_patch_set_serialized_bytes(const ParameterProposal& proposal)
{
    size_t bytes = saturating_add(proposal.policy_version.size(), sizeof(uint32_t));
    bytes = saturating_add(bytes, sizeof(proposal.goal));
    for (const ConfigPatchEntry& entry : proposal.entries) {
        bytes = saturating_add(bytes, sizeof(entry.scope) + sizeof(entry.owner) + sizeof(entry.target_id));
        bytes = saturating_add(bytes, entry.key.size());
        bytes = saturating_add(bytes, value_bytes(entry.expected_value));
        bytes = saturating_add(bytes, value_bytes(entry.new_value));
        bytes = saturating_add(bytes, entry.reason_code.size());
    }
    for (const std::string& explanation : proposal.explanation_codes)
        bytes = saturating_add(bytes, explanation.size());
    return bytes;
}

} // namespace Slic3r::AI::SmartSlicing
