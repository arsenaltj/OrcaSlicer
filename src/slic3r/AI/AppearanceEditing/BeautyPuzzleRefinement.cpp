#include "BeautyPuzzle.hpp"
#include "slic3r/AI/ColorMatching/ColorIslandCleanup.hpp"
#include "slic3r/AI/ColorMatching/SourceColorDetails.hpp"

namespace Slic3r::AI {
size_t BeautyPuzzle::constrain_selected_colors(const BeautySurface& surface,const std::vector<size_t>& selected,
    const std::vector<RGBA>& original,const std::vector<int32_t>& labels,const std::vector<std::string>& names,
    const ColorMatching::PortraitColorConstraintOptions& options,const std::vector<uint8_t>& protected_faces,
    const std::function<bool()>& canceled) {
    check_surface(surface);validate(surface);const size_t count=face_piece.size();
    require(!palette.empty(),"Match filaments before correcting portrait materials.");
    require(original.size()==count && labels.size()==count && (protected_faces.empty() || protected_faces.size()==count),
        "Portrait guidance and colors must belong to this model.");
    std::vector<uint8_t> chosen(count,0),pinned=protected_faces;
    if(pinned.empty())pinned.assign(count,0);
    pinned=protect_color_intent(surface,original,std::move(pinned));
    for(size_t f:checked_selection(selected))chosen[f]=1;
    std::vector<size_t> slots(count);std::vector<ColorMatching::PortraitFaceRole> roles(count,ColorMatching::PortraitFaceRole::Unknown);
    for(size_t f=0;f<count;++f) {
        require(filament_slots.count(face_piece[f]),"Region color matching is incomplete.");slots[f]=filament_slots.at(face_piece[f]);
        require(labels[f]>=-1 && (labels[f]<0 || size_t(labels[f])<names.size()),"Invalid portrait guidance label.");
        if(labels[f]>=0)roles[f]=ColorMatching::portrait_face_role(names[size_t(labels[f])]);
    }
    std::vector<ColorMatching::RegionMaterial> materials;
    for(const auto& c:palette)if(c.compatible)materials.push_back({c.slot,filament_color(c)});
    for(const auto& recipe:mixed_recipes)if(recipe.uniform_color)
        materials.push_back({*recipe.existing_virtual_slot,filament_color({*recipe.existing_virtual_slot,recipe.target_color,{},true})});
    auto policy=options;if(policy.lip_slots.empty())policy.lip_slots=ColorMatching::suggest_lip_material_slots(materials);
    require(surface.face_edge_lengths.empty() || surface.face_edge_lengths.size()==count,"Surface edge metrics belong to another model.");
    require(std::isfinite(surface.geometry_extent) && surface.geometry_extent>=0,"Invalid surface extent.");
    const ColorMatching::PortraitColorGeometry geometry{surface.face_edge_lengths,surface.geometry_extent*policy.boundary_scale_fraction};
    const auto* metrics=surface.face_edge_lengths.empty() || surface.geometry_extent==0?nullptr:&geometry;
    const auto proposals=ColorMatching::constrain_selected_portrait_materials(face_piece,slots,roles,chosen,pinned,
        surface.face_neighbors,surface.areas,original,materials,policy,canceled,metrics);
    if(proposals.empty())return 0;
    auto candidate=*this;std::set<uint32_t> affected,created;size_t changed=0;
    for(const auto& proposal:proposals) {
        if(canceled && canceled())throw std::runtime_error("Portrait material correction cancelled.");
        const auto id=candidate.allocate_id();created.insert(id);affected.insert(proposal.source_region);
        for(size_t f:proposal.faces){candidate.face_piece[f]=id;++changed;}
        candidate.paint_filament(id,proposal.slot);
        // An explicit semantic correction is material intent. Retaining a
        // source-mean target here would let detail recovery reintroduce red.
    }
    candidate.split_islands(affected,surface);
    struct Mean {std::array<double,3> rgb{};double area=0.;};std::map<uint32_t,Mean> remaining;
    for(size_t f=0;f<count;++f)if(affected.count(face_piece[f]) && !created.count(candidate.face_piece[f])) {
        if((f&4095)==0 && canceled && canceled())throw std::runtime_error("Portrait material correction cancelled.");
        auto& mean=remaining[candidate.face_piece[f]];const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
        for(size_t c=0;c<3;++c)mean.rgb[c]+=area*original[f][c];
    }
    for(const auto& item:remaining) {
        const auto& m=item.second;candidate.target_colors[item.first]={float(m.rgb[0]/m.area),float(m.rgb[1]/m.area),float(m.rgb[2]/m.area),1};
    }
    candidate.prune_colors();candidate.validate(surface);
    if(canceled && canceled())throw std::runtime_error("Portrait material correction cancelled.");
    *this=std::move(candidate);return changed;
}
size_t BeautyPuzzle::recover_source_color_details(const BeautySurface& surface, const std::vector<size_t>& selected,
    const std::vector<RGBA>& original, const std::vector<uint8_t>& protected_faces, const std::function<bool()>& canceled) {
    check_surface(surface);validate(surface);
    require(!palette.empty(),"Match filaments before recovering source color details.");
    require(original.size()==face_piece.size() && (protected_faces.empty() || protected_faces.size()==face_piece.size()),
        "Original colors and protection must belong to this model.");
    const auto selection=checked_selection(selected);
    std::vector<uint8_t> chosen(face_piece.size(),0), pinned=protected_faces;
    if(pinned.empty())pinned.assign(face_piece.size(),0);
    pinned=protect_color_intent(surface,original,std::move(pinned));
    std::vector<size_t> slots(face_piece.size());
    for(size_t f:selection)chosen[f]=1;
    for(size_t f=0;f<face_piece.size();++f) {
        require(filament_slots.count(face_piece[f]),"Region color matching is incomplete.");
        slots[f]=filament_slots.at(face_piece[f]);
    }
    std::vector<ColorMatching::RegionMaterial> materials;
    for(const auto& channel:palette)if(channel.compatible)materials.push_back({channel.slot,filament_color(channel)});
    for(const auto& recipe:mixed_recipes)if(recipe.uniform_color)
        materials.push_back({*recipe.existing_virtual_slot,filament_color({*recipe.existing_virtual_slot,recipe.target_color,{},true})});
    const auto proposals=ColorMatching::recover_selected_source_color_details(face_piece,slots,chosen,pinned,
        surface.face_neighbors,surface.areas,original,materials,{},canceled);
    if(proposals.empty())return 0;
    auto candidate=*this;std::set<uint32_t> affected, created;size_t changed=0;
    for(const auto& proposal:proposals) {
        if(canceled && canceled())throw std::runtime_error("Source color detail recovery cancelled.");
        const uint32_t id=candidate.allocate_id();created.insert(id);affected.insert(proposal.source_region);
        for(size_t f:proposal.faces){candidate.face_piece[f]=id;++changed;}
        const auto channel=std::find_if(palette.begin(),palette.end(),[&](const auto& c){return c.slot==proposal.slot && c.compatible;});
        if(channel!=palette.end())candidate.paint_filament(id,proposal.slot);
        else {
            const auto recipe=std::find_if(mixed_recipes.begin(),mixed_recipes.end(),[&](const auto& r){return r.existing_virtual_slot==proposal.slot;});
            require(recipe!=mixed_recipes.end(),"Recovered material is unavailable.");candidate.paint_mixed(id,*recipe);
        }
        candidate.target_colors[id]=proposal.source_color;
    }
    candidate.split_islands(affected,surface);
    struct Mean {std::array<double,3> rgb{};double area=0.;};std::map<uint32_t,Mean> remaining;
    for(size_t f=0;f<face_piece.size();++f)if(affected.count(face_piece[f]) && !created.count(candidate.face_piece[f])) {
        if((f&4095)==0 && canceled && canceled())throw std::runtime_error("Source color detail recovery cancelled.");
        auto& mean=remaining[candidate.face_piece[f]];const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
        for(size_t c=0;c<3;++c)mean.rgb[c]+=area*original[f][c];
    }
    for(const auto& entry:remaining) {
        const auto& mean=entry.second;
        candidate.target_colors[entry.first]={float(mean.rgb[0]/mean.area),float(mean.rgb[1]/mean.area),float(mean.rgb[2]/mean.area),1};
    }
    candidate.prune_colors();candidate.validate(surface);
    if(canceled && canceled())throw std::runtime_error("Source color detail recovery cancelled.");
    *this=std::move(candidate);return changed;
}
std::vector<uint8_t> BeautyPuzzle::refinement_protection(const BeautySurface& surface,
    const std::vector<size_t>& selected, const std::vector<int32_t>& labels,
    const std::vector<std::string>& names, bool boundary_repair) {
    const size_t count = surface.areas.size();
    require(surface.face_neighbors.size() == count && (labels.empty() || labels.size() == count), "Recognition belongs to another surface.");
    std::vector<uint8_t> chosen(count, 0), pinned(count, boundary_repair ? 0 : 1);
    for (size_t f : selected) { require(f < count, "Selection is outside the model."); chosen[f] = 1; }
    if (labels.empty() || names.empty()) return pinned;
    for (size_t f = 0; f < count; ++f) {
        const int32_t label = labels[f];
        require(label >= -1 && (label < 0 || size_t(label) < names.size()), "Invalid recognition label.");
        if (label < 0) continue;
        const auto& name = names[size_t(label)];
        const bool feature = name != "face" && name != "nose" && name != "neck" && name != "hair" && name != "cloth";
        pinned[f] = feature;
        if (feature && boundary_repair && chosen[f]) for (int32_t n : surface.face_neighbors[f])
            if (n >= 0 && labels[size_t(n)] != label) { pinned[f] = 0; break; }
    }
    return pinned;
}

std::vector<uint8_t> BeautyPuzzle::protect_color_intent(const BeautySurface& surface,
    const std::vector<RGBA>& original, std::vector<uint8_t> pinned) const {
    check_surface(surface);
    require(original.size() == face_piece.size() && pinned.size() == face_piece.size(), "Color intent protection belongs to another model.");
    struct Mean { std::array<double,3> rgb{}; double area=0.; };
    std::map<uint32_t,Mean> means;
    for (size_t f=0;f<face_piece.size();++f) {
        check_color(original[f]);auto& mean=means[face_piece[f]];
        const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
        for(size_t c=0;c<3;++c)mean.rgb[c]+=area*original[f][c];
    }
    std::set<uint32_t> explicit_colors;
    for(const auto& entry:means) {
        const auto target=target_colors.find(entry.first);
        if(target==target_colors.end()){explicit_colors.insert(entry.first);continue;}
        for(size_t c=0;c<3;++c)if(std::abs(entry.second.rgb[c]/entry.second.area-target->second[c])>1e-5)
            explicit_colors.insert(entry.first);
    }
    for(size_t f=0;f<face_piece.size();++f)if(explicit_colors.count(face_piece[f]))pinned[f]=1;
    return pinned;
}

size_t BeautyPuzzle::clean_color_islands(const BeautySurface& surface, const std::vector<size_t>& selected,
                                       const std::vector<uint8_t>& protected_faces, const std::function<bool()>& canceled) {
    check_surface(surface); validate(surface);
    require(!palette.empty(), "Match filaments before cleaning colors.");
    require(protected_faces.empty() || protected_faces.size() == face_piece.size(), "Cleanup protection belongs to another model.");
    const auto indices = checked_selection(selected);
    std::vector<size_t> slots(face_piece.size());
    std::vector<uint8_t> chosen(face_piece.size(), 0), pinned(face_piece.size(), 0);
    std::vector<std::array<uint8_t, 3>> smooth_edges(face_piece.size());
    for (size_t f : indices) chosen[f] = 1;
    for (size_t f = 0; f < face_piece.size(); ++f) {
        if ((f & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Color cleanup cancelled.");
        require(filament_slots.count(face_piece[f]) != 0, "Region color matching is incomplete.");
        slots[f] = filament_slots.at(face_piece[f]);
        // Automatic targets survive initial matching; explicit material paint
        // and legacy paint without provenance are protected conservatively.
        pinned[f] = !target_colors.count(face_piece[f]) || (!protected_faces.empty() && protected_faces[f]);
        for (size_t e = 0; e < 3; ++e) {
            const int32_t n = surface.face_neighbors[f][e];
            smooth_edges[f][e] = n >= 0 && surface.normals[f].dot(surface.normals[size_t(n)]) >= .85;
        }
    }
    const auto proposals = ColorMatching::find_enclosed_color_islands(slots, chosen, pinned, surface.face_neighbors,
                                                                    surface.areas, smooth_edges, {}, canceled);
    auto candidate = *this; std::map<size_t, std::vector<size_t>> by_slot; size_t changed = 0;
    for (const auto& proposal : proposals) {
        auto& faces = by_slot[proposal.slot]; faces.insert(faces.end(), proposal.faces.begin(), proposal.faces.end());
        changed += proposal.faces.size();
    }
    for (const auto& entry : by_slot) {
        if (canceled && canceled()) throw std::runtime_error("Color cleanup cancelled.");
        candidate.paint_faces_filament(entry.second, surface, entry.first);
    }
    candidate.validate(surface);
    if (canceled && canceled()) throw std::runtime_error("Color cleanup cancelled.");
    if (changed) *this = std::move(candidate);
    return changed;
}

size_t BeautyPuzzle::smooth_selected_boundaries(const BeautySurface& surface, const std::vector<size_t>& selected,
                                              const std::vector<uint8_t>& protected_faces, const std::function<bool()>& canceled) {
    check_surface(surface); validate(surface);
    require(!palette.empty(), "Match filaments before repairing color boundaries.");
    require(protected_faces.empty() || protected_faces.size() == face_piece.size(), "Boundary protection belongs to another model.");
    const auto indices = checked_selection(selected);
    std::vector<uint8_t> scope(face_piece.size(), 0);
    std::vector<int32_t> pinned(face_piece.size(), -1);
    for (size_t f : indices) scope[f] = 1;
    // Let the chosen border move in either direction, within three adjacent
    // rings only. Remote boundaries and other sides of the model stay exact.
    for (unsigned ring = 0; ring < 3; ++ring) {
        auto expanded = scope;
        for (size_t f = 0; f < scope.size(); ++f) if (scope[f])
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0) expanded[size_t(n)] = 1;
        scope.swap(expanded);
    }
    for (size_t f = 0; f < face_piece.size(); ++f) {
        require(filament_slots.count(face_piece[f]) != 0, "Region color matching is incomplete.");
        if (!target_colors.count(face_piece[f]) || (!protected_faces.empty() && protected_faces[f])) pinned[f] = 0;
        for (int32_t n : surface.face_neighbors[f])
            if (n < 0 || surface.normals[f].dot(surface.normals[size_t(n)]) < .85) pinned[f] = 0;
    }
    auto candidate = *this;
    candidate.smooth_partition(surface, scope, pinned, canceled);
    candidate.validate(surface);
    size_t changed = 0;
    for (size_t f = 0; f < face_piece.size(); ++f)
        changed += filament_slots.at(face_piece[f]) != candidate.filament_slots.at(candidate.face_piece[f]);
    if (canceled && canceled()) throw std::runtime_error("Boundary repair cancelled.");
    if (changed) *this = std::move(candidate);
    return changed;
}
}
