#pragma once

#include "PostGenerationWorkbenchState.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <cctype>

namespace Slic3r::GUI {

inline std::vector<AI::PhysicalFilamentChannel> workbench_project_channels(const DynamicPrintConfig& config)
{
    std::vector<AI::PhysicalFilamentChannel> channels;
    const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    const auto* mixed = config.option<ConfigOptionBools>("filament_is_mixed");
    if (!colors) return channels;
    // Editable project slots are independent of the generation channel limit.
    for (size_t slot = 0; slot < colors->values.size(); ++slot) {
        if (mixed && slot < mixed->values.size() && mixed->values[slot]) continue;
        auto color = colors->values[slot];
        if (!AI::is_rgb_hex_color(color)) continue;
        std::transform(color.begin(), color.end(), color.begin(),
            [](unsigned char value) { return char(std::toupper(value)); });
        channels.push_back({slot, std::move(color), {}, false});
    }
    return channels;
}

inline bool prepare_workbench_project_color(DynamicPrintConfig& config,
    const std::vector<AI::PhysicalFilamentChannel>& channels, size_t slot, std::string color,
    bool& changed, std::string& error)
{
    changed = false;
    error.clear();
    if (!workbench_physical_slot_exists(channels, slot)) {
        error = "The selected physical filament slot no longer exists.";
        return false;
    }
    if (color.size() != 7 || color.front() != '#' ||
        !std::all_of(color.begin() + 1, color.end(), [](unsigned char value) { return std::isxdigit(value); })) {
        error = "Invalid filament color; select an RGB color.";
        return false;
    }
    std::transform(color.begin(), color.end(), color.begin(),
        [](unsigned char value) { return char(std::toupper(value)); });
    auto* colors = config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || slot >= colors->values.size()) {
        error = "The project filament color configuration is incomplete.";
        return false;
    }
    auto previous = colors->values[slot];
    std::transform(previous.begin(), previous.end(), previous.begin(),
        [](unsigned char value) { return char(std::toupper(value)); });
    if (previous == color) return true;
    colors->values[slot] = std::move(color);
    changed = true;
    return true;
}

} // namespace Slic3r::GUI
