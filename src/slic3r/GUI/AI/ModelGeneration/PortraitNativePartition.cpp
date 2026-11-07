#include "PortraitNativePartition.hpp"

#include "ModelPreviewPalette.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Sha256Digest.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace Slic3r::GUI::NativePortrait {
namespace {

using BuildClock = std::chrono::steady_clock;

uint64_t build_elapsed_ms(const BuildClock::time_point& started)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(BuildClock::now() - started).count());
}

const char* build_stage_status_name(BuildStageStatus status)
{
    switch (status) {
    case BuildStageStatus::Succeeded: return "succeeded";
    case BuildStageStatus::Failed: return "failed";
    case BuildStageStatus::Skipped: return "skipped";
    case BuildStageStatus::NotRun: default: return "not_run";
    }
}

std::optional<BuildStage> build_stage_for_message(const char* message)
{
    if (!message) return std::nullopt;
    const std::string_view value(message);
    if (value.find("Oversampling") != std::string_view::npos) return BuildStage::AdaptiveSubdivision;
    if (value.find("Computing face colors") != std::string_view::npos) return BuildStage::FaceSampling;
    if (value.find("Resampling face colors") != std::string_view::npos) return BuildStage::FaceResampling;
    if (value.find("Color clustering") != std::string_view::npos ||
        value.find("Assigning cluster labels") != std::string_view::npos)
        return BuildStage::ColorClustering;
    if (value.find("Smoothing colors") != std::string_view::npos ||
        value.find("Updating face colors") != std::string_view::npos)
        return BuildStage::RegionSmoothing;
    if (value.find("Repair") != std::string_view::npos || value.find("repair") != std::string_view::npos ||
        value.find("Inspecting") != std::string_view::npos || value.find("Verifying") != std::string_view::npos)
        return BuildStage::StrictRepair;
    return std::nullopt;
}

class BuildStageTracker {
public:
    explicit BuildStageTracker(NativePortraitPartitionBuildReport* report) : m_report(report) {}

    void progress(const char* message)
    {
        const auto next = build_stage_for_message(message);
        if (!next || (m_current && *m_current == *next)) return;
        finish(BuildStageStatus::Succeeded);
        m_current = next;
        m_started = BuildClock::now();
        if (m_report) {
            auto& stage = m_report->stage(*next);
            stage.status = BuildStageStatus::NotRun;
            stage.detail = message ? message : "";
        }
    }

    void finish(BuildStageStatus status)
    {
        if (!m_current) return;
        if (m_report) {
            auto& stage = m_report->stage(*m_current);
            stage.duration_ms += build_elapsed_ms(m_started);
            stage.status = status;
        }
        m_current.reset();
    }

private:
    NativePortraitPartitionBuildReport* m_report {nullptr};
    std::optional<BuildStage> m_current;
    BuildClock::time_point m_started {};
};

struct EdgeRecord {
    uint32_t low {0}, high {0}, face {0};
    friend bool operator<(const EdgeRecord& lhs, const EdgeRecord& rhs)
    {
        return std::tie(lhs.low, lhs.high, lhs.face) < std::tie(rhs.low, rhs.high, rhs.face);
    }
};

class DisjointSet {
public:
    explicit DisjointSet(size_t count) : m_parent(count), m_rank(count, 0)
    {
        std::iota(m_parent.begin(), m_parent.end(), uint32_t(0));
    }

    uint32_t find(uint32_t value)
    {
        uint32_t root = value;
        while (m_parent[root] != root) root = m_parent[root];
        while (m_parent[value] != value) {
            const uint32_t next = m_parent[value];
            m_parent[value] = root;
            value = next;
        }
        return root;
    }

    void join(uint32_t lhs, uint32_t rhs)
    {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) return;
        if (m_rank[lhs] < m_rank[rhs]) std::swap(lhs, rhs);
        m_parent[rhs] = lhs;
        if (m_rank[lhs] == m_rank[rhs]) ++m_rank[lhs];
    }

private:
    std::vector<uint32_t> m_parent;
    std::vector<uint8_t> m_rank;
};

template<class Integer>
bool digest_integer(Sha256Digest& digest, Integer value)
{
    static_assert(std::is_integral_v<Integer>);
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    std::array<unsigned char, sizeof(Integer)> bytes {};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(bits >> (i * 8));
    return digest.update(bytes.data(), bytes.size());
}

bool digest_position(Sha256Digest& digest, float value)
{
    // OBJ round-trips are decimal. A micrometer grid keeps the native face
    // sequence deterministic without making the partition identity depend on
    // harmless text formatting differences.
    if (!std::isfinite(value) || std::abs(double(value)) > 1.e12) return false;
    return digest_integer(digest, int64_t(std::llround(double(value) * 1.e6)));
}

std::string digest_partition(const NativePortraitPartition& partition)
{
    Sha256Digest digest;
    constexpr char schema[] = "orca.native-workbench-partition/v12";
    if (!digest.update(schema, sizeof(schema)) ||
        !digest_integer(digest, uint64_t(partition.painted.indices.size())) ||
        !digest_integer(digest, uint64_t(partition.painted.cluster_colors.size())) ||
        !digest_integer(digest, uint64_t(partition.region_members.size()))) return {};
    for (const auto& face : partition.painted.indices)
        for (int vertex : face) {
            if (vertex < 0 || size_t(vertex) >= partition.painted.vertices.size()) return {};
            for (float value : partition.painted.vertices[size_t(vertex)])
                if (!digest_position(digest, value)) return {};
        }
    for (const auto& color : partition.painted.cluster_colors)
        for (size_t value : color)
            if (!digest_integer(digest, uint16_t(value))) return {};
    for (size_t value : partition.face_cluster_ids)
        if (!digest_integer(digest, uint32_t(value))) return {};
    for (uint32_t value : partition.face_region_ids)
        if (!digest_integer(digest, value)) return {};
    for (const auto& members : partition.region_members) {
        if (!digest_integer(digest, uint64_t(members.size()))) return {};
        for (uint32_t face : members)
            if (!digest_integer(digest, face)) return {};
    }
    for (const auto& neighbors : partition.region_adjacency) {
        if (!digest_integer(digest, uint64_t(neighbors.size()))) return {};
        for (uint32_t region : neighbors)
            if (!digest_integer(digest, region)) return {};
    }
    for (double area : partition.region_areas) {
        if (!std::isfinite(area) || area <= 0.) return {};
        uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(area));
        std::memcpy(&bits, &area, sizeof(bits));
        if (!digest_integer(digest, bits)) return {};
    }
    if (!digest.update(partition.boundary_digest.data(), partition.boundary_digest.size())) return {};
    return digest.final_hex();
}

std::string digest_boundaries(const std::vector<EdgeRecord>& edges,
                              const std::vector<size_t>& clusters,
                              const std::vector<uint32_t>& regions,
                              BoundarySummary& summary)
{
    Sha256Digest digest;
    constexpr char schema[] = "orca.native-workbench-boundaries/v12";
    if (!digest.update(schema, sizeof(schema))) return {};
    size_t begin = 0;
    while (begin < edges.size()) {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].low == edges[begin].low &&
               edges[end].high == edges[begin].high) ++end;
        ++summary.edge_count;
        if (end - begin == 1) ++summary.open_edges;
        if (end - begin > 2) ++summary.nonmanifold_edges;
        bool color_boundary = false;
        for (size_t i = begin + 1; i < end; ++i)
            color_boundary = color_boundary || clusters[edges[i].face] != clusters[edges[begin].face];
        if (color_boundary) ++summary.color_boundary_edges;
        bool boundary = end - begin != 2 || regions[edges[begin].face] != regions[edges[begin + 1].face];
        if (boundary) {
            ++summary.boundary_edges;
            if (!digest_integer(digest, edges[begin].low) || !digest_integer(digest, edges[begin].high) ||
                !digest_integer(digest, uint32_t(end - begin))) return {};
            for (size_t i = begin; i < end; ++i)
                if (!digest_integer(digest, edges[i].face) ||
                    !digest_integer(digest, uint32_t(clusters[edges[i].face])) ||
                    !digest_integer(digest, regions[edges[i].face])) return {};
        }
        begin = end;
    }
    if (!digest_integer(digest, uint64_t(summary.edge_count)) ||
        !digest_integer(digest, uint64_t(summary.boundary_edges)) ||
        !digest_integer(digest, uint64_t(summary.color_boundary_edges)) ||
        !digest_integer(digest, uint64_t(summary.open_edges)) ||
        !digest_integer(digest, uint64_t(summary.nonmanifold_edges))) return {};
    return digest.final_hex();
}

SC::Label normalized_label(SC::Label label)
{
    if (label == SC::Label::BodySkin) return SC::Label::FaceSkin;
    if (label == SC::Label::MouthInterior) return SC::Label::Lips;
    return label;
}

double triangle_area(const PaintedMesh& mesh, size_t face)
{
    const auto& indices = mesh.indices[face];
    const auto point = [&](int corner) {
        const auto& value = mesh.vertices[size_t(indices[corner])];
        return Vec3d(value[0], value[1], value[2]);
    };
    return 0.5 * (point(1) - point(0)).cross(point(2) - point(0)).norm();
}

size_t nearest_palette_slot(const std::array<size_t, 3>& color, const std::vector<SC::Color>& palette)
{
    std::vector<SC::Color> centers;
    centers.reserve(palette.size());
    for (const auto& candidate : palette) centers.push_back(PreviewPalette::to_lab(candidate));
    const SC::Color rgb {float(color[0]) / 255.f, float(color[1]) / 255.f, float(color[2]) / 255.f};
    return PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(rgb), centers);
}

std::string digest_color_reduction(const NativeColorReduction& reduction)
{
    Sha256Digest digest;
    constexpr char schema[] = "orca.native-workbench-color-reduction/v12";
    if (!digest.update(schema, sizeof(schema)) ||
        !digest.update(reduction.partition_digest.data(), reduction.partition_digest.size()) ||
        !digest_integer(digest, uint64_t(reduction.requested_color_count)) ||
        !digest_integer(digest, uint64_t(reduction.actual_color_count))) return {};
    for (size_t group : reduction.region_color_groups)
        if (!digest_integer(digest, uint32_t(group))) return {};
    for (size_t cluster : reduction.group_cluster_ids)
        if (!digest_integer(digest, uint32_t(cluster))) return {};
    for (const auto& merge : reduction.merge_order)
        if (!digest_integer(digest, merge.left_region) ||
            !digest_integer(digest, merge.right_region)) return {};
    return digest.final_hex();
}

struct ReductionGroupState {
    uint32_t root {0};
    std::vector<uint32_t> regions;
    double area {0.};
    PreviewPalette::Color weighted_lab {};
};

ReductionGroupState make_reduction_group(const NativeWorkbenchPartition& partition,
                                         std::vector<uint32_t> regions)
{
    ReductionGroupState group;
    std::sort(regions.begin(), regions.end());
    group.root = regions.front();
    group.regions = std::move(regions);
    for (uint32_t region : group.regions) {
        const double area = partition.region_areas[region];
        const auto& rgb8 = partition.painted.cluster_colors[partition.region_cluster_ids[region]];
        const PreviewPalette::Color rgb {float(rgb8[0]) / 255.f, float(rgb8[1]) / 255.f,
            float(rgb8[2]) / 255.f};
        const auto lab = PreviewPalette::to_lab(rgb);
        group.area += area;
        for (size_t channel = 0; channel < group.weighted_lab.size(); ++channel)
            group.weighted_lab[channel] += float(area * double(lab[channel]));
    }
    if (group.area > 0.)
        for (float& value : group.weighted_lab) value = float(double(value) / group.area);
    return group;
}

size_t representative_cluster(const NativeWorkbenchPartition& partition,
                              const ReductionGroupState& group,
                              const PortraitRegionAnnotations* annotations)
{
    struct Weight { double semantic_area {0.}, confidence_area {0.}, total_area {0.}; };
    std::map<size_t, Weight> weights;
    for (uint32_t region : group.regions) {
        const size_t cluster = partition.region_cluster_ids[region];
        auto& weight = weights[cluster];
        const double area = partition.region_areas[region];
        weight.total_area += area;
        if (annotations && annotations->regions[region].semantic_bound) {
            weight.semantic_area += area;
            weight.confidence_area += area * annotations->regions[region].confidence;
        }
    }
    size_t winner = weights.begin()->first;
    auto score = weights.begin()->second;
    for (const auto& [cluster, value] : weights) {
        const auto candidate = std::tie(value.semantic_area, value.confidence_area, value.total_area);
        const auto best = std::tie(score.semantic_area, score.confidence_area, score.total_area);
        if (candidate > best || (candidate == best && cluster < winner)) {
            winner = cluster;
            score = value;
        }
    }
    return winner;
}

template<class Vertices>
NativeMillimeterBounds bounds_from_vertices(const Vertices& vertices)
{
    NativeMillimeterBounds bounds;
    if (vertices.empty()) return bounds;
    bounds.minimum.fill(std::numeric_limits<double>::infinity());
    bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    for (const auto& vertex : vertices) for (int axis = 0; axis < 3; ++axis) {
        const double value = double(vertex[axis]);
        if (!std::isfinite(value)) return {};
        bounds.minimum[axis] = std::min(bounds.minimum[axis], value);
        bounds.maximum[axis] = std::max(bounds.maximum[axis], value);
    }
    return bounds;
}

} // namespace

const char* native_portrait_build_stage_name(BuildStage stage)
{
    switch (stage) {
    case BuildStage::Decode: return "decode";
    case BuildStage::SourceCheck: return "source_check";
    case BuildStage::AdaptiveSubdivision: return "adaptive_subdivision";
    case BuildStage::FaceSampling: return "face_sampling";
    case BuildStage::StrictRepair: return "strict_repair";
    case BuildStage::FaceResampling: return "face_resampling";
    case BuildStage::ColorClustering: return "color_clustering";
    case BuildStage::RegionSmoothing: return "region_smoothing";
    case BuildStage::RegionEnumeration: return "region_enumeration";
    case BuildStage::CarrierPublish: return "carrier_publish";
    case BuildStage::AppearancePublish: return "appearance_publish";
    case BuildStage::CachePublish: return "cache_publish";
    case BuildStage::MetadataPublish: return "metadata_publish";
    case BuildStage::PreviewSwitch: return "preview_switch";
    case BuildStage::Count: default: return "unknown";
    }
}

nlohmann::json encode_native_portrait_partition_build_report(
    const NativePortraitPartitionBuildReport& report)
{
    nlohmann::json stages = nlohmann::json::object();
    for (size_t index = 0; index < size_t(BuildStage::Count); ++index) {
        const auto stage = BuildStage(index);
        const auto& value = report.stages[index];
        stages[native_portrait_build_stage_name(stage)] = {
            {"status", build_stage_status_name(value.status)},
            {"duration_ms", value.duration_ms},
            {"detail", value.detail}};
    }
    const auto topology = [](const StrictMeshTopologyReport& value) {
        return nlohmann::json{{"open_edges", value.open_edges},
            {"nonmanifold_edges", value.nonmanifold_edges},
            {"nonmanifold_vertices", value.nonmanifold_vertices},
            {"degenerate_faces", value.degenerate_faces},
            {"duplicate_faces", value.duplicate_faces},
            {"inconsistent_edges", value.inconsistent_edges},
            {"self_intersections_checked", value.self_intersections_checked},
            {"self_intersects", value.self_intersects}};
    };
    const auto& strict = report.strict_repair;
    return {{"schema", "orca.native-workbench-partition-build-report/v1"},
        {"success", report.success}, {"error", report.error}, {"total_ms", report.total_ms},
        {"stages", std::move(stages)},
        {"strict_repair", {{"success", strict.success}, {"canceled", strict.canceled},
            {"first_cgal_attempted", strict.first_cgal_attempted},
            {"enhanced_repair_attempted", strict.enhanced_repair_attempted},
            {"second_cgal_attempted", strict.second_cgal_attempted},
            {"geometry_verified", strict.geometry_verified},
            {"sampled_deviation", strict.sampled_deviation},
            {"sampled_deviation_ratio", strict.sampled_deviation_ratio},
            {"area_change_ratio", strict.area_change_ratio}, {"strategy", strict.strategy},
            {"error", strict.error}, {"input_topology", topology(strict.input_topology)},
            {"output_topology", topology(strict.output_topology)},
            {"timings", {{"input_check_ms", strict.timings.input_check_ms},
                {"first_cgal_ms", strict.timings.first_cgal_ms},
                {"enhanced_repair_ms", strict.timings.enhanced_repair_ms},
                {"second_cgal_ms", strict.timings.second_cgal_ms},
                {"final_check_ms", strict.timings.final_check_ms},
                {"geometry_validation_ms", strict.timings.geometry_validation_ms},
                {"total_ms", strict.timings.total_ms}}}}}};
}

bool NativeMillimeterBounds::valid() const
{
    double diagonal_squared = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            maximum[axis] < minimum[axis]) return false;
        const double extent = maximum[axis] - minimum[axis];
        diagonal_squared += extent * extent;
    }
    return std::isfinite(diagonal_squared) && diagonal_squared > 0.0;
}

NativeMillimeterBounds native_workbench_bounds(const TexturedMesh& mesh)
{
    return bounds_from_vertices(mesh.vertices);
}

NativeMillimeterBounds native_workbench_bounds(const NativeWorkbenchPartition& partition)
{
    return bounds_from_vertices(partition.painted.vertices);
}

NativeMillimeterBounds native_workbench_bounds(const indexed_triangle_set& mesh)
{
    return bounds_from_vertices(mesh.vertices);
}

bool native_workbench_bounds_compatible(const NativeMillimeterBounds& source,
                                        const NativeMillimeterBounds& candidate,
                                        std::string& error,
                                        double maximum_deviation_ratio)
{
    error.clear();
    if (!source.valid() || !candidate.valid() || !std::isfinite(maximum_deviation_ratio) ||
        maximum_deviation_ratio < 0.0 || maximum_deviation_ratio > 0.1) {
        error = "Native workbench millimeter bounds are invalid.";
        return false;
    }
    double diagonal_squared = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = source.maximum[axis] - source.minimum[axis];
        diagonal_squared += extent * extent;
    }
    const double tolerance = std::max(1e-5, std::sqrt(diagonal_squared) * maximum_deviation_ratio);
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(source.minimum[axis] - candidate.minimum[axis]) > tolerance ||
            std::abs(source.maximum[axis] - candidate.maximum[axis]) > tolerance) {
            error = "Native workbench carrier size does not match the source millimeter frame.";
            return false;
        }
    }
    return true;
}

nlohmann::json encode_native_workbench_bounds(const NativeMillimeterBounds& bounds)
{
    if (!bounds.valid()) throw std::invalid_argument("Invalid native workbench millimeter bounds.");
    return {{"minimum", bounds.minimum}, {"maximum", bounds.maximum}};
}

bool decode_native_workbench_bounds(const nlohmann::json& document,
                                    NativeMillimeterBounds& bounds)
{
    try {
        if (!document.is_object()) return false;
        NativeMillimeterBounds decoded;
        decoded.minimum = document.at("minimum").get<std::array<double, 3>>();
        decoded.maximum = document.at("maximum").get<std::array<double, 3>>();
        if (!decoded.valid()) return false;
        bounds = decoded;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool NativeWorkbenchPartition::valid() const
{
    const size_t faces = painted.indices.size();
    if (faces == 0 || faces > maximum_face_count || painted.vertices.empty() ||
        painted.face_colors.size() != faces || painted.cluster_colors.size() < minimum_color_count ||
        painted.cluster_colors.size() > maximum_color_count || face_cluster_ids.size() != faces ||
        face_region_ids.size() != faces || region_members.empty() ||
        region_members.size() != region_cluster_ids.size() ||
        region_members.size() != region_adjacency.size() ||
        region_members.size() != region_areas.size() || geometry_id.size() != 64 ||
        partition_digest.size() != 64 ||
        boundary_digest.size() != 64 || boundary.open_edges != 0 ||
        boundary.nonmanifold_edges != 0) return false;
    std::vector<uint8_t> seen(faces, 0);
    for (size_t region = 0; region < region_members.size(); ++region) {
        if (region_members[region].empty() || region_cluster_ids[region] >= painted.cluster_colors.size() ||
            !std::isfinite(region_areas[region]) || region_areas[region] <= 0.) return false;
        if (!std::is_sorted(region_adjacency[region].begin(), region_adjacency[region].end()) ||
            std::adjacent_find(region_adjacency[region].begin(), region_adjacency[region].end()) !=
                region_adjacency[region].end()) return false;
        for (uint32_t neighbor : region_adjacency[region]) {
            if (neighbor >= region_members.size() || neighbor == region ||
                !std::binary_search(region_adjacency[neighbor].begin(),
                    region_adjacency[neighbor].end(), uint32_t(region))) return false;
        }
        for (uint32_t face : region_members[region]) {
            if (face >= faces || seen[face] || face_region_ids[face] != region ||
                face_cluster_ids[face] != region_cluster_ids[region]) return false;
            seen[face] = 1;
        }
    }
    if (std::find(seen.begin(), seen.end(), uint8_t(0)) != seen.end()) return false;
    for (size_t face = 0; face < faces; ++face) {
        if (face_cluster_ids[face] >= painted.cluster_colors.size() ||
            painted.face_colors[face] != painted.cluster_colors[face_cluster_ids[face]]) return false;
        for (int vertex : painted.indices[face])
            if (vertex < 0 || size_t(vertex) >= painted.vertices.size()) return false;
    }
    return true;
}

bool NativeWorkbenchPartitionIdentity::valid() const
{
    if (algorithm != algorithm_version || geometry_id.size() != 64 || face_count == 0 ||
        face_count > maximum_face_count || cluster_colors.size() < minimum_color_count ||
        cluster_colors.size() > maximum_color_count || face_cluster_ids.size() != face_count ||
        face_region_ids.size() != face_count || region_cluster_ids.empty() ||
        region_adjacency.size() != region_cluster_ids.size() ||
        region_areas.size() != region_cluster_ids.size() ||
        partition_digest.size() != 64 || boundary_digest.size() != 64 ||
        boundary.open_edges != 0 || boundary.nonmanifold_edges != 0) return false;
    for (const auto& color : cluster_colors)
        if (color[0] > 255 || color[1] > 255 || color[2] > 255) return false;
    std::vector<uint8_t> seen(region_cluster_ids.size(), 0);
    for (size_t region = 0; region < region_cluster_ids.size(); ++region) {
        if (region_cluster_ids[region] >= cluster_colors.size() ||
            !std::isfinite(region_areas[region]) || region_areas[region] <= 0. ||
            !std::is_sorted(region_adjacency[region].begin(), region_adjacency[region].end()) ||
            std::adjacent_find(region_adjacency[region].begin(), region_adjacency[region].end()) !=
                region_adjacency[region].end()) return false;
        for (uint32_t neighbor : region_adjacency[region])
            if (neighbor >= region_cluster_ids.size() || neighbor == region ||
                !std::binary_search(region_adjacency[neighbor].begin(),
                    region_adjacency[neighbor].end(), uint32_t(region))) return false;
    }
    for (size_t face = 0; face < face_count; ++face) {
        if (face_cluster_ids[face] >= cluster_colors.size() ||
            face_region_ids[face] >= region_cluster_ids.size() ||
            region_cluster_ids[face_region_ids[face]] != face_cluster_ids[face]) return false;
        seen[face_region_ids[face]] = 1;
    }
    return std::find(seen.begin(), seen.end(), uint8_t(0)) == seen.end();
}

PaintMeshRepairCallback native_mesh_repair_callback(StrictMeshRepairResult* strict_result)
{
    return [strict_result](const indexed_triangle_set& mesh, indexed_triangle_set& repaired_mesh,
              std::function<void(const char*, unsigned)> progress,
              std::function<bool()> cancel, std::string* error) {
        StrictMeshRepairOptions options;
        options.reject_geometry_change = true;
        auto result = strict_repair_mesh(mesh, options, progress, cancel);
        if (!result.success) {
            if (error) *error = result.error.empty() ? "Native portrait strict repair failed." : result.error;
            if (strict_result) *strict_result = std::move(result);
            return false;
        }
        repaired_mesh = std::move(result.repaired_mesh);
        if (strict_result) *strict_result = std::move(result);
        return true;
    };
}

NativeWorkbenchPartition build_native_workbench_partition(
    const TexturedMesh& texture_mesh,
    PaintMeshRepairCallback repair, const PaintCancelCallback& cancel,
    NativePortraitPartitionBuildReport* report)
{
    const auto total_started = BuildClock::now();
    BuildStageTracker tracker(report);
    auto finish_report = [&](bool success, const std::string& error = {}) {
        tracker.finish(success ? BuildStageStatus::Succeeded : BuildStageStatus::Failed);
        if (!report) return;
        report->total_ms += build_elapsed_ms(total_started);
        report->success = success;
        report->error = error;
        for (BuildStage stage : {BuildStage::AdaptiveSubdivision, BuildStage::FaceSampling,
             BuildStage::StrictRepair, BuildStage::FaceResampling, BuildStage::ColorClustering,
             BuildStage::RegionSmoothing}) {
            auto& value = report->stage(stage);
            if (value.status == BuildStageStatus::NotRun && success)
                value.status = BuildStageStatus::Skipped;
        }
    };
    try {
    if (cancel && cancel()) throw std::runtime_error("Native portrait partition canceled.");
    if (!repair)
        throw std::invalid_argument("Native portrait partition requires a strict mesh repair callback.");
    TexturePaintingSettings settings;
    settings.target_colors_num = 0;
    settings.min_cluster_k = default_model_color_count;
    settings.max_cluster_k = maximum_color_count;
    settings.max_color_distance = 10.0;
    settings.max_tail_color_distance = 18.0;
    settings.smooth_weight = 0.5;
    settings.oversampling_iters = 0;
    settings.fixed_palette.clear();
    settings.fixed_mapping_palette.clear();
    settings.face_color_overrides.clear();
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    settings.mesh_repair_callback = std::move(repair);
    settings.strict_mesh_repair = true;
    PaintedMesh painted;
    const bool success = texture_mesh.precomputed_face_colors.empty()
        ? texture_to_painting(texture_mesh, painted, settings,
            [&](int, const char* message) { tracker.progress(message); }, cancel)
        : face_colors_to_painting(texture_mesh, painted, settings,
            [&](int, const char* message) { tracker.progress(message); }, cancel);
    if (!success) {
        if (cancel && cancel()) throw std::runtime_error("Native portrait partition canceled.");
        throw std::runtime_error("Orca native texture partition failed.");
    }
    tracker.finish(BuildStageStatus::Succeeded);
    const auto enumeration_started = BuildClock::now();
    try {
        auto partition = finalize_native_workbench_partition(std::move(painted), cancel);
        if (report) {
            auto& enumeration = report->stage(BuildStage::RegionEnumeration);
            enumeration.status = BuildStageStatus::Succeeded;
            enumeration.duration_ms += build_elapsed_ms(enumeration_started);
        }
        finish_report(true);
        return partition;
    } catch (...) {
        if (report) {
            auto& enumeration = report->stage(BuildStage::RegionEnumeration);
            enumeration.status = BuildStageStatus::Failed;
            enumeration.duration_ms += build_elapsed_ms(enumeration_started);
        }
        throw;
    }
    } catch (const std::exception& failure) {
        finish_report(false, failure.what());
        throw;
    }
}

NativeWorkbenchPartition build_native_portrait_partition(
    const TexturedMesh& texture_mesh, size_t target_color_count,
    PaintMeshRepairCallback repair, const PaintCancelCallback& cancel,
    NativePortraitPartitionBuildReport* report)
{
    if (target_color_count < minimum_color_count || target_color_count > maximum_color_count)
        throw std::invalid_argument("Native workbench color count must be between one and thirty-two.");
    return build_native_workbench_partition(texture_mesh, std::move(repair), cancel, report);
}

NativeWorkbenchPartition finalize_native_workbench_partition(PaintedMesh painted, const PaintCancelCallback& cancel)
{
    const size_t face_count = painted.indices.size();
    if (face_count == 0 || face_count > maximum_face_count || painted.vertices.empty() ||
        painted.face_colors.size() != face_count ||
        painted.cluster_colors.size() < minimum_color_count ||
        painted.cluster_colors.size() > maximum_color_count)
        throw std::invalid_argument("Native portrait painted mesh is incomplete.");

    // TextureToColor reports display RGB centers. Distinct high-k centers may
    // quantize to the same RGB triplet; those are not physically distinguishable
    // model colors and must share one canonical native cluster.
    std::vector<std::array<size_t, 3>> unique_clusters;
    std::map<std::array<size_t, 3>, size_t> unique_cluster_ids;
    for (const auto& color : painted.cluster_colors) {
        if (color[0] > 255 || color[1] > 255 || color[2] > 255)
            throw std::invalid_argument("Native workbench cluster center is invalid.");
        if (unique_cluster_ids.emplace(color, unique_clusters.size()).second)
            unique_clusters.push_back(color);
    }
    painted.cluster_colors = std::move(unique_clusters);

    NativePortraitPartition result;
    result.painted = std::move(painted);
    std::map<std::array<size_t, 3>, size_t> cluster_ids;
    for (size_t cluster = 0; cluster < result.painted.cluster_colors.size(); ++cluster) {
        const auto& color = result.painted.cluster_colors[cluster];
        if (!cluster_ids.emplace(color, cluster).second)
            throw std::invalid_argument("Native workbench cluster centers are duplicated.");
    }
    result.face_cluster_ids.reserve(face_count);
    std::vector<EdgeRecord> edges;
    edges.reserve(face_count * 3);
    for (size_t face = 0; face < face_count; ++face) {
        if ((face & 4095) == 0 && cancel && cancel())
            throw std::runtime_error("Native portrait partition canceled.");
        const auto found = cluster_ids.find(result.painted.face_colors[face]);
        if (found == cluster_ids.end())
            throw std::invalid_argument("Native portrait face color is not a cluster center.");
        result.face_cluster_ids.push_back(found->second);
        const auto& triangle = result.painted.indices[face];
        for (int corner = 0; corner < 3; ++corner) {
            const int first = triangle[corner], second = triangle[(corner + 1) % 3];
            if (first < 0 || second < 0 || size_t(first) >= result.painted.vertices.size() ||
                size_t(second) >= result.painted.vertices.size() || first == second)
                throw std::invalid_argument("Native portrait mesh contains invalid triangle indices.");
            edges.push_back({uint32_t(std::min(first, second)), uint32_t(std::max(first, second)), uint32_t(face)});
        }
    }
    std::sort(edges.begin(), edges.end());

    DisjointSet connected(face_count);
    size_t begin = 0;
    while (begin < edges.size()) {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].low == edges[begin].low &&
               edges[end].high == edges[begin].high) ++end;
        std::array<uint32_t, maximum_color_count> first_by_cluster;
        first_by_cluster.fill(std::numeric_limits<uint32_t>::max());
        for (size_t i = begin; i < end; ++i) {
            const uint32_t face = edges[i].face;
            const size_t cluster = result.face_cluster_ids[face];
            if (first_by_cluster[cluster] == std::numeric_limits<uint32_t>::max()) first_by_cluster[cluster] = face;
            else connected.join(first_by_cluster[cluster], face);
        }
        begin = end;
    }

    result.face_region_ids.resize(face_count);
    std::unordered_map<uint32_t, uint32_t> root_regions;
    root_regions.reserve(face_count);
    for (uint32_t face = 0; face < face_count; ++face) {
        const uint32_t root = connected.find(face);
        auto [found, inserted] = root_regions.emplace(root, uint32_t(result.region_members.size()));
        if (inserted) {
            result.region_members.emplace_back();
            result.region_cluster_ids.push_back(result.face_cluster_ids[face]);
            result.region_areas.push_back(0.);
        }
        result.face_region_ids[face] = found->second;
        result.region_members[found->second].push_back(face);
        result.region_areas[found->second] += triangle_area(result.painted, face);
    }
    result.region_adjacency.resize(result.region_members.size());
    begin = 0;
    while (begin < edges.size()) {
        size_t end = begin + 1;
        while (end < edges.size() && edges[end].low == edges[begin].low &&
               edges[end].high == edges[begin].high) ++end;
        if (end - begin == 2) {
            const uint32_t first = result.face_region_ids[edges[begin].face];
            const uint32_t second = result.face_region_ids[edges[begin + 1].face];
            if (first != second) {
                result.region_adjacency[first].push_back(second);
                result.region_adjacency[second].push_back(first);
            }
        }
        begin = end;
    }
    for (auto& neighbors : result.region_adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    result.boundary_digest = digest_boundaries(edges, result.face_cluster_ids,
        result.face_region_ids, result.boundary);
    if (result.boundary_digest.empty()) throw std::runtime_error("Cannot identify native portrait boundaries.");
    indexed_triangle_set geometry;
    geometry.vertices.reserve(result.painted.vertices.size());
    geometry.indices.reserve(result.painted.indices.size());
    for (const auto& vertex : result.painted.vertices)
        geometry.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : result.painted.indices)
        geometry.indices.emplace_back(face[0], face[1], face[2]);
    result.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(geometry);
    result.partition_digest = digest_partition(result);
    if (result.partition_digest.empty() || !result.valid())
        throw std::runtime_error("Native portrait partition validation failed.");
    return result;
}

NativeWorkbenchPartition finalize_native_portrait_partition(PaintedMesh painted,
                                                             const PaintCancelCallback& cancel)
{
    return finalize_native_workbench_partition(std::move(painted), cancel);
}

NativeWorkbenchPartitionIdentity native_workbench_partition_identity(
    const NativeWorkbenchPartition& partition)
{
    if (!partition.valid()) throw std::invalid_argument("Native portrait partition is invalid.");
    NativeWorkbenchPartitionIdentity identity;
    identity.geometry_id = partition.geometry_id;
    identity.face_count = partition.painted.indices.size();
    identity.cluster_colors = partition.painted.cluster_colors;
    identity.face_cluster_ids = partition.face_cluster_ids;
    identity.face_region_ids = partition.face_region_ids;
    identity.region_cluster_ids = partition.region_cluster_ids;
    identity.region_adjacency = partition.region_adjacency;
    identity.region_areas = partition.region_areas;
    identity.boundary = partition.boundary;
    identity.partition_digest = partition.partition_digest;
    identity.boundary_digest = partition.boundary_digest;
    return identity;
}

NativePortraitPartitionIdentity native_portrait_partition_identity(
    const NativePortraitPartition& partition)
{
    return native_workbench_partition_identity(partition);
}

nlohmann::json encode_native_portrait_partition_identity(const NativePortraitPartitionIdentity& identity)
{
    if (!identity.valid()) throw std::invalid_argument("Native portrait partition identity is invalid.");
    return {{"schema", "orca.native-workbench-partition-identity/v12"},
        {"algorithm", identity.algorithm}, {"geometry_id", identity.geometry_id},
        {"face_count", identity.face_count}, {"cluster_colors", identity.cluster_colors},
        {"face_cluster_ids", identity.face_cluster_ids}, {"face_region_ids", identity.face_region_ids},
        {"region_cluster_ids", identity.region_cluster_ids},
        {"region_adjacency", identity.region_adjacency},
        {"region_areas", identity.region_areas},
        {"boundary", {{"edge_count", identity.boundary.edge_count},
            {"boundary_edges", identity.boundary.boundary_edges},
            {"color_boundary_edges", identity.boundary.color_boundary_edges},
            {"open_edges", identity.boundary.open_edges},
            {"nonmanifold_edges", identity.boundary.nonmanifold_edges}}},
        {"partition_digest", identity.partition_digest}, {"boundary_digest", identity.boundary_digest}};
}

bool decode_native_portrait_partition_identity(const nlohmann::json& document,
                                                NativePortraitPartitionIdentity& identity,
                                                std::string& error)
{
    error.clear();
    try {
        if (!document.is_object() || document.at("schema") != "orca.native-workbench-partition-identity/v12")
            throw std::invalid_argument("Native workbench partition identity schema is unsupported or stale.");
        NativePortraitPartitionIdentity decoded;
        decoded.algorithm = document.at("algorithm").get<uint32_t>();
        decoded.geometry_id = document.at("geometry_id").get<std::string>();
        decoded.face_count = document.at("face_count").get<size_t>();
        decoded.cluster_colors = document.at("cluster_colors").get<decltype(decoded.cluster_colors)>();
        decoded.face_cluster_ids = document.at("face_cluster_ids").get<decltype(decoded.face_cluster_ids)>();
        decoded.face_region_ids = document.at("face_region_ids").get<decltype(decoded.face_region_ids)>();
        decoded.region_cluster_ids = document.at("region_cluster_ids").get<decltype(decoded.region_cluster_ids)>();
        decoded.region_adjacency = document.at("region_adjacency").get<decltype(decoded.region_adjacency)>();
        decoded.region_areas = document.at("region_areas").get<decltype(decoded.region_areas)>();
        const auto& boundary = document.at("boundary");
        decoded.boundary.edge_count = boundary.at("edge_count").get<size_t>();
        decoded.boundary.boundary_edges = boundary.at("boundary_edges").get<size_t>();
        decoded.boundary.color_boundary_edges = boundary.at("color_boundary_edges").get<size_t>();
        decoded.boundary.open_edges = boundary.at("open_edges").get<size_t>();
        decoded.boundary.nonmanifold_edges = boundary.at("nonmanifold_edges").get<size_t>();
        decoded.partition_digest = document.at("partition_digest").get<std::string>();
        decoded.boundary_digest = document.at("boundary_digest").get<std::string>();
        if (!decoded.valid()) throw std::invalid_argument("Native portrait partition identity is invalid.");
        identity = std::move(decoded);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

NativeWorkbenchPartition restore_native_workbench_partition(
    const indexed_triangle_set& mesh, const NativeWorkbenchPartitionIdentity& identity,
    std::string& error)
{
    error.clear();
    NativeWorkbenchPartition result;
    if (!identity.valid() || mesh.indices.size() != identity.face_count) {
        error = "Native portrait partition identity does not match the carrier.";
        return result;
    }
    result.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    if (result.geometry_id != identity.geometry_id) {
        error = "Native portrait carrier geometry changed.";
        return {};
    }
    result.painted.vertices.reserve(mesh.vertices.size());
    result.painted.indices.reserve(mesh.indices.size());
    for (const auto& vertex : mesh.vertices)
        result.painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : mesh.indices)
        result.painted.indices.push_back({face[0], face[1], face[2]});
    result.painted.cluster_colors = identity.cluster_colors;
    result.face_cluster_ids = identity.face_cluster_ids;
    result.face_region_ids = identity.face_region_ids;
    result.region_cluster_ids = identity.region_cluster_ids;
    result.region_adjacency = identity.region_adjacency;
    result.region_areas = identity.region_areas;
    result.painted.face_colors.reserve(identity.face_count);
    for (size_t cluster : result.face_cluster_ids)
        result.painted.face_colors.push_back(result.painted.cluster_colors[cluster]);
    result.region_members.resize(result.region_cluster_ids.size());
    for (uint32_t face = 0; face < result.face_region_ids.size(); ++face)
        result.region_members[result.face_region_ids[face]].push_back(face);
    result.boundary = identity.boundary;
    result.boundary_digest = identity.boundary_digest;
    result.partition_digest = digest_partition(result);
    if (result.partition_digest != identity.partition_digest || !result.valid()) {
        error = "Native portrait partition digest changed.";
        return {};
    }
    return result;
}

NativePortraitPartition restore_native_portrait_partition(
    const indexed_triangle_set& mesh, const NativePortraitPartitionIdentity& identity,
    std::string& error)
{
    return restore_native_workbench_partition(mesh, identity, error);
}

bool PortraitRegionAnnotations::valid_for(const NativePortraitPartition& partition) const
{
    if (!partition.valid() || algorithm != algorithm_version || partition_digest != partition.partition_digest ||
        regions.size() != partition.region_members.size() || face_slots.size() != partition.painted.indices.size() ||
        !subface_slots.empty()) return false;
    for (size_t region = 0; region < regions.size(); ++region) {
        if (regions[region].region_id != region || !std::isfinite(regions[region].confidence) ||
            regions[region].confidence < 0.f || regions[region].confidence > 1.f ||
            regions[region].slot >= maximum_color_count ||
            !std::isfinite(regions[region].observed_area) || regions[region].observed_area < 0.) return false;
        for (double support : regions[region].role_support)
            if (!std::isfinite(support) || support < 0.) return false;
    }
    std::vector<size_t> expected(face_slots.size());
    for (size_t face = 0; face < expected.size(); ++face)
        expected[face] = regions[partition.face_region_ids[face]].slot;
    std::vector<uint8_t> seen(expected.size(), 0);
    std::vector<uint32_t> owners(expected.size(), UINT32_MAX);
    for (size_t id = 0; id < protected_components.size(); ++id) {
        const auto& component = protected_components[id];
        if (component.id != id || component.faces.empty() ||
            component.slot >= maximum_color_count ||
            !(component.semantic == SC::Label::Hair || component.semantic == SC::Label::Eyebrow ||
              component.semantic == SC::Label::EyeSclera || component.semantic == SC::Label::Iris ||
              component.semantic == SC::Label::Lips) ||
            !std::isfinite(component.confidence) || component.confidence < minimum_semantic_confidence ||
            component.confidence > 1.f) return false;
        for (uint32_t face : component.faces) {
            if (face >= expected.size() || seen[face]++) return false;
            owners[face] = uint32_t(id);
            expected[face] = component.slot;
        }
    }
    size_t override_count = 0;
    uint32_t previous = 0;
    for (const auto& overlay : face_overrides) {
        if (overlay.face >= expected.size() || (override_count && overlay.face <= previous) ||
            overlay.component_id >= protected_components.size() || !seen[overlay.face] ||
            owners[overlay.face] != overlay.component_id ||
            overlay.slot != protected_components[overlay.component_id].slot ||
            overlay.slot != expected[overlay.face]) return false;
        previous = overlay.face; ++override_count;
    }
    if (override_count != std::count(seen.begin(), seen.end(), uint8_t(1)) || expected != face_slots) return false;
    return true;
}

PortraitRegionAnnotations annotate_native_portrait_regions(
    const NativePortraitPartition& partition, const SC::Analysis& analysis,
    const BindingOptions& options)
{
    if (!partition.valid()) throw std::invalid_argument("Native portrait partition is invalid.");
    if (options.target_palette.size() < minimum_color_count || options.target_palette.size() > maximum_color_count ||
        !std::isfinite(options.minimum_confidence) || options.minimum_confidence < 0.f ||
        options.minimum_confidence > 1.f)
        throw std::invalid_argument("Native portrait binding options are invalid.");
    for (const auto& [label, slot] : options.semantic_slots)
        if (label == SC::Label::Unknown || slot >= options.target_palette.size())
            throw std::invalid_argument("Native portrait semantic slot is invalid.");
    const size_t face_count = partition.painted.indices.size();
    if (!options.person_ids.empty() && options.person_ids.size() != face_count)
        throw std::invalid_argument("Native portrait person identities do not match faces.");
    const bool semantic_available = analysis.face_labels.size() == face_count &&
        analysis.face_confidence.size() == face_count;
    if ((!analysis.face_labels.empty() || !analysis.face_confidence.empty()) && !semantic_available)
        throw std::invalid_argument("Native portrait semantics do not match the fixed partition.");

    PortraitRegionAnnotations result;
    result.partition_digest = partition.partition_digest;
    result.face_slots.resize(face_count);
    result.regions.reserve(partition.region_members.size());
    for (size_t region = 0; region < partition.region_members.size(); ++region) {
        const size_t cluster = partition.region_cluster_ids[region];
        const size_t fallback = nearest_palette_slot(partition.painted.cluster_colors[cluster], options.target_palette);
        if (fallback >= options.target_palette.size())
            throw std::runtime_error("Native portrait nearest-color fallback failed.");
        RegionAnnotation annotation;
        annotation.region_id = uint32_t(region);
        annotation.slot = fallback;
        if (semantic_available) {
            std::array<double, SC::label_count> scores {};
            double total_area = 0.;
            for (uint32_t face : partition.region_members[region]) {
                const double area = triangle_area(partition.painted, face);
                if (!std::isfinite(area) || area <= 0.) continue;
                total_area += area;
                const float confidence = analysis.face_confidence[face];
                const SC::Label label = normalized_label(analysis.face_labels[face]);
                if (label != SC::Label::Unknown && size_t(label) < scores.size() &&
                    std::isfinite(confidence) && confidence >= 0.f && confidence <= 1.f)
                    scores[size_t(label)] += area * confidence;
            }
            size_t winner = 0;
            double best = 0.;
            bool tied = false;
            for (size_t label = 1; label < scores.size(); ++label) {
                if (scores[label] > best) {
                    winner = label;
                    best = scores[label];
                    tied = false;
                } else if (scores[label] > 0. && std::abs(scores[label] - best) <=
                    std::numeric_limits<double>::epsilon() * std::max(1., total_area) * 16.) {
                    tied = true;
                }
            }
            annotation.confidence = total_area > 0. ? float(std::clamp(best / total_area, 0., 1.)) : 0.f;
            annotation.role_support = scores;
            annotation.observed_area = total_area;
            const auto slot = winner < scores.size()
                ? options.semantic_slots.find(SC::Label(winner)) : options.semantic_slots.end();
            if (!tied && winner != 0 && annotation.confidence >= options.minimum_confidence &&
                slot != options.semantic_slots.end()) {
                annotation.semantic = SC::Label(winner);
                annotation.slot = slot->second;
                annotation.semantic_bound = true;
            }
        }
        for (uint32_t face : partition.region_members[region]) result.face_slots[face] = annotation.slot;
        result.regions.push_back(annotation);
    }
    // Protect evidence on its original faces. A large mixed native region is
    // never allowed to outvote a smaller, independently observed feature.
    if (semantic_available) {
        const auto protected_role = [](SC::Label label) {
            return label == SC::Label::Hair || label == SC::Label::Eyebrow ||
                label == SC::Label::EyeSclera || label == SC::Label::Iris || label == SC::Label::Lips;
        };
        std::vector<SC::Label> labels(face_count, SC::Label::Unknown);
        for (size_t face = 0; face < face_count; ++face) {
            const auto label = normalized_label(analysis.face_labels[face]);
            const float confidence = analysis.face_confidence[face];
            if (protected_role(label) &&
                std::isfinite(confidence) && confidence >= std::max(options.minimum_confidence, minimum_semantic_confidence) && confidence <= 1.f)
                labels[face] = label;
        }
        DisjointSet components(face_count);
        std::vector<EdgeRecord> edges;
        for (uint32_t face = 0; face < face_count; ++face) if (labels[face] != SC::Label::Unknown) {
            const auto& triangle = partition.painted.indices[face];
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t a = uint32_t(triangle[corner]), b = uint32_t(triangle[(corner + 1) % 3]);
                edges.push_back({std::min(a,b), std::max(a,b), face});
            }
        }
        std::sort(edges.begin(), edges.end());
        for (size_t begin = 0; begin < edges.size();) {
            size_t end = begin + 1;
            while (end < edges.size() && edges[end].low == edges[begin].low && edges[end].high == edges[begin].high) ++end;
            if (end - begin == 2) {
                const auto a = edges[begin].face, b = edges[begin + 1].face;
                if (labels[a] == labels[b] && (options.person_ids.empty() || options.person_ids[a] == options.person_ids[b]))
                    components.join(a,b);
            }
            begin = end;
        }
        std::map<uint32_t, uint32_t> ids;
        for (uint32_t face = 0; face < face_count; ++face) if (labels[face] != SC::Label::Unknown) {
            const auto [it, inserted] = ids.emplace(components.find(face), uint32_t(result.protected_components.size()));
            if (inserted) {
                ProtectedRoleComponent component;
                component.id = it->second;
                component.person_id = options.person_ids.empty() ? 0 : options.person_ids[face];
                component.semantic = labels[face];
                const auto role_slot = options.semantic_slots.find(labels[face]);
                component.slot = role_slot != options.semantic_slots.end() ? role_slot->second :
                    result.regions[partition.face_region_ids[face]].slot;
                component.confidence = 1.f;
                result.protected_components.push_back(std::move(component));
            }
            auto& component = result.protected_components[it->second];
            component.faces.push_back(face);
            component.confidence = std::min(component.confidence, analysis.face_confidence[face]);
            result.face_overrides.push_back({face, component.id, component.slot});
            result.face_slots[face] = component.slot;
        }
    }
    if (!result.valid_for(partition)) throw std::runtime_error("Native portrait annotation validation failed.");
    return result;
}

bool NativeColorReduction::valid_for(const NativeWorkbenchPartition& partition) const
{
    if (!partition.valid() || algorithm != algorithm_version ||
        partition_digest != partition.partition_digest || requested_color_count < minimum_color_count ||
        requested_color_count > maximum_color_count || actual_color_count == 0 ||
        actual_color_count > requested_color_count ||
        region_color_groups.size() != partition.region_members.size() ||
        group_cluster_ids.size() != actual_color_count || mapping_digest.size() != 64)
        return false;
    std::map<size_t, size_t> cluster_groups;
    for (size_t region = 0; region < region_color_groups.size(); ++region) {
        const auto inserted = cluster_groups.emplace(partition.region_cluster_ids[region], region_color_groups[region]);
        if (!inserted.second && inserted.first->second != region_color_groups[region]) return false;
    }
    if (actual_color_count != std::min(requested_color_count, cluster_groups.size())) return false;
    std::vector<uint8_t> used(actual_color_count, 0);
    for (size_t group : region_color_groups) {
        if (group >= actual_color_count) return false;
        used[group] = 1;
    }
    if (std::find(used.begin(), used.end(), uint8_t(0)) != used.end()) return false;
    for (size_t cluster : group_cluster_ids)
        if (cluster >= partition.painted.cluster_colors.size()) return false;
    for (const auto& merge : merge_order)
        if (merge.left_region >= partition.region_members.size() ||
            merge.right_region >= partition.region_members.size() ||
            merge.left_region == merge.right_region) return false;
    return digest_color_reduction(*this) == mapping_digest;
}

bool NativeColorReduction::valid_for(const NativeWorkbenchPartitionIdentity& partition) const
{
    if (!partition.valid() || algorithm != algorithm_version ||
        partition_digest != partition.partition_digest || requested_color_count < minimum_color_count ||
        requested_color_count > maximum_color_count || actual_color_count == 0 ||
        actual_color_count > requested_color_count ||
        region_color_groups.size() != partition.region_cluster_ids.size() ||
        group_cluster_ids.size() != actual_color_count || mapping_digest.size() != 64)
        return false;
    std::map<size_t, size_t> cluster_groups;
    for (size_t region = 0; region < region_color_groups.size(); ++region) {
        const auto inserted = cluster_groups.emplace(partition.region_cluster_ids[region], region_color_groups[region]);
        if (!inserted.second && inserted.first->second != region_color_groups[region]) return false;
    }
    if (actual_color_count != std::min(requested_color_count, cluster_groups.size())) return false;
    std::vector<uint8_t> used(actual_color_count, 0);
    for (size_t group : region_color_groups) {
        if (group >= actual_color_count) return false;
        used[group] = 1;
    }
    if (std::find(used.begin(), used.end(), uint8_t(0)) != used.end()) return false;
    for (size_t cluster : group_cluster_ids)
        if (cluster >= partition.cluster_colors.size()) return false;
    for (const auto& merge : merge_order)
        if (merge.left_region >= partition.region_cluster_ids.size() ||
            merge.right_region >= partition.region_cluster_ids.size() ||
            merge.left_region == merge.right_region) return false;
    return digest_color_reduction(*this) == mapping_digest;
}

nlohmann::json encode_native_color_reduction(const NativeColorReduction& reduction)
{
    nlohmann::json merges = nlohmann::json::array();
    for (const auto& merge : reduction.merge_order)
        merges.push_back({merge.left_region, merge.right_region});
    return {{"schema", "orca.native-workbench-color-reduction/v12"},
        {"algorithm", reduction.algorithm},
        {"partition_digest", reduction.partition_digest},
        {"requested_color_count", reduction.requested_color_count},
        {"actual_color_count", reduction.actual_color_count},
        {"region_color_groups", reduction.region_color_groups},
        {"group_cluster_ids", reduction.group_cluster_ids},
        {"merge_order", std::move(merges)},
        {"mapping_digest", reduction.mapping_digest}};
}

bool decode_native_color_reduction(const nlohmann::json& document,
                                   const NativeWorkbenchPartitionIdentity& partition,
                                   NativeColorReduction& reduction,
                                   std::string& error)
{
    try {
        if (!partition.valid() || !document.is_object() ||
            document.at("schema") != "orca.native-workbench-color-reduction/v12")
            throw std::invalid_argument("Native workbench color reduction schema is unsupported or stale.");
        const auto bounded = [](const nlohmann::json& value, uint64_t maximum) {
            if (!value.is_number_integer())
                throw std::invalid_argument("Native workbench color reduction contains a negative index.");
            const auto signed_value = value.get<int64_t>();
            if (signed_value < 0)
                throw std::invalid_argument("Native workbench color reduction contains a negative index.");
            const auto decoded = uint64_t(signed_value);
            if (decoded > maximum)
                throw std::invalid_argument("Native workbench color reduction index is out of range.");
            return decoded;
        };
        NativeColorReduction decoded;
        decoded.algorithm = uint32_t(bounded(document.at("algorithm"), algorithm_version));
        decoded.partition_digest = document.at("partition_digest").get<std::string>();
        decoded.requested_color_count = size_t(bounded(
            document.at("requested_color_count"), maximum_color_count));
        decoded.actual_color_count = size_t(bounded(
            document.at("actual_color_count"), maximum_color_count));
        if (!document.at("region_color_groups").is_array() ||
            document.at("region_color_groups").size() != partition.region_cluster_ids.size() ||
            !document.at("group_cluster_ids").is_array() ||
            document.at("group_cluster_ids").size() != decoded.actual_color_count ||
            !document.at("merge_order").is_array() ||
            document.at("merge_order").size() > partition.region_cluster_ids.size())
            throw std::invalid_argument("Native workbench color reduction array sizes are invalid.");
        for (const auto& group : document.at("region_color_groups"))
            decoded.region_color_groups.push_back(size_t(bounded(
                group, decoded.actual_color_count ? decoded.actual_color_count - 1 : 0)));
        for (const auto& cluster : document.at("group_cluster_ids"))
            decoded.group_cluster_ids.push_back(size_t(bounded(
                cluster, partition.cluster_colors.size() - 1)));
        for (const auto& row : document.at("merge_order")) {
            if (!row.is_array() || row.size() != 2)
                throw std::invalid_argument("Native workbench color reduction merge is invalid.");
            decoded.merge_order.push_back({uint32_t(bounded(row[0],
                partition.region_cluster_ids.size() - 1)), uint32_t(bounded(row[1],
                partition.region_cluster_ids.size() - 1))});
        }
        decoded.mapping_digest = document.at("mapping_digest").get<std::string>();
        if (!decoded.valid_for(partition))
            throw std::invalid_argument("Native workbench color reduction identity is invalid.");
        reduction = std::move(decoded);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        reduction = {};
        error = failure.what();
        return false;
    }
}

NativeColorReduction reduce_native_workbench_partition(
    const NativeWorkbenchPartition& partition, size_t target_color_count,
    const PortraitRegionAnnotations* annotations,
    bool multiple_people_without_reliable_ids,
    const std::vector<uint32_t>& region_person_ids)
{
    if (!partition.valid()) throw std::invalid_argument("Native workbench partition is invalid.");
    if (target_color_count < minimum_color_count || target_color_count > maximum_color_count)
        throw std::invalid_argument("Native workbench target color count must be between one and thirty-two.");
    if (annotations && !annotations->valid_for(partition))
        throw std::invalid_argument("Native workbench semantic annotations are stale.");
    if (!region_person_ids.empty() && region_person_ids.size() != partition.region_members.size())
        throw std::invalid_argument("Native workbench person ids do not match atomic regions.");

    const size_t region_count = partition.region_members.size();
    DisjointSet initial(region_count);
    std::map<size_t, uint32_t> first_cluster_region;
    for (uint32_t region = 0; region < region_count; ++region) {
        const size_t cluster = partition.region_cluster_ids[region];
        auto [found, inserted] = first_cluster_region.emplace(cluster, region);
        if (!inserted) initial.join(found->second, region);
    }

    (void) multiple_people_without_reliable_ids;

    std::map<uint32_t, std::vector<uint32_t>> component_regions;
    for (uint32_t region = 0; region < region_count; ++region)
        component_regions[initial.find(region)].push_back(region);
    std::vector<ReductionGroupState> groups;
    groups.reserve(component_regions.size());
    for (auto& [root, regions] : component_regions) {
        (void) root;
        groups.push_back(make_reduction_group(partition, std::move(regions)));
    }
    std::sort(groups.begin(), groups.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.root < rhs.root;
    });

    NativeColorReduction result;
    result.partition_digest = partition.partition_digest;
    result.requested_color_count = target_color_count;
    std::vector<ReductionGroupState> selected_groups;
    while (groups.size() > 1) {
        if (selected_groups.empty() && groups.size() <= target_color_count)
            selected_groups = groups;
        size_t best_left = 0, best_right = 1;
        float best_distance = PreviewPalette::distance(groups[0].weighted_lab, groups[1].weighted_lab);
        std::pair<uint32_t, uint32_t> best_key = std::minmax(groups[0].root, groups[1].root);
        for (size_t left = 0; left < groups.size(); ++left) {
            for (size_t right = left + 1; right < groups.size(); ++right) {
                const float distance = PreviewPalette::distance(
                    groups[left].weighted_lab, groups[right].weighted_lab);
                const std::pair<uint32_t, uint32_t> key =
                    std::minmax(groups[left].root, groups[right].root);
                if (distance < best_distance || (distance == best_distance && key < best_key)) {
                    best_distance = distance;
                    best_key = key;
                    best_left = left;
                    best_right = right;
                }
            }
        }
        result.merge_order.push_back({best_key.first, best_key.second});
        auto regions = groups[best_left].regions;
        regions.insert(regions.end(), groups[best_right].regions.begin(), groups[best_right].regions.end());
        const auto merged = make_reduction_group(partition, std::move(regions));
        groups.erase(groups.begin() + best_right);
        groups.erase(groups.begin() + best_left);
        groups.push_back(merged);
        std::sort(groups.begin(), groups.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.root < rhs.root;
        });
    }
    if (selected_groups.empty()) selected_groups = groups;

    result.actual_color_count = selected_groups.size();
    result.region_color_groups.assign(region_count, 0);
    result.group_cluster_ids.reserve(selected_groups.size());
    for (size_t group = 0; group < selected_groups.size(); ++group) {
        for (uint32_t region : selected_groups[group].regions) result.region_color_groups[region] = group;
        result.group_cluster_ids.push_back(
            representative_cluster(partition, selected_groups[group], nullptr));
    }
    result.mapping_digest = digest_color_reduction(result);
    if (!result.valid_for(partition))
        throw std::runtime_error("Native workbench color reduction validation failed.");
    return result;
}

std::vector<size_t> match_reduced_palette_slots(
    const NativeWorkbenchPartitionIdentity& partition,
    const NativeColorReduction& reduction, const std::vector<SC::Color>& palette)
{
    if (!reduction.valid_for(partition) || palette.size() < reduction.actual_color_count ||
        palette.size() > maximum_color_count)
        throw std::invalid_argument("Reduced color groups cannot be assigned to the palette.");
    const size_t rows = reduction.actual_color_count, columns = palette.size();
    std::vector<SC::Color> targets;
    for (const auto& color : palette) targets.push_back(PreviewPalette::to_lab(color));
    std::vector<std::vector<double>> cost(rows, std::vector<double>(columns));
    std::vector<double> group_area(rows, 0.);
    for (size_t region = 0; region < partition.region_cluster_ids.size(); ++region) {
        const size_t row = reduction.region_color_groups[region];
        const double area = partition.region_areas[region];
        const auto& color = partition.cluster_colors[partition.region_cluster_ids[region]];
        const auto lab = PreviewPalette::to_lab({float(color[0])/255.f,float(color[1])/255.f,float(color[2])/255.f});
        group_area[row] += area;
        for (size_t column = 0; column < columns; ++column)
            cost[row][column] += area * PreviewPalette::distance(lab, targets[column]);
    }
    // Each preserved color group gets one equal vote in the global assignment.
    // Within that group, all native colors contribute by their surface area;
    // the largest representative alone can hide a nearly equal skin component.
    for (size_t row = 0; row < rows; ++row) {
        if (!std::isfinite(group_area[row]) || group_area[row] <= 0.)
            throw std::invalid_argument("Reduced color group area is invalid.");
        for (double& value : cost[row]) value /= group_area[row];
    }
    // Rectangular Hungarian assignment; stable ascending indices break ties.
    std::vector<double> u(rows + 1), v(columns + 1);
    std::vector<size_t> p(columns + 1), way(columns + 1);
    for (size_t row = 1; row <= rows; ++row) {
        p[0] = row;
        size_t column = 0;
        std::vector<double> minimum(columns + 1, std::numeric_limits<double>::infinity());
        std::vector<uint8_t> used(columns + 1, 0);
        do {
            used[column] = 1;
            const size_t current_row = p[column];
            double delta = std::numeric_limits<double>::infinity();
            size_t next = 0;
            for (size_t candidate = 1; candidate <= columns; ++candidate) if (!used[candidate]) {
                const double value = cost[current_row - 1][candidate - 1] - u[current_row] - v[candidate];
                if (value < minimum[candidate]) { minimum[candidate] = value; way[candidate] = column; }
                if (minimum[candidate] < delta) { delta = minimum[candidate]; next = candidate; }
            }
            for (size_t candidate = 0; candidate <= columns; ++candidate)
                if (used[candidate]) { u[p[candidate]] += delta; v[candidate] -= delta; }
                else minimum[candidate] -= delta;
            column = next;
        } while (p[column]);
        do {
            const size_t previous = way[column]; p[column] = p[previous]; column = previous;
        } while (column);
    }
    std::vector<size_t> result(rows);
    for (size_t column = 1; column <= columns; ++column)
        if (p[column]) result[p[column] - 1] = column - 1;
    return result;
}

bool native_portrait_color_carrier(const NativePortraitPartition& partition,
                                   indexed_triangle_set& mesh,
                                   std::vector<RGBA>& vertex_colors,
                                   std::string& error)
{
    error.clear();
    mesh = {};
    vertex_colors.clear();
    if (!partition.valid()) { error = "Native portrait partition is invalid."; return false; }
    std::unordered_map<uint64_t, uint32_t> split_vertices;
    split_vertices.reserve(std::min(partition.painted.indices.size() * 3,
        partition.painted.vertices.size() * partition.painted.cluster_colors.size()));
    mesh.vertices.reserve(partition.painted.vertices.size());
    mesh.indices.reserve(partition.painted.indices.size());
    vertex_colors.reserve(partition.painted.vertices.size());
    for (size_t face = 0; face < partition.painted.indices.size(); ++face) {
        const auto& rgb = partition.painted.face_colors[face];
        const RGBA color {float(rgb[0]) / 255.f, float(rgb[1]) / 255.f, float(rgb[2]) / 255.f, 1.f};
        stl_triangle_vertex_indices output;
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t source = uint32_t(partition.painted.indices[face][corner]);
            const uint64_t key = (uint64_t(source) << 32) | uint64_t(partition.face_cluster_ids[face]);
            auto [found, inserted] = split_vertices.emplace(key, uint32_t(mesh.vertices.size()));
            if (inserted) {
                const auto& vertex = partition.painted.vertices[source];
                mesh.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
                vertex_colors.push_back(color);
            } else if (vertex_colors[found->second] != color) {
                error = "Native portrait color boundary still shares a vertex.";
                mesh = {}; vertex_colors.clear(); return false;
            }
            output[corner] = int(found->second);
        }
        mesh.indices.push_back(output);
    }
    if (AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh) != partition.geometry_id) {
        error = "Native portrait carrier changed the final face sequence.";
        mesh = {}; vertex_colors.clear(); return false;
    }
    return true;
}

bool write_native_portrait_carrier(const boost::filesystem::path& destination,
                                   const NativePortraitPartition& partition,
                                   const NativeMillimeterBounds& source_bounds_mm,
                                   std::string& sha256, std::string& error)
{
    sha256.clear();
    if (AI::model_artifact_format(destination) != "obj") {
        error = "Native portrait carrier must be an OBJ.";
        return false;
    }
    if (!native_workbench_bounds_compatible(
            source_bounds_mm, native_workbench_bounds(partition), error)) return false;
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    if (!native_portrait_color_carrier(partition, mesh, colors, error) ||
        !AI::write_model_artifact(destination, mesh, colors, error)) return false;
    TriangleMesh verified;
    ObjInfo verified_colors;
    if (!AI::load_model_artifact(destination, verified, verified_colors, error) ||
        AI::SurfaceSelectionPersistence::geometry_fingerprint(verified.its) != partition.geometry_id ||
        !native_workbench_bounds_compatible(
            source_bounds_mm, native_workbench_bounds(verified.its), error)) {
        boost::system::error_code ignored;
        boost::filesystem::remove(destination, ignored);
        if (error.empty()) error = "Native portrait carrier read-back validation failed.";
        return false;
    }
    sha256 = AI::model_artifact_sha256(destination);
    if (sha256.empty()) {
        error = "Native portrait carrier could not be verified.";
        return false;
    }
    return true;
}

} // namespace Slic3r::GUI::NativePortrait
