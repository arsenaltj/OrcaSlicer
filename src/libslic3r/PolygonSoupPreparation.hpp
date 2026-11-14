#pragma once

#include <CGAL/version.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <exception>

namespace Slic3r { namespace MeshBoolean { namespace detail {

// Internal preparation/test seam. This certifies only installable topology,
// not closedness, geometry, volume or absence of self-intersections.
struct PolygonSoupOrientation
{
    bool valid = false;
    bool reused_existing = false;
};

struct DefaultSoupCompatibility
{
    template<class PolygonRange> bool operator()(const PolygonRange& polygons) const
    {
        return CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(polygons);
    }
};

// Inputs have already passed repair_polygon_soup(), with valid point indices
// and the default orientation visitor. In pinned CGAL 6.2.1, a compatible
// soup has no same-direction edges, marked edges or disconnected vertex fans;
// orient() then neither reverses a polygon nor duplicates a point. Move the
// existing final predicate before that no-op and reuse its exact result.
template<class PointRange, class PolygonRange, class Check = DefaultSoupCompatibility>
PolygonSoupOrientation orient_prepared_polygon_soup(PointRange& points, PolygonRange& polygons,
                                                   const Check& check_compatibility = {})
{
    if (polygons.empty()) return {};
#if CGAL_VERSION_NR == 1060211000
    try {
        if (check_compatibility(polygons)) return {true, true};
    } catch (const std::exception&) {
        // Only the added read-only qualification can fall back. Exceptions
        // from the original orientation/final predicate still propagate.
    }
#endif
    CGAL::Polygon_mesh_processing::orient_polygon_soup(points, polygons);
    // A false orientation return can mean vertices were split successfully;
    // preserve the original consumer's final predicate as the authority.
    return {!polygons.empty() &&
        CGAL::Polygon_mesh_processing::is_polygon_soup_a_polygon_mesh(polygons), false};
}

}}} // namespace Slic3r::MeshBoolean::detail
