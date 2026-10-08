#pragma once

#include <array>

namespace Slic3r::GUI {

inline constexpr std::array<const char*, 24> default_filament_colour_palette = {
    "#FFFFFF", "#FFF144", "#DCF478", "#0ACC38", "#057748", "#0D6284",
    "#0EE2A0", "#76D9F4", "#46A8F9", "#2850E0", "#443089", "#A03CF7",
    "#F330F9", "#D4B1DD", "#F95D73", "#F72323", "#7C4B00", "#F98C36",
    "#FCECD6", "#D3C5A3", "#AF7933", "#898989", "#BCBCBC", "#161616"
};

}
