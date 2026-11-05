#include "model_internal.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <stdexcept>

namespace orca_model_detail {
nlohmann::json orca_model_select_region(const Document& document, const nlohmann::json& request, std::atomic<bool>& canceled)
{
    using namespace Slic3r;
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    const auto& vertices = document.mesh.vertices;
    const auto& faces    = document.mesh.faces;
    // Preserve per-face discontinuities while feeding the existing geometry-welded
    // adjacency algorithm. No replacement region algorithm is maintained here.
    for (size_t face = 0; face < faces.size(); ++face) {
        if (canceled)
            throw Error("CANCELED", "Region selection canceled");
        stl_triangle_vertex_indices triangle;
        auto color = RGBA{float(document.mesh.colors[face][0]), float(document.mesh.colors[face][1]), float(document.mesh.colors[face][2]),
                          float(document.mesh.colors[face][3])};
        for (int corner = 0; corner < 3; ++corner) {
            auto point       = vertices.at(faces.at(face).at(corner));
            triangle[corner] = int(mesh.vertices.size());
            mesh.vertices.emplace_back(float(point[0]), float(point[1]), float(point[2]));
            colors.push_back(color);
        }
        mesh.indices.push_back(triangle);
    }
    AI::VertexColorRegionEditor editor;
    std::string error;
    if (!editor.initialize(std::move(mesh), std::move(colors), error, [&] { return canceled.load(); })) {
        if (canceled)
            throw Error("CANCELED", "Region selection canceled");
        throw std::runtime_error(error);
    }
    AI::RegionSelectionSettings settings;
    settings.color_distance       = request.value("colorDistance", 0.12f);
    settings.normal_angle_degrees = request.value("normalAngleDegrees", 70.0f);
    settings.local_radius_ratio   = request.value("localRadiusRatio", 0.035f);
    auto seed                     = request.at("seedFace").get<size_t>();
    if (seed >= faces.size())
        throw std::runtime_error("Seed face out of range");
    editor.update_selection(seed, AI::RegionSelectionOperation::Replace, settings);
    std::vector<size_t> selected;
    const auto& mask = editor.selected_faces();
    for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i])
            selected.push_back(i);
    return nlohmann::json{{"faces", selected},
                          {"geometryRevision", document.geometry},
                          {"revision", document.revision},
                          {"documentId", document.id}};
}

} // namespace orca_model_detail
