#pragma once

#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

namespace Slic3r::GUI {

// Parsers and geometry are independent of Orca's main-thread object IDs.
struct DecodedWorkbenchModel {
    TriangleMesh mesh;
    ObjInfo colors;
    std::shared_ptr<TexturedMesh> texture;
};

inline bool decode_workbench_model(const boost::filesystem::path& path, DecodedWorkbenchModel& decoded,
    std::string& error, const std::function<bool()>& canceled = {}, bool explicit_obj_colors = false)
{
    decoded = {};
    error.clear();
    auto stopped = [&] {
        if (!canceled || !canceled()) return false;
        decoded = {};
        error = "Model loading canceled.";
        return true;
    };
    if (stopped()) return false;
    if (boost::algorithm::iequals(path.extension().string(), ".obj")) {
        ObjParser::MtlData materials;
        if (!load_obj(path.string().c_str(), &decoded.mesh, decoded.colors, error, &materials, canceled)) return false;
        const auto& colors = decoded.colors;
        const auto& its = decoded.mesh.its;
        if (explicit_obj_colors && (!colors.vertex_colors.empty() || (!colors.face_colors.empty() && !colors.has_uv_png)))
            return !stopped();
        if (colors.has_uv_png && !colors.uvs.empty()) {
            auto texture = std::make_shared<TexturedMesh>();
            if (obj_to_textured_mesh(colors, its, materials, path.parent_path().string(), *texture))
                decoded.texture = std::move(texture);
        } else if (!colors.vertex_colors.empty() || !colors.face_colors.empty()) {
            auto texture = std::make_shared<TexturedMesh>();
            texture->vertices.reserve(its.vertices.size());
            texture->indices.reserve(its.indices.size());
            for (const auto& vertex : its.vertices) texture->vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
            for (const auto& face : its.indices) texture->indices.push_back({face[0], face[1], face[2]});
            texture->precomputed_face_colors.resize(its.indices.size(), {128, 128, 128});
            texture->precomputed_vertex_colors = colors.vertex_colors;
            for (size_t face = 0; face < its.indices.size(); ++face) {
                if ((face & 4095) == 0 && stopped()) return false;
                for (int channel = 0; channel < 3; ++channel) {
                    float value = 128.f;
                    if (!colors.vertex_colors.empty()) {
                        const auto& f = its.indices[face];
                        value = (colors.vertex_colors[f[0]][channel] + colors.vertex_colors[f[1]][channel] +
                            colors.vertex_colors[f[2]][channel]) / 3.f * 255.f;
                    } else if (face < colors.face_colors.size()) value = colors.face_colors[face][channel] * 255.f;
                    texture->precomputed_face_colors[face][channel] = size_t(std::clamp(value, 0.f, 255.f));
                }
            }
            decoded.texture = std::move(texture);
        }
        if (decoded.texture) {
            const Vec3f shift = decoded.mesh.bounding_box().center().cast<float>();
            for (auto& vertex : decoded.texture->vertices)
                for (int axis = 0; axis < 3; ++axis) vertex[axis] -= shift[axis];
        }
    } else if (boost::algorithm::iequals(path.extension().string(), ".glb")) {
        auto texture = std::make_shared<TexturedMesh>();
        if (!load_assimp_textured_model(path.string(), *texture, &error) || stopped()) return false;
        indexed_triangle_set its;
        its.vertices.reserve(texture->vertices.size());
        its.indices.reserve(texture->indices.size());
        for (auto& vertex : texture->vertices) {
            vertex = {vertex[0], -vertex[2], vertex[1]};
            its.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
        }
        for (const auto& face : texture->indices) its.indices.emplace_back(face[0], face[1], face[2]);
        its_merge_vertices(its);
        its_remove_degenerate_faces(its);
        its_compactify_vertices(its);
        decoded.mesh = TriangleMesh(std::move(its));
        if (decoded.mesh.volume() < 0.f) {
            decoded.mesh.flip_triangles();
            for (auto& face : texture->indices) std::swap(face[1], face[2]);
            for (auto& face : texture->uv_indices) std::swap(face[1], face[2]);
        }
        decoded.texture = std::move(texture);
    } else {
        error = "The workbench supports OBJ and GLB model files.";
        return false;
    }
    if (stopped()) return false;
    if (decoded.mesh.empty()) { error = "The supplied model has no printable geometry."; return false; }
    return true;
}

// Only call on the UI thread: Model and its children allocate global IDs.
inline std::unique_ptr<Model> assemble_workbench_model(DecodedWorkbenchModel& decoded,
    const boost::filesystem::path& path, bool converted_from_meters)
{
    auto model = std::make_unique<Model>();
    auto* object = model->add_object(path.filename().string().c_str(), path.string().c_str(), std::move(decoded.mesh));
    object->input_file = path.string();
    object->volumes.front()->source.is_converted_from_meters = converted_from_meters;
    model->texture_mesh = std::move(decoded.texture);
    return model;
}

} // namespace Slic3r::GUI
