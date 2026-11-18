#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Slic3r::MeshResourceBudget {
// Capacity policy, not a repair-success criterion. Native source/final editor
// data are measured separately from transient partition/cache workspace.
inline constexpr uint64_t block_bytes = 16ULL * 1024 * 1024;
inline constexpr uint64_t scratch_bytes = 256ULL * 1024 * 1024;
inline constexpr uint64_t editor_bytes = 1024ULL * 1024 * 1024;
inline constexpr size_t vertex_bytes = 128, face_bytes = 96;
inline constexpr size_t maximum_vertices = size_t(editor_bytes / vertex_bytes);
inline constexpr size_t maximum_faces = size_t(editor_bytes / face_bytes);
inline constexpr uint64_t local_obj_bytes = 2ULL * 1024 * 1024 * 1024;
inline bool editable(size_t vertices, size_t faces) {
    return vertices > 0 && faces > 0 && vertices <= maximum_vertices && faces <= maximum_faces &&
        uint64_t(vertices) * vertex_bytes + uint64_t(faces) * face_bytes <= editor_bytes &&
        vertices <= size_t(std::numeric_limits<int>::max());
}
inline bool partition_workspace(size_t faces) {
    // Sorted edge uses, DSU, root-to-region lookup; final labels/geometry are
    // counted by the editor budget. Checked before allocating scratch.
    return faces > 0 && uint64_t(faces) <= scratch_bytes / 48;
}
}
