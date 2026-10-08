#pragma once

#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include <map>

namespace Slic3r::GUI {

struct BeautyColorPaintResult {
    AI::SurfaceSelectionPersistence::FaceColorOverrides colors;
    size_t changed {0};
};

// Fill and brush share the same explicit face intent. Protection remains in
// force for painting; unlike selection repair, a brush never unlocks a face.
inline BeautyColorPaintResult paint_beauty_face_colors(
    const AI::SurfaceSelectionPersistence::FaceColorOverrides& before,
    const AI::SurfaceSelectionPersistence::SelectionState& selection,
    const std::vector<size_t>& faces, const std::array<float, 3>& color)
{
    BeautyColorPaintResult result {before, 0};
    for (float channel : color)
        if (!std::isfinite(channel) || channel < 0.f || channel > 1.f) return result;
    std::map<size_t, std::array<float, 3>> colors(before.begin(), before.end());
    for (size_t face : faces) {
        if (face >= selection.selected.size() || !selection.selected[face] ||
            (face < selection.protected_faces.size() && selection.protected_faces[face])) continue;
        auto found = colors.find(face);
        if (found != colors.end() && found->second == color) continue;
        colors[face] = color;
        ++result.changed;
    }
    if (result.changed) result.colors.assign(colors.begin(), colors.end());
    return result;
}

} // namespace Slic3r::GUI
