#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr uint32_t MODEL_FEATURE_SNAPSHOT_VERSION = 1;

struct MeshPoint3d
{
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct MeshTriangle
{
    std::array<uint32_t, 3> vertex_indices{};
};

struct TriangleMeshSnapshot
{
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    std::vector<MeshPoint3d> vertices;
    std::vector<MeshTriangle> triangles;
};

enum class FeatureAvailability { Known, Unknown, Unavailable };

template<class T> struct FeatureValue
{
    FeatureAvailability availability{FeatureAvailability::Unavailable};
    std::optional<T> value;
    std::string reason_code;

    static FeatureValue known(T known_value)
    {
        FeatureValue result;
        result.availability = FeatureAvailability::Known;
        result.value = std::move(known_value);
        return result;
    }

    static FeatureValue unknown(std::string reason)
    {
        FeatureValue result;
        result.availability = FeatureAvailability::Unknown;
        result.reason_code = std::move(reason);
        return result;
    }

    static FeatureValue unavailable(std::string reason)
    {
        FeatureValue result;
        result.availability = FeatureAvailability::Unavailable;
        result.reason_code = std::move(reason);
        return result;
    }

    bool known() const { return availability == FeatureAvailability::Known && value.has_value(); }
};

struct AxisAlignedBox3d
{
    MeshPoint3d minimum;
    MeshPoint3d maximum;
};

enum class MeshClosedness { Closed, Open, NonManifold };
enum class PrincipalAxis { X, Y, Z };
enum class OverhangSeverity { Severe, Moderate, Mild };

struct DegenerateFaceSummary
{
    size_t face_count{0};
    double area_epsilon_mm2{0.0};
};

struct BedContactCandidate
{
    double projected_area_mm2{0.0};
    size_t facet_count{0};
    double plane_z_mm{0.0};
};

struct OverhangBandSummary
{
    OverhangSeverity severity{OverhangSeverity::Mild};
    double minimum_angle_from_down_degrees{0.0};
    double maximum_angle_from_down_degrees{0.0};
    double surface_area_mm2{0.0};
    size_t facet_count{0};
};

struct OverhangSummary
{
    std::array<OverhangBandSummary, 3> bands;
};

struct ConfidenceBounds
{
    double lower{0.0};
    double upper{0.0};
};

struct BridgeCandidate
{
    uint32_t facet_index{0};
    MeshPoint3d centroid;
    MeshPoint3d direction_xy;
    double span_mm{0.0};
    double projected_area_mm2{0.0};
    ConfidenceBounds confidence;
};

struct ThinWallCandidate
{
    AxisAlignedBox3d bounds;
    MeshPoint3d center;
    PrincipalAxis narrow_axis{PrincipalAxis::X};
    double scale_upper_bound_mm{0.0};
    ConfidenceBounds confidence;
};

struct SmallHoleCandidate
{
    MeshPoint3d center;
    MeshPoint3d direction;
    double diameter_upper_bound_mm{0.0};
    ConfidenceBounds confidence;
};

struct SmallTextCandidate
{
    AxisAlignedBox3d bounds;
    double feature_scale_upper_bound_mm{0.0};
    ConfidenceBounds confidence;
};

struct CurvatureDistribution
{
    double minimum_inverse_mm{0.0};
    double maximum_inverse_mm{0.0};
    double mean_inverse_mm{0.0};
    size_t sample_count{0};
};

struct SurfaceCandidateSummary
{
    double surface_area_mm2{0.0};
    size_t facet_count{0};
};

struct VisibilityApproximation
{
    double visible_fraction{0.0};
    size_t sample_count{0};
    std::string viewpoint_id;
};

struct RiskEstimate
{
    double normalized_score{0.0};
    ConfidenceBounds confidence;
};

struct ToolSequenceComplexity
{
    size_t distinct_layer_sequences{0};
    size_t maximum_tools_per_layer{0};
};

struct ModelFeatureSnapshot
{
    uint32_t schema_version{MODEL_FEATURE_SNAPSHOT_VERSION};
    std::string analysis_version;
    std::string policy_version;
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;

    FeatureValue<AxisAlignedBox3d> bounding_box;
    FeatureValue<double> volume_mm3;
    FeatureValue<double> surface_area_mm2;
    FeatureValue<BedContactCandidate> bed_contact_candidate;
    FeatureValue<RiskEstimate> bed_stability;
    FeatureValue<double> height_to_footprint_ratio;
    FeatureValue<MeshPoint3d> center_of_mass;
    FeatureValue<MeshClosedness> closedness;
    FeatureValue<bool> winding_consistent;
    FeatureValue<DegenerateFaceSummary> degenerate_faces;
    FeatureValue<OverhangSummary> overhangs;
    FeatureValue<std::vector<BridgeCandidate>> bridge_candidates;
    FeatureValue<std::vector<ThinWallCandidate>> thin_wall_candidates;

    FeatureValue<std::vector<SmallHoleCandidate>> small_hole_candidates;
    FeatureValue<std::vector<SmallTextCandidate>> small_text_candidates;
    FeatureValue<CurvatureDistribution> curvature_distribution;
    FeatureValue<SurfaceCandidateSummary> orientation_sensitive_surfaces;
    FeatureValue<VisibilityApproximation> visibility_approximation;
    FeatureValue<RiskEstimate> support_contact_risk;
    FeatureValue<RiskEstimate> seam_visibility_risk;
    FeatureValue<size_t> material_partition_count;
    FeatureValue<size_t> color_partition_count;
    FeatureValue<ToolSequenceComplexity> layer_tool_sequence_complexity;
    FeatureValue<double> tool_change_density_per_layer;
};

const char* feature_availability_name(FeatureAvailability availability);
const char* mesh_closedness_name(MeshClosedness closedness);
const char* overhang_severity_name(OverhangSeverity severity);
size_t triangle_mesh_snapshot_bytes(const TriangleMeshSnapshot& mesh);

} // namespace Slic3r::AI::SmartSlicing
