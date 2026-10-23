#pragma once
#include <optional>
#include <cstddef>
#include <vector>
#include "FilamentColorPack.hpp"
class wxWindow;
namespace Slic3r::GUI {
class Plater;
bool show_orca_filament_selection(wxWindow* parent, Plater& plater, size_t selected_slot = size_t(-1), bool add_slot = false,
    const std::vector<FilamentColorPack>& import_sources = {},
    std::optional<FilamentColorPack> preloaded = std::nullopt);
void delete_orca_printer_filament(wxWindow* parent, Plater& plater, size_t slot);
// An explicit physical-slot choice; returning never mutates the project.
std::optional<size_t> choose_single_color_filament(wxWindow* parent, Plater& plater);
}
