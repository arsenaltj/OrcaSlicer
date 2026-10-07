#include "SourceColorDetails.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace Slic3r::AI::ColorMatching {
std::vector<ColorRegionSplit> recover_selected_source_color_details(
    const std::vector<uint32_t>& regions, const std::vector<size_t>& slots,
    const std::vector<uint8_t>& selected, const std::vector<uint8_t>& pinned,
    const std::vector<std::array<int32_t, 3>>& neighbors, const std::vector<double>& areas,
    const std::vector<RegionRGB>& original, const std::vector<RegionMaterial>& materials,
    const SourceColorDetailOptions& options, const std::function<bool()>& canceled) {
    const auto require=[](bool ok){if(!ok)throw std::invalid_argument("Invalid source color detail input.");};
    const size_t count=regions.size();
    require(count>0 && count<=UINT32_MAX && slots.size()==count && selected.size()==count &&
        pinned.size()==count && neighbors.size()==count && areas.size()==count && original.size()==count);
    require(std::isfinite(options.minimum_face_gain) && options.minimum_face_gain>=0 &&
        std::isfinite(options.minimum_selection_fraction) && options.minimum_selection_fraction>=0 && options.minimum_selection_fraction<=1 &&
        std::isfinite(options.minimum_region_fraction) && options.minimum_region_fraction>=0 && options.minimum_region_fraction<=1);
    std::vector<uint32_t> local_regions(count,0), face_patches(count);
    std::vector<double> local_areas(count,0.);
    std::map<uint32_t,size_t> active;
    for(size_t f=0;f<count;++f) {
        if((f&4095)==0 && canceled && canceled())throw std::runtime_error("Source color detail recovery cancelled.");
        require(regions[f]>0 && slots[f]!=std::numeric_limits<size_t>::max() && selected[f]<=1 && pinned[f]<=1);
        require(std::isfinite(areas[f]) && areas[f]>=0);
        for(float c:original[f])require(std::isfinite(c) && c>=0 && c<=1);
        for(int32_t n:neighbors[f]) {
            require(n>=-1 && (n<0 || (size_t(n)<count && size_t(n)!=f)));
            if(n>=0)require(std::find(neighbors[size_t(n)].begin(),neighbors[size_t(n)].end(),int32_t(f))!=neighbors[size_t(n)].end());
        }
        face_patches[f]=uint32_t(f);
        if(!selected[f] || pinned[f])continue;
        const auto entry=active.emplace(regions[f],slots[f]);
        require(entry.second || entry.first->second==slots[f]);
        local_regions[f]=regions[f];local_areas[f]=areas[f];
    }
    std::vector<AutomaticRegion> eligible;
    for(const auto& entry:active)eligible.push_back({entry.first,entry.second});
    ColorRegionOptions local;
    local.minimum_patch_gain=options.minimum_face_gain;
    local.minimum_surface_fraction=options.minimum_selection_fraction;
    local.minimum_region_fraction=options.minimum_region_fraction;
    return refine_automatic_color_regions(local_regions,face_patches,neighbors,local_areas,original,eligible,materials,local,canceled);
}
}
