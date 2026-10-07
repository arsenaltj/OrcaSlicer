#include "BeautyPuzzle.hpp"
#include "slic3r/AI/ColorMatching/AutomaticColorRegions.hpp"

namespace Slic3r::AI {

void BeautyPuzzle::match_filaments(const BeautySurface& surface,const std::vector<PhysicalFilamentChannel>& channels,
                     const std::vector<MixedColorRecipe>& available_mixed,
                     const std::vector<RGBA>& source_faces, bool preserve_color_regions) {
    check_surface(surface);
    require(source_faces.empty() || source_faces.size()==face_piece.size(),"Source colors belong to different geometry.");
    if(!is_valid_physical_channel_set(channels) || std::none_of(channels.begin(),channels.end(),[](const auto& c){return c.compatible;}))return;
    // Channel order is presentation, not material identity. Reordering the
    // same slots must not rematch a saved result or invalidate native mixtures.
    const bool unchanged=palette.size()==channels.size() && std::all_of(palette.begin(),palette.end(),[&](const auto& a) {
        return std::any_of(channels.begin(),channels.end(),[&](const auto& b) {
            return a.slot==b.slot && a.display_color==b.display_color && a.material_type==b.material_type && a.compatible==b.compatible;
        });
    });
    const bool previously_matched=!palette.empty();
    // Keep only used recipes with an identical native definition. A changed
    // or removed project slot must never silently acquire another recipe.
    mixed_recipes.erase(std::remove_if(mixed_recipes.begin(),mixed_recipes.end(),[&](const auto& recipe){
        return !unchanged || std::none_of(available_mixed.begin(),available_mixed.end(),[&](const auto& r){return same_native_mixed_recipe(r,recipe);});
    }),mixed_recipes.end());
    if(!unchanged) {
        for(auto it=filament_slots.begin();it!=filament_slots.end();) {
            // Explicit physical paint survives unrelated palette changes,
            // even when another material has identical RGB. Automatic and
            // custom RGB targets are rematched against the new palette.
            const auto old=std::find_if(palette.begin(),palette.end(),[&](const auto& c){return c.slot==it->second;});
            const bool retain=!target_colors.count(it->first) && old!=palette.end() &&
                std::any_of(channels.begin(),channels.end(),[&](const auto& c){return c.slot==old->slot &&
                    c.display_color==old->display_color && c.material_type==old->material_type && c.compatible;});
            if(retain)++it;else it=filament_slots.erase(it);
        }
    }
    for(auto it=filament_slots.begin();it!=filament_slots.end();)
        if(!native_palette_has_slot(channels,mixed_recipes,it->second))it=filament_slots.erase(it);else ++it;
    palette=channels;
    std::unordered_set<uint32_t> assigned;
    for(const auto& item:filament_slots)assigned.insert(item.first);
    if(unchanged && std::all_of(face_piece.begin(),face_piece.end(),[&](uint32_t id){return assigned.count(id)!=0;}))return;
    struct Mean {std::array<double,3> rgb {};double area=0;};
    std::map<uint32_t,Mean> means;
    for(size_t f=0;f<face_piece.size();++f) {
        if(assigned.count(face_piece[f]))continue;
        auto& mean=means[face_piece[f]];const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
        for(size_t c=0;c<3;++c)mean.rgb[c]+=area*(source_faces.empty()?surface.patches[surface.face_patch[f]].mean_color[c]:source_faces[f][c]);
    }
    std::vector<ColorMatching::AutomaticRegion> automatic_regions;
    for(const auto& entry:means) {
        if(unchanged && filament_slots.count(entry.first))continue;
        const auto painted=colors.find(entry.first);const auto& mean=entry.second;
        const auto target=target_colors.find(entry.first);
        const bool retain_target=target!=target_colors.end() || painted==colors.end() || !previously_matched;
        const auto color=target!=target_colors.end()?target->second:painted==colors.end()?std::array<float,4>{float(mean.rgb[0]/mean.area),float(mean.rgb[1]/mean.area),float(mean.rgb[2]/mean.area),1}:painted->second;
        // Saved/manual paint (including a removed recipe) is never rebound
        // to a newly changed virtual slot. New automatic pieces may reuse
        // the project's native mixtures and save their exact definitions.
        const auto slot=nearest_filament(color,painted==colors.end()?available_mixed:std::vector<MixedColorRecipe>{});
        const auto recipe=std::find_if(available_mixed.begin(),available_mixed.end(),[&](const auto& r){return r.existing_virtual_slot==slot &&
            std::none_of(palette.begin(),palette.end(),[&](const auto& c){return c.slot==slot;});});
        if(recipe!=available_mixed.end())paint_mixed(entry.first,*recipe);
        else paint_filament(entry.first,slot);
        if(retain_target)target_colors[entry.first]=color;
        if(!previously_matched && painted==colors.end())automatic_regions.push_back({entry.first,slot});
    }
    if(preserve_color_regions && !source_faces.empty() && !automatic_regions.empty()) {
        std::vector<ColorMatching::RegionMaterial> materials;
        for(const auto& channel:palette)if(channel.compatible)materials.push_back({channel.slot,filament_color(channel)});
        if(valid_native_mixed_palette(palette,available_mixed))for(const auto& recipe:available_mixed)if(recipe.uniform_color)
            materials.push_back({*recipe.existing_virtual_slot,filament_color({*recipe.existing_virtual_slot,recipe.target_color,{},true})});
        const auto splits=ColorMatching::refine_automatic_color_regions(face_piece,surface.face_patch,
            surface.face_neighbors,surface.areas,source_faces,automatic_regions,materials);
        if(!splits.empty()) {
            auto refined=*this;std::set<uint32_t> affected;std::unordered_set<uint32_t> created;
            for(const auto& split:splits) {
                const uint32_t id=refined.allocate_id();affected.insert(split.source_region);
                created.insert(id);
                for(size_t f:split.faces)refined.face_piece[f]=id;
                const auto recipe=std::find_if(available_mixed.begin(),available_mixed.end(),[&](const auto& r){return r.existing_virtual_slot==split.slot;});
                if(std::any_of(palette.begin(),palette.end(),[&](const auto& c){return c.slot==split.slot && c.compatible;}))refined.paint_filament(id,split.slot);
                else {require(recipe!=available_mixed.end(),"Refined material is unavailable.");refined.paint_mixed(id,*recipe);}
                refined.target_colors[id]=split.source_color;
            }
            refined.split_islands(affected,surface);
            // The residual colour target must describe its own remaining faces,
            // not the former parent including the colours just split off. This
            // matters when a later palette introduces a better third material.
            std::map<uint32_t,Mean> residual_means;
            for(size_t f=0;f<face_piece.size();++f)if(affected.count(face_piece[f]) && !created.count(refined.face_piece[f])) {
                auto& mean=residual_means[refined.face_piece[f]];
                const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
                for(size_t c=0;c<3;++c)mean.rgb[c]+=area*source_faces[f][c];
            }
            for(const auto& entry:residual_means) {
                const auto& mean=entry.second;
                refined.target_colors[entry.first]={float(mean.rgb[0]/mean.area),float(mean.rgb[1]/mean.area),float(mean.rgb[2]/mean.area),1};
            }
            refined.prune_colors();refined.validate(surface);
            *this=std::move(refined);
        }
    }
}

} // namespace Slic3r::AI
