// Header-only CGAL EPICK predicates, matching the desktop's self-intersection
// predicate. No GUI or provider dependency, and no destructive mesh conversion.
#include "model_internal.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/squared_distance_3.h>

namespace orca_model_detail {
J glb_removed_deviation(Context& c, const std::vector<V>& vertices, const std::vector<T>& faces, const std::set<size_t>& removed)
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    auto point   = [&](size_t id) {
        const auto& v = vertices[id];
        return Kernel::Point_3(v[0], v[1], v[2]);
    };
    auto triangle = [&](const T& t) { return Kernel::Triangle_3(point(t[0]), point(t[1]), point(t[2])); };
    V low = vertices.front(), high = low;
    for (const auto& v : vertices)
        for (size_t k = 0; k < 3; ++k) {
            low[k]  = std::min(low[k], v[k]);
            high[k] = std::max(high[k], v[k]);
        }
    double diagonal = 0;
    for (size_t k = 0; k < 3; ++k)
        diagonal += (high[k] - low[k]) * (high[k] - low[k]);
    diagonal          = std::sqrt(diagonal);
    double total_area = 0, removed_area = 0, maximum = 0;
    bool redundant_cover = true;
    std::set<T> unique;
    for (size_t i = 0; i < faces.size(); ++i) {
        if ((i & 4095) == 0)
            check_cancel(c);
        auto key = faces[i];
        std::sort(key.begin(), key.end());
        if (!unique.insert(key).second)
            continue;
        double area = std::sqrt(CGAL::to_double(triangle(faces[i]).squared_area()));
        total_area += area;
        if (removed.count(i))
            removed_area += area;
    }
    // Retained triangles are exactly the original triangles, so result->source
    // distance is identically zero. Check every removed triangle centroid and
    // vertex against the retained surface, stricter than desktop stride sampling.
    for (const auto id : removed) {
        const auto& t = faces.at(id);
        const auto &a = vertices[t[0]], &b = vertices[t[1]], &d = vertices[t[2]];
        bool covered = false;
        for (size_t i = 0; i < faces.size(); ++i) {
            if ((i & 4095) == 0)
                check_cancel(c);
            if (removed.count(i))
                continue;
            const auto cover = triangle(faces[i]);
            if (cover.has_on(point(t[0])) && cover.has_on(point(t[1])) && cover.has_on(point(t[2]))) {
                covered = true;
                break;
            }
        }
        redundant_cover &= covered;
        std::array<Kernel::Point_3, 4> samples{point(t[0]), point(t[1]), point(t[2]),
                                               Kernel::Point_3((a[0] + b[0] + d[0]) / 3, (a[1] + b[1] + d[1]) / 3,
                                                               (a[2] + b[2] + d[2]) / 3)};
        for (const auto& q : samples) {
            double nearest = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < faces.size(); ++i) {
                if ((i & 4095) == 0)
                    check_cancel(c);
                if (!removed.count(i))
                    nearest = std::min(nearest, CGAL::to_double(CGAL::squared_distance(q, triangle(faces[i]))));
            }
            if (!std::isfinite(nearest) || nearest < 0)
                throw Error("INVALID_GEOMETRY", "Cannot verify removed-surface deviation");
            maximum = std::max(maximum, std::sqrt(nearest));
        }
    }
    const double deviation   = diagonal > 0 ? maximum / diagonal : std::numeric_limits<double>::infinity();
    const double area_change = total_area > 0 ? removed_area / total_area : std::numeric_limits<double>::infinity();
    return {{"performed", true},
            {"status", deviation <= 0.002 && area_change <= 0.02 ? "passed" : "failed"},
            {"count", removed.size()},
            {"details",
             {{"sampledDeviationRatio", deviation},
              {"redundantSurfaceCover", redundant_cover},
              {"areaChangeRatio", area_change},
              {"maximumSampledDeviationRatio", 0.002},
              {"maximumAreaChangeRatio", 0.02}}}};
    // Every removed triangle must lie entirely in one retained convex triangle;
    // nearest-point tolerances alone cannot prove redundant surface coverage.
}
J glb_strict_predicates(Context& c, const std::vector<V>& vertices, const std::vector<T>& triangles)
{
    using Kernel  = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Surface = CGAL::Surface_mesh<Kernel::Point_3>;
    Surface mesh;
    std::vector<Kernel::Point_3> points;
    points.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        if ((i & 4095) == 0)
            check_cancel(c);
        const auto& p = vertices[i];
        points.emplace_back(p[0], p[1], p[2]);
    }
    // Incremental add_face rejects some temporarily disconnected vertex fans
    // even when the final soup is valid. Validate the complete indexed soup
    // before construction, avoiding an input face-order dependency.
    check_cancel(c);
    if (!CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(triangles))
        return {{"topologyValid", false},
                {"failureStage", "polygon_soup_topology"},
                {"selfIntersectionPerformed", false},
                {"selfIntersects", nullptr}};
    CGAL::Polygon_mesh_processing::polygon_soup_to_polygon_mesh(points, triangles, mesh);
    // Desktop also regards a disconnected vertex link as invalid topology.
    std::vector<Surface::Halfedge_index> non_manifold;
    CGAL::Polygon_mesh_processing::non_manifold_vertices(mesh, std::back_inserter(non_manifold));
    check_cancel(c);
    if (!non_manifold.empty())
        return {{"topologyValid", false},
                {"failureStage", "vertex_link"},
                {"nonManifoldVertexFans", non_manifold.size()},
                {"selfIntersectionPerformed", false},
                {"selfIntersects", nullptr}};
    check_cancel(c);
    const bool intersects = CGAL::Polygon_mesh_processing::does_self_intersect(mesh);
    check_cancel(c);
    return {{"topologyValid", true}, {"selfIntersectionPerformed", true}, {"selfIntersects", intersects}};
}
} // namespace orca_model_detail
