#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

namespace Slic3r::GUI::ModelPreviewNormals {

// Smooth only across a consistently oriented, manifold edge below the crease
// angle. A shared vertex alone never connects two surfaces or thin back faces.
inline std::vector<Vec3f> corner_normals(const indexed_triangle_set& mesh, float crease_degrees = 45.f,
                                       const std::function<void(const char*)>& trace = {})
{
    struct Edge { uint64_t key; uint32_t face; uint8_t side; };
    const size_t faces = mesh.indices.size();
    bool isolated_corners = true;
    for (size_t face = 0; face < faces; ++face) {
        const auto& f = mesh.indices[face];
        if (size_t(f[0]) != face * 3 || size_t(f[1]) != face * 3 + 1 || size_t(f[2]) != face * 3 + 2) {
            isolated_corners = false;
            break;
        }
    }
    if (trace) trace("topology_scan");
    // Expanded GLB triangles have no shared edge indices. Preserve their own
    // face normals without allocating and sorting three edge records per face.
    if (isolated_corners) {
        std::vector<Vec3f> normals(faces * 3);
        for (size_t face = 0; face < faces; ++face) {
            const auto& f = mesh.indices[face];
            Vec3f weighted = (mesh.vertices[f[1]] - mesh.vertices[f[0]]).cross(
                mesh.vertices[f[2]] - mesh.vertices[f[0]]);
            if (!weighted.allFinite()) weighted.setZero();
            Vec3f normal = Vec3f::Zero();
            normal += weighted;
            if (normal.norm() > 1e-8f) normal.normalize();
            if (!(normal.squaredNorm() > 1e-16f))
                normal = weighted.squaredNorm() > 1e-16f ? Vec3f(weighted.normalized()) : Vec3f(Vec3f::UnitZ());
            for (size_t corner = 0; corner < 3; ++corner) normals[face * 3 + corner] = normal;
        }
        if (trace) trace("isolated_faces");
        return normals;
    }
    std::vector<Vec3f> weighted(faces), normals(faces * 3);
    for (Vec3f& normal : normals) normal.setZero();
    std::vector<uint32_t> parent(faces * 3);
    std::iota(parent.begin(), parent.end(), 0);
    // 20 bits per vertex and 24 bits per face corner fit one integer record.
    // Keep the original representation for wider inputs; never truncate IDs.
    const bool compact_edges = mesh.vertices.size() <= (size_t(1) << 20) && faces <= (size_t(1) << 24) / 3;
    std::vector<Edge> edges;
    std::vector<uint64_t> packed_edges;
    if (compact_edges) packed_edges.reserve(faces * 3);
    else edges.reserve(faces * 3);
    for (size_t face = 0; face < faces; ++face) {
        const auto& f = mesh.indices[face];
        const Vec3f cross = (mesh.vertices[f[1]] - mesh.vertices[f[0]]).cross(
            mesh.vertices[f[2]] - mesh.vertices[f[0]]);
        if (cross.allFinite()) weighted[face] = cross;
        else weighted[face].setZero();
        for (uint8_t side = 0; side < 3; ++side) {
            const uint32_t a = uint32_t(f[side]), b = uint32_t(f[(side + 1) % 3]);
            if (a == b) continue;
            if (compact_edges)
                packed_edges.push_back((uint64_t(std::min(a, b)) << 44) | (uint64_t(std::max(a, b)) << 24) | (face * 3 + side));
            else edges.push_back({(uint64_t(std::min(a, b)) << 32) | std::max(a, b), uint32_t(face), side});
        }
    }
    if (trace) trace("edges");
    if (compact_edges) std::sort(packed_edges.begin(), packed_edges.end());
    else std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.key < b.key; });
    if (trace) trace("sort");
    const size_t edge_count = compact_edges ? packed_edges.size() : edges.size();
    const auto edge_key = [&](size_t index) {
        return compact_edges ? packed_edges[index] >> 24 : edges[index].key;
    };
    const auto edge_at = [&](size_t index) -> Edge {
        if (!compact_edges) return edges[index];
        const uint32_t corner = uint32_t(packed_edges[index] & 0xFFFFFFu);
        return {0, corner / 3, uint8_t(corner % 3)};
    };
    const auto root = [&](uint32_t id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
        return id;
    };
    const float min_dot = std::cos(crease_degrees * .01745329252f);
    for (size_t begin = 0; begin < edge_count;) {
        size_t end = begin + 1;
        while (end < edge_count && edge_key(end) == edge_key(begin)) ++end;
        if (end - begin == 2) {
            const Edge a = edge_at(begin), b = edge_at(begin + 1);
            const auto& fa = mesh.indices[a.face]; const auto& fb = mesh.indices[b.face];
            const int a0 = fa[a.side], a1 = fa[(a.side + 1) % 3];
            const int b0 = fb[b.side], b1 = fb[(b.side + 1) % 3];
            const float lengths = weighted[a.face].norm() * weighted[b.face].norm();
            if (a.face != b.face && a0 == b1 && a1 == b0 && lengths > 1e-12f &&
                weighted[a.face].dot(weighted[b.face]) >= min_dot * lengths) {
                const uint32_t a_first = a.face * 3 + a.side;
                const uint32_t a_second = a.face * 3 + (a.side + 1) % 3;
                const uint32_t b_first = b.face * 3 + b.side;
                const uint32_t b_second = b.face * 3 + (b.side + 1) % 3;
                const auto unite = [&](uint32_t left, uint32_t right) {
                    const uint32_t l = root(left), r = root(right);
                    parent[std::max(l, r)] = std::min(l, r);
                };
                unite(a_first, b_second);
                unite(a_second, b_first);
            }
        }
        begin = end;
    }
    std::vector<Edge>().swap(edges);
    std::vector<uint64_t>().swap(packed_edges);
    if (trace) trace("connect");
    for (size_t face = 0; face < faces; ++face)
        for (uint32_t corner = 0; corner < 3; ++corner)
            normals[root(uint32_t(face * 3 + corner))] += weighted[face];
    if (trace) trace("accumulate");
    for (size_t index = 0; index < normals.size(); ++index)
        if (parent[index] == index && normals[index].norm() > 1e-8f)
            normals[index].normalize();
    if (trace) trace("normalize");
    // Roots always have the smallest corner ID in their group. Fill from the
    // end so an output corner cannot overwrite an unconsumed root normal.
    for (size_t index = normals.size(); index-- > 0;) {
        const uint32_t representative = root(uint32_t(index));
        if (normals[representative].squaredNorm() > 1e-16f)
            normals[index] = normals[representative];
        else {
            const Vec3f& own = weighted[index / 3];
            normals[index] = own.squaredNorm() > 1e-16f ? Vec3f(own.normalized()) : Vec3f(Vec3f::UnitZ());
        }
    }
    if (trace) trace("publish");
    return normals;
}

} // namespace Slic3r::GUI::ModelPreviewNormals
