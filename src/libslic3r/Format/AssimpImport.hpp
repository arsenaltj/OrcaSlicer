#pragma once

#include <string>
#include <array>
#include <vector>

namespace Slic3r {

struct TexturedMesh;

// FallbackOnly omits raw colors when the same import supplies converted vertex
// colors. Existing painting/saving callers keep the raw output by default.
enum class AssimpRawColorPolicy { Always, FallbackOnly };

bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message = nullptr,
                               std::vector<std::array<float, 4>>* raw_vertex_colors = nullptr,
                               AssimpRawColorPolicy raw_color_policy = AssimpRawColorPolicy::Always);

} // namespace Slic3r
