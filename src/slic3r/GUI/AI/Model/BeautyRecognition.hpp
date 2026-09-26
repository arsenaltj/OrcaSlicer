#pragma once
#include "BeautyGuidance.hpp"
#include "BeautyPuzzle.hpp"
#include "BeautyMouthDetail.hpp"

namespace Slic3r::AI {
// Match untouched pieces, then retain measured light/dark relationships of
// facial features to nearby skin. Saved paint is never eligible for adjustment.
// This changes native slot choices only, not geometry, recognition or recipes.
inline size_t beauty_match_feature_filaments(BeautyPuzzle& puzzle,const BeautySurface& surface,
    const BeautyGuidance& guidance,const std::vector<RGBA>& source,
    const std::vector<PhysicalFilamentChannel>& channels,const std::vector<MixedColorRecipe>& mixtures={}) {
    std::set<uint32_t> eligible;
    for(uint32_t id:puzzle.face_piece)if(!puzzle.colors.count(id) && !puzzle.filament_slots.count(id))eligible.insert(id);
    puzzle.match_filaments(surface,channels,mixtures,source);
    if(eligible.empty() || source.size()!=puzzle.face_piece.size() ||
       !guidance.completed(source.size()) || guidance.names.empty() || puzzle.palette.empty())return 0;
    using RGB=tex2color::color_utils::ColorDouble;
    const auto lightness=[](const RGB& rgb) {
        double y=0;const double weights[]{.2126,.7152,.0722};
        for(size_t c=0;c<3;++c) {const double v=std::clamp(rgb[c],0.,1.);
            y+=weights[c]*(v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4));}
        return y>216./24389.?116.*std::cbrt(y)-16.:y*24389./27.;
    };
    const auto rgb=[](const std::array<float,4>& c){return RGB{c[0],c[1],c[2]};};
    const auto distance=[](const RGB& a,const RGB& b){return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(a,b);};
    struct Part {RGB source{};double area=0;std::map<std::string,double> labels;std::set<uint32_t> neighbors;std::string name;};
    std::map<uint32_t,Part> parts;
    for(size_t f=0;f<source.size();++f) {
        const auto id=puzzle.face_piece[f];auto& p=parts[id];const double area=surface.areas[f];p.area+=area;
        for(size_t c=0;c<3;++c)p.source[c]+=area*source[f][c];
        const int32_t label=guidance.labels[f];
        if(label>=0 && size_t(label)<guidance.names.size())p.labels[guidance.names[size_t(label)]]+=area;
        for(int32_t n:surface.face_neighbors[f])if(n>=0 && puzzle.face_piece[size_t(n)]!=id)p.neighbors.insert(puzzle.face_piece[size_t(n)]);
    }
    for(auto& entry:parts) {
        auto& p=entry.second;if(p.area<=0)continue;for(double& c:p.source)c/=p.area;
        double known=0;for(const auto& label:p.labels)known+=label.second;
        // Smoothed contours include an unlabeled rim. Require a majority of
        // the whole piece and agreement among known labels, not 80% coverage.
        for(const auto& label:p.labels)if(label.second>=p.area*.5 && label.second>=known*.8)p.name=label.first;
    }
    const auto feature=[](const std::string& n){return n=="lb" || n=="rb" || n=="iris" || n=="ulip" || n=="llip" || n=="imouth";};
    const auto skin=[](const std::string& n){return n=="face" || n=="nose";};
    std::vector<std::pair<size_t,RGB>> candidates;
    for(const auto& c:puzzle.palette)if(c.compatible)candidates.push_back({c.slot,rgb(BeautyPuzzle::filament_color(c))});
    if(valid_native_mixed_palette(puzzle.palette,mixtures))for(const auto& m:mixtures)if(m.uniform_color)
        candidates.push_back({*m.existing_virtual_slot,rgb(BeautyPuzzle::filament_color({*m.existing_virtual_slot,m.target_color,{},true}))});
    // Freeze the reference paint so results do not depend on piece iteration order.
    const auto paint=puzzle.colors;size_t changed=0;
    for(uint32_t id:eligible) {
        const auto& p=parts.at(id);if(!feature(p.name) || !paint.count(id))continue;
        std::set<uint32_t> visited{id},frontier{id},references;
        // Traverse only recognized facial parts, never a remote shell/person or
        // hair/clothing. Stop at the first skin neighborhood (at most 3 edges).
        for(unsigned hop=0;hop<3 && references.empty();++hop) {
            std::set<uint32_t> next;
            for(uint32_t at:frontier)for(uint32_t n:parts.at(at).neighbors)if(visited.insert(n).second) {
                const auto& name=parts.at(n).name;
                if(skin(name) && paint.count(n))references.insert(n);
                else if(feature(name) || name=="le" || name=="re")next.insert(n);
            }
            frontier.swap(next);
        }
        double source_l=0,paint_l=0,area=0;
        for(uint32_t n:references) {const auto& ref=parts.at(n);area+=ref.area;
            source_l+=ref.area*lightness(ref.source);paint_l+=ref.area*lightness(rgb(paint.at(n)));}
        if(area<=0)continue;source_l/=area;paint_l/=area;
        const double delta=std::clamp(lightness(p.source)-source_l,-32.,32.);
        if(std::abs(delta)<4.)continue; // No source contrast, no invented feature.
        const auto original=rgb(paint.at(id));const double error=distance(p.source,original);
        const double direction=delta<0?-1.:1.,required=std::min(18.,std::abs(delta)*.75);
        if(direction*(lightness(original)-paint_l)>=required)continue;
        // Repair collapsed contrast only. A sparse palette cannot reproduce
        // the exact difference; do not wash out already readable features.
        const auto score=[&](const RGB& c){const double contrast=direction*(lightness(c)-paint_l);
            return distance(p.source,c)+4.*std::max(0.,required-contrast)+.2*std::abs(contrast-std::abs(delta));};
        double best=score(original);size_t slot=puzzle.filament_slots.at(id);
        for(const auto& c:candidates) {
            if(distance(p.source,c.second)>error+10.)continue; // Bound absolute color loss.
            const double value=score(c.second);
            if(value+1.<best){best=value;slot=c.first;}
        }
        if(slot==puzzle.filament_slots.at(id))continue;
        const auto mix=std::find_if(mixtures.begin(),mixtures.end(),[&](const auto& m){return m.existing_virtual_slot==slot &&
            std::none_of(puzzle.palette.begin(),puzzle.palette.end(),[&](const auto& c){return c.slot==slot;});});
        const auto target=puzzle.target_colors.at(id);
        if(mix!=mixtures.end())puzzle.paint_mixed(id,*mix);else puzzle.paint_filament(id,slot);
        puzzle.target_colors[id]=target;
        ++changed;
    }
    return changed;
}

// Initial matched partitions can contain isolated semantic specks with exactly
// the same printing color as their surroundings. Remove their editing seams,
// but retain all facial parts, separate shells and every per-face filament slot.
// Call only for a new automatic partition, never on a restored user version.
inline size_t beauty_coalesce_matched_specks(BeautyPuzzle& puzzle,const BeautySurface& surface,
    const BeautyGuidance& guidance) {
    if(puzzle.palette.empty() || puzzle.face_piece.size()!=surface.areas.size() ||
       !guidance.completed(puzzle.face_piece.size()) || guidance.names.empty())return 0;
    struct Part {std::vector<size_t> faces;double area=0;bool protected_part=false;};
    std::map<uint32_t,Part> parts;double total_area=0;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        auto& part=parts[puzzle.face_piece[f]];part.faces.push_back(f);
        part.area+=surface.areas[f];total_area+=surface.areas[f];
        const auto label=guidance.labels[f];
        if(label>=0) {
            const auto& name=guidance.names.at(size_t(label));
            part.protected_part|=name!="face" && name!="hair" && name!="neck" && name!="cloth";
        }
    }
    std::vector<uint32_t> order;
    const double limit=total_area*0.0000125;
    for(const auto& p:parts)if(!p.second.protected_part && p.second.faces.size()<=12 && p.second.area<limit)
        order.push_back(p.first);
    std::sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b){
        return parts[a].area==parts[b].area?a<b:parts[a].area<parts[b].area;
    });
    size_t removed=0;
    for(uint32_t id:order) {
        auto& part=parts[id];const auto slot=puzzle.filament_slots.find(id);
        if(part.faces.empty() || part.faces.size()>12 || part.area>=limit || slot==puzzle.filament_slots.end())continue;
        std::map<uint32_t,size_t> contacts;size_t boundary=0;
        for(size_t f:part.faces)for(int32_t n:surface.face_neighbors[f]) {
            if(n>=0 && puzzle.face_piece[size_t(n)]==id)continue;
            ++boundary;if(n<0)continue;
            const auto target=puzzle.face_piece[size_t(n)];const auto other=puzzle.filament_slots.find(target);
            if(!parts[target].protected_part && other!=puzzle.filament_slots.end() && other->second==slot->second &&
               puzzle.colors.at(id)==puzzle.colors.at(target) && parts[target].area>=32.*part.area)++contacts[target];
        }
        if(contacts.empty())continue;
        const auto best=std::max_element(contacts.begin(),contacts.end(),[](const auto& a,const auto& b){return a.second<b.second;});
        if(best->second*3<boundary*2)continue;
        auto& target=parts[best->first];
        for(size_t f:part.faces){puzzle.face_piece[f]=best->first;target.faces.push_back(f);}
        target.area+=part.area;part.faces.clear();part.area=0;
        puzzle.colors.erase(id);puzzle.filament_slots.erase(id);puzzle.target_colors.erase(id);++removed;
    }
    return removed;
}

// A coarse clothing/hair label should not leave editing seams through a single
// pigment. Join adjacent automatic pieces only; never run on saved user edits.
// Keep source-color, anatomical and sharp geometric boundaries even when a
// limited filament palette maps both sides to the same slot.
inline size_t beauty_coalesce_body_regions(BeautyPuzzle& puzzle,const BeautySurface& surface,
    const BeautyGuidance& guidance,const std::vector<RGBA>& source) {
    const size_t count=puzzle.face_piece.size();
    if(source.size()!=count || surface.areas.size()!=count || !guidance.completed(count) || puzzle.palette.empty())return 0;
    struct Part {double area=0;std::array<double,3> rgb{};std::set<std::string> names;bool protected_part=false;};
    struct Contact {size_t edges=0,smooth=0;};
    std::map<uint32_t,Part> parts;
    std::map<std::pair<uint32_t,uint32_t>,Contact> contacts;
    for(size_t f=0;f<count;++f) {
        const auto id=puzzle.face_piece[f];auto& p=parts[id];p.area+=surface.areas[f];
        for(size_t c=0;c<3;++c)p.rgb[c]+=surface.areas[f]*source[f][c];
        const auto label=guidance.labels[f];
        if(label>=0) {
            const auto& name=guidance.names.at(size_t(label));
            if(name=="hair" || name=="cloth")p.names.insert(name);else p.protected_part=true;
        }
        for(int32_t n:surface.face_neighbors[f])if(n>=0 && size_t(n)>f && puzzle.face_piece[size_t(n)]!=id) {
            const auto key=std::minmax(id,puzzle.face_piece[size_t(n)]);auto& c=contacts[key];++c.edges;
            if(surface.normals[f].dot(surface.normals[size_t(n)])>.7)++c.smooth;
        }
    }
    for(auto& entry:parts)if(entry.second.area>0)for(auto& c:entry.second.rgb)c/=entry.second.area;
    struct Edge {uint32_t a,b;double distance;};std::vector<Edge> edges;
    for(const auto& entry:contacts) {
        const auto a=entry.first.first,b=entry.first.second;const auto& pa=parts[a];const auto& pb=parts[b];
        if(pa.protected_part || pb.protected_part || pa.area<=0 || pb.area<=0 || entry.second.smooth*5<entry.second.edges*4)continue;
        if(!puzzle.filament_slots.count(a) || !puzzle.filament_slots.count(b) ||
           puzzle.filament_slots.at(a)!=puzzle.filament_slots.at(b) || puzzle.colors.at(a)!=puzzle.colors.at(b))continue;
        const double d=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(pa.rgb,pb.rgb);
        if(d<=8.)edges.push_back({a,b,d});
    }
    std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return std::tie(a.distance,a.a,a.b)<std::tie(b.distance,b.a,b.b);});
    std::map<uint32_t,uint32_t> parent;std::map<uint32_t,std::vector<uint32_t>> members;
    for(const auto& entry:parts){parent[entry.first]=entry.first;members[entry.first]={entry.first};}
    const auto root=[&](uint32_t id){while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id;};
    size_t removed=0;bool changed=true;
    while(changed) {
        changed=false;
        for(const auto& edge:edges) {
            auto a=root(edge.a),b=root(edge.b);if(a==b)continue;
            auto names=parts[a].names;names.insert(parts[b].names.begin(),parts[b].names.end());
            if(names.size()!=1)continue; // Unknown-only pieces cannot initiate a merge.
            bool similar=true;
            // Complete-link color bound prevents gradual chaining into another material.
            for(auto x:members[a])for(auto y:members[b])if(
                tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(parts[x].rgb,parts[y].rgb)>8.)similar=false;
            if(!similar)continue;
            if(a>b)std::swap(a,b);
            parent[b]=a;parts[a].names=std::move(names);members[a].insert(members[a].end(),members[b].begin(),members[b].end());members[b].clear();
            puzzle.colors.erase(b);puzzle.filament_slots.erase(b);puzzle.target_colors.erase(b);++removed;changed=true;
        }
    }
    for(auto& id:puzzle.face_piece)id=root(id);
    return removed;
}

// Remove only tiny disconnected seeds near a newly inferred outline. Main
// components and labels outside this local band are never replaced.
inline void beauty_clean_feature_seeds(std::vector<int32_t>& labels,const BeautySurface& surface,
    const std::vector<uint8_t>& band,const std::set<int32_t>& allowed) {
    const auto before=labels;std::map<int32_t,size_t> totals;
    for(int32_t id:before)if(allowed.count(id))++totals[id];
    std::vector<uint8_t> seen(labels.size(),0);
    for(size_t seed=0;seed<labels.size();++seed)if(band[seed] && !seen[seed] && allowed.count(before[seed])) {
        std::vector<size_t> component{seed};seen[seed]=1;bool local=true;
        std::map<int32_t,size_t> boundary;size_t edges=0;
        for(size_t at=0;at<component.size();++at) {
            const size_t f=component[at];local=local && band[f];
            for(int32_t n:surface.face_neighbors[f]) {
                if(n<0){local=false;continue;}
                if(before[size_t(n)]==before[seed]) {
                    if(!seen[size_t(n)]){seen[size_t(n)]=1;component.push_back(size_t(n));}
                } else {++edges;if(allowed.count(before[size_t(n)]))++boundary[before[size_t(n)]];}
            }
        }
        if(!local || component.size()>std::min(size_t(8),totals[before[seed]]/20) || boundary.empty())continue;
        const auto best=std::max_element(boundary.begin(),boundary.end(),[](const auto& a,const auto& b){return a.second<b.second;});
        if(best->second*5<edges*3)continue;
        for(size_t f:component)labels[f]=best->first;
    }
}

// Shape hints guide editing; the original semantic scores/regions remain intact.
// Clear the old part first so a refined outline can shrink as well as expand.
inline BeautyGuidance beauty_feature_guidance(const GUI::LocalSemanticEvidence::Evidence& evidence,const BeautySurface* surface=nullptr) {
    BeautyGuidance result;result.labels=evidence.face_regions;result.eyes=evidence.eye_details;
    std::map<std::pair<std::string,std::string>,int32_t> ids;
    for(const auto& region:evidence.regions) {
        ids[{region.subject_id,region.label}]=int32_t(result.names.size());result.names.push_back(region.label);
        if(region.label=="re" || region.label=="le") {
            const auto found=std::find_if(result.eyes.begin(),result.eyes.end(),[&](const auto& eye){return eye.subject_id==region.subject_id && eye.label==region.label;});
            if(found==result.eyes.end())result.eyes.push_back({region.subject_id,region.label,region.faces,{}});
        }
    }
    auto id_for=[&](const std::string& subject,const std::string& name) {
        const auto key=std::make_pair(subject,name);const auto found=ids.find(key);
        if(found!=ids.end())return found->second;
        const auto id=int32_t(result.names.size());result.names.push_back(name);ids[key]=id;return id;
    };
    for(const auto& shape:evidence.feature_details) {
        const auto found=ids.find({shape.subject_id,shape.label});
        if(found!=ids.end()) {
            const int32_t old=found->second, background=id_for(shape.subject_id,"face");
            for(auto& label:result.labels)if(label==old)label=background;
        }
    }
    for(const auto& shape:evidence.feature_details) {
        const int32_t id=id_for(shape.subject_id,shape.label);
        for(size_t f:shape.faces)result.labels.at(f)=id;
        if(shape.label=="re" || shape.label=="le") {
            result.eyes.erase(std::remove_if(result.eyes.begin(),result.eyes.end(),[&](const auto& eye){return eye.subject_id==shape.subject_id && eye.label==shape.label;}),result.eyes.end());
            result.eyes.push_back({shape.subject_id,shape.label,shape.faces,shape.iris_faces});
        }
    }
    if(surface && surface->face_neighbors.size()==result.labels.size()) {
        std::set<std::string> subjects;for(const auto& shape:evidence.feature_details)subjects.insert(shape.subject_id);
        for(const auto& subject:subjects) {
            std::vector<uint8_t> band(result.labels.size(),0);std::set<int32_t> allowed{id_for(subject,"face")};
            for(const auto& shape:evidence.feature_details)if(shape.subject_id==subject) {
                const int32_t id=ids.at({subject,shape.label});allowed.insert(id);
                for(size_t f:shape.faces)band[f]=1;
                if(size_t(id)<evidence.regions.size())for(size_t f:evidence.regions[size_t(id)].faces)band[f]=1;
            }
            const auto initial=band;
            for(size_t f=0;f<band.size();++f)if(initial[f])for(int32_t n:surface->face_neighbors[f])if(n>=0)band[size_t(n)]=1;
            beauty_clean_feature_seeds(result.labels,*surface,band,allowed);
        }
        // Apply the same local topology rule to iris pinholes and specks, inside
        // each updated aperture. Never alter a different eye or an old-only hint.
        for(auto& eye:result.eyes) {
            const auto shape=std::find_if(evidence.feature_details.begin(),evidence.feature_details.end(),[&](const auto& h){return h.subject_id==eye.subject_id && h.label==eye.label;});
            if(shape==evidence.feature_details.end())continue;
            const int32_t id=ids.at({eye.subject_id,eye.label});eye.aperture_faces.clear();
            std::vector<int32_t> parts(result.labels.size(),-1);std::vector<uint8_t> band(result.labels.size(),0);
            for(size_t f=0;f<result.labels.size();++f)if(result.labels[f]==id){eye.aperture_faces.push_back(f);parts[f]=0;band[f]=1;}
            for(size_t f:eye.iris_faces)if(band[f])parts[f]=1;
            beauty_clean_feature_seeds(parts,*surface,band,{0,1});eye.iris_faces.clear();
            for(size_t f:eye.aperture_faces)if(parts[f]==1)eye.iris_faces.push_back(f);
        }
    }
    return result;
}

// Only an exact, unedited automatic result may be upgraded without replacing
// user work. IDs may have changed during serialization; partitions and paint may not.
inline bool same_automatic_puzzle(const BeautyPuzzle& saved,const BeautyPuzzle& automatic) {
    if(saved.face_piece.size()!=automatic.face_piece.size() || !saved.same_palette(automatic.palette) ||
       !saved.same_mixed_palette(automatic.mixed_recipes))return false;
    std::map<uint32_t,uint32_t> forward,reverse;
    for(size_t f=0;f<saved.face_piece.size();++f) {
        const auto a=saved.face_piece[f],b=automatic.face_piece[f];
        auto x=forward.emplace(a,b),y=reverse.emplace(b,a);
        if(x.first->second!=b || y.first->second!=a)return false;
        if(x.second) {
            const auto ca=saved.colors.find(a),cb=automatic.colors.find(b);
            const auto sa=saved.filament_slots.find(a),sb=automatic.filament_slots.find(b);
            if(ca==saved.colors.end() || cb==automatic.colors.end() || ca->second!=cb->second ||
               sa==saved.filament_slots.end() || sb==automatic.filament_slots.end() || sa->second!=sb->second)return false;
        }
    }
    return true;
}
inline std::vector<RGBA> beauty_source_face_colors(const indexed_triangle_set& mesh,const std::vector<RGBA>& colors) {
    if(colors.empty())return {};
    if(colors.size()!=mesh.vertices.size())throw std::runtime_error("Original texture does not match the surface.");
    std::vector<RGBA> result(mesh.indices.size(),RGBA{0,0,0,1});
    for(size_t f=0;f<mesh.indices.size();++f)for(int v:mesh.indices[f])for(size_t c=0;c<3;++c)
        result[f][c]+=colors.at(size_t(v))[c]/3.f;
    return result;
}
inline BeautyPuzzle beauty_supplement_features(const BeautyPuzzle& original,const BeautySurface& surface,
                                               const BeautyGuidance& guidance) {
    if(!guidance.completed(original.face_piece.size()))throw std::runtime_error("Recognize facial features first.");
    auto next=original;
    std::map<std::pair<uint32_t,int32_t>,std::vector<size_t>> cuts;
    for(size_t f=0;f<guidance.labels.size();++f) {
        const auto label=guidance.labels[f];if(label<0 || size_t(label)>=guidance.names.size())continue;
        const auto& name=guidance.names[size_t(label)];
        if(name=="le" || name=="re" || name=="iris" || name=="llip" || name=="ulip" || name=="imouth" || name=="nose" || name=="lb" || name=="rb")
            cuts[{original.face_piece[f],label}].push_back(f);
    }
    for(const auto& cut:cuts)next.assign_region(cut.second,surface);
    return next;
}
// A failed attempt is neither recognition nor a permanent cache entry. Briefly
// back off when navigating in/out; explicit Refresh always bypasses this gate.
inline bool beauty_recognition_due(const nlohmann::json& attempt,const std::string& geometry,
                                   const std::string& source,int64_t now) {
    try {
        if(attempt.at("geometry")!=geometry || attempt.at("source")!=source || attempt.at("state")!="failed")return true;
        const auto at=attempt.at("time").get<int64_t>();return at>now || now-at>=300;
    }catch(const std::exception&){return true;}
}
}
