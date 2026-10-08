#pragma once

#include "PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include <stdexcept>
#include <set>
#include <unordered_map>

namespace Slic3r::GUI {

inline WorkbenchCheckResult inspect_workbench_model(const boost::filesystem::path& path,
    const std::function<bool()>& canceled = {})
{
    WorkbenchCheckResult result;
    try {
        result.model_sha256 = AI::model_artifact_sha256(path);
        TriangleMesh mesh;
        ObjInfo colors;
        std::string error;
        if (!AI::load_model_artifact(path, mesh, colors, error, canceled)) throw std::runtime_error(error);
        const auto surface = AI::BeautySurface::build_for_appearance(mesh.its, colors.vertex_colors, {}, canceled);
        if (!surface || result.model_sha256.empty()) throw std::runtime_error("Topology inspection unavailable.");
        result.boundary_edges = surface->boundary_edges;
        result.nonmanifold_edges = surface->nonmanifold_edges;
        std::set<std::array<uint32_t, 3>> unique;
        std::unordered_map<uint64_t, std::array<size_t, 2>> directions;
        const double area_limit = std::pow(mesh.bounding_box().size().cast<double>().norm(), 2) * 5e-13;
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            if ((i & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
            const auto& face = mesh.its.indices[i];
            std::array<uint32_t, 3> ids {surface->vertex_class[face[0]], surface->vertex_class[face[1]], surface->vertex_class[face[2]]};
            if (surface->areas[i] <= area_limit) { ++result.degenerate_faces; continue; }
            auto key = ids;
            std::sort(key.begin(), key.end());
            if (!unique.insert(key).second) ++result.duplicate_faces;
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto a = ids[edge], b = ids[(edge + 1) % 3];
                const uint64_t id = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
                ++directions[id][a < b ? 0 : 1];
            }
        }
        for (const auto& item : directions)
            if (item.second[0] + item.second[1] == 2 && (item.second[0] == 2 || item.second[1] == 2)) ++result.inconsistent_edges;
        result.status = result.nonmanifold_edges || result.degenerate_faces || result.duplicate_faces || result.inconsistent_edges
            ? WorkbenchCheckStatus::Invalid : result.boundary_edges ? WorkbenchCheckStatus::Attention : WorkbenchCheckStatus::Normal;
        result.summary = "开放边 " + std::to_string(result.boundary_edges) + "；非流形边 " +
            std::to_string(result.nonmanifold_edges) + "；退化面 " + std::to_string(result.degenerate_faces) +
            "；重复面 " + std::to_string(result.duplicate_faces) + "；方向不一致 " +
            std::to_string(result.inconsistent_edges) + "。壁厚、悬垂和自交未检查。";
    } catch (const std::exception& error) {
        result.status = WorkbenchCheckStatus::Failed;
        result.summary = error.what();
    }
    return result;
}

inline bool workbench_safe_repair_allowed(const std::string& format, bool identity_bound_edits,
    bool has_candidate, bool dirty)
{
    return format == "obj" && !identity_bound_edits && !has_candidate && !dirty;
}

} // namespace Slic3r::GUI
