#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// UI-independent coordinator for Beauty operations. The model panel owns the
// actual files and preview; this class owns the transaction timeline and the
// interaction lock so the two cannot silently drift apart.
class BeautyWorkbenchTransactionController final {
public:
    enum class OperationKind {
        Selection,
        AppearanceRecolor,
        SurfaceSoften,
        MeshRepair,
        SpotCleanup,
        SemanticReoptimization,
        AcceptCandidate,
    };

    enum class TaskState {
        Idle,
        Editing,
        Submitting,
        PreviewReady,
        Accepting,
        Reoptimizing,
        CancelRequested,
        Failed,
    };

    struct InteractionCapabilities {
        bool can_orbit {true};
        bool can_zoom {true};
        bool can_view_original {true};
        bool can_view_semantic_regions {true};
        bool can_view_logs {true};
        bool can_edit_selection {false};
        bool can_submit_operation {false};
        bool can_reoptimize {false};
        bool can_change_model {true};
        bool can_accept {false};
        bool can_discard {false};
        bool can_cancel {false};
        bool can_undo {false};
        bool can_redo {false};
    };

    struct Entry {
        OperationKind kind {OperationKind::Selection};
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
        std::function<bool()> can_undo;
        std::function<bool()> can_redo;
    };

    using StateChanged = std::function<void(TaskState)>;

    void set_state_changed(StateChanged callback) { m_state_changed = std::move(callback); }

    TaskState state() const { return m_state; }
    const InteractionCapabilities& capabilities() const { return m_capabilities; }
    bool processing() const {
        return m_state == TaskState::Submitting || m_state == TaskState::Accepting ||
            m_state == TaskState::Reoptimizing || m_state == TaskState::CancelRequested;
    }

    bool begin(OperationKind kind)
    {
        if (processing()) return false;
        m_active_kind = kind;
        m_state = kind == OperationKind::SemanticReoptimization ? TaskState::Reoptimizing :
            kind == OperationKind::AcceptCandidate ? TaskState::Accepting : TaskState::Submitting;
        recompute_capabilities();
        notify();
        return true;
    }

    void mark_preview_ready()
    {
        m_state = TaskState::PreviewReady;
        recompute_capabilities();
        notify();
    }

    void mark_editing()
    {
        m_state = TaskState::Editing;
        recompute_capabilities();
        notify();
    }

    void mark_failed(std::string message = {})
    {
        m_last_error = std::move(message);
        m_state = TaskState::Failed;
        recompute_capabilities();
        notify();
    }

    void request_cancel()
    {
        if (!processing()) return;
        m_state = TaskState::CancelRequested;
        recompute_capabilities();
        notify();
    }

    void finish(bool success, bool candidate_ready = false, std::string error = {})
    {
        if (success) {
            m_last_error.clear();
            m_state = candidate_ready ? TaskState::PreviewReady : TaskState::Editing;
        } else {
            mark_failed(std::move(error));
            return;
        }
        recompute_capabilities();
        notify();
    }

    void reset()
    {
        m_entries.clear();
        m_redo.clear();
        m_last_error.clear();
        m_state = TaskState::Idle;
        recompute_capabilities();
        notify();
    }

    void truncate_to(size_t count)
    {
        if (processing() || count > m_entries.size()) return;
        m_entries.resize(count);
        m_redo.clear();
        m_state = TaskState::Editing;
        recompute_capabilities();
        notify();
    }

    void record(Entry entry)
    {
        if (processing() || !entry.undo || !entry.redo) return;
        m_entries.push_back(std::move(entry));
        m_redo.clear();
        if (m_state != TaskState::PreviewReady) m_state = TaskState::Editing;
        recompute_capabilities();
        notify();
    }

    bool undo()
    {
        if (processing() || m_entries.empty()) return false;
        if (m_entries.back().can_undo && !m_entries.back().can_undo()) {
            mark_failed("Beauty history source is missing or changed");
            return false;
        }
        Entry entry = std::move(m_entries.back());
        m_entries.pop_back();
        if (entry.undo) entry.undo();
        m_redo.push_back(std::move(entry));
        recompute_capabilities();
        notify();
        return true;
    }

    bool redo()
    {
        if (processing() || m_redo.empty()) return false;
        if (m_redo.back().can_redo && !m_redo.back().can_redo()) {
            mark_failed("Beauty history candidate is missing or changed");
            return false;
        }
        Entry entry = std::move(m_redo.back());
        m_redo.pop_back();
        if (entry.redo) entry.redo();
        m_entries.push_back(std::move(entry));
        recompute_capabilities();
        notify();
        return true;
    }

    const std::string& last_error() const { return m_last_error; }
    size_t undo_count() const { return m_entries.size(); }
    size_t redo_count() const { return m_redo.size(); }

private:
    void recompute_capabilities()
    {
        const bool busy = processing();
        const bool preview = m_state == TaskState::PreviewReady;
        m_capabilities = {};
        // Failed operations leave the stable model editable and retryable;
        // only the failed candidate publication actions remain unavailable.
        m_capabilities.can_edit_selection = !busy;
        m_capabilities.can_submit_operation = !busy;
        m_capabilities.can_reoptimize = !busy;
        m_capabilities.can_change_model = !busy;
        m_capabilities.can_accept = preview && !busy;
        m_capabilities.can_discard = preview && !busy;
        m_capabilities.can_cancel = busy;
        m_capabilities.can_undo = !busy && !m_entries.empty();
        m_capabilities.can_redo = !busy && !m_redo.empty();
    }

    void notify() { if (m_state_changed) m_state_changed(m_state); }

    TaskState m_state {TaskState::Idle};
    OperationKind m_active_kind {OperationKind::Selection};
    InteractionCapabilities m_capabilities;
    std::vector<Entry> m_entries;
    std::vector<Entry> m_redo;
    std::string m_last_error;
    StateChanged m_state_changed;
};

} // namespace Slic3r::GUI
