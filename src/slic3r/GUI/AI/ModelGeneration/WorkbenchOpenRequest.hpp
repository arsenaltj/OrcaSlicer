#pragma once

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "ModelLibraryEntry.hpp"
#include <atomic>
#include <stdexcept>

namespace Slic3r::GUI {

// An asset handoff never carries a creator's task or cancellation token.
// Existing library metadata remains the persistence format.
struct WorkbenchOpenRequest {
    enum class Origin { Smart, Advanced, Legacy, History, Library };
    AI::GeneratedModelArtifact artifact;
    ModelLibraryEntry entry;
    std::string sha256;
    std::string return_context;
    Origin origin {Origin::History};
    std::uint64_t generation {0};
    bool new_asset {false};

    bool identifies_asset() const {
        return !entry.model_path.empty() && !entry.job_id.empty() &&
            (artifact.local_path.empty() || artifact.local_path == entry.model_path) &&
            (artifact.job_id.empty() || artifact.job_id == entry.job_id);
    }
    bool matches_creator(const std::string& asset_id, std::uint64_t creator_generation) const {
        return identifies_asset() && !asset_id.empty() && entry.job_id == asset_id &&
            generation == creator_generation;
    }
};

// Fail before a second editor or import owner can be installed.
class WorkbenchInstanceLease final {
public:
    WorkbenchInstanceLease() {
        unsigned expected = 0;
        if (!s_live.compare_exchange_strong(expected, 1))
            throw std::logic_error("Only one application workbench may exist");
    }
    ~WorkbenchInstanceLease() { s_live.store(0); }
    WorkbenchInstanceLease(const WorkbenchInstanceLease&) = delete;
    WorkbenchInstanceLease& operator=(const WorkbenchInstanceLease&) = delete;
    static unsigned live_instances() { return s_live.load(); }
private:
    inline static std::atomic<unsigned> s_live {0};
};

} // namespace Slic3r::GUI
