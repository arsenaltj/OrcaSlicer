#pragma once

#include "slic3r/GUI/TextureImportDialog.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

inline bool stage_workbench_texture_import(ModelObject& object, const PaintedMesh& painted,
    std::vector<FilamentMatch> matches, const std::vector<TextureFilamentEntry>& entries,
    const std::vector<TextureNewMixedFilament>& mixed, PresetBundle& staged, std::string& error)
{
    auto fail = [&](const char* message) { error = message; return false; };
    if (object.volumes.size() != 1 || !object.volumes.front()->is_model_part() ||
        painted.face_colors.empty() || matches.empty()) return fail("The detached color result is incomplete.");
    std::vector<int> slots(entries.size(), -1);
    for (const auto& entry : entries) {
        if (entry.dialog_index < 0 || size_t(entry.dialog_index) >= slots.size()) return fail("Invalid material mapping index.");
        if (entry.kind == TextureFilamentKind::NewPhysical) return fail("This import preserves existing physical materials.");
        if (entry.kind == TextureFilamentKind::ExistingPhysical || entry.kind == TextureFilamentKind::ExistingMixed) {
            if (entry.project_config_index >= staged.filament_presets.size()) return fail("The material slot is no longer available.");
            slots[entry.dialog_index] = int(entry.project_config_index);
        }
    }
    for (const auto& recipe : mixed) {
        if (recipe.dialog_index < 0 || size_t(recipe.dialog_index) >= slots.size() ||
            recipe.component_dialog_indices.size() < 2 || recipe.component_dialog_indices.size() != recipe.ratios.size())
            return fail("The mixed material recipe is incomplete.");
        std::string components, ratios;
        int sum = 0;
        for (size_t i = 0; i < recipe.ratios.size(); ++i) {
            const int index = recipe.component_dialog_indices[i];
            if (index < 0 || size_t(index) >= slots.size() || slots[index] < 0 || recipe.ratios[i] <= 0 ||
                staged.project_config.opt_bool("filament_is_mixed", slots[index]))
                return fail("The recipe references an invalid physical material.");
            if (i) { components += ','; ratios += ','; }
            components += std::to_string(slots[index] + 1);
            ratios += std::to_string(recipe.ratios[i] / 100.);
            sum += recipe.ratios[i];
        }
        if (sum != 100) return fail("The mixed material ratios do not sum to 100 percent.");
        const size_t slot = staged.filament_presets.size();
        if (slot >= 32) return fail("The material slot limit has been reached.");
        const auto previous_multi = staged.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.dialog_index == recipe.dialog_index; });
        if (found == entries.end()) return fail("The mixed material has no color.");
        staged.set_num_filaments(unsigned(slot + 1), found->color_hex);
        auto* multi = staged.project_config.option<ConfigOptionStrings>("filament_multi_colour");
        std::copy(previous_multi.begin(), previous_multi.end(), multi->values.begin());
        staged.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot] = true;
        staged.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot] = components;
        staged.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot] = ratios;
        staged.filament_presets[slot] = staged.filament_presets[slots[recipe.component_dialog_indices.front()]];
        slots[recipe.dialog_index] = int(slot);
    }
    int base = 33;
    for (auto& match : matches) {
        if (match.cluster_index < 0 || size_t(match.cluster_index) >= painted.cluster_colors.size() ||
            match.filament_index < 0 || size_t(match.filament_index) >= slots.size() || slots[match.filament_index] < 0)
            return fail("One target color has no confirmed material assignment.");
        match.filament_index = slots[match.filament_index];
        base = std::min(base, match.filament_index + 1);
    }
    if (!apply_painted_mesh_to_volume(painted, matches, *object.volumes.front()))
        return fail("Native color painting could not be applied to the detached model.");
    object.volumes.front()->config.set("extruder", base);
    object.config.set("extruder", base);
    return true;
}
}
