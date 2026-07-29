#include "AIAssistantConfig.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <sstream>
#include <utility>

namespace Slic3r::GUI::AIAssistantConfig {
namespace {

struct AllowedChange
{
    const char*  scope;
    Preset::Type preset_type;
    const char*  key;
};

const std::vector<AllowedChange>& allowed_change_defs()
{
    static const std::vector<AllowedChange> defs = {
        { "print",    Preset::TYPE_PRINT,    "layer_height" },
        { "print",    Preset::TYPE_PRINT,    "wall_loops" },
        { "print",    Preset::TYPE_PRINT,    "sparse_infill_density" },
        { "print",    Preset::TYPE_PRINT,    "top_shell_layers" },
        { "print",    Preset::TYPE_PRINT,    "bottom_shell_layers" },
        { "print",    Preset::TYPE_PRINT,    "enable_support" },
        { "print",    Preset::TYPE_PRINT,    "brim_width" },
        { "print",    Preset::TYPE_PRINT,    "initial_layer_print_height" },
        { "print",    Preset::TYPE_PRINT,    "initial_layer_speed" },
        { "print",    Preset::TYPE_PRINT,    "slow_down_layers" },
        { "print",    Preset::TYPE_PRINT,    "outer_wall_speed" },
        { "print",    Preset::TYPE_PRINT,    "inner_wall_speed" },
        { "print",    Preset::TYPE_PRINT,    "top_surface_speed" },
        { "print",    Preset::TYPE_PRINT,    "sparse_infill_speed" },
        { "print",    Preset::TYPE_PRINT,    "support_top_z_distance" },
        { "print",    Preset::TYPE_PRINT,    "support_bottom_z_distance" },
        { "print",    Preset::TYPE_PRINT,    "support_object_xy_distance" },
        { "print",    Preset::TYPE_PRINT,    "support_interface_top_layers" },
        { "print",    Preset::TYPE_PRINT,    "support_interface_spacing" },
        { "print",    Preset::TYPE_PRINT,    "support_interface_speed" },
        { "filament", Preset::TYPE_FILAMENT, "nozzle_temperature" },
        { "filament", Preset::TYPE_FILAMENT, "bed_temperature" },
        { "filament", Preset::TYPE_FILAMENT, "fan_min_speed" },
        { "filament", Preset::TYPE_FILAMENT, "fan_max_speed" },
        { "filament", Preset::TYPE_FILAMENT, "filament_flow_ratio" },
        { "filament", Preset::TYPE_FILAMENT, "filament_max_volumetric_speed" },
    };
    return defs;
}

const AllowedChange* find_allowed_change(const std::string& scope, const std::string& key)
{
    const auto& defs = allowed_change_defs();
    auto it = std::find_if(defs.begin(), defs.end(), [&scope, &key](const AllowedChange& def) {
        return scope == def.scope && key == def.key;
    });
    return it == defs.end() ? nullptr : &*it;
}

Tab* tab_for_type(Preset::Type type)
{
    return wxGetApp().get_tab(type);
}

std::string value_to_string(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream ss;
        ss << value.get<double>();
        return ss.str();
    }
    return {};
}

json config_values_for_scope(Preset::Type type, const std::string& scope)
{
    json values = json::object();
    Tab* tab = tab_for_type(type);
    if (tab == nullptr || tab->get_config() == nullptr)
        return values;

    const DynamicPrintConfig* config = tab->get_config();
    for (const AllowedChange& def : allowed_change_defs()) {
        if (def.preset_type != type || scope != def.scope)
            continue;
        if (const ConfigOption* option = config->option(def.key); option != nullptr)
            values[def.key] = option->serialize();
    }
    return values;
}

json def_json_for_key(Preset::Type type, const std::string& key)
{
    json out = json::object();
    Tab* tab = tab_for_type(type);
    if (tab == nullptr || tab->get_config() == nullptr || tab->get_config()->def() == nullptr)
        return out;

    const ConfigOptionDef* def = tab->get_config()->def()->get(key);
    if (def == nullptr)
        return out;

    out["type"] = static_cast<int>(def->type);
    out["min"] = def->min;
    out["max"] = def->max;
    if (!def->sidetext.empty())
        out["unit"] = def->sidetext;
    return out;
}

bool proposal_contains_too_many_changes(const json& changes)
{
    constexpr size_t max_changes = 8;
    return !changes.is_array() || changes.size() > max_changes;
}

void add_rejection(ValidationResult& result, const std::string& key, const std::string& reason)
{
    result.rejected.push_back(key.empty() ? reason : key + ": " + reason);
}

bool is_normalized_value_in_range(const ConfigOptionDef& def, const ConfigOption& option)
{
    switch (option.type()) {
    case coFloat:
    case coPercent:
    case coFloatOrPercent:
        return def.is_value_valid(static_cast<const ConfigOptionFloat&>(option).value);
    case coInt:
        return def.is_value_valid(static_cast<const ConfigOptionInt&>(option).value);
    case coFloats:
    case coPercents:
        for (double value : static_cast<const ConfigOptionVector<double>&>(option).values)
            if (!def.is_value_valid(value))
                return false;
        return true;
    case coFloatsOrPercents:
        for (const FloatOrPercent& value : static_cast<const ConfigOptionVector<FloatOrPercent>&>(option).values)
            if (!def.is_value_valid(value.value))
                return false;
        return true;
    case coInts:
        for (int value : static_cast<const ConfigOptionVector<int>&>(option).values)
            if (!def.is_value_valid(value))
                return false;
        return true;
    default:
        return true;
    }
}

json optimization_guidance()
{
    return json::array({
        json::object({
            { "intent", "quality" },
            { "guidance", json::array({
                "Use smaller layer_height for fine detail.",
                "Reduce outer_wall_speed and top_surface_speed for better surface quality.",
                "Increase top_shell_layers and bottom_shell_layers when solid surfaces need fewer gaps."
            }) }
        }),
        json::object({
            { "intent", "strength" },
            { "guidance", json::array({
                "Increase wall_loops before increasing sparse_infill_density for stronger shells.",
                "Use moderate sparse_infill_density increases for load-bearing parts.",
                "Increase nozzle_temperature slightly only when under-extrusion or poor layer bonding is likely."
            }) }
        }),
        json::object({
            { "intent", "speed" },
            { "guidance", json::array({
                "Increase sparse_infill_speed before changing visible-wall speeds.",
                "Keep outer_wall_speed and top_surface_speed conservative when quality is requested.",
                "Respect filament_max_volumetric_speed when suggesting faster printing."
            }) }
        }),
        json::object({
            { "intent", "support" },
            { "guidance", json::array({
                "Enable support only when overhangs or bridges likely need it.",
                "Use support_top_z_distance, support_bottom_z_distance, support_object_xy_distance, support_interface_top_layers, support_interface_spacing, and support_interface_speed to trade removability against underside quality.",
                "Do not suggest changing printer geometry or support-related custom G-code."
            }) }
        }),
        json::object({
            { "intent", "adhesion" },
            { "guidance", json::array({
                "Increase brim_width for warping or small contact patches.",
                "Reduce initial_layer_speed and use slow_down_layers for first-layer reliability.",
                "Adjust bed_temperature only within the filament preset's valid range."
            }) }
        }),
        json::object({
            { "intent", "filament" },
            { "guidance", json::array({
                "Use filament_flow_ratio for small extrusion compensation, not as a substitute for calibration.",
                "Use fan_min_speed and fan_max_speed to balance cooling, detail, and layer bonding.",
                "Prefer calibration advice in assistant_text when a parameter change would be speculative."
            }) }
        })
    });
}

} // namespace

json build_context(const Plater& plater, const std::string& user_message, const std::string& request_id)
{
    json context = json::object();
    context["schema_version"] = 1;
    context["request_id"] = request_id;
    context["user_message"] = user_message;
    context["app"] = json::object({ { "name", "OrcaSlicer" } });

    context["model"] = json::object({
        { "object_count", plater.model().objects.size() },
        { "has_printable_object", !plater.model().objects.empty() }
    });

    context["config"] = json::object({
        { "print", config_values_for_scope(Preset::TYPE_PRINT, "print") },
        { "filament", config_values_for_scope(Preset::TYPE_FILAMENT, "filament") }
    });
    context["allowed_changes"] = allowed_changes();
    context["optimization_guidance"] = optimization_guidance();
    context["safety"] = json::object({
        { "no_mesh", true },
        { "no_gcode", true },
        { "no_paths", true },
        { "user_confirmation_required", true }
    });

    return context;
}

json allowed_changes()
{
    json scopes = json::object();
    for (const AllowedChange& def : allowed_change_defs()) {
        if (!scopes.contains(def.scope))
            scopes[def.scope] = json::object({ { "keys", json::object() } });
        scopes[def.scope]["keys"][def.key] = def_json_for_key(def.preset_type, def.key);
    }
    return json::object({ { "scopes", scopes } });
}

ValidationResult validate_proposal(const json& proposal)
{
    ValidationResult result;

    if (!proposal.is_object()) {
        add_rejection(result, {}, "Proposal is not a JSON object.");
        return result;
    }

    const json* changes = nullptr;
    if (proposal.contains("changes"))
        changes = &proposal["changes"];
    else if (proposal.contains("proposal") && proposal["proposal"].is_object() && proposal["proposal"].contains("changes"))
        changes = &proposal["proposal"]["changes"];

    if (changes == nullptr || proposal_contains_too_many_changes(*changes)) {
        add_rejection(result, {}, "Proposal must contain between 1 and 8 changes.");
        return result;
    }

    std::set<std::pair<std::string, std::string>> seen;
    for (const json& change : *changes) {
        if (!change.is_object()) {
            add_rejection(result, {}, "Change entry is not a JSON object.");
            continue;
        }

        const std::string scope = change.value("scope", std::string());
        const std::string key = change.value("key", std::string());
        const json* new_value_json = change.contains("new_value") ? &change["new_value"] :
                                     change.contains("value") ? &change["value"] : nullptr;
        const std::string new_value = new_value_json != nullptr ? value_to_string(*new_value_json) : std::string();
        const std::string reason = change.value("reason", std::string());

        if (scope.empty() || key.empty() || new_value.empty()) {
            add_rejection(result, key, "Change must include scope, key, and new_value.");
            continue;
        }

        if (!seen.emplace(scope, key).second) {
            add_rejection(result, key, "Duplicate change.");
            continue;
        }

        const AllowedChange* allowed = find_allowed_change(scope, key);
        if (allowed == nullptr) {
            add_rejection(result, key, "Setting is not allowlisted for AI changes.");
            continue;
        }

        Tab* tab = tab_for_type(allowed->preset_type);
        if (tab == nullptr || tab->get_config() == nullptr) {
            add_rejection(result, key, "Preset tab is not available.");
            continue;
        }

        const DynamicPrintConfig* current_config = tab->get_config();
        const ConfigDef* config_def = current_config->def();
        const ConfigOptionDef* option_def = config_def != nullptr ? config_def->get(key) : nullptr;
        if (option_def == nullptr) {
            add_rejection(result, key, "Setting is not defined in the current configuration.");
            continue;
        }

        const ConfigOption* old_option = current_config->option(key);
        const std::string old_value = old_option != nullptr ? old_option->serialize() : std::string();

        DynamicPrintConfig test_config(*current_config);
        try {
            test_config.set_deserialize_strict(key, new_value);
        } catch (const std::exception& e) {
            add_rejection(result, key, std::string("Invalid value: ") + e.what());
            continue;
        }

        const ConfigOption* normalized_option = test_config.option(key);
        if (normalized_option == nullptr) {
            add_rejection(result, key, "Value did not produce a valid config option.");
            continue;
        }

        if (!is_normalized_value_in_range(*option_def, *normalized_option)) {
            add_rejection(result, key, "Value is outside the allowed range.");
            continue;
        }

        const std::string normalized_value = normalized_option->serialize();
        if (normalized_value == old_value) {
            add_rejection(result, key, "Proposed value is already active.");
            continue;
        }

        result.accepted.push_back(ValidatedChange{
            allowed->preset_type,
            scope,
            key,
            old_value,
            normalized_value,
            reason
        });
    }

    return result;
}

DynamicPrintConfig build_patch_config(const std::vector<ValidatedChange>& changes)
{
    DynamicPrintConfig patch;
    for (const ValidatedChange& change : changes) {
        try {
            patch.set_deserialize_strict(change.key, change.new_value);
        } catch (const std::exception&) {
        }
    }
    return patch;
}

json validation_result_to_json(const ValidationResult& result)
{
    json accepted = json::array();
    for (const ValidatedChange& change : result.accepted) {
        accepted.push_back(json::object({
            { "scope", change.scope },
            { "key", change.key },
            { "old_value", change.old_value },
            { "new_value", change.new_value },
            { "reason", change.reason }
        }));
    }

    return json::object({
        { "accepted", accepted },
        { "rejected", result.rejected }
    });
}

} // namespace Slic3r::GUI::AIAssistantConfig
