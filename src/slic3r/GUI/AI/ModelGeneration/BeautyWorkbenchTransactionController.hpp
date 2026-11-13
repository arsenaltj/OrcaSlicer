#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// UI-independent coordinator for Beauty operations. The session owns snapshot
// and file lifetimes, the view restores previews, and this controller owns the
// transaction timeline and interaction lock.
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
        Restoring,
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
        std::function<bool()> restore_undo;
        std::function<bool()> restore_redo;
        std::function<void(std::function<void(bool)>)> restore_undo_async;
        std::function<void(std::function<void(bool)>)> restore_redo_async;
    };

    using StateChanged = std::function<void(TaskState)>;

    void set_state_changed(StateChanged callback) { m_state_changed = std::move(callback); }

    TaskState state() const { return m_state; }
    const InteractionCapabilities& capabilities() const { return m_capabilities; }
    bool processing() const {
        return m_state == TaskState::Submitting || m_state == TaskState::Accepting ||
            m_state == TaskState::Reoptimizing || m_state == TaskState::CancelRequested ||
            m_state == TaskState::Restoring;
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
        ++m_history_generation;
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
        if (processing() || (!entry.undo && !entry.restore_undo && !entry.restore_undo_async) ||
            (!entry.redo && !entry.restore_redo && !entry.restore_redo_async)) return;
        m_entries.push_back(std::move(entry));
        m_redo.clear();
        if (m_state != TaskState::PreviewReady) m_state = TaskState::Editing;
        recompute_capabilities();
        notify();
    }

    bool undo()
    {
        if (processing() || m_entries.empty() || m_entries.back().restore_undo_async) return false;
        if (m_entries.back().can_undo && !m_entries.back().can_undo()) {
            mark_failed("Beauty history source is missing or changed");
            return false;
        }
        Entry entry = m_entries.back();
        if (entry.restore_undo && !entry.restore_undo()) {
            mark_failed("Beauty history source could not be restored");
            return false;
        }
        if (!entry.restore_undo && entry.undo) entry.undo();
        m_entries.pop_back();
        m_redo.push_back(std::move(entry));
        recompute_capabilities();
        notify();
        return true;
    }

    bool redo()
    {
        if (processing() || m_redo.empty() || m_redo.back().restore_redo_async) return false;
        if (m_redo.back().can_redo && !m_redo.back().can_redo()) {
            mark_failed("Beauty history candidate is missing or changed");
            return false;
        }
        Entry entry = m_redo.back();
        if (entry.restore_redo && !entry.restore_redo()) {
            mark_failed("Beauty history candidate could not be restored");
            return false;
        }
        if (!entry.restore_redo && entry.redo) entry.redo();
        m_redo.pop_back();
        m_entries.push_back(std::move(entry));
        recompute_capabilities();
        notify();
        return true;
    }

    const std::string& last_error() const { return m_last_error; }
    size_t undo_count() const { return m_entries.size(); }
    size_t redo_count() const { return m_redo.size(); }

private:
    void restore_history_async(bool redo, std::function<void(bool)> complete)
    {
        auto& source = redo ? m_redo : m_entries;
        if (processing() || source.empty()) { if (complete) complete(false); return; }
        const auto restore = redo ? source.back().restore_redo_async : source.back().restore_undo_async;
        if (!restore) { const bool ok = redo ? this->redo() : undo(); if (complete) complete(ok); return; }
        const auto generation = ++m_history_generation;
        const auto lifetime = std::weak_ptr<const char>(m_lifetime);
        const auto delivered = std::make_shared<bool>(false);
        m_state = TaskState::Restoring; recompute_capabilities(); notify();
        auto finished = [this, lifetime, generation, delivered, redo, complete = std::move(complete)](bool ok) {
            if (lifetime.expired() || *delivered || generation != m_history_generation) return;
            *delivered = true;
            auto& from = redo ? m_redo : m_entries;
            auto& to = redo ? m_entries : m_redo;
            if (ok && !from.empty()) {
                to.push_back(std::move(from.back())); from.pop_back();
                m_last_error.clear();
                if (m_state != TaskState::PreviewReady) m_state = TaskState::Editing;
                recompute_capabilities(); notify();
            } else { ok = false; mark_failed("Beauty history source could not be restored"); }
            if (complete) complete(ok);
        };
        try { restore(finished); }
        catch (...) { finished(false); }
    }

public:
    void undo_async(std::function<void(bool)> complete) { restore_history_async(false, std::move(complete)); }
    void redo_async(std::function<void(bool)> complete) { restore_history_async(true, std::move(complete)); }
    bool begin_restore() {
        if (processing()) return false;
        m_state = TaskState::Restoring; recompute_capabilities(); notify(); return true;
    }

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
        m_capabilities.can_change_model = !busy && !preview;
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
    std::shared_ptr<const char> m_lifetime {std::make_shared<const char>(0)};
    std::size_t m_history_generation {0};
};

} // namespace Slic3r::GUI
