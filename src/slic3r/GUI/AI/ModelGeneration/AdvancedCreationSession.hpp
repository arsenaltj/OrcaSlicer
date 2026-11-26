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
    bool set_design_provider(std::string provider);
    bool set_upload(ViewId view, std::string path, bool explicit_replacement = false);
    std::string upload_change_rejection(ViewId view, const std::string& path, bool explicit_replacement = false) const;
    std::vector<ViewId> all_design_views() const;
    bool has_local_design_target(ViewId view) const;
    std::string single_design_rejection(ViewId view, const std::string& text) const;
    void set_model_options(ModelOptions options);
    bool draft_dirty() const { return m_draft_dirty; }
    void mark_draft_saved(const DesignDraft& saved);
    bool inputs_match_results() const;
    void arm_initial_design_guidance(const CallbackContext& context,
                                    std::vector<ViewId> targets = {}, std::string input_epoch = {});
    // targets is the accepted plan's full frozen design_targets, not operation.targets.
    void track_initial_design_plan(const CallbackContext& context, std::vector<ViewId> targets,
                                   std::string input_epoch);
    bool consume_initial_design_guidance();
    void dismiss_initial_design_guidance() { m_initial_design_guidance = InitialDesignGuidance::Consumed; }

    CallbackContext begin_open(std::string group_id);
    CallbackContext new_creation();
    CallbackContext context() const { return {m_epoch, m_opening_id}; }
    bool matches(const CallbackContext& context) const;
    bool apply_snapshot(GroupSnapshot snapshot, const CallbackContext& context, bool restore_draft = false,
                        const InputDraft* saved_input = nullptr);
    bool apply_model_job(const ModelJobProjection& job, const CallbackContext& context);
    CommandContext command_context() const;
    bool matches_command(const CommandContext& context, bool uses_selection = true) const;
    bool matches_model_command(const CommandContext& context) const;

    bool toggle(ViewId view);
    std::vector<ViewId> effective_views() const;
    std::array<bool, 4> model_selection() const;
    bool can_toggle_model_view(ViewId view) const;
    bool toggle_model_view(ViewId view);
    std::vector<ViewId> effective_model_views() const;
    const std::optional<ModelSelectionCandidate>& model_candidate() const { return m_model_candidate; }
    std::uint64_t begin_model_selection();
    bool toggle_candidate_view(ViewId view);
    std::vector<ViewId> candidate_model_views() const;
    Eligibility candidate_model_eligibility(const Capabilities& capabilities, const HostGuards& guards) const;
    Eligibility model_selection_entry_eligibility(const Capabilities& capabilities, const HostGuards& guards) const;
    std::optional<LocalDraft> candidate_local_draft() const;
    bool commit_model_selection(std::uint64_t id, std::uint64_t revision);
    void cancel_model_selection() { m_model_candidate.reset(); }
    void click(ViewId view, std::uint64_t now_ms, std::uint64_t double_click_ms,
               SelectionContext selection_context = SelectionContext::Design);
    bool flush_click(std::uint64_t now_ms);
    std::optional<ViewId> double_click(ViewId view);
    void cancel_pending_click() { m_pending_click.reset(); }
    Eligibility eligibility(const Capabilities& capabilities, const HostGuards& guards) const;
    std::string input_rejection() const;
    LocalDraft local_draft() const;
    void restore_local_draft(const LocalDraft& draft);

private:
    enum class InitialDesignGuidance { Unarmed, Pending, Consumed };
    InitialDesignGuidance m_initial_design_guidance {InitialDesignGuidance::Unarmed};
    std::vector<ViewId> m_guidance_targets;
    std::string m_guidance_input_epoch;
    bool toggle_selection(ViewId view, SelectionContext context);
    bool model_view_usable(ViewId view) const;
    Eligibility eligibility_for(const Capabilities& capabilities, const HostGuards& guards,
                                const std::array<bool, 4>& selected, bool selection_entry = false) const;
    struct PendingClick { ViewId view; std::uint64_t due; SelectionContext context; };
    InputDraft m_draft;
    std::optional<GroupSnapshot> m_group;
    std::optional<ModelJobProjection> m_accepted_model;
    std::array<bool, 4> m_selected {};
    std::array<bool, 4> m_model_selected {};
    std::array<bool, 4> m_model_seen {};
    std::array<bool, 4> m_invalidated_uploads {};
    std::array<std::string, 4> m_invalidated_versions;
    std::optional<PendingClick> m_pending_click;
    std::optional<ModelSelectionCandidate> m_model_candidate;
    std::uint64_t m_candidate_sequence {0};
    std::string m_opening_id;
    std::uint64_t m_epoch {1};
    std::uint64_t m_draft_revision {0};
    std::uint64_t m_selection_revision {0};
    std::uint64_t m_model_selection_revision {0};
    std::uint64_t m_model_options_revision {0};
    bool m_sidebar_expanded {true};
    bool m_draft_dirty {false};
};

} // namespace Slic3r::GUI::AdvancedCreation
