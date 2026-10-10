#pragma once

namespace Slic3r::GUI {
struct PostGenerationUiState
{
    enum class Mode { Result, Workbench };
    enum class Status { Empty, Loading, Ready, Editing, Processing, CandidateReady, ComparingBefore, Error };

    Mode mode {Mode::Result};
    Status status {Status::Empty};
    bool can_edit {false};
    bool can_switch_version {false};
    bool can_preview {false};
    bool can_accept {false};
    bool can_discard {false};
    bool can_undo {false};
    bool can_redo {false};
    bool can_import {false};
};

inline bool post_generation_asset_switch_allowed(bool busy, bool finishing_running,
    bool transaction_processing, bool candidate_ready, bool comparing_before, bool has_changes)
{
    return !busy && !finishing_running && !transaction_processing && !candidate_ready &&
        !comparing_before && !has_changes;
}

inline bool post_generation_return_to_design_allowed(const PostGenerationUiState& state)
{
    return state.status != PostGenerationUiState::Status::Loading &&
           state.status != PostGenerationUiState::Status::Processing;
}

// Keep workbench state derivation independent from wxWidgets and file
// lifetimes so the same rules can be checked by unit tests.
inline PostGenerationUiState derive_post_generation_ui_state(
    PostGenerationUiState::Mode mode,
    bool model_preview_ready,
    bool has_displayed_model,
    bool busy,
    bool finishing_running,
    bool transaction_processing,
    bool candidate_ready,
    bool comparing_before,
    bool has_changes,
    bool undo_available,
    bool redo_available,
    bool load_error = false)
{
    PostGenerationUiState state;
    state.mode = mode;
    const bool ready = model_preview_ready && has_displayed_model;
    const bool processing = busy || finishing_running || transaction_processing;
    if (load_error) {
        state.status = PostGenerationUiState::Status::Error;
    } else if (!ready) {
        state.status = processing ? PostGenerationUiState::Status::Loading : PostGenerationUiState::Status::Empty;
    } else if (processing) {
        state.status = PostGenerationUiState::Status::Processing;
    } else if (comparing_before) {
        state.status = PostGenerationUiState::Status::ComparingBefore;
    } else if (candidate_ready) {
        state.status = PostGenerationUiState::Status::CandidateReady;
    } else if (mode == PostGenerationUiState::Mode::Workbench && has_changes) {
        state.status = PostGenerationUiState::Status::Editing;
    } else {
        state.status = PostGenerationUiState::Status::Ready;
    }
    state.can_edit = ready && !processing && !comparing_before;
    state.can_switch_version = post_generation_asset_switch_allowed(busy, finishing_running,
        transaction_processing, candidate_ready, comparing_before, has_changes);
    state.can_preview = state.can_edit;
    state.can_accept = ready && !processing && candidate_ready && !comparing_before;
    state.can_discard = state.can_accept;
    state.can_undo = ready && !processing && !comparing_before && undo_available;
    state.can_redo = ready && !processing && !comparing_before && redo_available;
    state.can_import = ready && !processing && !candidate_ready && !comparing_before && !has_changes;
    return state;
}

}
