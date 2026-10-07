#pragma once

#include <string>
#include <array>
#include <vector>

namespace Slic3r {

struct TexturedMesh;

// FallbackOnly omits raw colors when the same import supplies converted vertex
// colors. Existing painting/saving callers keep the raw output by default.
enum class AssimpRawColorPolicy { Always, FallbackOnly };

// Metadata belongs to the imported material index, which need not be the
// document array index. Optional preview consumers use it alongside the mesh.
struct AssimpMaterialPreview {
    bool has_color_texture {false};
    std::string alpha_mode {"OPAQUE"};
    float alpha_cutoff {.5f};
    int wrap_s {10497}, wrap_t {10497}, min_filter {9729}, mag_filter {9729};
};

bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message = nullptr,
                               std::vector<std::array<float, 4>>* raw_vertex_colors = nullptr,
                               AssimpRawColorPolicy raw_color_policy = AssimpRawColorPolicy::Always,
                               std::vector<AssimpMaterialPreview>* preview_materials = nullptr);

} // namespace Slic3r
