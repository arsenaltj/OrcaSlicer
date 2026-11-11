#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// A scan-local, read-only relationship index. It proposes candidates only;
// opening still validates current bytes, geometry, appearance and certificate.
class ModelHistoryRepairIndex {
public:
    struct Node {
        boost::filesystem::path model, record, source;
        std::string job_id, model_hash, source_hash;
        bool published {false}, repaired {false};
    };
    explicit ModelHistoryRepairIndex(boost::filesystem::path root) : m_root(std::move(root)),
        m_root_stamp(stamp(m_root)), m_downloads_stamp(stamp(m_root / "downloads")) {}

    bool current() const {
        return stamp(m_root) == m_root_stamp && stamp(m_root / "downloads") == m_downloads_stamp;
    }

    void add(const boost::filesystem::path& model, const boost::filesystem::path& record,
             const std::string& job_id) {
        auto node = read(model, record, job_id);
        m_order.push_back(key(model));
        m_nodes[key(model)] = std::move(node);
    }

    std::vector<Node> candidates(const boost::filesystem::path& selected) const {
        std::vector<Node> result;
        for (const auto& id : m_order) {
            const auto& node = m_nodes.at(id);
            if (node.repaired && key(selected) != id && descends_from(node, selected)) result.push_back(node);
        }
        return result; // Input order is the library's newest-first order.
    }

    // Re-read every link on the loading worker. The UI projection and a stale
    // scan never grant permission to reuse a repair after its source changes.
    bool current_chain(const Node& candidate, const boost::filesystem::path& selected,
                       const std::function<bool()>& canceled = {}) const {
        auto current = candidate;
        std::set<std::string> seen;
        for (size_t depth = 0; depth <= m_nodes.size(); ++depth) {
            if ((canceled && canceled()) || !seen.insert(key(current.model)).second) return false;
            const auto fresh = read(current.model, current.record, current.job_id);
            if (!fresh.published || fresh.source != current.source || fresh.model_hash != current.model_hash ||
                fresh.source_hash != current.source_hash ||
                !safe_file(fresh.model) || !safe_file(fresh.source) ||
                AI::model_artifact_sha256(fresh.model) != fresh.model_hash ||
                AI::model_artifact_sha256(fresh.source) != fresh.source_hash) return false;
            if (depth == 0 && !fresh.repaired) return false;
            if (same_source_path(fresh.source, selected))
                return !seen.count(key(selected)) && (key(fresh.source) == key(selected) ||
                    (safe_file(selected) && AI::model_artifact_sha256(selected) == fresh.source_hash));
            const auto parent = m_nodes.find(key(fresh.source));
            if (parent == m_nodes.end()) return false;
            current = parent->second;
        }
        return false;
    }

private:
    static std::filesystem::file_time_type stamp(const boost::filesystem::path& path) {
        std::error_code error;
        const auto value = std::filesystem::last_write_time(std::filesystem::path(path.native()), error);
        return error ? std::filesystem::file_time_type{} : value;
    }
    static std::string key(const boost::filesystem::path& path) {
        auto text = path.lexically_normal().generic_string();
#ifdef _WIN32
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#endif
        return text;
    }
    bool same_source_path(const boost::filesystem::path& source, const boost::filesystem::path& selected) const {
        if (key(source) == key(selected)) return true;
        const auto entry = m_nodes.find(key(selected));
        if (entry == m_nodes.end() || source.extension() != selected.extension()) return false;
        const auto extension = selected.extension().string();
        if (extension != ".glb" && extension != ".obj") return false;
        const auto& job = entry->second.job_id;
        const auto download = m_root / "downloads" / ("orcaslicer-ai-" + job + extension);
        const auto task = m_root / job / (extension == ".glb" ? "model.glb" : "model-vertex-color.obj");
        // The library prefers the task copy when both exist. Publications may
        // still reference the downloaded copy of that same job. This only
        // proposes an alias; current_chain verifies both files' exact bytes.
        return (key(source) == key(download) && key(selected) == key(task)) ||
               (key(source) == key(task) && key(selected) == key(download));
    }
    bool safe_file(const boost::filesystem::path& path) const {
        boost::system::error_code error;
        const auto relative = path.lexically_normal().lexically_relative(m_root.lexically_normal());
        if (relative.empty() || relative.is_absolute()) return false;
        auto prefix = m_root;
        for (const auto& part : relative) {
            if (part == "..") return false;
            prefix /= part;
            if (boost::filesystem::is_symlink(prefix, error) || error) return false;
        }
        return boost::filesystem::is_regular_file(path, error) && !error;
    }
    Node read(const boost::filesystem::path& model, const boost::filesystem::path& record,
              const std::string& job_id) const {
        Node node {model, record, {}, job_id};
        try {
            if (!safe_file(record)) return node;
            boost::system::error_code error;
            const auto bytes = boost::filesystem::file_size(record, error);
            if (error || !bytes || bytes > 128ULL * 1024 * 1024) return node;
            boost::filesystem::ifstream input(record, std::ios::binary);
            using Json = nlohmann::json;
            const auto data = Json::parse(input, [](int depth, Json::parse_event_t event, Json& value) {
                if (event != Json::parse_event_t::key) return true;
                const auto& name = value.get_ref<const std::string&>();
                if (depth == 1) return name == "job_id" || name == "source" || name == "schema_version" ||
                    name == "schema" || name == "transaction_status" || name == "model_path" ||
                    name == "source_model" || name == "source_sha256" || name == "model_sha256" ||
                    name == "finishing" || name == "workbench_check_certificate";
                if (depth == 2) return name == "repair_mesh" || name == "repair_geometry_verified" || name == "metadata_path";
                return false;
            }, false);
            if (!data.is_object()) return node;
            const bool native = data.value("source", std::string{}) == "native_workbench_partition" &&
                data.value("schema", std::string{}) == "orca.native-workbench-partition-commit/v1" &&
                data.value("transaction_status", std::string{}) == "committed";
            // Native commit identity is its published record name; those
            // records predate (and do not contain) the ordinary job_id field.
            if (data.contains("job_id") ? data.at("job_id") != job_id :
                (!native || record.filename() != "orcaslicer-ai-" + job_id + ".json")) return node;
            const auto local_path = [&](const boost::filesystem::path& path) {
                if (path.empty()) return boost::filesystem::path{};
                if (!path.is_absolute()) return (m_root / path).lexically_normal();
                const auto inside = [](const boost::filesystem::path& path, const boost::filesystem::path& root) {
                    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
                    return !relative.empty() && !relative.is_absolute() &&
                        std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; });
                };
                if (inside(path, m_root)) return path.lexically_normal();
                // Native publications store absolute paths. A copied record may
                // still name its original publication; propose its local copy.
                // The complete two-location evidence validator must approve it
                // later, so this projection cannot authorize a relocated proof.
                const auto published_record = boost::filesystem::path(data.at("workbench_check_certificate")
                    .at("metadata_path").get<std::string>());
                const auto published_root = published_record.parent_path().filename() == "downloads"
                    ? published_record.parent_path().parent_path() : published_record.parent_path();
                if (!published_record.is_absolute() || !inside(path, published_root)) return boost::filesystem::path{};
                return (m_root / path.lexically_normal().lexically_relative(published_root.lexically_normal())).lexically_normal();
            };
            const auto local_model = local_path(boost::filesystem::path(data.at("model_path").get<std::string>()));
            node.source = local_path(boost::filesystem::path(data.at("source_model").get<std::string>()));
            if (local_model.empty() || node.source.empty() || key(local_model) != key(model)) return node;
            node.model_hash = data.at("model_sha256").get<std::string>();
            node.source_hash = data.at("source_sha256").get<std::string>();
            const auto valid_hash = [](const std::string& hash) {
                return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
            };
            if (!valid_hash(node.model_hash) || !valid_hash(node.source_hash)) return node;
            const bool saved = data.value("source", std::string{}) == "local_finishing" &&
                data.value("schema_version", 0) == 4 && !data.contains("schema") &&
                !data.contains("transaction_status");
            node.published = native || saved;
            const auto finishing = data.value("finishing", Json::object());
            node.repaired = node.published && (native || (finishing.value("repair_mesh", false) &&
                finishing.value("repair_geometry_verified", false)));
        } catch (const std::exception&) {}
        return node;
    }
    bool descends_from(Node current, const boost::filesystem::path& selected) const {
        std::set<std::string> seen;
        while (current.published && seen.insert(key(current.model)).second) {
            if (same_source_path(current.source, selected)) return !seen.count(key(selected));
            const auto parent = m_nodes.find(key(current.source));
            if (parent == m_nodes.end()) break;
            current = parent->second;
        }
        return false;
    }
    boost::filesystem::path m_root;
    std::filesystem::file_time_type m_root_stamp, m_downloads_stamp;
    std::map<std::string, Node> m_nodes;
    std::vector<std::string> m_order;
};

} // namespace Slic3r::GUI
