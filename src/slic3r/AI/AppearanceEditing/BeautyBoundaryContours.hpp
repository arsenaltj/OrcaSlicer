#pragma once
#include "libslic3r/TriangleMesh.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Slic3r::AI {
// A bounded display contour derived from a shared region border. Junctions stay
// fixed. The editable partition remains the authority; this removes its final
// sub-triangle staircase without changing mesh positions or saved color intent.
struct BeautyBoundaryContours {
    struct Edge { Vec3f a,b; size_t face; int32_t neighbor; uint32_t left,right; };
    struct Segment { Vec3f a,b; size_t face; int32_t neighbor; uint32_t left,right; float tolerance; Vec3f source_tangent=Vec3f::Zero(); int source_orientation{0}; };
    using Neighbors=std::vector<std::array<int32_t,3>>;

private:
    struct Cancellation {
        const std::function<bool()>& requested;
        size_t visited=0;
        void check() const {
            if(requested && requested())throw std::runtime_error("Puzzle display preparation cancelled.");
        }
        void visit() {
            if(requested && (++visited&4095)==0)check();
        }
    };
public:

    static Vec3f closest_triangle(const Vec3f& p,const Vec3f& a,const Vec3f& b,const Vec3f& c) {
        const Vec3f ab=b-a,ac=c-a,n=ab.cross(ac);
        if(n.squaredNorm()>1e-20f) {
            const Vec3f q=p-n*((p-a).dot(n)/n.squaredNorm());
            const float u=(b-q).cross(c-q).dot(n),v=(c-q).cross(a-q).dot(n),w=(a-q).cross(b-q).dot(n);
            if(u>=0 && v>=0 && w>=0)return q;
        }
        Vec3f best=a;float distance=(p-a).squaredNorm();
        for(const auto& edge:{std::pair<Vec3f,Vec3f>(a,b),{b,c},{c,a}}) {
            const Vec3f d=edge.second-edge.first;
            const Vec3f q=edge.first+d*std::clamp((p-edge.first).dot(d)/std::max(1e-20f,d.squaredNorm()),0.f,1.f);
            if((p-q).squaredNorm()<distance){best=q;distance=(p-q).squaredNorm();}
        }
        return best;
    }

    static std::vector<Vec3f> fair(const std::vector<Vec3f>& points,bool closed,const std::function<bool()>& canceled={}) {
        Cancellation cancellation{canceled};cancellation.check();
        if(points.size()<3)return points;
        Vec3f lo=points.front(),hi=lo;for(const auto& p:points){cancellation.visit();lo=lo.cwiseMin(p);hi=hi.cwiseMax(p);}
        const float envelope=.025f*(hi-lo).norm();
        auto result=points;
        for(int pass=0;pass<5;++pass) {
            cancellation.check();
            auto next=result;
            for(size_t i=0;i<points.size();++i) {
                cancellation.visit();
                if(!closed && (i==0 || i+1==points.size()))continue;
                const size_t prev=(i+points.size()-1)%points.size(),after=(i+1)%points.size();
                const float limit=std::min(envelope,1.25f*std::min((points[i]-points[prev]).norm(),(points[after]-points[i]).norm()));
                Vec3f delta=.5f*result[i]+.25f*(result[prev]+result[after])-points[i];
                if(delta.norm()>limit)delta*=limit/delta.norm();
                next[i]=points[i]+delta;
            }
            result.swap(next);
        }
        cancellation.check();
        return result;
    }

    static std::vector<Segment> build(const std::vector<Edge>& edges,const indexed_triangle_set& mesh,const Neighbors& neighbors,
                                      const std::function<bool()>& canceled={}) {
        Cancellation cancellation{canceled};cancellation.check();
        struct Key {
            float x,y,z;
            bool operator==(const Key& other) const {return x==other.x && y==other.y && z==other.z;}
        };
        struct KeyHash {
            size_t operator()(const Key& key) const {
                size_t hash=std::hash<float>{}(key.x);
                hash^=std::hash<float>{}(key.y)+0x9e3779b9+(hash<<6)+(hash>>2);
                hash^=std::hash<float>{}(key.z)+0x9e3779b9+(hash<<6)+(hash>>2);
                return hash;
            }
        };
        std::unordered_map<Key,size_t,KeyHash> vertices;
        vertices.reserve(edges.size()*2);
        std::vector<Vec3f> positions;
        std::vector<std::vector<size_t>> incident;
        std::vector<std::array<size_t,2>> endpoints;
        auto node=[&](const Vec3f& p) {
            const auto entry=vertices.emplace(Key{p.x(),p.y(),p.z()},positions.size());
            if(entry.second){positions.push_back(p);incident.emplace_back();}
            return entry.first->second;
        };
        for(size_t e=0;e<edges.size();++e) {
            cancellation.visit();
            const size_t a=node(edges[e].a),b=node(edges[e].b);endpoints.push_back({a,b});
            incident[a].push_back(e);incident[b].push_back(e);
        }
        auto continuation=[&](size_t v,size_t e)->size_t {
            if(incident[v].size()!=2)return edges.size();
            const size_t other=incident[v][0]==e?incident[v][1]:incident[v][0];
            return edges[other].left==edges[e].left && edges[other].right==edges[e].right ? other : edges.size();
        };
        std::vector<Segment> result;std::vector<uint8_t> visited(edges.size(),0);
        for(size_t seed=0;seed<edges.size();++seed) {
            cancellation.visit();
            if(visited[seed])continue;
            size_t start=endpoints[seed][0],edge=seed;
            // Walk back to the end of an open chain before traversing forward.
            size_t back=start,previous=seed;
            while(true) {
                cancellation.visit();
                const size_t next=continuation(back,previous);if(next==edges.size() || next==seed)break;
                back=endpoints[next][0]==back?endpoints[next][1]:endpoints[next][0];previous=next;
            }
            if(continuation(back,previous)==edges.size()){start=back;edge=previous;}
            std::vector<size_t> nodes{start},chain_edges;size_t current=start;bool closed=false;
            while(edge<edges.size() && !visited[edge]) {
                cancellation.visit();
                visited[edge]=1;chain_edges.push_back(edge);
                current=endpoints[edge][0]==current?endpoints[edge][1]:endpoints[edge][0];
                if(current==start){closed=true;break;}
                nodes.push_back(current);edge=continuation(current,edge);
            }
            std::vector<Vec3f> points;for(size_t v:nodes){cancellation.visit();points.push_back(positions[v]);}
            const auto smooth=fair(points,closed,canceled);
            std::vector<size_t> candidates;
            size_t candidate_node=std::numeric_limits<size_t>::max();
            auto emit=[&](const Vec3f& a,const Vec3f& b,size_t node_index,const Vec3f* projected_a=nullptr) -> Vec3f {
                cancellation.visit();
                const size_t ordinal=std::min(node_index,chain_edges.size()-1);
                const size_t edge_index=chain_edges[ordinal];const auto& source=edges[edge_index];
                // Orientation comes from the original chain traversal. A
                // smoothed corner can turn against its source edge's tangent;
                // that is curvature, not a reversal of the region's inside.
                const int source_orientation=endpoints[edge_index][0]==nodes[ordinal]?1:-1;
                if(candidate_node!=node_index) {
                    candidates.clear();
                    for(size_t e:incident[nodes[node_index]]) {
                        cancellation.visit();
                        candidates.push_back(edges[e].face);if(edges[e].neighbor>=0)candidates.push_back(size_t(edges[e].neighbor));
                    }
                    const size_t initial=candidates.size();
                    for(size_t k=0;k<initial;++k) {
                        cancellation.visit();
                        for(int32_t n:neighbors[candidates[k]])if(n>=0)candidates.push_back(size_t(n));
                    }
                    candidate_node=node_index;
                }
                const float spacing=std::max(1e-6f,(source.b-source.a).norm());
                auto on_surface=[&](const Vec3f& p) {
                    Vec3f best=p;float distance=std::numeric_limits<float>::infinity();
                    for(size_t f:candidates) {
                        cancellation.visit();
                        const auto& tri=mesh.indices[f];const Vec3f q=closest_triangle(p,mesh.vertices[tri[0]],mesh.vertices[tri[1]],mesh.vertices[tri[2]]);
                        if((q-p).squaredNorm()<distance){distance=(q-p).squaredNorm();best=q;}
                    }
                    return best;
                };
                const Vec3f surface_a=projected_a?*projected_a:on_surface(a);
                const Vec3f surface_b=on_surface(b);
                result.push_back({surface_a,surface_b,source.face,source.neighbor,source.left,source.right,.3f*spacing,source.b-source.a,source_orientation});
                return surface_b;
            };
            if(smooth.size()<3) {if(smooth.size()==2)emit(smooth[0],smooth[1],0);continue;}
            if(!closed)emit(smooth.front(),.5f*(smooth[0]+smooth[1]),0);
            for(size_t i=closed?0:1;i<(closed?smooth.size():smooth.size()-1);++i) {
                const Vec3f a=.5f*(smooth[(i+smooth.size()-1)%smooth.size()]+smooth[i]);
                const Vec3f b=.5f*(smooth[i]+smooth[(i+1)%smooth.size()]);Vec3f last=a;
                Vec3f projected_last;
                for(int part=1;part<=4;++part) {
                    const float t=part*.25f;const Vec3f next=(1-t)*(1-t)*a+2*t*(1-t)*smooth[i]+t*t*b;
                    projected_last=emit(last,next,i,part==1?nullptr:&projected_last);last=next;
                }
            }
            if(!closed)emit(.5f*(smooth[smooth.size()-2]+smooth.back()),smooth.back(),smooth.size()-1);
        }
        cancellation.check();
        return result;
    }
};
}
