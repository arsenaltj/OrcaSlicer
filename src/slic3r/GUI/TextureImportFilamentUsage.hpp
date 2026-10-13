#pragma once

#include "TextureImportDialog.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <set>

namespace Slic3r::GUI {

inline void configure_workbench_filament_capacity(TextureImportOptions& options,
    const DynamicPrintConfig& printer)
{
    if (!options.workbench_review) return;
    const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t count = nozzles ? nozzles->values.size() : 0;
    // A single nozzle may switch spools through a feeder. A multi-head machine
    // uses its physical heads; additional target colors need layered recipes.
    options.physical_filament_limit = count > 1 ? count : 0;
    options.allow_unverified_mixed_filaments = count > 1;
}

struct TextureImportFilamentUsage {
    std::set<size_t> physical, mixed, added;
    bool valid {true};
};

inline TextureImportFilamentUsage texture_import_filament_usage(
    const std::vector<TextureFilamentEntry>& entries, const std::vector<FilamentMatch>& matches)
{
    TextureImportFilamentUsage usage;
    const auto physical = [&](size_t index) {
        if (index >= entries.size() || (entries[index].kind != TextureFilamentKind::ExistingPhysical &&
            entries[index].kind != TextureFilamentKind::NewPhysical)) { usage.valid = false; return; }
        usage.physical.insert(index);
        if (entries[index].kind == TextureFilamentKind::NewPhysical) usage.added.insert(index);
    };
    for (const auto& match : matches) {
        if (match.filament_index < 0 || size_t(match.filament_index) >= entries.size()) {
            usage.valid = false; continue;
        }
        const size_t index = size_t(match.filament_index);
        const auto& entry = entries[index];
        if (entry.kind == TextureFilamentKind::ExistingPhysical || entry.kind == TextureFilamentKind::NewPhysical) {
            physical(index); continue;
        }
        usage.mixed.insert(index);
        if (entry.mixed_components.size() < 2 || entry.mixed_components.size() != entry.mixed_ratios.size()) {
            usage.valid = false; continue;
        }
        std::set<unsigned> components;
        int sum = 0;
        for (size_t i = 0; i < entry.mixed_components.size(); ++i) {
            const unsigned component = entry.mixed_components[i];
            if (!component || !components.insert(component).second || entry.mixed_ratios[i] <= 0 ||
                entry.mixed_ratios[i] > 100) { usage.valid = false; continue; }
            physical(size_t(component - 1));
            sum += entry.mixed_ratios[i];
        }
        if (sum != 100) usage.valid = false;
    }
    return usage;
}

} // namespace Slic3r::GUI
