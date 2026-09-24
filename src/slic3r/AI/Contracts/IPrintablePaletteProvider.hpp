#pragma once

#include "slic3r/AI/Contracts/ColorIntent.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI {

// Optional process/material evidence used by isolated local color workers.
// Existing palette callers may leave these fields empty.
struct PrintSublayerMaterial {
    PhysicalFilamentChannel channel;
    std::string identity;
    double min_layer_mm {0}, max_layer_mm {0}, line_width_mm {0};
    double temperature_c {0}, min_temperature_c {0}, max_temperature_c {0};
};
struct PrintSublayerProcess {
    std::string material_fingerprint, process_fingerprint;
    std::string surface_condition, measurement_condition;
    bool sublayers_enabled {false};
    bool first_layer_unsplit {true};
    std::vector<double> layer_heights_mm;
    double z_resolution_mm {0};
    double region_width_mm {0}, region_height_mm {0};
};

struct PrintablePaletteSnapshot
{
    std::vector<std::string> project_colors;
    std::vector<size_t>      valid_slots;
    std::vector<size_t>      compatible_slots;
    std::vector<std::string> compatible_colors;

    // Typed capability source. The flat fields above remain as a temporary
    // compatibility projection for existing model-generation callers.
    std::vector<PhysicalFilamentChannel> physical_channels;
    std::vector<MixedColorRecipe>        mixed_recipes;
    std::vector<ColorOutputMode>         supported_output_modes {ColorOutputMode::DiscreteFilament};

    bool supports(ColorOutputMode mode) const noexcept
    {
        return std::find(supported_output_modes.begin(), supported_output_modes.end(), mode) !=
               supported_output_modes.end();
    }

    bool rebuild_legacy_projection()
    {
        project_colors.clear();
        valid_slots.clear();
        compatible_slots.clear();
        compatible_colors.clear();
        if (!is_valid_physical_channel_set(physical_channels))
            return false;

        for (const PhysicalFilamentChannel& channel : physical_channels) {
            if (project_colors.size() <= channel.slot)
                project_colors.resize(channel.slot + 1);
            project_colors[channel.slot] = channel.display_color;
            valid_slots.push_back(channel.slot);
            if (!channel.compatible)
                continue;
            compatible_slots.push_back(channel.slot);
            if (std::find(compatible_colors.begin(), compatible_colors.end(), channel.display_color) ==
                compatible_colors.end())
                compatible_colors.push_back(channel.display_color);
        }
        return true;
    }
};

class IPrintablePaletteProvider
{
public:
    virtual ~IPrintablePaletteProvider() = default;

    virtual PrintablePaletteSnapshot printable_palette() const = 0;
};

} // namespace Slic3r::AI
