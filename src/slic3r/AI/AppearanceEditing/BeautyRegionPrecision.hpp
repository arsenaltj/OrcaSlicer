#pragma once
#include "BeautyEditRegions.hpp"
#include "slic3r/AI/ModelArtifacts/GlbGeometryEditing.hpp"

namespace Slic3r::AI {

struct BeautyRegionPrecision {
    static constexpr const char* algorithm_version="selected-surface-interior-region-v3";
    std::string source_geometry_id;
    indexed_triangle_set mesh;
    std::vector<SurfaceVertexBlend> vertices;
    std::vector<size_t> parent_faces;
    std::vector<uint32_t> face_region;
    std::vector<uint32_t> source_face_components,source_component_regions;
    size_t split_faces{0},curve_segments{0};
};

// Sample the bounded contour's signed side at welded surface vertices, then
// cut intersected triangles along that continuous linear field. Shared edges
// use one cut position, including separate UV seam copies. The original
// piecewise-planar surface is retained; no smoothing/deformation or global
// mesh refinement. Protected faces and junctions stay intact.
BeautyRegionPrecision refine_selected_region_mesh(
    const indexed_triangle_set&,const BeautySurface&,const std::vector<uint32_t>& regions,
    uint32_t selected,const std::vector<uint8_t>& protected_faces={},
    const std::function<bool()>& canceled={});

// Rebind against the actually loaded refined asset (importer float precision
// may differ). Initially every child inherits its parent's exact material.
// Assign semantic region colors explicitly afterward using ordinary APIs.
struct BeautyPrecisionLayers {BeautyPuzzle printing;BeautyEditRegions editing;};
struct BeautyPrecisionValidity {
    size_t source_components{0},refined_components{0};
    size_t unanchored_components{0},split_components{0},merged_components{0},removed_components{0};
    bool applicable() const {return !unanchored_components && !split_components && !merged_components && !removed_components;}
};
// Require a bijection between the old semantic components and new components
// via unchanged ownership. This protects semantic topology, not old print IDs.
BeautyPrecisionValidity assess_beauty_precision(const BeautyRegionPrecision&,const BeautySurface& loaded_surface);
BeautyPrecisionLayers remap_beauty_precision(const BeautyRegionPrecision&,
    const indexed_triangle_set& loaded_mesh,const BeautySurface& loaded_surface,
    const BeautyPuzzle& original,const std::string& refined_source_sha256,
    bool allow_component_changes_for_review=false);
}
