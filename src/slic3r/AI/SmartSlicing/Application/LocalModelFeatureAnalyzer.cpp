#include "LocalModelFeatureAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

constexpr double PI = 3.14159265358979323846;
constexpr size_t SNAPSHOT_DYNAMIC_STORAGE_ALLOWANCE = 2048;

MeshPoint3d add(const MeshPoint3d& lhs, const MeshPoint3d& rhs)
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

MeshPoint3d subtract(const MeshPoint3d& lhs, const MeshPoint3d& rhs)
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

MeshPoint3d multiply(const MeshPoint3d& value, double scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

double dot(const MeshPoint3d& lhs, const MeshPoint3d& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

MeshPoint3d cross(const MeshPoint3d& lhs, const MeshPoint3d& rhs)
{
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

double norm(const MeshPoint3d& value)
{
    return std::sqrt(dot(value, value));
}

bool finite(const MeshPoint3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

MeshPoint3d triangle_centroid(const MeshPoint3d& a, const MeshPoint3d& b, const MeshPoint3d& c)
{
    return multiply(add(add(a, b), c), 1.0 / 3.0);
}

class MemoryBudget
{
public:
    explicit MemoryBudget(size_t maximum) : m_maximum(maximum) {}

    bool consume(size_t bytes)
    {
        if (bytes > m_maximum - std::min(m_used, m_maximum))
            return false;
        m_used += bytes;
        m_peak = std::max(m_peak, m_used);
        return true;
    }

    bool consume_array(size_t count, size_t item_size)
    {
        if (count != 0 && item_size > std::numeric_limits<size_t>::max() / count)
            return false;
        return consume(count * item_size);
    }

    size_t peak() const { return m_peak; }

private:
    size_t m_maximum{0};
    size_t m_used{0};
    size_t m_peak{0};
};

bool canceled(const ModelFeatureAnalysisLimits& limits)
{
    return limits.cancellation_requested && limits.cancellation_requested();
}

ModelFeatureAnalysisResult stopped(ModelFeatureAnalysisStatus status, std::string diagnostic, size_t peak)
{
    ModelFeatureAnalysisResult result;
    result.status = status;
    result.diagnostic_code = std::move(diagnostic);
    result.peak_accounted_memory_bytes = peak;
    return result;
}

struct FaceData
{
    double area{0.0};
    double signed_volume{0.0};
    MeshPoint3d normal;
    MeshPoint3d centroid;
    MeshPoint3d signed_centroid_moment;
    bool degenerate{true};
};

struct EdgeOccurrence
{
    uint32_t first{0};
    uint32_t second{0};
    uint32_t face{0};
    int direction{0};
};

bool edge_less(const EdgeOccurrence& lhs, const EdgeOccurrence& rhs)
{
    if (lhs.first != rhs.first) return lhs.first < rhs.first;
    if (lhs.second != rhs.second) return lhs.second < rhs.second;
    return lhs.face < rhs.face;
}

class DisjointSet
{
public:
    explicit DisjointSet(size_t count) : m_parent(count), m_rank(count, 0)
    {
        std::iota(m_parent.begin(), m_parent.end(), size_t{0});
    }

    size_t find(size_t item)
    {
        size_t root = item;
        while (m_parent[root] != root)
            root = m_parent[root];
        while (m_parent[item] != item) {
            const size_t next = m_parent[item];
            m_parent[item] = root;
            item = next;
        }
        return root;
    }

    void unite(size_t lhs, size_t rhs)
    {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs)
            return;
        if (m_rank[lhs] < m_rank[rhs])
            std::swap(lhs, rhs);
        m_parent[rhs] = lhs;
        if (m_rank[lhs] == m_rank[rhs])
            ++m_rank[lhs];
    }

private:
    std::vector<size_t> m_parent;
    std::vector<uint8_t> m_rank;
};

void append_edge(std::vector<EdgeOccurrence>& edges, uint32_t from, uint32_t to, uint32_t face)
{
    const uint32_t first = std::min(from, to);
    const uint32_t second = std::max(from, to);
    edges.push_back({first, second, face, from == first ? 1 : -1});
}

std::pair<double, MeshPoint3d> longest_xy_edge(const MeshPoint3d& a, const MeshPoint3d& b,
                                               const MeshPoint3d& c, double epsilon)
{
    const std::array<std::pair<MeshPoint3d, MeshPoint3d>, 3> edges{{{a, b}, {b, c}, {c, a}}};
    double longest = 0.0;
    MeshPoint3d direction;
    for (const auto& [from, to] : edges) {
        MeshPoint3d candidate{to.x - from.x, to.y - from.y, 0.0};
        const double length = std::sqrt(candidate.x * candidate.x + candidate.y * candidate.y);
        if (length <= longest)
            continue;
        longest = length;
        direction = length > epsilon ? multiply(candidate, 1.0 / length) : MeshPoint3d{};
    }
    if (direction.x < -epsilon || (std::abs(direction.x) <= epsilon && direction.y < 0.0))
        direction = multiply(direction, -1.0);
    return {longest, direction};
}

ModelFeatureSnapshot initial_snapshot(const TriangleMeshSnapshot& mesh,
                                      const ModelFeatureAnalysisPolicy& policy)
{
    ModelFeatureSnapshot snapshot;
    snapshot.analysis_version = LocalModelFeatureAnalyzer::ANALYSIS_VERSION;
    snapshot.policy_version = policy.version;
    snapshot.object_id = mesh.object_id;
    snapshot.volume_id = mesh.volume_id;
    snapshot.geometry_fingerprint = mesh.geometry_fingerprint;

    snapshot.small_hole_candidates = FeatureValue<std::vector<SmallHoleCandidate>>::unavailable(
        "cross_section_analysis_not_available_v1");
    snapshot.small_text_candidates = FeatureValue<std::vector<SmallTextCandidate>>::unavailable(
        "semantic_scale_analysis_not_available_v1");
    snapshot.curvature_distribution = FeatureValue<CurvatureDistribution>::unavailable(
        "curvature_analysis_not_available_v1");
    snapshot.orientation_sensitive_surfaces = FeatureValue<SurfaceCandidateSummary>::unknown(
        "requires_orientation_search");
    snapshot.visibility_approximation = FeatureValue<VisibilityApproximation>::unavailable(
        "viewpoint_input_unavailable");
    snapshot.support_contact_risk = FeatureValue<RiskEstimate>::unknown("requires_trial_slice");
    snapshot.bed_stability = FeatureValue<RiskEstimate>::unknown("requires_contact_polygon_analysis");
    snapshot.seam_visibility_risk = FeatureValue<RiskEstimate>::unknown(
        "requires_seam_policy_and_viewpoint");
    snapshot.material_partition_count = FeatureValue<size_t>::unavailable("material_partition_input_unavailable");
    snapshot.color_partition_count = FeatureValue<size_t>::unavailable("color_partition_input_unavailable");
    snapshot.layer_tool_sequence_complexity = FeatureValue<ToolSequenceComplexity>::unavailable(
        "requires_trial_slice");
    snapshot.tool_change_density_per_layer = FeatureValue<double>::unavailable("requires_trial_slice");
    return snapshot;
}

ModelFeatureAnalysisResult analyze_mesh(const TriangleMeshSnapshot& mesh,
                                        const ModelFeatureAnalysisLimits& limits,
                                        const ModelFeatureAnalysisPolicy& policy)
{
    MemoryBudget memory(limits.maximum_working_memory_bytes);
    if (canceled(limits))
        return stopped(ModelFeatureAnalysisStatus::Canceled, "model_feature_analysis_canceled", memory.peak());
    if (!memory.consume(triangle_mesh_snapshot_bytes(mesh)) ||
        !memory.consume(sizeof(ModelFeatureSnapshot) + policy.version.size() +
                        SNAPSHOT_DYNAMIC_STORAGE_ALLOWANCE))
        return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                       "model_feature_memory_budget_exceeded", memory.peak());
    if (mesh.vertices.empty() || mesh.triangles.empty())
        return stopped(ModelFeatureAnalysisStatus::InvalidInput, "model_feature_mesh_empty", memory.peak());

    AxisAlignedBox3d bounds{mesh.vertices.front(), mesh.vertices.front()};
    for (const MeshPoint3d& vertex : mesh.vertices) {
        if (canceled(limits))
            return stopped(ModelFeatureAnalysisStatus::Canceled,
                           "model_feature_analysis_canceled", memory.peak());
        if (!finite(vertex))
            return stopped(ModelFeatureAnalysisStatus::InvalidInput,
                           "model_feature_vertex_non_finite", memory.peak());
        bounds.minimum.x = std::min(bounds.minimum.x, vertex.x);
        bounds.minimum.y = std::min(bounds.minimum.y, vertex.y);
        bounds.minimum.z = std::min(bounds.minimum.z, vertex.z);
        bounds.maximum.x = std::max(bounds.maximum.x, vertex.x);
        bounds.maximum.y = std::max(bounds.maximum.y, vertex.y);
        bounds.maximum.z = std::max(bounds.maximum.z, vertex.z);
    }

    if (!memory.consume_array(mesh.triangles.size(),
                              sizeof(FaceData) + sizeof(size_t) + sizeof(uint8_t)))
        return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                       "model_feature_memory_budget_exceeded", memory.peak());
    std::vector<FaceData> faces(mesh.triangles.size());
    DisjointSet components(mesh.triangles.size());
    std::vector<EdgeOccurrence> edges;
    size_t degenerate_count = 0;
    size_t usable_face_count = 0;
    double surface_area = 0.0;

    for (size_t face_index = 0; face_index < mesh.triangles.size(); ++face_index) {
        if (canceled(limits))
            return stopped(ModelFeatureAnalysisStatus::Canceled,
                           "model_feature_analysis_canceled", memory.peak());
        const MeshTriangle& triangle = mesh.triangles[face_index];
        if (std::any_of(triangle.vertex_indices.begin(), triangle.vertex_indices.end(),
                        [&](uint32_t index) { return index >= mesh.vertices.size(); }))
            return stopped(ModelFeatureAnalysisStatus::InvalidInput,
                           "model_feature_triangle_index_invalid", memory.peak());

        const MeshPoint3d& a = mesh.vertices[triangle.vertex_indices[0]];
        const MeshPoint3d& b = mesh.vertices[triangle.vertex_indices[1]];
        const MeshPoint3d& c = mesh.vertices[triangle.vertex_indices[2]];
        const MeshPoint3d area_vector = cross(subtract(b, a), subtract(c, a));
        const double area = 0.5 * norm(area_vector);
        FaceData& face = faces[face_index];
        face.area = area;
        face.centroid = triangle_centroid(a, b, c);
        if (area <= policy.degenerate_area_epsilon_mm2) {
            ++degenerate_count;
            continue;
        }

        face.degenerate = false;
        face.normal = multiply(area_vector, 1.0 / (2.0 * area));
        face.signed_volume = dot(a, cross(b, c)) / 6.0;
        face.signed_centroid_moment = multiply(add(add(a, b), c), face.signed_volume / 4.0);
        surface_area += area;
        ++usable_face_count;

        constexpr size_t EDGE_ACCOUNTING = 2 * sizeof(EdgeOccurrence);
        if (!memory.consume(3 * EDGE_ACCOUNTING))
            return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                           "model_feature_memory_budget_exceeded", memory.peak());
        append_edge(edges, triangle.vertex_indices[0], triangle.vertex_indices[1],
                    static_cast<uint32_t>(face_index));
        append_edge(edges, triangle.vertex_indices[1], triangle.vertex_indices[2],
                    static_cast<uint32_t>(face_index));
        append_edge(edges, triangle.vertex_indices[2], triangle.vertex_indices[0],
                    static_cast<uint32_t>(face_index));
    }

    if (canceled(limits))
        return stopped(ModelFeatureAnalysisStatus::Canceled, "model_feature_analysis_canceled", memory.peak());
    std::sort(edges.begin(), edges.end(), edge_less);
    if (canceled(limits))
        return stopped(ModelFeatureAnalysisStatus::Canceled, "model_feature_analysis_canceled", memory.peak());

    bool has_boundary = false;
    bool has_non_manifold_edge = false;
    bool winding_consistent = true;
    for (size_t first = 0; first < edges.size();) {
        if (canceled(limits))
            return stopped(ModelFeatureAnalysisStatus::Canceled,
                           "model_feature_analysis_canceled", memory.peak());
        size_t last = first + 1;
        while (last < edges.size() && edges[last].first == edges[first].first &&
               edges[last].second == edges[first].second)
            ++last;
        const size_t count = last - first;
        if (count == 1) {
            has_boundary = true;
        } else if (count == 2) {
            components.unite(edges[first].face, edges[first + 1].face);
            if (edges[first].direction == edges[first + 1].direction)
                winding_consistent = false;
        } else {
            has_non_manifold_edge = true;
            winding_consistent = false;
        }
        first = last;
    }

    ModelFeatureSnapshot snapshot = initial_snapshot(mesh, policy);
    snapshot.bounding_box = FeatureValue<AxisAlignedBox3d>::known(bounds);
    snapshot.surface_area_mm2 = FeatureValue<double>::known(surface_area);
    snapshot.degenerate_faces = FeatureValue<DegenerateFaceSummary>::known(
        {degenerate_count, policy.degenerate_area_epsilon_mm2});

    const MeshPoint3d extent = subtract(bounds.maximum, bounds.minimum);
    const double footprint_width = std::max(extent.x, extent.y);
    if (footprint_width > policy.scalar_epsilon)
        snapshot.height_to_footprint_ratio = FeatureValue<double>::known(extent.z / footprint_width);
    else
        snapshot.height_to_footprint_ratio = FeatureValue<double>::unknown("footprint_extent_too_small");

    std::vector<ThinWallCandidate> thin_candidates;
    const std::array<double, 3> extents{extent.x, extent.y, extent.z};
    for (size_t axis = 0; axis < extents.size(); ++axis) {
        if (extents[axis] > policy.thin_wall_global_extent_maximum_mm)
            continue;
        if (!memory.consume(2 * sizeof(ThinWallCandidate)))
            return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                           "model_feature_memory_budget_exceeded", memory.peak());
        thin_candidates.push_back({bounds,
                                   multiply(add(bounds.minimum, bounds.maximum), 0.5),
                                   static_cast<PrincipalAxis>(axis), extents[axis], {0.0, 0.35}});
    }
    if (thin_candidates.empty())
        snapshot.thin_wall_candidates = FeatureValue<std::vector<ThinWallCandidate>>::unknown(
            "no_axis_aligned_global_extent_candidate");
    else
        snapshot.thin_wall_candidates = FeatureValue<std::vector<ThinWallCandidate>>::known(
            std::move(thin_candidates));

    if (usable_face_count == 0) {
        snapshot.closedness = FeatureValue<MeshClosedness>::unknown("no_non_degenerate_faces");
        snapshot.winding_consistent = FeatureValue<bool>::unknown("no_non_degenerate_faces");
    } else {
        snapshot.closedness = FeatureValue<MeshClosedness>::known(
            has_non_manifold_edge ? MeshClosedness::NonManifold :
            has_boundary ? MeshClosedness::Open : MeshClosedness::Closed);
        snapshot.winding_consistent = FeatureValue<bool>::known(winding_consistent);
    }

    const bool closed_oriented = usable_face_count > 0 && !has_boundary && !has_non_manifold_edge &&
                                 winding_consistent;
    if (!closed_oriented) {
        snapshot.volume_mm3 = FeatureValue<double>::unknown("requires_closed_oriented_mesh");
        snapshot.center_of_mass = FeatureValue<MeshPoint3d>::unknown("requires_closed_oriented_mesh");
        snapshot.bed_contact_candidate = FeatureValue<BedContactCandidate>::unknown(
            "requires_closed_oriented_mesh");
        snapshot.overhangs = FeatureValue<OverhangSummary>::unknown("requires_closed_oriented_mesh");
        snapshot.bridge_candidates = FeatureValue<std::vector<BridgeCandidate>>::unknown(
            "requires_closed_oriented_mesh");
    } else {
        if (!memory.consume_array(mesh.triangles.size(),
                                  2 * sizeof(double) + sizeof(MeshPoint3d) + sizeof(size_t)))
            return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                           "model_feature_memory_budget_exceeded", memory.peak());
        std::vector<double> component_volumes(mesh.triangles.size(), 0.0);
        std::vector<MeshPoint3d> component_moments(mesh.triangles.size());
        std::vector<size_t> component_face_counts(mesh.triangles.size(), 0);
        for (size_t face_index = 0; face_index < faces.size(); ++face_index) {
            if (canceled(limits))
                return stopped(ModelFeatureAnalysisStatus::Canceled,
                               "model_feature_analysis_canceled", memory.peak());
            if (faces[face_index].degenerate)
                continue;
            const size_t root = components.find(face_index);
            component_volumes[root] += faces[face_index].signed_volume;
            component_moments[root] = add(component_moments[root], faces[face_index].signed_centroid_moment);
            ++component_face_counts[root];
        }

        bool reliable_volume = true;
        double total_volume = 0.0;
        MeshPoint3d total_moment;
        std::vector<double> component_sign(mesh.triangles.size(), 0.0);
        for (size_t root = 0; root < component_volumes.size(); ++root) {
            if (component_face_counts[root] == 0)
                continue;
            if (canceled(limits))
                return stopped(ModelFeatureAnalysisStatus::Canceled,
                               "model_feature_analysis_canceled", memory.peak());
            const double signed_volume = component_volumes[root];
            if (std::abs(signed_volume) <= policy.scalar_epsilon) {
                reliable_volume = false;
                break;
            }
            const double sign = signed_volume < 0.0 ? -1.0 : 1.0;
            component_sign[root] = sign;
            total_volume += std::abs(signed_volume);
            total_moment = add(total_moment, multiply(component_moments[root], sign));
        }

        if (!reliable_volume || total_volume <= policy.scalar_epsilon) {
            snapshot.volume_mm3 = FeatureValue<double>::unknown("component_volume_unreliable");
            snapshot.center_of_mass = FeatureValue<MeshPoint3d>::unknown("component_volume_unreliable");
            snapshot.bed_contact_candidate = FeatureValue<BedContactCandidate>::unknown(
                "component_orientation_unreliable");
            snapshot.overhangs = FeatureValue<OverhangSummary>::unknown("component_orientation_unreliable");
            snapshot.bridge_candidates = FeatureValue<std::vector<BridgeCandidate>>::unknown(
                "component_orientation_unreliable");
        } else {
            snapshot.volume_mm3 = FeatureValue<double>::known(total_volume);
            snapshot.center_of_mass = FeatureValue<MeshPoint3d>::known(multiply(total_moment, 1.0 / total_volume));

            BedContactCandidate bed_contact;
            bed_contact.plane_z_mm = bounds.minimum.z;
            OverhangSummary overhang;
            overhang.bands = {{
                {OverhangSeverity::Severe, 0.0, policy.overhang_maximum_angle_from_down_degrees[0], 0.0, 0},
                {OverhangSeverity::Moderate, policy.overhang_maximum_angle_from_down_degrees[0],
                 policy.overhang_maximum_angle_from_down_degrees[1], 0.0, 0},
                {OverhangSeverity::Mild, policy.overhang_maximum_angle_from_down_degrees[1],
                 policy.overhang_maximum_angle_from_down_degrees[2], 0.0, 0},
            }};
            std::vector<BridgeCandidate> bridge_candidates;

            for (size_t face_index = 0; face_index < faces.size(); ++face_index) {
                if (canceled(limits))
                    return stopped(ModelFeatureAnalysisStatus::Canceled,
                                   "model_feature_analysis_canceled", memory.peak());
                const FaceData& face = faces[face_index];
                if (face.degenerate)
                    continue;
                const size_t root = components.find(face_index);
                const MeshPoint3d normal = multiply(face.normal, component_sign[root]);
                const MeshTriangle& triangle = mesh.triangles[face_index];
                const MeshPoint3d& a = mesh.vertices[triangle.vertex_indices[0]];
                const MeshPoint3d& b = mesh.vertices[triangle.vertex_indices[1]];
                const MeshPoint3d& c = mesh.vertices[triangle.vertex_indices[2]];
                const bool at_bed = std::abs(a.z - bounds.minimum.z) <= policy.bed_plane_tolerance_mm &&
                                    std::abs(b.z - bounds.minimum.z) <= policy.bed_plane_tolerance_mm &&
                                    std::abs(c.z - bounds.minimum.z) <= policy.bed_plane_tolerance_mm;
                if (at_bed && -normal.z >= policy.bed_facing_minimum_cosine) {
                    bed_contact.projected_area_mm2 += face.area * -normal.z;
                    ++bed_contact.facet_count;
                }

                if (normal.z < 0.0 && !at_bed) {
                    const double angle = std::acos(std::clamp(-normal.z, 0.0, 1.0)) * 180.0 / PI;
                    for (OverhangBandSummary& band : overhang.bands) {
                        if (angle <= band.maximum_angle_from_down_degrees) {
                            band.surface_area_mm2 += face.area;
                            ++band.facet_count;
                            break;
                        }
                    }
                }

                if (!at_bed && -normal.z >= policy.bridge_facing_minimum_cosine) {
                    const auto [span, direction] = longest_xy_edge(a, b, c, policy.scalar_epsilon);
                    if (span >= policy.bridge_minimum_span_mm) {
                        if (!memory.consume(2 * sizeof(BridgeCandidate)))
                            return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                                           "model_feature_memory_budget_exceeded", memory.peak());
                        bridge_candidates.push_back({static_cast<uint32_t>(face_index), face.centroid, direction,
                                                     span, face.area * -normal.z, {0.0, 0.45}});
                    }
                }
            }
            snapshot.bed_contact_candidate = FeatureValue<BedContactCandidate>::known(bed_contact);
            snapshot.overhangs = FeatureValue<OverhangSummary>::known(overhang);
            if (bridge_candidates.empty())
                snapshot.bridge_candidates = FeatureValue<std::vector<BridgeCandidate>>::unknown(
                    "no_high_confidence_bridge_candidate");
            else
                snapshot.bridge_candidates = FeatureValue<std::vector<BridgeCandidate>>::known(
                    std::move(bridge_candidates));
        }
    }

    ModelFeatureAnalysisResult result;
    result.status = ModelFeatureAnalysisStatus::Completed;
    result.snapshot = std::move(snapshot);
    result.peak_accounted_memory_bytes = memory.peak();
    return result;
}

} // namespace

bool ModelFeatureAnalysisPolicy::valid() const
{
    return !version.empty() && scalar_epsilon > 0.0 && degenerate_area_epsilon_mm2 >= 0.0 &&
           bed_plane_tolerance_mm >= 0.0 && bed_facing_minimum_cosine >= 0.0 &&
           bed_facing_minimum_cosine <= 1.0 && bridge_facing_minimum_cosine >= 0.0 &&
           bridge_facing_minimum_cosine <= 1.0 && bridge_minimum_span_mm >= 0.0 &&
           thin_wall_global_extent_maximum_mm >= 0.0 &&
           overhang_maximum_angle_from_down_degrees[0] > 0.0 &&
           overhang_maximum_angle_from_down_degrees[0] < overhang_maximum_angle_from_down_degrees[1] &&
           overhang_maximum_angle_from_down_degrees[1] < overhang_maximum_angle_from_down_degrees[2] &&
           overhang_maximum_angle_from_down_degrees[2] <= 90.0;
}

LocalModelFeatureAnalyzer::LocalModelFeatureAnalyzer(ModelFeatureAnalysisPolicy policy) : m_policy(std::move(policy))
{
    if (!m_policy.valid())
        throw std::invalid_argument("Invalid model feature analysis policy.");
}

ModelFeatureAnalysisResult LocalModelFeatureAnalyzer::analyze(
    const TriangleMeshSnapshot& mesh, const ModelFeatureAnalysisLimits& limits) const
{
    try {
        return analyze_mesh(mesh, limits, m_policy);
    } catch (const std::bad_alloc&) {
        return stopped(ModelFeatureAnalysisStatus::ResourceLimitExceeded,
                       "model_feature_system_memory_exhausted", 0);
    }
}

const char* model_feature_analysis_status_name(ModelFeatureAnalysisStatus status)
{
    switch (status) {
    case ModelFeatureAnalysisStatus::Completed: return "completed";
    case ModelFeatureAnalysisStatus::Canceled: return "canceled";
    case ModelFeatureAnalysisStatus::ResourceLimitExceeded: return "resource_limit_exceeded";
    case ModelFeatureAnalysisStatus::InvalidInput: return "invalid_input";
    }
    return "invalid_input";
}

} // namespace Slic3r::AI::SmartSlicing
