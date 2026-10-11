#pragma once
#include "ModelLibraryRepository.hpp"
#include "ModelLibraryFilter.hpp"
#include <functional>

namespace Slic3r::GUI {
enum class ModelLibraryCommand { OpenModel, OpenImage, Import, Retexture, Remove, MatchColors };
struct ModelLibraryActionToken { uint64_t revision; std::string job_id; };

// The library owns selection/list identity. Actions are injected capabilities;
// neither a hidden model panel nor a native project is required to browse.
class ModelLibraryApplication final {
public:
    struct Ports {
        std::function<bool()> can_replace;
        std::function<bool(ModelLibraryCommand, const ModelLibraryEntry&)> execute;
        std::function<void()> choose_local;
    };
    explicit ModelLibraryApplication(boost::filesystem::path root, Ports ports = {})
        : m_root(std::move(root)), m_ports(std::move(ports)) {}
    const boost::filesystem::path& root() const { return m_root; }
    void set_ports(Ports ports) { m_ports = std::move(ports); ++m_revision; }
    const std::vector<ModelLibraryEntry>& entries() const { return m_entries; }
    uint64_t revision() const { return m_revision; }
    void choose_local() {
        const auto revision = m_revision;
        const auto choose = m_ports.choose_local;
        const auto allowed = m_ports.can_replace;
        if (choose && (!allowed || allowed()) && revision == m_revision) choose();
    }
    void replace(std::vector<ModelLibraryEntry> entries) { m_entries = std::move(entries); ++m_revision; }
    std::vector<size_t> filter(ModelLibraryCategory category, const wxString& query, bool images_only = false) const {
        return filter_model_library(m_entries, category, query, images_only);
    }
    ModelLibraryActionToken token(size_t index) const { return {m_revision, m_entries.at(index).job_id}; }
    const ModelLibraryEntry* find(const ModelLibraryActionToken& token) const {
        if (token.revision != m_revision) return nullptr;
        const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const auto& entry) { return entry.job_id == token.job_id; });
        return it == m_entries.end() ? nullptr : &*it;
    }
    bool execute(ModelLibraryCommand command, const ModelLibraryActionToken& token) {
        if (!find(token)) return false;
        const auto execute = m_ports.execute;
        const auto allowed = m_ports.can_replace;
        if (!execute || (allowed && !allowed())) return false;
        // Permission callbacks can pump the event loop, refresh the list or
        // detach a feature. Recheck identity after they return.
        const auto* entry = find(token);
        if (!entry) return false;
        if (command != ModelLibraryCommand::OpenImage && command != ModelLibraryCommand::Remove && entry->design_only) return false;
        const auto copy = *entry; // A callback may replace the list synchronously.
        return execute(command, copy);
    }
private:
    boost::filesystem::path m_root;
    Ports m_ports;
    std::vector<ModelLibraryEntry> m_entries;
    uint64_t m_revision {0};
};
}
