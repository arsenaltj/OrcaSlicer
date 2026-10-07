#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

inline AI::ModelArtifactTextureSurface::Face preview_texture_face_color(
    AI::ModelArtifactTextureSurface::Face face, const std::array<float, 3>* color)
{
    if (!color) return face;
    face.image = -1;
    face.alpha_mode = AI::ModelArtifactTextureSurface::AlphaMode::Opaque;
    for (auto& corner : face.corners) {
        // The texture shader consumes linear material factors; saved edits are sRGB.
        for (size_t channel = 0; channel < 3; ++channel) {
            const float value = std::clamp((*color)[channel], 0.f, 1.f);
            corner.multiplier[channel] = value <= .04045f ? value / 12.92f
                : std::pow((value + .055f) / 1.055f, 2.4f);
        }
        corner.multiplier[3] = 1.f;
    }
    return face;
}

} // namespace Slic3r::GUI
