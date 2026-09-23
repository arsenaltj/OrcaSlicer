#pragma once

#include "LocalPrintColorApplication.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include <type_traits>

namespace Slic3r::GUI::LocalPrintColorCommit {

// Construct on the UI thread before history capture. Config cloning, routing
// allocation and geometry validation must finish before either live half moves.
struct Prepared {
    ModelConfig volume_config, object_config;
    LocalPrintColorApplication::PreparedPainting painting;
    ObjectID volume_id, object_id;
    uint64_t paint_stamp {0}, volume_stamp {0}, object_stamp {0};
    std::string old_source, source;
    bool single_volume {false}, consumed {false};
};

inline bool prepare(const ModelVolume& volume, LocalPrintColorApplication::PreparedPainting painting,
    std::string source, Prepared& destination, std::string& error)
{
    error.clear();
    const auto* object = volume.get_object();
    if (!object || !volume.is_model_part() || source.empty() || painting.base_extruder < 1 ||
        painting.base_extruder > 32 ||
        AI::SurfaceSelectionPersistence::geometry_fingerprint(volume.mesh().its) != painting.geometry_id) {
        error = "Model geometry or color routing changed before preparation.";
        return false;
    }
    Prepared prepared;
    prepared.volume_id = volume.id(); prepared.object_id = object->id();
    prepared.paint_stamp = volume.mmu_segmentation_facets.timestamp();
    prepared.volume_stamp = static_cast<const ModelConfig&>(volume.config).timestamp();
    prepared.object_stamp = static_cast<const ModelConfig&>(object->config).timestamp();
    prepared.old_source = volume.source.input_file;
    prepared.source = std::move(source);
    prepared.single_volume = object->volumes.size() == 1;
    prepared.volume_config.assign_config(volume.config.get());
    prepared.volume_config.set("extruder", painting.base_extruder);
    if (prepared.single_volume) {
        prepared.object_config.assign_config(object->config.get());
        prepared.object_config.set("extruder", painting.base_extruder);
    }
    prepared.painting = std::move(painting);
    destination = std::move(prepared);
    return true;
}

inline bool validate(const ModelVolume& volume, const Prepared& prepared, std::string& error)
{
    error.clear();
    const auto* object = volume.get_object();
    if (prepared.consumed || !object || volume.id() != prepared.volume_id || object->id() != prepared.object_id ||
        (object->volumes.size() == 1) != prepared.single_volume || volume.source.input_file != prepared.old_source ||
        volume.mmu_segmentation_facets.timestamp() != prepared.paint_stamp ||
        static_cast<const ModelConfig&>(volume.config).timestamp() != prepared.volume_stamp ||
        static_cast<const ModelConfig&>(object->config).timestamp() != prepared.object_stamp ||
        AI::SurfaceSelectionPersistence::geometry_fingerprint(volume.mesh().its) != prepared.painting.geometry_id) {
        error = "Prepared model colors, geometry or settings changed; reopen the current model.";
        return false;
    }
    return true;
}

// Called immediately after validate/history capture without event dispatch.
// Native rvalue config assignment moves DynamicPrintConfig (noexcept), updates
// a scalar timestamp and clears the moved-from map; paint data is moved too.
inline void commit(ModelVolume& volume, Prepared& prepared) noexcept
{
    static_assert(std::is_nothrow_move_assignable_v<TriangleSelector::TriangleSplittingData>);
    if (prepared.consumed) return;
    volume.config.assign_config(std::move(prepared.volume_config));
    if (prepared.single_volume) volume.get_object()->config.assign_config(std::move(prepared.object_config));
    volume.mmu_segmentation_facets.set_data(std::move(prepared.painting.data));
    volume.source.input_file.swap(prepared.source);
    prepared.consumed = true;
}

inline void commit(ModelVolume& volume, Prepared& painting, UndoRedo::ProjectConfigUndo::Prepared& config,
    ProjectConfigRestore::ColorCache& cache, PresetBundle& bundle) noexcept
{
    if (painting.consumed) return;
    ProjectConfigRestore::commit(config, cache, bundle);
    commit(volume, painting);
}
} // namespace Slic3r::GUI::LocalPrintColorCommit
