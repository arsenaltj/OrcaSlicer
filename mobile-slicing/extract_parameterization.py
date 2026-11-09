"""Compile exactly the existing Core LSCM implementation without its boolean module."""
import hashlib
import pathlib
import sys

source=pathlib.Path(sys.argv[1]);target=pathlib.Path(sys.argv[2])
text=source.read_text(encoding='utf-8')
expected='6c097b3f98ea98f9ac6dc4737f55730a6d9f92eeac22400c0ef26326bd07d891'
if hashlib.sha256(source.read_bytes()).hexdigest()!=expected:
    raise SystemExit('MeshBoolean source changed; re-review parameterization extraction anchors')
def section(start,end):
    if text.count(start)!=1 or text.count(end)!=1:raise SystemExit('Ambiguous Core extraction anchor')
    return text[text.index(start):text.index(end)]
convert=section('template<class _Mesh>\nvoid triangle_mesh_to_cgal(const std::vector<stl_vertex>', 'inline Vec3f to_vec3f(const _EpicMesh::Point& v)')
algorithm=section('std::optional<std::vector<Vec2f>> parameterize_lscm(const indexed_triangle_set &mesh)', '// /////////////////////////////////////////////////////////////////////////////\n// Boolean operations for CGAL meshes')
header='''// Generated from the fixed original MeshBoolean.cpp; never edit the algorithm here.
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/boost/graph/border.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/detect_features.h>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <CGAL/Surface_mesh_parameterization/Error_code.h>
#include <CGAL/Surface_mesh_parameterization/LSCM_parameterizer_3.h>
#include <CGAL/Surface_mesh_parameterization/parameterize.h>
#include <boost/property_map/property_map.hpp>
namespace Slic3r { namespace MeshBoolean { namespace cgal {
using EpicKernel=CGAL::Exact_predicates_inexact_constructions_kernel;
using _EpicMesh=CGAL::Surface_mesh<EpicKernel::Point_3>;
'''
target.parent.mkdir(parents=True,exist_ok=True)
target.write_text(header+convert+algorithm+'\n}}}\n',encoding='utf-8')
