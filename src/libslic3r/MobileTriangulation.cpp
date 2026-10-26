#include "Triangulation.hpp"
#ifdef ORCA_MOBILE_ANDROID
namespace Slic3r {
Triangulation::Indices Triangulation::triangulate(const Points&, const HalfEdges&) { return {}; }
Triangulation::Indices Triangulation::triangulate(const Polygon&) { return {}; }
Triangulation::Indices Triangulation::triangulate(const Polygons&) { return {}; }
Triangulation::Indices Triangulation::triangulate(const ExPolygon&) { return {}; }
Triangulation::Indices Triangulation::triangulate(const ExPolygons&) { return {}; }
Triangulation::Changes Triangulation::create_changes(const Points& points, const Points&) { Changes out(points.size()); for(size_t i=0;i<out.size();++i) out[i]=static_cast<uint32_t>(i); return out; }
Triangulation::Indices Triangulation::triangulate(const ExPolygons&, const Points&) { return {}; }
Triangulation::Indices Triangulation::triangulate(const ExPolygons&, const Points&, const Changes&) { return {}; }
}
#endif
