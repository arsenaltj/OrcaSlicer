"""Select unchanged desktop repair-only definitions; never rewrite algorithms."""
from pathlib import Path
import hashlib, json, sys
source, output = map(Path, sys.argv[1:3])
text = source.read_text(encoding="utf-8").replace("\r\n", "\n")
expected = "9adf158ee49c0d46306f55accf777b87781b4371d383f87770f72d43b5ac2b48"
if hashlib.sha256(text.encode()).hexdigest() != expected:
    raise SystemExit("Desktop MeshBoolean changed; review repair definitions and source anchors")
def section(begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise SystemExit("Ambiguous repair-only source anchor")
    return text[text.index(begin):text.index(end)]
chunks = [
    ("diagnostics_and_exact_query", section("namespace {\n\nconstexpr size_t invalid_repair_metric", "using MapMatrixXfUnaligned")),
    ("cgal_repair_helpers", section("namespace cgal {", "void save_CGALMesh(")),
    ("geometry_conversion", section("template<class _Mesh> void triangle_mesh_to_cgal(const TriangleMesh& M", "// /////////////////////////////////////////////////////////////////////////////\n// Isotropic remeshing")),
    ("actual_cgal_repair_and_float_validation", section("bool does_self_intersect(const TriangleMesh &mesh)", "} // namespace cgal")),
]
headers = """// Generated repair-only compilation of the pinned desktop source.
// Every selected definition is copied unchanged; no unrelated segmentation/MCUT/GUI entrypoints.
#include "libslic3r/Exception.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/PolygonSoupPreparation.hpp"
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Cartesian_converter.h>
#include <CGAL/boost/graph/helpers.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <boost/log/trivial.hpp>
#include <tbb/task_arena.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <type_traits>
namespace Slic3r { namespace MeshBoolean {
"""
output.parent.mkdir(parents=True, exist_ok=True)
generated = headers + "\n".join(body for _, body in chunks) + "\n} } }\n"
if not output.exists() or output.read_text(encoding="utf-8") != generated:
    output.write_text(generated, encoding="utf-8")
output.with_suffix(".sources.json").write_text(json.dumps({
    "source": str(source), "sourceCommit": "323c6c1c9ca7e5f94c7b7161af64584af0a69f3c",
    "normalizedSourceSha256": expected,
    "sections": [{"name": name, "sha256": hashlib.sha256(body.encode()).hexdigest(), "bytes": len(body.encode())} for name, body in chunks]
}, indent=2) + "\n", encoding="utf-8")

def emit(name, header, parts, suffix):
    destination = output.parent / name
    content = header + "\n".join(body for _, body in parts) + suffix
    if not destination.exists() or destination.read_text(encoding="utf-8") != content:
        destination.write_text(content, encoding="utf-8")
    return [{"name": n, "sha256": hashlib.sha256(body.encode()).hexdigest(), "bytes": len(body.encode())} for n, body in parts]
solid_parts = [
    ("eigen_conversion", section("using MapMatrixXfUnaligned", "void minus(EigenMesh &A")),
    ("actual_igl_self_union", section("void self_union(EigenMesh &A)", "namespace cgal {")),
]
solid_audit = emit("MeshBooleanSolidUnion.cpp", """// Pinned desktop IGL/CGAL fallback; never a stub or a replacement algorithm.
#include <igl/MeshBooleanType.h>
#include <igl/copyleft/cgal/mesh_boolean.h>
#undef L
#include "libslic3r/MeshBoolean.hpp"
namespace Slic3r { namespace MeshBoolean {
""", solid_parts, "\n} }\n")
def local_sections(filename, expected_sha, definitions):
    data = source.with_name(filename).read_text(encoding="utf-8").replace("\r\n", "\n")
    if hashlib.sha256(data.encode()).hexdigest() != expected_sha:
        raise SystemExit("Desktop " + filename + " changed; review selected definitions")
    result = []
    for name, begin, end in definitions:
        if data.count(begin) != 1 or data.count(end) != 1:
            raise SystemExit("Ambiguous geometry-only source anchor: " + name)
        result.append((name, data[data.index(begin):data.index(end)]))
    return result
triangle_parts = local_sections("TriangleMesh.cpp", "ff43d4bce733056ab3c99c3b55514ee27e9b65ca6fd7ca058aa56f4cf3a2af61", [
    ('stats_and_its_constructors', 'static void update_bounding_box', '// #define SLIC3R_TRACE_REPAIR'),
    ('volume', 'float TriangleMesh::volume()', 'void TriangleMesh::WriteOBJFile'),
    ('flip', 'void TriangleMesh::flip_triangles()', 'void TriangleMesh::align_to_origin()'),
    ('bbox', 'BoundingBoxf3 TriangleMesh::bounding_box()', 'BoundingBoxf3 TriangleMesh::transformed_bounding_box(const Transform3d &trafo)'),
    ('adjacency_algorithms', '// Create a mapping from triangle edge into face.', 'bool its_store_triangle('),
    ('volume_algorithm', 'float its_volume(const indexed_triangle_set &its)', 'MassProperties MassProperties::transformed'),
    ('patch_count', '// Number of disconnected patches', '// Same as its_number_of_patches(its) > 1'),
    ('open_edges', 'size_t its_num_open_edges(const std::vector<Vec3i32> &face_neighbors)', 'void VertexFaceIndex::create'),
    ('vertex_face_index', 'void VertexFaceIndex::create(const indexed_triangle_set &its)', 'std::vector<Vec3i32> its_face_neighbors(const indexed_triangle_set &its)'),
    ('face_neighbors', 'std::vector<Vec3i32> its_face_neighbors(const indexed_triangle_set &its)', 'std::vector<Vec3f> its_face_normals'),
 ])
triangle_audit = emit("TriangleMeshRepairGeometry.cpp", """// Exact desktop geometry definitions; no file import, slicing, Qhull or 2D projection.
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/MeshSplitImpl.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"
#include "libslic3r/Execution/ExecutionSeq.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>
namespace Slic3r {
""", triangle_parts, "\n}\n")
bbox_parts = local_sections("BoundingBox.cpp", "955d5de60a1cb94b85a77a2bb44e0b42360e3dde66ce98429fbe62df1dbd542f", [
    ("point_merge", "template <class PointType> void\nBoundingBox3Base<PointType>::merge(const PointType &point)", "template <class PointType> void\nBoundingBox3Base<PointType>::merge(const PointsType &points)"),
    ("size", "template <class PointType> PointType\nBoundingBox3Base<PointType>::size() const", "template <class PointType, typename APointsType> double BoundingBoxBase"),
])
bbox_audit = emit("RepairBoundingBox.cpp", '#include "libslic3r/BoundingBox.hpp"\nnamespace Slic3r {\n', bbox_parts, "\n}\n")
output.with_suffix(".geometry-sources.json").write_text(json.dumps({"solidUnion": solid_audit, "triangleGeometry": triangle_audit, "boundingBox": bbox_audit}, indent=2)+"\n", encoding="utf-8")

mesh_source = source.with_name("MeshRepair.cpp").read_text(encoding="utf-8").replace("\r\n", "\n")
if hashlib.sha256(mesh_source.encode()).hexdigest() != "c2b2e2e670daa0330103a3c6b1d774e15b3b43df79a2d1b2eb3b78fc94534e8e":
    raise SystemExit("Desktop MeshRepair changed; review enhanced-only definition boundary")
begin = "namespace Slic3r {"
end = "namespace {\n\nusing StrictClock"
if mesh_source.count(begin) != 1 or mesh_source.count(end) != 1:
    raise SystemExit("Ambiguous enhanced repair source boundary")
enhanced_body = mesh_source[mesh_source.index(begin):mesh_source.index(end)]
# This is the entire original enhanced implementation and repair_mesh body.
# There is deliberately no substitute definition for strict_repair_mesh.
enhanced_audit = emit("DesktopEnhancedRepair.cpp", mesh_source[:mesh_source.index(begin)],
    [("complete_original_enhanced_repair", enhanced_body)], "\n} // namespace Slic3r\n")
digest_begin = "std::string mesh_repair_geometry_id(const indexed_triangle_set& mesh)"
digest_end = "StrictMeshRepairResult strict_repair_mesh("
if mesh_source.count(digest_begin) != 1 or mesh_source.count(digest_end) != 1:
    raise SystemExit("Ambiguous original geometry identity definition")
digest_body = mesh_source[mesh_source.index(digest_begin):mesh_source.index(digest_end)]
digest_audit = emit("RepairGeometryIdentity.cpp", '#include "libslic3r/MeshRepair.hpp"\n#include "libslic3r/Sha256Digest.hpp"\n#include <cstring>\nnamespace Slic3r {\n',
    [("ordered_geometry_identity", digest_body)], "\n}\n")
output.with_suffix(".enhanced-sources.json").write_text(json.dumps({"normalizedMeshRepairSha256":"c2b2e2e670daa0330103a3c6b1d774e15b3b43df79a2d1b2eb3b78fc94534e8e", "enhanced": enhanced_audit, "geometryIdentity": digest_audit}, indent=2)+"\n", encoding="utf-8")
