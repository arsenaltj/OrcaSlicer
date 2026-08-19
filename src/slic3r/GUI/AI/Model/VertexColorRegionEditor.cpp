#include "VertexColorRegionEditor.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace Slic3r::AI {
namespace {

constexpr float PI = 3.14159265358979323846f;

uint64_t edge_key(uint32_t first, uint32_t second)
{
    if (first > second)
        std::swap(first, second);
    return (uint64_t(first) << 32) | uint64_t(second);
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

} // namespace

bool VertexColorRegionEditor::initialize(indexed_triangle_set mesh, std::vector<RGBA> vertex_colors,
                                         std::string& error)
{
    clear();
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
    m_face_normals.resize(m_mesh.indices.size());
    m_face_centers.resize(m_mesh.indices.size());
    m_face_neighbors.resize(m_mesh.indices.size());
    m_selected_faces.assign(m_mesh.indices.size(), 0);

    Vec3f minimum = m_mesh.vertices.front();
    Vec3f maximum = minimum;
    for (const Vec3f& vertex : m_mesh.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    m_mesh_diagonal = (maximum - minimum).norm();

    std::unordered_map<uint64_t, uint32_t> first_face_by_edge;
    first_face_by_edge.reserve(m_mesh.indices.size() * 3);
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        const Vec3f& a = m_mesh.vertices[face[0]];
        const Vec3f& b = m_mesh.vertices[face[1]];
        const Vec3f& c = m_mesh.vertices[face[2]];
        Vec3f normal = (b - a).cross(c - a);
        if (normal.squaredNorm() > 1e-12f)
            normal.normalize();
        else
            normal = Vec3f::UnitZ();
        m_face_normals[face_index] = normal;
        m_face_centers[face_index] = (a + b + c) / 3.0f;

        const std::array<std::pair<uint32_t, uint32_t>, 3> edges {{
            {uint32_t(face[0]), uint32_t(face[1])},
            {uint32_t(face[1]), uint32_t(face[2])},
            {uint32_t(face[2]), uint32_t(face[0])}
        }};
        for (const auto& edge : edges) {
            const uint64_t key = edge_key(edge.first, edge.second);
            const auto owner = first_face_by_edge.find(key);
            if (owner == first_face_by_edge.end()) {
                first_face_by_edge.emplace(key, uint32_t(face_index));
            } else if (owner->second != face_index) {
                m_face_neighbors[face_index].push_back(owner->second);
                m_face_neighbors[owner->second].push_back(uint32_t(face_index));
            }
        }
    }
    return true;
}

void VertexColorRegionEditor::clear()
{
    m_mesh = {};
    m_vertex_colors.clear();
    m_face_normals.clear();
    m_face_centers.clear();
    m_face_neighbors.clear();
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
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        double distance = 0.0;
        if (ray_triangle_intersection(ray_origin, direction,
                                      m_mesh.vertices[face[0]], m_mesh.vertices[face[1]],
                                      m_mesh.vertices[face[2]], distance) && distance < nearest) {
            nearest = distance;
            result = face_index;
        }
    }
    return result;
}

RGBA VertexColorRegionEditor::face_color(size_t face_index) const
{
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
    if (!ready() || seed_face >= m_mesh.indices.size())
        return m_selected_face_count;
    const std::vector<size_t> region = operation == RegionSelectionOperation::Replace
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
    std::vector<uint8_t> selected_vertices(m_mesh.vertices.size(), 0);
    for (size_t face_index = 0; face_index < m_mesh.indices.size(); ++face_index) {
        if (!m_selected_faces[face_index])
            continue;
        const stl_triangle_vertex_indices& face = m_mesh.indices[face_index];
        selected_vertices[face[0]] = 1;
        selected_vertices[face[1]] = 1;
        selected_vertices[face[2]] = 1;
    }
    for (size_t vertex = 0; vertex < selected_vertices.size(); ++vertex) {
        if (selected_vertices[vertex])
            m_vertex_colors[vertex] = color;
    }
    return true;
}

bool VertexColorRegionEditor::apply_color_to_obj_copy(const RGBA& color,
                                                      const boost::filesystem::path& source,
                                                      const boost::filesystem::path& destination,
                                                      std::string& error)
{
    const std::vector<RGBA> original_colors = m_vertex_colors;
    if (!apply_color(color)) {
        error = "No model region is selected.";
        return false;
    }
    if (write_obj_copy(source, destination, error))
        return true;
    m_vertex_colors = original_colors;
    return false;
}

bool VertexColorRegionEditor::write_obj_copy(const boost::filesystem::path& source,
                                             const boost::filesystem::path& destination,
                                             std::string& error) const
{
    if (!ready()) {
        error = "No vertex-color model is loaded.";
        return false;
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

    std::string line;
    size_t vertex_index = 0;
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string tag;
        parser >> tag;
        if (tag != "v") {
            output << line << '\n';
            continue;
        }
        std::string x;
        std::string y;
        std::string z;
        if (!(parser >> x >> y >> z) || vertex_index >= m_vertex_colors.size()) {
            output.close();
            boost::filesystem::remove(temporary, filesystem_error);
            error = "The source OBJ vertex layout changed while recoloring.";
            return false;
        }
        const RGBA& color = m_vertex_colors[vertex_index++];
        output << "v " << x << ' ' << y << ' ' << z << ' '
               << std::fixed << std::setprecision(6)
               << color[0] << ' ' << color[1] << ' ' << color[2] << ' ' << color[3] << '\n';
    }
    output.close();
    if (!output || vertex_index != m_vertex_colors.size()) {
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
