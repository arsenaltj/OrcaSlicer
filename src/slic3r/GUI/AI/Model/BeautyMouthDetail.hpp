#pragma once
#include "BeautyGuidance.hpp"
#include "BeautySurface.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r::AI {
// Preserve a coherent light texture inside an already recognized mouth. This
// is not a teeth detector: closed, flat, saturated or weak evidence is skipped.
// Run only before a fresh automatic partition; never rewrite saved edits.
inline size_t beauty_refine_mouth_guidance(const BeautySurface& surface,
    const std::vector<RGBA>& source,BeautyGuidance& guidance) {
    const size_t count=source.size();
    if(!guidance.completed(count) || surface.areas.size()!=count || surface.face_neighbors.size()!=count)return 0;
    std::vector<uint8_t> member(count,0),seen(count,0),light(count,0);
    for(size_t f=0;f<count;++f) {
        const auto id=guidance.labels[f];
        member[f]=id>=0 && size_t(id)<guidance.names.size() && guidance.names[size_t(id)]=="imouth";
    }
    size_t changed=0;
    for(size_t seed=0;seed<count;++seed)if(member[seed] && !seen[seed]) {
        std::vector<size_t> mouth{seed};seen[seed]=1;const int32_t label=guidance.labels[seed];
        for(size_t i=0;i<mouth.size();++i)for(int32_t n:surface.face_neighbors[mouth[i]])
            if(n>=0 && member[size_t(n)] && !seen[size_t(n)] && guidance.labels[size_t(n)]==label) {
                seen[size_t(n)]=1;mouth.push_back(size_t(n));
            }
        if(mouth.size()<12)continue;
        std::array<double,64> histogram{};double area=0,sum=0;bool valid=true;
        const auto brightness=[&](size_t f){return .2126*source[f][0]+.7152*source[f][1]+.0722*source[f][2];};
        for(size_t f:mouth) {
            const double l=brightness(f),a=surface.areas[f];
            if(!std::isfinite(l) || !std::isfinite(a) || a<0){valid=false;break;}
            const int bin=std::min(63,int(std::clamp(l,0.,1.)*64));histogram[size_t(bin)]+=a;area+=a;sum+=a*bin;
        }
        if(!valid || area<=0)continue;
        double left=0,weighted=0,best=-1;int threshold=-1;
        for(int bin=0;bin<63;++bin) {
            left+=histogram[size_t(bin)];weighted+=histogram[size_t(bin)]*bin;const double right=area-left;
            if(left<area*.1 || right<area*.1)continue;
            const double separation=(sum-weighted)/right-weighted/left;
            if(separation<12.)continue;
            const double score=left*right*separation*separation;
            if(score>best){best=score;threshold=bin;}
        }
        if(threshold<0)continue;
        double light_area=0,bright_mean=0,chroma_mean=0;
        for(size_t f:mouth) {
            light[f]=std::min(63,int(std::clamp(brightness(f),0.,1.)*64))>threshold;
            if(!light[f])continue;
            const double a=surface.areas[f];light_area+=a;bright_mean+=a*brightness(f);
            const auto& c=source[f];chroma_mean+=a*(std::max({c[0],c[1],c[2]})-std::min({c[0],c[1],c[2]}));
        }
        // Do not turn dim or colored mouth textures into a teeth-like region.
        if(light_area<=0 || bright_mean/light_area<.65 || chroma_mean/light_area>.20)continue;
        std::set<size_t> visited;std::vector<size_t> largest;double largest_area=0;
        for(size_t f:mouth)if(light[f] && visited.insert(f).second) {
            std::vector<size_t> component{f};double a=0;
            for(size_t i=0;i<component.size();++i) {
                const size_t at=component[i];a+=surface.areas[at];
                for(int32_t n:surface.face_neighbors[at])if(n>=0 && light[size_t(n)] && guidance.labels[size_t(n)]==label && visited.insert(size_t(n)).second)
                    component.push_back(size_t(n));
            }
            if(a>largest_area){largest_area=a;largest=std::move(component);}
        }
        if(largest.size()<4 || largest_area<area*.1 || largest_area>area*.9 || largest_area<light_area*.7)continue;
        if(guidance.names.size()>=10001)break;
        // Keep the existing semantic vocabulary; this is a separate texture
        // part of the mouth, not a newly inferred anatomical label.
        const int32_t id=int32_t(guidance.names.size());guidance.names.push_back("imouth");
        for(size_t f:largest)guidance.labels[f]=id;
        changed+=largest.size();
    }
    return changed;
}
}
