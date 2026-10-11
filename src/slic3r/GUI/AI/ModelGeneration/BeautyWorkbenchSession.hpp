#pragma once
#include "BeautyWorkbenchTransactionController.hpp"
#include <boost/filesystem/path.hpp>
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
    };
    struct Draft {
        std::vector<boost::filesystem::path> candidate_files, accepted_files;
        std::shared_ptr<Snapshot> source, reoptimization_before, portrait_before, portrait_preview;
        size_t undo_base {0}, file_base {0};
        bool manual_color_dirty {false};
    };
    BeautyWorkbenchTransactionController& transactions() { return m_transactions; }
    Draft& draft() { return m_draft; }
    void record(BeautyWorkbenchTransactionController::OperationKind kind,
                std::shared_ptr<Snapshot> before, std::shared_ptr<Snapshot> after, Ports ports) {
        if (!before || !after || !ports.valid || !ports.restore || m_transactions.processing()) return;
        m_draft.candidate_files.push_back(after->candidate);
        m_transactions.record({kind, "Beauty candidate", {}, {},
            [before, valid = ports.valid] { return valid(*before); },
            [after, valid = ports.valid] { return valid(*after); },
            [before, restore = ports.restore] { return restore(*before); },
            [after, restore = ports.restore] { return restore(*after); }});
    }
    void clear_candidates(size_t start, const boost::filesystem::path& accepted, const Ports& ports) {
        if (start > m_draft.candidate_files.size()) return;
        for (size_t index = start; index < m_draft.candidate_files.size(); ++index) {
            const auto& path = m_draft.candidate_files[index];
            if (path.empty() || path == accepted ||
                std::find(m_draft.accepted_files.begin(), m_draft.accepted_files.end(), path) != m_draft.accepted_files.end()) continue;
            if (ports.remove_candidate) ports.remove_candidate(path);
        }
        m_draft.candidate_files.resize(start);
    }
private:
    BeautyWorkbenchTransactionController m_transactions;
    Draft m_draft;
};
}
