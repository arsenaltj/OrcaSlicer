#pragma once
#include "BeautyWorkbenchTransactionController.hpp"
#include <boost/filesystem/path.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>

namespace Slic3r::GUI {

// Snapshots and file ownership belong to the session, while an injected view
// restores previews and an injected repository checks/removes owned candidates.
template<class Snapshot>
class BeautyWorkbenchSession final {
public:
    struct Ports {
        std::function<bool(const Snapshot&)> valid;
        std::function<bool(const Snapshot&)> restore;
        std::function<void(const boost::filesystem::path&)> remove_candidate;
        std::function<void(const Snapshot&, std::function<void(bool)>)> restore_async;
    };
    struct Draft {
        std::shared_ptr<Snapshot> reoptimization_before, portrait_before, portrait_preview;
        bool manual_color_dirty {false};
    };
    using Revision = std::uint64_t;

    BeautyWorkbenchTransactionController& transactions() { return m_transactions; }
    Draft& draft() { return m_draft; }
    const std::shared_ptr<Snapshot>& source() const { return m_source; }
    Revision revision() const { return m_revision; }
    bool is_current(Revision revision) const { return revision == m_revision; }

    // Capture the discard/history boundary together. Callers explicitly start
    // a new draft; successive edits keep the original checkpoint.
    void start_draft(std::shared_ptr<Snapshot> source) {
        if (!source) return;
        m_source = std::move(source);
        m_undo_base = m_transactions.undo_count();
        m_file_base = m_candidate_files.size();
        ++m_revision;
    }
    void clear_draft() {
        m_source.reset();
        ++m_revision;
    }
    void reset() {
        clear_draft();
        m_draft = {};
        m_candidate_files.clear();
        m_accepted_files.clear();
        m_undo_base = m_file_base = 0;
        m_transactions.reset();
    }
    void track_candidate(const boost::filesystem::path& path) {
        if (!path.empty()) m_candidate_files.push_back(path);
    }
    void forget_candidate(const boost::filesystem::path& path) {
        m_candidate_files.erase(std::remove(m_candidate_files.begin(), m_candidate_files.end(), path),
            m_candidate_files.end());
    }
    // Also used during shutdown when publication completed before its UI
    // callback. Only session-owned appearance carriers may be retained.
    void preserve_publication(const boost::filesystem::path& candidate,
                              const boost::filesystem::path& carrier = {}) {
        if (!candidate.empty()) m_accepted_files.push_back(candidate);
        if (!carrier.empty() &&
            std::find(m_candidate_files.begin(), m_candidate_files.end(), carrier) != m_candidate_files.end())
            m_accepted_files.push_back(carrier);
    }
    bool complete_accept(Revision revision, const boost::filesystem::path& candidate,
                         const boost::filesystem::path& carrier,
                         BeautyWorkbenchTransactionController::Entry history = {}) {
        if (!is_current(revision) || candidate.empty()) return false;
        preserve_publication(candidate, carrier);
        m_transactions.finish(true);
        if (m_source && (history.undo || history.restore_undo || history.restore_undo_async) &&
            (history.redo || history.restore_redo || history.restore_redo_async)) {
            m_transactions.truncate_to(m_undo_base);
            m_transactions.record(std::move(history));
        }
        clear_draft();
        m_draft.manual_color_dirty = false;
        return true;
    }
    bool discard(const boost::filesystem::path& accepted, const Ports& ports) {
        if (!m_source || m_transactions.processing() || !ports.restore) return false;
        // Failed validation/restore must keep both the draft and its files so
        // a retry cannot lose an undo source or the currently visible model.
        const auto source = m_source;
        if ((ports.valid && !ports.valid(*source)) || !ports.restore(*source)) return false;
        m_transactions.truncate_to(m_undo_base);
        clear_candidates(m_file_base, accepted, ports);
        clear_draft();
        return true;
    }
    void record(BeautyWorkbenchTransactionController::OperationKind kind,
                std::shared_ptr<Snapshot> before, std::shared_ptr<Snapshot> after, Ports ports) {
        if (!before || !after || m_transactions.processing()) return;
        if (ports.restore_async) {
            track_candidate(after->candidate);
            BeautyWorkbenchTransactionController::Entry entry;
            entry.kind = kind; entry.label = "Beauty candidate";
            entry.restore_undo_async = [before, restore = ports.restore_async](auto complete) { restore(*before, std::move(complete)); };
            entry.restore_redo_async = [after, restore = ports.restore_async](auto complete) { restore(*after, std::move(complete)); };
            m_transactions.record(std::move(entry)); return;
        }
        if (!ports.valid || !ports.restore) return;
        track_candidate(after->candidate);
        m_transactions.record({kind, "Beauty candidate", {}, {},
            [before, valid = ports.valid] { return valid(*before); },
            [after, valid = ports.valid] { return valid(*after); },
            [before, restore = ports.restore] { return restore(*before); },
            [after, restore = ports.restore] { return restore(*after); }});
    }
    void clear_candidates(size_t start, const boost::filesystem::path& accepted, const Ports& ports) {
        if (start > m_candidate_files.size()) return;
        for (size_t index = start; index < m_candidate_files.size(); ++index) {
            const auto& path = m_candidate_files[index];
            if (path.empty() || path == accepted ||
                std::find(m_accepted_files.begin(), m_accepted_files.end(), path) != m_accepted_files.end()) continue;
            if (ports.remove_candidate) ports.remove_candidate(path);
        }
        m_candidate_files.resize(start);
    }
    void discard_async(const boost::filesystem::path& accepted, Ports ports, std::function<void(bool)> complete) {
        if (!ports.restore_async) { const bool ok = discard(accepted, ports); if (complete) complete(ok); return; }
        if (!m_source || !m_transactions.begin_restore()) { if (complete) complete(false); return; }
        const auto source = m_source;
        const auto revision = m_revision;
        const auto lifetime = std::weak_ptr<const char>(m_lifetime);
        const auto delivered = std::make_shared<bool>(false);
        auto finished = [this, lifetime, delivered, source, revision, accepted, ports,
                         complete = std::move(complete)](bool ok) {
            if (lifetime.expired() || *delivered || revision != m_revision || source != m_source) return;
            *delivered = true;
            m_transactions.finish(ok);
            if (ok) {
                m_transactions.truncate_to(m_undo_base);
                clear_candidates(m_file_base, accepted, ports); clear_draft();
            }
            if (complete) complete(ok);
        };
        try { ports.restore_async(*source, finished); }
        catch (...) { finished(false); }
    }
private:
    BeautyWorkbenchTransactionController m_transactions;
    Draft m_draft;
    std::shared_ptr<Snapshot> m_source;
    std::vector<boost::filesystem::path> m_candidate_files, m_accepted_files;
    size_t m_undo_base {0}, m_file_base {0};
    Revision m_revision {0};
    std::shared_ptr<const char> m_lifetime {std::make_shared<const char>(0)};
};
}
