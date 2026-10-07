#include "PortraitColorConstraints.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::AI::ColorMatching {
namespace {
void require(bool value) { if(!value)throw std::invalid_argument("Invalid portrait material constraint input."); }
void check_color(const RegionRGB& color) { for(float c:color)require(std::isfinite(c) && c>=0 && c<=1); }
double distance(const RegionRGB& a,const RegionRGB& b) {
    return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01({a[0],a[1],a[2]},{b[0],b[1],b[2]});
}
}
PortraitFaceRole portrait_face_role(const std::string& name) {
    if(name=="face" || name=="nose" || name=="neck" || name=="lr" || name=="rr")return PortraitFaceRole::Skin;
    if(name=="le" || name=="re" || name=="iris" || name=="lb" || name=="rb")return PortraitFaceRole::Eye;
    if(name=="hair")return PortraitFaceRole::Hair;
    if(name=="ulip" || name=="llip")return PortraitFaceRole::Lips;
    if(name=="imouth")return PortraitFaceRole::Mouth;
    if(name=="cloth")return PortraitFaceRole::Other;
    return PortraitFaceRole::Unknown;
}
std::vector<size_t> suggest_lip_material_slots(const std::vector<RegionMaterial>& materials) {
    std::vector<size_t> slots;
    for(const auto& material:materials) {
        check_color(material.color);
        const auto& c=material.color;const double high=std::max({c[0],c[1],c[2]}),low=std::min({c[0],c[1],c[2]});
        if(high<=0 || c[0]!=high || high-low<.15 || (high-low)/high<.35)continue;
        const double hue=60.*(c[1]-c[2])/(high-low);
        double y=0;const double weight[]{.2126,.7152,.0722};
        for(size_t i=0;i<3;++i)y+=weight[i]*(c[i]<=.04045?c[i]/12.92:std::pow((c[i]+.055)/1.055,2.4));
        const double lightness=y>216./24389.?116.*std::cbrt(y)-16.:y*24389./27.;
        if(std::abs(hue)<=18. && lightness<65.)slots.push_back(material.slot);
    }
    std::sort(slots.begin(),slots.end());slots.erase(std::unique(slots.begin(),slots.end()),slots.end());return slots;
}
std::vector<ColorRegionSplit> constrain_selected_portrait_materials(
    const std::vector<uint32_t>& regions,const std::vector<size_t>& slots,const std::vector<PortraitFaceRole>& roles,
    const std::vector<uint8_t>& selected,const std::vector<uint8_t>& pinned,
    const std::vector<std::array<int32_t,3>>& neighbors,const std::vector<double>& areas,
    const std::vector<RegionRGB>& original,const std::vector<RegionMaterial>& materials,
    const PortraitColorConstraintOptions& options,const std::function<bool()>& canceled,const PortraitColorGeometry* geometry) {
    const auto checkpoint=[&]{if(canceled && canceled())throw std::runtime_error("Portrait material correction cancelled.");};
    checkpoint();const size_t count=regions.size(),missing=std::numeric_limits<size_t>::max();
    require(count>0 && slots.size()==count && roles.size()==count && selected.size()==count && pinned.size()==count &&
        neighbors.size()==count && areas.size()==count && original.size()==count);
    require(std::isfinite(options.maximum_source_error_increase) && options.maximum_source_error_increase>=0 && options.lip_boundary_guard_rings<=8 &&
        std::isfinite(options.boundary_edge_penalty) && options.boundary_edge_penalty>=0 && options.coherence_passes<=32 &&
        std::isfinite(options.boundary_scale_fraction) && options.boundary_scale_fraction>0 && options.boundary_scale_fraction<=1);
    const bool geometric=geometry && options.geometric_coherence;
    const double length=geometric?geometry->reference_length:1.;
    if(geometric)require(geometry->edge_lengths.size()==count && std::isfinite(length) && length>0 && std::isfinite(length*length) && length*length>0);
    const auto face_weight=[&](size_t f){return geometric?areas[f]/(length*length):1.;};
    const auto reverse_side=[&](size_t f,size_t e,size_t n){
        std::array<size_t,3> left{},right{};size_t left_count=0,right_count=0;
        for(size_t side=0;side<3;++side) {
            if(neighbors[f][side]==int32_t(n))left[left_count++]=side;
            if(neighbors[n][side]==int32_t(f))right[right_count++]=side;
        }
        require(left_count>0 && left_count==right_count);
        // Coincident triangles can share several edges with the same face.
        // Match their edge-length multisets instead of taking the first side.
        const auto sort_sides=[&](auto& sides,size_t face,size_t size){
            for(size_t side=0;side<size;++side)require(std::isfinite(geometry->edge_lengths[face][sides[side]]));
            std::sort(sides.begin(),sides.begin()+size,[&](size_t a,size_t b){
                const auto x=geometry->edge_lengths[face][a],y=geometry->edge_lengths[face][b];
                return x==y?a<b:x<y;
            });
        };
        sort_sides(left,f,left_count);sort_sides(right,n,right_count);
        return right[size_t(std::find(left.begin(),left.begin()+left_count,e)-left.begin())];
    };
    const auto edge_weight=[&](size_t f,size_t e){
        if(!geometric)return 1.;
        const double a=geometry->edge_lengths[f][e];const int32_t n=neighbors[f][e];
        if(n<0)return a/length;
        const size_t reverse=reverse_side(f,e,size_t(n));
        // Use one symmetric weight even for accepted rounding differences.
        return (a+double(geometry->edge_lengths[size_t(n)][reverse]))/(2.*length);
    };
    std::map<size_t,RegionRGB> palette;
    for(const auto& m:materials){check_color(m.color);require(m.slot!=missing && palette.emplace(m.slot,m.color).second);}
    const std::set<size_t> lips(options.lip_slots.begin(),options.lip_slots.end());
    for(size_t slot:lips)require(palette.count(slot));
    std::vector<uint8_t> rim(count,0);std::vector<size_t> frontier;
    for(size_t f=0;f<count;++f) {
        if((f&4095)==0)checkpoint();
        require(regions[f]>0 && slots[f]!=missing && selected[f]<=1 && pinned[f]<=1 &&
            uint8_t(roles[f])<=uint8_t(PortraitFaceRole::Other) && std::isfinite(areas[f]) && areas[f]>=0);check_color(original[f]);
        if(geometric)require(std::isfinite(face_weight(f)));
        for(size_t e=0;e<3;++e) {
            const int32_t n=neighbors[f][e];
            require(n>=-1 && (n<0 || (size_t(n)<count && size_t(n)!=f)));
            if(n>=0)require(std::find(neighbors[size_t(n)].begin(),neighbors[size_t(n)].end(),int32_t(f))!=neighbors[size_t(n)].end());
            if(geometric) {
                const double a=geometry->edge_lengths[f][e];require(std::isfinite(a) && a>=0 && std::isfinite(edge_weight(f,e)));
                if(n>=0) {
                    const size_t reverse=reverse_side(f,e,size_t(n));
                    const double b=geometry->edge_lengths[size_t(n)][reverse];
                    require(a>0 && std::isfinite(b) && b>0 && std::abs(a-b)<=1e-5*std::max(a,b));
                }
            }
        }
        if(roles[f]==PortraitFaceRole::Lips || roles[f]==PortraitFaceRole::Mouth){rim[f]=1;frontier.push_back(f);}
    }
    for(unsigned ring=0;ring<options.lip_boundary_guard_rings;++ring) {
        std::vector<size_t> next;
        for(size_t f:frontier)for(int32_t n:neighbors[f])if(n>=0 && !rim[size_t(n)]){rim[size_t(n)]=1;next.push_back(size_t(n));}
        frontier=std::move(next);checkpoint();
    }
    std::vector<size_t> proposed(count,missing);
    for(size_t f=0;f<count;++f) {
        if((f&4095)==0)checkpoint();
        if(!selected[f] || pinned[f] || rim[f] || !lips.count(slots[f]) || areas[f]<=0)continue;
        if(roles[f]!=PortraitFaceRole::Skin && roles[f]!=PortraitFaceRole::Eye && roles[f]!=PortraitFaceRole::Hair)continue;
        double best=std::numeric_limits<double>::infinity();size_t slot=missing;
        for(const auto& m:palette)if(!lips.count(m.first)) {
            const double error=distance(original[f],m.second);
            if(error<best){best=error;slot=m.first;}
        }
        if(slot!=missing && best<=distance(original[f],palette.at(slots[f]))+options.maximum_source_error_increase)proposed[f]=slot;
    }
    // Removing a correction from its donor must not turn the retained color into
    // disconnected pieces. Unknown/protected faces cannot be repainted to bridge
    // those pieces, so conservatively withdraw the correction instead.
    std::set<uint32_t> affected;
    std::vector<size_t> pending;
    for(size_t f=0;f<count;++f)if(proposed[f]!=missing){affected.insert(regions[f]);pending.push_back(f);}
    std::map<uint32_t,size_t> sizes,seeds;
    for(size_t f=0;f<count;++f)if(affected.count(regions[f])){++sizes[regions[f]];seeds.emplace(regions[f],f);}
    std::vector<size_t> visited(count,0),search;
    size_t stamp=0;
    for(const auto& seed:seeds) {
        ++stamp;search.assign(1,seed.second);visited[seed.second]=stamp;
        for(size_t at=0;at<search.size();++at) {
            if((at&4095)==0)checkpoint();
            for(int32_t n:neighbors[search[at]])if(n>=0 && regions[size_t(n)]==seed.first && visited[size_t(n)]!=stamp) {
                visited[size_t(n)]=stamp;search.push_back(size_t(n));
            }
        }
        require(search.size()==sizes.at(seed.first));
    }
    std::vector<uint8_t> removed(count,0);
    const auto donor_stays_connected=[&](size_t face) {
        std::vector<size_t> adjacent;
        for(int32_t n:neighbors[face])if(n>=0 && regions[size_t(n)]==regions[face] && !removed[size_t(n)] &&
            std::find(adjacent.begin(),adjacent.end(),size_t(n))==adjacent.end())adjacent.push_back(size_t(n));
        if(adjacent.size()<2)return true;
        ++stamp;search.assign(1,adjacent.front());visited[adjacent.front()]=stamp;
        for(size_t at=0;at<search.size() && at<4096;++at) {
            if((at&255)==0)checkpoint();
            for(int32_t n:neighbors[search[at]])if(n>=0 && size_t(n)!=face && !removed[size_t(n)] &&
                regions[size_t(n)]==regions[face] && visited[size_t(n)]!=stamp) {
                visited[size_t(n)]=stamp;search.push_back(size_t(n));
            }
            if(std::all_of(adjacent.begin(),adjacent.end(),[&](size_t n){return visited[n]==stamp;}))return true;
        }
        return false; // A long or absent connection is not sufficient evidence.
    };
    // Retry withdrawn faces after accepted leaf removals, until a fixed point.
    // This makes a second invocation idempotent on the same selection/roles.
    while(!pending.empty()) {
        std::vector<size_t> next;bool progress=false;
        for(size_t f:pending) {
            checkpoint();
            if(donor_stays_connected(f)){removed[f]=1;progress=true;}
            else next.push_back(f);
        }
        if(!progress){for(size_t f:next)proposed[f]=missing;break;}
        pending=std::move(next);
    }
    // Refine only the accepted mask. Donor connectivity and semantic protection
    // cannot change here. Prefer joining neighbouring real materials over
    // introducing isolated shades, within the same per-face source-loss bound.
    if(options.boundary_edge_penalty>0 && options.coherence_passes>0) {
        std::vector<size_t> faces,choices;
        for(size_t f=0;f<count;++f)if(proposed[f]!=missing)faces.push_back(f);
        for(const auto& color:palette)if(!lips.count(color.first))choices.push_back(color.first);
        std::vector<double> errors;errors.reserve(faces.size()*choices.size());
        std::vector<double> limits;limits.reserve(faces.size());
        for(size_t f:faces) {
            checkpoint();limits.push_back(distance(original[f],palette.at(slots[f]))+options.maximum_source_error_increase);
            for(size_t slot:choices)errors.push_back(distance(original[f],palette.at(slot)));
        }
        const auto score=[&](size_t f,size_t slot,double source_error) {
            double boundary=0;
            for(size_t e=0;e<3;++e) {
                const int32_t n=neighbors[f][e];
                if(n>=0 && (proposed[size_t(n)]==missing?slots[size_t(n)]:proposed[size_t(n)])!=slot)boundary+=edge_weight(f,e);
            }
            return source_error*face_weight(f)+options.boundary_edge_penalty*boundary;
        };
        std::vector<size_t> indices(count,missing);
        for(size_t i=0;i<faces.size();++i)indices[faces[i]]=i;
        std::vector<uint8_t> seen_component(count,0);
        std::vector<size_t> component_visit(count,0);size_t component_stamp=0;
        std::vector<size_t> component;
        for(unsigned pass=0;pass<options.coherence_passes;++pass) {
            bool changed=false;
            for(size_t at=0;at<faces.size();++at) {
                if((at&255)==0)checkpoint();
                const size_t index=pass%2==0?at:faces.size()-1-at,f=faces[index];
                const auto current=std::lower_bound(choices.begin(),choices.end(),proposed[f]);
                const size_t offset=index*choices.size();
                double best=score(f,proposed[f],errors[offset+size_t(current-choices.begin())]);size_t slot=proposed[f];
                for(size_t c=0;c<choices.size();++c) {
                    if(errors[offset+c]>limits[index])continue;
                    const double value=score(f,choices[c],errors[offset+c]);
                    if(value<best-1e-9){best=value;slot=choices[c];}
                }
                if(slot!=proposed[f]){proposed[f]=slot;changed=true;}
            }
            // A two-face (or larger) island may need a joint move: either
            // individual move creates an internal edge although the joint move
            // removes its exterior boundary. Use the same energy and hard
            // per-face limits, with no mesh-specific face-count threshold.
            if(options.component_coherence) {
                std::fill(seen_component.begin(),seen_component.end(),0);
                for(size_t seed:faces) {
                    if(seen_component[seed])continue;
                    const size_t old_slot=proposed[seed];
                    ++component_stamp;component.assign(1,seed);seen_component[seed]=1;component_visit[seed]=component_stamp;
                    std::set<size_t> adjacent_slots;
                    for(size_t at=0;at<component.size();++at) {
                        if((at&255)==0)checkpoint();
                        for(int32_t n:neighbors[component[at]])if(n>=0) {
                            const size_t face=size_t(n),slot=proposed[face]==missing?slots[face]:proposed[face];
                            if(proposed[face]==old_slot) {
                                // Include earlier blocks that now share this
                                // slot, otherwise their internal edges would be
                                // omitted from the energy of the next move.
                                if(component_visit[face]!=component_stamp){component_visit[face]=component_stamp;seen_component[face]=1;component.push_back(face);}
                            } else if(slot!=old_slot && palette.count(slot) && !lips.count(slot))adjacent_slots.insert(slot);
                        }
                    }
                    if(component.size()<2 || adjacent_slots.empty())continue;
                    const size_t old_index=size_t(std::lower_bound(choices.begin(),choices.end(),old_slot)-choices.begin());
                    double best_delta=0;size_t best_slot=old_slot;
                    for(size_t slot:adjacent_slots) {
                        const size_t candidate=size_t(std::lower_bound(choices.begin(),choices.end(),slot)-choices.begin());
                        double delta=0;bool feasible=true;
                        for(size_t at=0;at<component.size();++at) {
                            if((at&255)==0)checkpoint();
                            const size_t f=component[at],index=indices[f],offset=index*choices.size();
                            if(errors[offset+candidate]>limits[index]){feasible=false;break;}
                            delta+=(errors[offset+candidate]-errors[offset+old_index])*face_weight(f);
                            for(size_t e=0;e<3;++e) {
                                const int32_t n=neighbors[f][e];
                                if(n<0 || proposed[size_t(n)]==old_slot)continue;
                                const size_t other=proposed[size_t(n)]==missing?slots[size_t(n)]:proposed[size_t(n)];
                                delta+=options.boundary_edge_penalty*edge_weight(f,e)*(int(other!=slot)-int(other!=old_slot));
                            }
                        }
                        if(feasible && delta<best_delta-1e-9){best_delta=delta;best_slot=slot;}
                    }
                    if(best_slot!=old_slot){for(size_t f:component)proposed[f]=best_slot;changed=true;}
                }
            }
            if(!changed)break;
        }
    }
    std::vector<ColorRegionSplit> result;std::vector<uint8_t> seen(count,0);std::vector<size_t> queue;
    for(size_t seed=0;seed<count;++seed) {
        if((seed&4095)==0)checkpoint();
        if(seen[seed] || proposed[seed]==missing)continue;
        queue.assign(1,seed);seen[seed]=1;std::array<double,3> sum{};double area=0.;
        for(size_t at=0;at<queue.size();++at) {
            if((at&4095)==0)checkpoint();const size_t f=queue[at];area+=areas[f];
            for(size_t c=0;c<3;++c)sum[c]+=areas[f]*original[f][c];
            for(int32_t n:neighbors[f])if(n>=0 && !seen[size_t(n)] && regions[size_t(n)]==regions[seed] && proposed[size_t(n)]==proposed[seed]) {
                seen[size_t(n)]=1;queue.push_back(size_t(n));
            }
        }
        result.push_back({regions[seed],proposed[seed],{float(sum[0]/area),float(sum[1]/area),float(sum[2]/area),1},queue});
    }
    checkpoint();return result;
}
}
