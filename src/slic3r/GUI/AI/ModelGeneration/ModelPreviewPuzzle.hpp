#pragma once
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyBoundaryContours.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "BeautySourceSnapshot.hpp"
#include <numeric>
#include <optional>
#include <string>

namespace Slic3r::GUI {
// Display-only boundaries; they never enter the printable mesh or texture.
struct ModelPreviewPuzzle {
    using BoundaryEdge=AI::BeautyBoundaryContours::Segment;
    struct CpuState {
        std::vector<BoundaryEdge> contours,active_edges;
        // Contours depend on geometry, adjacency and exact face partition, not palette colors.
        std::vector<uint32_t> contour_partition;
        std::string contour_geometry_id;
        const indexed_triangle_set* contour_mesh=nullptr;
        const AI::BeautySurface* contour_surface=nullptr;
        bool contour_cache_ready=false;
        // Workbench source colors are fixed for an installed surface. Cache their
        // packed display RGB across repaints; geometry/source replacement resets it.
        std::vector<uint32_t> packed_base_colors;
        const std::array<float,4>* packed_base_source=nullptr;
        const indexed_triangle_set* packed_base_mesh=nullptr;
        const AI::BeautySurface* packed_base_surface=nullptr;
        std::string packed_base_geometry_id;
    };
    CpuState cpu;
    std::unique_ptr<GLModel> fill,painted_fill,borders,active;
    static uint32_t packed_rgb(const std::array<float,4>& c) {
        return (uint32_t(std::lround(c[0]*255))<<16) |
               (uint32_t(std::lround(c[1]*255))<<8) | uint32_t(std::lround(c[2]*255));
    }
    // Screen-width ribbons work even on drivers that clamp GL line width to 1.
    // The normal attribute carries the opposite endpoint for shader expansion.
    static void add_stroke(GLModel::Geometry& geometry,const Vec3f& a,const Vec3f& b) {
        if((b-a).squaredNorm()<1e-20f)return;
        const auto first=unsigned(geometry.vertices_count());
        geometry.add_vertex(a,b,Vec2f(1,1));geometry.add_vertex(a,b,Vec2f(-1,1));
        geometry.add_vertex(b,a,Vec2f(1,-1));geometry.add_vertex(b,a,Vec2f(-1,-1));
        geometry.add_triangle(first,first+1,first+2);geometry.add_triangle(first,first+2,first+3);
    }
    void reset() {
        fill.reset(); painted_fill.reset(); borders.reset(); active.reset(); cpu.active_edges.clear();cpu.contours.clear();
        cpu.contour_partition.clear();cpu.contour_geometry_id.clear();cpu.contour_mesh=nullptr;cpu.contour_surface=nullptr;
        cpu.contour_cache_ready=false;
        cpu.packed_base_colors.clear();cpu.packed_base_source=nullptr;cpu.packed_base_mesh=nullptr;
        cpu.packed_base_surface=nullptr;cpu.packed_base_geometry_id.clear();
    }
    struct Frame {
        GLModel::Geometry colors,painted_colors,lines,highlight;
        std::optional<GLModel::PreparedGeometry> ready_colors,ready_painted_colors,ready_lines,ready_highlight;
        bool repaint=false,reuse_fill=false;
        size_t fill_vertices=0;
    };
    // Owned CPU arrays only: safe to discard on a worker without a GL context.
    struct Prepared {
        CpuState cache;
        Frame frame;
        std::map<uint32_t,std::array<float,4>> paint;
        std::vector<uint32_t> paint_partition;
        uint32_t selected=UINT32_MAX;
        bool custom_partition=false,valid=false;
        // A selection editor owns an exact copy of the immutable worker input.
        // Check all positions/indices before rebinding the cache to its live mesh.
        bool rebind_identical_mesh(const indexed_triangle_set& source, const indexed_triangle_set& target) {
            if(!valid || cache.contour_mesh!=&source || cache.packed_base_mesh!=&source)return false;
            if(!BeautySourceSnapshot::identical_mesh(source,target))return false;
            cache.contour_mesh=&target;cache.packed_base_mesh=&target;
            return true;
        }
        bool matches(const indexed_triangle_set& mesh,const std::vector<std::array<float,4>>& base_colors,
                     const AI::BeautySurface& surface,const AI::BeautyPuzzle& puzzle,uint32_t choice,
                     const std::vector<uint32_t>* edit_regions) const {
            if(!valid || cache.contour_mesh!=&mesh || cache.contour_surface!=&surface ||
               cache.contour_geometry_id!=puzzle.geometry_id || cache.packed_base_source!=base_colors.data() ||
               cache.packed_base_colors.size()!=base_colors.size() || paint!=puzzle.colors || selected!=choice ||
               custom_partition!=(edit_regions!=nullptr))return false;
            if(custom_partition && paint_partition!=puzzle.face_piece)return false;
            return cache.contour_partition==(edit_regions?*edit_regions:puzzle.face_piece);
        }
    };
    static Prepared prepare_initial(const indexed_triangle_set& mesh,
        const std::vector<std::array<float,4>>& base_colors,const AI::BeautySurface& surface,
        const AI::BeautyPuzzle& puzzle,uint32_t selected,
        const std::vector<uint32_t>* edit_regions=nullptr,const std::function<bool()>& canceled={}) {
        if(surface.geometry_id!=puzzle.geometry_id || puzzle.face_piece.size()!=mesh.indices.size() ||
           surface.normals.size()!=mesh.indices.size() || surface.face_neighbors.size()!=mesh.indices.size() ||
           (edit_regions && edit_regions->size()!=mesh.indices.size()))
            throw std::runtime_error("Puzzle display belongs to different geometry.");
        Prepared result;
        result.paint=puzzle.colors;result.selected=selected;result.custom_partition=edit_regions!=nullptr;
        if(edit_regions)result.paint_partition=puzzle.face_piece;
        result.frame=build(result.cache,mesh,base_colors,surface,puzzle,selected,true,edit_regions,false,0,0,canceled);
        auto& frame=result.frame;
        if(!frame.colors.is_empty())frame.ready_colors=GLModel::prepare_geometry(std::move(frame.colors),canceled);
        if(!frame.painted_colors.is_empty())frame.ready_painted_colors=GLModel::prepare_geometry(std::move(frame.painted_colors),canceled);
        if(!frame.lines.is_empty())frame.ready_lines=GLModel::prepare_geometry(std::move(frame.lines),canceled);
        if(!frame.highlight.is_empty())frame.ready_highlight=GLModel::prepare_geometry(std::move(frame.highlight),canceled);
        result.valid=true;
        return result;
    }
    bool install(Prepared& prepared,const indexed_triangle_set& mesh,
        const std::vector<std::array<float,4>>& base_colors,const AI::BeautySurface& surface,
        const AI::BeautyPuzzle& puzzle,uint32_t selected,const std::vector<uint32_t>* edit_regions=nullptr) {
        if(!prepared.matches(mesh,base_colors,surface,puzzle,selected,edit_regions))return false;
        prepared.valid=false;
        cpu=std::move(prepared.cache);
        publish(prepared.frame);
        return true;
    }
    void update(const indexed_triangle_set& mesh,const std::vector<std::array<float,4>>& base_colors,
                const AI::BeautySurface& surface,const AI::BeautyPuzzle& puzzle,
                uint32_t selected,bool repaint,const std::vector<uint32_t>* edit_regions=nullptr) {
        auto frame=build(cpu,mesh,base_colors,surface,puzzle,selected,repaint,edit_regions,bool(fill),
                         fill?fill->vertices_count():0,fill?fill->indices_count():0,{});
        publish(frame);
    }
private:
    static Frame build(CpuState& cache,const indexed_triangle_set& mesh,
        const std::vector<std::array<float,4>>& base_colors,const AI::BeautySurface& surface,
        const AI::BeautyPuzzle& puzzle,uint32_t selected,bool repaint,const std::vector<uint32_t>* edit_regions,
        bool has_fill,size_t existing_vertices,size_t existing_indices,const std::function<bool()>& canceled) {
        const auto checkpoint=[&] {
            if(canceled && canceled())throw std::runtime_error("Puzzle display preparation cancelled.");
        };
        checkpoint();
        Frame frame;
        auto& colors=frame.colors;auto& painted_colors=frame.painted_colors;auto& lines=frame.lines;auto& highlight=frame.highlight;
        cache.active_edges.clear();
        const auto& partition=edit_regions?*edit_regions:puzzle.face_piece;
        const bool rebuild=!cache.contour_cache_ready || cache.contour_mesh!=&mesh || cache.contour_surface!=&surface ||
            cache.contour_geometry_id!=puzzle.geometry_id || cache.contour_partition!=partition;
        const size_t fill_vertices=mesh.indices.size()*3;
        const bool reuse_fill=repaint && has_fill && !rebuild &&
            existing_vertices==fill_vertices && existing_indices==fill_vertices;
        std::vector<AI::BeautyBoundaryContours::Edge> edges;
        colors.format={GLModel::Geometry::EPrimitiveType::Triangles,GLModel::Geometry::EVertexLayout::P3N3T2};
        painted_colors.format=colors.format;
        lines.format=highlight.format={GLModel::Geometry::EPrimitiveType::Triangles,GLModel::Geometry::EVertexLayout::P3N3T2};
        if(repaint) {
            // P3N3T2 stores position, normal, then color/alpha in eight floats.
            colors.vertices.resize(mesh.indices.size()*3*8);
            // Attribute-only updates consume no new indices. Keep topology in
            // the existing GLModel instead of allocating/filling it per color.
            if(!reuse_fill)colors.indices.resize(fill_vertices);
            if(cache.packed_base_source!=base_colors.data() || cache.packed_base_colors.size()!=base_colors.size() ||
               cache.packed_base_mesh!=&mesh || cache.packed_base_surface!=&surface ||
               cache.packed_base_geometry_id!=puzzle.geometry_id) {
                cache.packed_base_colors.resize(base_colors.size());
                for(size_t vertex=0;vertex<base_colors.size();++vertex) {
                    if((vertex&4095)==0)checkpoint();
                    cache.packed_base_colors[vertex]=packed_rgb(base_colors[vertex]);
                }
                cache.packed_base_source=base_colors.data();cache.packed_base_mesh=&mesh;cache.packed_base_surface=&surface;
                cache.packed_base_geometry_id=puzzle.geometry_id;
            }
        }
        const uint32_t fallback_rgb=packed_rgb({.7f,.7f,.7f,1.f});
        for(size_t f=0;(repaint || rebuild) && f<mesh.indices.size();++f) {
            if((f&4095)==0)checkpoint();
            const auto& face=mesh.indices[f];const uint32_t id=puzzle.face_piece[f];
            if(repaint) {
                Vec3f n=surface.normals[f].cast<float>();
                const auto painted=puzzle.colors.find(id);const auto first=unsigned(f*3);
                const uint32_t paint_rgb=painted!=puzzle.colors.end()?packed_rgb(painted->second):0;
                for(int corner=0;corner<3;++corner) {
                    const size_t vertex_index=size_t(face[corner]);
                    const auto base_color=vertex_index<base_colors.size()?base_colors[vertex_index]:std::array<float,4>{.7f,.7f,.7f,1};
                    const uint32_t rgb=painted!=puzzle.colors.end()?paint_rgb:
                        vertex_index<cache.packed_base_colors.size()?cache.packed_base_colors[vertex_index]:fallback_rgb;
                    const auto& position=mesh.vertices[face[corner]];
                    float* vertex=colors.vertices.data()+size_t(first+corner)*8;
                    vertex[0]=position.x();vertex[1]=position.y();vertex[2]=position.z();
                    vertex[3]=n.x();vertex[4]=n.y();vertex[5]=n.z();
                    vertex[6]=float(rgb);vertex[7]=base_color[3];
                }
                if(painted!=puzzle.colors.end()) {
                    const auto start=unsigned(painted_colors.vertices_count());
                    for(int corner=0;corner<3;++corner)
                        painted_colors.add_vertex(mesh.vertices[face[corner]],n,Vec2f(float(paint_rgb),1));
                    painted_colors.add_triangle(start,start+1,start+2);
                }
                if(!reuse_fill) {
                    colors.indices[first]=first;colors.indices[first+1]=first+1;colors.indices[first+2]=first+2;
                }
            }
            if(rebuild) {
                const uint32_t edit_id=partition[f];
                for(int edge=0;edge<3;++edge) {
                    const int32_t other=surface.face_neighbors[f][edge];
                    if(other>=0 && partition[other]==edit_id)continue;
                    if(other>=0 && size_t(other)<f)continue;
                    const uint32_t adjacent=other>=0?partition[other]:UINT32_MAX;
                    edges.push_back({mesh.vertices[face[edge]],mesh.vertices[face[(edge+1)%3]],f,other,std::min(edit_id,adjacent),std::max(edit_id,adjacent)});
                }
            }
        }
        if(rebuild) {
            checkpoint();
            auto next=AI::BeautyBoundaryContours::build(edges,mesh,surface.face_neighbors,canceled);
            checkpoint();
            cache.contour_partition=partition;
            cache.contour_geometry_id=puzzle.geometry_id;
            cache.contour_mesh=&mesh;cache.contour_surface=&surface;
            cache.contours=std::move(next);cache.contour_cache_ready=true;
        }
        size_t stroke_count=0;
        for(const auto& edge:cache.contours) {
            if((stroke_count++&4095)==0)checkpoint();
            const bool chosen=edge.left==selected || (edge.right!=UINT32_MAX && edge.right==selected);
            if(edit_regions && !chosen)continue;
            auto& line=chosen?highlight:lines;add_stroke(line,edge.a,edge.b);
            if(chosen && edge.neighbor>=0)cache.active_edges.push_back(edge);
        }
        checkpoint();
        frame.repaint=repaint;frame.reuse_fill=reuse_fill;frame.fill_vertices=fill_vertices;
        return frame;
    }
    void publish(Frame& frame) {
        auto& colors=frame.colors;auto& painted_colors=frame.painted_colors;auto& lines=frame.lines;auto& highlight=frame.highlight;
        const bool repaint=frame.repaint,reuse_fill=frame.reuse_fill;
        const size_t fill_vertices=frame.fill_vertices;
        auto publish=[](GLModel::Geometry& geometry,std::optional<GLModel::PreparedGeometry>& prepared,
                        std::unique_ptr<GLModel>& target,const ColorRGBA& color) {
            target.reset();if(!prepared && geometry.is_empty())return;
            target=std::make_unique<GLModel>();
            if(prepared)target->init_from(std::move(*prepared));
            else target->init_from(std::move(geometry));
            target->set_color(color);
        };
        if(repaint) {
            if(!reuse_fill || !fill->update_vertex_attributes(colors.vertices)) {
                // A rejected attribute update must still publish a complete
                // model, including the unchanged linear triangle topology.
                if(!frame.ready_colors && colors.indices.empty()) {
                    colors.indices.resize(fill_vertices);
                    std::iota(colors.indices.begin(),colors.indices.end(),0u);
                }
                publish(colors,frame.ready_colors,fill,ColorRGBA(1.f,1.f,1.f,1.f));
            }
        }
        if(repaint) publish(painted_colors,frame.ready_painted_colors,painted_fill,ColorRGBA(1.f,1.f,1.f,1.f));
        publish(lines,frame.ready_lines,borders,ColorRGBA(.05f,.30f,.34f,1));
        publish(highlight,frame.ready_highlight,active,ColorRGBA(1,.42f,.02f,1));
    }
};
}
