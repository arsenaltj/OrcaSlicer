#pragma once

#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// Provider-free input used to translate Orca's physical and virtual slot arrays
// into the neutral model-generation capability contract.
struct OrcaPaletteSlotCapability
{
    size_t                               slot { 0 };
    std::string                          display_color;
    std::string                          material_type;
    bool                                 is_mixed { false };
    bool                                 compatible { false };
    std::vector<AI::MixedColorComponent> mixed_components;
};

inline std::vector<size_t> select_model_generation_physical_slots(
    const std::vector<OrcaPaletteSlotCapability>& slots)
{
    std::vector<size_t> selected;
    selected.reserve(AI::kMaxPhysicalColorChannels);
    for (const OrcaPaletteSlotCapability& slot : slots) {
        if (slot.is_mixed || !AI::is_rgb_hex_color(slot.display_color) ||
            std::find(selected.begin(), selected.end(), slot.slot) != selected.end())
            continue;
        selected.push_back(slot.slot);
        if (selected.size() == AI::kMaxPhysicalColorChannels)
            break;
    }
    return selected;
}

inline AI::PrintablePaletteSnapshot build_orca_palette_snapshot(
    const std::vector<OrcaPaletteSlotCapability>& slots, bool allow_new_process_mix = true)
{
    AI::PrintablePaletteSnapshot snapshot;
    snapshot.supported_output_modes.clear();

    const std::vector<size_t> selected_slots = select_model_generation_physical_slots(slots);
    snapshot.physical_channels.reserve(selected_slots.size());
    for (const size_t selected_slot : selected_slots) {
        const auto found = std::find_if(slots.begin(), slots.end(), [selected_slot](const auto& candidate) {
            return !candidate.is_mixed && candidate.slot == selected_slot;
        });
        if (found == slots.end())
            continue;
        snapshot.physical_channels.push_back(
            {found->slot, found->display_color, found->material_type, found->compatible});
    }

    for (const OrcaPaletteSlotCapability& slot : slots) {
        if (!slot.is_mixed)
            continue;
        AI::MixedColorRecipe recipe {slot.display_color, slot.mixed_components, slot.slot};
        if (!AI::is_valid_mixed_color_recipe(recipe))
            continue;
        const bool all_components_available =
            std::all_of(recipe.components.begin(), recipe.components.end(), [&snapshot](const auto& component) {
                return std::any_of(snapshot.physical_channels.begin(), snapshot.physical_channels.end(),
                                   [&component](const auto& channel) {
                                       return channel.compatible && channel.slot == component.slot;
                                   });
            });
        if (all_components_available)
            snapshot.mixed_recipes.push_back(std::move(recipe));
    }

    snapshot.rebuild_legacy_projection();
    if (!snapshot.physical_channels.empty())
        snapshot.supported_output_modes.push_back(AI::ColorOutputMode::DiscreteFilament);
    const size_t compatible_count = static_cast<size_t>(std::count_if(
        snapshot.physical_channels.begin(), snapshot.physical_channels.end(),
        [](const AI::PhysicalFilamentChannel& channel) { return channel.compatible; }));
    if ((allow_new_process_mix && compatible_count >= 2) || !snapshot.mixed_recipes.empty())
        snapshot.supported_output_modes.push_back(AI::ColorOutputMode::ProcessMix);
    return snapshot;
}

} // namespace Slic3r::GUI
