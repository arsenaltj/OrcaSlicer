#pragma once
#include "BeautySurface.hpp"
#include <queue>

namespace Slic3r::AI {
// Texture-derived detail inside a verified eye mask, not another ML label or
// a claim to identify the anatomical pupil. Weak/flat textures remain unchanged.
struct BeautyEyeDetail {
    static std::vector<size_t> dark_region(const indexed_triangle_set& mesh,
        const std::vector<RGBA>& colors,const BeautySurface& surface,const std::vector<size_t>& eye) {
        const size_t count=mesh.indices.size();
        if(colors.size()!=mesh.vertices.size() || surface.areas.size()!=count ||
            surface.face_neighbors.size()!=count || eye.size()<8)return {};
        std::vector<uint8_t> member(count,0),dark(count,0),seen(count,0);
        std::vector<double> light(count,0.);
        std::array<double,64> histogram{};
        double area=0.,weighted=0.;
        for(size_t f:eye) {
            if(f>=count || member[f])return {};
            member[f]=1;
            const auto& tri=mesh.indices[f];double l=0.;
            for(int corner=0;corner<3;++corner) {
                const auto& c=colors[size_t(tri[corner])];
                l+=(.2126*c[0]+.7152*c[1]+.0722*c[2])/3.;
            }
            if(!std::isfinite(l))return {};
            light[f]=std::clamp(l,0.,1.);
            const double a=std::max(surface.areas[f],1e-15);
            const int bin=std::min(63,int(light[f]*64));
            histogram[size_t(bin)]+=a;area+=a;weighted+=a*bin;
        }
        double left=0.,sum=0.,best=-1.;int threshold=-1;
        for(int bin=0;bin<63;++bin) {
            left+=histogram[size_t(bin)];sum+=histogram[size_t(bin)]*bin;
            const double right=area-left;
            if(left<area*.1 || right<area*.1)continue;
            const double contrast=(weighted-sum)/right-sum/left;
            if(contrast<10.)continue;
            const double score=left*right*contrast*contrast;
            if(score>best){best=score;threshold=bin;}
        }
        if(threshold<0)return {};
        for(size_t f:eye)dark[f]=std::min(63,int(light[f]*64))<=threshold;
        std::vector<size_t> selected,component;double largest=0.;
        for(size_t seed:eye)if(dark[seed] && !seen[seed]) {
            component.assign(1,seed);seen[seed]=1;double a=0.;
            for(size_t at=0;at<component.size();++at) {
                const size_t f=component[at];a+=surface.areas[f];
                for(int32_t n:surface.face_neighbors[f])if(n>=0 && dark[size_t(n)] && !seen[size_t(n)]) {
                    seen[size_t(n)]=1;component.push_back(size_t(n));
                }
            }
            if(a>largest){largest=a;selected=component;}
        }
        if(selected.size()<4 || largest<area*.08 || largest>area*.85)return {};
        // Retain a small enclosed catchlight within the dark piece. Otherwise
        // changing its color leaves a hard-to-pick pinhole in the eye region.
        std::fill(dark.begin(),dark.end(),0);
        for(size_t f:selected)dark[f]=1;
        std::fill(seen.begin(),seen.end(),0);
        for(size_t seed:eye)if(!dark[seed] && !seen[seed]) {
            component.assign(1,seed);seen[seed]=1;double a=0.;bool outside=false;
            for(size_t at=0;at<component.size();++at) {
                const size_t f=component[at];a+=surface.areas[f];
                for(int32_t n:surface.face_neighbors[f]) {
                    if(n<0 || !member[size_t(n)]){outside=true;continue;}
                    if(!dark[size_t(n)] && !seen[size_t(n)]){seen[size_t(n)]=1;component.push_back(size_t(n));}
                }
            }
            if(!outside && a<largest*.08)selected.insert(selected.end(),component.begin(),component.end());
        }
        std::sort(selected.begin(),selected.end());
        return selected;
    }
};
}
