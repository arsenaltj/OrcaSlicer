#include "BeautySurface.hpp"
#include "SurfaceSelectionState.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>

namespace Slic3r::AI {
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checkpoint(const std::function<bool()>& canceled) {
    if (canceled && canceled()) throw std::runtime_error("Beauty surface preparation cancelled.");
}
uint64_t edge_key(uint32_t a, uint32_t b) {
    return (uint64_t(std::min(a,b)) << 32) | std::max(a,b);
}
struct SurfaceEdge { int32_t first=-1, second=-1; uint8_t first_side=0, second_side=0, count=0; };
using SurfaceEdges = boost::unordered_flat_map<uint64_t,SurfaceEdge>;
void insert_edge(SurfaceEdges& edges,uint32_t x,uint32_t y,size_t face,int side) {
    if(x==y)return;
    auto& edge=edges[edge_key(x,y)];
    if(edge.count==0) {edge.first=int32_t(face);edge.first_side=uint8_t(side);}
    else if(edge.count==1) {edge.second=int32_t(face);edge.second_side=uint8_t(side);}
    if(edge.count<3)++edge.count;
}
struct VertexGraph {
    struct Edge { uint32_t first,second; float length; };
    std::vector<size_t> offsets;
    std::vector<uint32_t> neighbors;
    std::vector<float> lengths;
    std::vector<Edge> edges;
    void reserve(size_t vertices,size_t count) {offsets.assign(vertices+1,0);edges.reserve(count);}
    void add(const indexed_triangle_set& mesh,const std::vector<uint32_t>& representatives,uint64_t key) {
        const auto a=uint32_t(key>>32),b=uint32_t(key);
        ++offsets[a+1];++offsets[b+1];
        const float length=(mesh.vertices[representatives[a]]-mesh.vertices[representatives[b]]).norm();
        edges.push_back({a,b,length});
    }
    void finish(const std::function<bool()>& canceled={}) {
        for(size_t v=1;v<offsets.size();++v) {
            if((v&4095)==0)checkpoint(canceled);
            offsets[v]+=offsets[v-1];
        }
        checkpoint(canceled);
        neighbors.resize(offsets.back());lengths.resize(offsets.back());
        auto cursor=offsets;
        size_t at=0;
        for(const auto& edge:edges) {
            if((at++&4095)==0)checkpoint(canceled);
            const auto set=[&](uint32_t x,uint32_t y){const size_t at=cursor[x]++;neighbors[at]=y;lengths[at]=edge.length;};
            set(edge.first,edge.second);set(edge.second,edge.first);
        }
        std::vector<Edge>().swap(edges);
    }
};
VertexGraph prepare_vertex_graph(const indexed_triangle_set& mesh,const std::vector<uint32_t>& classes,
    const std::vector<uint32_t>& representatives,const std::function<bool()>& canceled) {
    checkpoint(canceled);
    VertexGraph graph;
    {
        SurfaceEdges edges;edges.reserve(mesh.indices.size()*2);
        for(size_t f=0;f<mesh.indices.size();++f) {
            if((f&4095)==0)checkpoint(canceled);
            const auto& face=mesh.indices[f];
            for(int side=0;side<3;++side)insert_edge(edges,classes[face[side]],classes[face[(side+1)%3]],f,side);
        }
        graph.reserve(representatives.size(),edges.size());
        size_t at=0;
        for(const auto& edge:edges) {
            if((at++&4095)==0)checkpoint(canceled);
            graph.add(mesh,representatives,edge.first);
        }
    }
    // Release the edge table before allocating distance rows, as in full build.
    graph.finish(canceled);checkpoint(canceled);return graph;
}
struct Position {
    std::array<uint32_t,3> bits;
    bool operator==(const Position& b) const { return bits == b.bits; }
};
struct PositionHash {
    size_t operator()(const Position& p) const {
        size_t h = p.bits[0];
        for (size_t i=1;i<3;++i) h ^= size_t(p.bits[i]) + 0x9e3779b9 + (h<<6) + (h>>2);
        return h;
    }
};
Position position_key(const Vec3f& p) {
    Position key;
    for (int i=0;i<3;++i) { float value=p[i] == 0.f ? 0.f : p[i]; std::memcpy(&key.bits[i],&value,4); }
    return key;
}
float ramp(double d, double radius) {
    const double t=std::clamp(d / radius, 0.0, 1.0);
    return float(t*t*(3-2*t));
}
std::vector<uint8_t> effective_mask(size_t count, const std::vector<uint8_t>& selected,
    const std::vector<uint8_t>& protection) {
    require(selected.size()==count && (protection.empty() || protection.size()==count),
        "Beauty selection does not match the current model.");
    std::vector<uint8_t> mask(count);
    for(size_t f=0;f<count;++f) {
        require(selected[f]<=1 && (protection.empty() || protection[f]<=1), "Invalid beauty selection mask.");
        mask[f]=selected[f] && (protection.empty() || !protection[f]);
    }
    require(std::find(mask.begin(),mask.end(),1)!=mask.end(), "Select an editable surface first.");
    return mask;
}
}

std::shared_ptr<BeautySurface> BeautySurface::build(const indexed_triangle_set& mesh,
    const std::vector<RGBA>& colors, const std::vector<uint32_t>& saved_partition,
    const std::function<bool()>& canceled)
{
    return build_impl(mesh,colors,saved_partition,canceled,true);
}
std::shared_ptr<BeautySurface> BeautySurface::build_for_appearance(const indexed_triangle_set& mesh,
    const std::vector<RGBA>& colors,const std::vector<uint32_t>& saved_partition,
    const std::function<bool()>& canceled)
{
    return build_impl(mesh,colors,saved_partition,canceled,false);
}
std::shared_ptr<BeautySurface> BeautySurface::build_impl(const indexed_triangle_set& mesh,
    const std::vector<RGBA>& colors,const std::vector<uint32_t>& saved_partition,
    const std::function<bool()>& canceled,bool vertex_graph)
{
    require(!mesh.vertices.empty() && !mesh.indices.empty() && mesh.indices.size()<=2000000 && mesh.vertices.size()<=6000000,
        "Beauty editing supports up to two million triangles.");
    require(colors.empty() || colors.size()==mesh.vertices.size(), "Beauty colors do not match model vertices.");
    require(saved_partition.empty() || saved_partition.size()==mesh.indices.size(), "Saved patches belong to a different model.");
    checkpoint(canceled);
    auto out=std::make_shared<BeautySurface>();
    out->geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    require(!out->geometry_id.empty(), "Unable to identify the beauty surface.");
    // Only the first occurrence is kept. The temporary position lookup is never
    // iterated, so contiguous buckets preserve class and downstream edge order.
    {
        boost::unordered_flat_map<Position,uint32_t,PositionHash> welded;
        welded.reserve(mesh.vertices.size());
        out->vertex_class.resize(mesh.vertices.size());
        for(size_t v=0;v<mesh.vertices.size();++v) {
            if((v&4095)==0) checkpoint(canceled);
            require(mesh.vertices[v].allFinite(), "Beauty mesh contains non-finite positions.");
            const auto inserted=welded.emplace(position_key(mesh.vertices[v]),uint32_t(out->class_representative.size()));
            if(inserted.second) out->class_representative.push_back(uint32_t(v));
            out->vertex_class[v]=inserted.first->second;
        }
    }
    const size_t faces=mesh.indices.size();
    out->face_neighbors.assign(faces,{-1,-1,-1});
    out->centers.resize(faces);out->normals.resize(faces);out->areas.resize(faces);
    out->face_edge_lengths.resize(faces);
    Vec3d lower=Vec3d::Zero(),upper=Vec3d::Zero();
    std::vector<std::array<double,3>> face_colors(faces);
    double total_area=0;
    {
        VertexGraph graph;
        const size_t vertices=out->class_representative.size();
        {
            SurfaceEdges edges;
            edges.reserve(faces*2);
            for(size_t f=0;f<faces;++f) {
                if((f&4095)==0) checkpoint(canceled);
                const auto& triangle=mesh.indices[f];
                for(int v:triangle) require(v>=0 && size_t(v)<mesh.vertices.size(), "Beauty mesh has invalid triangle indices.");
                const Vec3d a=mesh.vertices[triangle[0]].cast<double>(), b=mesh.vertices[triangle[1]].cast<double>(), c=mesh.vertices[triangle[2]].cast<double>();
                if(f==0)lower=upper=a;
                lower=lower.cwiseMin(a).cwiseMin(b).cwiseMin(c);upper=upper.cwiseMax(a).cwiseMax(b).cwiseMax(c);
                out->centers[f]=(a+b+c)/3.;
                const Vec3d n=(b-a).cross(c-a);
                const double length=n.norm();out->areas[f]=length*.5;total_area+=out->areas[f];
                out->normals[f]=length>1e-15 ? Vec3d(n/length) : Vec3d::Zero();
                for(int k=0;k<3;++k) {
                    const uint32_t x=out->vertex_class[triangle[k]], y=out->vertex_class[triangle[(k+1)%3]];
                    const double edge_length=(mesh.vertices[triangle[k]].cast<double>()-mesh.vertices[triangle[(k+1)%3]].cast<double>()).norm();
                    require(std::isfinite(edge_length) && edge_length<=std::numeric_limits<float>::max(),"Beauty edge length is invalid.");
                    out->face_edge_lengths[f][k]=float(edge_length);
                    insert_edge(edges,x,y,f,k);
                }
                for(int k=0;k<3;++k) for(int ch=0;ch<3;++ch) {
                    const float value=colors.empty()?0.5f:colors[triangle[k]][ch];
                    require(std::isfinite(value) && value>=0 && value<=1, "Beauty source color is invalid.");
                    face_colors[f][ch]+=value/3.;
                }
            }
            // Preserve edge iteration order, and release the table before rows.
            if(vertex_graph)graph.reserve(vertices,edges.size());
            for(const auto& entry:edges) {
                const auto& edge=entry.second;
                if(edge.count==1) ++out->boundary_edges;
                else if(edge.count>2) ++out->nonmanifold_edges;
                if(edge.count==2) {
                    out->face_neighbors[edge.first][edge.first_side]=edge.second;
                    out->face_neighbors[edge.second][edge.second_side]=edge.first;
                }
                if(vertex_graph)graph.add(mesh,out->class_representative,entry.first);
            }
        }
        if(vertex_graph) {
            graph.finish();
            out->neighbor_offsets=std::move(graph.offsets);out->vertex_neighbors=std::move(graph.neighbors);
            out->edge_lengths=std::move(graph.lengths);
        }
    }
    out->geometry_extent=(upper-lower).norm();
    require(std::isfinite(out->geometry_extent),"Beauty model extent is invalid.");
    const uint32_t unassigned=std::numeric_limits<uint32_t>::max();
    out->face_patch.assign(faces,unassigned);
    if(!saved_partition.empty()) {
        uint32_t max_id=0;
        for(uint32_t id:saved_partition) {require(id<faces,"Saved patch ID is invalid.");max_id=std::max(max_id,id);}
        out->patches.resize(size_t(max_id)+1);
        out->face_patch=saved_partition;
        for(size_t f=0;f<faces;++f)out->patches[saved_partition[f]].faces.push_back(f);
        for(const auto& patch:out->patches)require(!patch.faces.empty(),"Saved patch partition is incomplete.");
    } else {
        // Area-based size avoids tying a human-sized patch to source tessellation.
        const double target_area=std::max(total_area/3000.,1e-8);
        std::vector<size_t> queue;
        for(size_t seed=0;seed<faces;++seed) {
            if((seed&4095)==0)checkpoint(canceled);
            if(out->face_patch[seed]!=unassigned)continue;
            const uint32_t id=uint32_t(out->patches.size());out->patches.emplace_back();
            auto& patch=out->patches.back();queue={seed};out->face_patch[seed]=id;
            double area=out->areas[seed];
            for(size_t at=0;at<queue.size();++at) {
                const size_t f=queue[at];patch.faces.push_back(f);
                for(int32_t neighbor:out->face_neighbors[f]) {
                    if(neighbor<0 || out->face_patch[neighbor]!=unassigned || queue.size()>=512 || area>=target_area)continue;
                    if(out->normals[seed].dot(out->normals[neighbor])<.90)continue;
                    double color_distance=0;
                    for(int ch=0;ch<3;++ch) {
                        const double delta=face_colors[seed][ch]-face_colors[neighbor][ch];
                        color_distance+=delta*delta;
                    }
                    if(color_distance>.0225)continue;
                    out->face_patch[neighbor]=id;queue.push_back(size_t(neighbor));area+=out->areas[neighbor];
                }
            }
        }
    }
    for(size_t p=0;p<out->patches.size();++p) {
        if((p&255)==0)checkpoint(canceled);
        auto& patch=out->patches[p];
        for(size_t f:patch.faces) {
            const double weight=out->areas[f];patch.area+=weight;
            patch.center+=out->centers[f]*weight;patch.normal+=out->normals[f]*weight;
            for(int ch=0;ch<3;++ch)patch.mean_color[ch]+=face_colors[f][ch]*weight;
            for(int32_t n:out->face_neighbors[f])if(n>=0 && out->face_patch[n]!=p)patch.neighbors.push_back(out->face_patch[n]);
        }
        if(patch.area>0) {patch.center/=patch.area;for(double& c:patch.mean_color)c/=patch.area;}
        if(patch.normal.norm()>1e-12)patch.normal.normalize();
        std::sort(patch.neighbors.begin(),patch.neighbors.end());
        patch.neighbors.erase(std::unique(patch.neighbors.begin(),patch.neighbors.end()),patch.neighbors.end());
    }
    return out;
}

std::vector<float> BeautySurface::face_weights(const indexed_triangle_set& mesh,
    const std::vector<uint8_t>& selected,const std::vector<uint8_t>& protection,double radius) const
{
    require(face_patch.size()==mesh.indices.size(),"Beauty patches do not match the model.");
    require(std::isfinite(radius) && radius>=0 && radius<=50,"Invalid appearance feather radius.");
    auto mask=effective_mask(face_patch.size(),selected,protection);
    std::vector<float> result(mask.begin(),mask.end());
    if(radius==0)return result;
    std::vector<double> distance(mask.size(),std::numeric_limits<double>::infinity());
    using Entry=std::pair<double,size_t>;std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
    for(size_t f=0;f<mask.size();++f)if(mask[f]) {
        for(int32_t n:face_neighbors[f])if(n<0 || !mask[n]) {
            const double d=n<0?0.:(centers[f]-centers[n]).norm()*.5;
            if(d<distance[f]) {distance[f]=d;queue.emplace(d,f);}
        }
    }
    while(!queue.empty()) {
        const auto [d,f]=queue.top();queue.pop();if(d!=distance[f] || d>=radius)continue;
        for(int32_t n:face_neighbors[f])if(n>=0 && mask[n]) {
            const double next=d+(centers[f]-centers[n]).norm();
            if(next<distance[n]) {distance[n]=next;queue.emplace(next,size_t(n));}
        }
    }
    for(size_t f=0;f<mask.size();++f)if(mask[f])result[f]=ramp(distance[f],radius);
    return result;
}

std::vector<float> BeautySurface::vertex_weights(const indexed_triangle_set& mesh,
    const std::vector<uint8_t>& selected,const std::vector<uint8_t>& protection,double radius) const
{
    return vertex_weights_impl(mesh,selected,protection,radius,{});
}
std::vector<float> BeautySurface::vertex_weights_impl(const indexed_triangle_set& mesh,
    const std::vector<uint8_t>& selected,const std::vector<uint8_t>& protection,double radius,
    const std::function<bool()>& canceled) const
{
    require(vertex_class.size()==mesh.vertices.size() && face_patch.size()==mesh.indices.size(),"Beauty surface does not match geometry.");
    require(std::isfinite(radius) && radius>0 && radius<=50,"Choose a positive geometry falloff radius.");
    const auto mask=effective_mask(mesh.indices.size(),selected,protection);
    // Local ownership keeps an appearance surface immutable for other workers.
    const auto graph=neighbor_offsets.empty()?prepare_vertex_graph(mesh,vertex_class,class_representative,canceled):VertexGraph{};
    const auto& offsets=neighbor_offsets.empty()?graph.offsets:neighbor_offsets;
    const auto& neighbors=neighbor_offsets.empty()?graph.neighbors:vertex_neighbors;
    const auto& lengths=neighbor_offsets.empty()?graph.lengths:edge_lengths;
    std::vector<uint8_t> inside(class_representative.size(),0),blocked(inside.size(),0);
    for(size_t f=0;f<mask.size();++f)for(int v:mesh.indices[f]) {
        (mask[f]?inside:blocked)[vertex_class[v]]=1;
    }
    std::vector<double> distance(inside.size(),std::numeric_limits<double>::infinity());
    using Entry=std::pair<double,size_t>;std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
    for(size_t v=0;v<inside.size();++v)if(inside[v] && blocked[v]) {distance[v]=0;queue.emplace(0,v);}
    while(!queue.empty()) {
        const auto [d,v]=queue.top();queue.pop();if(d!=distance[v] || d>=radius)continue;
        for(size_t e=offsets[v];e<offsets[v+1];++e) {
            const size_t n=neighbors[e];if(!inside[n])continue;
            const double next=d+lengths[e];
            if(next<distance[n]) {distance[n]=next;queue.emplace(next,n);}
        }
    }
    std::vector<float> result(mesh.vertices.size(),0);
    for(size_t v=0;v<result.size();++v) {
        const size_t id=vertex_class[v];if(inside[id] && !blocked[id])result[v]=ramp(distance[id],radius);
    }
    return result;
}

indexed_triangle_set BeautySurface::deform(const indexed_triangle_set& mesh,
    const std::vector<uint8_t>& selected,const std::vector<uint8_t>& protection,
    double displacement,double radius,size_t& moved,const std::function<bool()>& canceled) const
{
    require(geometry_id==SurfaceSelectionPersistence::geometry_fingerprint(mesh),"The geometry changed; reload the beauty surface.");
    require(std::isfinite(displacement) && std::abs(displacement)<=2.,"The geometry trial is limited to two millimetres.");
    auto weights=vertex_weights_impl(mesh,selected,protection,radius,canceled);
    const auto mask=effective_mask(mesh.indices.size(),selected,protection);
    Vec3d direction=Vec3d::Zero();
    for(size_t f=0;f<mask.size();++f)if(mask[f])direction+=normals[f]*areas[f];
    require(direction.norm()>1e-10,"Select one side of the surface for a directional geometry trial.");direction.normalize();
    indexed_triangle_set out=mesh;moved=0;
    for(size_t v=0;v<mesh.vertices.size();++v) {
        if((v&4095)==0)checkpoint(canceled);
        if(weights[v]>0 && displacement!=0) {
            out.vertices[v]+=(direction*(displacement*weights[v])).cast<float>();
            if(out.vertices[v]!=mesh.vertices[v])++moved;
        }
    }
    require(moved>0,"Selection has no movable interior. Expand it or reduce the falloff radius.");
    for(size_t f=0;f<mesh.indices.size();++f) {
        if((f&4095)==0)checkpoint(canceled);
        if(!mask[f] || areas[f]<1e-12)continue;
        const auto& t=out.indices[f];const Vec3d a=out.vertices[t[0]].cast<double>(),b=out.vertices[t[1]].cast<double>(),c=out.vertices[t[2]].cast<double>();
        const Vec3d normal=(b-a).cross(c-a);
        require(normal.dot(normals[f])>areas[f]*.1,"This displacement would fold or collapse the surface. Reduce its amount.");
    }
    return out;
}
} // namespace Slic3r::AI
