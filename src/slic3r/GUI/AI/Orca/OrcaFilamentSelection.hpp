#pragma once
#include <optional>
#include <cstddef>
class wxWindow;
namespace Slic3r::GUI {
class Plater;
void show_orca_filament_selection(wxWindow* parent, Plater& plater);
// An explicit physical-slot choice; returning never mutates the project.
std::optional<size_t> choose_single_color_filament(wxWindow* parent, Plater& plater);
}
