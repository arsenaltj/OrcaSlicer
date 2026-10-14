#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::AI::SmartSlicing {
namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TOP_MICRO_SLOPE_DEGREES = 15.0;
constexpr double THIN_WALL_FEATURE_MM = 0.8;

struct Vector3
{
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

Vector3 subtract(const Vec3f& lhs, const Vec3f& rhs)
{
    return {double(lhs.x() - rhs.x()), double(lhs.y() - rhs.y()), double(lhs.z() - rhs.z())};
}

Vector3 cross(const Vector3& lhs, const Vector3& rhs)
{
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

double dot(const Vector3& lhs, const Vector3& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

double norm(const Vector3& value)
{
    return std::sqrt(dot(value, value));
}

double edge_length(const Vec3f& lhs, const Vec3f& rhs)
{
    return norm(subtract(lhs, rhs));
}

} // namespace

SmartAnalysisResult SmartSlicingEngine::AnalyzeAndPrecompute(
    const ModelObject* model, const MachinePreset& machine, const FilamentPreset& filament) const
{
    if (model == nullptr)
        return {SmartAnalysisResult::CONTRACT_VERSION, false, "smart_slice_model_unavailable", {}, {}};

    const TriangleMesh mesh = model->raw_mesh();
    const indexed_triangle_set& its = mesh.its;
    ModelMetrics metrics;
    metrics.triangle_count = its.indices.size();
    if (its.vertices.empty() || its.indices.empty()) {
        metrics.finite_geometry = false;
        return {SmartAnalysisResult::CONTRACT_VERSION, false, "smart_slice_mesh_empty", metrics, {}};
    }

    metrics.bounding_box.min_x_mm = metrics.bounding_box.min_y_mm =
        metrics.bounding_box.min_z_mm = std::numeric_limits<double>::max();
    metrics.bounding_box.max_x_mm = metrics.bounding_box.max_y_mm =
        metrics.bounding_box.max_z_mm = std::numeric_limits<double>::lowest();
    for (const Vec3f& vertex : its.vertices) {
        if (!vertex.array().allFinite()) {
            metrics.finite_geometry = false;
            return {SmartAnalysisResult::CONTRACT_VERSION, false,
                    "smart_slice_vertex_non_finite", metrics, {}};
        }
        metrics.bounding_box.min_x_mm = std::min(metrics.bounding_box.min_x_mm, double(vertex.x()));
        metrics.bounding_box.min_y_mm = std::min(metrics.bounding_box.min_y_mm, double(vertex.y()));
        metrics.bounding_box.min_z_mm = std::min(metrics.bounding_box.min_z_mm, double(vertex.z()));
        metrics.bounding_box.max_x_mm = std::max(metrics.bounding_box.max_x_mm, double(vertex.x()));
        metrics.bounding_box.max_y_mm = std::max(metrics.bounding_box.max_y_mm, double(vertex.y()));
        metrics.bounding_box.max_z_mm = std::max(metrics.bounding_box.max_z_mm, double(vertex.z()));
    }

    double signed_volume = 0.0;
    double overhang_45_area = 0.0;
    double overhang_60_area = 0.0;
    double top_micro_area = 0.0;
    double thin_wall_area = 0.0;
    for (const Vec3i32& triangle : its.indices) {
        if ((triangle.array() < 0).any() ||
            static_cast<size_t>(triangle.maxCoeff()) >= its.vertices.size()) {
            metrics.finite_geometry = false;
            return {SmartAnalysisResult::CONTRACT_VERSION, false,
                    "smart_slice_triangle_index_invalid", metrics, {}};
        }
        const Vec3f& a = its.vertices[triangle.x()];
        const Vec3f& b = its.vertices[triangle.y()];
        const Vec3f& c = its.vertices[triangle.z()];
        const Vector3 ab = subtract(b, a);
        const Vector3 ac = subtract(c, a);
        const Vector3 area_vector = cross(ab, ac);
        const double area = 0.5 * norm(area_vector);
        if (!std::isfinite(area) || area <= 1e-12)
            continue;
        metrics.surface_area_mm2 += area;
        signed_volume += dot({double(a.x()), double(a.y()), double(a.z())},
                             cross({double(b.x()), double(b.y()), double(b.z())},
                                   {double(c.x()), double(c.y()), double(c.z())})) / 6.0;

        const Vector3 normal = {area_vector.x / (2.0 * area), area_vector.y / (2.0 * area),
                                area_vector.z / (2.0 * area)};
        if (normal.z < 0.0) {
            // Angles are measured from the downward vertical.  The 45°/60°
            // thresholds match the support policy exposed to the UI.
            const double angle = std::acos(std::clamp(-normal.z, 0.0, 1.0)) * 180.0 / PI;
            if (angle >= 45.0) overhang_45_area += area;
            if (angle >= 60.0) overhang_60_area += area;
        } else if (normal.z >= std::cos(TOP_MICRO_SLOPE_DEGREES * PI / 180.0)) {
            top_micro_area += area;
        }

        if (std::min({edge_length(a, b), edge_length(b, c), edge_length(c, a)}) <=
            THIN_WALL_FEATURE_MM)
            thin_wall_area += area;
    }

    const double area = metrics.surface_area_mm2;
    metrics.volume_mm3 = std::abs(signed_volume);
    metrics.high_overhang_area_ratio_45_deg = area > 0.0 ? overhang_45_area / area : 0.0;
    metrics.severe_overhang_area_ratio_60_deg = area > 0.0 ? overhang_60_area / area : 0.0;
    metrics.top_micro_slope_surface_ratio = area > 0.0 ? top_micro_area / area : 0.0;
    metrics.thin_wall_feature_ratio = area > 0.0 ? thin_wall_area / area : 0.0;
    metrics.closed_mesh = its_num_open_edges(its) == 0;
    return AnalyzeAndPrecompute(metrics, machine, filament);
}

} // namespace Slic3r::AI::SmartSlicing
