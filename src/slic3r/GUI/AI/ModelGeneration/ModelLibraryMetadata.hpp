#pragma once

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdint>
#include <chrono>
#include <map>
#include <set>
#include <string>

namespace Slic3r::GUI {

// Read-only list summaries. Never use these projections to save or restore an
// edited model: the authoritative metadata still contains the complete work.
class ModelLibraryMetadata
{
public:
    ModelLibraryMetadata() = default;
    ~ModelLibraryMetadata() { persist(); }
    ModelLibraryMetadata(const ModelLibraryMetadata&) = delete;
    ModelLibraryMetadata& operator=(const ModelLibraryMetadata&) = delete;
    // Used only by the deletion preflight; skip large editing arrays while
    // preserving both the accepted document and any newer draft dependency.
    static bool references_base(std::istream& input, const std::string& filename)
    {
        using Json = nlohmann::json;
        const auto record = Json::parse(input, [](int depth, Json::parse_event_t event, Json& value) {
            if (event != Json::parse_event_t::key) return true;
            const auto& key = value.get_ref<const std::string&>();
            if (depth == 1) return key == "beauty_workbench" || key == "beauty_puzzle_draft";
            if (depth == 2) return key == "puzzle_base_file";
            return false;
        }, false);
        if (!record.is_object()) throw std::runtime_error("Unreadable history metadata");
        for (const char* key : {"beauty_workbench", "beauty_puzzle_draft"}) {
            const auto it = record.find(key);
            if (it == record.end() || !it->is_object()) continue;
            const auto base = it->find("puzzle_base_file");
            if (base != it->end() && base->is_string() && base->get<std::string>() == filename) return true;
        }
        return false;
    }

    static nlohmann::json parse(std::istream& input)
    {
        using Json = nlohmann::json;
        static const std::set<std::string> fields {
            "source", "job_id", "model_path", "generated_at", "imported_at",
            "triangle_count", "load_seconds", "print_feedback", "use_printable_colors",
            "provider", "provider_task_id", "provider_conversion_task_id", "prompt",
            "palette", "palette_roles", "reference_image_path", "ai_image_path",
            "color_intent_path", "color_intent_schema", "color_intent_sha256"
        };
        auto result = Json::parse(input, [](int depth, Json::parse_event_t event, Json& value) {
            return depth != 1 || event != Json::parse_event_t::key ||
                   fields.count(value.get_ref<const std::string&>()) != 0;
        }, false);
        if (!result.is_object()) return {};
        // A broken optional field must not prevent the rest of the library
        // from opening. The full model reader retains its own validation.
        for (auto it = result.begin(); it != result.end();) {
            const auto& key = it.key();
            const bool valid = key == "palette" ? it->is_array() :
                key == "palette_roles" ? it->is_object() :
                key == "use_printable_colors" ? it->is_boolean() :
                key == "load_seconds" ? it->is_number() :
                key == "generated_at" || key == "imported_at" || key == "triangle_count" ? it->is_number_integer() :
                it->is_string();
            if (!valid) it = result.erase(it); else ++it;
        }
        return result;
    }

    nlohmann::json read(const boost::filesystem::path& path)
    {
        load_persistent(path.parent_path());
        const auto key = path.generic_string();
        Stamp before;
        if (!stamp(path, before)) { m_entries.erase(key); m_persistent.erase(key); m_persistent_dirty=true; return {}; }
        const auto found = m_entries.find(key);
        if (found != m_entries.end() && found->second.stamp == before)
            return found->second.summary;
        const auto cached = m_persistent.find(key);
        if (cached != m_persistent.end() && cached->second.stamp == before) {
            m_entries[key] = cached->second;
            return cached->second.summary;
        }
        boost::filesystem::ifstream input(path, std::ios::binary);
        auto summary = input ? parse(input) : nlohmann::json();
        Stamp after;
        // Do not cache an incomplete snapshot while another worker is saving.
        if (stamp(path, after) && before == after && summary.is_object()) {
            if (m_entries.size() >= 512) m_entries.clear();
            m_entries[key] = {after, summary};
            if (m_persistent.size() >= 1024) m_persistent.clear();
            m_persistent[key] = {after, summary};
            m_persistent_dirty=true;
        } else m_entries.erase(key);
        return summary;
    }

private:
    struct Stamp {
        std::filesystem::file_time_type modified;
        uintmax_t size;
        bool operator==(const Stamp& other) const { return modified == other.modified && size == other.size; }
    };
    struct Entry { Stamp stamp; nlohmann::json summary; };
    static constexpr const char* cache_name = "library-summary-cache-v1.json";
    static int64_t stamp_ticks(const Stamp& value)
    {
        return static_cast<int64_t>(value.modified.time_since_epoch().count());
    }
    static bool stamp_from(const nlohmann::json& value, Stamp& result)
    {
        try {
            if (!value.is_object() || !value.at("modified").is_number_integer() || !value.at("size").is_number_unsigned()) return false;
            const auto ticks=value.at("modified").get<int64_t>();
            result.modified=std::filesystem::file_time_type(std::filesystem::file_time_type::duration(ticks));
            result.size=value.at("size").get<uintmax_t>();
            return true;
        } catch (...) { return false; }
    }
    void load_persistent(const boost::filesystem::path& directory)
    {
        const auto cache_path=directory/cache_name;
        if (m_cache_loaded && cache_path==m_cache_path) return;
        if (m_cache_loaded && m_persistent_dirty) persist();
        m_cache_loaded=true;m_cache_path=cache_path;m_persistent.clear();m_persistent_dirty=false;
        boost::filesystem::ifstream input(cache_path,std::ios::binary);
        if(!input)return;
        try {
            const auto root=nlohmann::json::parse(input,nullptr,false);
            if(!root.is_object() || root.value("schema",std::string())!="orcaslicer.history-summary-cache.v1")return;
            const auto entries=root.find("entries");if(entries==root.end() || !entries->is_object())return;
            for(auto it=entries->begin();it!=entries->end() && m_persistent.size()<1024;++it) {
                if(!it.value().is_object() || !it.value().contains("summary"))continue;
                Stamp stamp;
                if(!stamp_from(it.value(),stamp) || !it.value().at("summary").is_object())continue;
                m_persistent.emplace(it.key(),Entry{stamp,it.value().at("summary")});
            }
        } catch (...) { m_persistent.clear(); }
    }
    void persist()
    {
        if(!m_cache_loaded || !m_persistent_dirty || m_cache_path.empty())return;
        try {
            nlohmann::json root{{"schema","orcaslicer.history-summary-cache.v1"},{"entries",nlohmann::json::object()}};
            for(const auto& item:m_persistent) {
                root["entries"][item.first]={{"modified",stamp_ticks(item.second.stamp)},
                    {"size",item.second.stamp.size},{"summary",item.second.summary}};
            }
            boost::system::error_code error;
            boost::filesystem::create_directories(m_cache_path.parent_path(),error);
            const auto temporary=m_cache_path.parent_path()/boost::filesystem::unique_path("history-summary-%%%%-%%%%.tmp");
            boost::filesystem::ofstream output(temporary,std::ios::binary);output<<root.dump();output.close();
            if(!output) { boost::filesystem::remove(temporary,error); return; }
            boost::filesystem::remove(m_cache_path,error);
            boost::filesystem::rename(temporary,m_cache_path,error);
            if(!error)m_persistent_dirty=false; else boost::filesystem::remove(temporary,error);
        } catch (...) {}
    }
    static bool stamp(const boost::filesystem::path& path, Stamp& result)
    {
        const std::filesystem::path native(path.native());
        std::error_code error;
        result.modified = std::filesystem::last_write_time(native, error);
        if (error) return false;
        result.size = std::filesystem::file_size(native, error);
        return !error;
    }
    std::map<std::string, Entry> m_entries;
    std::map<std::string, Entry> m_persistent;
    boost::filesystem::path m_cache_path;
    bool m_cache_loaded=false;
    bool m_persistent_dirty=false;
};
}
