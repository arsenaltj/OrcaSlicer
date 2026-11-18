#pragma once
#include "ModelLibraryRepository.hpp"
#include "ModelLibraryFilter.hpp"
#include "ModelLibraryRemovalState.hpp"
#include <functional>

namespace Slic3r::GUI {
enum class ModelLibraryCommand { OpenModel, OpenImage, Import, Retexture, Remove, MatchColors, Export, RecordPrintResult };
struct ModelLibraryActionToken { uint64_t revision; std::string job_id; };
enum class ModelLibraryCommitReason { Generated, Checked, Beauty, Imported };
struct ModelLibraryCommit {
    ModelLibraryEntry entry;
    std::string record_sha256;
    ModelLibraryCommitReason reason {ModelLibraryCommitReason::Generated};
    uint64_t revision {0};
    bool foreground {false};
    uint64_t request {0};
    static uint64_t next_request() { static std::atomic<uint64_t> next {0}; return ++next; }
};

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
        : m_root(std::move(root)), m_ports(std::move(ports)), m_removals(std::make_shared<ModelLibraryRemovalState>(m_root)) {}
    const boost::filesystem::path& root() const { return m_root; }
    void set_ports(Ports ports) { m_ports = std::move(ports); ++m_revision; }
    const std::vector<ModelLibraryEntry>& entries() const { return m_entries; }
    uint64_t revision() const { return m_revision; }
    bool can_replace() const {const auto allowed=m_ports.can_replace;const auto revision=m_revision;return (!allowed || allowed()) && revision==m_revision;}
    void choose_local() {
        const auto revision = m_revision;
        const auto choose = m_ports.choose_local;
        const auto allowed = m_ports.can_replace;
        if (choose && (!allowed || allowed()) && revision == m_revision) choose();
    }
    uint64_t commit_revision() const { return m_commit_revision; }
    std::optional<ModelLibraryCommit> commit(ModelLibraryCommit event) {
        if (event.entry.job_id.empty() || event.entry.model_path.empty() || m_removals->blocked(event.entry)) return {};
        const auto found = m_commits.find(event.entry.job_id);
        if (found != m_commits.end() && event.request && event.request < found->second.request) return {};
        if (found != m_commits.end() && !event.record_sha256.empty() &&
            found->second.record_sha256 == event.record_sha256) return {};
        event.revision = ++m_commit_revision;
        m_commits[event.entry.job_id] = event;
        merge_commits(m_snapshot_entries, event.revision - 1);
        apply_removals();
        return event;
    }
    // A scan sees the filesystem at an earlier epoch. Overlay only records
    // committed after it started; removal projection always runs afterwards.
    void merge_commits(std::vector<ModelLibraryEntry>& entries, uint64_t observed) const {
        bool changed = false;
        for (const auto& [job, event] : m_commits) {
            if (event.revision <= observed || m_removals->blocked(event.entry)) continue;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.job_id == job; }), entries.end());
            entries.push_back(event.entry);
            changed = true;
        }
        if (!changed) return; // A scan keeps its own browsing order when nothing is overlaid.
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.generated_at != b.generated_at ? a.generated_at > b.generated_at : a.job_id < b.job_id;
        });
    }
    void replace(std::vector<ModelLibraryEntry> entries, uint64_t removal_revision, uint64_t observed) {
        m_removals->reconcile(entries, removal_revision);
        merge_commits(entries, observed);
        m_snapshot_entries = std::move(entries); apply_removals();
    }
    void replace(std::vector<ModelLibraryEntry> entries, uint64_t removal_revision) {
        replace(std::move(entries), removal_revision, m_commit_revision);
    }
    void replace(std::vector<ModelLibraryEntry> entries) { replace(std::move(entries), m_removals->revision()); }
    uint64_t removal_revision() const { return m_removals->revision(); }
    bool removal_active() const { return m_removals->active(); }
    void set_removal_state(std::shared_ptr<ModelLibraryRemovalState> removals) {
        m_removals = std::move(removals); apply_removals();
    }
    void apply_removals() { m_entries = m_removals->project(m_snapshot_entries); ++m_revision; }
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
        if (command != ModelLibraryCommand::OpenImage && command != ModelLibraryCommand::Remove && command != ModelLibraryCommand::Export && entry->design_only) return false;
        const auto copy = *entry; // A callback may replace the list synchronously.
        return execute(command, copy);
    }
private:
    boost::filesystem::path m_root;
    Ports m_ports;
    std::vector<ModelLibraryEntry> m_entries;
    std::vector<ModelLibraryEntry> m_snapshot_entries;
    std::shared_ptr<ModelLibraryRemovalState> m_removals;
    std::map<std::string, ModelLibraryCommit> m_commits;
    uint64_t m_commit_revision {0};
    uint64_t m_revision {0};
};
}
