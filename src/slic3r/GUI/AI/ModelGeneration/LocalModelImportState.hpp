#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <wx/clntdata.h>
#include <wx/window.h>

namespace Slic3r::GUI {
// The persistent import action owns this local operation. Workers capture only
// its cancellation token, never the control or a second generation-task owner.
struct LocalModelImportState final : wxClientData {
    std::shared_ptr<std::atomic<bool>> cancel;
    bool open_beauty = false;
    bool parsing = false;
    // UI may roll back after joining a completed worker during shutdown.
    std::function<void()> rollback_after_join;
    void complete(const std::shared_ptr<std::atomic<bool>>& expected, bool rollback) {
        if(cancel!=expected) return;
        if(rollback && rollback_after_join) rollback_after_join();
        cancel.reset();
        parsing=false;
        rollback_after_join={};
    }
};
inline LocalModelImportState* local_model_import_state(wxWindow* owner)
{
    return owner ? dynamic_cast<LocalModelImportState*>(owner->GetClientObject()) : nullptr;
}
// The caller has joined the archive/preview workers before rollback.
inline void complete_local_model_import(wxWindow* owner,
    const std::shared_ptr<std::atomic<bool>>& expected, bool rollback)
{
    auto* state = local_model_import_state(owner);
    if(state)state->complete(expected,rollback);
}
} // namespace Slic3r::GUI
