#pragma once
#include "libslic3r/Point.hpp"
#include "TriMesh.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Surface_mesh/Surface_mesh.h>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Slic3r { namespace tex2color {
namespace cgalutils {

using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CGALMesh = CGAL::Surface_mesh<Kernel::Point_3>;

template<class Tree>
Kernel::Point_3 closest_point_with_strict_hint(const Tree& tree,
    const Kernel::Point_3& query, const Kernel::Point_3& hint)
{
    const auto projected = tree.closest_point(query, hint);
    // Keep the original traversal and representation when the seed itself
    // attains the minimum. Accept a seed-assisted result only after an exact
    // strict improvement, without relying on its equal-distance behavior.
    if (Kernel().compare_distance_3_object()(query, projected, hint) != CGAL::SMALLER)
        return tree.closest_point(query);
    // Keep the original representation for near-zero planar projections,
    // including signed zeros and the final float rounding.
    const double scale = std::max(1., CGAL::to_double((query - CGAL::ORIGIN).squared_length()));
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    if (CGAL::to_double(CGAL::squared_distance(query, projected)) <= 16. * epsilon * epsilon * scale)
        return tree.closest_point(query);
    return projected;
}

inline CGALMesh trimesh_to_cgal(const TriMesh& mesh) {
    CGALMesh cm;
    std::vector<CGALMesh::Vertex_index> vmap(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
        vmap[i] = cm.add_vertex(Kernel::Point_3(mesh.vertices[i].x(), mesh.vertices[i].y(), mesh.vertices[i].z()));
    for (const auto& f : mesh.indices) {
        cm.add_face(vmap[f[0]], vmap[f[1]], vmap[f[2]]);
    }
    return cm;
}

inline TriMesh cgal_to_trimesh(const CGALMesh& cm) {
    TriMesh mesh;
    std::map<CGALMesh::Vertex_index, size_t> vmap;
    size_t idx = 0;
    for (auto v : cm.vertices()) {
        if (!cm.is_valid(v) || cm.is_removed(v)) continue;
        auto p = cm.point(v);
        mesh.vertices.push_back(Vec3f((float)p.x(), (float)p.y(), (float)p.z()));
        vmap[v] = idx++;
    }
    for (auto f : cm.faces()) {
        if (!cm.is_valid(f) || cm.is_removed(f)) continue;
        auto h = cm.halfedge(f);
        auto v0 = cm.target(h);
        auto v1 = cm.target(cm.next(h));
        auto v2 = cm.target(cm.next(cm.next(h)));
        mesh.indices.push_back(Vec3i32((int)vmap[v0], (int)vmap[v1], (int)vmap[v2]));
    }
    return mesh;
}

inline bool is_mesh_halfedge_compatible(const TriMesh& mesh) {
    // A directed edge may occur once, and every vertex must have one connected
    // fan. Keep the existing predicate, without a hash container per vertex,
    // edge and face. No geometric tolerance or coordinate is changed here.
    constexpr auto missing = std::numeric_limits<std::size_t>::max();
    struct Edge { std::uint64_t key; std::size_t halfedge; };
    std::vector<Edge> edges;
    edges.reserve(mesh.indices.size() * 3);
    std::vector<std::size_t> offsets(mesh.vertices.size() + 1, 0);
    for (std::size_t face = 0; face < mesh.indices.size(); ++face) {
        const auto& triangle = mesh.indices[face];
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[2] == triangle[0]) return false;
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto vertex = std::size_t(triangle[corner]);
            if (vertex >= mesh.vertices.size()) return false;
            ++offsets[vertex + 1];
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto from = std::uint32_t(triangle[corner]);
            const auto to = std::uint32_t(triangle[(corner + 1) % 3]);
            edges.push_back({(std::uint64_t(std::min(from, to)) << 32) | std::max(from, to), face * 3 + corner});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& left, const Edge& right) { return left.key < right.key; });
    std::vector<std::array<std::size_t, 3>> neighbors(mesh.indices.size(), {missing, missing, missing});
    for (std::size_t begin = 0; begin < edges.size();) {
        auto end = begin + 1;
        while (end < edges.size() && edges[end].key == edges[begin].key) ++end;
        if (end - begin > 2) return false;
        if (end - begin == 2) {
            const auto first = edges[begin].halfedge, second = edges[begin + 1].halfedge;
            const auto& a = mesh.indices[first / 3];
            const auto& b = mesh.indices[second / 3];
            if ((a[first % 3] < a[(first + 1) % 3]) == (b[second % 3] < b[(second + 1) % 3])) return false;
            neighbors[first / 3][first % 3] = second / 3;
            neighbors[second / 3][second % 3] = first / 3;
        }
        begin = end;
    }
    std::vector<Edge>().swap(edges);
    for (std::size_t vertex = 1; vertex < offsets.size(); ++vertex) offsets[vertex] += offsets[vertex - 1];
    std::vector<std::size_t> incident(offsets.back());
    auto cursor = offsets;
    for (std::size_t face = 0; face < mesh.indices.size(); ++face)
        for (auto vertex : mesh.indices[face]) incident[cursor[std::size_t(vertex)]++] = face;
    std::vector<std::size_t> seen(mesh.indices.size(), missing), stack;
    for (std::size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        if (offsets[vertex] == offsets[vertex + 1]) continue;
        const auto first = incident[offsets[vertex]];
        stack.assign(1, first);
        seen[first] = vertex;
        std::size_t connected = 0;
        while (!stack.empty()) {
            const auto face = stack.back(); stack.pop_back(); ++connected;
            for (auto adjacent : neighbors[face]) {
                if (adjacent == missing || seen[adjacent] == vertex) continue;
                const auto& triangle = mesh.indices[adjacent];
                if (std::size_t(triangle[0]) == vertex || std::size_t(triangle[1]) == vertex || std::size_t(triangle[2]) == vertex) {
                    seen[adjacent] = vertex;
                    stack.push_back(adjacent);
                }
            }
        }
        if (connected != offsets[vertex + 1] - offsets[vertex]) return false;
    }
    return true;
}

inline bool convert_trimesh_to_cgal(const TriMesh& mesh, CGALMesh& cgal_mesh) {
    cgal_mesh = trimesh_to_cgal(mesh);
    return cgal_mesh.number_of_faces() > 0 || mesh.indices.empty();
}

inline bool convert_trimesh_to_cgal(
    const TriMesh& mesh, const std::vector<Vec2f>& vertex_uvs,
    CGALMesh& cgal_mesh, std::vector<Vec2f>& cgal_vertex_uvs)
{
    cgal_mesh.clear();
    std::vector<CGALMesh::Vertex_index> vmap(mesh.vertices.size());
    cgal_vertex_uvs.clear();

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        vmap[i] = cgal_mesh.add_vertex(Kernel::Point_3(
            mesh.vertices[i].x(), mesh.vertices[i].y(), mesh.vertices[i].z()));
    }

    cgal_vertex_uvs.resize(cgal_mesh.num_vertices());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (i < vertex_uvs.size())
            cgal_vertex_uvs[vmap[i]] = vertex_uvs[i];
        else
            cgal_vertex_uvs[vmap[i]] = Vec2f(0.f, 0.f);
    }

    for (const auto& f : mesh.indices)
        cgal_mesh.add_face(vmap[f[0]], vmap[f[1]], vmap[f[2]]);

    return true;
}

} // namespace cgalutils
} // namespace tex2color
} // namespace Slic3r
