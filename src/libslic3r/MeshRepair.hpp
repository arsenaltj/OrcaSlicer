#pragma once

#include "TriangleMesh.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r {

class TriangleMesh;

struct MeshRepairStats
{
    std::size_t welded_vertices    = 0;
    std::size_t removed_triangles  = 0;
    std::size_t reversed_shells    = 0;
    std::size_t filled_holes       = 0;
    std::size_t removed_shells     = 0;
    std::size_t self_intersections = 0;
    // False when the input already was closed, consistently oriented and free of self-intersections.
    bool        modified           = false;
};

// Returns false, leaving the mesh untouched, if no solid can be built. progress gets the percentage done and may
// throw to cancel. See docs/HLSD/mesh-repair.md.
bool repair_mesh(TriangleMesh &mesh, MeshRepairStats *stats = nullptr, std::string *error = nullptr,
                 const std::function<void(int)> &progress = {});

struct StrictMeshTopologyReport
{
    std::size_t open_edges = 0;
    std::size_t nonmanifold_edges = 0;
    std::size_t nonmanifold_vertices = 0;
    std::size_t degenerate_faces = 0;
    std::size_t duplicate_faces = 0;
    std::size_t inconsistent_edges = 0;
    bool self_intersections_checked = false;
    bool self_intersects = false;

    bool safe() const
    {
        return open_edges == 0 && nonmanifold_edges == 0 && nonmanifold_vertices == 0 &&
               degenerate_faces == 0 && duplicate_faces == 0 && inconsistent_edges == 0 &&
               self_intersections_checked && !self_intersects;
    }
};

// Inspection facts remain strict. Only an actual native execution may grant
// OrcaNative acceptance; callers must retain its source/output identities.
enum class MeshRepairAcceptance { Strict, OrcaNative };

// Ordered triangle coordinates, independent of carrier-only vertex splitting.
// Empty on invalid/non-finite geometry. Used only for native acceptance proofs.
std::string mesh_repair_geometry_id(const indexed_triangle_set& mesh);

struct StrictMeshRepairTimings
{
    std::uint64_t input_check_ms = 0;
    std::uint64_t first_cgal_ms = 0;
    std::uint64_t enhanced_repair_ms = 0;
    std::uint64_t second_cgal_ms = 0;
    std::uint64_t final_check_ms = 0;
    std::uint64_t geometry_validation_ms = 0;
    std::uint64_t total_ms = 0;
};

struct StrictMeshRepairOptions
{
    double maximum_sampled_deviation_ratio = 0.002;
    double maximum_area_change_ratio = 0.02;
    // Automatic publication requires verified geometry as well as topology.
    bool reject_geometry_change = true;
    // Opt in only for an explicit native-compatible workflow. The native
    // repair must succeed and its actual float output must be inspected;
    // strict safety and native acceptance remain distinct recorded facts.
    bool allow_native_repair_geometry_change = false;
    MeshRepairAcceptance acceptance = MeshRepairAcceptance::Strict;
    // Optional immutable pre-sampling geometry. Only native fallback reads it;
    // the existing optimized successful path is unchanged.
    const indexed_triangle_set* native_source = nullptr;
};

struct StrictMeshRepairResult
{
    bool success = false;
    bool canceled = false;
    bool resource_failure = false;
    bool first_cgal_attempted = false;
    bool enhanced_repair_attempted = false;
    bool second_cgal_attempted = false;
    bool geometry_verified = false;
    bool native_repair_compatible = false;
    MeshRepairAcceptance acceptance = MeshRepairAcceptance::Strict;
    bool native_repair_succeeded = false;
    std::string native_source_geometry_id;
    std::string native_output_geometry_id;
    indexed_triangle_set repaired_mesh;
    StrictMeshTopologyReport input_topology;
    StrictMeshTopologyReport output_topology;
    RepairedMeshErrors cgal_errors;
    MeshRepairStats enhanced_stats;
    double sampled_deviation = 0.0;
    double sampled_deviation_ratio = 0.0;
    double area_change_ratio = 0.0;
    std::string strategy;
    std::string error;
    StrictMeshRepairTimings timings;

    bool accepted() const {
        return success && !canceled && (output_topology.safe() ||
            (acceptance == MeshRepairAcceptance::OrcaNative && native_repair_succeeded &&
             native_source_geometry_id.size() == 64 && native_output_geometry_id.size() == 64));
    }
};

// Strict whole-mesh repair used by automatic workflows. Every fallback starts
// from the immutable input. Success certifies topology and either the strict
// geometry limits or the explicit native-compatible result (recorded separately).
// Cancellation or an unverifiable output leaves no mesh.
StrictMeshRepairResult strict_repair_mesh(
    const indexed_triangle_set &mesh,
    const StrictMeshRepairOptions &options = {},
    const std::function<void(const char *message, unsigned progress)> &progress = {},
    const std::function<bool()> &canceled = {});

} // namespace Slic3r
