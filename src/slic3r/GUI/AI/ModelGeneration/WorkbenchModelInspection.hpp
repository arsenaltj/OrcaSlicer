#pragma once

#include "PostGenerationWorkbenchState.hpp"
#include "WorkbenchArtifactValidation.hpp"
#include "PortraitNativeCheckpoint.hpp"
#include "PortraitNativePartitionCache.hpp"
#include "WorkbenchHistoryEvidence.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshRepair.hpp"
#include <boost/filesystem.hpp>
#include <cmath>
#include <stdexcept>
#include <set>
#include <unordered_map>

namespace Slic3r::GUI {

inline bool workbench_topology_safe(const WorkbenchCheckResult& result)
{
    return result.status == WorkbenchCheckStatus::Normal && result.boundary_edges == 0 &&
        result.nonmanifold_edges == 0 && result.nonmanifold_vertices == 0 &&
        result.degenerate_faces == 0 && result.duplicate_faces == 0 &&
        result.inconsistent_edges == 0 && result.self_intersections_checked && result.self_intersections == 0;
}

inline bool workbench_repair_publication_allowed(const WorkbenchCheckResult& result,
    bool geometry_verified)
{
    return result.candidate_status == RepairCandidateStatus::Ready && geometry_verified &&
        workbench_topology_safe(result) && result.appearance_mapped && result.appearance_confidence >= 0.99;
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
        const auto issue = [&](size_t face) { if (result.issue_faces.size() < 4096) result.issue_faces.push_back(face); };
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            if ((i & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
            const auto& face = mesh.its.indices[i];
            std::array<uint32_t, 3> ids = native_closed
                ? std::array<uint32_t, 3> {uint32_t(face[0]), uint32_t(face[1]), uint32_t(face[2])}
                : std::array<uint32_t, 3> {surface->vertex_class[face[0]], surface->vertex_class[face[1]], surface->vertex_class[face[2]]};
            // Small positive-area triangles still close the surface. Dropping
            // them from incidence fabricates three open edges on a closed mesh.
            // Only collapsed/collinear triangles are topology defects; CGAL's
            // independent intersection predicate still checks narrow triangles.
            if (surface->areas[i] <= 0.) { ++result.degenerate_faces; issue(i); continue; }
            if (surface->areas[i] <= area_limit) ++result.small_faces;
            for (const uint32_t vertex : ids) vertex_faces[vertex].push_back(i);
            auto key = ids;
            std::sort(key.begin(), key.end());
            if (!unique.insert(key).second) { ++result.duplicate_faces; issue(i); }
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
            if (count != 2 || item.second[0] == 2 || item.second[1] == 2)
                for (size_t face : edge_faces[item.first]) issue(face);
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
                for (size_t face : incident) issue(face);
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
            if (connected != incident.size()) { ++result.nonmanifold_vertices; for (size_t face : incident) issue(face); }
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
            result.self_intersections_checked = true;
        }
        if (canceled && canceled()) throw std::runtime_error("Model inspection canceled.");
        std::sort(result.issue_faces.begin(), result.issue_faces.end());
        result.issue_faces.erase(std::unique(result.issue_faces.begin(), result.issue_faces.end()), result.issue_faces.end());
        result.status = result.nonmanifold_edges || result.nonmanifold_vertices || result.degenerate_faces || result.duplicate_faces || result.inconsistent_edges || result.self_intersections
            ? WorkbenchCheckStatus::Invalid : result.boundary_edges
                ? WorkbenchCheckStatus::Attention : WorkbenchCheckStatus::Normal;
        result.force_import_risk = result.status == WorkbenchCheckStatus::Attention;
        result.summary = "开放边 " + std::to_string(result.boundary_edges) + "；非流形边 " +
            std::to_string(result.nonmanifold_edges) + "；退化面 " + std::to_string(result.degenerate_faces) +
            "；重复面 " + std::to_string(result.duplicate_faces) + "；方向不一致 " +
            std::to_string(result.inconsistent_edges) + "；非流形顶点 " + std::to_string(result.nonmanifold_vertices) +
            "；边界环 " + std::to_string(result.boundary_loops) + "；连通组件 " +
            std::to_string(result.isolated_components) + "；自交检测：" +
            (result.self_intersections_checked ? result.self_intersections ? "发现自交" : "未发现自交" : "尚未完成") + "。";
    } catch (const std::exception& error) {
        result.status = WorkbenchCheckStatus::Failed;
        result.summary = error.what();
    }
    return result;
}

inline bool restore_workbench_appearance_topology(TriangleMesh& mesh,
    const boost::filesystem::path& candidate, const std::function<bool()>& canceled = {},
    const boost::filesystem::path& source = {}, WorkbenchCheckResult* verified = nullptr);

inline WorkbenchCheckResult inspect_workbench_model(const boost::filesystem::path& path,
    const std::function<bool()>& canceled = {}, const boost::filesystem::path& appearance_source = {})
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
        if (!restore_workbench_appearance_topology(mesh, path, canceled, appearance_source, &result))
            result = inspect_workbench_mesh(mesh, canceled);
        if (AI::model_artifact_sha256(path) != hash) {
            result.status = WorkbenchCheckStatus::Failed;
            result.summary = "模型文件在检查期间已变化，请重新检查。";
        }
    }
    result.model_sha256 = hash;
    return result;
}

// Expected identities and native_reference are frozen from an owned live
// portrait candidate. The original commit is evidence, never its own authority.
struct WorkbenchNativeTopologyCertificate {
    nlohmann::json partition_metadata;
    std::string partition_metadata_path;
    nlohmann::json native_reference;
    std::string candidate_sha256;
    std::string carrier_sha256;
    std::string geometry_id;
    std::string appearance_id;
    std::string partition_digest;
    std::string topology_hash;
    size_t face_count {0};
};

inline bool decode_native_publication_certificate(
    const boost::filesystem::path& candidate,
    const boost::filesystem::path& source,
    const WorkbenchNativeTopologyCertificate& proof,
    WorkbenchCheckResult& result,
    std::string& rejection,
    const std::function<bool()>& canceled = {})
{
    try {
        const auto require = [](bool valid, const char* error) {
            if (!valid) throw std::runtime_error(error);
        };
        const auto checkpoint = [&] {
            if (canceled && canceled())
                throw std::runtime_error("Native publication verification canceled.");
        };
        const auto verify_files = [&] {
            checkpoint();
            require(boost::filesystem::is_regular_file(candidate) &&
                    !boost::filesystem::is_symlink(candidate) &&
                    boost::filesystem::is_regular_file(source) &&
                    !boost::filesystem::is_symlink(source),
                    "Native publication files are unavailable.");
            require(AI::model_artifact_sha256(candidate) == proof.candidate_sha256 &&
                    AI::model_artifact_sha256(source) == proof.carrier_sha256,
                    "Native publication file identity changed.");
            checkpoint();
        };

        checkpoint();
        require(WorkbenchArtifacts::valid_sha256(proof.candidate_sha256) &&
                WorkbenchArtifacts::valid_sha256(proof.carrier_sha256) &&
                proof.candidate_sha256 == proof.carrier_sha256 &&
                !proof.geometry_id.empty() && !proof.appearance_id.empty() &&
                proof.face_count > 0,
                "Native publication expected identity is invalid.");

        const auto& reference = proof.native_reference;
        const auto& metadata = proof.partition_metadata;
        const auto evidence = workbench_history_evidence_location(source,
            boost::filesystem::path(boost::nowide::widen(proof.partition_metadata_path)), metadata, canceled);
        const auto evidence_path = boost::nowide::narrow(evidence.metadata.wstring());
        require(WorkbenchArtifacts::valid_native_partition_reference(
                    reference, proof.partition_digest, proof.topology_hash) &&
                WorkbenchArtifacts::native_partition_has_exact_mesh(reference) &&
                reference.at("carrier_sha256") == proof.carrier_sha256 &&
                reference.at("geometry_id") == proof.geometry_id &&
                reference.at("appearance_id") == proof.appearance_id &&
                WorkbenchArtifacts::same_model_path(
                    reference.at("metadata_path").get<std::string>(),
                    evidence_path) &&
                reference.at("cache_reference").at("face_count").get<size_t>() ==
                    proof.face_count,
                "Native publication reference identity mismatch.");
        nlohmann::json verified_reference;
        require(restorable_native_partition_metadata_matches(metadata,
                    evidence.metadata, evidence.model, proof.carrier_sha256,
                    proof.geometry_id, proof.appearance_id, verified_reference) &&
                verified_reference == reference,
                "Native publication checkpoint mismatch.");
        verify_files();

        const auto& native = metadata.at("native_portrait_topology_certificate");
        require(native.at("schema") ==
                    "orca.native-workbench-topology-certificate/v1" &&
                AI::beauty_leaf_digest(native.dump()) == proof.topology_hash &&
                native.at("carrier_sha256") == proof.carrier_sha256 &&
                native.at("geometry_id") == proof.geometry_id &&
                native.at("native_mesh_sha256") ==
                    reference.at("cache_reference").at("native_mesh_sha256") &&
                native.at("partition_digest") == proof.partition_digest &&
                native.at("coordinate_units") == reference.at("coordinate_units") &&
                native.at("source_bounds_mm") == reference.at("source_bounds_mm") &&
                native.at("carrier_bounds_mm") == reference.at("carrier_bounds_mm"),
                "Native topology certificate identity mismatch.");

        const auto& report = metadata.at("partition_build_report");
        const auto& strict = report.at("strict_repair");
        require(report.at("schema") ==
                    "orca.native-workbench-partition-build-report/v1" &&
                report.at("success").get<bool>() &&
                strict.at("success").get<bool>() &&
                !strict.at("canceled").get<bool>() &&
                strict.at("geometry_verified").get<bool>() &&
                strict.at("output_topology") == native.at("topology"),
                "Native strict repair certificate mismatch.");
        const double deviation = strict.at("sampled_deviation_ratio").get<double>();
        const double area_change = strict.at("area_change_ratio").get<double>();
        const StrictMeshRepairOptions limits;
        require(std::isfinite(deviation) && deviation >= 0.0 &&
                deviation <= limits.maximum_sampled_deviation_ratio &&
                std::isfinite(area_change) && area_change >= 0.0 &&
                area_change <= limits.maximum_area_change_ratio &&
                native.at("sampled_deviation_ratio").get<double>() == deviation &&
                native.at("area_change_ratio").get<double>() == area_change,
                "Native strict geometry certificate mismatch.");

        const auto& certificate = metadata.at("workbench_check_certificate");
        WorkbenchCheckResult checked;
        require(WorkbenchArtifacts::valid_workbench_check_certificate(
                    certificate, WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                    proof.carrier_sha256, proof.geometry_id, proof.appearance_id,
                    evidence_path, metadata,
                    proof.partition_digest, proof.topology_hash) &&
                WorkbenchArtifacts::decode_workbench_check_certificate(certificate, checked) &&
                workbench_topology_safe(checked) &&
                checked.before_faces == proof.face_count,
                "Native workbench certificate mismatch.");
        const auto& topology = native.at("topology");
        require(checked.boundary_edges == topology.at("open_edges").get<size_t>() &&
                checked.nonmanifold_edges == topology.at("nonmanifold_edges").get<size_t>() &&
                checked.nonmanifold_vertices == topology.at("nonmanifold_vertices").get<size_t>() &&
                checked.degenerate_faces == topology.at("degenerate_faces").get<size_t>() &&
                checked.duplicate_faces == topology.at("duplicate_faces").get<size_t>() &&
                checked.inconsistent_edges == topology.at("inconsistent_edges").get<size_t>() &&
                checked.self_intersections_checked ==
                    topology.at("self_intersections_checked").get<bool>() &&
                (checked.self_intersections != 0) == topology.at("self_intersects").get<bool>(),
                "Native topology reports disagree.");

        verify_files();
        const auto final_evidence = workbench_history_evidence_location(source,
            boost::filesystem::path(boost::nowide::widen(proof.partition_metadata_path)), metadata, canceled);
        require(final_evidence.model == evidence.model && final_evidence.metadata == evidence.metadata,
            "Native publication evidence changed during verification.");
        checked.model_sha256 = proof.candidate_sha256;
        checked.source_sha256 = proof.carrier_sha256;
        checked.candidate_status = RepairCandidateStatus::None;
        result = std::move(checked);
        rejection.clear();
        return true;
    } catch (const std::exception& error) {
        result = {};
        rejection = error.what();
        return false;
    }
}

// Color writers duplicate corners for independent face colors. Recover the
// immutable native indices only when every ordered corner is unchanged and
// the entire original native certificate/cache still verifies. Reuse that
// check just as native publication/import does; color-only versions do not
// acquire a second geometry interpretation through coordinate welding.
inline bool restore_workbench_appearance_topology(TriangleMesh& mesh,
    const boost::filesystem::path& candidate, const std::function<bool()>& canceled,
    const boost::filesystem::path& source, WorkbenchCheckResult* verified)
{
    if (verified) *verified = {};
    try {
        const auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Appearance topology verification canceled.");
        };
        const auto read = [&](const boost::filesystem::path& path) {
            checkpoint();
            if (boost::filesystem::is_symlink(path) || !boost::filesystem::is_regular_file(path) ||
                boost::filesystem::file_size(path) > 128ULL * 1024 * 1024)
                throw std::runtime_error("Appearance source metadata is unavailable.");
            boost::filesystem::ifstream input(path, std::ios::binary);
            return nlohmann::json::parse(input);
        };
        const auto same_corners = [&](const indexed_triangle_set& left, const indexed_triangle_set& right) {
            if (left.indices.size() != right.indices.size()) return false;
            for (size_t face = 0; face < left.indices.size(); ++face) {
                if ((face & 4095) == 0) checkpoint();
                for (int corner = 0; corner < 3; ++corner) {
                    const auto a = left.indices[face][corner], b = right.indices[face][corner];
                    if (a < 0 || b < 0 || size_t(a) >= left.vertices.size() || size_t(b) >= right.vertices.size() ||
                        left.vertices[a] != right.vertices[b]) return false;
                }
            }
            return true;
        };
        auto current = source.empty() ? candidate : source;
        const auto candidate_hash = AI::model_artifact_sha256(candidate);
        if (!WorkbenchArtifacts::valid_sha256(candidate_hash)) return false;
        const auto root = current.parent_path().filename() == "downloads"
            ? current.parent_path().parent_path() : current.parent_path();
        std::set<std::string> visited;
        struct VerifiedSource {
            boost::filesystem::path model, metadata;
            std::string model_hash, metadata_hash;
        };
        std::vector<VerifiedSource> ancestry;
        // Legacy versions have only a source link. Bound traversal and reject
        // cycles; new saved certificates avoid this work on normal reopen.
        for (size_t depth = 0; depth < 64; ++depth) {
            checkpoint();
            if (!visited.insert(current.generic_string()).second || boost::filesystem::is_symlink(current)) return false;
            const auto hash = AI::model_artifact_sha256(current);
            const auto metadata_path = AI::beauty_metadata_path(current, root);
            const auto metadata_hash = AI::model_artifact_sha256(metadata_path);
            const auto metadata = read(metadata_path);
            if (hash.empty() || metadata_hash.empty() || metadata.at("model_sha256") != hash) return false;
            TriangleMesh carrier; ObjInfo colors; std::string error;
            if (!AI::load_model_artifact(current, carrier, colors, error, canceled) ||
                !same_corners(mesh.its, carrier.its)) return false;
            ancestry.push_back({current, metadata_path, hash, metadata_hash});
            if (metadata.contains("native_portrait_partition")) {
                const auto& reference = metadata.at("native_portrait_partition");
                WorkbenchNativeTopologyCertificate proof;
                proof.partition_metadata = metadata;
                proof.partition_metadata_path = boost::nowide::narrow(metadata_path.wstring());
                proof.native_reference = reference;
                proof.candidate_sha256 = proof.carrier_sha256 = hash;
                proof.geometry_id = reference.at("geometry_id").get<std::string>();
                proof.appearance_id = reference.at("appearance_id").get<std::string>();
                proof.partition_digest = reference.at("partition_digest").get<std::string>();
                proof.topology_hash = reference.at("topology_hash").get<std::string>();
                proof.face_count = reference.at("cache_reference").at("face_count").get<size_t>();
                WorkbenchCheckResult checked;
                if (!decode_native_publication_certificate(current, current, proof, checked, error, canceled)) return false;
                TriangleMesh exact;
                NativePortrait::NativePortraitPartitionIdentity identity;
                const boost::filesystem::path cache(boost::nowide::widen(reference.at("cache_path").get<std::string>()));
                if (!NativePortrait::load_native_portrait_partition_cache(
                        reference.at("cache_reference"), cache.parent_path(), identity, error, &exact) ||
                    !NativePortrait::same_native_triangle_corners(carrier.its, exact.its, canceled) ||
                    checked.before_faces != exact.its.indices.size() ||
                    checked.before_vertices != exact.its.vertices.size()) return false;
                checkpoint();
                for (const auto& item : ancestry) {
                    checkpoint();
                    if (boost::filesystem::is_symlink(item.model) || boost::filesystem::is_symlink(item.metadata) ||
                        AI::model_artifact_sha256(item.model) != item.model_hash ||
                        AI::model_artifact_sha256(item.metadata) != item.metadata_hash) return false;
                }
                if (AI::model_artifact_sha256(candidate) != candidate_hash) return false;
                checkpoint();
                checked.model_sha256 = checked.source_sha256 = candidate_hash;
                checked.purpose = WorkbenchCheckPurpose::SavedModelReadOnly;
                exact.set_init_shift(mesh.get_init_shift());
                mesh = std::move(exact);
                if (verified) *verified = std::move(checked);
                return true;
            }
            if (metadata.value("source", std::string{}) != "local_finishing") return false;
            const boost::filesystem::path relative(metadata.at("source_model").get<std::string>());
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return false;
            const auto next = (root / relative).lexically_normal();
            if (AI::model_artifact_sha256(next) != metadata.at("source_sha256")) return false;
            current = next;
        }
    } catch (const std::exception&) {
        // An unavailable/changed ancestry never certifies the model. The
        // caller retains the complete inspection and ordinary failure path.
    }
    return false;
}

// Final publication checks also cover ordinary appearance and filament saves.
// Candidate statistics never certify the source.
inline WorkbenchCheckResult inspect_workbench_publication(const boost::filesystem::path& candidate,
    const boost::filesystem::path& source, const WorkbenchCheckResult* repair,
    const std::function<bool()>& canceled = {},
    const WorkbenchNativeTopologyCertificate* native = nullptr)
{
    if (native) {
        WorkbenchCheckResult certified;
        std::string rejection;
        if (!repair && decode_native_publication_certificate(
                candidate, source, *native, certified, rejection, canceled))
            return certified;
        // Invalid proof always takes the full path, but safe geometry alone
        // cannot authorize changed publication identities.
        const auto full_check = inspect_workbench_model(candidate, canceled);
        if (!workbench_topology_safe(full_check))
            throw std::runtime_error(
                "Native publication certificate rejected; full inspection failed: " +
                full_check.summary);
        throw std::runtime_error("Native publication certificate identity mismatch: " +
            (rejection.empty() ? std::string("incompatible repair candidate") : rejection));
    }
    if (repair && (!workbench_topology_safe(*repair) ||
        AI::model_artifact_sha256(candidate) != repair->candidate_sha256 ||
        AI::model_artifact_sha256(source) != repair->source_sha256))
        throw std::runtime_error("修复候选或原模型已变化，请重新检查。");
    // A new color candidate has not published its metadata yet. Its source is
    // supplied by the owning save transaction, then verified like history.
    auto checked = inspect_workbench_model(candidate, canceled, source);
    if (!workbench_topology_safe(checked))
        throw std::runtime_error("候选最终拓扑检查未通过：" + checked.summary);
    if (repair) {
        // Keep the operation's original counts while the final inspection
        // supplies actual published geometry and fresh topology evidence.
        checked.after_faces = checked.before_faces;
        checked.after_vertices = checked.before_vertices;
        checked.before_faces = repair->before_faces;
        checked.before_vertices = repair->before_vertices;
        checked.removed_faces = repair->removed_faces;
        checked.reversed_faces = repair->reversed_faces;
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
