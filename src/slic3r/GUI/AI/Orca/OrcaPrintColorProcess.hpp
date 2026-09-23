#pragma once

#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <limits>

namespace Slic3r::GUI {

// Main-thread snapshot of the active plate's effective routing. Automatic modes
// have no final assignment yet: their profile limits must hold on every nozzle.
// This is a capability envelope, never a claim about an actual layer's tool.
struct PrintColorNozzleRouting {
    FilamentMapMode mode {fmmDefault};
    std::vector<int> filament_maps; // logical material slot -> 1-based extruder
};


// Pure extraction used by the live adapter and exercised with native config
// objects. This deliberately does not turn profile defaults into a sliced
// surface proof, or assume a material slot is a physical extruder index.
inline void capture_print_color_process(AI::PrintablePaletteSnapshot& snapshot,
    const DynamicPrintConfig& printer, const DynamicPrintConfig& print,
    const std::vector<const DynamicPrintConfig*>& filaments,
    const std::vector<std::string>& identities,
    const PrintColorNozzleRouting* routing = nullptr)
{
    auto& process = snapshot.sublayer_process;
    process = {};
    process.material_fingerprint = snapshot.material_fingerprint;
    process.process_fingerprint = snapshot.process_fingerprint;
    if (const auto* enabled = print.option<ConfigOptionBool>("enable_mixed_color_sublayer"))
        process.sublayers_enabled = enabled->value;
    if (const auto* height = print.option<ConfigOptionFloat>("layer_height"))
        if (std::isfinite(height->value) && height->value > 0) process.layer_heights_mm = {height->value};

    const auto* nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t nozzle_count = nozzles ? nozzles->values.size() : 0;
    auto positive = [](double value) { return std::isfinite(value) && value > 0; };
    auto nozzle_indices = [&](size_t slot) {
        std::vector<size_t> indices;
        if (nozzle_count == 1) indices.push_back(0);
        else if (routing && (routing->mode == fmmAutoForFlush || routing->mode == fmmAutoForMatch)) {
            for (size_t i = 0; i < nozzle_count; ++i) indices.push_back(i);
        } else if (routing && (routing->mode == fmmManual || routing->mode == fmmNozzleManual) &&
                   slot < routing->filament_maps.size()) {
            const int mapped = routing->filament_maps[slot];
            if (mapped > 0 && size_t(mapped) <= nozzle_count) indices.push_back(size_t(mapped - 1));
        }
        return indices;
    };
    const auto* minimum = printer.option<ConfigOptionFloats>("min_layer_height");
    const auto* maximum = printer.option<ConfigOptionFloats>("max_layer_height");
    const auto* width = print.option<ConfigOptionFloatOrPercent>("line_width");
    snapshot.sublayer_materials.clear();
    for (size_t i = 0; i < snapshot.physical_channels.size(); ++i) {
        AI::PrintSublayerMaterial material;
        material.channel = snapshot.physical_channels[i];
        material.identity = i < identities.size() ? identities[i] : "";
        const auto indices = nozzle_indices(material.channel.slot);
        bool limits_known = !indices.empty() && minimum && maximum &&
            minimum->values.size() == nozzle_count && maximum->values.size() == nozzle_count;
        double min_height = 0, max_height = std::numeric_limits<double>::infinity(), line_width = 0;
        for (size_t nozzle : indices) {
            if (!limits_known || !positive(nozzles->values[nozzle]) ||
                !positive(minimum->values[nozzle]) || !positive(maximum->values[nozzle]) ||
                minimum->values[nozzle] > maximum->values[nozzle]) { limits_known = false; break; }
            min_height = std::max(min_height, minimum->values[nozzle]);
            max_height = std::min(max_height, maximum->values[nozzle]);
            if (width && positive(width->value))
                line_width = std::max(line_width, width->percent ? width->value * nozzles->values[nozzle] / 100. : width->value);
        }
        if (limits_known && min_height <= max_height) {
            material.min_layer_mm = min_height;
            material.max_layer_mm = max_height;
            material.line_width_mm = positive(line_width) ? line_width : 0.;
        }
        // Auto-width and absent/malformed per-nozzle limits remain unknown.
        // Region-specific widths and actual layer tool choices need a slice proof.
        const auto* config = i < filaments.size() ? filaments[i] : nullptr;
        auto temperature = [&](const char* key) {
            const auto* option = config ? config->option<ConfigOptionInts>(key) : nullptr;
            return option && !option->values.empty() ? double(option->get_at(0)) : 0.;
        };
        material.temperature_c = temperature("nozzle_temperature");
        material.min_temperature_c = temperature("nozzle_temperature_range_low");
        material.max_temperature_c = temperature("nozzle_temperature_range_high");
        snapshot.sublayer_materials.push_back(std::move(material));
    }
    // No Z-step setting, batch/measurement record or minimum region dimensions
    // exist in these two profile configs. Leave those fields unknown; a model
    // bounding box is not evidence of a small colored feature's printable size.
}
} // namespace Slic3r::GUI
