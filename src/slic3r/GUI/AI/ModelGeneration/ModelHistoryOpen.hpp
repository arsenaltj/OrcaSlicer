#pragma once
#include "ModelLibraryEntry.hpp"
#include "PostGenerationWorkbenchState.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace Slic3r::GUI {

struct ModelHistoryOpenEvent {
    enum class Phase { Begin, Progress, Loaded, Failed };
    enum class Stage { Reading, Validating, Preview, Displaying };
    uint64_t request {0};
    std::string asset;
    Phase phase {Phase::Begin};
    Stage stage {Stage::Reading};
    std::string resolved_asset;

    bool matches(uint64_t generation, const std::string& selected_asset) const {
        return request != 0 && request == generation && asset == selected_asset;
    }
};
using ModelHistoryOpenHandler = std::function<bool(const ModelHistoryOpenEvent&)>;

// Owned by the existing preview-load request, never persisted. A worker writes
// the resolved entry; only its generation-checked GUI callback can install it.
struct ModelHistoryOpenRequest {
    uint64_t generation {0};
    std::string selected_asset; // Immutable while the worker resolves a repaired version.
    ModelLibraryEntry entry, previous;
    boost::filesystem::path root;
    std::string model_hash, record_hash;
    std::shared_ptr<const ModelHistoryRepairIndex> repairs;
    std::optional<WorkbenchCheckResult> check, previous_check;
    std::atomic<bool> canceled {false};
    bool installed {false}, restoring {false};
    bool worker_pending {false}; // GUI-owned, includes the deferred worker start.
    std::function<void(ModelHistoryOpenEvent::Stage)> progress;
    std::function<void()> drained;
    std::function<void()> restored, restore_presentation;

    // Cancellation keeps the request/busy gate until its parser has returned.
    // Both methods run on the GUI thread; canceled is the worker's stop token.
    bool cancel_after_drain(std::function<void()> resume) {
        canceled = true;
        if (!worker_pending) return false;
        if (!drained) drained = std::move(resume);
        return true;
    }
    bool worker_finished() {
        worker_pending = false;
        auto resume = std::move(drained);
        drained = {};
        if (resume) { resume(); return true; }
        return false;
    }
};

} // namespace Slic3r::GUI
