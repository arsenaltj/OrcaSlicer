#pragma once

#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <nlohmann/json.hpp>
#include <atomic>

namespace Slic3r::GUI::OrcaPrintColorRegions {
using Json = nlohmann::json;

// Preserve exact native contours, including holes. Neither a bounding box nor
// a surviving inward offset proves that every part can accept a recipe.
inline Json geometry(const ExPolygons& polygons, double line_width_mm,
    const std::atomic<bool>& cancelled, size_t& remaining_points)
{
    if (!std::isfinite(line_width_mm) || line_width_mm <= 0.)
        throw std::runtime_error("Invalid native external wall width.");
    Json out={{"polygons",Json::array()},{"external_wall_width_mm",line_width_mm},
        {"area_mm2",0.},{"islands_without_inset_core",0},{"zero_area_islands",0}};
    double area=0.;size_t without_core=0,zero_area=0;
    auto ring=[&](const Polygon& polygon) {
        Json points=Json::array();
        for (const auto& p:polygon.points) {
            if (cancelled.load()) throw std::runtime_error("Region inspection cancelled.");
            if (remaining_points==0) throw std::runtime_error("Region geometry exceeds the inspection point budget.");
            --remaining_points;points.push_back({p.x(),p.y()});
        }
        return points;
    };
    for (const auto& polygon:polygons) {
        if (cancelled.load()) throw std::runtime_error("Region inspection cancelled.");
        Json holes=Json::array();
        for (const auto& hole:polygon.holes) holes.push_back(ring(hole));
        const auto core=offset_ex(polygon,-float(scale_(line_width_mm/2.)));
        const double island_area=polygon.area()*SCALING_FACTOR*SCALING_FACTOR;
        if (!std::isfinite(island_area) || island_area<0.)
            throw std::runtime_error("Invalid native colored region signed area: "+std::to_string(island_area));
        // Clipper intersections may retain touching, collinear contours. Keep
        // their exact points and identify them, rather than dropping evidence
        // or treating a zero-area contact as printable material.
        if(island_area==0.)++zero_area;
        area+=island_area;if(core.empty())++without_core;
        out["polygons"].push_back({{"contour",ring(polygon.contour)},{"holes",std::move(holes)},
            {"area_mm2",island_area},{"zero_area",island_area==0.},{"inset_core_exists",island_area>0. && !core.empty()}});
    }
    out["area_mm2"]=area;out["islands_without_inset_core"]=without_core;
    out["zero_area_islands"]=zero_area;
    return out;
}

inline Json capture(const PrintObject& object,const Layer& layer,
    const std::atomic<bool>& cancelled,size_t& remaining_points)
{
    Json rows=Json::array();
    for (const auto* region:layer.regions()) {
        if (cancelled.load()) throw std::runtime_error("Region inspection cancelled.");
        const auto polygons=to_expolygons(region->slices.surfaces);
        if (polygons.empty()) continue;
        const auto& native=region->region();
        Json row=geometry(polygons,native.flow(object,frExternalPerimeter,layer.height,layer.id()==0).width(),
            cancelled,remaining_points);
        row["native_region_id"]=native.print_region_id();
        // Native role resolution uses one-based filament IDs. These are not
        // logical color targets or final physical nozzle/tool assignments.
        row["role_filament_ids_1based"]={{"external_wall",native.extruder(frExternalPerimeter)},
            {"inner_wall",native.extruder(frPerimeter)},{"sparse_infill",native.extruder(frInfill)},
            {"solid_infill",native.extruder(frSolidInfill)},{"top_surface",native.extruder(frTopSolidInfill)},
            {"bottom_surface",native.config().bottom_surface_filament_id.value}};
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace Slic3r::GUI::OrcaPrintColorRegions
