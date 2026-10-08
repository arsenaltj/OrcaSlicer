#pragma once

#include "PortraitPaletteRoles.hpp"
#include "slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace Slic3r::GUI::PortraitParentCleanup {
using Json = nlohmann::json;
using Color = AI::SemanticColoring::Color;

struct Bundle {
    Json candidate;
    std::shared_ptr<const Json> partition;
    std::shared_ptr<const AI::BeautySurfaceShapeLock> locks;
    std::string reason;
};
struct Result {
    std::map<std::string, Color> cell_colors;
    std::map<size_t, Color> root_colors;
    std::map<size_t, std::string> root_labels;
    std::map<std::string, std::string> cell_labels;
    size_t applied = 0;
    std::string reason;
    Json audit = Json::object();
};

inline bool safe_filename(const std::string& path) {
    return !path.empty() && path.size() < 128 && path.find("..") == std::string::npos &&
        std::all_of(path.begin(), path.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        });
}
inline std::string read_hash(const boost::filesystem::path& path) {
    if (boost::filesystem::is_symlink(path) || !boost::filesystem::is_regular_file(path) ||
        boost::filesystem::file_size(path) > 128ULL * 1024 * 1024) return {};
    return AI::model_artifact_sha256(path);
}
inline Json read_verified(const boost::filesystem::path& root, const std::string& name, const std::string& hash) {
    if (!safe_filename(name) || !AI::ShapeLockSet::sha256(hash) || read_hash(root / name) != hash)
        throw std::invalid_argument("parent_cleanup_reference_or_hash_mismatch");
    boost::filesystem::ifstream stream(root / name, std::ios::binary);
    return Json::parse(stream);
}
inline bool frozen_label(const std::string& label) {
    return AI::ShapeLockSet::supported_label(label) || label == "teeth" ||
        label.compare(0, 5, "iris-") == 0 || label.compare(0, 11, "periocular-") == 0;
}

// A parent partition may change while every accepted facial polygon and lock stays fixed.
inline std::string frozen_boundary(const Json& partition, const AI::BeautySurfaceShapeLock& locks) {
    Json records = Json::array(), cells = Json::array();
    for (const auto& lock : locks.document.at("locks")) {
        Json row;
        for (const auto* key : {"label", "subject_id", "parent_label", "status", "view_support",
                               "reasons", "locked_cells", "nested_cells", "periocular_cells"})
            row[key] = lock.value(key, Json::array());
        records.push_back(std::move(row));
    }
    std::sort(records.begin(), records.end(), [](const Json& a, const Json& b) {
        return std::make_pair(a.at("subject_id"), a.at("label")) < std::make_pair(b.at("subject_id"), b.at("label"));
    });
    const auto fixed = [](const Json& ring) {
        Json result = Json::array();
        for (const auto& point : ring) {
            Json p = Json::array();
            for (const auto& coordinate : point) p.push_back(int64_t(std::floor(coordinate.get<double>() * 1e9 + .5)));
            result.push_back(std::move(p));
        }
        return result;
    };
    for (const auto& face : partition.at("faces")) for (const auto& cell : face.at("cells")) {
        if (!frozen_label(cell.at("label"))) continue;
        Json holes = Json::array();
        for (const auto& ring : cell.at("holes")) holes.push_back(fixed(ring));
        cells.push_back({{"id", cell.at("id")}, {"source_face_id", face.at("source_face_id")},
            {"label", cell.at("label")}, {"parent_label", cell.at("parent_label")},
            {"subject_id", cell.at("subject_id")}, {"polygon", fixed(cell.at("polygon"))}, {"holes", holes}});
    }
    std::sort(cells.begin(), cells.end(), [](const Json& a, const Json& b) { return a.at("id") < b.at("id"); });
    return AI::beauty_leaf_digest(Json{{"geometry_id", partition.at("geometry_id")},
        {"source_sha256", partition.at("source_sha256")}, {"face_count", partition.at("face_count")},
        {"locks", records}, {"cells", cells}}.dump());
}

inline std::string root_cell_id(const Json& partition, size_t face) {
    return AI::beauty_leaf_digest(Json{{"schema", "orca.surface-root-cell/v1"},
        {"geometry_id", partition.at("geometry_id")}, {"source_sha256", partition.at("source_sha256")},
        {"source_face_id", face}}.dump());
}

inline Bundle load_bundle(const boost::filesystem::path& root, const std::string& source,
                          const std::string& geometry, size_t count) {
    Bundle result;
    try {
        const auto catalog_path = root / "portrait_parent_cleanup_catalog.json";
        if (read_hash(catalog_path).empty()) { result.reason = "candidate_catalog_missing"; return result; }
        boost::filesystem::ifstream stream(catalog_path, std::ios::binary);
        const auto catalog = Json::parse(stream);
        if (catalog.at("schema") != "orca.portrait-parent-cleanup-catalog/v1" ||
            !catalog.at("candidates").is_array() || catalog.at("candidates").size() > 32)
            throw std::invalid_argument("invalid_parent_cleanup_catalog");
        const auto& rows = catalog.at("candidates");
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const Json& row) {
            return row.at("source_sha256") == source && row.at("geometry_id") == geometry && row.at("face_count") == count;
        });
        if (found == rows.end()) { result.reason = "candidate_identity_not_found"; return result; }
        auto candidate = read_verified(root, found->at("path"), found->at("sha256"));
        if (candidate.at("schema") != "orca.portrait-parent-cleanup-candidate/v1" ||
            candidate.at("source_sha256") != source || candidate.at("geometry_id") != geometry ||
            candidate.at("face_count") != count || !candidate.at("cells").is_array() ||
            candidate.at("cells").size() > 250000)
            throw std::invalid_argument("invalid_parent_cleanup_candidate");
        for (const auto* key : {"partition_sha256", "shape_lock_fingerprint", "detail_freeze_sha256",
                               "runtime_sha256", "policy_sha256", "partition_file_sha256", "lock_sha256",
                               "frozen_boundary_sha256", "boundary_runtime_sha256", "boundary_policy_sha256"})
            if (candidate.at(key) != found->at(key) || !AI::ShapeLockSet::sha256(candidate.at(key)))
                throw std::invalid_argument("parent_cleanup_identity_drift");
        for (const auto* key : {"partition_path", "lock_path"})
            if (candidate.at(key) != found->at(key)) throw std::invalid_argument("parent_cleanup_path_drift");
        auto partition = read_verified(root, candidate.at("partition_path"), candidate.at("partition_file_sha256"));
        const auto lock = read_verified(root, candidate.at("lock_path"), candidate.at("lock_sha256"));
        const auto identity = AI::BeautySurfaceShapeLock::identity(partition);
        if (identity.at("geometry_id") != geometry || identity.at("source_sha256") != source ||
            identity.at("face_count") != count || identity.at("runtime_sha256") != candidate.at("boundary_runtime_sha256") ||
            identity.at("boundary_policy_sha256") != candidate.at("boundary_policy_sha256") ||
            partition.at("partition_sha256") != candidate.at("partition_sha256") ||
            partition.at("triangle_budget").get<size_t>() > std::min<size_t>(20000, count * 2 / 100))
            throw std::invalid_argument("parent_cleanup_partition_identity_drift");
        auto locks = AI::BeautySurfaceShapeLock::decode(lock, partition, identity, candidate.at("partition_file_sha256"));
        if (locks.fingerprint(partition) != candidate.at("shape_lock_fingerprint") ||
            frozen_boundary(partition, locks) != candidate.at("frozen_boundary_sha256"))
            throw std::invalid_argument("parent_cleanup_frozen_boundary_drift");
        result.candidate = std::move(candidate);
        result.partition = std::make_shared<Json>(std::move(partition));
        result.locks = std::make_shared<AI::BeautySurfaceShapeLock>(std::move(locks));
        result.reason = "candidate_bundle_ready";
    } catch (const std::exception& error) { result = {}; result.reason = error.what(); }
    return result;
}

inline bool can_adopt(const Bundle& bundle, const Json* partition,
                      const AI::BeautySurfaceShapeLock* locks, bool allow_upgrade) {
    if (!bundle.partition) return false;
    if (!partition) return allow_upgrade;
    if (!locks || frozen_boundary(*partition, *locks) != bundle.candidate.at("frozen_boundary_sha256")) return false;
    return allow_upgrade || partition->at("partition_sha256") == bundle.partition->at("partition_sha256");
}

inline std::map<std::string, Color> inherited_cell_colors(const Bundle& bundle,
    const std::map<std::string, Color>& previous) {
    std::map<std::string, Color> result;
    if (!bundle.partition) return result;
    for (const auto& face : bundle.partition->at("faces")) for (const auto& cell : face.at("cells")) {
        const auto id = cell.at("id").get<std::string>();
        const auto found = previous.find(id);
        if (found != previous.end()) result.emplace(id, found->second);
    }
    return result;
}

inline Result colors(const Bundle& bundle, const std::vector<std::string>& roles, const std::vector<Color>& palette) {
    Result result;
    try {
        if (!bundle.partition || !bundle.locks) { result.reason = bundle.reason; return result; }
        if (roles.size() != palette.size() || roles.empty() || !valid_portrait_roles(roles, palette.size()))
            throw std::invalid_argument("palette_roles_unavailable");
        for (const auto& rgb : palette) for (float channel : rgb)
            if (!std::isfinite(channel) || channel < 0 || channel > 1) throw std::invalid_argument("invalid_parent_palette");
        std::map<std::string, std::pair<size_t, Json>> cells;
        std::set<size_t> explicit_roots;
        std::set<std::string> subjects, frozen, seen;
        const auto relabels = bundle.candidate.value("ownership_relabels", Json::object());
        if (!relabels.is_object()) throw std::invalid_argument("invalid_parent_ownership_relabels");
        std::set<std::string> used_relabels;
        for (const auto& lock : bundle.locks->document.at("locks")) {
            subjects.insert(lock.at("subject_id").get<std::string>());
            for (const auto* field : {"locked_cells", "nested_cells", "periocular_cells"})
                for (const auto& id : lock.value(field, Json::array())) frozen.insert(id.get<std::string>());
        }
        for (const auto& face : bundle.partition->at("faces")) {
            const auto root = face.at("source_face_id").get<size_t>();
            explicit_roots.insert(root);
            for (const auto& cell : face.at("cells")) cells.emplace(cell.at("id").get<std::string>(), std::make_pair(root, cell));
        }
        for (const auto& row : bundle.candidate.at("cells")) {
            if (!row.is_array() || row.size() != 6 || !row[0].is_string() || !row[1].is_string() ||
                !row[2].is_string() || !row[3].is_number_unsigned() || !row[4].is_boolean() || !row[5].is_string())
                throw std::invalid_argument("invalid_parent_cleanup_cell");
            const auto id = row[0].get<std::string>(), role = row[1].get<std::string>(), label = row[2].get<std::string>();
            const size_t root = row[3];
            const bool implicit = row[4];
            const auto subject = row[5].get<std::string>();
            if (!AI::ShapeLockSet::sha256(id) || !seen.insert(id).second || frozen.count(id) ||
                root >= bundle.partition->at("face_count").get<size_t>() || !subjects.count(subject) ||
                (role == "portrait-skin" && label != "skin") ||
                ((role == "portrait-light" || role == "portrait-mid") && label != "cloth") ||
                (role != "portrait-skin" && role != "portrait-light" && role != "portrait-mid"))
                throw std::invalid_argument("parent_cleanup_cell_boundary_drift");
            if (implicit) {
                if (explicit_roots.count(root) || root_cell_id(*bundle.partition, root) != id)
                    throw std::invalid_argument("parent_cleanup_mixed_root_or_mapping_drift");
                result.root_labels.emplace(root, label);
            } else {
                const auto cell = cells.find(id);
                if (cell == cells.end() || cell->second.first != root ||
                    cell->second.second.at("subject_id") != subject || frozen_label(cell->second.second.at("label")))
                    throw std::invalid_argument("parent_cleanup_cell_boundary_drift");
                const auto& source_cell = cell->second.second;
                if (source_cell.at("label") != label) {
                    // Reviewed ownership is separate from the immutable historical boundary label.
                    const auto source_label = source_cell.at("label").get<std::string>();
                    const auto proposal = relabels.find(id);
                    if ((source_label != "face" && source_label != "R6" && source_label != "skin" &&
                         source_label != "hair" && source_label != "cloth") ||
                        proposal == relabels.end() || proposal->size() != 3 ||
                        proposal->at("source_label") != source_cell.at("label") ||
                        proposal->at("source_parent_label") != source_cell.at("parent_label") ||
                        proposal->at("target_label") != label)
                        throw std::invalid_argument("parent_cleanup_ownership_mapping_drift");
                    used_relabels.insert(id);
                }
                result.cell_labels.emplace(id, label);
            }
            const auto slot = std::find(roles.begin(), roles.end(), role);
            if (slot == roles.end()) {
                if (role == "portrait-mid") continue;
                throw std::invalid_argument("required_palette_role_missing");
            }
            const auto color = palette.at(size_t(slot - roles.begin()));
            if (implicit) result.root_colors.emplace(root, color);
            else result.cell_colors.emplace(id, color);
        }
        if (used_relabels.size() != relabels.size())
            throw std::invalid_argument("unused_parent_ownership_relabel");
        result.applied = result.root_colors.size() + result.cell_colors.size();
        result.reason = result.applied ? "candidate_applied" : "candidate_empty";
    } catch (const std::exception& error) { result = {}; result.reason = error.what(); }
    return result;
}

inline void apply_roots(const Result& result, AI::SemanticColoring::FaceColors& faces,
                        AI::SemanticColoring::SubfaceColors& subfaces) {
    if (result.root_colors.empty()) return;
    std::map<size_t, Color> composed(faces.begin(), faces.end());
    for (const auto& entry : result.root_colors) composed[entry.first] = entry.second;
    faces.assign(composed.begin(), composed.end());
    subfaces.erase(std::remove_if(subfaces.begin(), subfaces.end(), [&](const auto& row) {
        return result.root_colors.count(row.face_id) != 0;
    }), subfaces.end());
}
} // namespace Slic3r::GUI::PortraitParentCleanup
