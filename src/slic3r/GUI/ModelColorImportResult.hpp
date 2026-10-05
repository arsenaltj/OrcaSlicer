#pragma once

#include "libslic3r/Model.hpp"
#include <set>
#include <optional>

namespace Slic3r::GUI {

// Feedback from native texture matching, including virtual mixed filament IDs.
struct ModelColorImportResult {
    bool cancelled { false };
    std::string error;
    std::optional<size_t> single_color_filament;
    bool colors_applied { false };
    size_t source_color_count { 0 };
    size_t mapped_color_count { 0 };
};

// Apply to the private incoming model before it reaches the live project.
inline bool apply_single_color_import(Model& model, size_t slot)
{
    if (slot >= static_cast<size_t>(EnforcerBlockerType::ExtruderMax) || model.objects.empty()) return false;
    for (ModelObject* object : model.objects) {
        object->config.set_key_value("extruder", new ConfigOptionInt(int(slot + 1)));
        for (ModelVolume* volume : object->volumes) {
            volume->config.set_key_value("extruder", new ConfigOptionInt(int(slot + 1)));
            volume->mmu_segmentation_facets.reset();
        }
    }
    return true;
}

inline void collect_model_color_import_result(ModelColorImportResult* result, const Model& model, size_t source_colors)
{
    if (result == nullptr)
        return;
    std::set<size_t> used_filaments;
    for (const ModelObject* object : model.objects)
        for (const ModelVolume* volume : object->volumes) {
            const auto ids = volume->get_extruders_from_multi_material_painting();
            used_filaments.insert(ids.begin(), ids.end());
        }
    result->source_color_count = source_colors;
    result->mapped_color_count = used_filaments.size();
    result->colors_applied = !used_filaments.empty();
}

} // namespace Slic3r::GUI
