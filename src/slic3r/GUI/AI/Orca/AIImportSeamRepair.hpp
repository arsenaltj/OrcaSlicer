#pragma once

#include "libslic3r/MeshSeamRepair.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

// Called only after the native texture/filament matcher has assigned faces.
// Exact positional seams can be joined without moving a corner or changing
// triangle order, so all painted facet indexes remain valid. Real holes are
// left unchanged for explicit repair.
inline size_t stitch_ai_import_seams(ModelObject& object)
{
    size_t stitched = 0;
    for (ModelVolume* volume : object.volumes) {
        if (!volume || !volume->is_model_part() || its_num_open_edges(volume->mesh().its) == 0) continue;
        TriangleMesh candidate = volume->mesh();
        if (!stitch_exact_mesh_seams(candidate)) continue;
        volume->set_mesh(std::move(candidate));
        volume->set_new_unique_id();
        ++stitched;
    }
    return stitched;
}

} // namespace Slic3r::GUI
