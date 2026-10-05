#pragma once

#include "libslic3r/MeshSeamRepair.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

// Called only after the native texture/filament matcher has assigned faces.
// Exact positional seams can be joined without moving a corner or changing
// triangle order, so all painted facet indexes remain valid. Real holes are
// left unchanged for explicit repair.
struct AIImportSeamResult {
    size_t stitched_volumes = 0;
    bool has_open_edges = false;
};

inline AIImportSeamResult stitch_ai_import_seams(ModelObject& object)
{
    AIImportSeamResult result;
    for (ModelVolume* volume : object.volumes) {
        // Once an opening is known, untouched modifiers cannot change the
        // status. Model parts must still be visited for possible seam repair.
        if (!volume || (!volume->is_model_part() && result.has_open_edges)) continue;
        if (its_num_open_edges(volume->mesh().its) == 0) continue;
        if (!volume->is_model_part()) {
            result.has_open_edges = true;
            continue;
        }
        TriangleMesh candidate = volume->mesh();
        if (!stitch_exact_mesh_seams(candidate)) {
            result.has_open_edges = true;
            continue;
        }
        // Success already proves the candidate has no open edges. Reuse that
        // result in this transaction, not a cache tied to a mutable mesh.
        volume->set_mesh(std::move(candidate));
        volume->set_new_unique_id();
        ++result.stitched_volumes;
    }
    return result;
}

} // namespace Slic3r::GUI
