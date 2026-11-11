#pragma once
#include "ModelOriginalAppearance.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include <unordered_map>
#include <numeric>
#include <memory>
#include <cmath>

namespace Slic3r::GUI {

// This sampler keeps the original GLB's textures and seams alive across a
// topology repair. Unchanged triangles use exact corner correspondence; only
// newly repaired triangles need a closest-surface query. Nothing is baked into
// an averaged vertex color before the color partition is established.
class ModelOriginalSurfaceSampler {
    using SC = AI::SemanticColoring::Color;
    using Bary = AI::SemanticColoring::Barycentric;
    struct Binding { uint32_t face {UINT32_MAX}; std::array<uint8_t,3> corners {0,1,2}; };
    using Key = std::array<float,9>;
    struct Hash {
        size_t operator()(const Key& key) const {
            size_t value = 0;
            for (float c : key) value ^= std::hash<float>{}(c) + size_t(0x9e3779b9) + (value<<6) + (value>>2);
            return value;
        }
    };
    TriangleMesh m_origin;
    ObjInfo m_colors;
    AI::ModelArtifactTextureSurface m_texture;
    std::vector<Binding> m_binding;
    std::map<size_t,std::array<Vec3f,3>> m_changed;
    AABBTreeIndirect::Tree3f m_tree;
    static Key key(const indexed_triangle_set& mesh, size_t face) {
        std::array<Vec3f,3> p;
        for (int i=0;i<3;++i) p[i]=mesh.vertices[mesh.indices[face][i]];
        std::sort(p.begin(),p.end(),[](const auto& a,const auto& b) {
            return std::tie(a.x(),a.y(),a.z()) < std::tie(b.x(),b.y(),b.z());
        });
        Key result;
        for (int i=0;i<3;++i) for (int c=0;c<3;++c) result[i*3+c]=p[i][c];
        return result;
    }
    static int texel(int value, int size, int wrap) {
        if (wrap == 33071) return std::clamp(value,0,size-1);
        const int period = wrap == 33648 ? size*2 : size;
        value = ((value%period)+period)%period;
        return value<size ? value : period-1-value;
    }
    static float linear(float c) { return c<=.04045f ? c/12.92f : std::pow((c+.055f)/1.055f,2.4f); }
    static float srgb(float c) { return c<=.0031308f ? c*12.92f : 1.055f*std::pow(c,1.f/2.4f)-.055f; }
public:
    const TriangleMesh& origin_mesh() const noexcept { return m_origin; }
    const ObjInfo& origin_colors() const noexcept { return m_colors; }
    const AI::ModelArtifactTextureSurface& origin_texture_surface() const noexcept { return m_texture; }

    AI::ModelArtifactTextureSurface texture_surface() const {
        if (m_texture.faces.empty()) return {};
        AI::ModelArtifactTextureSurface result;
        result.images=m_texture.images; result.faces.resize(m_binding.size());
        for (size_t face=0;face<m_binding.size();++face) {
            const auto& binding=m_binding[face];
            if (binding.face!=UINT32_MAX) {
                auto& out=result.faces[face];out=m_texture.faces[binding.face];
                for (int c=0;c<3;++c) out.corners[c]=m_texture.faces[binding.face].corners[binding.corners[c]];
            } else for (int c=0;c<3;++c) {
                Bary bary{};bary[c]=1;
                const auto color=sample(face,bary);
                for (int k=0;k<3;++k) result.faces[face].corners[c].multiplier[k]=linear(color[k]);
            }
        }
        return result;
    }
    static std::shared_ptr<ModelOriginalSurfaceSampler> load(const boost::filesystem::path& origin,
        const std::string& hash, const indexed_triangle_set& current, std::string& error,
        const AI::SemanticColoring::Cancel& cancel = {}, const TriangleMesh* decoded_mesh = nullptr,
        const ObjInfo* decoded_colors = nullptr, const AI::ModelArtifactTextureSurface* decoded_texture = nullptr) {
        if (AI::model_artifact_sha256(origin)!=hash) {error="原始外观模型身份已变化。";return {};}
        auto result=std::make_shared<ModelOriginalSurfaceSampler>();
        if (decoded_mesh && decoded_colors && decoded_texture) {
            result->m_origin=*decoded_mesh;result->m_colors=*decoded_colors;result->m_texture=*decoded_texture;
        } else if (!AI::load_model_artifact(origin,result->m_origin,result->m_colors,error,cancel,&result->m_texture)) return {};
        if (AI::model_artifact_sha256(origin)!=hash) {error="原始外观模型在解码期间发生变化。";return {};}
        if (!original_appearance_detail::has_appearance(result->m_origin.its,result->m_colors,result->m_texture)) {
            error="原始模型没有可采样的外观。";return {};
        }
        result->m_binding.resize(current.indices.size());
        const auto& source=result->m_origin.its;
        const bool ordered=AI::SurfaceSelectionPersistence::geometry_fingerprint(source)==
            AI::SurfaceSelectionPersistence::geometry_fingerprint(current);
        std::unordered_map<Key,uint32_t,Hash> faces;
        if (!ordered) {
            faces.reserve(source.indices.size());
            for (uint32_t f=0;f<source.indices.size();++f) {
                if ((f&4095)==0 && cancel && cancel()) {error="原始外观映射已取消。";return {};}
                faces.emplace(key(source,f),f);
            }
        }
        for (size_t f=0;f<current.indices.size();++f) {
            if ((f&4095)==0 && cancel && cancel()) {error="原始外观映射已取消。";return {};}
            auto& binding=result->m_binding[f];
            if (ordered) binding.face=uint32_t(f);
            else if (const auto found=faces.find(key(current,f));found!=faces.end()) binding.face=found->second;
            if (binding.face!=UINT32_MAX) {
                for (int c=0;c<3;++c) for (int s=0;s<3;++s)
                    if (current.vertices[current.indices[f][c]]==source.vertices[source.indices[binding.face][s]])
                        binding.corners[c]=uint8_t(s);
            } else {
                auto& p=result->m_changed[f];
                for (int c=0;c<3;++c) p[c]=current.vertices[current.indices[f][c]];
            }
        }
        if (!result->m_changed.empty()) result->m_tree=AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(source.vertices,source.indices);
        return result;
    }
    SC sample(size_t face, const Bary& input) const {
        const auto& source=m_origin.its;
        const auto& binding=m_binding.at(face);
        Bary bary{};
        size_t origin_face=binding.face;
        if (origin_face!=UINT32_MAX) {
            for (int c=0;c<3;++c) bary[binding.corners[c]]+=input[c];
        } else {
            const auto& p=m_changed.at(face);
            const Vec3d query=(input[0]*p[0]+input[1]*p[1]+input[2]*p[2]).cast<double>();
            Vec3d nearest;
            const double distance=AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                source.vertices,source.indices,m_tree,query,origin_face,nearest);
            if (distance<0 || origin_face>=source.indices.size()) throw std::runtime_error("修复表面的原始外观映射失败。");
            const auto& tri=source.indices[origin_face];
            const Vec3d a=source.vertices[tri[0]].cast<double>(),u=source.vertices[tri[1]].cast<double>()-a,
                v=source.vertices[tri[2]].cast<double>()-a,q=nearest-a;
            const double d=u.dot(u)*v.dot(v)-u.dot(v)*u.dot(v);
            if (d<=0) throw std::runtime_error("原始外观来源面退化。");
            bary[1]=float(std::clamp((v.dot(v)*q.dot(u)-u.dot(v)*q.dot(v))/d,0.,1.));
            bary[2]=float(std::clamp((u.dot(u)*q.dot(v)-u.dot(v)*q.dot(u))/d,0.,1.-bary[1]));
            bary[0]=1-bary[1]-bary[2];
        }
        SC color{};
        if (m_texture.faces.size()==source.indices.size()) {
            const auto& f=m_texture.faces[origin_face];
            std::array<float,2> uv{};SC factor{};
            for (int k=0;k<3;++k) {
                for (int c=0;c<2;++c) uv[c]+=bary[k]*f.corners[k].uv[c];
                for (int c=0;c<3;++c) factor[c]+=bary[k]*f.corners[k].multiplier[c];
            }
            color={1,1,1};
            if (f.image>=0 && size_t(f.image)<m_texture.images.size()) {
                const auto& image=m_texture.images[f.image];
                if (image.width<=0 || image.height<=0 || image.rgba.size()!=size_t(image.width)*image.height*4)
                    throw std::runtime_error("原始贴图数据无效。");
                const float x=uv[0]*image.width-.5f,y=uv[1]*image.height-.5f;
                const int ix=int(std::floor(x)),iy=int(std::floor(y));
                color={0,0,0};
                for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx) {
                    const float weight=(dx?x-ix:1-(x-ix))*(dy?y-iy:1-(y-iy));
                    const size_t offset=(size_t(texel(iy+dy,image.height,f.wrap_t))*image.width+texel(ix+dx,image.width,f.wrap_s))*4;
                    for (int c=0;c<3;++c) color[c]+=weight*linear(image.rgba[offset+c]/255.f);
                }
            }
            for (int c=0;c<3;++c) color[c]=std::clamp(srgb(color[c]*factor[c]),0.f,1.f);
        } else {
            const bool vertex=m_colors.vertex_colors.size()==source.vertices.size();
            for (int k=0;k<3;++k) for (int c=0;c<3;++c)
                color[c]+=bary[k]*(vertex?m_colors.vertex_colors[source.indices[origin_face][k]][c]:m_colors.face_colors[origin_face][c]);
        }
        return color;
    }
};
} // namespace Slic3r::GUI
