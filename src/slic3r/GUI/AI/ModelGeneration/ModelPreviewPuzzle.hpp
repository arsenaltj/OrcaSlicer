#pragma once
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyBoundaryContours.hpp"
#include "slic3r/GUI/GLModel.hpp"

namespace Slic3r::GUI {
// Display-only boundaries; they never enter the printable mesh or texture.
struct ModelPreviewPuzzle {
    using BoundaryEdge=AI::BeautyBoundaryContours::Segment;
    std::vector<BoundaryEdge> contours,active_edges;
    std::unique_ptr<GLModel> fill, borders, active;
    // Screen-width ribbons work even on drivers that clamp GL line width to 1.
    // The normal attribute carries the opposite endpoint for shader expansion.
    static void add_stroke(GLModel::Geometry& geometry,const Vec3f& a,const Vec3f& b) {
        if((b-a).squaredNorm()<1e-20f)return;
        const auto first=unsigned(geometry.vertices_count());
        geometry.add_vertex(a,b,Vec2f(1,1));geometry.add_vertex(a,b,Vec2f(-1,1));
        geometry.add_vertex(b,a,Vec2f(1,-1));geometry.add_vertex(b,a,Vec2f(-1,-1));
        geometry.add_triangle(first,first+1,first+2);geometry.add_triangle(first,first+2,first+3);
    }
    void reset() { fill.reset(); borders.reset(); active.reset(); active_edges.clear();contours.clear(); }
    void update(const indexed_triangle_set& mesh, const std::vector<std::array<float,4>>& base_colors,
                const AI::BeautySurface& surface, const AI::BeautyPuzzle& puzzle,
                uint32_t selected, bool repaint, const std::vector<uint32_t>* edit_regions = nullptr) {
        GLModel::Geometry colors, lines, highlight;
        active_edges.clear();
        const bool rebuild=repaint || contours.empty();
        std::vector<AI::BeautyBoundaryContours::Edge> edges;
        colors.format={GLModel::Geometry::EPrimitiveType::Triangles,GLModel::Geometry::EVertexLayout::P3N3T2};
        lines.format=highlight.format={GLModel::Geometry::EPrimitiveType::Triangles,GLModel::Geometry::EVertexLayout::P3N3T2};
        if(repaint){colors.reserve_vertices(mesh.indices.size()*3);colors.reserve_indices(mesh.indices.size()*3);}
        for(size_t f=0;rebuild && f<mesh.indices.size();++f) {
            const auto& face=mesh.indices[f];const uint32_t id=puzzle.face_piece[f];
            const uint32_t edit_id=edit_regions?(*edit_regions)[f]:id;
            Vec3f n=surface.normals[f].cast<float>();
            if(repaint) {
                const auto painted=puzzle.colors.find(id);const auto first=unsigned(colors.vertices_count());
                for(int corner=0;corner<3;++corner) {
                    const auto base_color=size_t(face[corner])<base_colors.size()?base_colors[face[corner]]:std::array<float,4>{.7f,.7f,.7f,1};
                    auto c=painted!=puzzle.colors.end()?painted->second:base_color;c[3]=base_color[3];
                    const uint32_t rgb=(uint32_t(std::lround(c[0]*255))<<16) | (uint32_t(std::lround(c[1]*255))<<8) | uint32_t(std::lround(c[2]*255));
                    colors.add_vertex(mesh.vertices[face[corner]],n,Vec2f(float(rgb),c[3]));
                }
                colors.add_triangle(first,first+1,first+2);
            }
            for(int edge=0;edge<3;++edge) {
                const int32_t other=surface.face_neighbors[f][edge];
                if(other>=0 && (edit_regions?(*edit_regions)[other]:puzzle.face_piece[other])==edit_id)continue;
                if(other>=0 && size_t(other)<f)continue;
                const uint32_t adjacent=other>=0?(edit_regions?(*edit_regions)[other]:puzzle.face_piece[other]):UINT32_MAX;
                edges.push_back({mesh.vertices[face[edge]],mesh.vertices[face[(edge+1)%3]],f,other,std::min(edit_id,adjacent),std::max(edit_id,adjacent)});
            }
        }
        if(rebuild)contours=AI::BeautyBoundaryContours::build(edges,mesh,surface.face_neighbors);
        for(const auto& edge:contours) {
            const bool chosen=edge.left==selected || (edge.right!=UINT32_MAX && edge.right==selected);
            if(edit_regions && !chosen)continue;
            auto& line=chosen?highlight:lines;add_stroke(line,edge.a,edge.b);
            if(chosen && edge.neighbor>=0)active_edges.push_back(edge);
        }
        auto publish=[](GLModel::Geometry& geometry,std::unique_ptr<GLModel>& target,const ColorRGBA& color) {
            target.reset();if(geometry.is_empty())return;
            target=std::make_unique<GLModel>();target->init_from(std::move(geometry));target->set_color(color);
        };
        if(repaint)publish(colors,fill,ColorRGBA(1.f,1.f,1.f,1.f));
        publish(lines,borders,ColorRGBA(.05f,.30f,.34f,1));publish(highlight,active,ColorRGBA(1,.42f,.02f,1));
    }
};
}
