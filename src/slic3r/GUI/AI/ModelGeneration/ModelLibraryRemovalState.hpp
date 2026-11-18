#pragma once
#include "ModelLibraryEntry.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace Slic3r::GUI {

// GUI-owned removal admission/projection, shared by the existing feature host.
// It is not a file proof; workers still verify receipts and certified bytes.
class ModelLibraryRemovalState {
public:
    enum class Phase { Accepted, Committed, Finished, Failed };
    struct Event { uint64_t request; ModelLibraryEntry entry; Phase phase; };
    explicit ModelLibraryRemovalState(boost::filesystem::path root) : m_root(std::move(root)) {}
    uint64_t revision() const { return m_revision; }
    static std::string path_key(boost::filesystem::path path) {
        auto text = path.lexically_normal().generic_string();
#ifdef _WIN32
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#endif
        return text;
    }
    std::string key(const ModelLibraryEntry& entry) const {
        auto path = entry.design_only ? entry.preview_path : entry.model_path;
        if (path.empty()) path = entry.ai_image_path;
        if (path.empty()) return {};
        if (!path.is_absolute()) path = m_root / path;
        // The task copy and downloaded mirror are one registered task asset.
        // Other saved versions, even with identical bytes, remain independent.
        const auto ext = path.extension().string();
        if (!entry.design_only && !entry.job_id.empty() && (ext == ".glb" || ext == ".obj") &&
            path_key(path) == path_key(m_root / "downloads" / ("orcaslicer-ai-" + entry.job_id + ext)))
            path = m_root / entry.job_id / (ext == ".glb" ? "model.glb" : "model-vertex-color.obj");
        return std::to_string(entry.job_id.size()) + ":" + entry.job_id +
            (entry.design_only ? ":design:" : ":model:") + path_key(path);
    }
    bool same(const ModelLibraryEntry& left, const ModelLibraryEntry& right) const {
        const auto id = key(left); return !id.empty() && id == key(right);
    }
    bool blocked(const ModelLibraryEntry& entry) const { return m_excluded.count(key(entry)) != 0; }
    bool active() const { return m_active.has_value(); }
    bool committed(uint64_t request) const {
        return m_active && m_active->request == request && m_active->phase == Phase::Committed;
    }
    Event begin(const ModelLibraryEntry& entry) {
        Event event {++m_revision, entry, Phase::Accepted};
        m_active = event; m_excluded[key(entry)] = false; return event;
    }
    bool commit(uint64_t request) {
        if (!m_active || m_active->request != request || m_active->phase != Phase::Accepted) return false;
        m_active->phase = Phase::Committed; m_excluded[key(m_active->entry)] = true; ++m_revision; return true;
    }
    bool finish(uint64_t request, bool committed) {
        if (!m_active || m_active->request != request) return false;
        const auto id = key(m_active->entry);
        if (committed || m_active->phase == Phase::Committed) m_excluded[id] = true;
        else m_excluded.erase(id);
        m_active.reset(); ++m_revision; return true;
    }
    std::vector<ModelLibraryEntry> project(const std::vector<ModelLibraryEntry>& entries) const {
        std::vector<ModelLibraryEntry> result; result.reserve(entries.size());
        for (const auto& entry : entries) if (!blocked(entry)) result.push_back(entry);
        return result;
    }
    // Only a scan started at the current epoch may acknowledge an explicitly
    // re-registered asset. Repository scans already reject persisted receipts.
    void reconcile(const std::vector<ModelLibraryEntry>& entries, uint64_t scan_revision) {
        if (scan_revision != m_revision || m_active) return;
        for (const auto& entry : entries) m_excluded.erase(key(entry));
    }
private:
    boost::filesystem::path m_root;
    std::optional<Event> m_active;
    std::map<std::string, bool> m_excluded;
    uint64_t m_revision {0};
};

// Preserve both ends of a history resolution without attaching older saved
// versions to a later edit. Callers supply the actual current editor identity.
struct WorkbenchLibraryBinding {
    ModelLibraryEntry selected, resolved;
    static bool same_editor_asset(const ModelLibraryRemovalState& removals,
                                  const ModelLibraryEntry& left, const ModelLibraryEntry& right) {
        // Native activation uses a presentation id distinct from the published
        // registration id. Exact asset paths join those aliases only here;
        // library exclusion keys and independent saved versions stay distinct.
        return removals.same(left, right) || (!left.model_path.empty() && !right.model_path.empty() &&
            ModelLibraryRemovalState::path_key(left.model_path) == ModelLibraryRemovalState::path_key(right.model_path));
    }
    void replace_checked_asset(const ModelLibraryRemovalState& removals,
                               const ModelLibraryEntry& before, const ModelLibraryEntry& after) {
        if (!same_editor_asset(removals, before, resolved)) selected = before;
        resolved = after;
    }
    bool affected(const ModelLibraryRemovalState& removals, const ModelLibraryEntry& removed,
                  const ModelLibraryEntry& current) const {
        return same_editor_asset(removals, removed, current) ||
            (same_editor_asset(removals, current, resolved) && same_editor_asset(removals, removed, selected));
    }
};
} // namespace Slic3r::GUI
