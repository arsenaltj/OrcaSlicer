#pragma once
#include "BeautySurface.hpp"
#include "BeautyBoundaryContours.hpp"

namespace Slic3r::AI {

struct BeautyRegionBoundaryInput {
    std::vector<BeautyBoundaryContours::Segment> curves;
    std::vector<uint8_t> pinned_faces;
};

// Shared validation and contour extraction for face-level and face-interior
// edits. Both use the same protection and fitting policy.
BeautyRegionBoundaryInput selected_region_boundary_input(
    const indexed_triangle_set& mesh,const BeautySurface& surface,
    const std::vector<uint32_t>& regions,uint32_t selected,
    const std::vector<uint8_t>& protected_faces={},
    const std::function<bool()>& canceled={});

struct BeautyRegionBoundaryPlan {
    static constexpr const char* algorithm_version="selected-surface-curve-region-v2";
    std::vector<size_t> added,removed;
    size_t curve_segments{0};
};

// Fit the existing bounded surface curves, then classify existing face centers
// in a local topological band. No source-color loss or old print-piece
// connectivity constraint: this is an explicit edit of semantic ownership.
// Protected faces, shared junctions and folds retain their ownership. This is
// a face-level approximation; it does not split triangles along a curve.
BeautyRegionBoundaryPlan plan_selected_region_boundary(
    const indexed_triangle_set& mesh,const BeautySurface& surface,
    const std::vector<uint32_t>& regions,uint32_t selected,
    const std::vector<uint8_t>& protected_faces={},
    const std::function<bool()>& canceled={});

}
