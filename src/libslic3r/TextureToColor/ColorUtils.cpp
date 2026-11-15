#include "ColorUtils.hpp"
#include "ColorDistancePrepared.hpp"

#include <CGAL/Aff_transformation_3.h>
#include <CGAL/boost/graph/graph_traits_Surface_mesh.h>
#include <Eigen/Core>
#include <CGAL/Surface_mesh/Surface_mesh.h>
#include <CGAL/Origin.h>
#include <CGAL/centroid.h>
#include <CGAL/Named_function_parameters.h>
#include <CGAL/boost/graph/helpers.h>
#include <Eigen/Eigenvalues>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <new>
#include <thread>

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#include <boost/next_prior.hpp>

#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits_3.h>
#include <CGAL/AABB_tree.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Point_3.h>
#include <CGAL/Polygon_mesh_processing/compute_normal.h>
#include <CGAL/Polygon_mesh_processing/intersection.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/mesh_segmentation.h>
#include <string>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include "CgalUtils.hpp"
#include "TriMesh.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include <boost/log/trivial.hpp>
#include <vector>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <queue>

namespace Slic3r { namespace tex2color {
namespace color_utils {

// #define DEBUG_FLAG

#ifndef M_PI
#define M_PI 3.1415926535897932
#endif

#ifndef EPSILON
#define EPSILON 1e-6
#endif

#ifndef DOUBLE_LIMITS
#define DOUBLE_LIMITS
#define Double_MAX std::numeric_limits<double>::max()
#define Double_MIN -std::numeric_limits<double>::max()
#endif  // !DOUBLE_LIMITS

namespace PMP = CGAL::Polygon_mesh_processing;

using cgalutils::CGALMesh;
using CGALKernel = cgalutils::Kernel;

static constexpr double TOPO_SMOOTH_WEIGHT_THRESHOLD = 0.3;

namespace detail {
template<typename Mesh>
double average_edge_length_impl(const Mesh& m) {
    double total = 0.0;
    size_t count = 0;
    for (auto e : m.edges()) {
        auto h = m.halfedge(e);
        auto p0 = m.point(m.source(h));
        auto p1 = m.point(m.target(h));
        total += std::sqrt(CGAL::squared_distance(p0, p1));
        ++count;
    }
    return count > 0 ? total / count : 1.0;
}
} // namespace detail

typedef CGAL::Aff_transformation_3<CGALKernel> Affine_transformation_3;
typedef boost::graph_traits<CGALMesh>::halfedge_descriptor halfedge_descriptor;
typedef boost::graph_traits<CGALMesh>::edge_descriptor edge_descriptor;
typedef boost::graph_traits<CGALMesh>::vertex_descriptor vertex_descriptor;
typedef CGAL::AABB_face_graph_triangle_primitive<CGALMesh> Primitive;
typedef CGAL::AABB_traits_3<CGALKernel, Primitive> Traits;
typedef CGAL::AABB_tree<Traits> Tree;
typedef CGALMesh::template Property_map<vertex_descriptor, CGALKernel::Vector_3> VNMap;

static inline ColorDouble convert_rgb_uint_to_rgb_double(const Color& color) {
    return ColorDouble{static_cast<double>(color[0]), static_cast<double>(color[1]), static_cast<double>(color[2])};
}

static void normalize(CGALKernel::Vector_3& vec) {
    double squared_length = vec.squared_length();
    if (squared_length > EPSILON) {
        vec /= sqrt(squared_length);
    }
}

static double get_angle_between_vectors(const CGALKernel::Vector_3& v1, const CGALKernel::Vector_3& v2) {
    CGALKernel::Vector_3 dir1{v1}, dir2{v2};
    normalize(dir1);
    normalize(dir2);
    double product_dot = dir1.x() * dir2.x() + dir1.y() * dir2.y() + dir1.z() * dir2.z();
    if (product_dot > 1.0 - EPSILON) {
        return 0.0;
    } else if (product_dot < -1.0 + EPSILON) {
        return 180.0;
    }
    return std::acos(product_dot) / M_PI * 180.0;
}

static void calc_face_normals(const CGALMesh& mesh, std::vector<Eigen::Vector3d>& face_normals) {
    std::size_t fcnt = mesh.number_of_faces();
    face_normals.resize(fcnt);
    for (auto face : mesh.faces()) {
        std::vector<CGALKernel::Point_3> points;
        for (auto vtx : mesh.vertices_around_face(mesh.halfedge(face))) {
            points.push_back(mesh.point(vtx));
        }
        Eigen::Vector3d pos1(points[0].x(), points[0].y(), points[0].z());
        Eigen::Vector3d pos2(points[1].x(), points[1].y(), points[1].z());
        Eigen::Vector3d pos3(points[2].x(), points[2].y(), points[2].z());
        face_normals[face] = (pos3 - pos2).cross(pos1 - pos2);
        face_normals[face].normalize();
    }
    return;
}

static bool check_and_repair_self_intersect(CGALMesh& mesh, bool* is_self_intersect_status = nullptr) {
    // true means the mesh is no self intersect now
    // false means the mesh is still self intersect
    auto is_self_intersect = PMP::does_self_intersect(mesh);
    if (is_self_intersect_status != nullptr) {
        *is_self_intersect_status = is_self_intersect;
    }
    if (is_self_intersect) {
        bool repair = PMP::experimental::remove_self_intersections(mesh);
        if (repair) {
            return true;
        } else {
            return false;
        }
    }
    return true;
}

static bool save_polylines(const std::string& file_name, const std::vector<std::vector<CGALKernel::Point_3>>& polylines) {
    std::vector<CGALKernel::Point_3> points;
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    for (auto& polyline : polylines) {
        std::size_t begin_pt_idx = points.size();
        for (auto& pt : polyline) {
            points.push_back(pt);
        }
        for (std::size_t i = 1; i < polyline.size(); ++i) {
            lines.emplace_back(begin_pt_idx + i, begin_pt_idx + i + 1);  // obj is begin at 1
        }
    }
    std::ofstream output_file(file_name, std::ios::out);
    for (auto& point : points) {
        output_file << "v " << point[0] << " " << point[1] << " " << point[2] << "\n";
    }
    for (auto& line : lines) {
        output_file << "l " << line.first << " " << line.second << "\n";
    }
    output_file.close();
    return true;
}

using SmoothingClock = std::chrono::steady_clock;

static bool smoothing_canceled(const SmoothParameters& parameters)
{
    return parameters.cancel_callback && parameters.cancel_callback();
}

static void record_smoothing_stage(const SmoothParameters& parameters, const char* stage,
                                  SmoothingClock::time_point started, std::size_t count = 0)
{
    const auto elapsed = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        SmoothingClock::now() - started).count());
    BOOST_LOG_TRIVIAL(info) << "[NativeSmoothing] stage=" << stage << " elapsed_ms=" << elapsed << " count=" << count;
    if (parameters.diagnostic_callback) {
        try { parameters.diagnostic_callback(stage, elapsed, count); }
        catch (...) { /* Diagnostics must not invalidate a repair. */ }
    }
}

static bool smooth_region_topo_boundary(const CGALMesh& mesh, std::vector<std::size_t>& face_labels,
    const std::vector<std::array<std::size_t, 3>>& neighbors, const SmoothParameters& parameters,
    std::size_t max_iters = 20)
{
    const auto started = SmoothingClock::now();
    std::size_t iter = 0;
    while (iter < max_iters) {
        ++iter;
        bool flip_flag = false;
        for (auto face : mesh.faces()) {
            if ((std::size_t(face) & 4095) == 0 && smoothing_canceled(parameters)) return false;
            std::size_t same_label_count = 0, max_adj_cnt = 0;
            std::size_t max_adj_label = face_labels[face];
            std::array<std::size_t, 3> labels {}, counts {};
            std::size_t used = 0;
            // Preserve face, neighbor and tie-breaking order from the original
            // in-place sweep. Triangles have at most three adjacent faces.
            for (const auto adjacent : neighbors[face]) {
                if (adjacent == std::numeric_limits<std::size_t>::max()) continue;
                const auto label = face_labels[adjacent];
                if (label == face_labels[face]) { ++same_label_count; continue; }
                std::size_t slot = 0;
                while (slot < used && labels[slot] != label) ++slot;
                if (slot == used) labels[used++] = label;
                if (++counts[slot] > max_adj_cnt) {
                    max_adj_cnt = counts[slot]; max_adj_label = label;
                }
            }
            if (max_adj_cnt > same_label_count) {
                face_labels[face] = max_adj_label; flip_flag = true;
            }
        }
        if (!flip_flag) break;
    }
    record_smoothing_stage(parameters, "topology", started, iter);
    return !smoothing_canceled(parameters);
}

static bool smooth_region_geom_boundary(CGALMesh& mesh, const std::vector<std::size_t>& face_labels,
                                       const SmoothParameters& smooth_parameters, bool optimized = true) {
    if (smoothing_canceled(smooth_parameters)) return false;
    const auto extraction_started = SmoothingClock::now();
    std::vector<CGALKernel::Vector_3> face_normals(mesh.num_faces());
    std::vector<std::uint8_t> normal_ready(mesh.num_faces(), 0);
    const auto normal = [&](CGALMesh::Face_index face) -> const CGALKernel::Vector_3& {
        if (!normal_ready[face]) {
            face_normals[face] = PMP::compute_face_normal(face, mesh);
            normal_ready[face] = 1;
        }
        return face_normals[face];
    };
    // 1. extract boundary vertices and make polines
    std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_vtx_to_degree;
    std::unordered_set<CGAL::SM_Edge_index> segment_boundary_edges;
    std::unordered_set<CGAL::SM_Edge_index> feature_edges;
    std::unordered_set<CGAL::SM_Vertex_index> feature_vertices;
    constexpr double feature_angle = 45;
    for (const auto& edge : mesh.edges()) {
        if ((std::size_t(edge) & 4095) == 0 && smoothing_canceled(smooth_parameters)) return false;
        if (!mesh.is_valid(edge) || mesh.is_border(edge)) {
            continue;
        }
        auto source = mesh.source(mesh.halfedge(edge));
        auto target = mesh.target(mesh.halfedge(edge));
        auto face_1 = mesh.face(mesh.halfedge(edge));
        auto face_2 = mesh.face(mesh.opposite(mesh.halfedge(edge)));
        const auto& normal_1 = normal(face_1);
        const auto& normal_2 = normal(face_2);
        double angle = get_angle_between_vectors(normal_1, normal_2);
        if (angle > feature_angle) {
            feature_edges.insert(edge);
            feature_vertices.insert(source);
            feature_vertices.insert(target);
        }

        if (face_labels[face_1] == face_labels[face_2]) {
            continue;
        }

        segment_boundary_edges.insert(edge);
        ++map_vtx_to_degree[source];
        ++map_vtx_to_degree[target];
    }

    // 2. smooth each polyline
    std::vector<std::vector<CGAL::SM_Vertex_index>> polylines;
    std::unordered_set<CGAL::SM_Edge_index> visited_edges;

    const auto trace_polyline = [&](std::vector<CGAL::SM_Vertex_index>& polyline) {
        // The former tail recursion can exhaust the Windows stack on a long
        // color boundary. Retain the same first-edge order iteratively.
        while (!polyline.empty() && !smoothing_canceled(smooth_parameters)) {
            const auto current = polyline.back();
            if (map_vtx_to_degree[current] != 2) return;
            bool advanced = false;
            for (const auto halfedge : mesh.halfedges_around_target(mesh.halfedge(current))) {
                const auto edge = mesh.edge(halfedge);
                if (visited_edges.count(edge) || !segment_boundary_edges.count(edge)) continue;
                visited_edges.insert(edge);
                const auto adjacent = mesh.source(halfedge);
                if (!map_vtx_to_degree.count(adjacent)) continue;
                polyline.push_back(adjacent);
                advanced = true;
                break;
            }
            if (!advanced) return;
        }
    };

    // 2.1. open polyline: from T nodes search other 2-degree nodes
    for (auto& [src_vtx, degree] : map_vtx_to_degree) {
        if (degree == 2) {
            continue;
        }
        for (auto& src_halfedge : mesh.halfedges_around_target(mesh.halfedge(src_vtx))) {
            CGAL::SM_Edge_index src_edge = mesh.edge(src_halfedge);
            if (visited_edges.count(src_edge) || !segment_boundary_edges.count(src_edge)) {
                continue;
            }
            visited_edges.insert(src_edge);
            CGAL::SM_Vertex_index adj_vtx = mesh.source(src_halfedge);
            if (!map_vtx_to_degree.count(adj_vtx)) {
                continue;
            }
            std::vector<CGAL::SM_Vertex_index> polyline{src_vtx, adj_vtx};
            trace_polyline(polyline);
            polylines.push_back(std::move(polyline));
        }
    }

    // 2.2. closed polylines
    for (auto edge : segment_boundary_edges) {
        if (visited_edges.count(edge)) {
            continue;
        }
        visited_edges.insert(edge);
        CGAL::SM_Halfedge_index halfedge = mesh.halfedge(edge);
        std::vector<CGAL::SM_Vertex_index> polyline{mesh.source(halfedge), mesh.target(halfedge)};
        trace_polyline(polyline);
        if (polyline.front() != polyline.back()) {
            std::cerr << "[Error]:  loop polyline but not closed!!!\n";
        }
        polylines.push_back(std::move(polyline));
    }

    record_smoothing_stage(smooth_parameters, "boundary-extraction", extraction_started, polylines.size());
    if (smoothing_canceled(smooth_parameters)) return false;
    // 3. smooth boundary
    // AABB face primitives keep a reference to the mesh geometry. Keep that
    // geometry immutable while the working mesh is moved by the smoothing
    // loop; CGAL 6.x may otherwise retain stale primitive bounds and abort
    // while querying or destroying the accelerated tree.
    if (polylines.empty())
        return true;
    const CGALMesh projection_mesh = mesh;
    Tree boundary_tree(projection_mesh.faces().begin(), projection_mesh.faces().end(), projection_mesh);
    // CGAL's optional Kd-tree is not needed for the exact AABB projection and
    // its destruction is unstable for some imported meshes on Windows.
    boundary_tree.do_not_accelerate_distance_queries();
    const auto index_started = SmoothingClock::now();
    boundary_tree.build(); // Finish lazy construction before any parallel const query.
    record_smoothing_stage(smooth_parameters, "projection-index", index_started, projection_mesh.number_of_faces());
    const auto projection_started = SmoothingClock::now();
    std::size_t projection_count = 0;
    tbb::task_arena projection_arena(std::max(1, std::min(4, tbb::this_task_arena::max_concurrency())));
    const auto project_point = [&](const CGALKernel::Point_3& query, CGALMesh::Vertex_index vertex) {
        if (!optimized) return boundary_tree.closest_point(query);
        return cgalutils::closest_point_with_strict_hint(boundary_tree, query, projection_mesh.point(vertex));
    };

    constexpr std::size_t max_iters = 5;
    const double smooth_weight = smooth_parameters.smooth_weight;  // Controls smoothing intensity; larger values produce smoother results. Range: 0.1~1.0.
    double origin_weight = std::max(1.0 - smooth_weight, 0.0);

    // Short polylines dominate imported color boundaries. Batch only paths
    // with disjoint writes and no read/write dependency, retaining the original
    // order at every shared mutable vertex. A batch bounds cancellation delay;
    // callbacks and diagnostics stay on the calling thread.
    struct ProjectionBatch { std::size_t begin, end; };
    std::vector<std::size_t> active_paths, point_offsets(polylines.size() + 1, 0);
    std::vector<ProjectionBatch> batches;
    std::vector<std::size_t> read_stamp(mesh.num_vertices(), 0), write_stamp(mesh.num_vertices(), 0);
    std::size_t stamp = 1, batch_begin = 0, batch_points = 0, iteration_queries = 0;
    const auto flush_batch = [&] {
        if (batch_begin != active_paths.size()) batches.push_back({batch_begin, active_paths.size()});
        batch_begin = active_paths.size(); batch_points = 0; ++stamp;
    };
    for (std::size_t path = 0; path < polylines.size(); ++path) {
        if (smoothing_canceled(smooth_parameters)) return false;
        const auto& polyline = polylines[path];
        point_offsets[path + 1] = point_offsets[path] + polyline.size();
        const bool closed = polyline.front() == polyline.back();
        // The old loop discarded every prepared point for this closed path.
        if (optimized && closed && feature_vertices.count(polyline.front())) continue;
        const auto writes = [&](std::size_t index) {
            return !feature_vertices.count(polyline[index]) &&
                (closed || (index > 0 && index + 1 < polyline.size()));
        };
        bool conflicts = false;
        for (std::size_t index = 0; index < polyline.size(); ++index) {
            const auto vertex = std::size_t(polyline[index]);
            conflicts = conflicts || write_stamp[vertex] == stamp ||
                (writes(index) && read_stamp[vertex] == stamp);
        }
        if (conflicts || active_paths.size() - batch_begin >= 128 || batch_points + polyline.size() > 4096)
            flush_batch();
        for (std::size_t index = 0; index < polyline.size(); ++index) {
            const auto vertex = std::size_t(polyline[index]);
            read_stamp[vertex] = stamp;
            if (writes(index)) write_stamp[vertex] = stamp;
            if (index > 0 && index + 1 < polyline.size() && (!optimized || writes(index))) ++iteration_queries;
        }
        if (closed) ++iteration_queries;
        active_paths.push_back(path);
        batch_points += polyline.size();
    }
    flush_batch();
    record_smoothing_stage(smooth_parameters, "projection-plan", projection_started, batches.size());
    std::vector<CGALKernel::Point_3> projected_points(point_offsets.back());
    for (std::size_t iter = 0; iter < max_iters; ++iter) {
        const auto process_path = [&](std::size_t active) {
            const auto path = active_paths[active];
            const auto& polyline = polylines[path];
            std::size_t pt_cnt = polyline.size();
            auto* points = projected_points.data() + point_offsets[path];
            const auto project = [&](std::size_t pt_idx) {
                if (optimized && feature_vertices.count(polyline[pt_idx])) return;
                const std::array<CGALKernel::Point_3, 2> pts {mesh.point(polyline[pt_idx - 1]), mesh.point(polyline[pt_idx + 1])};
                CGALKernel::Point_3 smooth_pt = CGAL::ORIGIN + ((CGAL::centroid(pts.begin(), pts.end()) - CGAL::ORIGIN) * smooth_weight +
                                                                (mesh.point(polyline[pt_idx]) - CGAL::ORIGIN) * origin_weight);
                // An exact vertex on the immutable projection surface is a
                // valid distance bound, without enabling CGAL's unsafe Kd-tree.
                points[pt_idx] = project_point(smooth_pt, polyline[pt_idx]);
            };
            if (optimized && pt_cnt > 257) {
                projection_arena.execute([&] {
                    tbb::parallel_for(tbb::blocked_range<std::size_t>(1, pt_cnt - 1, 128),
                        [&](const tbb::blocked_range<std::size_t>& range) {
                            for (auto index = range.begin(); index != range.end(); ++index) project(index);
                        });
                });
            } else {
                for (std::size_t index = 1; index + 1 < pt_cnt; ++index) project(index);
            }
            if (polyline.front() == polyline.back()) {
                if (feature_vertices.count(polyline.front())) {
                    return;
                }
                const std::array<CGALKernel::Point_3, 2> pts {mesh.point(polyline[1]), mesh.point(polyline[pt_cnt - 2])};
                CGALKernel::Point_3 smooth_pt = CGAL::ORIGIN + ((CGAL::centroid(pts.begin(), pts.end()) - CGAL::ORIGIN) * smooth_weight +
                                                                (mesh.point(polyline.front()) - CGAL::ORIGIN) * origin_weight);
                mesh.point(polyline.front()) = project_point(smooth_pt, polyline.front());
            }

            for (std::size_t pt_idx = 1; pt_idx + 1 < pt_cnt; ++pt_idx) {
                if (feature_vertices.count(polyline[pt_idx])) {
                    continue;
                }
                mesh.point(polyline[pt_idx]) = points[pt_idx];
            }
        };
        for (const auto batch : batches) {
            if (smoothing_canceled(smooth_parameters)) return false;
            if (optimized && batch.end - batch.begin > 1) {
                projection_arena.execute([&] {
                    tbb::parallel_for(tbb::blocked_range<std::size_t>(batch.begin, batch.end, 1),
                        [&](const tbb::blocked_range<std::size_t>& range) {
                            for (auto path = range.begin(); path != range.end(); ++path) process_path(path);
                        });
                });
            } else {
                for (auto path = batch.begin; path != batch.end; ++path) process_path(path);
            }
        }
        projection_count += iteration_queries;
    }

    // Final projection only reads and writes its own vertex. Preserve the
    // number and order of repeated projections for each vertex (including
    // closed endpoints), then evaluate different vertices independently.
    std::vector<std::size_t> final_counts(mesh.num_vertices(), 0);
    std::vector<CGALMesh::Vertex_index> final_vertices;
    for (const auto& polyline : polylines)
        for (const auto vertex : polyline) {
            if (final_counts[vertex]++ == 0) final_vertices.push_back(vertex);
            ++projection_count;
        }
    for (std::size_t begin = 0; begin < final_vertices.size(); begin += 512) {
        if (smoothing_canceled(smooth_parameters)) return false;
        const auto end = std::min(begin + 512, final_vertices.size());
        const auto project_final = [&](std::size_t index) {
            const auto vertex = final_vertices[index];
            for (std::size_t repeat = 0; repeat < final_counts[vertex]; ++repeat)
                mesh.point(vertex) = project_point(mesh.point(vertex), vertex);
        };
        if (optimized) {
            projection_arena.execute([&] {
                tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end, 32),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                        for (auto index = range.begin(); index != range.end(); ++index) project_final(index);
                    });
            });
        } else {
            for (auto index = begin; index < end; ++index) project_final(index);
        }
    }

    record_smoothing_stage(smooth_parameters, "projection", projection_started, projection_count);
    return !smoothing_canceled(smooth_parameters);
}

// HSV, XYZ, and LAB are only used internally for computing color differences, so they are declared in this cpp file only.
typedef std::array<double, 3> HSV;
typedef std::array<double, 3> XYZ;  // Intermediate space for converting between LAB and RGB
typedef std::array<double, 3> LAB;  // CIELAB was designed to match human visual perception; the standard method for perceptual color difference
// Common white points
const XYZ D65_WHITE = {0.95047, 1.0, 1.08883};

/**
 * @brief Convert an RGB color to the HSV color space.
 *
 * @param rgb Input RGB color [R, G, B], range 0~255.
 * @return HSV output [H, S, V], H in 0~360, S and V in 0~1.
 */
static HSV convert_rgb_to_hsv(const RGB& rgb) {
    // Normalize to [0, 1]
    double r = rgb[0] / 255.0;
    double g = rgb[1] / 255.0;
    double b = rgb[2] / 255.0;

    double max = std::max({r, g, b});
    double min = std::min({r, g, b});
    double delta = max - min;

    // Compute hue H
    double h = 0;
    if (delta == 0) {
        h = 0;  // Gray; hue is undefined
    } else {
        if (max == r) {
            h = 60.0 * fmod((g - b) / delta, 6.0);
        } else if (max == g) {
            h = 60.0 * ((b - r) / delta + 2.0);
        } else {  // max == b
            h = 60.0 * ((r - g) / delta + 4.0);
        }
        if (h < 0) {
            h += 360.0;
        }
    }

    // Compute saturation S
    double s = (max == 0) ? 0 : (delta / max);

    // Compute value V
    double v = max;

    return {h, s, v};
}

/**
 * @brief Convert an HSV color to the RGB color space.
 *
 * @param hsv Input HSV color [H, S, V], H in 0~360, S and V in 0~1.
 * @return RGB output [R, G, B], range 0~255.
 */
static RGB convert_hsv_to_rgb(const HSV& hsv) {
    double h = hsv[0];
    double s = hsv[1];
    double v = hsv[2];

    double c = v * s;
    double x = c * (1 - std::abs(fmod(h / 60.0, 2.0) - 1));
    double m = v - c;

    double r, g, b;

    if (h < 60) {
        r = c;
        g = x;
        b = 0;
    } else if (h < 120) {
        r = x;
        g = c;
        b = 0;
    } else if (h < 180) {
        r = 0;
        g = c;
        b = x;
    } else if (h < 240) {
        r = 0;
        g = x;
        b = c;
    } else if (h < 300) {
        r = x;
        g = 0;
        b = c;
    } else {
        r = c;
        g = 0;
        b = x;
    }

    return {static_cast<std::size_t>((r + m) * 255 + 0.5), static_cast<std::size_t>((g + m) * 255 + 0.5), static_cast<std::size_t>((b + m) * 255 + 0.5)};
}

static XYZ convert_rgb_to_xyz(const RGB& color_rgb) {
    ColorDouble rgb{static_cast<double>(color_rgb[0]), static_cast<double>(color_rgb[1]), static_cast<double>(color_rgb[2])};
    auto gammaCorrect = [](double v) -> double {
        v = v / 255.0;
        if (v > 0.04045) {
            return std::pow((v + 0.055) / 1.055, 2.4);
        } else {
            return v / 12.92;
        }
    };

    double r = gammaCorrect(rgb[0]);
    double g = gammaCorrect(rgb[1]);
    double b = gammaCorrect(rgb[2]);

    // sRGB to XYZ matrix
    return {r * 0.4124564 + g * 0.3575761 + b * 0.1804375, r * 0.2126729 + g * 0.7151522 + b * 0.0721750, r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
}

// sRGB non-linear channel values [0,1] (double) -> linear light -> XYZ; equivalent to convert_rgb_to_xyz when v=n/255
static XYZ convert_srgb01_to_xyz(double rs, double gs, double bs) {
    auto gamma_correct = [](double v) -> double {
        v = std::clamp(v, 0.0, 1.0);
        return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : (v / 12.92);
    };
    const double r = gamma_correct(rs);
    const double g = gamma_correct(gs);
    const double b = gamma_correct(bs);
    return {r * 0.4124564 + g * 0.3575761 + b * 0.1804375, r * 0.2126729 + g * 0.7151522 + b * 0.0721750, r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
}

static LAB convert_xyz_to_lab(const XYZ& xyz) {
    auto f = [](double t) -> double {
        const double delta = 6.0 / 29.0;
        if (t > delta * delta * delta) {
            return std::cbrt(t);
        } else {
            return t / (3.0 * delta * delta) + 4.0 / 29.0;
        }
    };

    // D65 white point
    double xn = D65_WHITE[0], yn = D65_WHITE[1], zn = D65_WHITE[2];

    double fx = f(xyz[0] / xn);
    double fy = f(xyz[1] / yn);
    double fz = f(xyz[2] / zn);

    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

// ColorDouble here represents sRGB [0,1]; see calc_rgb_color_difference_by_ciede2000_srgb01 header comment
static LAB convert_srgb01_to_lab(const ColorDouble& srgb01) {
    return convert_xyz_to_lab(convert_srgb01_to_xyz(srgb01[0], srgb01[1], srgb01[2]));
}

static LAB convert_rgb_to_lab(const RGB& rgb) {
    return convert_xyz_to_lab(convert_rgb_to_xyz(rgb));
}

static Color convert_lab_to_rgb(const LAB& lab) {
    // Lab → XYZ
    const double delta = 6.0 / 29.0;
    const double delta2x3 = 3.0 * delta * delta;

    double fy = (lab[0] + 16.0) / 116.0;
    double fx = lab[1] / 500.0 + fy;
    double fz = fy - lab[2] / 200.0;

    double x = D65_WHITE[0] * (fx > delta ? fx * fx * fx : delta2x3 * (fx - 4.0 / 29.0));
    double y = D65_WHITE[1] * (fy > delta ? fy * fy * fy : delta2x3 * (fy - 4.0 / 29.0));
    double z = D65_WHITE[2] * (fz > delta ? fz * fz * fz : delta2x3 * (fz - 4.0 / 29.0));

    // XYZ -> linear RGB (sRGB inverse matrix)
    double r_lin = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    double g_lin = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    double b_lin = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    // linear RGB -> sRGB (inverse gamma correction)
    auto inverse_gamma = [](double v) -> double {
        v = std::max(v, 0.0);
        return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };

    auto to_uint8 = [](double v) -> std::size_t { return static_cast<std::size_t>(std::clamp(std::round(v * 255.0), 0.0, 255.0)); };

    return {to_uint8(inverse_gamma(r_lin)), to_uint8(inverse_gamma(g_lin)), to_uint8(inverse_gamma(b_lin))};
}

/**
 * @brief CIEDE2000 color-difference computation.
 *
 * Currently the most accurate color-difference formula, recommended by CIE as the industry standard.
 *
 * @param lab1 LAB values of the first color.
 * @param lab2 LAB values of the second color.
 * @return Color difference (typically < 1.0 is imperceptible to the human eye).
 */
static double ciede2000(const std::array<double, 3>& lab1, const std::array<double, 3>& lab2) {
    // Parameters in the CIE L*C*h* formula
    double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];

    // Compute C1 and C2
    double C1 = std::sqrt(a1 * a1 + b1 * b1);
    double C2 = std::sqrt(a2 * a2 + b2 * b2);
    double C_avg = (C1 + C2) / 2.0;

    // G factor (compensates for non-linearity in the mid-low chroma region)
    double C7 = C_avg * C_avg * C_avg * C_avg * C_avg * C_avg * C_avg;
    double G = 0.5 * (1.0 - std::sqrt(C7 / (C7 + 6103515625.0)));

    // a1' and a2'
    double a1_prime = (1.0 + G) * a1;
    double a2_prime = (1.0 + G) * a2;

    // C'1 and C'2
    double C1_prime = std::sqrt(a1_prime * a1_prime + b1 * b1);
    double C2_prime = std::sqrt(a2_prime * a2_prime + b2 * b2);
    double C_prime_avg = (C1_prime + C2_prime) / 2.0;

    // h'1 and h'2
    double h1_prime = std::atan2(b1, a1_prime);
    double h2_prime = std::atan2(b2, a2_prime);
    if (h1_prime < 0) {
        h1_prime += 2 * M_PI;
    }
    if (h2_prime < 0) {
        h2_prime += 2 * M_PI;
    }

    // Compute dh'
    double dh_prime;
    if (C1_prime * C2_prime == 0.0) {
        dh_prime = 0.0;
    } else if (std::abs(h1_prime - h2_prime) <= M_PI) {
        dh_prime = h2_prime - h1_prime;
    } else if (h2_prime <= h1_prime) {
        dh_prime = h2_prime - h1_prime + 2 * M_PI;
    } else {
        dh_prime = h2_prime - h1_prime - 2 * M_PI;
    }

    // Compute dH'
    double dH_prime = 2.0 * std::sqrt(C1_prime * C2_prime) * std::sin(dh_prime / 2.0);

    // Compute dL'
    double dL_prime = L2 - L1;

    // Compute dC'
    double dC_prime = C2_prime - C1_prime;

    // Compute h_prime_avg
    double h_prime_avg;
    if (C1_prime * C2_prime == 0.0) {
        h_prime_avg = h1_prime + h2_prime;
    } else if (std::abs(h1_prime - h2_prime) > M_PI) {
        const double sum = h1_prime + h2_prime;
        h_prime_avg = (sum + (sum < 2 * M_PI ? 2 * M_PI : -2 * M_PI)) / 2.0;
    } else {
        h_prime_avg = (h1_prime + h2_prime) / 2.0;
    }

    // Compute T
    double T = 1.0 - 0.17 * std::cos(h_prime_avg - M_PI / 6.0) + 0.24 * std::cos(2.0 * h_prime_avg) + 0.32 * std::cos(3.0 * h_prime_avg + M_PI / 30.0) -
               0.20 * std::cos(4.0 * h_prime_avg - 63.0 * M_PI / 180.0);

    // Compute rotation term R_T = -R_C * sin(2*delta_theta), where delta_theta = 30 * exp(-((h_bar'-275)/25)^2)
    // h_prime_avg is in radians; convert to degrees for delta_theta; 2*delta_theta = 60 * exp(...), convert back to radians for sin
    double h_prime_avg_deg = h_prime_avg * 180.0 / M_PI;
    double C_prime_avg_7 = std::pow(C_prime_avg, 7);
    double R = -2.0 * std::sqrt(C_prime_avg_7 / (C_prime_avg_7 + 6103515625.0)) *
               std::sin((60.0 * M_PI / 180.0) * std::exp(-std::pow((h_prime_avg_deg - 275.0) / 25.0, 2)));

    // Compute SL, SC, SH
    double L_prime_avg = (L1 + L2) / 2.0;
    double SL = 1.0 + 0.015 * std::pow(L_prime_avg - 50.0, 2) / std::sqrt(20 + std::pow(L_prime_avg - 50.0, 2));
    double SC = 1.0 + 0.045 * C_prime_avg;
    double SH = 1.0 + 0.015 * C_prime_avg * T;

    // Final color difference
    double deltaE = std::sqrt(std::pow(dL_prime / SL, 2) + std::pow(dC_prime / SC, 2) + std::pow(dH_prime / SH, 2) + R * (dC_prime / SC) * (dH_prime / SH));

    return deltaE;
}

namespace detail {

bool has_default_lab_chroma_environment() {
#if defined(__FAST_MATH__) || defined(_M_FP_FAST) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
    // Runtime controls cannot qualify compile-time reassociation assumptions.
    return false;
#elif defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
    // Compare default exception masks, rounding and FTZ/DAZ controls,
    // never sticky exception flags. Unknown architectures keep scalar.
    constexpr unsigned int control_mask = 0xFFC0u;
    constexpr unsigned int default_controls = 0x1F80u;
    return FLT_EVAL_METHOD == 0 && std::fegetround() == FE_TONEAREST &&
           (_mm_getcsr() & control_mask) == default_controls;
#else
    return false;
#endif
}

bool prepare_lab_chroma(const std::array<double, 3>& lab, double& chroma) {
    if (!has_default_lab_chroma_environment() ||
        !std::isfinite(lab[0]) || !std::isfinite(lab[1]) || !std::isfinite(lab[2]))
        return false;
    const double a = lab[1], b = lab[2];
    const double prepared_chroma = std::sqrt(a * a + b * b);
    if (!std::isfinite(prepared_chroma))
        return false;
    chroma = prepared_chroma;
    return true;
}

double ciede2000_prepared(const std::array<double, 3>& lab1, const std::array<double, 3>& lab2,
                          double chroma1, double chroma2) {
    // Parameters in the CIE L*C*h* formula
    double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];

    // Original raw chroma, prepared from these exact corresponding inputs.
    double C1 = chroma1;
    double C2 = chroma2;
    double C_avg = (C1 + C2) / 2.0;

    // G factor (compensates for non-linearity in the mid-low chroma region)
    double C7 = C_avg * C_avg * C_avg * C_avg * C_avg * C_avg * C_avg;
    double G = 0.5 * (1.0 - std::sqrt(C7 / (C7 + 6103515625.0)));

    // a1' and a2'
    double a1_prime = (1.0 + G) * a1;
    double a2_prime = (1.0 + G) * a2;

    // C'1 and C'2
    double C1_prime = std::sqrt(a1_prime * a1_prime + b1 * b1);
    double C2_prime = std::sqrt(a2_prime * a2_prime + b2 * b2);
    double C_prime_avg = (C1_prime + C2_prime) / 2.0;

    // h'1 and h'2
    double h1_prime = std::atan2(b1, a1_prime);
    double h2_prime = std::atan2(b2, a2_prime);
    if (h1_prime < 0) {
        h1_prime += 2 * M_PI;
    }
    if (h2_prime < 0) {
        h2_prime += 2 * M_PI;
    }

    // Compute dh'
    double dh_prime;
    if (C1_prime * C2_prime == 0.0) {
        dh_prime = 0.0;
    } else if (std::abs(h1_prime - h2_prime) <= M_PI) {
        dh_prime = h2_prime - h1_prime;
    } else if (h2_prime <= h1_prime) {
        dh_prime = h2_prime - h1_prime + 2 * M_PI;
    } else {
        dh_prime = h2_prime - h1_prime - 2 * M_PI;
    }

    // Compute dH'
    double dH_prime = 2.0 * std::sqrt(C1_prime * C2_prime) * std::sin(dh_prime / 2.0);

    // Compute dL'
    double dL_prime = L2 - L1;

    // Compute dC'
    double dC_prime = C2_prime - C1_prime;

    // Compute h_prime_avg
    double h_prime_avg;
    if (C1_prime * C2_prime == 0.0) {
        h_prime_avg = h1_prime + h2_prime;
    } else if (std::abs(h1_prime - h2_prime) > M_PI) {
        const double sum = h1_prime + h2_prime;
        h_prime_avg = (sum + (sum < 2 * M_PI ? 2 * M_PI : -2 * M_PI)) / 2.0;
    } else {
        h_prime_avg = (h1_prime + h2_prime) / 2.0;
    }

    // Compute T
    double T = 1.0 - 0.17 * std::cos(h_prime_avg - M_PI / 6.0) + 0.24 * std::cos(2.0 * h_prime_avg) + 0.32 * std::cos(3.0 * h_prime_avg + M_PI / 30.0) -
               0.20 * std::cos(4.0 * h_prime_avg - 63.0 * M_PI / 180.0);

    // Compute rotation term R_T = -R_C * sin(2*delta_theta), where delta_theta = 30 * exp(-((h_bar'-275)/25)^2)
    // h_prime_avg is in radians; convert to degrees for delta_theta; 2*delta_theta = 60 * exp(...), convert back to radians for sin
    double h_prime_avg_deg = h_prime_avg * 180.0 / M_PI;
    double C_prime_avg_7 = std::pow(C_prime_avg, 7);
    double R = -2.0 * std::sqrt(C_prime_avg_7 / (C_prime_avg_7 + 6103515625.0)) *
               std::sin((60.0 * M_PI / 180.0) * std::exp(-std::pow((h_prime_avg_deg - 275.0) / 25.0, 2)));

    // Compute SL, SC, SH
    double L_prime_avg = (L1 + L2) / 2.0;
    double SL = 1.0 + 0.015 * std::pow(L_prime_avg - 50.0, 2) / std::sqrt(20 + std::pow(L_prime_avg - 50.0, 2));
    double SC = 1.0 + 0.045 * C_prime_avg;
    double SH = 1.0 + 0.015 * C_prime_avg * T;

    // Final color difference
    double deltaE = std::sqrt(std::pow(dL_prime / SL, 2) + std::pow(dC_prime / SC, 2) + std::pow(dH_prime / SH, 2) + R * (dC_prime / SC) * (dH_prime / SH));

    return deltaE;
}

} // namespace detail

double calc_lab_color_difference_by_ciede2000(const ColorDouble& lab1, const ColorDouble& lab2) {
    return ciede2000(lab1, lab2);
}

double calc_rgb_color_difference_by_ciede2000(const RGB& rgb1, const RGB& rgb2) {
    auto lab1 = convert_rgb_to_lab(rgb1);
    auto lab2 = convert_rgb_to_lab(rgb2);
    return ciede2000(lab1, lab2);
}

double calc_rgb_color_difference_by_ciede2000_srgb01(const ColorDouble& rgb1, const ColorDouble& rgb2) {
    const LAB lab1 = convert_srgb01_to_lab(rgb1);
    const LAB lab2 = convert_srgb01_to_lab(rgb2);
    return ciede2000(lab1, lab2);
}

// Working-space distance function type: inputs are two colors in the same space (RGB-double or Lab)
using WorkingDistFunc = double (*)(const ColorDouble&, const ColorDouble&);

namespace {

// One immutable working-color set and one K trial own this optional cache.
// All writes happen between parallel passes. Readers never lazily initialize
// or invalidate it; RGB and unqualified Lab inputs use the original scalar.
class PreparedWorkingDistances {
public:
    PreparedWorkingDistances(const std::vector<ColorDouble>& working_colors,
                             std::size_t k, WorkingDistFunc dist_func)
        : m_working_colors(working_colors), m_dist_func(dist_func) {
        if (dist_func != ciede2000 || !detail::has_default_lab_chroma_environment())
            return;
        try {
            m_point_chroma.resize(working_colors.size());
            m_center_chroma.resize(k);
        } catch (const std::bad_alloc&) {
            disable();
            return;
        }
        for (std::size_t i = 0; i < working_colors.size(); ++i) {
            if (!detail::prepare_lab_chroma(working_colors[i], m_point_chroma[i])) {
                disable();
                return;
            }
        }
        m_prepared = true;
    }

    // Used only after the original ordered centroid accumulation/division.
    // Failure invalidates the whole trial before any reader is launched.
    void prepare_color(const ColorDouble& color, double& chroma) {
        if (m_prepared && !detail::prepare_lab_chroma(color, chroma))
            disable();
    }

    // The caller refreshes immediately after writing all three components,
    // including empty-cluster replacement. In-place center update order stays
    // observable to the following empty-cluster search, exactly as before.
    void refresh_center(std::size_t index, const ColorDouble& center) {
        if (m_prepared && !detail::prepare_lab_chroma(center, m_center_chroma[index]))
            disable();
    }

    void refresh_centers(const std::vector<ColorDouble>& centers) {
        for (std::size_t i = 0; i < centers.size() && m_prepared; ++i)
            refresh_center(i, centers[i]);
    }

    // Called once at each reader range entry, never once per pair. Cached
    // values are valid only in the same verified default floating-point mode.
    bool can_use_on_this_thread() const {
        return m_prepared && detail::has_default_lab_chroma_environment();
    }

    double point_to_color(std::size_t index, const ColorDouble& color, double chroma,
                          bool use_prepared) const {
        if (use_prepared)
            return detail::ciede2000_prepared(m_working_colors[index], color,
                                              m_point_chroma[index], chroma);
        return m_dist_func(m_working_colors[index], color);
    }

    double point_to_center(std::size_t point, std::size_t center_index,
                           const ColorDouble& center, bool use_prepared) const {
        if (use_prepared)
            return detail::ciede2000_prepared(m_working_colors[point], center,
                                              m_point_chroma[point], m_center_chroma[center_index]);
        return m_dist_func(m_working_colors[point], center);
    }

private:
    void disable() {
        m_prepared = false;
        std::vector<double>().swap(m_point_chroma);
        std::vector<double>().swap(m_center_chroma);
    }

    const std::vector<ColorDouble>& m_working_colors;
    WorkingDistFunc m_dist_func;
    std::vector<double> m_point_chroma;
    std::vector<double> m_center_chroma;
    bool m_prepared {false};
};

} // namespace

// Farthest Point Sampling (FPS) initialization algorithm.
// The first center is the point nearest to the global centroid; subsequent centers are the points farthest from the existing set.
static std::vector<ColorDouble> farthest_point_sampling_init(const std::vector<ColorDouble>& working_colors, std::size_t k, PreparedWorkingDistances& distances) {
    const bool trace = std::getenv("ORCASLICER_UI_LATENCY_TRACE") != nullptr;
    const auto started = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<ColorDouble> centers(k);
    const auto available = std::max(1u, std::thread::hardware_concurrency());
    const int threads = std::min({4, int(available > 1 ? available - 1 : 1u),
        std::max(1, tbb::this_task_arena::max_concurrency())});
    tbb::task_arena arena(threads);
    bool parallel = threads > 1 && working_colors.size() >= 256;
    const auto for_each_point = [&](const auto& operation) {
        if (parallel) {
            try {
                arena.execute([&] {
                    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, working_colors.size(), 256),
                        [&](const tbb::blocked_range<std::size_t>& range) {
                            const bool use_prepared = distances.can_use_on_this_thread();
                            for (std::size_t i = range.begin(); i < range.end(); ++i) operation(i, use_prepared);
                        });
                });
                return;
            } catch (const std::exception& failure) {
                // Distance writes are independent and min updates idempotent.
                // Recompute the entire pass before selecting any center.
                parallel = false;
                BOOST_LOG_TRIVIAL(warning) << "[NativeClustering] seed distance pass failed; retrying sequentially: " << failure.what();
            }
        }
        const bool use_prepared = distances.can_use_on_this_thread();
        for (std::size_t i = 0; i < working_colors.size(); ++i) operation(i, use_prepared);
    };

    // 1. Compute the global centroid and pick the nearest point as the first center
    ColorDouble centroid = {0.0, 0.0, 0.0};
    for (const auto& c : working_colors) {
        centroid[0] += c[0];
        centroid[1] += c[1];
        centroid[2] += c[2];
    }
    const auto n = static_cast<double>(working_colors.size());
    centroid[0] /= n;
    centroid[1] /= n;
    centroid[2] /= n;
    double centroid_chroma = 0.0;
    distances.prepare_color(centroid, centroid_chroma);

    double best_dist = std::numeric_limits<double>::max();
    std::size_t first_idx = 0;
    // Reuse the original minimum-distance buffer. Only independent distances
    // run in parallel; centroid sums and all first-match choices stay ordered.
    std::vector<double> min_distances(working_colors.size());
    for_each_point([&](std::size_t i, bool use_prepared) { min_distances[i] = distances.point_to_color(i, centroid, centroid_chroma, use_prepared); });
    for (std::size_t i = 0; i < working_colors.size(); ++i) {
        const double d = min_distances[i];
        if (d < best_dist) {
            best_dist = d;
            first_idx = i;
        }
    }
    centers[0] = working_colors[first_idx];
    distances.refresh_center(0, centers[0]);

    // Minimum distance from each point to the already-selected center set
    std::fill(min_distances.begin(), min_distances.end(), std::numeric_limits<double>::max());

    // 2. Greedily select the remaining K-1 centers: pick the point with the largest min_distance each time
    for (std::size_t i = 1; i < k; ++i) {
        const ColorDouble& last_center = centers[i - 1];

        // Update each point's minimum distance with the newly added center
        double farthest_dist = -1.0;
        std::size_t farthest_idx = 0;
        for_each_point([&](std::size_t c_idx, bool use_prepared) {
            const double d = distances.point_to_center(c_idx, i - 1, last_center, use_prepared);
            if (d < min_distances[c_idx]) {
                min_distances[c_idx] = d;
            }
        });
        for (std::size_t c_idx = 0; c_idx < working_colors.size(); ++c_idx) {
            if (min_distances[c_idx] > farthest_dist) {
                farthest_dist = min_distances[c_idx];
                farthest_idx = c_idx;
            }
        }

        centers[i] = working_colors[farthest_idx];
        distances.refresh_center(i, centers[i]);
    }

    if (trace)
        BOOST_LOG_TRIVIAL(info) << "[NativeClustering] stage=seed elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
            << " samples=" << working_colors.size() << " centers=" << k << " workers=" << (parallel ? threads : 1);
    return centers;
}

bool remesh_mesh(TriMesh& bbs_mesh, std::vector<std::size_t>& face_labels, double target_edge_length_ratio) {
    if (face_labels.size() != bbs_mesh.indices.size()) {
        BOOST_LOG_TRIVIAL(warning) << "Input mesh face count does not match label count";
        return false;
    }

    // Back up face labels for recovery after remeshing.
    std::vector<std::size_t> face_labels_of_original_mesh(face_labels);

    CGALMesh cgal_mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, cgal_mesh);

    // AABBTreeIndirect references vertices/faces externally, so snapshot the
    // pre-remesh geometry by moving it out of bbs_mesh (which is overwritten
    // below with the post-remesh mesh). std::move on std::vector is O(1).
    TriVertices old_vertices = std::move(bbs_mesh.vertices);
    TriFaces    old_indices  = std::move(bbs_mesh.indices);
    auto original_mesh_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(old_vertices, old_indices);

    std::unordered_set<CGAL::SM_Edge_index> feature_edges;
    std::unordered_set<CGAL::SM_Vertex_index> feature_vertices;
    CGALMesh::Property_map<CGALMesh::Edge_index, bool> constrained_edges =
        cgal_mesh.add_property_map<CGALMesh::Edge_index, bool>("constrained_edges", false).first;
    CGALMesh::Property_map<CGALMesh::Vertex_index, bool> constrained_vertices =
        cgal_mesh.add_property_map<CGALMesh::Vertex_index, bool>("constrained_vertices", false).first;

    // An edge is considered a geometric feature edge if its dihedral angle is less than 135 degrees (loose threshold)
    constexpr double feature_angle = 135;

    auto is_feature_edge = [&](CGAL::SM_Edge_index edge) -> bool {
        if (cgal_mesh.is_border(edge)) {
            return true;
        }
        auto halfedge_1 = cgal_mesh.halfedge(edge);
        auto halfedge_2 = cgal_mesh.opposite(halfedge_1);
        auto face_1 = cgal_mesh.face(halfedge_1);
        auto face_2 = cgal_mesh.face(halfedge_2);
        if (face_labels[face_1] != face_labels[face_2]) {
            // Boundary between different color regions; treated as a feature edge
            return true;
        }
        // TODO: CGAL remeshing tends to crash when too many constrained edges are added; needs handling
        //auto normal_1 = PMP::compute_face_normal(face_1, cgal_mesh);
        //auto normal_2 = PMP::compute_face_normal(face_2, cgal_mesh);
        //double angle = 180 - get_angle_between_vectors(normal_1, normal_2);
        //BOOST_LOG_TRIVIAL(debug) << "end.\n";
        //return angle > feature_angle;
        return false;
    };

    for (auto edge : cgal_mesh.edges()) {
        if (is_feature_edge(edge)) {
            feature_edges.insert(edge);
            feature_vertices.insert(cgal_mesh.source(cgal_mesh.halfedge(edge)));
            feature_vertices.insert(cgal_mesh.target(cgal_mesh.halfedge(edge)));
            constrained_edges[edge] = true;
            constrained_vertices[cgal_mesh.source(cgal_mesh.halfedge(edge))] = true;
            constrained_vertices[cgal_mesh.target(cgal_mesh.halfedge(edge))] = true;
        }
    }

#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "remesh_mesh: feature_edges.size() = " << feature_edges.size() << ".\n";

    std::vector<std::vector<CGALKernel::Point_3>> polylines;
    for (auto edge : feature_edges) {
        auto src_vtx = cgal_mesh.source(cgal_mesh.halfedge(edge));
        auto trg_vtx = cgal_mesh.target(cgal_mesh.halfedge(edge));
        polylines.push_back({cgal_mesh.point(src_vtx), cgal_mesh.point(trg_vtx)});
    }
    save_polylines("ColorUtils_remesh_feature_lines.obj", polylines);
#endif  // DEBUG_FLAG

    std::size_t iters = 5;
#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "PMP::isotropic_remeshing start...\n";
#endif  // DEBUG_FLAG
    // TODO: CGAL remeshing preserves geometric boundaries but does not maintain face labels well; colors need to be recomputed
    PMP::isotropic_remeshing(cgal_mesh.faces(), target_edge_length_ratio * detail::average_edge_length_impl(cgal_mesh), cgal_mesh,
                             CGAL::parameters::number_of_iterations(iters)
                                 .protect_constraints(true)
                                 .edge_is_constrained_map(constrained_edges)
                                 .vertex_is_constrained_map(constrained_vertices)
                                 .collapse_constraints(true));
#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "PMP::isotropic_remeshing finshed...\n";
#endif  // DEBUG_FLAG
    if (PMP::does_self_intersect(cgal_mesh)) {
        PMP::experimental::remove_self_intersections(cgal_mesh);
    }

    bbs_mesh.clear();

    std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_cgal_vtx_to_bbs_vtx;
    TriVertices bbs_vertices;
    TriFaces bbs_faces;
    face_labels.clear();
    face_labels.reserve(cgal_mesh.number_of_faces());

    for (const auto& cgal_vtx : cgal_mesh.vertices()) {
        if (!cgal_mesh.is_valid(cgal_vtx) || cgal_mesh.is_removed(cgal_vtx) || cgal_mesh.is_isolated(cgal_vtx)) {
            continue;
        }
        if (!map_cgal_vtx_to_bbs_vtx.count(cgal_vtx)) {
            map_cgal_vtx_to_bbs_vtx[cgal_vtx] = bbs_vertices.size();
            const auto& cgal_point = cgal_mesh.point(cgal_vtx);
            bbs_vertices.emplace_back(TriVertex(cgal_point.x(), cgal_point.y(), cgal_point.z()));
        }
    }

    for (const auto& cgal_face : cgal_mesh.faces()) {
        if (!cgal_mesh.is_valid(cgal_face) || cgal_mesh.is_removed(cgal_face)) {
            continue;
        }
        TriFace bbs_face;
        std::size_t face_vid = 0;
        for (auto cgal_vtx : cgal_mesh.vertices_around_face(cgal_mesh.halfedge(cgal_face))) {
            do { if (!(map_cgal_vtx_to_bbs_vtx.count(cgal_vtx))) { BOOST_LOG_TRIVIAL(warning) << "CGAL mesh contains a face with an invalid vertex"; return false; } } while(0);
            bbs_face[face_vid] = map_cgal_vtx_to_bbs_vtx[cgal_vtx];
            ++face_vid;
        }
        bbs_faces.push_back(std::move(bbs_face));
    }

    bbs_mesh = TriMesh(bbs_faces, bbs_vertices);

    face_labels.resize(bbs_mesh.indices.size());
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, bbs_mesh.indices.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            auto& face = bbs_mesh.indices[fid];
            Vec3f face_centroid = (bbs_mesh.vertices[face[0]] + bbs_mesh.vertices[face[1]] + bbs_mesh.vertices[face[2]]) / 3.0;
            size_t hit_idx = 0;
            Vec3f  closest;
            AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                old_vertices, old_indices, original_mesh_tree, face_centroid, hit_idx, closest);
            face_labels[fid] = face_labels_of_original_mesh[hit_idx];
        }
    });

    return true;
}

bool is_closed(const TriMesh& bbs_mesh) {
    CGALMesh cgal_mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, cgal_mesh);

#ifdef DEBUG_FLAG
    std::size_t border_edges_count = 0;
    std::size_t edges_count = 0;
    for (auto edge : cgal_mesh.edges()) {
        if (cgal_mesh.is_border(edge)) {
            ++border_edges_count;
        }
        ++edges_count;
    }

    BOOST_LOG_TRIVIAL(debug) << "border edges count = " << border_edges_count << "\n";
    BOOST_LOG_TRIVIAL(debug) << "edges count = " << edges_count << "\n";

    std::size_t border_faces_count = 0;
    std::size_t faces_count = 0;
    for (auto face : cgal_mesh.faces()) {
        for (auto halfedge : cgal_mesh.halfedges_around_face(cgal_mesh.halfedge(face))) {
            auto edge = cgal_mesh.edge(halfedge);
            if (cgal_mesh.is_border(edge)) {
                ++border_faces_count;
                break;
            }
        }
        ++faces_count;
    }

    BOOST_LOG_TRIVIAL(debug) << "border faces count = " << border_faces_count << "\n";
    BOOST_LOG_TRIVIAL(debug) << "faces count = " << faces_count << "\n";

    BOOST_LOG_TRIVIAL(debug) << "vertices count = " << cgal_mesh.number_of_vertices() << "\n";
    std::size_t num_of_components = 0;
    std::unordered_set<CGAL::SM_Face_index> visited_faces;
    for (auto src_face : cgal_mesh.faces()) {
        if (visited_faces.count(src_face)) {
            continue;
        }
        ++num_of_components;
        std::queue<CGAL::SM_Face_index> que;
        que.push(src_face);
        visited_faces.insert(src_face);
        while (!que.empty()) {
            auto curr_face = que.front();
            que.pop();
            for (auto adj_face : cgal_mesh.faces_around_face(cgal_mesh.halfedge(curr_face))) {
                if (!cgal_mesh.is_valid(adj_face) || cgal_mesh.is_removed(adj_face) || visited_faces.count(adj_face)) {
                    continue;
                }
                que.push(adj_face);
                visited_faces.insert(adj_face);
            }
        }
    }
    BOOST_LOG_TRIVIAL(debug) << "components count = " << num_of_components << "\n";
#endif  // DEBUG_FLAG

    return CGAL::is_closed(cgal_mesh);
}

struct RegionSmoothingWorkspace::Impl {
    CGALMesh mesh;
    std::vector<std::array<std::size_t, 3>> neighbors;
    bool valid = false;
    bool finished = false;
};

RegionSmoothingWorkspace::RegionSmoothingWorkspace(const TriMesh& source, const SmoothParameters& parameters)
    : m_impl(std::make_unique<Impl>())
{
    const auto started = SmoothingClock::now();
    if (smoothing_canceled(parameters) || !cgalutils::convert_trimesh_to_cgal(source, m_impl->mesh) ||
        m_impl->mesh.number_of_faces() != source.indices.size()) return;
    auto& mesh = m_impl->mesh;
    m_impl->neighbors.resize(mesh.num_faces());
    for (auto face : mesh.faces()) {
        if ((std::size_t(face) & 4095) == 0 && smoothing_canceled(parameters)) return;
        auto& adjacent = m_impl->neighbors[face];
        adjacent.fill(std::numeric_limits<std::size_t>::max());
        std::size_t count = 0;
        for (auto neighbor : mesh.faces_around_face(mesh.halfedge(face))) {
            if (neighbor == CGALMesh::null_face() || !mesh.is_valid(neighbor) || mesh.is_removed(neighbor)) continue;
            if (count == adjacent.size()) return;
            adjacent[count++] = std::size_t(neighbor);
        }
    }
    m_impl->valid = true;
    record_smoothing_stage(parameters, "prepare", started, mesh.number_of_faces());
}

RegionSmoothingWorkspace::~RegionSmoothingWorkspace() = default;

bool RegionSmoothingWorkspace::apply_topology(std::vector<std::size_t>& labels, const SmoothParameters& parameters)
{
    if (!m_impl->valid || m_impl->finished) return false;
    if (labels.size() != m_impl->mesh.number_of_faces() || smoothing_canceled(parameters)) {
        m_impl->valid = false;
        return false;
    }
    const bool success = !(parameters.smooth_weight >= TOPO_SMOOTH_WEIGHT_THRESHOLD) ||
        smooth_region_topo_boundary(m_impl->mesh, labels, m_impl->neighbors, parameters);
    if (!success) m_impl->valid = false;
    return success;
}

bool RegionSmoothingWorkspace::finish(TriMesh& output, const std::vector<std::size_t>& labels,
                                     const SmoothParameters& parameters)
{
    if (!m_impl->valid || m_impl->finished) return false;
    // A canceled or failed finish cannot later install its partial workspace.
    m_impl->valid = false;
    if (labels.size() != m_impl->mesh.number_of_faces() || smoothing_canceled(parameters)) return false;
    if (parameters.smooth_weight > EPSILON) {
        std::vector<CGALKernel::Point_3> original_points;
        original_points.reserve(m_impl->mesh.num_vertices());
        for (auto vertex : m_impl->mesh.vertices()) original_points.push_back(m_impl->mesh.point(vertex));
        try {
            if (!smooth_region_geom_boundary(m_impl->mesh, labels, parameters)) return false;
        } catch (const std::exception&) {
            if (smoothing_canceled(parameters)) return false;
            // A failed optimized query cannot feed the fallback mutated data.
            std::size_t index = 0;
            for (auto vertex : m_impl->mesh.vertices()) m_impl->mesh.point(vertex) = original_points[index++];
            BOOST_LOG_TRIVIAL(warning) << "[NativeSmoothing] retrying exact serial projection from immutable geometry";
            if (!smooth_region_geom_boundary(m_impl->mesh, labels, parameters, false)) return false;
        }
    }
    const auto started = SmoothingClock::now();
    auto result = cgalutils::cgal_to_trimesh(m_impl->mesh);
    if (smoothing_canceled(parameters)) return false;
    output = std::move(result);
    m_impl->finished = true;
    record_smoothing_stage(parameters, "export", started, output.indices.size());
    return true;
}

static bool smooth_region_labels(TriMesh& tri_mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& parameters)
{
    RegionSmoothingWorkspace workspace(tri_mesh, parameters);
    return workspace.apply_topology(face_labels, parameters) && workspace.finish(tri_mesh, face_labels, parameters);
}

bool smooth_region(TriMesh& tri_mesh, std::vector<std::array<std::size_t, 3>>& face_colors, const SmoothParameters& smooth_parameters) {
    // Convert colors to labels
    std::size_t label_next = 0;
    std::vector<std::size_t> face_labels;
    face_labels.reserve(face_colors.size());
    std::map<std::array<std::size_t, 3>, std::size_t> map_color_to_label;
    std::unordered_map<std::size_t, std::array<std::size_t, 3>> map_label_to_color;
    for (auto& color : face_colors) {
        if (!map_color_to_label.count(color)) {
            map_color_to_label[color] = label_next;
            map_label_to_color[label_next] = color;
            ++label_next;
        }
        face_labels.push_back(map_color_to_label[color]);
    }

    if (!smooth_region_labels(tri_mesh, face_labels, smooth_parameters))
        return false;

    // Convert labels back to colors
    for (std::size_t fid = 0; fid < face_labels.size(); ++fid) {
        face_colors[fid] = map_label_to_color[face_labels[fid]];
    }

    return true;
}

bool smooth_region(TriMesh& tri_mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& smooth_parameters) {
    return smooth_region_labels(tri_mesh, face_labels, smooth_parameters);
}

// Compute the squared Euclidean distance between two colors (RGB vectors)
double calc_rgb_color_difference_by_squared_rgb_double(const ColorDouble& c1, const ColorDouble& c2) {
    double dr = c1[0] - c2[0];
    double dg = c1[1] - c2[1];
    double db = c1[2] - c2[2];
    return dr * dr + dg * dg + db * db;
}

// Compute the squared Euclidean distance between two colors (RGB vectors)
double calc_rgb_color_difference_by_squared_rgb(const RGB& c1, const RGB& c2) {
    auto c1d = convert_rgb_uint_to_rgb_double(c1);
    auto c2d = convert_rgb_uint_to_rgb_double(c2);
    return calc_rgb_color_difference_by_squared_rgb_double(c1d, c2d);
}

static void record_clustering_stage(const ClusterParameters& parameters, const char* stage,
                                    SmoothingClock::duration elapsed, std::size_t count)
{
    const auto milliseconds = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    BOOST_LOG_TRIVIAL(info) << "[NativeClustering] stage=" << stage << " elapsed_ms=" << milliseconds << " count=" << count;
    if (parameters.diagnostic_callback) {
        try { parameters.diagnostic_callback(stage, milliseconds, count); }
        catch (...) { /* Diagnostics must not change clustering or repair success. */ }
    }
}

// K-Means core: run FPS initialization + assign/update iterations in working space, return cluster centers
static std::vector<ColorDouble> kmeans_core(const std::vector<ColorDouble>& working_colors, std::size_t k, std::size_t max_iter,
                                            const ClusterParameters& parameters, PreparedWorkingDistances& distances) {
    const auto fps_started = SmoothingClock::now();
    std::vector<ColorDouble> centers = farthest_point_sampling_init(working_colors, k, distances);
    record_clustering_stage(parameters, "kmeans.fps", SmoothingClock::now() - fps_started, k);
    std::vector<std::size_t> assignments(working_colors.size());
    SmoothingClock::duration assignment_time{}, update_time{};
    std::size_t assignment_count = 0, update_count = 0, stable_centers = 0;
    std::size_t pending_stable_columns = 0, assignment_reusable_columns = 0;
    const auto record_iterations = [&] {
        record_clustering_stage(parameters, "kmeans.assignment", assignment_time, assignment_count);
        record_clustering_stage(parameters, "kmeans.update", update_time, update_count);
        record_clustering_stage(parameters, "kmeans.stable_centers", {}, stable_centers);
        record_clustering_stage(parameters, "kmeans.center_comparisons", {}, k * update_count);
        record_clustering_stage(parameters, "kmeans.assignment_reusable_columns", {}, assignment_reusable_columns);
        record_clustering_stage(parameters, "kmeans.assignment_column_comparisons", {},
            assignment_count > 0 ? k * (assignment_count - 1) : 0);
        record_clustering_stage(parameters, "kmeans.working_colors", {}, working_colors.size());
    };
    const auto& cancel_cb = parameters.cancel_callback;

    for (std::size_t iter = 0; iter < max_iter; ++iter) {
        if (cancel_cb && cancel_cb()) { record_iterations(); return centers; }
        const auto stable_columns_for_pass = pending_stable_columns;
        std::atomic<bool> changed(false);

        const auto assignment_started = SmoothingClock::now();
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, working_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
            const bool use_prepared = distances.can_use_on_this_thread();
            for (std::size_t i = range.begin(); i < range.end(); ++i) {
                double min_dist = std::numeric_limits<double>::max();
                std::size_t best_cluster = 0;

                for (std::size_t j = 0; j < k; ++j) {
                    double d = distances.point_to_center(i, j, centers[j], use_prepared);
                    if (d < min_dist) {
                        min_dist = d;
                        best_cluster = j;
                    }
                }

                if (assignments[i] != best_cluster) {
                    changed.store(true, std::memory_order_relaxed);
                    assignments[i] = best_cluster;
                }
            }
        });
        assignment_time += SmoothingClock::now() - assignment_started;
        ++assignment_count;
        // Count only stable centers consumed by a complete next assignment.
        // Final updates, cancellation and failed passes cannot claim a hit.
        assignment_reusable_columns += stable_columns_for_pass;
        pending_stable_columns = 0;

        if (!changed.load()) {
            break;
        }

        const auto update_started = SmoothingClock::now();
        std::vector<ColorDouble> new_centers(k, {0.0, 0.0, 0.0});
        std::vector<std::size_t> counts(k, 0);

        for (std::size_t i = 0; i < working_colors.size(); ++i) {
            std::size_t cluster_id = assignments[i];
            ++counts[cluster_id];
            new_centers[cluster_id][0] += working_colors[i][0];
            new_centers[cluster_id][1] += working_colors[i][1];
            new_centers[cluster_id][2] += working_colors[i][2];
        }

        for (std::size_t i = 0; i < k; ++i) {
            const auto previous_center = centers[i];
            if (counts[i] == 0) {
                const bool use_prepared = distances.can_use_on_this_thread();
                double max_min_dist = -1.0;
                std::size_t best_idx = 0;
                for (std::size_t p = 0; p < working_colors.size(); ++p) {
                    double nearest = std::numeric_limits<double>::max();
                    for (std::size_t c = 0; c < k; ++c) {
                        if (c == i || counts[c] == 0) {
                            continue;
                        }
                        double d = distances.point_to_center(p, c, centers[c], use_prepared);
                        if (d < nearest) {
                            nearest = d;
                        }
                    }
                    if (nearest > max_min_dist) {
                        max_min_dist = nearest;
                        best_idx = p;
                    }
                }
                centers[i] = working_colors[best_idx];
            } else {
                centers[i][0] = new_centers[i][0] / counts[i];
                centers[i][1] = new_centers[i][1] / counts[i];
                centers[i][2] = new_centers[i][2] / counts[i];
            }
            distances.refresh_center(i, centers[i]);
            // Passive evidence for future request-local reuse. It neither
            // selects a computation path nor changes the original update order.
            if (std::memcmp(previous_center.data(), centers[i].data(), 3 * sizeof(double)) == 0) {
                ++stable_centers;
                ++pending_stable_columns;
            }
        }
        update_time += SmoothingClock::now() - update_started;
        ++update_count;
    }

    record_iterations();
    return centers;
}

// K-Means clustering algorithm
std::vector<Color> cluster_k_means(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters) {
    std::size_t k = cluster_parameters.cluster_k;
    std::size_t max_iter = cluster_parameters.max_iter;

    if (k == 0 || colors.empty()) {
        return {};
    }

    const bool use_lab = (cluster_parameters.color_difference_method != ColorDifferenceMethod::RGB);
    WorkingDistFunc working_dist_func = use_lab ? ciede2000 : calc_rgb_color_difference_by_squared_rgb_double;

    // ==========================================
    // 1. Preprocessing: deduplicate + pre-convert to working space
    // ==========================================
    std::vector<Color> unique_colors = colors;
    std::sort(unique_colors.begin(), unique_colors.end());
    auto last = std::unique(unique_colors.begin(), unique_colors.end());
    unique_colors.erase(last, unique_colors.end());

#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "Input colors: " << colors.size() << ", Unique colors: " << unique_colors.size();
#endif  // DEBUG_FLAG

    if (unique_colors.size() < k) {
        BOOST_LOG_TRIVIAL(warning) << "Unique color count (" << unique_colors.size() << ") is less than target K (" << k << "). Adjusting K.";
        k = unique_colors.size();
        if (k == 0) {
            return {};
        }
        return unique_colors;
    }

    std::vector<ColorDouble> working_colors(unique_colors.size());
    for (std::size_t i = 0; i < unique_colors.size(); ++i) {
        if (use_lab) {
            working_colors[i] = convert_rgb_to_lab(unique_colors[i]);
        } else {
            working_colors[i] = {static_cast<double>(unique_colors[i][0]), static_cast<double>(unique_colors[i][1]), static_cast<double>(unique_colors[i][2])};
        }
    }

    // ==========================================
    // 2. K-Means clustering
    // ==========================================
    PreparedWorkingDistances distances(working_colors, k, working_dist_func);
    auto centers = kmeans_core(working_colors, k, max_iter, cluster_parameters, distances);

    // ==========================================
    // 3. Output: convert from working space back to RGB
    // ==========================================
    std::vector<Color> result(k);
    for (std::size_t i = 0; i < k; ++i) {
        if (use_lab) {
            result[i] = convert_lab_to_rgb(centers[i]);
        } else {
            result[i] = {static_cast<std::size_t>(std::round(centers[i][0])), static_cast<std::size_t>(std::round(centers[i][1])),
                         static_cast<std::size_t>(std::round(centers[i][2]))};
        }
    }

    return result;
}

std::vector<Color> cluster_to_specified_colors(const std::vector<Color>& colors, const std::vector<Color>& specified_colors) {
    std::vector<Color> cluster_colors = colors;
    std::vector<ColorDouble> specified_double_colors;
    specified_double_colors.reserve(specified_colors.size());
    for (auto& color : specified_colors) {
        specified_double_colors.push_back(ColorDouble{static_cast<double>(color[0]), static_cast<double>(color[1]), static_cast<double>(color[2])});
    }

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, cluster_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t i = range.begin(); i < range.end(); ++i) {
            ColorDouble p_color{static_cast<double>(cluster_colors[i][0]), static_cast<double>(cluster_colors[i][1]),
                                static_cast<double>(cluster_colors[i][2])};

            double min_dist = std::numeric_limits<double>::max();
            std::size_t best_cluster = 0;

            for (std::size_t j = 0; j < specified_double_colors.size(); ++j) {
                double d = calc_rgb_color_difference_by_squared_rgb_double(p_color, specified_double_colors[j]);
                if (d < min_dist) {
                    min_dist = d;
                    best_cluster = j;
                }
            }

            cluster_colors[i] = specified_colors[best_cluster];
        }
    });

    return cluster_colors;
}

// Color PCA struct
struct ColorPCA {
    std::size_t color_idx;  // Index of the original color in ColorList
    double pca_value;  // Projection value onto the first principal component

    ColorPCA(std::size_t c_idx, double pca_val)
        : color_idx(c_idx),
          pca_value(pca_val) {}

    // Comparison operator (ascending order)
    bool operator<(const ColorPCA& other) const { return pca_value < other.pca_value; }

    // Equality check
    bool operator==(const ColorPCA& other) const { return color_idx == other.color_idx; }
};

[[maybe_unused]] static std::vector<ColorPCA> sort_colors_by_pca(const std::vector<Color>& colors) {
    const std::size_t n = colors.size();

    if (n == 0) {
        return {};
    }

    if (n == 1) {
        return {{0, 0.0}};
    }

    // Step 1: Data preprocessing - normalize to [0, 1]
    Eigen::MatrixXd data(n, 3);

    for (std::size_t i = 0; i < n; ++i) {
        data(i, 0) = static_cast<double>(colors[i][0]) / 255.0;  // R
        data(i, 1) = static_cast<double>(colors[i][1]) / 255.0;  // G
        data(i, 2) = static_cast<double>(colors[i][2]) / 255.0;  // B
    }

    // Step 2: Compute mean and center the data
    Eigen::RowVector3d mean = data.colwise().mean();
    Eigen::MatrixXd centered = data.rowwise() - mean;

    // Step 3: Compute covariance matrix (3x3)
    Eigen::Matrix3d cov = (centered.adjoint() * centered) / static_cast<double>(n - 1);

    // Step 4: Eigenvalue decomposition
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);

    if (solver.info() != Eigen::Success) {
#ifdef DEBUG_FLAG
        std::cerr << "PCA: Eigenvalue decomposition failed" << std::endl;
#endif  // DEBUG_FLAG
        // Fallback: return an approximate result sorted by luminance.
        // Luminance is a key perceptual feature; convert RGB to grayscale (L = 0.299R + 0.587G + 0.114B) and sort in ascending order.
        std::vector<ColorPCA> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            double luminance = 0.299 * colors[i][0] + 0.587 * colors[i][1] + 0.114 * colors[i][2];
            result.push_back({i, luminance});
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    // Get eigenvalues and eigenvectors (sorted by eigenvalue in descending order)
    Eigen::Vector3d eigenvalues = solver.eigenvalues();
    Eigen::Matrix3d eigenvectors = solver.eigenvectors();

    // Step 5: Find the eigenvector corresponding to the largest eigenvalue (first principal component)
    Eigen::MatrixXd::Index max_eigenvalue_idx;
    eigenvalues.maxCoeff(&max_eigenvalue_idx);

    Eigen::Vector3d first_principal_component = eigenvectors.col(max_eigenvalue_idx);

    // Step 6: Project centered data onto the first principal component
    Eigen::VectorXd projections = centered * first_principal_component;

    // Step 7: Build result and sort
    std::vector<ColorPCA> result;
    result.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        result.push_back({i, projections(i)});
    }

    std::sort(result.begin(), result.end());

    return result;
}

std::vector<Color> cluster_adaptive(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters) {
    if (colors.empty()) {
        return {};
    }

    const double max_color_distance = cluster_parameters.max_color_distance;
    const double max_tail_color_distance = cluster_parameters.max_tail_color_distance;
    const std::size_t max_iter = cluster_parameters.max_iter;
    const bool use_lab = (cluster_parameters.color_difference_method != ColorDifferenceMethod::RGB);
    WorkingDistFunc working_dist_func = use_lab ? ciede2000 : calc_rgb_color_difference_by_squared_rgb_double;

    // ==========================================
    // 1. Deduplicate + convert to working space
    // ==========================================
    const auto prepare_started = SmoothingClock::now();
    std::vector<Color> unique_colors = colors;
    std::sort(unique_colors.begin(), unique_colors.end());
    auto last = std::unique(unique_colors.begin(), unique_colors.end());
    unique_colors.erase(last, unique_colors.end());

    BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive: colors=" << colors.size()
                             << " unique=" << unique_colors.size()
                             << " min_k=" << cluster_parameters.min_cluster_k
                             << " max_k=" << cluster_parameters.max_cluster_k
                             << " max_color_distance=" << max_color_distance
                             << " max_tail_color_distance=" << max_tail_color_distance;

    if (unique_colors.size() <= 1) {
        return unique_colors;
    }

    std::vector<ColorDouble> working_colors(unique_colors.size());
    for (std::size_t i = 0; i < unique_colors.size(); ++i) {
        if (use_lab) {
            working_colors[i] = convert_rgb_to_lab(unique_colors[i]);
        } else {
            working_colors[i] = {static_cast<double>(unique_colors[i][0]), static_cast<double>(unique_colors[i][1]), static_cast<double>(unique_colors[i][2])};
        }
    }
    record_clustering_stage(cluster_parameters, "adaptive.prepare", SmoothingClock::now() - prepare_started, unique_colors.size());

    // ==========================================
    // 2. Binary search k: find the smallest k where P99 radius <= max_color_distance
    // ==========================================
    const std::size_t max_k = std::max<std::size_t>(1,
        std::min<std::size_t>(cluster_parameters.max_cluster_k, unique_colors.size()));
    const std::size_t min_k = std::min<std::size_t>(
        std::max<std::size_t>(1, cluster_parameters.min_cluster_k), max_k);
    std::size_t lo = min_k;
    std::size_t hi = max_k;
    std::size_t best_k = 0;
    std::vector<ColorDouble> best_centers;

    constexpr double kRadiusPercentile = 0.99;

    struct ClusterRadius {
        double percentile {0.0};
        double tail {0.0};
    };
    auto calc_radius = [&](const std::vector<ColorDouble>& centers, PreparedWorkingDistances& prepared_distances) -> ClusterRadius {
        prepared_distances.refresh_centers(centers);
        std::vector<double> distances(working_colors.size());
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, working_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
            const bool use_prepared = prepared_distances.can_use_on_this_thread();
            for (std::size_t i = range.begin(); i < range.end(); ++i) {
                double min_dist = std::numeric_limits<double>::max();
                for (std::size_t j = 0; j < centers.size(); ++j) {
                    double d = prepared_distances.point_to_center(i, j, centers[j], use_prepared);
                    if (d < min_dist) {
                        min_dist = d;
                    }
                }
                distances[i] = min_dist;
            }
        });
        if (distances.empty()) {
            return {};
        }
        const double tail = *std::max_element(distances.begin(), distances.end());
        std::size_t idx = std::min<std::size_t>(static_cast<std::size_t>(distances.size() * kRadiusPercentile), distances.size() - 1);
        std::nth_element(distances.begin(), distances.begin() + idx, distances.end());
        return {distances[idx], tail};
    };

    const auto& cancel_cb = cluster_parameters.cancel_callback;

    while (lo <= hi) {
        if (cancel_cb && cancel_cb()) return {};
        std::size_t mid = lo + (hi - lo) / 2;
        PreparedWorkingDistances distances(working_colors, mid, working_dist_func);
        auto centers = kmeans_core(working_colors, mid, max_iter, cluster_parameters, distances);
        if (cancel_cb && cancel_cb()) return {};
        const auto radius_started = SmoothingClock::now();
        const ClusterRadius radius = calc_radius(centers, distances);
        record_clustering_stage(cluster_parameters, "adaptive.radius", SmoothingClock::now() - radius_started, mid);

        BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive binary search: k=" << mid
                                 << " p99_radius=" << radius.percentile
                                 << " tail_radius=" << radius.tail;

        const bool tail_satisfied = max_tail_color_distance <= 0.0 ||
            radius.tail <= max_tail_color_distance;
        if (radius.percentile <= max_color_distance && tail_satisfied) {
            best_k = mid;
            best_centers = std::move(centers);
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    if (best_k == 0) {
        best_k = std::min<std::size_t>(max_k, unique_colors.size());
        PreparedWorkingDistances distances(working_colors, best_k, working_dist_func);
        best_centers = kmeans_core(working_colors, best_k, max_iter, cluster_parameters, distances);
        BOOST_LOG_TRIVIAL(warning) << "cluster_adaptive: binary search found no k satisfying max_radius<="
                                   << max_color_distance << ", fallback to k=" << best_k;
    }

    BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive: best_k=" << best_k;

    // ==========================================
    // 3. Convert centers back to RGB
    // ==========================================
    std::vector<Color> result(best_k);
    for (std::size_t i = 0; i < best_k; ++i) {
        if (use_lab) {
            result[i] = convert_lab_to_rgb(best_centers[i]);
        } else {
            result[i] = {static_cast<std::size_t>(std::round(best_centers[i][0])), static_cast<std::size_t>(std::round(best_centers[i][1])),
                         static_cast<std::size_t>(std::round(best_centers[i][2]))};
        }
    }

    return result;
}

static std::vector<std::vector<CGAL::SM_Face_index>> get_connected_face_groups(const CGALMesh& mesh) {
    std::vector<std::vector<CGAL::SM_Face_index>> face_groups;
    std::unordered_set<CGAL::SM_Face_index> visited_faces;
    for (auto src_face : mesh.faces()) {
        if (visited_faces.count(src_face)) {
            continue;
        }
        std::vector<CGAL::SM_Face_index> face_group;
        std::queue<CGAL::SM_Face_index> que;
        que.push(src_face);
        visited_faces.insert(src_face);
        while (!que.empty()) {
            auto curr_face = que.front();
            que.pop();
            face_group.push_back(curr_face);
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(curr_face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face) || visited_faces.count(adj_face)) {
                    continue;
                }
                que.push(adj_face);
                visited_faces.insert(adj_face);
            }
        }
        face_groups.push_back(std::move(face_group));
    }
    return face_groups;
}

bool get_components(const TriMesh& bbs_mesh, const std::vector<Vec2f>& bbs_vertex_uvs, std::vector<TriMesh>& component_meshes,
                    std::vector<std::vector<Vec2f>>& component_vertex_uvs) {
    component_meshes.clear();
    component_vertex_uvs.clear();

    if (bbs_mesh.vertices.size() != bbs_vertex_uvs.size()) {
        BOOST_LOG_TRIVIAL(warning) << "Input mesh vertex count does not match texture coordinate count";
        return false;
    }

    CGALMesh cgal_mesh;
    std::vector<Vec2f> cgal_vertex_uvs;
    if (!cgalutils::convert_trimesh_to_cgal(bbs_mesh, bbs_vertex_uvs, cgal_mesh, cgal_vertex_uvs)) {
        BOOST_LOG_TRIVIAL(warning) << "Mesh conversion failed";
        return false;
    }

    auto face_groups = get_connected_face_groups(cgal_mesh);

    for (const auto& faces : face_groups) {
        std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_cgal_vtx_to_bbs_vtx;
        TriVertices bbs_vertices;
        TriFaces bbs_faces;
        std::vector<Vec2f> bbs_vertex_uvs;

        for (auto cgal_face : faces) {
            TriFace bbs_face;
            std::size_t face_vid = 0;
            for (auto cgal_vtx : cgal_mesh.vertices_around_face(cgal_mesh.halfedge(cgal_face))) {
                if (!map_cgal_vtx_to_bbs_vtx.count(cgal_vtx)) {
                    map_cgal_vtx_to_bbs_vtx[cgal_vtx] = bbs_vertices.size();
                    const auto& cgal_point = cgal_mesh.point(cgal_vtx);
                    bbs_vertices.push_back(TriVertex(cgal_point.x(), cgal_point.y(), cgal_point.z()));
                    bbs_vertex_uvs.push_back(cgal_vertex_uvs[cgal_vtx]);
                }
                bbs_face[face_vid] = map_cgal_vtx_to_bbs_vtx[cgal_vtx];
                ++face_vid;
            }
            bbs_faces.push_back(std::move(bbs_face));
        }

        component_meshes.push_back(TriMesh(bbs_faces, bbs_vertices));
        component_vertex_uvs.push_back(std::move(bbs_vertex_uvs));
    }

    return true;
}

ColorDistancePalette::ColorDistancePalette(const std::vector<RGB>& colors)
{
    m_lab_colors.reserve(colors.size());
    for (const auto& color : colors) m_lab_colors.push_back(convert_rgb_to_lab(color));
}

bool ColorDistancePalette::nearest(const RGB& color, std::size_t& nearest_color_id) const
{
    if (m_lab_colors.empty()) return false;
    const auto lab = convert_rgb_to_lab(color);
    double min_dist = std::numeric_limits<double>::max();
    nearest_color_id = 0;
    for (std::size_t i = 0; i < m_lab_colors.size(); ++i) {
        const auto distance = ciede2000(m_lab_colors[i], lab);
        if (distance < min_dist) {
            min_dist = distance;
            nearest_color_id = i;
        }
    }
    return true;
}

bool calc_nearest_color_id(const std::vector<RGB>& colors, const RGB& color, std::size_t& nearest_color_id) {
    if (colors.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No color list provided";
        return false;
    }
    double min_dist = std::numeric_limits<double>::max();
    nearest_color_id = 0;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        double dist = calc_rgb_color_difference_by_ciede2000(colors[i], color);
        if (dist < min_dist) {
            min_dist = dist;
            nearest_color_id = i;
        }
    }
    return true;
}

bool mesh_cluster(const TriMesh& bbs_mesh, const std::vector<RGB>& cluster_centers, std::vector<RGB>& map_face_to_rgb,
                  std::vector<std::size_t>& map_face_to_cluster_id) {
    if (cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No cluster centers provided";
        return false;
    }

    CGALMesh mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, mesh);
    if (mesh.number_of_faces() != bbs_mesh.indices.size()) {
        BOOST_LOG_TRIVIAL(warning) << "CGAL mesh face count does not match BBS mesh face count";
        return false;
    }
    if (mesh.number_of_faces() != map_face_to_rgb.size()) {
        BOOST_LOG_TRIVIAL(warning) << "CGAL mesh face count does not match RGB count";
        return false;
    }

    if (cluster_centers.size() == 1) {
        std::fill(map_face_to_rgb.begin(), map_face_to_rgb.end(), cluster_centers[0]);
#ifdef DEBUG_FLAG
        BOOST_LOG_TRIVIAL(debug) << "input cluster centers' size is 1, so we set all face RGB as same with it and return.\n";
#endif
        return true;
    }

    std::vector<double> map_face_to_area(mesh.number_of_faces());
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mesh.number_of_faces()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            CGAL::SM_Face_index face(fid);
            map_face_to_area[fid] = std::max(PMP::face_area(face, mesh), EPSILON);
        }
    });

    // Step 1: Identify faces that definitely belong to a cluster center.
    // A face is definitively assigned when dist1 * absolute_difference_times < dist2 (nearest vs. second-nearest center).
    constexpr double absolute_difference_times = 1.5;
    // dE <= 1.0: imperceptible to the human eye, high-precision color matching
    // dE <= 2.0: slight difference, noticeable by experts; printing / image processing standard
    // dE <= 3.0: noticeable by ordinary observers; general quality control
    constexpr double difference_epsilon = 3.0;
    constexpr std::size_t invalid_cluster_id = std::numeric_limits<std::size_t>::max();
    map_face_to_cluster_id.resize(mesh.number_of_faces(), invalid_cluster_id);
    std::vector<std::vector<std::pair<double, std::size_t>>> map_face_to_dists(mesh.number_of_faces(),
                                                                               std::vector<std::pair<double, std::size_t>>(cluster_centers.size()));
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mesh.number_of_faces()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            CGAL::SM_Face_index face(fid);
            std::vector<std::pair<double, std::size_t>>& dist_and_cid_vec = map_face_to_dists[fid];
            for (std::size_t cluster_id = 0; cluster_id < cluster_centers.size(); ++cluster_id) {
                double dist = calc_rgb_color_difference_by_ciede2000(map_face_to_rgb[fid], cluster_centers[cluster_id]);
                //dist_and_cid_vec.emplace_back(dist, cluster_id);
                dist_and_cid_vec[cluster_id] = std::pair<double, std::size_t>{dist, cluster_id};
            }
            std::sort(dist_and_cid_vec.begin(), dist_and_cid_vec.end());
            if (dist_and_cid_vec[0].first < difference_epsilon || dist_and_cid_vec[0].first * absolute_difference_times < dist_and_cid_vec[1].first) {
                map_face_to_cluster_id[fid] = dist_and_cid_vec[0].second;
            }
        }
    });

    std::unordered_set<std::size_t> unclusted_fids;
    for (std::size_t fid = 0; fid < mesh.number_of_faces(); ++fid) {
        if (map_face_to_cluster_id[fid] == invalid_cluster_id) {
            unclusted_fids.insert(fid);
        }
    }

    auto convert_unclustered_to_clustered = [&](const std::unordered_set<std::size_t>& iter_clustered_fids) -> bool {
        if (iter_clustered_fids.empty()) {
            return false;
        }
        for (auto& fid : iter_clustered_fids) {
            unclusted_fids.erase(fid);
        }
        return true;
    };

    // Step 2: Flood. Use faces computed in the previous step as seeds and propagate outward.
    while (!unclusted_fids.empty()) {
        bool changed = false;
        std::unordered_set<std::size_t> iter_clustered_fids;
        // If an uncolored face has an adjacent color whose count exceeds the sum of all other colors, assign that color
        for (auto fid : unclusted_fids) {
            CGAL::SM_Face_index face(fid);
            std::size_t count = 0;
            std::unordered_map<std::size_t, std::size_t> map_cluster_id_to_count;
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {
                    continue;
                }
                ++count;
                ++map_cluster_id_to_count[map_face_to_cluster_id[adj_face]];
            }
            for (auto& [cluster_id, cnt] : map_cluster_id_to_count) {
                if (cluster_id != invalid_cluster_id && cnt * 2 > count) {
                    map_face_to_cluster_id[fid] = cluster_id;
                    iter_clustered_fids.insert(fid);
                    break;
                }
            }
        }
        changed = convert_unclustered_to_clustered(iter_clustered_fids) || changed;
        iter_clustered_fids.clear();

        // If a face's nearest cluster center (by color distance) happens to have an adjacent face, assign that color too
        for (auto fid : unclusted_fids) {
            CGAL::SM_Face_index face(fid);
            std::unordered_set<std::size_t> adj_cluster_ids;
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {
                    continue;
                }
                if (map_face_to_cluster_id[adj_face] == invalid_cluster_id) {
                    continue;
                }
                adj_cluster_ids.insert(map_face_to_cluster_id[adj_face]);
            }
            if (adj_cluster_ids.count(map_face_to_dists[fid].front().second)) {
                map_face_to_cluster_id[fid] = map_face_to_dists[fid].front().second;
                iter_clustered_fids.insert(fid);
            }
        }
        changed = convert_unclustered_to_clustered(iter_clustered_fids) || changed;
        iter_clustered_fids.clear();

        // If no face colors were modified in this iteration, stop
        if (!changed) {
            break;
        }
    }

    // Step 3: Handle remaining unclustered faces (run after the above operations complete)
    constexpr bool use_average_color = true;
    for (auto src_fid : std::vector<std::size_t>(unclusted_fids.begin(), unclusted_fids.end())) {
        if (!unclusted_fids.count(src_fid)) {
            continue;
        }
        // Compute connected unclustered faces
        std::queue<std::size_t> que;
        std::unordered_set<std::size_t> connected_unclustered_faces;
        que.push(src_fid);
        connected_unclustered_faces.insert(src_fid);
        double sum_r = 0, sum_g = 0, sum_b = 0;
        double sum_area = 0.0;
        std::unordered_map<std::size_t, double> map_cluster_id_to_adj_area;
        while (!que.empty()) {
            auto curr_fid = que.front();
            que.pop();
            sum_r += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][0];
            sum_g += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][1];
            sum_b += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][2];
            sum_area += map_face_to_area[curr_fid];
            CGAL::SM_Face_index curr_face(curr_fid);
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(curr_face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {  // Invalid face
                    continue;
                }
                if (map_face_to_cluster_id[adj_face] != invalid_cluster_id) {
                    map_cluster_id_to_adj_area[map_face_to_cluster_id[adj_face]] += map_face_to_area[adj_face];
                } else {
                    if (!connected_unclustered_faces.count(adj_face)) {  // Already clustered or already recorded
                        que.push(adj_face);
                        connected_unclustered_faces.insert(adj_face);
                    }
                }
            }
        }
        std::size_t matched_cluster_id = invalid_cluster_id;
        if (use_average_color) {
            // Use average color
            RGB average_color{static_cast<std::size_t>(sum_r / sum_area), static_cast<std::size_t>(sum_g / sum_area),
                              static_cast<std::size_t>(sum_b / sum_area)};
            double min_dist = std::numeric_limits<double>::max();
            for (auto& [cluster_id, area] : map_cluster_id_to_adj_area) {
                double dist = calc_rgb_color_difference_by_ciede2000(average_color, cluster_centers[cluster_id]);
                if (dist < min_dist) {
                    min_dist = dist;
                    matched_cluster_id = cluster_id;
                }
            }
        } else {
            // Use adjacent area
            double adj_max_area = 0.0;
            for (auto& [cluster_id, area] : map_cluster_id_to_adj_area) {
                if (area > adj_max_area) {
                    adj_max_area = area;
                    matched_cluster_id = cluster_id;
                }
            }
        }
        for (auto fid : connected_unclustered_faces) {
            map_face_to_cluster_id[fid] = matched_cluster_id;
            unclusted_fids.erase(fid);
        }
    }

    // Convert cluster center IDs to colors
    for (std::size_t fid = 0; fid < mesh.number_of_faces(); ++fid) {
        if (map_face_to_cluster_id[fid] == invalid_cluster_id) {
            map_face_to_rgb[fid] = cluster_centers[0];
            map_face_to_cluster_id[fid] = 0;  // Prevent out-of-bounds errors when using cluster_id later
        } else {
            map_face_to_rgb[fid] = cluster_centers[map_face_to_cluster_id[fid]];
        }
    }

    return true;
}

}  // namespace color_utils

}  // namespace tex2color
}  // namespace Slic3r
