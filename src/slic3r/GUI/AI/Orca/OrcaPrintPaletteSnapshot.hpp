#pragma once

#include "OrcaPaletteSnapshotBuilder.hpp"
#include "OrcaPrintColorProcess.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <utility>

namespace Slic3r::GUI::OrcaPrintPaletteSnapshot {
namespace detail {
inline std::string snapshot_hash(const nlohmann::json& snapshot)
{
    const auto bytes = snapshot.dump();
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int count = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &count, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Unable to fingerprint the print configuration.");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(count * 2);
    for (unsigned int i = 0; i < count; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}
} // namespace detail

// Read-only capture of a specified native bundle and plate routing. The live
// adapter supplies its effective displayed colors; staged transactions can use
// the bundle overload below. Keep the existing fingerprint payload unchanged
// so extracting this function does not invalidate saved confirmations.
inline AI::PrintablePaletteSnapshot capture(const PresetBundle* bundle,
    std::vector<std::string> project_colors, const PrintColorNozzleRouting& nozzle_routing={})
{
    const auto* mixed_flags = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
    const auto* mixed_components = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionStrings>("filament_mixed_components");
    const auto* mixed_ratios = bundle == nullptr
        ? nullptr : bundle->project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");

    std::vector<OrcaPaletteSlotCapability> capabilities;
    capabilities.reserve(project_colors.size());
    for (size_t slot = 0; slot < project_colors.size(); ++slot) {
        std::string color = project_colors[slot];
        std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        const bool is_mixed = mixed_flags != nullptr && slot < mixed_flags->values.size() &&
                              mixed_flags->values[slot];
        OrcaPaletteSlotCapability capability {slot, color, {}, is_mixed, true, {}};
        if (is_mixed && mixed_components != nullptr && slot < mixed_components->values.size()) {
            const std::vector<unsigned int> component_ids = parse_mixed_components(mixed_components->values[slot]);
            const std::vector<double> ratios = parse_mixed_ratios(
                mixed_ratios != nullptr && slot < mixed_ratios->values.size() ? mixed_ratios->values[slot] : "",
                component_ids.size());
            for (size_t index = 0; index < component_ids.size() && index < ratios.size(); ++index) {
                if (component_ids[index] == 0) {
                    capability.mixed_components.clear();
                    break;
                }
                capability.mixed_components.push_back({component_ids[index] - 1, ratios[index]});
            }
        }
        capabilities.push_back(std::move(capability));
    }

    const std::vector<size_t> physical_slots = select_model_generation_physical_slots(capabilities);
    struct SlotTemperature {
        std::string type;
        int         temperature { 0 };
        int         range_low { 0 };
        int         range_high { 0 };
    };
    std::vector<SlotTemperature> slot_temperatures(physical_slots.size());
    bool metadata_complete = bundle != nullptr;
    for (size_t index = 0; index < physical_slots.size(); ++index) {
        const size_t slot = physical_slots[index];
        const Preset* preset = bundle != nullptr && slot < bundle->filament_presets.size()
            ? bundle->filaments.find_preset(bundle->filament_presets[slot]) : nullptr;
        if (preset == nullptr) {
            metadata_complete = false;
            continue;
        }
        const auto* types = preset->config.option<ConfigOptionStrings>("filament_type");
        const auto* temperatures = preset->config.option<ConfigOptionInts>("nozzle_temperature");
        const auto* range_lows = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_low");
        const auto* range_highs = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_high");
        auto capability = std::find_if(capabilities.begin(), capabilities.end(), [slot](const auto& candidate) {
            return !candidate.is_mixed && candidate.slot == slot;
        });
        if (types != nullptr && !types->values.empty() && capability != capabilities.end())
            capability->material_type = types->get_at(0);
        if (types == nullptr || types->values.empty() || temperatures == nullptr || temperatures->values.empty() ||
            range_lows == nullptr || range_lows->values.empty() || range_highs == nullptr || range_highs->values.empty()) {
            metadata_complete = false;
            continue;
        }
        slot_temperatures[index] =
            {types->get_at(0), temperatures->get_at(0), range_lows->get_at(0), range_highs->get_at(0)};
    }

    std::vector<size_t> compatible_slots = physical_slots;
    if (physical_slots.size() >= 2 && metadata_complete) {
        std::vector<size_t> best {physical_slots.front()};
        const uint32_t subset_count = uint32_t(1) << physical_slots.size();
        for (uint32_t mask = 1; mask < subset_count; ++mask) {
            std::vector<size_t> slots;
            std::vector<std::string> selected_types;
            std::vector<int> selected_temperatures;
            std::vector<int> selected_lows;
            std::vector<int> selected_highs;
            for (size_t bit = 0; bit < physical_slots.size(); ++bit) {
                if ((mask & (uint32_t(1) << bit)) == 0)
                    continue;
                slots.push_back(physical_slots[bit]);
                selected_types.push_back(slot_temperatures[bit].type);
                selected_temperatures.push_back(slot_temperatures[bit].temperature);
                selected_lows.push_back(slot_temperatures[bit].range_low);
                selected_highs.push_back(slot_temperatures[bit].range_high);
            }
            if (slots.size() > best.size() &&
                Print::check_multi_filaments_compatibility(selected_types, selected_temperatures, selected_lows,
                                                           selected_highs) == FilamentCompatibilityType::Compatible)
                best = std::move(slots);
        }
        compatible_slots = std::move(best);
    }

    for (OrcaPaletteSlotCapability& capability : capabilities) {
        if (!capability.is_mixed)
            capability.compatible = std::find(compatible_slots.begin(), compatible_slots.end(), capability.slot) !=
                                    compatible_slots.end();
    }
    AI::PrintablePaletteSnapshot snapshot = build_orca_palette_snapshot(capabilities, metadata_complete);
    nlohmann::json materials = {{"colors", project_colors}, {"slots", nlohmann::json::array()}};
    nlohmann::json process = nlohmann::json::object();
    if (bundle != nullptr) {
        for (size_t slot : physical_slots) {
            nlohmann::json item = {{"slot", slot}};
            const auto* preset = slot < bundle->filament_presets.size()
                ? bundle->filaments.find_preset(bundle->filament_presets[slot]) : nullptr;
            if (preset != nullptr) {
                item["preset"] = preset->name;
                for (const auto& key : preset->config.keys())
                    if (const auto* option = preset->config.option(key)) item["settings"][key] = option->serialize();
            }
            materials["slots"].push_back(std::move(item));
        }
        // Print settings contain no device connection secrets. Conservatively
        // invalidate on every process change, including sublayer definitions.
        const auto& print = bundle->prints.get_edited_preset().config;
        for (const auto& key : print.keys())
            if (const auto* option = print.option(key)) process["print"][key] = option->serialize();
        const auto& printer = bundle->printers.get_edited_preset().config;
        for (const auto* key : {"printer_model", "nozzle_diameter", "min_layer_height", "max_layer_height",
                               "extruder_type", "single_extruder_multi_material", "printable_area"})
            if (const auto* option = printer.option(key)) process["printer"][key] = option->serialize();
        const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
        if (nozzles && nozzles->values.size() > 1) {
            // Plate mapping can change independently of the global presets.
            // Bind it so a candidate cannot survive a different manual route.
            process["routing"] = {{"mode", int(nozzle_routing.mode)}, {"filament_maps", nozzle_routing.filament_maps}};
        }
        for (const auto* key : {"filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios"})
            if (const auto* option = bundle->project_config.option(key)) process["mix"][key] = option->serialize();
    }
    snapshot.material_fingerprint = detail::snapshot_hash(materials);
    snapshot.process_fingerprint = detail::snapshot_hash(process);
    snapshot.material_metadata_complete = metadata_complete;
    if(bundle)for(auto& recipe:snapshot.mixed_recipes) {
        nlohmann::json settings=nlohmann::json::object();const size_t slot=*recipe.existing_virtual_slot;
        for(const auto* key:{"filament_mixed_components","filament_mixed_sublayer_ratios","filament_mixed_gradient_range","filament_mixed_gradient_curve"}) {
            const auto* option=bundle->project_config.option<ConfigOptionStrings>(key);
            settings[key]=option && slot<option->values.size()?option->values[slot]:"";
        }
        for(const auto* key:{"filament_mixed_gradient","filament_mixed_gradient_per_part"}) {
            const auto* option=bundle->project_config.option<ConfigOptionBools>(key);
            settings[key]=option && slot<option->values.size() && option->values[slot];
        }
        recipe.native_settings_fingerprint=detail::snapshot_hash(settings);
        recipe.uniform_color=!settings.at("filament_mixed_gradient").get<bool>() &&
                             !settings.at("filament_mixed_gradient_per_part").get<bool>();
    }
    if (bundle != nullptr) {
        std::vector<const DynamicPrintConfig*> configs;
        std::vector<std::string> identities;
        for (const auto& channel : snapshot.physical_channels) {
            const auto* preset = channel.slot < bundle->filament_presets.size()
                ? bundle->filaments.find_preset(bundle->filament_presets[channel.slot]) : nullptr;
            configs.push_back(preset ? &preset->config : nullptr);
            nlohmann::json identity = {{"slot", channel.slot}};
            if (preset) {
                identity["preset"] = preset->name;
                for (const auto& key : preset->config.keys())
                    if (const auto* option = preset->config.option(key)) identity["settings"][key] = option->serialize();
            }
            identities.push_back(preset ? detail::snapshot_hash(identity) : "");
        }
        capture_print_color_process(snapshot, bundle->printers.get_edited_preset().config,
            bundle->prints.get_edited_preset().config, configs, identities, &nozzle_routing);
    }
    // Preserve the raw all-slot projection for the legacy manual matcher. Typed
    // consumers use physical_channels and mixed_recipes, which remain separated.
    snapshot.project_colors = std::move(project_colors);
    return snapshot;
}

inline AI::PrintablePaletteSnapshot capture(const PresetBundle& bundle,
    const PrintColorNozzleRouting& nozzle_routing={})
{
    const auto* colors=bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    return capture(&bundle,colors ? colors->values : std::vector<std::string>{},nozzle_routing);
}
} // namespace Slic3r::GUI::OrcaPrintPaletteSnapshot
