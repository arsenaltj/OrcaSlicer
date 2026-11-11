#pragma once

#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

#include <boost/filesystem/path.hpp>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Slic3r::GUI::NativePortrait {

namespace SC = AI::SemanticColoring;

inline constexpr uint32_t algorithm_version = 12;
inline constexpr uint32_t legacy_portrait_algorithm_version = 9;
inline constexpr size_t minimum_color_count = 1;
inline constexpr size_t maximum_color_count = 32;
inline constexpr size_t default_model_color_count = 6;
inline constexpr size_t portrait_model_color_count = 5;
inline constexpr float minimum_semantic_confidence = 0.70f;
inline constexpr size_t maximum_face_count = 1000000;
inline constexpr double maximum_bounds_deviation_ratio = 0.0025;

enum class BuildStage : uint8_t {
    Decode,
    SourceCheck,
    AdaptiveSubdivision,
    FaceSampling,
    StrictRepair,
    FaceResampling,
    ColorClustering,
    RegionSmoothing,
    RegionEnumeration,
    CarrierPublish,
    AppearancePublish,
    CachePublish,
    MetadataPublish,
    PreviewSwitch,
    Count
};

enum class BuildStageStatus : uint8_t { NotRun, Succeeded, Failed, Skipped };

struct BuildStageReport {
    BuildStageStatus status {BuildStageStatus::NotRun};
    uint64_t duration_ms {0};
    std::string detail;
};

struct NativePortraitPartitionBuildReport {
    std::array<BuildStageReport, size_t(BuildStage::Count)> stages;
    StrictMeshRepairResult strict_repair;
    uint64_t total_ms {0};
    bool success {false};
    std::string error;

    BuildStageReport& stage(BuildStage value) { return stages[size_t(value)]; }
    const BuildStageReport& stage(BuildStage value) const { return stages[size_t(value)]; }
};

const char* native_portrait_build_stage_name(BuildStage stage);
nlohmann::json encode_native_portrait_partition_build_report(
    const NativePortraitPartitionBuildReport& report);

struct BoundarySummary {
    size_t edge_count {0};
    size_t boundary_edges {0};
    size_t color_boundary_edges {0};
    size_t open_edges {0};
    size_t nonmanifold_edges {0};
};

// All active v11 partition carriers are published in Orca's Z-up millimetre
// frame. Bounds are persisted so an old metre-valued carrier cannot be
// mistaken for a valid v11 checkpoint.
struct NativeMillimeterBounds {
    std::array<double, 3> minimum {};
    std::array<double, 3> maximum {};

    bool valid() const;
};

// The native texture pipeline is the sole owner of this geometry and region
// membership. Semantic consumers receive it by const reference only.
struct NativeWorkbenchPartition {
    PaintedMesh painted;
    // SurfaceSelectionPersistence identity of the final native face/corner
    // sequence. It intentionally survives carrier-only vertex splitting.
    std::string geometry_id;
    std::vector<size_t> face_cluster_ids;
    std::vector<uint32_t> face_region_ids;
    std::vector<std::vector<uint32_t>> region_members;
    std::vector<size_t> region_cluster_ids;
    std::vector<std::vector<uint32_t>> region_adjacency;
    std::vector<double> region_areas;
    BoundarySummary boundary;
    std::string partition_digest;
    std::string boundary_digest;

    bool valid() const;
};

// Source compatibility for v9 consumers while they migrate to the shared
// workbench name. Persisted v9/v10 identities are never accepted as active v11
// partitions; they are read only as stale metadata and rebuilt from origin.
using NativePortraitPartition = NativeWorkbenchPartition;

// Persisted independently from semantic labels and Honor slot bindings. The
// member arrays describe only native clustering and connectivity.
struct NativeWorkbenchPartitionIdentity {
    uint32_t algorithm {algorithm_version};
    std::string geometry_id;
    size_t face_count {0};
    std::vector<std::array<size_t, 3>> cluster_colors;
    std::vector<size_t> face_cluster_ids;
    std::vector<uint32_t> face_region_ids;
    std::vector<size_t> region_cluster_ids;
    std::vector<std::vector<uint32_t>> region_adjacency;
    std::vector<double> region_areas;
    BoundarySummary boundary;
    std::string partition_digest;
    std::string boundary_digest;

    bool valid() const;
};

using NativePortraitPartitionIdentity = NativeWorkbenchPartitionIdentity;

PaintMeshRepairCallback native_mesh_repair_callback(StrictMeshRepairResult* result = nullptr);

NativeWorkbenchPartition build_native_workbench_partition(
    const TexturedMesh& texture_mesh,
    PaintMeshRepairCallback repair,
    const PaintCancelCallback& cancel = {},
    NativePortraitPartitionBuildReport* report = nullptr);

// Transitional wrapper. target_color_count is deliberately ignored: v11
// always builds the same adaptive atomic partition and reduces it afterwards.
NativeWorkbenchPartition build_native_portrait_partition(
    const TexturedMesh& texture_mesh,
    size_t target_color_count,
    PaintMeshRepairCallback repair,
    const PaintCancelCallback& cancel = {},
    NativePortraitPartitionBuildReport* report = nullptr);

// Finalize an already clustered native result. This is used by persistence
// and focused tests; it performs no color or semantic decisions.
NativeWorkbenchPartition finalize_native_workbench_partition(
    PaintedMesh painted,
    const PaintCancelCallback& cancel = {});
NativeWorkbenchPartition finalize_native_portrait_partition(
    PaintedMesh painted,
    const PaintCancelCallback& cancel = {});

NativeMillimeterBounds native_workbench_bounds(const TexturedMesh& mesh);
NativeMillimeterBounds native_workbench_bounds(const NativeWorkbenchPartition& partition);
NativeMillimeterBounds native_workbench_bounds(const indexed_triangle_set& mesh);
bool native_workbench_bounds_compatible(
    const NativeMillimeterBounds& source,
    const NativeMillimeterBounds& candidate,
    std::string& error,
    double maximum_deviation_ratio = maximum_bounds_deviation_ratio);
nlohmann::json encode_native_workbench_bounds(const NativeMillimeterBounds& bounds);
bool decode_native_workbench_bounds(
    const nlohmann::json& document,
    NativeMillimeterBounds& bounds);

NativeWorkbenchPartitionIdentity native_workbench_partition_identity(
    const NativeWorkbenchPartition& partition);
NativePortraitPartitionIdentity native_portrait_partition_identity(
    const NativePortraitPartition& partition);
nlohmann::json encode_native_portrait_partition_identity(
    const NativePortraitPartitionIdentity& identity);
bool decode_native_portrait_partition_identity(
    const nlohmann::json& document,
    NativePortraitPartitionIdentity& identity,
    std::string& error);
NativeWorkbenchPartition restore_native_workbench_partition(
    const indexed_triangle_set& mesh,
    const NativeWorkbenchPartitionIdentity& identity,
    std::string& error);
NativePortraitPartition restore_native_portrait_partition(
    const indexed_triangle_set& mesh,
    const NativePortraitPartitionIdentity& identity,
    std::string& error);

struct BindingOptions {
    std::vector<SC::Color> target_palette;
    std::map<SC::Label, size_t> semantic_slots;
    float minimum_confidence {minimum_semantic_confidence};
    std::vector<uint32_t> person_ids;
};

struct RegionAnnotation {
    uint32_t region_id {0};
    SC::Label semantic {SC::Label::Unknown};
    float confidence {0.f};
    size_t slot {0};
    bool semantic_bound {false};
    std::array<double, SC::label_count> role_support {};
    double observed_area {0.};
};

struct ProtectedRoleComponent {
    uint32_t id {0}, person_id {0};
    SC::Label semantic {SC::Label::Unknown};
    float confidence {0.f};
    size_t slot {0};
    std::vector<uint32_t> faces;
};

struct RoleFaceOverride {
    uint32_t face {0}, component_id {0};
    size_t slot {0};
};

struct PortraitRegionAnnotations {
    uint32_t algorithm {algorithm_version};
    std::string partition_digest;
    std::vector<RegionAnnotation> regions;
    std::vector<size_t> face_slots;
    std::vector<ProtectedRoleComponent> protected_components;
    std::vector<RoleFaceOverride> face_overrides;
    // v9 never creates midpoint leaves. The field is explicit so callers can
    // assert the contract before publishing ModelMatchedColors.
    std::vector<AI::ModelSubfaceSlot> subface_slots;

    bool valid_for(const NativePortraitPartition&) const;
};

struct NativeColorMerge {
    uint32_t left_region {0};
    uint32_t right_region {0};
};

struct NativeColorReduction {
    uint32_t algorithm {algorithm_version};
    std::string partition_digest;
    size_t requested_color_count {0};
    size_t actual_color_count {0};
    std::vector<size_t> region_color_groups;
    std::vector<size_t> group_cluster_ids;
    std::vector<NativeColorMerge> merge_order;
    std::string mapping_digest;

    bool valid_for(const NativeWorkbenchPartition&) const;
    bool valid_for(const NativeWorkbenchPartitionIdentity&) const;
};

nlohmann::json encode_native_color_reduction(const NativeColorReduction& reduction);
bool decode_native_color_reduction(
    const nlohmann::json& document,
    const NativeWorkbenchPartitionIdentity& partition,
    NativeColorReduction& reduction,
    std::string& error);

PortraitRegionAnnotations annotate_native_portrait_regions(
    const NativePortraitPartition& partition,
    const SC::Analysis& analysis,
    const BindingOptions& options);

// Builds a deterministic agglomerative hierarchy over immutable atomic
// regions. Semantic evidence never changes this color hierarchy.
NativeColorReduction reduce_native_workbench_partition(
    const NativeWorkbenchPartition& partition,
    size_t target_color_count,
    const PortraitRegionAnnotations* annotations = nullptr,
    bool multiple_people_without_reliable_ids = false,
    const std::vector<uint32_t>& region_person_ids = {});

// Minimum-cost injective assignment. Distinct reduced groups retain distinct
// physical slots even when several groups have the same nearest palette color.
std::vector<size_t> match_reduced_palette_slots(
    const NativeWorkbenchPartitionIdentity& partition,
    const NativeColorReduction& reduction,
    const std::vector<SC::Color>& palette);

// Split vertices only where final cluster colors differ. The ordered face
// sequence and coordinates stay unchanged, preserving surface identity.
bool native_portrait_color_carrier(
    const NativePortraitPartition& partition,
    indexed_triangle_set& mesh,
    std::vector<RGBA>& vertex_colors,
    std::string& error);

// AI::write_model_artifact writes through a sibling .partial file and renames
// only after a complete close, so a failed publication cannot replace input.
bool write_native_portrait_carrier(
    const boost::filesystem::path& destination,
    const NativePortraitPartition& partition,
    const NativeMillimeterBounds& source_bounds_mm,
    std::string& sha256,
    std::string& error);

} // namespace Slic3r::GUI::NativePortrait
