#pragma once

#include "slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include "libslic3r/Print.hpp"
#include "FilamentColorPack.hpp"
#include "WorkbenchFilamentRecipes.hpp"
#include <functional>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <numeric>
#include <cctype>
#include <set>

namespace Slic3r::GUI::PrinterFilamentConfig {

inline constexpr int workbench_row_height_dip = 28;

inline std::optional<size_t> mixed_panel_index(
    const std::vector<size_t>& mixed_config_indices, size_t config_slot)
{
    const auto found = std::find(mixed_config_indices.begin(), mixed_config_indices.end(), config_slot);
    if (found == mixed_config_indices.end()) return {};
    return size_t(std::distance(mixed_config_indices.begin(), found));
}

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
    nlohmann::json layout = {{"slots", nlohmann::json::array()}};
    const auto* colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const size_t count = bundle.filament_presets.size();
    std::vector<std::string> types;
    std::vector<int> temperatures, lows, highs;
    for (size_t i = 0; i < count; ++i) {
        const bool mixed = bundle.is_mixed_filament(i);
        identity["slots"].push_back({i, mixed, bundle.filament_presets[i]});
        layout["slots"].push_back({i, mixed});
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
    // The settings rail uses this value to decide whether its native-style
    // rows must be rebuilt. Colors, material presets and recipe ratios are
    // deliberately excluded so ordinary edits can update controls in place.
    state.layout = layout.dump();
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

inline std::optional<size_t> nearest_replacement_slot(const std::string& source_color,
    const std::vector<WorkbenchFilamentSlot>& targets)
{
    std::optional<size_t> best;
    double best_distance = std::numeric_limits<double>::max();
    const bool source_valid = AI::is_rgb_hex_color(source_color);
    for (const auto& target : targets) {
        if (!target.exists || !target.configured || !target.compatible) continue;
        double distance = std::numeric_limits<double>::max();
        if (source_valid && AI::is_rgb_hex_color(target.color)) {
            try { distance = WorkbenchFilamentRecipes::distance(source_color, target.color); }
            catch (...) { distance = std::numeric_limits<double>::max(); }
        }
        if (!best || distance < best_distance - 1e-7 ||
            (std::abs(distance - best_distance) <= 1e-7 && target.slot < *best)) {
            best = target.slot;
            best_distance = distance;
        }
    }
    return best;
}

// The workbench application path receives a logical card directly, without
// opening the staged configuration dialog.  It therefore needs the same
// physical-slot reduction that card import performs before mixed-slot cleanup.
// Always use the native remove/validation path so object, painting and recipe
// references are remapped atomically by the caller's undo transaction.
inline bool trim_unused_physical_slots(PresetBundle& staged, std::vector<size_t>& bindings,
    size_t desired_physical, std::vector<PrinterFilamentSlotRemap::Change>& changes, std::string& error)
{
    while (true) {
        const auto state = capture(staged);
        if (state.physical_count <= desired_physical) return true;
        if (state.slots.empty()) {
            error = "色卡切换后没有可保留的实体耗材槽位";
            return false;
        }
        const auto source = state.slots.back();
        std::optional<size_t> replacement;
        auto try_replacement = [&](size_t slot) {
            if (replacement || slot == source.slot) return;
            const auto candidate = std::find_if(state.slots.begin(), state.slots.end(),
                [slot](const auto& item) { return item.slot == slot && item.configured && item.compatible; });
            if (candidate == state.slots.end()) return;
            PresetBundle probe(staged);
            std::string reason;
            if (remove(probe, source.slot, slot, reason)) replacement = slot;
        };
        // Prefer an identity already used by the current logical card, then
        // fall back to any compatible physical slot.
        for (const size_t slot : bindings) try_replacement(slot);
        for (const auto& slot : state.slots) try_replacement(slot.slot);
        if (!replacement || !remove(staged, source.slot, *replacement, error)) {
            if (error.empty()) error = "没有合法替代耗材，无法按当前色卡减少实体槽位";
            return false;
        }
        changes.push_back({{}, int(source.slot), int(*replacement)});
        for (auto& binding : bindings) {
            if (binding == source.slot) binding = *replacement;
            else if (binding > source.slot) --binding;
        }
    }
}

// Keep a color-card draft bounded to its physical slots plus the recipes used
// by the logical card. Mixed slots are native suffix entries, so removing from
// the tail preserves every physical nozzle slot and lets the existing remap
// transaction update model, painting, and plate references atomically.
inline bool trim_unused_mixed_slots(PresetBundle& staged, std::vector<size_t>& bindings,
    size_t logical_target_count, std::vector<PrinterFilamentSlotRemap::Change>& changes, std::string& error)
{
    std::set<size_t> retained(bindings.begin(), bindings.end());
    if (!bindings.empty() && bindings.size() != logical_target_count) {
        error = "色卡目标与槽位绑定数量不一致，配置草稿保持原状";
        return false;
    }
    size_t physical_count = 0;
    size_t retained_mixed_count = 0;
    for (size_t i = 0; i < staged.filament_presets.size(); ++i)
        if (!staged.is_mixed_filament(i)) ++physical_count;
    for (const size_t slot : retained) {
        if (slot >= staged.filament_presets.size()) {
            error = "色卡包含无效槽位绑定，配置草稿保持原状";
            return false;
        }
        if (staged.is_mixed_filament(slot)) ++retained_mixed_count;
    }
    // Color-pack import leaves bindings empty when every target is a physical
    // slot. In that case retain its historical exact-card-size cleanup.
    const size_t desired_total = bindings.empty()
        ? logical_target_count : physical_count + retained_mixed_count;
    while (staged.filament_presets.size() > desired_total) {
        const size_t at = staged.filament_presets.size() - 1;
        if (!staged.is_mixed_filament(at)) {
            error = "色卡切换后存在无法安全清理的实体或叠色槽位";
            return false;
        }
        // Recipes are native suffix slots, but reusing an older recipe can
        // leave an unused mixed slot before a recipe needed by the new card.
        // Native deletion is tail-only, so move the retained recipe into the
        // first unretained mixed slot before deleting the old tail.  The
        // model/painting reference is recorded as a normal removal remap;
        // geometry and physical slot identities are unchanged.
        if (retained.count(at)) {
            const auto destination = [&]() -> size_t {
                for (size_t i = 0; i < at; ++i)
                    if (staged.is_mixed_filament(i) && !retained.count(i)) return i;
                return size_t(-1);
            }();
            if (destination == size_t(-1)) {
                error = "色卡切换后存在无法安全清理的实体或叠色槽位";
                return false;
            }
            auto copy_string = [&](const char* key) {
                auto* option = staged.project_config.option<ConfigOptionStrings>(key);
                if (option && at < option->values.size() && destination < option->values.size())
                    option->values[destination] = option->values[at];
            };
            auto copy_bool = [&](const char* key) {
                auto* option = staged.project_config.option<ConfigOptionBools>(key);
                if (option && at < option->values.size() && destination < option->values.size())
                    option->values[destination] = option->values[at];
            };
            auto copy_int = [&](const char* key) {
                auto* option = staged.project_config.option<ConfigOptionInts>(key);
                if (option && at < option->values.size() && destination < option->values.size())
                    option->values[destination] = option->values[at];
            };
            staged.filament_presets[destination] = staged.filament_presets[at];
            for (const auto* key : {"filament_colour", "filament_multi_colour", "filament_colour_type",
                "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient_range",
                "filament_mixed_gradient_curve"}) copy_string(key);
            for (const auto* key : {"filament_is_mixed", "filament_mixed_gradient", "filament_mixed_gradient_per_part"})
                copy_bool(key);
            for (const auto* key : {"filament_map", "filament_nozzle_map", "filament_volume_map"}) copy_int(key);
            if (at < staged.ams_multi_color_filment.size() && destination < staged.ams_multi_color_filment.size())
                staged.ams_multi_color_filment[destination] = staged.ams_multi_color_filment[at];
            retained.erase(at);
            retained.insert(destination);
            for (auto& binding : bindings) if (binding == at) binding = destination;
            // Delete the old tail and remap any references to the retained
            // recipe into its compacted slot.
            staged.update_num_filaments(unsigned(at));
            changes.push_back({{}, int(at), int(destination)});
            continue;
        }
        std::vector<WorkbenchFilamentSlot> replacements;
        const auto current = capture(staged);
        for (const auto& slot : current.slots)
            if (slot.exists && slot.configured && slot.compatible) replacements.push_back(slot);
        const auto source = std::find_if(current.mixed.begin(), current.mixed.end(),
            [at](const auto& item) { return item.slot == at; });
        const auto replacement = nearest_replacement_slot(
            source == current.mixed.end() ? std::string{} : source->color, replacements);
        if (!replacement) {
            error = "色卡切换缺少可用替代槽位，配置草稿保持原状";
            return false;
        }
        staged.update_num_filaments(unsigned(at));
        changes.push_back({{}, int(at), int(*replacement)});
        std::set<size_t> shifted;
        for (const size_t slot : retained)
            if (slot != at) shifted.insert(slot > at ? slot - 1 : slot);
        retained.swap(shifted);
        for (auto& binding : bindings) if (binding > at) --binding;
    }
    if (staged.filament_presets.size() != desired_total) {
        error = "色卡切换后槽位数量无法与色卡保持一致，配置草稿保持原状";
        return false;
    }
    return true;
}

// Remove one virtual recipe at the native suffix. The replacement is used by
// the shared remap transaction for any model or painting references; recipe
// components themselves must never point at another virtual slot.
inline bool remove_mixed(PresetBundle& staged, size_t slot, size_t replacement, std::string& error)
{
    const auto* replacement_preset = replacement < staged.filament_presets.size()
        ? staged.filaments.find_preset(staged.filament_presets[replacement], false) : nullptr;
    const auto* colors = staged.project_config.option<ConfigOptionStrings>("filament_colour");
    if (slot >= staged.filament_presets.size() || !staged.is_mixed_filament(slot) ||
        slot + 1 != staged.filament_presets.size() || replacement >= slot ||
        staged.is_mixed_filament(replacement) || !replacement_preset || !replacement_preset->is_compatible ||
        !colors || replacement >= colors->values.size() || !AI::is_rgb_hex_color(colors->values[replacement])) {
        error = "只能删除末尾叠色槽，并选择有效实体耗材作为替代";
        return false;
    }
    const auto components = staged.project_config.option<ConfigOptionStrings>("filament_mixed_components");
    if (components) for (size_t i = 0; i < slot && i < components->values.size(); ++i) {
        const auto ids = parse_mixed_components(components->values[i]);
        if (std::find(ids.begin(), ids.end(), unsigned(slot + 1)) != ids.end()) {
            error = "该叠色槽仍被其他配方引用，无法删除";
            return false;
        }
    }
    PresetBundle candidate(staged);
    candidate.update_num_filaments(unsigned(slot));
    auto validation_presets = candidate.filament_presets;
    for (auto& name : validation_presets)
        if (!candidate.filaments.find_preset(name, false)) name = staged.filament_presets[replacement];
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
    std::vector<SlotChange>& changes, std::string& error, const ChooseReplacement& choose = {},
    std::vector<size_t>* imported_bindings = nullptr,
    std::optional<size_t> physical_capacity_override = {})
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
    // Retire old virtual recipes before growing physical slots. Otherwise a
    // stale recipe can consume the 32nd native slot and reject a valid card.
    if (!state.mixed.empty()) {
        std::vector<size_t> unused;
        if (!trim_unused_mixed_slots(candidate, unused, state.physical_count,
                pending_changes, error)) return false;
        state = capture(candidate);
    }
    const size_t physical_capacity = std::min(limit,
        physical_capacity_override.value_or(state.physical_capacity));
    const auto bases = WorkbenchFilamentRecipes::select_bases(pack.colors, physical_capacity);
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
    std::vector<size_t> preferred_bindings(pack.colors.size(), size_t(-1));
    for (size_t i = 0; i < bases.size(); ++i) {
        const size_t source_index = bases[i];
        const size_t at = state.slots[i].slot;
        preferred_bindings[source_index] = at;
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
    std::vector<size_t> bindings = pack.colors.size() == target
        ? preferred_bindings : std::vector<size_t>{};
    if (pack.colors.size() > target && capture(candidate).ready) {
        const size_t old_count = candidate.filament_presets.size();
        if (!WorkbenchFilamentRecipes::stage_targets(candidate, pack.colors, bindings, error,
                preferred_bindings)) return false;
        for (size_t at = old_count; at < candidate.filament_presets.size(); ++at)
            pending_changes.push_back({{at}, -1, -1});
    }
    if (candidate.filament_presets.size() > pack.colors.size() &&
        !trim_unused_mixed_slots(candidate, bindings, pack.colors.size(), pending_changes, error)) return false;
    if (candidate.filament_presets.size() != pack.colors.size()) {
        error = "色卡切换后槽位数量无法与色卡保持一致，配置草稿保持原状"; return false;
    }
    if (bindings.size() != pack.colors.size() ||
        std::any_of(bindings.begin(), bindings.end(), [&](size_t slot) {
            return slot >= candidate.filament_presets.size();
        })) {
        error = "色卡未生成完整的原生槽位绑定，配置草稿保持原状"; return false;
    }
    // A staged card may intentionally leave a slot without a compatible
    // material preset.  The dialog will keep that color and ask the user to
    // choose a compatible preset before completion.  Only run the complete
    // project validation once every slot is ready; structural validation above
    // still guarantees that the card and native slot arrays have matching
    // lengths.
    if (capture(candidate).ready &&
        !ProjectConfigRestore::validate(candidate.project_config, candidate.filament_presets, candidate, error)) return false;
    staged.project_config = std::move(candidate.project_config);
    staged.filament_presets = std::move(candidate.filament_presets);
    changes = std::move(pending_changes);
    if (imported_bindings) *imported_bindings = std::move(bindings);
    return true;
}

struct StagedColorPackImport {
    std::shared_ptr<PresetBundle> configuration;
    std::vector<SlotChange> changes;
    std::vector<size_t> bindings;
    std::string source_signature;
    size_t physical_count {0};
    size_t mixed_count {0};
};

struct HonorSuiteCompatibility {
    bool compatible {false};
    size_t nozzle_count {0};
    std::string reason;
};

inline bool honor_material_category_matches(std::string actual, std::string expected)
{
    const auto uppercase = [](unsigned char value) { return char(std::toupper(value)); };
    std::transform(actual.begin(), actual.end(), actual.begin(), uppercase);
    std::transform(expected.begin(), expected.end(), expected.begin(), uppercase);
    return !actual.empty() && actual == expected;
}

inline HonorSuiteCompatibility honor_suite_compatibility(const PresetBundle& bundle,
    const HonorFilamentSuite& suite)
{
    HonorSuiteCompatibility result;
    if (!suite.valid()) {
        result.reason = "荣耀耗材套装数据无效";
        return result;
    }
    const auto& printer = bundle.printers.get_edited_preset().config;
    const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    result.nozzle_count = nozzles ? nozzles->values.size() : 0;
    const size_t required = suite.physical_slots.size();
    if (!result.nozzle_count) {
        result.reason = "请选择 FFF 打印机";
        return result;
    }
    if (printer.opt_bool("single_extruder_multi_material")) {
        result.reason = "当前打印机为单喷嘴多材料模式，此套装需要 6 个真实独立喷头";
        return result;
    }
    const auto snapshot = capture(bundle);
    if (result.nozzle_count < required || snapshot.physical_capacity < required) {
        result.reason = "当前打印机为 " + std::to_string(result.nozzle_count) +
            " 头，此套装需要 6 个实体耗材";
        return result;
    }
    result.compatible = true;
    return result;
}

// Honor suites are physical hardware definitions, not generic color cards.
// This path never selects bases or creates/removes mixed recipes. It prepares
// six real slots in a copy and leaves every unrelated physical or mixed slot
// untouched until the caller commits the complete transaction.
inline bool stage_honor_physical_suite_import(const PresetBundle& live,
    const HonorFilamentSuite& suite, StagedColorPackImport& result, std::string& error)
{
    result = {};
    error.clear();
    const auto compatibility = honor_suite_compatibility(live, suite);
    if (!compatibility.compatible) {
        error = compatibility.reason;
        return false;
    }
    const auto source = capture(live);
    if (!source.ready) {
        error = source.reason.empty() ? "当前实体耗材配置不可用" : source.reason;
        return false;
    }

    auto staged = std::make_shared<PresetBundle>(live);
    std::vector<SlotChange> changes;
    const size_t required = suite.physical_slots.size();
    constexpr size_t missing_slot = std::numeric_limits<size_t>::max();
    const auto collect_bindings = [&](const WorkbenchPrinterFilaments& state) {
        std::vector<size_t> bindings(required, missing_slot);
        std::set<size_t> used;
        for (size_t index = 0; index < required; ++index) {
            const auto found = std::find_if(state.slots.begin(), state.slots.end(), [&](const auto& slot) {
                return slot.exists && slot.configured && slot.compatible && !slot.preset.empty() &&
                    used.find(slot.slot) == used.end() &&
                    honor_material_category_matches(slot.material,
                        suite.physical_slots[index].material_category);
            });
            if (found != state.slots.end()) {
                bindings[index] = found->slot;
                used.insert(found->slot);
            }
        }
        return bindings;
    };
    const auto reusable_preset = [&](const std::string& material_category) {
        const auto found = std::find_if(source.slots.begin(), source.slots.end(), [&](const auto& slot) {
            return slot.exists && slot.configured && slot.compatible && !slot.preset.empty() &&
                honor_material_category_matches(slot.material, material_category);
        });
        return found == source.slots.end() ? std::string{} : found->preset;
    };

    auto staged_state = capture(*staged);
    auto bindings = collect_bindings(staged_state);
    while (std::find(bindings.begin(), bindings.end(), missing_slot) != bindings.end()) {
        const size_t missing = size_t(std::find(bindings.begin(), bindings.end(), missing_slot) - bindings.begin());
        const std::string preset = reusable_preset(suite.physical_slots[missing].material_category);
        if (preset.empty()) {
            error = "当前没有可复用的 " + suite.physical_slots[missing].material_category +
                " 兼容实体耗材预设";
            return false;
        }
        if (staged_state.physical_count >= staged_state.physical_capacity) {
            error = "当前打印机没有足够的兼容实体喷头槽位，无法应用荣耀耗材套装";
            return false;
        }
        if (staged->filament_presets.size() >= size_t(EnforcerBlockerType::ExtruderMax)) {
            error = "原生槽位已满，无法补齐六个实体耗材";
            return false;
        }
        const size_t before = staged_state.physical_count;
        const size_t at = add(*staged);
        staged->filament_presets[at] = preset;
        staged->project_config.option<ConfigOptionStrings>("filament_colour")->values[at] =
            suite.physical_slots[missing].color;
        staged->project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[at] =
            suite.physical_slots[missing].color;
        staged->project_config.option<ConfigOptionStrings>("filament_colour_type")->values[at] = "1";
        changes.push_back({{at}, -1, -1});
        staged_state = capture(*staged);
        if (staged_state.physical_count != before + 1) {
            error = "无法补齐六个实体耗材槽位，工程保持原状";
            return false;
        }
        bindings = collect_bindings(staged_state);
    }

    auto& colors = staged->project_config.option<ConfigOptionStrings>("filament_colour")->values;
    auto& multi = staged->project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
    auto& types = staged->project_config.option<ConfigOptionStrings>("filament_colour_type")->values;
    for (size_t index = 0; index < required; ++index) {
        const size_t slot = bindings[index];
        colors[slot] = suite.physical_slots[index].color;
        multi[slot] = suite.physical_slots[index].color;
        types[slot] = "1";
    }

    const auto imported = capture(*staged);
    if (!imported.ready || imported.physical_count < required ||
        !ProjectConfigRestore::validate(staged->project_config, staged->filament_presets, *staged, error) ||
        !PrinterFilamentSlotRemap::validate(changes, live.filament_presets.size(), staged->filament_presets.size())) {
        if (error.empty()) error = !imported.reason.empty()
            ? imported.reason : "荣耀耗材套装暂存结果不完整，工程保持原状";
        return false;
    }
    for (size_t index = 0; index < required; ++index) {
        const size_t slot = bindings[index];
        const auto imported_slot = std::find_if(imported.slots.begin(), imported.slots.end(),
            [slot](const auto& candidate) { return candidate.slot == slot; });
        if (slot >= staged->filament_presets.size() || staged->is_mixed_filament(slot) ||
            colors[slot] != suite.physical_slots[index].color || imported_slot == imported.slots.end() ||
            !honor_material_category_matches(imported_slot->material,
                suite.physical_slots[index].material_category)) {
            error = "荣耀套装未生成完整的六实体槽绑定，工程保持原状";
            return false;
        }
    }

    result.configuration = std::move(staged);
    result.changes = std::move(changes);
    result.bindings = std::move(bindings);
    result.source_signature = source.signature;
    result.physical_count = required;
    result.mixed_count = 0;
    return true;
}

inline bool stage_workbench_color_pack_import(const PresetBundle& live,
    const FilamentColorPack& pack, StagedColorPackImport& result, std::string& error)
{
    result = {};
    if (!pack.valid_for_workbench_import()) {
        error = "工作台色卡必须包含 1 至 32 个有效颜色";
        return false;
    }
    const auto source = capture(live);
    // A SEMM/single-nozzle profile permits up to 32 logical physical rows, but
    // direct workbench import must use the actually configured spools as its
    // physical base. Fixed multi-nozzle printers may additionally fill idle
    // nozzles up to their hardware capacity.
    const size_t physical_capacity = source.physical_capacity == size_t(EnforcerBlockerType::ExtruderMax)
        ? source.physical_count : source.physical_capacity;
    auto staged = std::make_shared<PresetBundle>(live);
    std::vector<SlotChange> changes;
    std::vector<size_t> bindings;
    const ChooseReplacement choose = [](const WorkbenchFilamentSlot& slot,
        const std::vector<WorkbenchFilamentSlot>& targets) {
        return nearest_replacement_slot(slot.color, targets);
    };
    if (!import_color_pack(*staged, pack, changes, error, choose, &bindings, physical_capacity)) return false;
    const auto imported = capture(*staged);
    const size_t expected_physical = std::min(pack.colors.size(), physical_capacity);
    if (!imported.ready || imported.physical_count != expected_physical ||
        imported.mixed.size() != pack.colors.size() - expected_physical ||
        bindings.size() != pack.colors.size() ||
        std::set<size_t>(bindings.begin(), bindings.end()).size() != bindings.size() ||
        !PrinterFilamentSlotRemap::validate(changes, live.filament_presets.size(), staged->filament_presets.size())) {
        error = !imported.ready && !imported.reason.empty()
            ? imported.reason : "色卡暂存结果不完整，工程保持原状";
        return false;
    }
    result.configuration = std::move(staged);
    result.changes = std::move(changes);
    result.bindings = std::move(bindings);
    result.source_signature = source.signature;
    result.physical_count = imported.physical_count;
    result.mixed_count = imported.mixed.size();
    return true;
}

} // namespace Slic3r::GUI::PrinterFilamentConfig
