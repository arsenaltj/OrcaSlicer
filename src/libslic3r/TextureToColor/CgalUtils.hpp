#pragma once
#include "libslic3r/Point.hpp"
#include "libslic3r/MeshResourceBudget.hpp"
#include "TriMesh.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Surface_mesh/Surface_mesh.h>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <cstdio>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

namespace Slic3r { namespace tex2color {
namespace cgalutils {

using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CGALMesh = CGAL::Surface_mesh<Kernel::Point_3>;

struct FloatMoveValidation {
    const char* reason = "valid";
    std::size_t face = std::numeric_limits<std::size_t>::max();
    std::size_t other_face = std::numeric_limits<std::size_t>::max();
    bool original_pair_intersects = false;
};

// Traverse immutable node bounds only. The visitor tests the final triangles
// with the same exact predicate as the full CGAL self-intersection check;
// intersecting each old triangle with a query cuboid first is redundant.
template<class Tree, class Visitor>
struct MovedFaceTraversal {
    Visitor& visitor;
    bool go_further() const { return true; }
    void intersection(const CGAL::Bbox_3&, const typename Tree::Primitive& primitive) {
        visitor(primitive.id());
    }
    bool do_intersect(const CGAL::Bbox_3& query, const CGAL::AABB_node<typename Tree::AABB_traits>& node) const {
        return CGAL::do_overlap(query, node.bbox());
    }
};

// The immutable tree already used by projection bounds every unchanged face.
// Only faces incident to an actually moved vertex can gain an intersection.
// Expand the final query by the largest movement on each axis, so the old
// bounds also contain candidates which moved into that query. Validate float
// export coordinates, without rebuilding or checking the entire mesh again.
template<class Tree>
bool validate_float_vertex_moves(CGALMesh& mesh, const CGALMesh& original,
    const Tree& original_tree, const std::vector<CGALMesh::Vertex_index>& vertices,
    const std::function<bool()>& canceled = {}, FloatMoveValidation* diagnostic = nullptr,
    tbb::task_arena* query_arena = nullptr)
{
    if (diagnostic) *diagnostic = {};
    const auto reject = [&](const char* reason) {
        if (diagnostic) diagnostic->reason = reason;
        return false;
    };
    if (canceled && canceled()) return reject("canceled");
    std::array<double, 3> movement {};
    std::vector<std::uint8_t> visited(mesh.num_faces(), 0);
    std::vector<CGALMesh::Face_index> changed_faces;
    for (auto vertex : vertices) {
        if (canceled && canceled()) return reject("canceled");
        if (!mesh.is_valid(vertex) || mesh.is_removed(vertex) ||
            !original.is_valid(vertex) || original.is_removed(vertex)) return reject("invalid-vertex");
        const auto& point = mesh.point(vertex);
        const std::array<float, 3> rounded {float(point.x()), float(point.y()), float(point.z())};
        if (!std::all_of(rounded.begin(), rounded.end(), [](float v) { return std::isfinite(v); })) return reject("non-finite-output");
        mesh.point(vertex) = Kernel::Point_3(rounded[0], rounded[1], rounded[2]);
        const auto& before = original.point(vertex);
        const auto& after = mesh.point(vertex);
        if (before == after) continue;
        for (int axis = 0; axis < 3; ++axis)
            movement[axis] = std::max(movement[axis], std::nextafter(
                std::abs(CGAL::to_double(after[axis] - before[axis])), std::numeric_limits<double>::infinity()));
        const auto halfedge = mesh.halfedge(vertex);
        if (halfedge == CGALMesh::null_halfedge()) return reject("missing-halfedge");
        for (auto h : mesh.halfedges_around_target(halfedge)) {
            const auto face = mesh.face(h);
            if (face != CGALMesh::null_face() && !visited[face]) {
                visited[face] = 1; changed_faces.push_back(face);
            }
        }
    }
    const Kernel kernel;
    const auto triangle = [&](CGALMesh::Face_index face) {
        const auto h = mesh.halfedge(face);
        return kernel.construct_triangle_3_object()(mesh.point(mesh.target(h)),
            mesh.point(mesh.target(mesh.next(h))), mesh.point(mesh.source(h)));
    };
    // Dense local checks revisit each neighbor's box many times. Compute the
    // exact final boxes once, without changing query or predicate order. A
    // sparse edit keeps the former path, and the optional array never exceeds
    // one transient block. Larger meshes retain the bounded traversal below.
    std::unique_ptr<CGAL::Bbox_3[]> final_bounds;
    if (!changed_faces.empty() && changed_faces.size() >= mesh.num_faces() / 8 &&
        mesh.num_faces() <= MeshResourceBudget::block_bytes / sizeof(CGAL::Bbox_3)) {
        final_bounds = std::make_unique<CGAL::Bbox_3[]>(mesh.num_faces());
        for (auto face : mesh.faces()) {
            if ((size_t(face) & 4095) == 0 && canceled && canceled()) return reject("canceled");
            final_bounds[size_t(face)] = triangle(face).bbox();
        }
    }
    const auto bounds_for = [&](CGALMesh::Face_index face) {
        return final_bounds ? final_bounds[size_t(face)] : triangle(face).bbox();
    };
    for (auto face : changed_faces) {
        if (canceled && canceled()) return reject("canceled");
        if (triangle(face).is_degenerate()) {
            if (diagnostic) diagnostic->face = std::size_t(face);
            return reject("degenerate-output-face");
        }
    }
    struct Rejected {};
    // Queries read only the rounded final mesh and the already-built immutable
    // tree. Keep failures private, then select the first in the former face
    // order. Cancellation and diagnostics are committed by the calling thread.
    const auto check_face = [&](CGALMesh::Face_index face, FloatMoveValidation& failure,
                                std::size_t& queries, bool poll_cancel) {
        try {
            const auto bounds = bounds_for(face);
            std::array<double, 3> low, high;
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::nextafter(bounds.min(axis) - movement[axis], -std::numeric_limits<double>::infinity());
                high[axis] = std::nextafter(bounds.max(axis) + movement[axis], std::numeric_limits<double>::infinity());
                if (!std::isfinite(low[axis]) || !std::isfinite(high[axis])) {
                    failure.reason = "non-finite-query";
                    return false;
                }
            }
            const CGAL::Bbox_3 query(low[0], low[1], low[2], high[0], high[1], high[2]);
            const auto visit = [&](auto other) {
                if ((++queries & 4095) == 0 && poll_cancel && canceled && canceled()) {
                    failure.reason = "canceled";
                    throw Rejected {};
                }
                // A pair of changed faces needs one exact test. Unchanged
                // faces are always tested from the changed endpoint.
                if (face == other || (visited[other] && other < face) ||
                    !CGAL::do_overlap(bounds, bounds_for(other))) return;
                if (CGAL::Polygon_mesh_processing::internal::do_faces_intersect<Kernel>(
                        face, other, mesh, mesh.points(), kernel.construct_segment_3_object(),
                        kernel.construct_triangle_3_object(), kernel.do_intersect_3_object())) {
                    failure.reason = "intersecting-output-faces";
                    failure.face = std::size_t(face);
                    failure.other_face = std::size_t(other);
                    if (diagnostic)
                        failure.original_pair_intersects = CGAL::Polygon_mesh_processing::internal::do_faces_intersect<Kernel>(
                            face, other, original, original.points(), kernel.construct_segment_3_object(),
                            kernel.construct_triangle_3_object(), kernel.do_intersect_3_object());
                    throw Rejected {};
                }
            };
            MovedFaceTraversal<Tree, decltype(visit)> traversal {visit};
            original_tree.traversal(query, traversal);
        } catch (const Rejected&) {
            return false;
        }
        return true;
    };
    constexpr std::size_t batch_size = 512;
    if (query_arena && query_arena->max_concurrency() > 1 && changed_faces.size() >= 256) {
        std::array<FloatMoveValidation, batch_size> failures;
        std::array<std::uint8_t, batch_size> valid;
        for (std::size_t begin = 0; begin < changed_faces.size(); begin += batch_size) {
            if (canceled && canceled()) return reject("canceled");
            const auto count = std::min(batch_size, changed_faces.size() - begin);
            query_arena->execute([&] {
                tbb::parallel_for(tbb::blocked_range<std::size_t>(0, count, 32),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                        for (auto index = range.begin(); index != range.end(); ++index) {
                            failures[index] = {};
                            std::size_t queries = 0;
                            valid[index] = check_face(changed_faces[begin + index], failures[index], queries, false);
                        }
                    });
            });
            if (canceled && canceled()) return reject("canceled");
            for (std::size_t index = 0; index < count; ++index)
                if (!valid[index]) {
                    if (diagnostic) *diagnostic = failures[index];
                    return false;
                }
        }
    } else {
        std::size_t queries = 0;
        for (auto face : changed_faces) {
            if (canceled && canceled()) return reject("canceled");
            FloatMoveValidation failure;
            if (!check_face(face, failure, queries, true)) {
                if (diagnostic) *diagnostic = failure;
                return canceled && canceled() ? reject("canceled") : false;
            }
        }
    }
    return canceled && canceled() ? reject("canceled") : true;
}

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
