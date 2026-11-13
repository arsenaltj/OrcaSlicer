#pragma once

#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "ModelColorPreviewShader.hpp"
#include <array>
#include <utility>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Slic3r::GUI::ModelPreviewGeometry {

// CPU-only overlay preparation preserves the existing RGB8 sentinel and face
// order. A worker can freeze it before the GUI installs any model resources.
inline GLModel::Geometry manual_color_geometry(const indexed_triangle_set& mesh,
    const std::vector<Vec3f>& normals,
    const std::vector<std::pair<size_t, std::array<float, 3>>>& colors,
    const std::function<bool()>& canceled = {})
{
    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles,
        GLModel::Geometry::EVertexLayout::P3N3T2};
    if (colors.size() > std::numeric_limits<uint32_t>::max() / 3)
        throw std::runtime_error("Manual color preview is too large.");
    if (!colors.empty() && normals.size() != mesh.indices.size() * 3)
        throw std::runtime_error("Manual color normals do not match the model.");
    geometry.reserve_vertices(colors.size() * 3);
    geometry.reserve_indices(colors.size() * 3);
    for (size_t at = 0; at < colors.size(); ++at) {
        if ((at & 4095) == 0 && canceled && canceled())
            throw std::runtime_error("Render geometry preparation cancelled.");
        const auto& item = colors[at];
        if (item.first >= mesh.indices.size())
            throw std::runtime_error("Manual color face does not belong to the model.");
        const auto& face = mesh.indices[item.first];
        const auto packed = preview_rgb8(item.second[0], item.second[1], item.second[2]);
        const auto base = uint32_t(geometry.vertices_count());
        for (size_t corner = 0; corner < 3; ++corner) {
            if (face[corner] < 0 || size_t(face[corner]) >= mesh.vertices.size())
                throw std::runtime_error("Manual color vertex does not belong to the model.");
            geometry.add_vertex(mesh.vertices[face[corner]], normals[item.first * 3 + corner],
                Vec2f(float(packed), -1.f));
        }
        geometry.add_triangle(base, base + 1, base + 2);
    }
    if (canceled && canceled()) throw std::runtime_error("Render geometry preparation cancelled.");
    return geometry;
}

// CPU-only sharing for expanded historical preview geometry. Every attribute
// bit and triangle corner stays intact; consumers that edit per-face attributes
// must keep the expanded representation. Unsupported or costly inputs fall back.
inline std::optional<GLModel::Geometry> share_exact_vertices(
    const indexed_triangle_set& mesh, const GLModel::Geometry& raw,
    const std::function<bool()>& canceled = {})
{
    using Geometry = GLModel::Geometry;
    const auto stop = [&] {
        if (canceled && canceled()) throw std::runtime_error("Render geometry preparation cancelled.");
    };
    stop();
    constexpr size_t stride = 8;
    const size_t corners = raw.vertices.size() / stride;
    if (raw.format.type != Geometry::EPrimitiveType::Triangles ||
        raw.format.vertex_layout != Geometry::EVertexLayout::P3N3T2 ||
        raw.index_type != Geometry::EIndexType::UINT || raw.vertices.size() % stride != 0 ||
        corners == 0 || corners % 3 != 0 || corners > std::numeric_limits<uint32_t>::max() ||
        raw.indices.size() != corners || mesh.indices.size() != corners / 3 ||
        mesh.vertices.empty() || mesh.vertices.size() >= corners - corners / 4)
        return {};

    constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    Geometry output;
    output.format = raw.format;
    output.color = raw.color;
    output.index_type = raw.index_type;
    const size_t reserve = std::min(corners, mesh.vertices.size() + mesh.vertices.size() / 10);
    output.reserve_vertices(reserve);
    output.reserve_indices(corners);
    std::vector<uint32_t> heads(mesh.vertices.size(), none), next;
    next.reserve(reserve);
    for (size_t corner = 0; corner < corners; ++corner) {
        if ((corner & 4095) == 0) stop();
        if (raw.indices[corner] != corner) return {};
        const int source = mesh.indices[corner / 3][corner % 3];
        if (source < 0 || size_t(source) >= heads.size()) return {};
        const float* vertex = raw.vertices.data() + corner * stride;
        uint32_t shared = heads[source];
        unsigned depth = 0;
        while (shared != none && std::memcmp(vertex, output.vertices.data() + shared * stride,
                                             stride * sizeof(float)) != 0) {
            if (++depth >= 8) return {};
            shared = next[shared];
        }
        if (shared == none) {
            if (next.size() >= corners - corners / 4) return {};
            // Dropping repeated signed zeros or nonfinite positions can change
            // the original bounds scan's bit pattern. Leave those inputs alone.
            for (size_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(vertex[axis]) || (vertex[axis] == 0.f && std::signbit(vertex[axis])))
                    return {};
            shared = uint32_t(next.size());
            output.vertices.insert(output.vertices.end(), vertex, vertex + stride);
            next.push_back(heads[source]);
            heads[source] = shared;
        }
        output.indices.push_back(shared);
    }
    stop();
    return output;
}

} // namespace Slic3r::GUI::ModelPreviewGeometry
