#pragma once

#include "AdvancedCreationTypes.hpp"
#include <utility>

namespace Slic3r::GUI::AdvancedCreation {

// Advanced state has a single desktop owner. Widgets only render this projection
// and dispatch user intent; the service remains the owner of durable versions.
class Session {
public:
    const InputDraft& draft() const { return m_draft; }
    const std::optional<GroupSnapshot>& group() const { return m_group; }
    const std::array<bool, 4>& selection() const { return m_selected; }
    bool sidebar_expanded() const { return m_sidebar_expanded; }
    void set_sidebar_expanded(bool expanded) { m_sidebar_expanded = expanded; }

    bool set_design_draft(DesignDraft draft);
    bool set_upload(ViewId view, std::string path);
    void set_model_options(ModelOptions options) { m_draft.geometry = std::move(options); ++m_draft_revision; }
    bool draft_dirty() const { return m_draft_dirty; }
    void mark_draft_saved(const DesignDraft& saved);
    bool inputs_match_results() const;

    CallbackContext begin_open(std::string group_id);
    CallbackContext new_creation();
    CallbackContext context() const { return {m_epoch, m_opening_id}; }
    bool matches(const CallbackContext& context) const;
    bool apply_snapshot(GroupSnapshot snapshot, const CallbackContext& context, bool restore_draft = false,
                        const InputDraft* saved_input = nullptr);
    bool apply_model_job(const ModelJobProjection& job, const CallbackContext& context);
    CommandContext command_context() const;
    bool matches_command(const CommandContext& context, bool uses_selection = true) const;

    bool toggle(ViewId view);
    std::vector<ViewId> effective_views() const;
    void click(ViewId view, std::uint64_t now_ms, std::uint64_t double_click_ms);
    bool flush_click(std::uint64_t now_ms);
    std::optional<ViewId> double_click(ViewId view);
    void cancel_pending_click() { m_pending_click.reset(); }
    Eligibility eligibility(const Capabilities& capabilities, const HostGuards& guards) const;

private:
    struct PendingClick { ViewId view; std::uint64_t due; };
    InputDraft m_draft;
    std::optional<GroupSnapshot> m_group;
    std::optional<ModelJobProjection> m_accepted_model;
    std::array<bool, 4> m_selected {};
    std::optional<PendingClick> m_pending_click;
    std::string m_opening_id;
    std::uint64_t m_epoch {1};
    std::uint64_t m_draft_revision {0};
    std::uint64_t m_selection_revision {0};
    bool m_sidebar_expanded {true};
    bool m_draft_dirty {false};
};

} // namespace Slic3r::GUI::AdvancedCreation
