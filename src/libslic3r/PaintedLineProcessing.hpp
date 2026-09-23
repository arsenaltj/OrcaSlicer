#ifndef slic3r_PaintedLineProcessing_hpp_
#define slic3r_PaintedLineProcessing_hpp_

#include "EdgeGrid.hpp"
#include <vector>
#include <algorithm>
#include <cassert>
#include <functional>
#include <tbb/parallel_for.h>

namespace Slic3r { struct ColoredLine; class ExPolygon; }

namespace Slic3r::SegmentationDetail {

// Recover a nested painted region only when its source boundary identifies
// the inner color and contains no boundary belonging to the enclosing color.
void repair_nested_colored_regions(std::vector<std::vector<ExPolygon>> &regions,
                                   const std::vector<std::vector<ColoredLine>> &contours);

// Resolve two-color overlap components with a unique source-boundary witness.
// Regions covered by a third color and conflicting boundary evidence stay intact.
void repair_boundary_owned_overlaps(std::vector<std::vector<ExPolygon>> &regions,
                                    const std::vector<std::vector<ColoredLine>> &contours);

// Repartition invalid graph output within the input shape, keeping existing
// explicit paint where the projected contour has no new color evidence.
void repair_invalid_colored_partition(std::vector<std::vector<ExPolygon>> &regions,
                                     const std::vector<std::vector<ColoredLine>> &contours);

// Use whole-interval source distance and competing-color clearance to recover
// missing contour paint. Source state zero participates as unpainted evidence.
bool restore_missing_contour_colors(std::vector<std::vector<ColoredLine>> &contours,
                                    const std::vector<ColoredLine> &source_cuts,
                                    const std::function<void()> &throw_on_cancel);

// Only unique candidate colors inside previously default-only coverage may be
// transferred. Existing explicit paint is protected without a color priority.
bool transfer_default_color_regions(std::vector<std::vector<ExPolygon>> &regions,
                                    const std::vector<std::vector<ExPolygon>> &candidate);

struct PaintedLine
{
    size_t contour_idx;
    size_t line_idx;
    Line   projected_line;
    int    color;
};

// Internal projection reduction, shared with segmentation regression tests.
std::vector<std::vector<PaintedLine>> post_process_painted_lines(
    const std::vector<EdgeGrid::Contour> &contours, std::vector<PaintedLine> &&painted_lines);

// An absent projected-paint list still produces default-colored boundaries.
std::vector<std::vector<ColoredLine>> colorize_contours(
    const std::vector<EdgeGrid::Contour> &contours, const std::vector<std::vector<PaintedLine>> &painted_contours);

// Propagation reaches at most group_size neighboring layers. Fixed groups with
// alternating buffers keep same-buffer writes disjoint, regardless of how TBB
// splits its tasks. A blocked_range grain size does not define aligned groups.
template<class ProcessGroup>
void for_each_painting_layer_group(size_t num_layers, size_t group_size, ProcessGroup &&process_group)
{
    assert(group_size > 0);
    const size_t num_groups = num_layers / group_size + (num_layers % group_size != 0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_groups), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t group = range.begin(); group < range.end(); ++group) {
            const size_t begin = group * group_size;
            process_group(begin, begin + std::min(group_size, num_layers - begin), (group & 1) * num_layers);
        }
    });
}

} // namespace Slic3r::SegmentationDetail

#endif
