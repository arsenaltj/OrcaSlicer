#pragma once

#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "libslic3r/Model.hpp"
#include <limits>

namespace Slic3r::GUI::LocalPrintColorApplication {
struct PreparedPainting {
    TriangleSelector::TriangleSplittingData data;
    std::string geometry_id;
    int base_extruder {1};
};

// Orca's 3MF loader recenters vertex coordinates and compensates the volume
// transform. Keep the same ordered faces, winding and vertex connectivity;
// permit only the loader's bounding-box centering and bounded float roundoff.
// Texture import may weld UV seams before unit conversion, while the workbench
// converts first. Float rounding can therefore change the vertex count without
// changing any ordered triangle. That route checks corners, not vertex aliases.
inline bool same_surface_partition(const indexed_triangle_set& source, const indexed_triangle_set& target,
                                   bool require_vertex_bijection = true)
{
    if (source.indices.empty() || source.indices.size() != target.indices.size() ||
        (require_vertex_bijection && source.vertices.size() != target.vertices.size())) return false;
    const size_t missing = std::numeric_limits<size_t>::max();
    std::vector<size_t> forward(require_vertex_bijection ? source.vertices.size() : 0, missing),
                        reverse(require_vertex_bijection ? target.vertices.size() : 0, missing);
    if (source.vertices.empty()) return false;
    Vec3d minimum = source.vertices.front().cast<double>(), maximum = minimum;
    for (const auto& vertex : source.vertices) {
        if (!vertex.allFinite()) return false;
        minimum = minimum.cwiseMin(vertex.cast<double>()); maximum = maximum.cwiseMax(vertex.cast<double>());
    }
    const Vec3d center = 0.5 * (minimum + maximum);
    const Vec3f native_shift = center.cast<float>();
    bool unchanged = true, recentered = true;
    for (size_t f = 0; f < source.indices.size(); ++f) for (int corner = 0; corner < 3; ++corner) {
        const int a = source.indices[f][corner], b = target.indices[f][corner];
        if (a < 0 || b < 0 || size_t(a) >= source.vertices.size() || size_t(b) >= target.vertices.size()) return false;
        if (require_vertex_bijection) {
            if ((forward[a] != missing && forward[a] != size_t(b)) || (reverse[b] != missing && reverse[b] != size_t(a))) return false;
            forward[a] = size_t(b); reverse[b] = size_t(a);
        }
        const Vec3d from = source.vertices[a].cast<double>(), to = target.vertices[b].cast<double>();
        if (!from.allFinite() || !to.allFinite()) return false;
        const double scale = std::max({1.0, from.cwiseAbs().maxCoeff(), to.cwiseAbs().maxCoeff()});
        const double tolerance = std::min(1e-4, 8.0 * std::numeric_limits<float>::epsilon() * scale);
        unchanged = unchanged && (to - from).cwiseAbs().maxCoeff() <= tolerance;
        const Vec3d native_centered = (source.vertices[a] - native_shift).cast<double>();
        recentered = recentered && (to - native_centered).cwiseAbs().maxCoeff() <= tolerance;
        if (!unchanged && !recentered) return false;
    }
    return true;
}

// Prepare all native face annotations before the host takes its undo snapshot.
// No RGB lookup, mesh replacement, texture quantization or project mutation.
inline bool prepare(const TriangleMesh& mesh, const AI::LocalPrintColorResult& result,
    const std::string& materials, const std::string& process, PreparedPainting& destination, std::string& error)
{
    if (!result.valid(error)) return false;
    if (!result.confirmed) { error = "Confirm the color result before applying it."; return false; }
    if (result.material_fingerprint != materials || result.process_fingerprint != process) {
        error = "Materials or process changed; recompute the color result."; return false;
    }
    if (mesh.its.indices.size() != result.face_count || result.face_count > size_t(std::numeric_limits<int>::max()) ||
        AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its) != result.geometry_id) {
        error = "The surface partition belongs to different geometry."; return false;
    }
    std::vector<EnforcerBlockerType> states;
    const size_t max_slot = int(EnforcerBlockerType::ExtruderMax) - int(EnforcerBlockerType::Extruder1);
    size_t base_slot = max_slot;
    for (const auto& target : result.targets) {
        if (!target.executable || !target.physical_slot || target.recipe || *target.physical_slot > max_slot) {
            error = "Direct application requires a valid physical assignment for every target."; return false;
        }
        base_slot = std::min(base_slot, *target.physical_slot);
        states.push_back(static_cast<EnforcerBlockerType>(int(EnforcerBlockerType::Extruder1) + int(*target.physical_slot)));
    }
    TriangleSelector selector(mesh);
    for (size_t face = 0; face < result.face_count; ++face) selector.set_facet(int(face), states[result.face_targets[face]]);
    PreparedPainting prepared;
    prepared.data = selector.serialize();
    prepared.geometry_id = result.geometry_id;
    prepared.base_extruder = int(base_slot) + 1;
    destination = std::move(prepared);
    return true;
}

inline bool prepare_from_source(const TriangleMesh& mesh, const indexed_triangle_set& source,
    const AI::LocalPrintColorResult& result, const std::string& materials, const std::string& process,
    PreparedPainting& destination, std::string& error)
{
    if (AI::SurfaceSelectionPersistence::geometry_fingerprint(source) != result.geometry_id ||
        !same_surface_partition(source, mesh.its)) {
        error = "The surface partition belongs to different geometry."; return false;
    }
    auto native_result = result;
    native_result.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    return prepare(mesh, native_result, materials, process, destination, error);
}

// Compare semantic painting after canonical serialization, since 3MF loading
// may rebuild its bitstream bookkeeping. A local edit must not be mistaken for
// an unchanged saved confirmation. This function never mutates the volume.
inline bool matches_saved_painting(const TriangleMesh& mesh, const indexed_triangle_set& source,
    const TriangleSelector::TriangleSplittingData& actual, const AI::LocalPrintColorResult& saved)
{
    PreparedPainting expected; std::string error;
    if (!prepare_from_source(mesh, source, saved, saved.material_fingerprint, saved.process_fingerprint, expected, error))
        return false;
    TriangleSelector selector(mesh);
    selector.deserialize(actual);
    return selector.serialize() == expected.data;
}

// Call on the main thread inside the host's transaction, after final identity
// checks. Other volume annotations and all object transforms stay untouched.
inline bool apply(ModelVolume& volume, PreparedPainting&& prepared, std::string& error)
{
    if (AI::SurfaceSelectionPersistence::geometry_fingerprint(volume.mesh().its) != prepared.geometry_id) {
        error = "Model geometry changed before the color transaction."; return false;
    }
    volume.config.set("extruder", prepared.base_extruder);
    volume.mmu_segmentation_facets.set_data(std::move(prepared.data));
    return true;
}
} // namespace Slic3r::GUI::LocalPrintColorApplication
