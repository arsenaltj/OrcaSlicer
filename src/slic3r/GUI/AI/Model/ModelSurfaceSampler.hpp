#pragma once
#include "ModelArtifact.hpp"
#include "SurfaceSelectionState.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/MeshResourceBudget.hpp"
#include <deque>
#include <unordered_map>
#include <numeric>
#include <memory>
#include <optional>
#include <cmath>
#include <tuple>
#include <limits>

namespace Slic3r::AI {
// This sampler keeps the original GLB's textures and seams alive across a
// topology repair. Unchanged triangles use exact corner correspondence; only
// newly repaired triangles need a closest-surface query. Nothing is baked into
// an averaged vertex color before the color partition is established.
class ModelSurfaceSampler {
    using SC = std::array<float,3>;
    using Bary = std::array<float,3>;
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
    ModelArtifactTextureSurface m_texture;
    std::vector<Binding> m_binding;
    std::deque<Binding> m_large_binding;
    size_t binding_count() const { return m_binding.size() + m_large_binding.size(); }
    Binding& binding_at(size_t face) { return m_large_binding.empty() ? m_binding.at(face) : m_large_binding.at(face); }
    const Binding& binding_at(size_t face) const { return m_large_binding.empty() ? m_binding.at(face) : m_large_binding.at(face); }
    std::map<size_t,std::array<Vec3f,3>> m_changed;
    AABBTreeIndirect::Tree3f m_tree;
    std::vector<Vec3i32> m_projection_faces;
    std::vector<uint32_t> m_projection_sources;
    struct ProjectionChunk {
        std::vector<Vec3i32> faces;
        std::vector<uint32_t> sources;
        AABBTreeIndirect::Tree3f tree;
    };
    std::vector<ProjectionChunk> m_projection_chunks;
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
    const ModelArtifactTextureSurface& origin_texture_surface() const noexcept { return m_texture; }

    ModelArtifactTextureSurface texture_surface() const {
        if (m_texture.faces.empty()) return {};
        ModelArtifactTextureSurface result;
        result.images=m_texture.images; result.faces.resize(binding_count());
        for (size_t face=0;face<binding_count();++face) {
            const auto& binding=binding_at(face);
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
    static std::shared_ptr<ModelSurfaceSampler> load(const boost::filesystem::path& origin,
        const std::string& hash, const indexed_triangle_set& current, std::string& error,
        const std::function<bool()>& cancel = {}, const TriangleMesh* decoded_mesh = nullptr,
        const ObjInfo* decoded_colors = nullptr, const ModelArtifactTextureSurface* decoded_texture = nullptr) {
        if (model_artifact_sha256(origin)!=hash) {error="原始外观模型身份已变化。";return {};}
        auto result=std::make_shared<ModelSurfaceSampler>();
        if (decoded_mesh && decoded_colors && decoded_texture) {
            result->m_origin=*decoded_mesh;result->m_colors=*decoded_colors;result->m_texture=*decoded_texture;
        } else if (!load_model_artifact(origin,result->m_origin,result->m_colors,error,cancel,&result->m_texture)) return {};
        if (model_artifact_sha256(origin)!=hash) {error="原始外观模型在解码期间发生变化。";return {};}
        if (!(result->m_colors.vertex_colors.size()==result->m_origin.its.vertices.size() || result->m_colors.face_colors.size()==result->m_origin.its.indices.size() || result->m_texture.faces.size()==result->m_origin.its.indices.size())) {
            error="原始模型没有可采样的外观。";return {};
        }
        const auto& source=result->m_origin.its;
        const bool bounded = std::max(source.indices.size(), current.indices.size()) > 1000000;
        if (bounded) {
            if (uint64_t(current.indices.size()) * sizeof(Binding) > MeshResourceBudget::scratch_bytes)
                throw std::length_error("原始外观映射暂存资源超出预算；原件及修复成果保留。");
            result->m_large_binding.resize(current.indices.size());
        } else result->m_binding.resize(current.indices.size());
        const bool ordered=SurfaceSelectionPersistence::geometry_fingerprint(source)==
            SurfaceSelectionPersistence::geometry_fingerprint(current);
        std::optional<bool> same_vertex_storage;
        std::unordered_map<Key,std::vector<uint32_t>,Hash> faces;
        std::unordered_map<Key,size_t,Hash> occurrences;
        // The former partition domain ended at one million faces. Keep its
        // successful lookup path; larger inputs use compact block-allocated
        // hash/order records instead of a hash node and vector per triangle.
        struct FaceOrder { size_t hash; uint32_t face; uint32_t next {0}; };
        std::deque<FaceOrder> sorted_faces;
        const auto workspace = [&](uint64_t additional) {
            const uint64_t bytes = uint64_t(result->m_binding.capacity() + result->m_large_binding.size()) * sizeof(Binding) +
                uint64_t(result->m_changed.size()) * (sizeof(decltype(result->m_changed)::value_type) + 4 * sizeof(void*)) + additional;
            if (bytes > MeshResourceBudget::scratch_bytes)
                throw std::length_error("原始外观映射暂存资源超出预算；原件及修复成果保留。");
        };
        if (!ordered && bounded) {
            workspace(uint64_t(source.indices.size()) * sizeof(FaceOrder));
            for (uint32_t f = 0; f < source.indices.size(); ++f) {
                if ((f & 4095) == 0 && cancel && cancel()) { error="原始外观映射已取消。"; return {}; }
                sorted_faces.push_back({Hash{}(key(source, f)), f});
            }
            std::sort(sorted_faces.begin(), sorted_faces.end(), [](const auto& a, const auto& b) {
                return std::tie(a.hash, a.face) < std::tie(b.hash, b.face);
            });
        } else if (!ordered) {
            faces.reserve(source.indices.size());
            for (uint32_t f=0;f<source.indices.size();++f) {
                if ((f&4095)==0 && cancel && cancel()) {error="原始外观映射已取消。";return {};}
                faces[key(source,f)].push_back(f);
            }
        }
        for (size_t f=0;f<current.indices.size();++f) {
            if ((f&4095)==0 && cancel && cancel()) {error="原始外观映射已取消。";return {};}
            auto& binding=result->binding_at(f);
            if (ordered) binding.face=uint32_t(f);
            else if (bounded) {
                const auto target = key(current, f);
                const auto hash = Hash{}(target);
                const auto begin = std::lower_bound(sorted_faces.begin(), sorted_faces.end(), hash,
                    [](const auto& entry, size_t value) { return entry.hash < value; });
                auto first = sorted_faces.end();
                size_t count = 0;
                for (auto i = begin; i != sorted_faces.end() && i->hash == hash; ++i)
                    if (key(source, i->face) == target) { if (!count) first = i; ++count; }
                if (count) {
                    size_t occurrence = first->next++ % count;
                    for (auto i = first; i != sorted_faces.end() && i->hash == hash; ++i)
                        if (key(source, i->face) == target && occurrence-- == 0) { binding.face = i->face; break; }
                    if (count > 1) {
                        if (!same_vertex_storage) same_vertex_storage = source.vertices == current.vertices;
                        if (*same_vertex_storage) {
                            auto ids = current.indices[f]; std::sort(ids.data(), ids.data() + 3);
                            for (auto i = first; i != sorted_faces.end() && i->hash == hash; ++i) {
                                if (key(source, i->face) != target) continue;
                                auto original_ids = source.indices[i->face]; std::sort(original_ids.data(), original_ids.data() + 3);
                                if (ids == original_ids) { binding.face = i->face; break; }
                            }
                        }
                    }
                }
            }
            else if (const auto found=faces.find(key(current,f));found!=faces.end()) {
                // Coincident source faces can carry independent UV/material
                // seams. Consume their stable source order, never the first
                // face repeatedly. An exact ordered source uses the fast path.
                auto& occurrence=occurrences[found->first];
                binding.face=found->second[occurrence++ % found->second.size()];
                if (found->second.size() > 1) {
                    if (!same_vertex_storage) same_vertex_storage = source.vertices == current.vertices;
                    if (*same_vertex_storage) {
                        auto ids = current.indices[f];
                        std::sort(ids.data(), ids.data() + 3);
                        for (const auto candidate : found->second) {
                            auto source_ids = source.indices[candidate];
                            std::sort(source_ids.data(), source_ids.data() + 3);
                            if (ids == source_ids) { binding.face = candidate; break; }
                        }
                    }
                }
            }
            if (binding.face!=UINT32_MAX) {
                for (int c=0;c<3;++c) for (int s=0;s<3;++s)
                    if (current.vertices[current.indices[f][c]]==source.vertices[source.indices[binding.face][s]])
                        binding.corners[c]=uint8_t(s);
            } else {
                if (bounded) workspace(uint64_t(sorted_faces.size()) * sizeof(FaceOrder) +
                    sizeof(decltype(result->m_changed)::value_type) + 4 * sizeof(void*));
                auto& p=result->m_changed[f];
                for (int c=0;c<3;++c) p[c]=current.vertices[current.indices[f][c]];
            }
        }
        std::deque<FaceOrder>().swap(sorted_faces);
        if (bounded && !result->m_changed.empty()) {
            // Each acceleration block and its build input stay below 16 MiB.
            // The complete collection is also charged to the 256 MiB scratch
            // budget. Blocks share the immutable original vertex array.
            constexpr size_t per_chunk = 65536;
            uint64_t retained = 0;
            const auto finish_chunk = [&] {
                auto& chunk = result->m_projection_chunks.back();
                size_t power = 1; while (power < chunk.faces.size()) power *= 2;
                const uint64_t nodes = uint64_t(power * 2 - 1) * sizeof(AABBTreeIndirect::Tree3f::Node);
                const uint64_t input = uint64_t(chunk.faces.size()) * 128;
                if (nodes + input > MeshResourceBudget::block_bytes)
                    throw std::length_error("原始外观查询块超出资源预算。");
                const uint64_t arrays = uint64_t(chunk.faces.capacity()) * sizeof(Vec3i32) +
                    uint64_t(chunk.sources.capacity()) * sizeof(uint32_t);
                workspace(retained + arrays + nodes + input);
                chunk.tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(source.vertices, chunk.faces);
                retained += arrays + nodes;
            };
            for (uint32_t f = 0; f < source.indices.size(); ++f) {
                if ((f & 4095) == 0 && cancel && cancel()) { error="原始外观映射已取消。"; return {}; }
                const auto& tri = source.indices[f];
                const Vec3d u=(source.vertices[tri[1]]-source.vertices[tri[0]]).cast<double>();
                const Vec3d v=(source.vertices[tri[2]]-source.vertices[tri[0]]).cast<double>();
                const double area=u.cross(v).squaredNorm();
                if (!std::isfinite(area) || area <= 0.) continue;
                if (result->m_projection_chunks.empty() || result->m_projection_chunks.back().faces.size() == per_chunk) {
                    workspace(retained + uint64_t(per_chunk) * (sizeof(Vec3i32) + sizeof(uint32_t)));
                    result->m_projection_chunks.emplace_back();
                    auto& chunk = result->m_projection_chunks.back();
                    chunk.faces.reserve(per_chunk); chunk.sources.reserve(per_chunk);
                }
                auto& chunk = result->m_projection_chunks.back();
                chunk.faces.push_back(tri); chunk.sources.push_back(f);
                if (chunk.faces.size() == per_chunk) finish_chunk();
            }
            if (result->m_projection_chunks.empty()) { error="原始外观没有有效来源表面。"; return {}; }
            if (result->m_projection_chunks.back().faces.size() < per_chunk) finish_chunk();
            return result;
        }
        if (!result->m_changed.empty()) {
            for (uint32_t f=0;f<source.indices.size();++f) {
                const auto& tri=source.indices[f];
                const Vec3d u=(source.vertices[tri[1]]-source.vertices[tri[0]]).cast<double>();
                const Vec3d v=(source.vertices[tri[2]]-source.vertices[tri[0]]).cast<double>();
                const double area=u.cross(v).squaredNorm();
                if (!std::isfinite(area) || area<=0) continue;
                result->m_projection_faces.push_back(tri); result->m_projection_sources.push_back(f);
            }
            if (result->m_projection_faces.empty()) {error="原始外观没有有效来源表面。";return {};}
            result->m_tree=AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(source.vertices,result->m_projection_faces);
        }
        return result;
    }
    SC sample(size_t face, const Bary& input) const {
        const auto& source=m_origin.its;
        const auto& binding=binding_at(face);
        Bary bary{};
        size_t origin_face=binding.face;
        if (origin_face!=UINT32_MAX) {
            for (int c=0;c<3;++c) bary[binding.corners[c]]+=input[c];
        } else {
            const auto& p=m_changed.at(face);
            const Vec3d query=(input[0]*p[0]+input[1]*p[1]+input[2]*p[2]).cast<double>();
            Vec3d nearest;
            if (!m_projection_chunks.empty()) {
                double best = std::numeric_limits<double>::infinity();
                origin_face = size_t(-1);
                for (const auto& chunk : m_projection_chunks) {
                    Vec3d point; size_t index = size_t(-1);
                    const auto distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                        source.vertices, chunk.faces, chunk.tree, query, index, point);
                    if (!std::isfinite(distance) || distance < 0. || index >= chunk.sources.size() || !point.allFinite())
                        throw std::runtime_error("修复表面的原始外观映射失败。");
                    if (distance < best || (distance == best && chunk.sources[index] < origin_face)) {
                        best = distance; nearest = point; origin_face = chunk.sources[index];
                    }
                }
            } else {
                const double distance=AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                    source.vertices,m_projection_faces,m_tree,query,origin_face,nearest);
                if (!std::isfinite(distance) || distance<0 || origin_face>=m_projection_sources.size() || !nearest.allFinite())
                    throw std::runtime_error("修复表面的原始外观映射失败。");
                origin_face=m_projection_sources[origin_face];
            }
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
            if (f.image < -1 || (f.image>=0 && size_t(f.image)>=m_texture.images.size()))
                throw std::runtime_error("原始贴图引用无效，不能替换为默认颜色。");
            if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) throw std::runtime_error("原始纹理坐标无效。");
            if (f.image>=0) {
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
        for (float channel : color) if (!std::isfinite(channel) || channel<0 || channel>1)
            throw std::runtime_error("原始外观插值颜色无效。");
        return color;
    }
};
} // namespace Slic3r::AI
