#pragma once

#include "libslic3r/Triangulation.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>

namespace Slic3r::AI::SurfaceTriangulation {
// Partition coordinates are bounded by the unit source triangle at 1e9.
// Products of coordinate differences fit int64_t; do not subtract large
// floating point shoelace products when deciding whether an ear has area.
inline int64_t orientation(const Point& a, const Point& b, const Point& c) {
    return int64_t(b.x()-a.x())*int64_t(c.y()-a.y())-
           int64_t(b.y()-a.y())*int64_t(c.x()-a.x());
}
// Contours also occur far from the origin and may enclose half of one fixed
// coordinate square. Floating shoelace products lose that area by cancellation.
inline boost::multiprecision::int128_t signed_twice_area(const Polygon& polygon) {
    boost::multiprecision::int128_t sum=0;
    for(size_t i=0;i<polygon.points.size();++i) {
        const auto& a=polygon.points[i];const auto& b=polygon.points[(i+1)%polygon.points.size()];
        sum+=boost::multiprecision::int128_t(a.x())*b.y()-boost::multiprecision::int128_t(a.y())*b.x();
    }
    return sum;
}
inline void orient(Polygon& polygon,bool outer) {
    const auto area=signed_twice_area(polygon);
    if((outer && area<0) || (!outer && area>0)) polygon.reverse();
}
inline void check(bool value, const char* message) {
    if (!value) throw std::invalid_argument(message);
}
inline bool on_segment(const Point& p,const Point& a,const Point& b) {
    return orientation(a,b,p)==0 && p.x()>=std::min(a.x(),b.x()) && p.x()<=std::max(a.x(),b.x()) &&
           p.y()>=std::min(a.y(),b.y()) && p.y()<=std::max(a.y(),b.y());
}
inline Triangulation::Indices triangulate(const ExPolygon& polygon) {
    Points points;
    std::vector<uint32_t> original;
    std::map<std::pair<coord_t,coord_t>,size_t> unique;
    Triangulation::HalfEdges boundary;
    size_t offset=0;
    const auto ring=[&](const Polygon& contour) {
        check(contour.points.size()>=3,"Collapsed triangulation ring.");
        std::vector<uint32_t> ring_indices;
        for(size_t i=0;i<contour.points.size();++i) {
            const auto& p=contour.points[i];
            check(p.x()>=-20 && p.y()>=-20 && p.x()<=1000000020 && p.y()<=1000000020,
                  "Triangulation coordinates exceed source face.");
            const auto entry=unique.emplace(std::make_pair(p.x(),p.y()),points.size());
            if(entry.second) {points.push_back(p);original.push_back(uint32_t(offset+i));}
            ring_indices.push_back(uint32_t(entry.first->second));
        }
        for(size_t i=0;i<ring_indices.size();++i) {
            const auto a=ring_indices[i],b=ring_indices[(i+1)%ring_indices.size()];
            check(a!=b,"Collapsed triangulation boundary segment.");boundary.emplace_back(a,b);
        }
        offset+=contour.points.size();
    };
    ring(polygon.contour);for(const auto& hole:polygon.holes) ring(hole);
    // CGAL's input assertions are not a Release validation contract.
    for(size_t i=0;i<boundary.size();++i) for(size_t j=i+1;j<boundary.size();++j) {
        const auto e=boundary[i],f=boundary[j];
        const auto& a=points[e.first];const auto& b=points[e.second];
        const auto& c=points[f.first];const auto& d=points[f.second];
        const auto ab_c=orientation(a,b,c),ab_d=orientation(a,b,d),cd_a=orientation(c,d,a),cd_b=orientation(c,d,b);
        const auto opposite=[](int64_t x,int64_t y){return (x<0 && y>0)||(x>0 && y<0);};
        const auto interior=[](const Point& p,const Point& a,const Point& b){return p!=a && p!=b && on_segment(p,a,b);};
        check(!(opposite(ab_c,ab_d)&&opposite(cd_a,cd_b)) && !interior(c,a,b) &&
              !interior(d,a,b) && !interior(a,c,d) && !interior(b,c,d),
              "Triangulation rings intersect.");
    }
    std::sort(boundary.begin(),boundary.end());
    check(std::adjacent_find(boundary.begin(),boundary.end())==boundary.end(),"Repeated triangulation boundary segment.");
    for(const auto& e:boundary) check(!std::binary_search(boundary.begin(),boundary.end(),std::make_pair(e.second,e.first)),
        "Overlapping triangulation boundary segments.");
    auto triangles=Triangulation::triangulate(points,boundary);
    check(!triangles.empty(),"Constrained triangulation produced no faces.");
    std::map<std::pair<uint32_t,uint32_t>,int> edges;
    const auto edge=[&](uint32_t a,uint32_t b) { if(a<b) ++edges[{a,b}];else --edges[{b,a}]; };
    for(auto& t:triangles) {
        auto area=orientation(points[t[0]],points[t[1]],points[t[2]]);
        check(area!=0,"Constrained triangulation produced a zero area face.");
        if(area<0) std::swap(t[1],t[2]);
        while(t[0]>t[1] || t[0]>t[2]) { const auto first=t[0];t[0]=t[1];t[1]=t[2];t[2]=first; }
        for(size_t k=0;k<3;++k) edge(t[k],t[(k+1)%3]);
    }
    for(const auto& e:boundary) edge(e.second,e.first);
    for(const auto& e:edges) check(e.second==0,"Triangulation lost a boundary segment or created a seam.");
    for(auto& t:triangles) for(size_t i=0;i<3;++i) t[i]=int32_t(original[t[i]]);
    std::sort(triangles.begin(),triangles.end(),[](const auto& a,const auto& b) {
        return std::make_tuple(a[0],a[1],a[2])<std::make_tuple(b[0],b[1],b[2]);
    });
    return triangles;
}
} // namespace Slic3r::AI::SurfaceTriangulation
