#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Color.hpp"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r::AI {

// Shared model-space substrate. Patches label existing faces; they never split
// the mesh or limit the precision of a hand-painted face selection.
struct BeautyPatch {
    std::vector<size_t> faces;
    std::vector<uint32_t> neighbors;
    Vec3d center {Vec3d::Zero()}, normal {Vec3d::Zero()};
    std::array<double, 3> mean_color {};
    double area {0};
};

struct BeautySurface {
    static constexpr unsigned algorithm_version = 1;
    std::string geometry_id;
    std::vector<uint32_t> face_patch;
    std::vector<BeautyPatch> patches;
    std::vector<std::array<int32_t, 3>> face_neighbors;
    // Topology preflight on position-welded edges; UV seams stay untouched.
    size_t boundary_edges {0}, nonmanifold_edges {0};
    std::vector<Vec3d> centers, normals;
    std::vector<double> areas;
    // Geometrically identical UV-seam vertices share a class only for surface
    // operations. Source indices and UVs are not rewritten.
    std::vector<uint32_t> vertex_class, class_representative;
    std::vector<size_t> neighbor_offsets;
    std::vector<uint32_t> vertex_neighbors;
    std::vector<float> edge_lengths;

    static std::shared_ptr<BeautySurface> build(const indexed_triangle_set& mesh,
        const std::vector<RGBA>& colors, const std::vector<uint32_t>& saved_partition = {},
        const std::function<bool()>& canceled = {});

    // Appearance feathering stays inside the selected surface. Geometry uses
    // separate continuous vertex weights and freezes every outside/protected
    // incident vertex, including duplicates across UV seams.
    std::vector<float> face_weights(const indexed_triangle_set& mesh,
        const std::vector<uint8_t>& selected, const std::vector<uint8_t>& protected_faces,
        double feather_mm) const;
    std::vector<float> vertex_weights(const indexed_triangle_set& mesh,
        const std::vector<uint8_t>& selected, const std::vector<uint8_t>& protected_faces,
        double falloff_mm) const;
    indexed_triangle_set deform(const indexed_triangle_set& mesh,
        const std::vector<uint8_t>& selected, const std::vector<uint8_t>& protected_faces,
        double displacement_mm, double falloff_mm, size_t& moved_vertices,
        const std::function<bool()>& canceled = {}) const;
};

} // namespace Slic3r::AI
