#pragma once
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <map>
#include <algorithm>
#include <limits>

namespace Slic3r::GUI {
// A proposal only. The native dialog and its existing transaction own creation.
inline ColorDecomposeRecipeResult suggest_native_mixed_filament(const std::string& target_hex,
    const std::vector<std::string>& colors,const std::vector<std::string>& names,
    const std::vector<std::string>& types,const std::string& current_color = {}) {
    ColorDecomposeRgb target;
    if(!color_decompose_hex_to_rgb(target_hex,target) || colors.size()!=types.size())return {};
    using RGB=tex2color::color_utils::ColorDouble;
    const RGB desired{target.r/255.,target.g/255.,target.b/255.};
    const auto error=[&](const std::string& hex) {
        ColorDecomposeRgb rgb;if(!color_decompose_hex_to_rgb(hex,rgb))return std::numeric_limits<double>::infinity();
        return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(desired,{rgb.r/255.,rgb.g/255.,rgb.b/255.});
    };
    std::map<std::string,std::vector<ColorDecomposePhysicalFilament>> groups;
    double best_error=error(current_color);
    for(size_t i=0;i<colors.size();++i) {
        best_error=std::min(best_error,error(colors[i]));
        if(!types[i].empty())groups[types[i]].push_back({colors[i],i<names.size()?names[i]:std::string{},types[i],false,unsigned(i+1)});
    }
    ColorDecomposeRecipeResult best;
    for(const auto& entry:groups) {
        if(entry.second.size()<2)continue;
        auto candidate=recommend_from_physical_filaments(target,entry.second,entry.first);
        if(!candidate.valid || candidate.components.size()<2 || candidate.components.size()>3)continue;
        std::vector<std::string> components;std::vector<int> ratios;
        for(const auto& c:candidate.components){components.push_back(c.color_hex);ratios.push_back(c.ratio);}
        // Rank the same predicted blend the native slot will actually display.
        candidate.matched_color_hex=blend_color_multi(components,ratios);
        const double value=error(candidate.matched_color_hex);
        if(value+.5<best_error){best_error=value;best=std::move(candidate);}
    }
    return best;
}
}
