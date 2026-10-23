#pragma once

#include "PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include <stdexcept>
#include <set>
#include <unordered_map>

namespace Slic3r::GUI {

inline bool workbench_topology_safe(const WorkbenchCheckResult& result)
{
    return result.status == WorkbenchCheckStatus::Normal && result.boundary_edges == 0 &&
        result.nonmanifold_edges == 0 && result.nonmanifold_vertices == 0 &&
        result.degenerate_faces == 0 && result.duplicate_faces == 0 &&
        result.inconsistent_edges == 0 && result.self_intersections == 0;
}

inline WorkbenchCheckResult inspect_workbench_mesh(const TriangleMesh& mesh,
    const std::function<bool()>& canceled = {})
{
    WorkbenchCheckResult result;
    try {
        if (mesh.empty()) throw std::runtime_error("Topology inspection unavailable: empty mesh.");
        const auto surface = AI::BeautySurface::build_for_appearance(mesh.its, {}, {}, canceled);
        if (!surface) throw std::runtime_error("Topology inspection unavailable.");
        // Closed native topology may intentionally split a non-manifold
        // vertex into separate fans. Do not join those fans again by position.
        const bool native_closed = its_num_open_edges(mesh.its) == 0;
        result.before_faces = mesh.its.indices.size();
        result.before_vertices = mesh.its.vertices.size();
        std::set<std::array<uint32_t, 3>> unique;
        std::unordered_map<uint64_t, std::array<size_t, 2>> directions;
        std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> edge_uses;
        std::unordered_map<uint64_t, std::vector<size_t>> edge_faces;
        std::unordered_map<uint32_t, size_t> vertex_nonmanifold_edges, vertex_boundary_degree;
        std::vector<std::vector<size_t>> vertex_faces(mesh.its.vertices.size());
        const double area_limit = std::pow(mesh.bounding_box().size().cast<double>().norm(), 2) * 5e-13;
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            if ((i & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
            const auto& face = mesh.its.indices[i];
            std::array<uint32_t, 3> ids = native_closed
                ? std::array<uint32_t, 3> {uint32_t(face[0]), uint32_t(face[1]), uint32_t(face[2])}
                : std::array<uint32_t, 3> {surface->vertex_class[face[0]], surface->vertex_class[face[1]], surface->vertex_class[face[2]]};
            if (surface->areas[i] <= area_limit) { ++result.degenerate_faces; continue; }
            for (const uint32_t vertex : ids) vertex_faces[vertex].push_back(i);
            auto key = ids;
            std::sort(key.begin(), key.end());
            if (!unique.insert(key).second) ++result.duplicate_faces;
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto a = ids[edge], b = ids[(edge + 1) % 3];
                const uint64_t id = (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
                ++directions[id][a < b ? 0 : 1];
                edge_uses[id].push_back({a, b});
                edge_faces[id].push_back(i);
            }
        }
        std::unordered_map<uint32_t, std::vector<uint32_t>> boundary_graph;
        for (const auto& item : directions) {
            if (item.second[0] + item.second[1] == 2 && (item.second[0] == 2 || item.second[1] == 2)) ++result.inconsistent_edges;
            const auto uses = edge_uses.find(item.first);
            const size_t count = uses == edge_uses.end() ? 0 : uses->second.size();
            if (count == 1) ++result.boundary_edges;
            else if (count > 2) ++result.nonmanifold_edges;
            if (count != 2) {
                const uint32_t a = uint32_t(item.first >> 32), b = uint32_t(item.first & 0xffffffffu);
                if (count > 2) { ++vertex_nonmanifold_edges[a]; ++vertex_nonmanifold_edges[b]; }
                if (count == 1) {
                    ++vertex_boundary_degree[a]; ++vertex_boundary_degree[b];
                    boundary_graph[a].push_back(b);
                    boundary_graph[b].push_back(a);
                }
            }
        }
        std::vector<std::vector<size_t>> face_neighbors(mesh.its.indices.size());
        for (const auto& item : edge_faces) if (item.second.size() == 2) {
            face_neighbors[item.second[0]].push_back(item.second[1]);
            face_neighbors[item.second[1]].push_back(item.second[0]);
        }
        // Two closed fans touching at one index have no bad edge. Test the
        // connectivity of each vertex link rather than counting bad edges only.
        std::vector<size_t> vertex_seen(mesh.its.indices.size(), size_t(-1));
        std::vector<size_t> vertex_stack;
        for (size_t vertex = 0; vertex < vertex_faces.size(); ++vertex) {
            if ((vertex & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
            const auto& incident = vertex_faces[vertex];
            if (incident.empty()) continue;
            const auto bad = vertex_nonmanifold_edges.find(uint32_t(vertex));
            const auto boundary = vertex_boundary_degree.find(uint32_t(vertex));
            if ((bad != vertex_nonmanifold_edges.end() && bad->second > 0) ||
                (boundary != vertex_boundary_degree.end() && boundary->second != 2)) {
                ++result.nonmanifold_vertices;
                continue;
            }
            vertex_stack.assign(1, incident.front());
            vertex_seen[incident.front()] = vertex;
            size_t connected = 0;
            while (!vertex_stack.empty()) {
                const size_t face = vertex_stack.back(); vertex_stack.pop_back(); ++connected;
                for (size_t neighbor : face_neighbors[face]) {
                    if (vertex_seen[neighbor] == vertex) continue;
                    const auto& f = mesh.its.indices[neighbor];
                    bool contains = false;
                    for (int corner = 0; corner < 3; ++corner)
                        contains |= size_t(native_closed ? uint32_t(f[corner]) : surface->vertex_class[f[corner]]) == vertex;
                    if (contains) { vertex_seen[neighbor] = vertex; vertex_stack.push_back(neighbor); }
                }
            }
            if (connected != incident.size()) ++result.nonmanifold_vertices;
        }
        std::vector<uint8_t> face_seen(mesh.its.indices.size(), 0);
        for (size_t face = 0; face < face_seen.size(); ++face) if (!face_seen[face]) {
            ++result.isolated_components;
            std::vector<size_t> stack {face}; face_seen[face] = 1;
            while (!stack.empty()) {
                const size_t current = stack.back(); stack.pop_back();
                for (const size_t next : face_neighbors[current]) if (!face_seen[next]) {
                    face_seen[next] = 1; stack.push_back(next);
                }
            }
        }
        // Count connected boundary rings and retain their edge/vertex sizes for
        // the candidate summary. A ring with a branch is still one component;
        // the non-manifold vertex count above explains why it is unsafe.
        std::set<uint32_t> boundary_seen;
        for (const auto& item : boundary_graph) {
            if (!boundary_seen.insert(item.first).second) continue;
            std::vector<uint32_t> stack {item.first}; size_t size = 0;
            while (!stack.empty()) {
                const uint32_t v = stack.back(); stack.pop_back(); ++size;
                const auto next = boundary_graph.find(v);
                if (next == boundary_graph.end()) continue;
                for (const uint32_t n : next->second)
                    if (boundary_seen.insert(n).second) stack.push_back(n);
            }
            ++result.boundary_loops; result.boundary_loop_sizes.push_back(size);
        }
        // CGAL supplies the authoritative self-intersection predicate used by
        // the repair backend. Keep it as a count-like risk flag for the UI.
        if (result.boundary_edges == 0 && result.nonmanifold_edges == 0 &&
            result.nonmanifold_vertices == 0 && result.degenerate_faces == 0 && result.inconsistent_edges == 0) {
            TriangleMesh topology = mesh;
            if (!native_closed) its_merge_vertices(topology.its);
            // A failed predicate is an unavailable check, never zero risk.
            result.self_intersections = MeshBoolean::cgal::does_self_intersect(topology) ? 1 : 0;
        }
        if (canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
        result.status = result.nonmanifold_edges || result.nonmanifold_vertices || result.degenerate_faces || result.duplicate_faces || result.inconsistent_edges || result.self_intersections
            ? WorkbenchCheckStatus::Invalid : result.boundary_edges
                ? WorkbenchCheckStatus::Attention : WorkbenchCheckStatus::Normal;
        result.force_import_risk = result.status == WorkbenchCheckStatus::Attention;
        result.summary = "开放边 " + std::to_string(result.boundary_edges) + "；非流形边 " +
            std::to_string(result.nonmanifold_edges) + "；退化面 " + std::to_string(result.degenerate_faces) +
            "；重复面 " + std::to_string(result.duplicate_faces) + "；方向不一致 " +
            std::to_string(result.inconsistent_edges) + "；非流形顶点 " + std::to_string(result.nonmanifold_vertices) +
            "；边界环 " + std::to_string(result.boundary_loops) + "；连通组件 " +
            std::to_string(result.isolated_components) + "；自交风险 " + std::to_string(result.self_intersections) + "。";
    } catch (const std::exception& error) {
        result.status = WorkbenchCheckStatus::Failed;
        result.summary = error.what();
    }
    return result;
}

inline WorkbenchCheckResult inspect_workbench_model(const boost::filesystem::path& path,
    const std::function<bool()>& canceled = {})
{
    WorkbenchCheckResult result;
    const auto hash = AI::model_artifact_sha256(path);
    TriangleMesh mesh;
    ObjInfo colors;
    std::string error;
    if (hash.empty() || !AI::load_model_artifact(path, mesh, colors, error, canceled)) {
        result.status = WorkbenchCheckStatus::Failed;
        result.summary = error.empty() ? "Topology inspection unavailable." : error;
    } else {
        result = inspect_workbench_mesh(mesh, canceled);
        if (AI::model_artifact_sha256(path) != hash) {
            result.status = WorkbenchCheckStatus::Failed;
            result.summary = "模型文件在检查期间已变化，请重新检查。";
        }
    }
    result.model_sha256 = hash;
    return result;
}

// Publication is the last topology gate, shared by explicit repair acceptance
// and automatic filament saving. Candidate statistics never certify the source.
inline WorkbenchCheckResult inspect_workbench_publication(const boost::filesystem::path& candidate,
    const boost::filesystem::path& source, const WorkbenchCheckResult* repair,
    const std::function<bool()>& canceled = {})
{
    if (repair && (!workbench_topology_safe(*repair) ||
        AI::model_artifact_sha256(candidate) != repair->candidate_sha256 ||
        AI::model_artifact_sha256(source) != repair->source_sha256))
        throw std::runtime_error("修复候选或原模型已变化，请重新检查。");
    auto checked = inspect_workbench_model(candidate, canceled);
    if (!workbench_topology_safe(checked))
        throw std::runtime_error("候选最终拓扑检查未通过：" + checked.summary);
    if (repair) {
        checked.candidate_status = RepairCandidateStatus::Accepted;
        checked.source_sha256 = repair->source_sha256;
        checked.candidate_sha256 = checked.model_sha256;
        checked.repair_reason = repair->repair_reason;
    }
    return checked;
}

inline bool workbench_safe_repair_allowed(const std::string& format, bool identity_bound_edits,
    bool has_candidate, bool dirty)
{
    return (format == "obj" || format == "glb") && !has_candidate && !dirty;
}

} // namespace Slic3r::GUI
