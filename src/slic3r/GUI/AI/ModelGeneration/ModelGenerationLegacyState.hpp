#pragma once

#include "slic3r/AI/Contracts/ColorIntent.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Slic3r::GUI {

// Old history still carries generation palettes, although new generation uses
// natural colors. Retain that input state without constructing hidden choices.
struct ModelGenerationLegacyState
{
    int palette_source { 1 }; // 0 project, 1 unrestricted, 2 recommended/custom
    size_t palette_color_count { AI::kLegacyDefaultTargetPaletteColors };
    double print_width_mm { 160.0 };
    double nozzle_mm { 0.4 };
    double line_width_mm { 0.4 };
    double minimum_feature_mm { 0.8 };

    void restore_palette_color_count(size_t count)
    {
        palette_color_count = AI::is_supported_target_palette_color_count(count)
            ? count : AI::kLegacyDefaultTargetPaletteColors;
    }

    void restore_print_settings(double width, double nozzle, double line_width, double minimum_feature)
    {
        print_width_mm = restore_number(width, 20.0, 2000.0, 1);
        nozzle_mm = restore_number(nozzle, 0.1, 2.0, 2);
        line_width_mm = restore_number(line_width, 0.1, 3.0, 2);
        minimum_feature_mm = restore_number(minimum_feature, 0.1, 20.0, 2);
    }

private:
    static double restore_number(double value, double minimum, double maximum, int digits)
    {
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
        if (!std::isfinite(value)) return value;
        // wxSpinCtrlDouble clamps, formats to its display precision, then parses
        // that text back. Its increment does not snap programmatic restoration.
        // Keep printf rounding, including ties; round(value * scale) differs.
        char text[64];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::strtod(text, nullptr);
    }
};

} // namespace Slic3r::GUI
