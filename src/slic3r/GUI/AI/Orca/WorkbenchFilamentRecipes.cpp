#include "WorkbenchFilamentRecipes.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include <map>
#include <numeric>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI::WorkbenchFilamentRecipes {

PreviewPalette::Color color(const std::string& hex)
{
    ColorDecomposeRgb rgb;
    if (!color_decompose_hex_to_rgb(hex, rgb)) throw std::runtime_error("无效耗材颜色");
    return {rgb.r / 255.f, rgb.g / 255.f, rgb.b / 255.f};
}

double distance(const std::string& a, const std::string& b)
{
    return PreviewPalette::distance(PreviewPalette::to_lab(color(a)), PreviewPalette::to_lab(color(b)));
}

// Evaluate the native recommender, with pure colours also available. The
// singleton fallback is used only while selecting bases, never to invent a mix.
double reconstruction_error(const std::vector<std::string>& targets, const std::vector<size_t>& bases)
{
    std::vector<ColorDecomposePhysicalFilament> physical;
    for (size_t at : bases) physical.push_back({targets[at], "", "PLA", false, unsigned(at + 1)});
    double sum = 0;
    std::map<std::string, double> target_errors;
    for (const auto& target : targets) {
        const auto previous = target_errors.find(target);
        if (previous != target_errors.end()) { sum += previous->second; continue; }
        double best = std::numeric_limits<double>::max();
        for (size_t at : bases) best = std::min(best, distance(target, targets[at]));
        if (best > 0 && physical.size() > 1) {
            ColorDecomposeRgb rgb; color_decompose_hex_to_rgb(target, rgb);
            const auto recipe = recommend_from_physical_filaments(rgb, physical, "PLA");
            if (recipe.valid) best = std::min(best, distance(target, recipe.matched_color_hex));
        }
        target_errors.emplace(target, best);
        sum += best;
    }
    return sum;
}

// Stable ordering and lexicographic tie-breaking retain colour-pack identities,
// including duplicate RGB entries. No deduplication is performed.
std::vector<size_t> select_bases(const std::vector<std::string>& targets, size_t capacity)
{
    if (!capacity || targets.empty()) return {};
    capacity = std::min(capacity, targets.size());
    if (targets.size() == capacity) {
        std::vector<size_t> all(capacity); std::iota(all.begin(), all.end(), 0); return all;
    }
    std::map<std::vector<std::string>, double> cache;
    auto score = [&](const std::vector<size_t>& bases) {
        std::vector<std::string> key;
        for (size_t at : bases) key.push_back(targets[at]);
        const auto found = cache.find(key);
        if (found != cache.end()) return found->second;
        return cache.emplace(std::move(key), reconstruction_error(targets, bases)).first->second;
    };
    auto better = [](double value, const auto& indices, double best, const auto& chosen) {
        return value < best - 1e-7 || (std::abs(value - best) <= 1e-7 && indices < chosen);
    };
    std::vector<size_t> chosen;
    double best = std::numeric_limits<double>::max();
    if (targets.size() <= 8) {
        const unsigned end = 1u << unsigned(targets.size());
        for (unsigned mask = 1; mask < end; ++mask) {
            std::vector<size_t> indices;
            for (size_t i = 0; i < targets.size(); ++i) if (mask & (1u << unsigned(i))) indices.push_back(i);
            if (indices.size() != capacity) continue;
            const double value = score(indices);
            if (better(value, indices, best, chosen)) { chosen = indices; best = value; }
        }
        return chosen;
    }
    while (chosen.size() < capacity) {
        std::vector<size_t> next;
        best = std::numeric_limits<double>::max();
        for (size_t i = 0; i < targets.size(); ++i) {
            if (std::binary_search(chosen.begin(), chosen.end(), i)) continue;
            auto trial = chosen; trial.push_back(i); std::sort(trial.begin(), trial.end());
            const double value = score(trial);
            if (better(value, trial, best, next)) { next = std::move(trial); best = value; }
        }
        chosen = std::move(next);
    }
    // At most 256 swaps, in deterministic original-pack order.
    size_t budget = 256;
    for (size_t pass = 0; pass < 2 && budget; ++pass) {
        auto next = chosen; best = score(chosen);
        for (size_t at = 0; at < chosen.size() && budget; ++at)
            for (size_t i = 0; i < targets.size() && budget; ++i) {
                if (std::binary_search(chosen.begin(), chosen.end(), i)) continue;
                --budget;
                auto trial = chosen; trial[at] = i; std::sort(trial.begin(), trial.end());
                const double value = score(trial);
                if (better(value, trial, best, next)) { next = std::move(trial); best = value; }
            }
        if (next == chosen) break;
        chosen = std::move(next);
    }
    return chosen;
}

bool compatible_components(const PresetBundle& bundle, const std::vector<unsigned>& ids)
{
    if (ids.size() < 2 || std::set<unsigned>(ids.begin(), ids.end()).size() != ids.size()) return false;
    std::vector<std::string> types;
    std::vector<int> temperatures, lows, highs;
    for (unsigned id : ids) {
        if (!id || id > bundle.filament_presets.size() || bundle.is_mixed_filament(id - 1)) return false;
        const auto* preset = bundle.filaments.find_preset(bundle.filament_presets[id - 1], false);
        if (!preset || !preset->is_compatible) return false;
        const auto* type = preset->config.option<ConfigOptionStrings>("filament_type");
        if (!type || type->values.empty() || type->get_at(0).empty()) return false;
        types.push_back(type->get_at(0));
        if (types.back() != types.front()) return false;
        for (const auto& option : {std::make_pair("nozzle_temperature", &temperatures),
            std::make_pair("nozzle_temperature_range_low", &lows), std::make_pair("nozzle_temperature_range_high", &highs)}) {
            const auto* values = preset->config.option<ConfigOptionInts>(option.first);
            option.second->push_back(values && !values->values.empty() ? values->get_at(0) : 0);
        }
    }
    return Print::check_multi_filaments_compatibility(types, temperatures, lows, highs) == FilamentCompatibilityType::Compatible;
}

bool valid_recipe(const PresetBundle& bundle, size_t slot)
{
    if (!bundle.is_mixed_filament(slot) || bundle.project_config.opt_bool("filament_mixed_gradient", slot)) return false;
    const auto ids = parse_mixed_components(bundle.project_config.opt_string("filament_mixed_components", slot));
    if (!compatible_components(bundle, ids)) return false;
    const auto text = bundle.project_config.opt_string("filament_mixed_sublayer_ratios", slot);
    if (text.empty()) return false;
    return validate_mixed_filament_params(bundle.project_config.option<ConfigOptionBools>("filament_is_mixed")->values,
        bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values,
        bundle.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values,
        bundle.project_config.option<ConfigOptionBools>("filament_mixed_gradient")->values,
        bundle.project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range")->values,
        bundle.project_config.option<ConfigOptionStrings>("filament_mixed_gradient_curve")->values).empty();
}

// Append only virtual slots. All generated recipes are validated against real
// identities and material/temperature compatibility before publishing the copy.
bool stage_targets(PresetBundle& staged, const std::vector<std::string>& targets,
    std::vector<size_t>& bindings, std::string& error, const std::vector<size_t>& preferred)
{
    PresetBundle candidate(staged);
    std::vector<size_t> result;
    // A color-card entry represents one independent output slot. Never bind
    // two entries to the same native physical or virtual slot; duplicate RGB
    // entries therefore retain their separate card identities.
    std::set<size_t> used_slots;
    std::vector<ColorDecomposePhysicalFilament> physical;
    for (size_t i = 0; i < candidate.filament_presets.size(); ++i) if (!candidate.is_mixed_filament(i)) {
        const auto* preset = candidate.filaments.find_preset(candidate.filament_presets[i], false);
        const auto* type = preset ? preset->config.option<ConfigOptionStrings>("filament_type") : nullptr;
        ColorDecomposeRgb rgb;
        if (!preset || !preset->is_compatible || !type || type->values.empty() ||
            !color_decompose_hex_to_rgb(candidate.project_config.opt_string("filament_colour", unsigned(i)), rgb)) {
            error = "请先补选兼容预设和实际装料颜色，才能生成叠色配方"; return false;
        }
        physical.push_back({candidate.project_config.opt_string("filament_colour", unsigned(i)), preset->name, type->get_at(0), false, unsigned(i + 1)});
    }
    auto append_identity_recipe = [&](const std::vector<unsigned>& ids, const std::vector<double>& ratios,
                                      const std::string& appearance) -> std::optional<size_t> {
        const size_t slot = candidate.filament_presets.size();
        if (slot >= 32) {
            error = "叠色配方超过原生32槽上限";
            return {};
        }
        if (ids.size() != ratios.size() || ids.size() < 2 ||
            std::any_of(ratios.begin(), ratios.end(), [](double ratio) { return ratio <= 0.; }) ||
            !compatible_components(candidate, ids)) {
            error = "重复颜色没有可用的材料/温度兼容叠色配方";
            return {};
        }
        const auto multi = candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
        candidate.set_num_filaments(unsigned(slot + 1), appearance);
        std::copy(multi.begin(), multi.end(),
            candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values.begin());
        candidate.filament_presets[slot] = candidate.filament_presets[ids.front() - 1];
        candidate.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot] = true;
        std::string component_text, ratio_text;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) { component_text += ','; ratio_text += ','; }
            component_text += std::to_string(ids[i]);
            ratio_text += std::to_string(ratios[i]);
        }
        candidate.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot] = component_text;
        candidate.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot] = ratio_text;
        candidate.project_config.option<ConfigOptionStrings>("filament_colour")->values[slot] = appearance;
        candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[slot] = appearance;
        if (!valid_recipe(candidate, slot)) {
            error = "新叠色配方验证失败";
            return {};
        }
        return slot;
    };
    // Use the same computation as the native sidebar after base colours change.
    for (size_t i = 0; i < candidate.filament_presets.size(); ++i) if (valid_recipe(candidate, i)) {
        const auto ids = parse_mixed_components(candidate.project_config.opt_string("filament_mixed_components", unsigned(i)));
        const auto ratios = parse_mixed_ratios(candidate.project_config.opt_string("filament_mixed_sublayer_ratios", unsigned(i)), ids.size());
        std::vector<std::string> hexes;
        std::vector<int> percentages;
        for (size_t j = 0; j < ids.size(); ++j) {
            hexes.push_back(candidate.project_config.opt_string("filament_colour", ids[j] - 1));
            percentages.push_back(int(std::lround(ratios[j] * 100)));
        }
        const auto estimated = blend_color_multi(hexes, percentages);
        if (candidate.project_config.opt_string("filament_colour", unsigned(i)) != estimated) {
            candidate.project_config.option<ConfigOptionStrings>("filament_colour")->values[i] = estimated;
            candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[i] = estimated;
        }
    }
    for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
        const auto& target = targets[target_index];
        ColorDecomposeRgb rgb;
        if (!color_decompose_hex_to_rgb(target, rgb)) { error = "目标色无效"; return false; }
        std::optional<size_t> exact;
        for (const auto& spool : physical) if (!used_slots.count(spool.filament_index - 1) && distance(target, spool.color_hex) == 0) {
            if (!exact) exact = spool.filament_index - 1;
            if (target_index < preferred.size() && preferred[target_index] == spool.filament_index - 1)
                exact = preferred[target_index];
        }
        if (exact) { used_slots.insert(*exact); result.push_back(*exact); continue; }

        // Reuse an unused exact virtual identity before cloning a duplicate.
        // This matters when a saved configuration already contains multiple
        // legal recipes with the same displayed RGB: preferred bindings must
        // keep those native slot identities stable.
        std::optional<size_t> exact_recipe;
        for (size_t i = 0; i < candidate.filament_presets.size(); ++i) {
            if (used_slots.count(i) || !valid_recipe(candidate, i) ||
                distance(target, candidate.project_config.opt_string("filament_colour", unsigned(i))) != 0.)
                continue;
            exact_recipe = i;
            if (target_index < preferred.size() && preferred[target_index] == i) break;
        }
        if (exact_recipe) {
            used_slots.insert(*exact_recipe);
            result.push_back(*exact_recipe);
            continue;
        }

        // Equal RGB entries are distinct workbench identities. Once the only
        // exact physical slot is consumed, clone the preceding legal recipe or
        // synthesize a near-pure two-spool recipe instead of binding the same
        // native slot twice. The latter is needed because the native
        // recommender intentionally returns a one-component exact match.
        std::optional<size_t> duplicate_slot;
        for (size_t earlier = target_index; earlier-- > 0;) {
            if (targets[earlier] == target && earlier < result.size()) {
                duplicate_slot = result[earlier];
                break;
            }
        }
        if (duplicate_slot && *duplicate_slot < candidate.filament_presets.size()) {
            std::optional<size_t> appended;
            if (candidate.is_mixed_filament(*duplicate_slot) && valid_recipe(candidate, *duplicate_slot)) {
                const auto ids = parse_mixed_components(
                    candidate.project_config.opt_string("filament_mixed_components", unsigned(*duplicate_slot)));
                const auto ratios = parse_mixed_ratios(
                    candidate.project_config.opt_string("filament_mixed_sublayer_ratios", unsigned(*duplicate_slot)), ids.size());
                appended = append_identity_recipe(ids, ratios,
                    candidate.project_config.opt_string("filament_colour", unsigned(*duplicate_slot)));
            } else if (!candidate.is_mixed_filament(*duplicate_slot)) {
                const auto primary = std::find_if(physical.begin(), physical.end(), [&](const auto& spool) {
                    return spool.filament_index - 1 == *duplicate_slot;
                });
                std::optional<unsigned> partner;
                std::string estimated;
                double best_delta = std::numeric_limits<double>::max();
                if (primary != physical.end()) for (const auto& spool : physical) {
                    if (spool.filament_index == primary->filament_index ||
                        !compatible_components(candidate, {primary->filament_index, spool.filament_index})) continue;
                    const bool same = distance(primary->color_hex, spool.color_hex) == 0.;
                    const std::vector<int> percentages = same ? std::vector<int>{50, 50} : std::vector<int>{99, 1};
                    const auto blended = blend_color_multi({primary->color_hex, spool.color_hex}, percentages);
                    const double delta = distance(target, blended);
                    if (!partner || delta < best_delta - 1e-7 ||
                        (std::abs(delta - best_delta) <= 1e-7 && spool.filament_index < *partner)) {
                        partner = spool.filament_index;
                        estimated = blended;
                        best_delta = delta;
                    }
                }
                if (primary != physical.end() && partner) {
                    const bool same = distance(primary->color_hex,
                        candidate.project_config.opt_string("filament_colour", *partner - 1)) == 0.;
                    appended = append_identity_recipe({primary->filament_index, *partner},
                        same ? std::vector<double>{0.5, 0.5} : std::vector<double>{0.99, 0.01}, estimated);
                }
            }
            if (appended) {
                used_slots.insert(*appended);
                result.push_back(*appended);
                continue;
            }
            if (!error.empty()) return false;
        }
        ColorDecomposeRecipeResult best;
        double best_error = std::numeric_limits<double>::max();
        for (size_t seed = 0; seed < physical.size(); ++seed) {
            std::vector<ColorDecomposePhysicalFilament> group {physical[seed]};
            std::vector<unsigned> ids {physical[seed].filament_index};
            for (size_t at = 0; at < physical.size(); ++at) if (at != seed) {
                auto proposed = ids; proposed.push_back(physical[at].filament_index);
                if (compatible_components(candidate, proposed)) { ids = std::move(proposed); group.push_back(physical[at]); }
            }
            // Prevent the native recommender's cross-material fallback by
            // supplying only a mutually compatible group.
            if (group.size() < 2) continue;
            const auto recipe = recommend_from_physical_filaments(rgb, group, group.front().type);
            if (!recipe.valid || recipe.components.size() < 2) continue;
            const double delta = distance(target, recipe.matched_color_hex);
            if (delta < best_error) { best = recipe; best_error = delta; }
        }
        // Prefer existing legal recipes when they reproduce this target at
        // least as well as a new estimate, retaining native slot identities.
        std::optional<size_t> existing;
        for (size_t i = 0; i < candidate.filament_presets.size(); ++i) if (!used_slots.count(i) && valid_recipe(candidate, i)) {
            ColorDecomposeRgb stored;
            const auto hex = candidate.project_config.opt_string("filament_colour", unsigned(i));
            if (!color_decompose_hex_to_rgb(hex, stored)) continue;
            const double delta = distance(target, hex);
            const bool preferred_slot = target_index < preferred.size() && preferred[target_index] == i;
            if (delta < best_error - 1e-7 || (std::abs(delta - best_error) <= 1e-7 && (!existing || preferred_slot))) {
                best_error = delta; existing = i;
            }
        }
        if (existing) { used_slots.insert(*existing); result.push_back(*existing); continue; }
        if (!best.valid) {
            error = "目标色 " + target + " 没有材料/温度兼容的叠色配方；请调整实体耗材"; return false;
        }
        std::vector<std::pair<unsigned, int>> normalized;
        for (const auto& component : best.components) normalized.emplace_back(component.filament_index, component.ratio);
        std::sort(normalized.begin(), normalized.end());
        std::optional<size_t> reused;
        for (size_t i = 0; i < candidate.filament_presets.size(); ++i) if (!used_slots.count(i) && valid_recipe(candidate, i)) {
            const auto ids = parse_mixed_components(candidate.project_config.opt_string("filament_mixed_components", unsigned(i)));
            const auto ratios = parse_mixed_ratios(candidate.project_config.opt_string("filament_mixed_sublayer_ratios", unsigned(i)), ids.size());
            std::vector<std::pair<unsigned, int>> stored;
            for (size_t j = 0; j < ids.size(); ++j) stored.emplace_back(ids[j], int(std::lround(ratios[j] * 100)));
            std::sort(stored.begin(), stored.end());
            if (stored == normalized) { reused = i; break; }
        }
        if (reused) { used_slots.insert(*reused); result.push_back(*reused); continue; }
        const size_t slot = candidate.filament_presets.size();
        if (slot >= 32) { error = "叠色配方超过原生32槽上限"; return false; }
        const auto multi = candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
        candidate.set_num_filaments(unsigned(slot + 1), best.matched_color_hex);
        std::copy(multi.begin(), multi.end(), candidate.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values.begin());
        candidate.filament_presets[slot] = candidate.filament_presets[normalized.front().first - 1];
        candidate.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot] = true;
        std::string ids, ratios;
        // Keep the recommender's component order: the native estimated blend
        // rounds between layers. Sorting is only for recipe identity above.
        for (const auto& component : best.components) {
            if (!ids.empty()) { ids += ','; ratios += ','; }
            ids += std::to_string(component.filament_index); ratios += std::to_string(component.ratio / 100.0);
        }
        candidate.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot] = ids;
        candidate.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot] = ratios;
        if (!valid_recipe(candidate, slot)) { error = "新叠色配方验证失败"; return false; }
        used_slots.insert(slot);
        result.push_back(slot);
    }
    if (!ProjectConfigRestore::validate(candidate.project_config, candidate.filament_presets, candidate, error)) return false;
    staged.project_config = std::move(candidate.project_config);
    staged.filament_presets = std::move(candidate.filament_presets);
    bindings = std::move(result);
    return true;
}
} // namespace Slic3r::GUI::WorkbenchFilamentRecipes
