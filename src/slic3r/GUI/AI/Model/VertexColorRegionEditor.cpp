#include "VertexColorRegionEditor.hpp"
#include "ModelArtifact.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <limits>
#include <memory_resource>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace Slic3r::AI {
namespace {

constexpr float PI = 3.14159265358979323846f;
constexpr size_t PICK_BVH_LEAF_SIZE = 8;
constexpr size_t PREPARATION_BATCH_SIZE = 4096;

struct RegionPreparationCanceled {};

void check_preparation_canceled(const std::function<bool()>& canceled)
{
    if (canceled && canceled()) throw RegionPreparationCanceled {};
}

struct PositionCell
{
    int64_t x {0};
    int64_t y {0};
    int64_t z {0};

    bool operator==(const PositionCell& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PositionCellHash
{
    size_t operator()(const PositionCell& cell) const
    {
        size_t seed = std::hash<int64_t> {}(cell.x);
        seed ^= std::hash<int64_t> {}(cell.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<int64_t> {}(cell.z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct IndexedEdgeOwner
{
    uint32_t first_vertex {0};
    uint32_t second_vertex {0};
    uint32_t first_face {0};
    uint32_t use_count {1};
};

uint64_t edge_key(uint32_t first, uint32_t second)
{
    if (first > second)
        std::swap(first, second);
    return (uint64_t(first) << 32) | uint64_t(second);
}

PositionCell position_cell(const Vec3f& position, double cell_size)
{
    return {
        int64_t(std::floor(double(position.x()) / cell_size)),
        int64_t(std::floor(double(position.y()) / cell_size)),
        int64_t(std::floor(double(position.z()) / cell_size))
    };
}

float color_distance_squared(const RGBA& left, const RGBA& right)
{
    const float red = left[0] - right[0];
    const float green = left[1] - right[1];
    const float blue = left[2] - right[2];
    return red * red + green * green + blue * blue;
}

bool ray_triangle_intersection(const Vec3d& origin, const Vec3d& direction,
                               const Vec3f& a_float, const Vec3f& b_float, const Vec3f& c_float,
                               double& distance)
{
    constexpr double epsilon = 1e-9;
    const Vec3d a = a_float.cast<double>();
    const Vec3d b = b_float.cast<double>();
    const Vec3d c = c_float.cast<double>();
    const Vec3d edge_a = b - a;
    const Vec3d edge_b = c - a;
    const Vec3d p = direction.cross(edge_b);
    const double determinant = edge_a.dot(p);
    if (std::abs(determinant) < epsilon)
        return false;
    const double inverse = 1.0 / determinant;
    const Vec3d offset = origin - a;
    const double u = offset.dot(p) * inverse;
    if (u < 0.0 || u > 1.0)
        return false;
    const Vec3d q = offset.cross(edge_a);
    const double v = direction.dot(q) * inverse;
    if (v < 0.0 || u + v > 1.0)
        return false;
    distance = edge_b.dot(q) * inverse;
    return distance > epsilon;
}

bool ray_box_intersection(const Vec3d& origin, const Vec3d& direction,
                          const Vec3f& minimum_float, const Vec3f& maximum_float,
                          double maximum_distance, double& entry_distance)
{
    constexpr double direction_epsilon = 1e-12;
    constexpr double bounds_epsilon = 1e-7;
    const Vec3d minimum = minimum_float.cast<double>().array() - bounds_epsilon;
    const Vec3d maximum = maximum_float.cast<double>().array() + bounds_epsilon;
    double near_distance = 0.0;
    double far_distance = maximum_distance;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) <= direction_epsilon) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                return false;
            continue;
        }
        double first = (minimum[axis] - origin[axis]) / direction[axis];
        double second = (maximum[axis] - origin[axis]) / direction[axis];
        if (first > second)
            std::swap(first, second);
        near_distance = std::max(near_distance, first);
        far_distance = std::min(far_distance, second);
        if (near_distance > far_distance)
            return false;
    }
    entry_distance = near_distance;
    return far_distance > 0.0;
}

} // namespace

bool VertexColorRegionEditor::initialize(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
                                         std::string& error)
{
    return initialize(std::move(mesh), std::move(vertex_colors), error, {});
}

bool VertexColorRegionEditor::initialize(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
                                         std::string& error, const std::function<bool()>& canceled)
{
    return initialize_impl(std::move(mesh), std::move(vertex_colors), error, canceled, true);
}

bool VertexColorRegionEditor::initialize_for_picking(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
    std::string& error, const std::function<bool()>& canceled)
{
    return initialize_impl(std::move(mesh), std::move(vertex_colors), error, canceled, false);
}

bool VertexColorRegionEditor::initialize_impl(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
    std::string& error, const std::function<bool()>& canceled, bool regions) try
{
    const auto started = std::chrono::steady_clock::now();
    clear();
    check_preparation_canceled(canceled);
    if (mesh.indices.empty() || mesh.vertices.empty()) {
        error = "The OBJ contains no selectable triangles.";
        return false;
    }
    if (vertex_colors.size() != mesh.vertices.size()) {
        error = "Local recoloring requires OBJ vertex colors.";
        return false;
    }

    m_mesh = std::move(mesh);
    m_vertex_colors = std::move(vertex_colors);
    m_selected_faces.assign(m_mesh.indices.size(), 0);

    Vec3f minimum = m_mesh.vertices.front();
    Vec3f maximum = minimum;
    for (size_t batch_begin = 0; batch_begin < m_mesh.vertices.size(); batch_begin += PREPARATION_BATCH_SIZE) {
        check_preparation_canceled(canceled);
        const size_t batch_end = std::min(m_mesh.vertices.size(), batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t vertex_index = batch_begin; vertex_index < batch_end; ++vertex_index) {
            const Vec3f& vertex = m_mesh.vertices[vertex_index];
            minimum = minimum.cwiseMin(vertex);
            maximum = maximum.cwiseMax(vertex);
        }
    }
    m_mesh_diagonal = (maximum - minimum).norm();

    const auto setup_done = std::chrono::steady_clock::now();
    double indexed_ms = 0, weld_ms = 0, adjacency_ms = 0;
    if (regions) {
        auto topology = prepare_region_topology_impl(true, canceled);
        indexed_ms = topology->indexed_ms;weld_ms = topology->weld_ms;adjacency_ms = topology->adjacency_ms;
        m_face_centers = std::move(topology->centers);
        if (!install_region_topology(std::move(topology)))
            throw std::logic_error("Region topology does not belong to this model.");
    } else {
        m_face_centers.resize(m_mesh.indices.size());
        for (size_t begin = 0; begin < m_mesh.indices.size(); begin += PREPARATION_BATCH_SIZE) {
            check_preparation_canceled(canceled);
            const size_t end = std::min(m_mesh.indices.size(), begin + PREPARATION_BATCH_SIZE);
            for (size_t face_index = begin; face_index < end; ++face_index) {
                const auto& face = m_mesh.indices[face_index];
                m_face_centers[face_index] = (m_mesh.vertices[face[0]] + m_mesh.vertices[face[1]] + m_mesh.vertices[face[2]]) / 3.0f;
            }
        }
        indexed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - setup_done).count();
    }
    const auto adjacency_done = std::chrono::steady_clock::now();
    m_pick_face_order.resize(m_mesh.indices.size());
    std::iota(m_pick_face_order.begin(), m_pick_face_order.end(), uint32_t(0));
    m_pick_nodes.reserve(std::max<size_t>(1, m_mesh.indices.size() / 2));
    build_pick_bvh(0, m_pick_face_order.size(), canceled);
    check_preparation_canceled(canceled);
    const auto bvh_done = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    // Argument copies and destruction of temporary maps are outside these stages.
    BOOST_LOG_TRIVIAL(info) << "AI region initialize: faces=" << m_mesh.indices.size()
        << ", setup_ms=" << milliseconds(started, setup_done)
        << ", indexed_ms=" << indexed_ms
        << ", weld_ms=" << weld_ms
        << ", adjacency_ms=" << adjacency_ms
        << ", bvh_ms=" << milliseconds(adjacency_done, bvh_done)
        << ", region_topology=" << region_selection_ready();
    return true;
}
catch (const RegionPreparationCanceled&) {
    clear();
    error = "Local selection preparation canceled.";
    return false;
}
catch (...) {
    clear();
    throw;
}

std::unique_ptr<VertexColorRegionEditor::RegionTopology>
VertexColorRegionEditor::prepare_region_topology_impl(bool centers,
    const std::function<bool()>& canceled) const
{
    check_preparation_canceled(canceled);
    const auto started = std::chrono::steady_clock::now();
    auto topology = std::make_unique<RegionTopology>();
    topology->source = this;topology->generation = m_geometry_generation;
    topology->normals.resize(m_mesh.indices.size());
    topology->neighbors.resize(m_mesh.indices.size());
    if (centers) topology->centers.resize(m_mesh.indices.size());
    // Temporary topology nodes share an arena and are released together after
    // initialization. No editor state retains an allocator or arena reference.
    std::pmr::monotonic_buffer_resource topology_memory;
    std::pmr::unordered_map<uint64_t, IndexedEdgeOwner> indexed_edges{&topology_memory};
    bool independent_corners = m_mesh.indices.size() <= size_t(std::numeric_limits<int32_t>::max() / 3) &&
        m_mesh.indices.size() <= m_mesh.vertices.size() / 3;
    for (size_t batch_begin = 0; independent_corners && batch_begin < m_mesh.indices.size(); batch_begin += PREPARATION_BATCH_SIZE) {
        check_preparation_canceled(canceled);
        const size_t batch_end = std::min(m_mesh.indices.size(), batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t face = batch_begin; independent_corners && face < batch_end; ++face) {
            const auto& indices = m_mesh.indices[face];
            independent_corners = size_t(indices[0]) == face * 3 && size_t(indices[1]) == face * 3 + 1 &&
                size_t(indices[2]) == face * 3 + 2;
        }
    }
    // Independent face corners cannot share indexed edges. Their geometric
    // seams still follow the same canonical-point and boundary grouping rules.
    if (!independent_corners)
        indexed_edges.reserve(m_mesh.indices.size() * 3);
    for (size_t batch_begin = 0; batch_begin < m_mesh.indices.size(); batch_begin += PREPARATION_BATCH_SIZE) {
        check_preparation_canceled(canceled);
        const size_t batch_end = std::min(m_mesh.indices.size(), batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t face_index = batch_begin; face_index < batch_end; ++face_index) {
            const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
            const Vec3f& a = m_mesh.vertices[face[0]];
            const Vec3f& b = m_mesh.vertices[face[1]];
            const Vec3f& c = m_mesh.vertices[face[2]];
            Vec3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 1e-12f)
                normal.normalize();
            else
                normal = Vec3f::UnitZ();
            topology->normals[face_index] = normal;
            if (centers) topology->centers[face_index] = (a + b + c) / 3.0f;

            if (independent_corners)
                continue;

            const std::array<std::pair<uint32_t, uint32_t>, 3> edges {{
                {uint32_t(face[0]), uint32_t(face[1])},
                {uint32_t(face[1]), uint32_t(face[2])},
                {uint32_t(face[2]), uint32_t(face[0])}
            }};
            for (const auto& edge : edges) {
                const uint64_t key = edge_key(edge.first, edge.second);
                const auto [owner, inserted] = indexed_edges.emplace(
                    key, IndexedEdgeOwner {edge.first, edge.second, uint32_t(face_index), 1});
                if (!inserted) {
                    ++owner->second.use_count;
                    if (owner->second.first_face != face_index) {
                        topology->neighbors[face_index].push_back(owner->second.first_face);
                        topology->neighbors[owner->second.first_face].push_back(uint32_t(face_index));
                    }
                }
            }
        }
    }

    const auto indexed_done = std::chrono::steady_clock::now();
    // OBJ exporters commonly duplicate vertices along UV or material seams. Weld only
    // boundary-edge endpoints for selection adjacency; the mesh and its indices remain unchanged.
    const double position_tolerance = std::clamp(double(m_mesh_diagonal) * 1e-7, 1e-7, 1e-4);
    const double tolerance_squared = position_tolerance * position_tolerance;
    constexpr uint32_t no_canonical = std::numeric_limits<uint32_t>::max();
    struct CanonicalEntry {
        uint32_t vertex;
        uint32_t next;
    };
    // Cell heads refer to one contiguous list, avoiding a tiny vector allocation
    // for each occupied cell. Entries own only indices, never mesh references.
    // Flat storage avoids per-cell nodes and releases old arrays on growth.
    // Cell iterators are used only until the next insertion.
    boost::unordered_flat_map<PositionCell, uint32_t, PositionCellHash> vertices_by_cell;
    vertices_by_cell.reserve(m_mesh.vertices.size() / (independent_corners ? 3 : 1));
    std::vector<CanonicalEntry> canonical_entries;
    canonical_entries.reserve(m_mesh.vertices.size() / (independent_corners ? 3 : 1));
    std::vector<uint32_t> canonical_vertices(m_mesh.vertices.size());
    // The extra center-cell lookup pays off for very large meshes with many
    // exactly repeated positions; smaller meshes keep the single-pass search.
    const bool try_exact_canonical = m_mesh.vertices.size() >= 4'000'000;
    for (size_t batch_begin = 0; batch_begin < m_mesh.vertices.size(); batch_begin += PREPARATION_BATCH_SIZE) {
        check_preparation_canceled(canceled);
        const size_t batch_end = std::min(m_mesh.vertices.size(), batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t vertex_index = batch_begin; vertex_index < batch_end; ++vertex_index) {
            const Vec3f& vertex = m_mesh.vertices[vertex_index];
            const PositionCell cell = position_cell(vertex, position_tolerance);
            uint32_t canonical = no_canonical;
            bool exact_canonical = false;
            if (try_exact_canonical) {
                const auto same_cell = vertices_by_cell.find(cell);
                if (same_cell != vertices_by_cell.end()) {
                    for (uint32_t entry = same_cell->second; entry != no_canonical; entry = canonical_entries[entry].next) {
                        const uint32_t candidate = canonical_entries[entry].vertex;
                        const Vec3f& existing = m_mesh.vertices[candidate];
                        if (existing.x() == vertex.x() && existing.y() == vertex.y() && existing.z() == vertex.z()) {
                            canonical = candidate;
                            exact_canonical = true;
                            break;
                        }
                        if ((existing.cast<double>() - vertex.cast<double>()).squaredNorm() <= tolerance_squared)
                            canonical = std::min(canonical, candidate);
                    }
                }
            }
            // An identical canonical point was inserted only after all older nearby
            // canonicals had been ruled out. Later insertions cannot have a smaller index.
            if (!exact_canonical) {
                for (int64_t dx = -1; dx <= 1; ++dx) {
                    for (int64_t dy = -1; dy <= 1; ++dy) {
                        for (int64_t dz = -1; dz <= 1; ++dz) {
                            if (try_exact_canonical && dx == 0 && dy == 0 && dz == 0)
                                continue;
                            const auto candidates = vertices_by_cell.find({cell.x + dx, cell.y + dy, cell.z + dz});
                            if (candidates == vertices_by_cell.end())
                                continue;
                            for (uint32_t entry = candidates->second; entry != no_canonical; entry = canonical_entries[entry].next) {
                                const uint32_t candidate = canonical_entries[entry].vertex;
                                if ((m_mesh.vertices[candidate].cast<double>() - vertex.cast<double>()).squaredNorm() <=
                                    tolerance_squared)
                                    canonical = std::min(canonical, candidate);
                            }
                        }
                    }
                }
            }
            if (canonical == no_canonical) {
                canonical = uint32_t(vertex_index);
                const auto [head, inserted] = vertices_by_cell.try_emplace(cell, no_canonical);
                canonical_entries.push_back({canonical, head->second});
                head->second = uint32_t(canonical_entries.size() - 1);
            }
            canonical_vertices[vertex_index] = canonical;
        }
    }

    const auto weld_done = std::chrono::steady_clock::now();
    struct GeometricBoundaryEdge {
        uint64_t key;
        uint32_t face;
    };
    std::vector<GeometricBoundaryEdge> boundary_edges;
    const auto append_boundary = [&](uint32_t first_vertex, uint32_t second_vertex, uint32_t face) {
        const uint32_t first = canonical_vertices[first_vertex];
        const uint32_t second = canonical_vertices[second_vertex];
        if (first != second)
            boundary_edges.push_back({edge_key(first, second), face});
    };
    if (independent_corners) {
        boundary_edges.reserve(m_mesh.indices.size() * 3);
        for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
            if ((face_index & 4095) == 0) check_preparation_canceled(canceled);
            const auto& face = m_mesh.indices[face_index];
            for (size_t side = 0; side < 3; ++side)
                append_boundary(uint32_t(face[side]), uint32_t(face[(side + 1) % 3]), uint32_t(face_index));
        }
    } else {
        size_t edge_work = 0;
        for (const auto& item : indexed_edges) {
            if ((edge_work++ & 4095) == 0) check_preparation_canceled(canceled);
            const IndexedEdgeOwner& edge = item.second;
            if (edge.use_count == 1)
                append_boundary(edge.first_vertex, edge.second_vertex, edge.first_face);
        }
    }
    // Keep the original comparator and ordering cost. Cancellation is cooperative:
    // one standard-library sort must finish before its following checkpoint.
    check_preparation_canceled(canceled);
    std::sort(boundary_edges.begin(), boundary_edges.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    check_preparation_canceled(canceled);
    size_t boundary_work = 0;
    for (size_t begin = 0; begin < boundary_edges.size();) {
        if ((boundary_work++ & 4095) == 0) check_preparation_canceled(canceled);
        size_t end = begin + 1;
        while (end < boundary_edges.size() && boundary_edges[end].key == boundary_edges[begin].key) {
            if ((end & 4095) == 0) check_preparation_canceled(canceled);
            ++end;
        }
        if (end - begin == 2 && boundary_edges[begin].face != boundary_edges[begin + 1].face) {
            const uint32_t first = boundary_edges[begin].face;
            const uint32_t second = boundary_edges[begin + 1].face;
            // Independent corners have at most one neighbor per side. Allocate
            // once on the first link, without allocating for disconnected faces.
            if (independent_corners) {
                if (topology->neighbors[first].empty())
                    topology->neighbors[first].reserve(3);
                if (topology->neighbors[second].empty())
                    topology->neighbors[second].reserve(3);
            }
            topology->neighbors[first].push_back(second);
            topology->neighbors[second].push_back(first);
        }
        begin = end;
    }
    for (size_t batch_begin = 0; batch_begin < topology->neighbors.size(); batch_begin += PREPARATION_BATCH_SIZE) {
        check_preparation_canceled(canceled);
        const size_t batch_end = std::min(topology->neighbors.size(), batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t face = batch_begin; face < batch_end; ++face) {
            std::vector<uint32_t>& neighbors = topology->neighbors[face];
            std::sort(neighbors.begin(), neighbors.end());
            neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
        }
    }

    check_preparation_canceled(canceled);
    const auto adjacency_done = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    topology->indexed_ms = milliseconds(started, indexed_done);
    topology->weld_ms = milliseconds(indexed_done, weld_done);
    topology->adjacency_ms = milliseconds(weld_done, adjacency_done);
    return topology;
}

std::unique_ptr<VertexColorRegionEditor::RegionTopology>
VertexColorRegionEditor::prepare_region_topology(std::string& error,
    const std::function<bool()>& canceled) const try
{
    if (!ready()) {error = "The model contains no selectable triangles.";return {};}
    return prepare_region_topology_impl(false, canceled);
}
catch (const RegionPreparationCanceled&) {
    error = "Local selection preparation canceled.";
    return {};
}

bool VertexColorRegionEditor::install_region_topology(std::unique_ptr<RegionTopology> topology)
{
    if (!topology || !ready() || region_selection_ready() || topology->source != this ||
        topology->generation != m_geometry_generation ||
        topology->neighbors.size() != m_mesh.indices.size() || topology->normals.size() != m_mesh.indices.size())
        return false;
    m_face_normals = std::move(topology->normals);
    m_face_neighbors = std::move(topology->neighbors);
    return true;
}

uint32_t VertexColorRegionEditor::build_pick_bvh(size_t begin, size_t end, const std::function<bool()>& canceled)
{
    if ((m_pick_nodes.size() & (PREPARATION_BATCH_SIZE - 1)) == 0)
        check_preparation_canceled(canceled);
    const size_t count = end - begin;
    PickBvhNode node;
    node.minimum = Vec3f::Constant(std::numeric_limits<float>::infinity());
    node.maximum = Vec3f::Constant(-std::numeric_limits<float>::infinity());
    if (count <= PICK_BVH_LEAF_SIZE) {
        for (size_t item = begin; item < end; ++item) {
            const stl_triangle_vertex_indices& face = m_mesh.indices[m_pick_face_order[item]];
            for (size_t corner = 0; corner < 3; ++corner) {
                const Vec3f& vertex = m_mesh.vertices[face[corner]];
                node.minimum = node.minimum.cwiseMin(vertex);
                node.maximum = node.maximum.cwiseMax(vertex);
            }
        }
        const uint32_t node_index = uint32_t(m_pick_nodes.size());
        m_pick_nodes.emplace_back(node);
        m_pick_nodes[node_index].first = uint32_t(begin);
        m_pick_nodes[node_index].count = uint32_t(count);
        return node_index;
    }

    Vec3f center_minimum = node.minimum;
    Vec3f center_maximum = node.maximum;
    for (size_t batch_begin = begin; batch_begin < end; batch_begin += PREPARATION_BATCH_SIZE) {
        if (count >= PREPARATION_BATCH_SIZE) check_preparation_canceled(canceled);
        const size_t batch_end = std::min(end, batch_begin + PREPARATION_BATCH_SIZE);
        for (size_t item = batch_begin; item < batch_end; ++item) {
            const uint32_t face_index = m_pick_face_order[item];
            center_minimum = center_minimum.cwiseMin(m_face_centers[face_index]);
            center_maximum = center_maximum.cwiseMax(m_face_centers[face_index]);
        }
    }

    const uint32_t node_index = uint32_t(m_pick_nodes.size());
    m_pick_nodes.emplace_back(node);
    Eigen::Index split_axis = 0;
    (center_maximum - center_minimum).maxCoeff(&split_axis);
    const size_t middle = begin + count / 2;
    const auto face_less = [this, split_axis](uint32_t left, uint32_t right) {
        const float left_value = m_face_centers[left][split_axis];
        const float right_value = m_face_centers[right][split_axis];
        return left_value == right_value ? left < right : left_value < right_value;
    };
    if (count >= PREPARATION_BATCH_SIZE) check_preparation_canceled(canceled);
    std::nth_element(m_pick_face_order.begin() + begin,
                     m_pick_face_order.begin() + middle,
                     m_pick_face_order.begin() + end, face_less);
    const uint32_t left = build_pick_bvh(begin, middle, canceled);
    const uint32_t right = build_pick_bvh(middle, end, canceled);
    // Each triangle's bounds are visited once in a leaf; parent boxes are
    // assembled from their children without scanning the same vertices again.
    m_pick_nodes[node_index].minimum = m_pick_nodes[left].minimum.cwiseMin(m_pick_nodes[right].minimum);
    m_pick_nodes[node_index].maximum = m_pick_nodes[left].maximum.cwiseMax(m_pick_nodes[right].maximum);
    m_pick_nodes[node_index].left = left;
    m_pick_nodes[node_index].right = right;
    return node_index;
}

void VertexColorRegionEditor::clear()
{
    ++m_geometry_generation;
    m_mesh = {};
    m_vertex_colors.clear();
    m_face_color_overrides.clear();
    m_face_normals.clear();
    m_face_centers.clear();
    m_face_neighbors.clear();
    m_pick_face_order.clear();
    m_pick_nodes.clear();
    m_selected_faces.clear();
    m_selected_face_count = 0;
    m_mesh_diagonal = 0.0f;
}

std::optional<size_t> VertexColorRegionEditor::pick_face(const Vec3d& ray_origin,
                                                         const Vec3d& ray_direction) const
{
    if (!ready() || ray_direction.squaredNorm() < 1e-12)
        return std::nullopt;
    const Vec3d direction = ray_direction.normalized();
    double nearest = std::numeric_limits<double>::infinity();
    std::optional<size_t> result;
    if (m_pick_nodes.empty())
        return result;

    struct PendingNode
    {
        uint32_t index;
        double entry_distance;
    };
    double root_distance = 0.0;
    if (!ray_box_intersection(ray_origin, direction, m_pick_nodes.front().minimum,
                              m_pick_nodes.front().maximum, nearest, root_distance))
        return result;
    std::vector<PendingNode> pending {{0, root_distance}};
    pending.reserve(64);
    while (!pending.empty()) {
        const PendingNode current = pending.back();
        pending.pop_back();
        if (current.entry_distance > nearest)
            continue;
        const PickBvhNode& node = m_pick_nodes[current.index];
        if (node.is_leaf()) {
            for (uint32_t item = node.first; item < node.first + node.count; ++item) {
                const size_t face_index = m_pick_face_order[item];
                const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
                double distance = 0.0;
                if (!ray_triangle_intersection(ray_origin, direction,
                                               m_mesh.vertices[face[0]], m_mesh.vertices[face[1]],
                                               m_mesh.vertices[face[2]], distance))
                    continue;
                if (distance < nearest - 1e-9 ||
                    (std::abs(distance - nearest) <= 1e-9 && (!result || face_index < *result))) {
                    nearest = distance;
                    result = face_index;
                }
            }
            continue;
        }

        const PickBvhNode& left = m_pick_nodes[node.left];
        const PickBvhNode& right = m_pick_nodes[node.right];
        double left_distance = 0.0;
        double right_distance = 0.0;
        const bool hit_left = ray_box_intersection(
            ray_origin, direction, left.minimum, left.maximum, nearest, left_distance);
        const bool hit_right = ray_box_intersection(
            ray_origin, direction, right.minimum, right.maximum, nearest, right_distance);
        if (hit_left && hit_right) {
            if (left_distance <= right_distance) {
                pending.push_back({node.right, right_distance});
                pending.push_back({node.left, left_distance});
            } else {
                pending.push_back({node.left, left_distance});
                pending.push_back({node.right, right_distance});
            }
        } else if (hit_left) {
            pending.push_back({node.left, left_distance});
        } else if (hit_right) {
            pending.push_back({node.right, right_distance});
        }
    }
    return result;
}

RGBA VertexColorRegionEditor::corner_color(size_t face_index, size_t corner) const
{
    const auto edited = m_face_color_overrides.find(face_index);
    return edited == m_face_color_overrides.end()
        ? m_vertex_colors[m_mesh.indices[face_index][corner]] : edited->second;
}

RGBA VertexColorRegionEditor::face_color(size_t face_index) const
{
    const auto edited = m_face_color_overrides.find(face_index);
    if (edited != m_face_color_overrides.end())
        return edited->second;
    RGBA result {0.0f, 0.0f, 0.0f, 1.0f};
    const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
    for (size_t channel = 0; channel < 4; ++channel)
        result[channel] = (m_vertex_colors[face[0]][channel] + m_vertex_colors[face[1]][channel] +
                           m_vertex_colors[face[2]][channel]) / 3.0f;
    return result;
}

std::vector<size_t> VertexColorRegionEditor::smart_region(
    size_t seed_face, const RegionSelectionSettings& settings) const
{
    std::vector<size_t> result;
    if (seed_face >= m_mesh.indices.size())
        return result;
    const RGBA seed_color = face_color(seed_face);
    const float maximum_color_distance = settings.color_distance * settings.color_distance;
    const float minimum_normal_dot = std::cos(settings.normal_angle_degrees * PI / 180.0f);
    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::queue<size_t> pending;
    visited[seed_face] = 1;
    pending.push(seed_face);
    while (!pending.empty()) {
        const size_t current = pending.front();
        pending.pop();
        result.push_back(current);
        for (uint32_t neighbor : m_face_neighbors[current]) {
            if (visited[neighbor])
                continue;
            if (color_distance_squared(face_color(neighbor), seed_color) > maximum_color_distance)
                continue;
            if (m_face_normals[current].dot(m_face_normals[neighbor]) < minimum_normal_dot)
                continue;
            visited[neighbor] = 1;
            pending.push(neighbor);
        }
    }
    return result;
}

std::vector<size_t> VertexColorRegionEditor::local_patch(
    size_t seed_face, const RegionSelectionSettings& settings) const
{
    std::vector<size_t> result;
    if (seed_face >= m_mesh.indices.size())
        return result;
    const Vec3f seed_center = m_face_centers[seed_face];
    const float radius = std::max(1e-5f, m_mesh_diagonal * settings.local_radius_ratio);
    const float radius_squared = radius * radius;
    const float minimum_normal_dot = std::cos(std::min(88.0f, settings.normal_angle_degrees + 15.0f) *
                                               PI / 180.0f);
    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::queue<size_t> pending;
    visited[seed_face] = 1;
    pending.push(seed_face);
    while (!pending.empty()) {
        const size_t current = pending.front();
        pending.pop();
        result.push_back(current);
        for (uint32_t neighbor : m_face_neighbors[current]) {
            if (visited[neighbor])
                continue;
            if ((m_face_centers[neighbor] - seed_center).squaredNorm() > radius_squared)
                continue;
            if (m_face_normals[current].dot(m_face_normals[neighbor]) < minimum_normal_dot)
                continue;
            visited[neighbor] = 1;
            pending.push(neighbor);
        }
    }
    return result;
}

size_t VertexColorRegionEditor::update_selection(size_t seed_face, RegionSelectionOperation operation,
                                                 const RegionSelectionSettings& settings)
{
    if (!region_selection_ready() || seed_face >= m_mesh.indices.size())
        return m_selected_face_count;
    const std::vector<size_t> region = (operation == RegionSelectionOperation::Replace || operation == RegionSelectionOperation::AddSimilar)
        ? smart_region(seed_face, settings) : local_patch(seed_face, settings);
    if (operation == RegionSelectionOperation::Replace) {
        std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
        m_selected_face_count = 0;
    }
    for (size_t face : region) {
        const bool selected = operation != RegionSelectionOperation::Remove;
        if (bool(m_selected_faces[face]) == selected)
            continue;
        m_selected_faces[face] = selected ? 1 : 0;
        if (selected)
            ++m_selected_face_count;
        else
            --m_selected_face_count;
    }
    return m_selected_face_count;
}

size_t VertexColorRegionEditor::select_faces(const std::vector<size_t>& face_indices)
{
    if (!ready())
        return 0;

    std::vector<uint8_t> selected(m_mesh.indices.size(), 0);
    size_t selected_count = 0;
    for (size_t face_index : face_indices) {
        if (face_index >= selected.size() || selected[face_index])
            continue;
        selected[face_index] = 1;
        ++selected_count;
    }
    // Invalid or empty evidence is non-destructive, matching the other automatic
    // localization helpers and preserving an in-progress manual selection.
    if (selected_count == 0)
        return 0;
    m_selected_faces = std::move(selected);
    m_selected_face_count = selected_count;
    return selected_count;
}

size_t VertexColorRegionEditor::select_palette_material(const std::vector<RGBA>& palette,
                                                        size_t palette_index)
{
    if (!ready() || palette.empty() || palette_index >= palette.size())
        return m_selected_face_count;

    std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
    m_selected_face_count = 0;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const RGBA color = face_color(face_index);
        size_t nearest_index = 0;
        float nearest_distance = color_distance_squared(color, palette.front());
        for (size_t candidate = 1; candidate < palette.size(); ++candidate) {
            const float distance = color_distance_squared(color, palette[candidate]);
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest_index = candidate;
            }
        }
        if (nearest_index != palette_index)
            continue;
        m_selected_faces[face_index] = 1;
        ++m_selected_face_count;
    }
    return m_selected_face_count;
}

size_t VertexColorRegionEditor::select_elevated_overhang_regions(
    const OverhangRegionSettings& settings)
{
    if (!region_selection_ready())
        return 0;

    const float ground_band = std::max(0.0f, settings.ground_band_mm);
    const float surface_angle = std::clamp(settings.maximum_surface_angle_degrees, 0.0f, 89.9f);
    const float maximum_normal_z = -std::cos(surface_angle * PI / 180.0f);
    const double minimum_region_area = std::max(0.0f, settings.minimum_region_area_mm2);
    const double minimum_region_ratio = std::max(0.0f, settings.minimum_region_area_ratio);
    const float ground_limit = std::min_element(
        m_mesh.vertices.begin(), m_mesh.vertices.end(),
        [](const Vec3f& left, const Vec3f& right) { return left.z() < right.z(); })->z() + ground_band;

    std::vector<double> face_areas(m_mesh.indices.size(), 0.0);
    std::vector<uint8_t> candidates(m_mesh.indices.size(), 0);
    double surface_area = 0.0;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        const Vec3f& a = m_mesh.vertices[face[0]];
        const Vec3f& b = m_mesh.vertices[face[1]];
        const Vec3f& c = m_mesh.vertices[face[2]];
        const double area = 0.5 * double((b - a).cross(c - a).norm());
        face_areas[face_index] = area;
        surface_area += area;
        if (area > 1e-12 && m_face_normals[face_index].z() < maximum_normal_z &&
            std::min({a.z(), b.z(), c.z()}) > ground_limit)
            candidates[face_index] = 1;
    }

    std::vector<uint8_t> visited(m_mesh.indices.size(), 0);
    std::vector<uint8_t> localized(m_mesh.indices.size(), 0);
    size_t localized_count = 0;
    for (size_t seed = 0; seed < candidates.size(); ++seed) {
        if (!candidates[seed] || visited[seed])
            continue;
        visited[seed] = 1;
        std::vector<size_t> pending {seed};
        std::vector<size_t> region;
        double region_area = 0.0;
        while (!pending.empty()) {
            const size_t face_index = pending.back();
            pending.pop_back();
            region.push_back(face_index);
            region_area += face_areas[face_index];
            for (uint32_t neighbor : m_face_neighbors[face_index]) {
                if (!candidates[neighbor] || visited[neighbor])
                    continue;
                visited[neighbor] = 1;
                pending.push_back(neighbor);
            }
        }
        const double region_ratio = surface_area > 0.0 ? region_area / surface_area : 0.0;
        if (region_area < minimum_region_area || region_ratio < minimum_region_ratio)
            continue;
        for (size_t face_index : region) {
            localized[face_index] = 1;
            ++localized_count;
        }
    }

    // A failed localization is non-destructive: an existing manual/material selection
    // remains available for the user to inspect or edit.
    if (localized_count == 0)
        return 0;
    m_selected_faces = std::move(localized);
    m_selected_face_count = localized_count;
    return localized_count;
}

void VertexColorRegionEditor::clear_selection()
{
    std::fill(m_selected_faces.begin(), m_selected_faces.end(), uint8_t(0));
    m_selected_face_count = 0;
}

bool VertexColorRegionEditor::restore_selection(const std::vector<uint8_t>& selected_faces)
{
    if (!ready() || selected_faces.size() != m_selected_faces.size())
        return false;
    m_selected_face_count = 0;
    for (size_t face_index = 0; face_index < selected_faces.size(); ++face_index) {
        m_selected_faces[face_index] = selected_faces[face_index] == 0 ? 0 : 1;
        m_selected_face_count += m_selected_faces[face_index];
    }
    return true;
}

bool VertexColorRegionEditor::apply_color(const RGBA& color)
{
    if (!ready() || m_selected_face_count == 0)
        return false;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        if (!m_selected_faces[face_index])
            continue;
        const auto& face = m_mesh.indices[face_index];
        if (m_vertex_colors[face[0]] == color && m_vertex_colors[face[1]] == color &&
            m_vertex_colors[face[2]] == color)
            m_face_color_overrides.erase(face_index);
        else
            m_face_color_overrides[face_index] = color;
    }
    return true;
}

bool VertexColorRegionEditor::apply_color_to_obj_copy(const RGBA& color,
                                                      const boost::filesystem::path& source,
                                                      const boost::filesystem::path& destination,
                                                      std::string& error)
{
    const auto original_overrides = m_face_color_overrides;
    if (!apply_color(color)) {
        error = "No model region is selected.";
        return false;
    }
    const bool written = write_obj_copy(source, destination, error);
    // Export is a candidate copy. Keep the source editor intact so cached
    // before/after views and undo followed by another edit use original colors.
    m_face_color_overrides = original_overrides;
    return written;
}

void VertexColorRegionEditor::build_color_mesh(indexed_triangle_set& mesh,
                                               std::vector<RGBA>& colors) const
{
    mesh = m_mesh;
    colors = m_vertex_colors;
    std::vector<uint8_t> assigned(m_mesh.vertices.size(), 0);
    std::unordered_map<int, std::vector<int>> variants;
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        for (size_t corner = 0; corner < 3; ++corner) {
            const int source_vertex = m_mesh.indices[face_index][corner];
            const RGBA color = corner_color(face_index, corner);
            if (!assigned[source_vertex]) {
                colors[source_vertex] = color;
                assigned[source_vertex] = 1;
            }
            if (colors[source_vertex] == color)
                continue;
            auto& alternatives = variants[source_vertex];
            auto existing = std::find_if(alternatives.begin(), alternatives.end(),
                [&colors, &color](int index) { return colors[index] == color; });
            int output_vertex;
            if (existing != alternatives.end()) {
                output_vertex = *existing;
            } else {
                output_vertex = int(mesh.vertices.size());
                mesh.vertices.push_back(m_mesh.vertices[source_vertex]);
                colors.push_back(color);
                alternatives.push_back(output_vertex);
            }
            mesh.indices[face_index][corner] = output_vertex;
        }
    }
}

bool VertexColorRegionEditor::write_obj_copy(const boost::filesystem::path& source,
                                             const boost::filesystem::path& destination,
                                             std::string& error) const
{
    if (!ready()) {
        error = "No vertex-color model is loaded.";
        return false;
    }
    indexed_triangle_set color_mesh;
    std::vector<RGBA> colors;
    build_color_mesh(color_mesh, colors);
    if (model_artifact_format(source) == "glb" || model_artifact_format(destination) == "glb") {
        TriangleMesh source_mesh; ObjInfo source_colors;
        if (!load_model_artifact(source, source_mesh, source_colors, error)) return false;
        if (source_mesh.its.vertices.size() != m_mesh.vertices.size() || source_mesh.its.indices != m_mesh.indices) {
            error = "The source model changed while recoloring. Please reload it.";
            return false;
        }
        for (size_t i = 0; i < m_mesh.vertices.size(); ++i)
            if ((source_mesh.its.vertices[i] - m_mesh.vertices[i]).squaredNorm() > 1e-10f) {
                error = "The source geometry changed while recoloring. Please reload it.";
                return false;
            }
        return write_model_artifact(destination, color_mesh, colors, error);
    }
    boost::filesystem::ifstream input(source);
    if (!input) {
        error = "Unable to read the source OBJ.";
        return false;
    }
    boost::system::error_code filesystem_error;
    boost::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "Unable to create the edited model directory.";
        return false;
    }
    boost::filesystem::path temporary = destination;
    temporary += ".tmp";
    boost::filesystem::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        error = "Unable to create the edited OBJ.";
        return false;
    }

    // Emit the derived position/color array once. Texture and normal records keep
    // their original ordering; only face position indices are remapped below.
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t vertex = 0; vertex < color_mesh.vertices.size(); ++vertex) {
        const auto& position = color_mesh.vertices[vertex];
        const auto& color = colors[vertex];
        output << "v " << position.x() << ' ' << position.y() << ' ' << position.z() << ' '
               << color[0] << ' ' << color[1] << ' ' << color[2] << ' ' << color[3] << '\n';
    }
    auto invalid_source = [&]() {
        output.close();
        boost::filesystem::remove(temporary, filesystem_error);
        error = "The source OBJ geometry or face layout changed while recoloring. Please reload it.";
        return false;
    };
    std::string line;
    size_t vertex_index = 0;
    size_t face_index = 0;
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string tag;
        parser >> tag;
        if (tag != "v" && tag != "f") {
            output << line << '\n';
            continue;
        }
        if (tag == "v") {
            Vec3f position;
            if (!(parser >> position.x() >> position.y() >> position.z()) ||
                vertex_index >= m_mesh.vertices.size())
                return invalid_source();
            if (!position.allFinite() ||
                (position - m_mesh.vertices[vertex_index]).squaredNorm() > 1e-10f)
                return invalid_source();
            ++vertex_index;
            continue;
        }

        std::vector<int> source_indices;
        std::vector<std::string> suffixes;
        std::string token;
        while (parser >> token) {
            if (token.front() == '#')
                break;
            const size_t slash = token.find('/');
            const std::string index_text = token.substr(0, slash);
            std::istringstream index_parser(index_text);
            int64_t index;
            char trailing;
            if (!(index_parser >> index) || (index_parser >> trailing) || index == 0)
                return invalid_source();
            index = index < 0 ? int64_t(vertex_index) + index : index - 1;
            if (index < 0 || index >= int64_t(m_mesh.vertices.size()))
                return invalid_source();
            source_indices.push_back(int(index));
            suffixes.push_back(slash == std::string::npos ? "" : token.substr(slash));
        }
        // Match the OBJ reader's triangle/quad fan, preserving face identity even
        // when the reader corrected the source winding on a closed mesh.
        if (source_indices.size() < 3 || source_indices.size() > 4)
            return invalid_source();
        for (size_t triangle = 1; triangle + 1 < source_indices.size(); ++triangle) {
            if (face_index >= m_mesh.indices.size())
                return invalid_source();
            const std::array<size_t, 3> fan {{0, triangle, triangle + 1}};
            const auto& source_face = m_mesh.indices[face_index];
            std::array<size_t, 3> token_indices;
            for (size_t corner = 0; corner < 3; ++corner) {
                const auto found = std::find_if(fan.begin(), fan.end(),
                    [&](size_t token_index) { return source_indices[token_index] == source_face[corner]; });
                if (found == fan.end())
                    return invalid_source();
                token_indices[corner] = *found;
            }
            output << "f";
            for (size_t corner = 0; corner < 3; ++corner)
                output << ' ' << color_mesh.indices[face_index][corner] + 1 << suffixes[token_indices[corner]];
            output << '\n';
            ++face_index;
        }
    }
    output.close();
    if (!output || input.bad() || vertex_index != m_vertex_colors.size() || face_index != m_mesh.indices.size()) {
        boost::filesystem::remove(temporary, filesystem_error);
        error = "The edited OBJ could not be written completely.";
        return false;
    }
    boost::filesystem::remove(destination, filesystem_error);
    filesystem_error.clear();
    boost::filesystem::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        boost::filesystem::remove(temporary, filesystem_error);
        error = "Unable to finalize the edited OBJ.";
        return false;
    }
    return true;
}

} // namespace Slic3r::AI
