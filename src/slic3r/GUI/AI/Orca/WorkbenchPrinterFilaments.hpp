#pragma once

#include "slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include "libslic3r/Print.hpp"
#include "FilamentColorPack.hpp"
#include "WorkbenchFilamentRecipes.hpp"
#include <functional>
#include <optional>
#include <numeric>
#include <cctype>
#include <set>

namespace Slic3r::GUI::PrinterFilamentConfig {

inline GUI::WorkbenchPrinterFilaments capture(const PresetBundle& bundle)
{
    GUI::WorkbenchPrinterFilaments state;
    state.printer = bundle.printers.get_edited_preset().name;
    const auto& printer = bundle.printers.get_edited_preset().config;
    const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    state.nozzles = nozzles ? nozzles->values.size() : 0;
    state.physical_capacity = state.nozzles > 1 && !printer.opt_bool("single_extruder_multi_material") &&
        !bundle.is_bbl_vendor() ? state.nozzles : 32;
    nlohmann::json identity = {{"printer", state.printer}, {"nozzles", nozzles ? nozzles->serialize() : ""},
        {"slots", nlohmann::json::array()}};
    const auto* colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const size_t count = bundle.filament_presets.size();
    std::vector<std::string> types;
    std::vector<int> temperatures, lows, highs;
    for (size_t i = 0; i < count; ++i) {
        const bool mixed = bundle.is_mixed_filament(i);
        identity["slots"].push_back({i, mixed, bundle.filament_presets[i]});
        if (mixed) {
            WorkbenchMixedFilament recipe;
            recipe.slot = i;
            recipe.color = colors && i < colors->values.size() ? colors->values[i] : "";
            recipe.valid = WorkbenchFilamentRecipes::valid_recipe(bundle, i);
            const auto ids = parse_mixed_components(bundle.project_config.opt_string("filament_mixed_components", i));
            const auto ratios = parse_mixed_ratios(bundle.project_config.opt_string("filament_mixed_sublayer_ratios", i), ids.size());
            for (size_t j = 0; j < ids.size(); ++j) {
                if (j) recipe.recipe += " + ";
                recipe.recipe += std::to_string(ids[j]) + "号 " + std::to_string(int(std::lround(ratios[j] * 100))) + "%";
            }
            if (!recipe.valid) recipe.reason = "配方引用、材料或温度不兼容";
            state.mixed.push_back(std::move(recipe));
            continue;
        }
        WorkbenchFilamentSlot slot;
        slot.slot = i; slot.exists = true; slot.preset = bundle.filament_presets[i];
        slot.color = colors && i < colors->values.size() ? colors->values[i] : "";
        std::transform(slot.color.begin(), slot.color.end(), slot.color.begin(),
            [](unsigned char c) { return char(std::toupper(c)); });
        slot.removable = bundle.can_remove_physical_filament(i);
        const auto* preset = bundle.filaments.find_preset(slot.preset, false);
        if (preset) {
            const auto* type = preset->config.option<ConfigOptionStrings>("filament_type");
            if (type && !type->values.empty()) slot.material = type->get_at(0);
            slot.compatible = preset->is_compatible;
            for (auto item : {std::make_pair("nozzle_temperature", &temperatures),
                              std::make_pair("nozzle_temperature_range_low", &lows),
                              std::make_pair("nozzle_temperature_range_high", &highs)}) {
                const auto* option = preset->config.option<ConfigOptionInts>(item.first);
                item.second->push_back(option && !option->values.empty() ? option->get_at(0) : 0);
            }
        }
        slot.configured = preset && AI::is_rgb_hex_color(slot.color) && !slot.material.empty();
        slot.reason = !slot.configured ? "待配置耗材预设和装料颜色" : !slot.compatible ? "耗材与当前打印机不兼容" : "";
        types.push_back(slot.material);
        state.slots.push_back(std::move(slot));
    }
    state.physical_count = state.slots.size();
    state.idle_nozzles = state.nozzles > state.physical_count ? state.nozzles - state.physical_count : 0;
    // Actual loaded color/material changes also retire an old match: manual
    // overlays carry appearance, not native slot references.
    for (const auto& slot : state.slots) identity["loaded"].push_back({slot.slot, slot.color, slot.material, slot.compatible});
    for (const auto& recipe : state.mixed) identity["recipes"].push_back({recipe.slot, recipe.color, recipe.recipe, recipe.valid});
    state.identity = identity.dump();
    nlohmann::json signature = identity;
    for (const auto& key : bundle.project_config.keys())
        signature["project"][key] = bundle.project_config.option(key)->serialize();
    for (const auto& slot : state.slots) {
        signature["compatible"].push_back(slot.compatible);
        if (const auto* preset = bundle.filaments.find_preset(slot.preset, false))
            for (const auto& key : preset->config.keys()) signature["materials"][slot.preset][key] = preset->config.option(key)->serialize();
    }
    state.signature = signature.dump();
    if (!state.nozzles) state.reason = "请选择 FFF 打印机";
    else if (!state.physical_count) state.reason = "请配置至少一个真实耗材";
    else if (state.physical_count > state.physical_capacity) state.reason = "实体耗材超过喷嘴容量，请减少实体槽或导入色卡自动生成叠色";
    else if (count > size_t(EnforcerBlockerType::ExtruderMax)) state.reason = "原生涂色和预览最多支持 32 个槽位";
    else for (const auto& slot : state.slots) if (!slot.configured || !slot.compatible) { state.reason = slot.reason; break; }
    if (state.reason.empty() && state.nozzles == 1 && types.size() > 1 &&
        Print::check_multi_filaments_compatibility(types, temperatures, lows, highs) != FilamentCompatibilityType::Compatible)
        state.reason = "耗材材料或温度不能在当前单喷嘴打印机上共同使用";
    state.ready = state.reason.empty();
    return state;
}

// Use native sizing/purge normalization, preserving metadata on existing slots.
inline void resize(PresetBundle& staged, size_t count)
{
    const auto multi = staged.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
    staged.set_num_filaments(unsigned(count));
    auto& restored = staged.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
    std::copy_n(multi.begin(), std::min(multi.size(), restored.size()), restored.begin());
}

// Insert before virtual materials, just like Sidebar::add_custom_filament.
// Return the insertion index so the native host can shift model references in
// the same undo transaction. RGB is deliberately irrelevant to slot identity.
inline size_t add(PresetBundle& staged)
{
    const size_t old = staged.filament_presets.size();
    size_t at = old;
    for (size_t i = 0; i < old; ++i) if (staged.is_mixed_filament(i)) { at = i; break; }
    resize(staged, old + 1);
    std::rotate(staged.filament_presets.begin() + at, staged.filament_presets.begin() + old, staged.filament_presets.end());
    for (const auto* key : {"filament_colour", "filament_multi_colour", "filament_colour_type", "filament_mixed_components",
         "filament_mixed_sublayer_ratios", "filament_mixed_gradient_range", "filament_mixed_gradient_curve"}) {
        auto& values = staged.project_config.option<ConfigOptionStrings>(key)->values;
        std::rotate(values.begin() + at, values.begin() + old, values.end());
    }
    for (const auto* key : {"filament_is_mixed", "filament_mixed_gradient", "filament_mixed_gradient_per_part"}) {
        auto& values = staged.project_config.option<ConfigOptionBools>(key)->values;
        std::rotate(values.begin() + at, values.begin() + old, values.end());
    }
    for (const auto* key : {"filament_map", "filament_nozzle_map", "filament_volume_map"}) {
        auto& values = staged.project_config.option<ConfigOptionInts>(key)->values;
        std::rotate(values.begin() + at, values.begin() + old, values.end());
    }
    const size_t count = old + 1;
    auto permutation = [at, old](size_t i) { return i == at ? old : i > at ? i - 1 : i; };
    auto& matrix = staged.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    const auto previous = matrix;
    for (size_t nozzle = 0; nozzle < matrix.size() / (count * count); ++nozzle)
        for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < count; ++j)
            matrix[nozzle * count * count + i * count + j] = previous[nozzle * count * count + permutation(i) * count + permutation(j)];
    auto& volumes = staged.project_config.option<ConfigOptionFloats>("flush_volumes_vector")->values;
    const auto old_volumes = volumes;
    for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < 2; ++j) volumes[i * 2 + j] = old_volumes[permutation(i) * 2 + j];
    auto& components = staged.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values;
    for (size_t i = 0; i < count; ++i) if (staged.is_mixed_filament(i)) {
        auto ids = parse_mixed_components(components[i]); std::string text;
        for (auto id : ids) {
            if (id > at) ++id;
            if (!text.empty()) text += ',';
            text += std::to_string(id);
        }
        components[i] = std::move(text);
    }
    staged.filament_presets[at].clear();
    staged.project_config.option<ConfigOptionStrings>("filament_colour")->values[at].clear();
    return at;
}

inline bool remove(PresetBundle& staged, size_t slot, size_t replacement, std::string& error)
{
    const auto state = capture(staged);
    const auto source = std::find_if(state.slots.begin(), state.slots.end(), [slot](const auto& s) { return s.slot == slot; });
    const auto target = std::find_if(state.slots.begin(), state.slots.end(), [replacement](const auto& s) { return s.slot == replacement; });
    if (source == state.slots.end() || !source->removable || slot == replacement ||
        target == state.slots.end() || !target->exists || !target->configured || !target->compatible) {
        error = "至少保留一个实体耗材；请选择已配置且兼容的实体替代耗材"; return false;
    }
    // Resolve every recipe before shrinking the native arrays. Duplicated mix
    // components are unsafe (including gradients), and are rejected by validation.
    PresetBundle candidate(staged);
    auto& components = candidate.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values;
    for (size_t i = 0; i < components.size(); ++i) if (staged.is_mixed_filament(i)) {
        auto ids = parse_mixed_components(components[i]);
        std::string text;
        for (auto& id : ids) {
            if (id == slot + 1) id = unsigned(replacement + 1);
            if (!text.empty()) text += ',';
            text += std::to_string(id);
        }
        if (std::set<unsigned>(ids.begin(), ids.end()).size() != ids.size()) {
            error = "此替代耗材会使混色配方组件重复，无法安全删除"; return false;
        }
        components[i] = std::move(text);
    }
    candidate.update_num_filaments(unsigned(slot));
    // Drafts may still have pending material choices. Validate array sizes
    // and mixed references with installed names, without filling those
    // choices in the actual draft. Completion separately requires readiness.
    auto validation_presets = candidate.filament_presets;
    for (auto& name : validation_presets)
        if (!candidate.filaments.find_preset(name, false)) name = target->preset;
    if (!ProjectConfigRestore::validate(candidate.project_config, validation_presets, candidate, error)) return false;
    staged.project_config = std::move(candidate.project_config);
    staged.filament_presets = std::move(candidate.filament_presets);
    return true;
}

using SlotChange = PrinterFilamentSlotRemap::Change;
using ChooseReplacement = std::function<std::optional<size_t>(const WorkbenchFilamentSlot&,
    const std::vector<WorkbenchFilamentSlot>&)>;

// Shared by the real dialog and regressions. All edits, including structural
// changes and replacement choices, publish to the draft only after success.
inline bool import_color_pack(PresetBundle& staged, const FilamentColorPack& pack,
    std::vector<SlotChange>& changes, std::string& error, const ChooseReplacement& choose = {})
{
    error.clear();
    const size_t limit = size_t(EnforcerBlockerType::ExtruderMax);
    if (pack.colors.empty() || pack.colors.size() > limit ||
        !std::all_of(pack.colors.begin(), pack.colors.end(), AI::is_rgb_hex_color) ||
        (!pack.presets.empty() && pack.presets.size() != pack.colors.size()) ||
        (!pack.multi_colors.empty() && pack.multi_colors.size() != pack.colors.size()) ||
        (!pack.color_types.empty() && pack.color_types.size() != pack.colors.size())) {
        error = "色卡颜色或槽位数据无效，未导入"; return false;
    }
    PresetBundle candidate(staged);
    auto pending_changes = changes;
    auto state = capture(candidate);
    const auto bases = WorkbenchFilamentRecipes::select_bases(pack.colors, state.physical_capacity);
    const size_t target = bases.size();
    if (!state.nozzles || target > limit || candidate.filament_presets.size() > limit ||
        (target > state.physical_count && target - state.physical_count > limit - candidate.filament_presets.size())) {
        error = "实体与混色槽位总数无法容纳此色卡，未导入"; return false;
    }
    std::string reusable_preset;
    for (const auto& slot : state.slots)
        if (slot.exists && slot.compatible) reusable_preset = slot.preset;
    while (state.physical_count < target) {
        const size_t before = state.physical_count;
        const size_t at = add(candidate);
        if (pack.presets.empty()) candidate.filament_presets[at] = reusable_preset;
        state = capture(candidate);
        if (state.physical_count != before + 1) {
            error = "无法增加实体耗材槽位，配置草稿保持原状"; return false;
        }
        pending_changes.push_back({{at}, -1, -1});
    }
    for (size_t i = 0; i < bases.size(); ++i) {
        const size_t source_index = bases[i];
        const size_t at = state.slots[i].slot;
        auto& name = candidate.filament_presets[at];
        const std::string desired = pack.presets.empty() ? name : pack.presets[source_index];
        const auto* preset = candidate.filaments.find_preset(desired, false);
        name = preset && preset->is_compatible ? desired : "";
        candidate.project_config.option<ConfigOptionStrings>("filament_colour")->values[at] = pack.colors[source_index];
        candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[at] =
            pack.multi_colors.empty() ? pack.colors[source_index] : pack.multi_colors[source_index];
        candidate.project_config.option<ConfigOptionStrings>("filament_colour_type")->values[at] =
            pack.color_types.empty() ? "1" : pack.color_types[source_index];
    }
    while ((state = capture(candidate)).physical_count > target) {
        const auto& source = state.slots.back();
        if (!source.exists || !source.removable) {
            error = "无法安全减少实体耗材槽位，请先处理额外槽位的引用"; return false;
        }
        std::vector<WorkbenchFilamentSlot> replacements;
        // Retain only targets that survive the entire import. Native removal
        // checks every virtual recipe, including duplicate components.
        for (size_t i = 0; i < target; ++i) {
            const auto& slot = state.slots[i];
            if (!slot.exists || !slot.configured || !slot.compatible) continue;
            PresetBundle probe(candidate); std::string reason;
            if (remove(probe, source.slot, slot.slot, reason)) replacements.push_back(slot);
        }
        if (replacements.empty()) {
            error = "没有合法替代耗材，无法按色卡减少槽位；请先配置兼容预设或调整混色配方"; return false;
        }
        // Even an unconfigured slot may be referenced by a model. The UI
        // always asks for an explicit replacement; cancelling is atomic.
        const auto replacement = choose ? choose(source, replacements) : std::nullopt;
        if (!replacement) { error = "已取消减少槽位，配置草稿保持原状"; return false; }
        if (std::none_of(replacements.begin(), replacements.end(), [&](const auto& slot) { return slot.slot == *replacement; }) ||
            !remove(candidate, source.slot, *replacement, error)) {
            if (error.empty()) error = "替代耗材无效，配置草稿保持原状";
            return false;
        }
        if (capture(candidate).physical_count != state.physical_count - 1) {
            error = "无法减少实体耗材槽位，配置草稿保持原状"; return false;
        }
        pending_changes.push_back({{}, int(source.slot), int(*replacement)});
    }
    if (pack.colors.size() > target && capture(candidate).ready) {
        std::vector<size_t> bindings;
        const size_t old_count = candidate.filament_presets.size();
        if (!WorkbenchFilamentRecipes::stage_targets(candidate, pack.colors, bindings, error)) return false;
        for (size_t at = old_count; at < candidate.filament_presets.size(); ++at)
            pending_changes.push_back({{at}, -1, -1});
    }
    staged.project_config = std::move(candidate.project_config);
    staged.filament_presets = std::move(candidate.filament_presets);
    changes = std::move(pending_changes);
    return true;
}
} // namespace Slic3r::GUI::PrinterFilamentConfig
