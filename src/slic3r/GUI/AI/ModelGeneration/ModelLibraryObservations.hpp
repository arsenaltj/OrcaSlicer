#pragma once
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace Slic3r::GUI {
// Mutable display observations stay outside the exact certificate document.
// This internal helper is included only by the library's implementation files.
class ModelLibraryObservations {
public:
    static bool has_check_certificate(const nlohmann::json& metadata)
    {
        return metadata.is_object() && metadata.contains("workbench_check_certificate");
    }

    static boost::filesystem::path observations_path(boost::filesystem::path metadata_path)
    {
        metadata_path.replace_extension(".observations.json");
        return metadata_path;
    }

    // Viewing, importing and print feedback do not reissue topology proof.
    // Only these display observations may change beside a certified record.
    static bool write_observations(const boost::filesystem::path& metadata_path,
        const nlohmann::json& metadata, const nlohmann::json& updates)
    {
        boost::filesystem::path temporary;
        try {
            if (!has_check_certificate(metadata) || !valid_observation_fields(updates) || updates.empty()) return false;
            const auto hash = metadata.value("model_sha256", std::string{});
            if (!valid_model_hash(hash)) return false;
            const auto path = observations_path(metadata_path);
            if (boost::filesystem::is_symlink(path)) return false;
            auto fields = read_observation_fields(path, hash);
            if (boost::filesystem::exists(path) && !fields.is_object()) return false;
            if (!fields.is_object()) fields = nlohmann::json::object();
            for (auto it = updates.begin(); it != updates.end(); ++it) fields[it.key()] = it.value();
            const nlohmann::json document {{"schema", "orca.model-library-observations/v1"},
                {"model_sha256", hash}, {"fields", std::move(fields)}};
            temporary = path.parent_path() / boost::filesystem::unique_path("library-observations-%%%%-%%%%.tmp");
            {
                boost::filesystem::ofstream output(temporary, std::ios::binary);
                output << document.dump(); output.close();
                if (!output) throw std::runtime_error("Model observations could not be written.");
            }
            boost::filesystem::rename(temporary, path);
            return true;
        } catch (...) {
            if (!temporary.empty()) {
                boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored);
            }
            return false;
        }
    }

    static std::optional<boost::filesystem::path> owned_observations_companion(
        const boost::filesystem::path& metadata_path, const nlohmann::json& metadata)
    {
        if (!has_check_certificate(metadata)) return std::nullopt;
        const auto identity = metadata.find("model_sha256");
        if (identity == metadata.end() || !identity->is_string()) return std::nullopt;
        const auto hash = identity->get<std::string>();
        if (!valid_model_hash(hash)) return std::nullopt;
        const auto path = observations_path(metadata_path);
        return read_observation_fields(path, hash).is_object()
            ? std::optional<boost::filesystem::path>(path) : std::nullopt;
    }

    static nlohmann::json overlay(nlohmann::json summary,
        const boost::filesystem::path& metadata_path)
    {
        if (!summary.is_object()) return summary;
        const auto path = observations_path(metadata_path);
        boost::system::error_code error;
        if (!boost::filesystem::is_regular_file(path, error) || error) return summary;
        const auto fields = read_observation_fields(path, certified_model_hash(metadata_path));
        if (fields.is_object()) for (auto it = fields.begin(); it != fields.end(); ++it)
            summary[it.key()] = it.value();
        return summary;
    }

private:
    static bool valid_model_hash(const std::string& hash)
    {
        return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    static bool valid_observation_fields(const nlohmann::json& fields)
    {
        if (!fields.is_object()) return false;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (it.key() == "load_seconds") {
                if (!it->is_number()) return false;
                const auto seconds = it->get<double>();
                if (!std::isfinite(seconds) || seconds < 0.0) return false;
            } else if (it.key() == "imported_at" || it.key() == "print_feedback_at") {
                if (!it->is_number_integer() || *it < 0) return false;
            } else if (it.key() == "print_feedback") {
                if (!it->is_string() || (*it != "success" && *it != "issue")) return false;
            } else return false;
        }
        return true;
    }
    static nlohmann::json read_observation_fields(const boost::filesystem::path& path,
        const std::string& model_hash)
    {
        try {
            boost::system::error_code error;
            const auto bytes = boost::filesystem::file_size(path, error);
            if (!valid_model_hash(model_hash) || error || bytes == 0 || bytes > 16 * 1024 ||
                boost::filesystem::is_symlink(path)) return nullptr;
            boost::filesystem::ifstream input(path, std::ios::binary);
            const auto document = nlohmann::json::parse(input, nullptr, false);
            if (!document.is_object() || document.value("schema", std::string{}) !=
                    "orca.model-library-observations/v1" ||
                document.value("model_sha256", std::string{}) != model_hash ||
                !document.contains("fields") || !valid_observation_fields(document.at("fields"))) return nullptr;
            return document.at("fields");
        } catch (...) { return nullptr; }
    }

    struct Identity {
        std::filesystem::file_time_type modified;
        uintmax_t size {0};
        std::string model_hash;
    };
    // Summaries intentionally omit model_sha256. Cache only this independent
    // identity projection, leaving the existing summary cache contract intact.
    // Companion fields are always read fresh so their refresh/deletion is visible.
    static std::string certified_model_hash(const boost::filesystem::path& path)
    {
        try {
            const auto native = std::filesystem::path(path.native());
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(native, error);
            if (error) return {};
            const auto size = std::filesystem::file_size(native, error);
            if (error || size == 0 || size > 128ULL * 1024 * 1024 || boost::filesystem::is_symlink(path)) return {};
            thread_local std::map<std::string, Identity> identities;
            const auto key = path.generic_string();
            const auto found = identities.find(key);
            if (found != identities.end() && found->second.modified == modified && found->second.size == size)
                return found->second.model_hash;
            bool certified = false;
            boost::filesystem::ifstream input(path, std::ios::binary);
            const auto metadata = nlohmann::json::parse(input,
                [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
                    if (depth != 1 || event != nlohmann::json::parse_event_t::key) return true;
                    const auto& field = value.get_ref<const std::string&>();
                    if (field == "workbench_check_certificate") certified = true;
                    return field == "model_sha256";
                }, false);
            if (!certified || !metadata.is_object() || !metadata.contains("model_sha256") ||
                !metadata.at("model_sha256").is_string()) return {};
            const auto hash = metadata.at("model_sha256").get<std::string>();
            if (!valid_model_hash(hash)) return {};
            const auto after_modified = std::filesystem::last_write_time(native, error);
            if (error || after_modified != modified) return {};
            const auto after_size = std::filesystem::file_size(native, error);
            if (error || after_size != size) return {};
            if (identities.size() >= 512) identities.clear();
            identities[key] = {modified, size, hash};
            return hash;
        } catch (...) { return {}; }
    }
};
}
