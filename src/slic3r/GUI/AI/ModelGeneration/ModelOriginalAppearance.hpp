#pragma once

#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::GUI {

struct OriginalAppearanceReference {
    boost::filesystem::path carrier_model;
    std::string carrier_sha256;
    std::string topology_sha256;
    boost::filesystem::path origin_model;
    std::string origin_sha256;

    bool valid() const
    {
        return !carrier_model.empty() && !origin_model.empty() &&
            AI::SurfaceSelectionPersistence::detail::valid_fingerprint(carrier_sha256) &&
            AI::SurfaceSelectionPersistence::detail::valid_fingerprint(topology_sha256) &&
            AI::SurfaceSelectionPersistence::detail::valid_fingerprint(origin_sha256);
    }

    nlohmann::json encode(const boost::filesystem::path& root) const
    {
        if (!valid()) throw std::invalid_argument("Invalid original appearance reference.");
        const auto carrier = carrier_model.lexically_relative(root);
        const auto origin = origin_model.lexically_relative(root);
        const auto safe_relative = [](const boost::filesystem::path& path) {
            return !path.empty() && !path.is_absolute() && !path.has_root_path() &&
                std::none_of(path.begin(), path.end(), [](const auto& part) { return part == ".."; });
        };
        if (!safe_relative(carrier) || !safe_relative(origin))
            throw std::invalid_argument("Original appearance paths are outside the model library.");
        return {{"schema", "orca.model-original-appearance/v1"},
                {"carrier_model", carrier.generic_string()},
                {"carrier_sha256", carrier_sha256},
                {"topology_sha256", topology_sha256},
                {"origin_model", origin.generic_string()},
                {"origin_sha256", origin_sha256}};
    }
};

struct ResolvedOriginalAppearance {
    OriginalAppearanceReference reference;
    TriangleMesh carrier_mesh;
    ObjInfo carrier_appearance;
    AI::ModelArtifactTextureSurface carrier_texture;
    bool restored_legacy_chain {false};
};

namespace original_appearance_detail {

inline bool stopped(const std::function<bool()>& canceled)
{
    return canceled && canceled();
}

inline bool safe_library_file(const boost::filesystem::path& root,
                              const boost::filesystem::path& relative,
                              boost::filesystem::path& resolved)
{
    if (root.empty() || relative.empty() || relative.is_absolute() || relative.has_root_path()) return false;
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized == "." ||
        std::any_of(normalized.begin(), normalized.end(), [](const auto& part) { return part == ".."; }))
        return false;
    boost::system::error_code error;
    const auto canonical_root = boost::filesystem::canonical(root, error);
    if (error) return false;
    auto candidate = canonical_root;
    for (const auto& part : normalized) {
        candidate /= part;
        if (boost::filesystem::is_symlink(candidate, error) || error) return false;
    }
    resolved = boost::filesystem::canonical(candidate, error);
    if (error) return false;
    const auto within = resolved.lexically_relative(canonical_root);
    return !within.empty() && !within.is_absolute() && *within.begin() != "..";
}

inline nlohmann::json read_record(const boost::filesystem::path& path)
{
    boost::system::error_code error;
    const auto bytes = boost::filesystem::file_size(path, error);
    if (error || bytes == 0 || bytes > 128ULL * 1024 * 1024 || boost::filesystem::is_symlink(path, error))
        return {};
    boost::filesystem::ifstream input(path, std::ios::binary);
    return input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json();
}

inline bool has_appearance(const indexed_triangle_set& mesh, const ObjInfo& appearance,
                           const AI::ModelArtifactTextureSurface& texture)
{
    return appearance.vertex_colors.size() == mesh.vertices.size() ||
        appearance.face_colors.size() == mesh.indices.size() ||
        texture.faces.size() == mesh.indices.size();
}

inline bool load_carrier(const boost::filesystem::path& path, const std::string& expected_hash,
                         const std::string& expected_topology, ResolvedOriginalAppearance& output,
                         std::string& error, const std::function<bool()>& canceled)
{
    if (stopped(canceled)) { error = "Original appearance loading canceled."; return false; }
    if (AI::model_artifact_sha256(path) != expected_hash) {
        error = "原始配色证据哈希已变化。";
        return false;
    }
    TriangleMesh mesh;
    ObjInfo appearance;
    AI::ModelArtifactTextureSurface texture;
    std::string load_error;
    if (!AI::load_model_artifact(path, mesh, appearance, load_error, canceled, &texture) || mesh.empty()) {
        error = load_error.empty() ? "无法读取原始配色证据。" : load_error;
        return false;
    }
    if (AI::SurfaceSelectionPersistence::topology_fingerprint(mesh.its) != expected_topology) {
        error = "原始配色证据与当前模型拓扑不匹配。";
        return false;
    }
    if (!has_appearance(mesh.its, appearance, texture)) {
        error = "原始模型不包含可用的贴图、顶点色或面颜色。";
        return false;
    }
    output.carrier_mesh = std::move(mesh);
    output.carrier_appearance = std::move(appearance);
    output.carrier_texture = std::move(texture);
    return true;
}

inline bool map_appearance_to_vertices(const indexed_triangle_set& source,
                                       const ObjInfo& appearance,
                                       const indexed_triangle_set& target,
                                       std::vector<RGBA>& colors,
                                       std::string& error,
                                       const std::function<bool()>& canceled = {})
{
    colors.clear();
    error.clear();
    const bool vertex_colors = appearance.vertex_colors.size() == source.vertices.size();
    const bool face_colors = appearance.face_colors.size() == source.indices.size();
    if (!vertex_colors && !face_colors) {
        error = "原始配色载体缺少可映射的顶点色或面颜色。";
        return false;
    }
    if (source.vertices.empty() || source.indices.empty() || target.vertices.empty() || target.indices.empty()) {
        error = "原始配色映射缺少有效网格。";
        return false;
    }
    try {
        const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
            source.vertices, source.indices);
        colors.assign(target.vertices.size(), RGBA {1.f, 1.f, 1.f, 1.f});
        for (size_t index = 0; index < target.vertices.size(); ++index) {
            if ((index & 4095) == 0 && stopped(canceled))
                throw std::runtime_error("Original appearance mapping canceled.");
            size_t face = 0;
            Vec3d nearest;
            const Vec3d query = target.vertices[index].cast<double>();
            const double distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                source.vertices, source.indices, tree, query, face, nearest);
            if (distance < 0. || face >= source.indices.size())
                throw std::runtime_error("原始配色无法映射到修复候选。");
            // Model preview also gives vertex colors precedence when an
            // artifact exposes both representations. Keep the immutable
            // carrier and topology-remap paths on the same rule.
            if (vertex_colors) {
                const auto& triangle = source.indices[face];
                const Vec3d a = source.vertices[triangle[0]].cast<double>();
                const Vec3d u = source.vertices[triangle[1]].cast<double>() - a;
                const Vec3d v = source.vertices[triangle[2]].cast<double>() - a;
                const Vec3d q = nearest - a;
                const double uu = u.dot(u), uv = u.dot(v), vv = v.dot(v);
                const double denominator = uu * vv - uv * uv;
                if (denominator <= 0.)
                    throw std::runtime_error("原始配色映射遇到无效来源面。");
                const double b = std::clamp((vv * q.dot(u) - uv * q.dot(v)) / denominator, 0., 1.);
                const double c = std::clamp((uu * q.dot(v) - uv * q.dot(u)) / denominator, 0., 1. - b);
                for (size_t channel = 0; channel < 4; ++channel)
                    colors[index][channel] = float((1. - b - c) * appearance.vertex_colors[triangle[0]][channel] +
                        b * appearance.vertex_colors[triangle[1]][channel] +
                        c * appearance.vertex_colors[triangle[2]][channel]);
            } else {
                colors[index] = appearance.face_colors[face];
            }
        }
        return true;
    } catch (const std::exception& failure) {
        colors.clear();
        error = failure.what();
        return false;
    }
}

inline std::optional<OriginalAppearanceReference> decode_reference(
    const nlohmann::json& value, const boost::filesystem::path& root,
    const std::string& expected_topology, std::string& error)
{
    if (!value.is_object() || value.value("schema", std::string()) != "orca.model-original-appearance/v1") {
        error = "原始配色证据记录格式无效。";
        return std::nullopt;
    }
    for (const char* field : {"carrier_model", "carrier_sha256", "topology_sha256", "origin_model", "origin_sha256"})
        if (!value.contains(field) || !value[field].is_string()) {
            error = "原始配色证据记录缺少必要字段。";
            return std::nullopt;
        }
    OriginalAppearanceReference reference;
    boost::filesystem::path carrier, origin;
    if (!safe_library_file(root, boost::filesystem::path(value["carrier_model"].get<std::string>()), carrier) ||
        !safe_library_file(root, boost::filesystem::path(value["origin_model"].get<std::string>()), origin)) {
        error = "原始配色证据路径无效或不在模型目录中。";
        return std::nullopt;
    }
    reference.carrier_model = carrier;
    reference.carrier_sha256 = value["carrier_sha256"].get<std::string>();
    reference.topology_sha256 = value["topology_sha256"].get<std::string>();
    reference.origin_model = origin;
    reference.origin_sha256 = value["origin_sha256"].get<std::string>();
    if (!reference.valid() || reference.topology_sha256 != expected_topology) {
        error = "原始配色证据身份与当前模型不匹配。";
        return std::nullopt;
    }
    if (AI::model_artifact_sha256(origin) != reference.origin_sha256) {
        error = "最初导入模型已缺失或发生变化。";
        return std::nullopt;
    }
    return reference;
}

} // namespace original_appearance_detail

inline bool resolve_original_appearance_reference(
    const OriginalAppearanceReference& reference, const indexed_triangle_set& current_mesh,
    const boost::filesystem::path& root, ResolvedOriginalAppearance& output,
    std::string& error, const std::function<bool()>& canceled = {})
{
    using namespace original_appearance_detail;
    output = {};
    error.clear();
    const std::string topology = AI::SurfaceSelectionPersistence::topology_fingerprint(current_mesh);
    if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(topology) ||
        topology != reference.topology_sha256) {
        error = "原始配色证据与当前模型拓扑不匹配。";
        return false;
    }
    nlohmann::json encoded;
    try { encoded = reference.encode(root); }
    catch (const std::exception& failure) { error = failure.what(); return false; }
    const auto decoded = decode_reference(encoded, root, topology, error);
    if (!decoded) return false;
    output.reference = *decoded;
    return load_carrier(decoded->carrier_model, decoded->carrier_sha256, topology,
                        output, error, canceled);
}

// A topology-changing repair uses the current coordinates for the closest-point
// query, but samples color from the immutable carrier with the same ordered
// connectivity. This prevents a saved low-color preview becoming the next AI
// palette source after repair.
inline bool prepare_original_appearance_for_candidate(
    const boost::filesystem::path& current_model,
    const boost::filesystem::path& candidate_model,
    const std::string& expected_current_sha256,
    const OriginalAppearanceReference& inherited,
    const boost::filesystem::path& carrier_destination,
    const boost::filesystem::path& root,
    OriginalAppearanceReference& result,
    std::string& error,
    const std::function<bool()>& canceled = {})
{
    using namespace original_appearance_detail;
    result = {};
    error.clear();
    boost::filesystem::path temporary;
    bool carrier_published = false;
    const auto cleanup = [&] {
        boost::system::error_code ignored;
        if (!temporary.empty()) boost::filesystem::remove(temporary, ignored);
        if (carrier_published) boost::filesystem::remove(carrier_destination, ignored);
    };
    try {
        if (stopped(canceled)) throw std::runtime_error("Original appearance mapping canceled.");
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(expected_current_sha256) ||
            AI::model_artifact_sha256(current_model) != expected_current_sha256)
            throw std::runtime_error("当前模型在原始配色映射期间发生变化。");

        TriangleMesh current_mesh, candidate_mesh;
        ObjInfo ignored_appearance;
        std::string load_error;
        if (!AI::load_model_artifact(current_model, current_mesh, ignored_appearance, load_error, canceled) ||
            current_mesh.empty())
            throw std::runtime_error(load_error.empty() ? "无法读取修复前模型。" : load_error);
        ResolvedOriginalAppearance resolved;
        if (!resolve_original_appearance_reference(inherited, current_mesh.its, root, resolved, error, canceled))
            return false;
        if (!AI::load_model_artifact(candidate_model, candidate_mesh, ignored_appearance, load_error, canceled) ||
            candidate_mesh.empty())
            throw std::runtime_error(load_error.empty() ? "无法读取修复候选模型。" : load_error);
        const std::string topology = AI::SurfaceSelectionPersistence::topology_fingerprint(candidate_mesh.its);
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(topology))
            throw std::runtime_error("无法建立修复候选的拓扑身份。");
        if (topology == inherited.topology_sha256) {
            result = resolved.reference;
            return true;
        }

        std::vector<RGBA> colors;
        if (!map_appearance_to_vertices(current_mesh.its, resolved.carrier_appearance,
                candidate_mesh.its, colors, error, canceled))
            return false;
        if (AI::model_artifact_sha256(current_model) != expected_current_sha256)
            throw std::runtime_error("当前模型在原始配色映射期间发生变化。");
        if (boost::filesystem::exists(carrier_destination) || boost::filesystem::is_symlink(carrier_destination))
            throw std::runtime_error("原始配色伴随文件已存在，未覆盖现有资产。");
        boost::system::error_code path_error;
        const auto canonical_root = boost::filesystem::canonical(root, path_error);
        const auto canonical_parent = boost::filesystem::canonical(carrier_destination.parent_path(), path_error);
        if (path_error) throw std::runtime_error("原始配色伴随文件目录不可用。");
        const auto relative_parent = canonical_parent.lexically_relative(canonical_root);
        if (relative_parent.empty() || relative_parent.is_absolute() || *relative_parent.begin() == "..")
            throw std::runtime_error("原始配色伴随文件必须位于模型目录中。");
        temporary = carrier_destination.parent_path() /
            boost::filesystem::unique_path(".source-appearance-%%%%-%%%%.obj");
        if (!AI::write_model_artifact(temporary, candidate_mesh.its, colors, error)) {
            cleanup();
            return false;
        }
        if (stopped(canceled)) throw std::runtime_error("Original appearance mapping canceled.");
        boost::filesystem::rename(temporary, carrier_destination);
        temporary.clear();
        carrier_published = true;
        const auto carrier_hash = AI::model_artifact_sha256(carrier_destination);
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(carrier_hash))
            throw std::runtime_error("原始配色伴随文件校验失败。");
        result = {boost::filesystem::canonical(carrier_destination), carrier_hash, topology,
                  resolved.reference.origin_model, resolved.reference.origin_sha256};
        carrier_published = false;
        return true;
    } catch (const std::exception& failure) {
        cleanup();
        error = failure.what();
        return false;
    }
}

// Resolve an explicit v1 reference first. Legacy history is walked without
// modifying it; the oldest topology-compatible ancestor becomes the carrier.
inline bool resolve_original_appearance(const boost::filesystem::path& current_model,
                                        const indexed_triangle_set& current_mesh,
                                        const nlohmann::json& current_metadata,
                                        const boost::filesystem::path& root,
                                        ResolvedOriginalAppearance& output,
                                        std::string& error,
                                        const std::function<bool()>& canceled = {})
{
    using namespace original_appearance_detail;
    output = {};
    error.clear();
    const std::string topology = AI::SurfaceSelectionPersistence::topology_fingerprint(current_mesh);
    if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(topology)) {
        error = "无法建立当前模型的拓扑身份。";
        return false;
    }
    if (current_metadata.is_object() && current_metadata.contains("original_appearance")) {
        const auto decoded = decode_reference(current_metadata["original_appearance"], root, topology, error);
        if (!decoded) return false;
        output.reference = *decoded;
        return load_carrier(decoded->carrier_model, decoded->carrier_sha256, topology, output, error, canceled);
    }

    const bool has_parent = current_metadata.is_object() && current_metadata.contains("source_model");
    boost::filesystem::path candidate;
    std::string candidate_hash;
    boost::filesystem::path origin = current_model;
    std::string origin_hash = AI::model_artifact_sha256(current_model);
    nlohmann::json metadata = current_metadata;
    std::set<std::string> visited;
    boost::system::error_code current_error;
    const auto canonical_current = boost::filesystem::canonical(current_model, current_error);
    if (current_error) {
        error = "无法确认当前模型的历史身份。";
        return false;
    }
    visited.insert(canonical_current.generic_string());
    for (size_t depth = 0; depth < 64 && metadata.is_object() && metadata.contains("source_model"); ++depth) {
        if (stopped(canceled)) { error = "Original appearance loading canceled."; return false; }
        if (!metadata["source_model"].is_string() || !metadata.contains("source_sha256") ||
            !metadata["source_sha256"].is_string()) {
            error = "历史版本的来源模型记录不完整。";
            return false;
        }
        boost::filesystem::path source;
        if (!safe_library_file(root, boost::filesystem::path(metadata["source_model"].get<std::string>()), source)) {
            error = "历史版本引用了无效的来源模型路径。";
            return false;
        }
        const auto key = source.generic_string();
        if (!visited.insert(key).second) { error = "历史版本的来源模型链存在循环。"; return false; }
        const std::string expected_hash = metadata["source_sha256"].get<std::string>();
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(expected_hash) ||
            AI::model_artifact_sha256(source) != expected_hash) {
            error = "历史版本的来源模型已缺失或发生变化。";
            return false;
        }
        TriangleMesh mesh;
        ObjInfo appearance;
        AI::ModelArtifactTextureSurface texture;
        std::string load_error;
        if (!AI::load_model_artifact(source, mesh, appearance, load_error, canceled, &texture) || mesh.empty()) {
            error = load_error.empty() ? "无法读取历史来源模型。" : load_error;
            return false;
        }
        if (AI::SurfaceSelectionPersistence::topology_fingerprint(mesh.its) == topology &&
            has_appearance(mesh.its, appearance, texture)) {
            candidate = source;
            candidate_hash = expected_hash;
        }
        origin = source;
        origin_hash = expected_hash;
        const auto record = AI::beauty_metadata_path(source, root);
        metadata = read_record(record);
        if (!metadata.is_object()) break;
        if (depth == 63 && metadata.contains("source_model")) {
            error = "历史版本的来源模型链超过安全上限。";
            return false;
        }
    }

    if (!has_parent) {
        candidate = current_model;
        candidate_hash = origin_hash;
    }
    if (candidate.empty()) {
        error = "未找到与当前拓扑兼容的原始配色证据。";
        return false;
    }
    output.reference = {candidate, candidate_hash, topology, origin, origin_hash};
    if (!output.reference.valid()) { error = "原始配色证据身份无效。"; return false; }
    output.restored_legacy_chain = has_parent;
    return load_carrier(candidate, candidate_hash, topology, output, error, canceled);
}

inline boost::filesystem::path original_appearance_library_root(
    const boost::filesystem::path& model, const boost::filesystem::path& metadata)
{
    if (!metadata.empty() && metadata.parent_path().filename() == "downloads")
        return metadata.parent_path().parent_path();
    const boost::filesystem::path configured = boost::filesystem::path(Slic3r::data_dir()) / "models";
    boost::system::error_code error;
    const auto root = boost::filesystem::canonical(configured, error);
    const auto file = boost::filesystem::canonical(model, error);
    if (!error) {
        const auto relative = file.lexically_relative(root);
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") return root;
    }
    return {};
}

} // namespace Slic3r::GUI
