#include "ModelFeatureSnapshot.hpp"

#include <limits>

namespace Slic3r::AI::SmartSlicing {
namespace {

size_t saturating_add(size_t lhs, size_t rhs)
{
    return rhs > std::numeric_limits<size_t>::max() - lhs ? std::numeric_limits<size_t>::max() : lhs + rhs;
}

size_t saturating_multiply(size_t lhs, size_t rhs)
{
    return lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs ?
               std::numeric_limits<size_t>::max() : lhs * rhs;
}

} // namespace

const char* feature_availability_name(FeatureAvailability availability)
{
    switch (availability) {
    case FeatureAvailability::Known: return "known";
    case FeatureAvailability::Unknown: return "unknown";
    case FeatureAvailability::Unavailable: return "unavailable";
    }
    return "unavailable";
}

const char* mesh_closedness_name(MeshClosedness closedness)
{
    switch (closedness) {
    case MeshClosedness::Closed: return "closed";
    case MeshClosedness::Open: return "open";
    case MeshClosedness::NonManifold: return "non_manifold";
    }
    return "non_manifold";
}

const char* overhang_severity_name(OverhangSeverity severity)
{
    switch (severity) {
    case OverhangSeverity::Severe: return "severe";
    case OverhangSeverity::Moderate: return "moderate";
    case OverhangSeverity::Mild: return "mild";
    }
    return "mild";
}

size_t triangle_mesh_snapshot_bytes(const TriangleMeshSnapshot& mesh)
{
    size_t bytes = saturating_multiply(mesh.vertices.size(), sizeof(MeshPoint3d));
    bytes = saturating_add(bytes, saturating_multiply(mesh.triangles.size(), sizeof(MeshTriangle)));
    bytes = saturating_add(bytes, mesh.geometry_fingerprint.size());
    return bytes;
}

} // namespace Slic3r::AI::SmartSlicing
