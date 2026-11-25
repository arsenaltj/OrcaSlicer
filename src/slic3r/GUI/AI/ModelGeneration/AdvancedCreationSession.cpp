#include "AdvancedCreationSession.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r::GUI::AdvancedCreation {

const char* view_name(ViewId view)
{
    static constexpr std::array<const char*, 4> names {"front", "left", "back", "right"};
    return names.at(index(view));
}

std::optional<ViewId> parse_view(const std::string& name)
{
    for (ViewId view : AllViews) if (name == view_name(view)) return view;
    return std::nullopt;
}

bool DesignDraft::operator==(const DesignDraft& other) const
{
    return prompt == other.prompt && style == other.style && custom_style == other.custom_style &&
           provider == other.provider && model == other.model;
}

GridLayout grid_layout(int width, int height, int margin, int gap, int caption_height)
{
    width = std::max(0, width); height = std::max(0, height);
    margin = std::max(0, std::min({margin, width / 4, height / 4}));
    gap = std::max(0, std::min({gap, width / 4, height / 4}));
    const int card_width = std::max(0, (width - margin * 2 - gap) / 2);
    const int card_height = std::max(0, (height - margin * 2 - gap) / 2);
    const int caption = std::clamp(caption_height, 0, card_height);
    GridLayout result;
    for (std::size_t i = 0; i < 4; ++i) {
        auto& card = result.cards[i];
        card = {margin + static_cast<int>(i % 2) * (card_width + gap),
                margin + static_cast<int>(i / 2) * (card_height + gap), card_width, card_height};
        result.images[i] = {card.x, card.y, card.width, card.height - caption};
    }
    return result;
}

LayoutRect contain_image(LayoutRect available, int image_width, int image_height)
{
    if (image_width <= 0 || image_height <= 0 || available.width <= 0 || available.height <= 0)
        return {available.x, available.y, 0, 0};
    const double scale = std::min(static_cast<double>(available.width) / image_width,
                                  static_cast<double>(available.height) / image_height);
    const int width = std::min(available.width, static_cast<int>(std::round(image_width * scale)));
    const int height = std::min(available.height, static_cast<int>(std::round(image_height * scale)));
    return {available.x + (available.width - width) / 2, available.y + (available.height - height) / 2, width, height};
}

bool design_attempt_generating(const std::string& state)
{
    return state == "queued" || state == "submitting";
}

bool design_attempt_failed(const std::string& state)
{
    return state == "failed" || state == "blocked" || state == "stopped" || state == "ambiguous" ||
           state == "result_pending" || state == "result_saved";
}

bool design_attempt_recoverable(const std::string& state)
{
    return state == "result_pending" || state == "result_saved";
}

bool Session::set_design_draft(DesignDraft draft)
{
    if (m_draft.design == draft) return false;
    const bool style_changed = draft.style != m_draft.design.style ||
                               draft.custom_style != m_draft.design.custom_style;
    m_draft.design = std::move(draft);
    if (style_changed && m_group) {
        for (ViewId view : AllViews)
            if (m_group->views[index(view)].usable) m_selected[index(view)] = true;
        ++m_selection_revision;
    }
    m_draft_dirty = true;
    ++m_draft_revision;
    return true;
}

bool Session::set_design_provider(std::string provider)
{
    if (provider == m_draft.design.provider) return false;
    auto draft = m_draft.design;
    draft.provider = std::move(provider);
    draft.model.clear();
    // The submitted draft owns the model that produced the current four views.
    // Returning to that provider must also restore its otherwise hidden model.
    if (m_group && m_group->latest_submitted_draft &&
        m_group->latest_submitted_draft->provider == draft.provider)
        draft.model = m_group->latest_submitted_draft->model;
    return set_design_draft(std::move(draft));
}

bool Session::set_upload(ViewId view, std::string path, bool explicit_replacement)
{
    if (!upload_change_rejection(view, path, explicit_replacement).empty()) return false;
    const auto slot = index(view);
    if (m_draft.uploads.at(slot) == path && !explicit_replacement) return false;
    m_draft.uploads[slot] = std::move(path);
    m_draft.replaced_uploads[slot] = explicit_replacement;
    ++m_draft.upload_revisions[slot];
    m_invalidated_uploads[slot] = true;
    if (m_group) {
        auto& design = m_group->views[slot];
        if (!design.canonical_version_id.empty()) m_invalidated_versions[slot] = design.canonical_version_id;
        design = {};
        m_group->pending_views[slot] = !m_draft.uploads[slot].empty();
    }
    if (!m_selected[slot] && !m_draft.uploads[slot].empty()) {
        m_selected[slot] = true;
        ++m_selection_revision;
    }
    m_draft_dirty = true;
    ++m_draft_revision;
    return true;
}

std::string Session::upload_change_rejection(ViewId view, const std::string& path, bool explicit_replacement) const
{
    if (view != ViewId::Front && !path.empty() && m_draft.uploads[0].empty() &&
        (explicit_replacement || path != m_draft.uploads[index(view)])) return "missing_front";
    return {};
}

std::vector<ViewId> Session::all_design_views() const
{
    const bool images = std::any_of(m_draft.uploads.begin(), m_draft.uploads.end(),
        [](const auto& path) { return !path.empty(); });
    std::vector<ViewId> result;
    if (!images && m_draft.design.prompt.find_first_not_of(" \r\n\t") == std::string::npos) return result;
    for (ViewId view : AllViews)
        if (!images || !m_draft.uploads[index(view)].empty()) result.push_back(view);
    return result;
}

bool Session::has_local_design_target(ViewId view) const
{
    const auto slot = index(view);
    if (!m_group || !m_group->published || m_invalidated_uploads[slot]) return false;
    const auto& image = m_group->views[slot];
    return image.usable || design_attempt_failed(image.attempt_state) || image.display_error;
}

std::string Session::single_design_rejection(ViewId view, const std::string& text) const
{
    if (!has_local_design_target(view)) return "missing_current_view";
    if (text.size() > 2000) return "invalid_input";
    const auto input = input_rejection();
    if (!input.empty() && input != "empty_input") return input;
    for (const auto& design : m_group->views)
        if (design_attempt_recoverable(design.attempt_state)) return "recovery_required";

    // With no uploads or global description, the visible edit base, a main
    // design reference for an auxiliary retry, or the local text can suffice.
    const auto& front = m_group->views[index(ViewId::Front)];
    const bool main_reference = view != ViewId::Front && !m_invalidated_uploads[index(ViewId::Front)] &&
        !front.canonical_version_id.empty();
    if (input == "empty_input" && !m_group->views[index(view)].usable && !main_reference &&
        text.find_first_not_of(" \r\n\t") == std::string::npos) return "empty_input";
    return {};
}

void Session::mark_draft_saved(const DesignDraft& saved)
{
    if (saved == m_draft.design && (!m_group || m_draft.uploads == m_group->uploads) &&
        std::none_of(m_invalidated_uploads.begin(), m_invalidated_uploads.end(), [](bool flag) { return flag; }))
        m_draft_dirty = false;
}

bool Session::inputs_match_results() const
{
    return m_group && m_draft.uploads == m_group->uploads &&
           std::none_of(m_group->pending_views.begin(), m_group->pending_views.end(),
                        [](bool pending) { return pending; }) &&
           m_group->latest_submitted_draft &&
           m_draft.design == *m_group->latest_submitted_draft;
}

CallbackContext Session::begin_open(std::string group_id)
{
    ++m_epoch;
    m_opening_id = std::move(group_id);
    m_pending_click.reset();
    m_model_candidate.reset();
    m_accepted_model.reset();
    m_initial_design_guidance = InitialDesignGuidance::Unarmed;
    m_guidance_targets.clear(); m_guidance_input_epoch.clear();
    m_model_selected.fill(false); m_model_seen.fill(false);
    m_invalidated_uploads.fill(false); m_invalidated_versions.fill({});
    return context();
}

void Session::arm_initial_design_guidance(const CallbackContext& callback, std::vector<ViewId> targets,
                                         std::string input_epoch)
{
    if (matches(callback) && !callback.group_id.empty() &&
        m_initial_design_guidance == InitialDesignGuidance::Unarmed) {
        m_initial_design_guidance = InitialDesignGuidance::Pending;
        m_guidance_targets = std::move(targets);
        m_guidance_input_epoch = std::move(input_epoch);
    }
}

void Session::track_initial_design_plan(const CallbackContext& callback, std::vector<ViewId> targets,
                                       std::string input_epoch)
{
    if (matches(callback) && m_initial_design_guidance == InitialDesignGuidance::Pending) {
        m_guidance_targets = std::move(targets);
        m_guidance_input_epoch = std::move(input_epoch);
    }
}

bool Session::consume_initial_design_guidance()
{
    if (m_initial_design_guidance != InitialDesignGuidance::Pending || !m_group ||
        m_group->id != m_opening_id || !m_group->active_operation_id.empty() ||
        m_group->stop_intent || !inputs_match_results() ||
        (!m_guidance_input_epoch.empty() && m_guidance_input_epoch != m_group->input_epoch))
        return false;
    bool has_target = false;
    for (ViewId direction : AllViews) {
        // A local repaint may target only a successful view. The creation's
        // other current targets must still be usable before the first guide.
        if (!m_group->design_targets[index(direction)] &&
            std::find(m_guidance_targets.begin(), m_guidance_targets.end(), direction) == m_guidance_targets.end()) continue;
        has_target = true;
        const auto& view = m_group->views[index(direction)];
        if (!view.usable || view.asset_pending || view.display_error ||
            view.version_id.empty() || view.image_path.empty() ||
            view.version_id != view.canonical_version_id ||
            design_attempt_generating(view.attempt_state) || design_attempt_failed(view.attempt_state))
            return false;
    }
    if (!has_target) return false;
    m_initial_design_guidance = InitialDesignGuidance::Consumed;
    return true;
}

CallbackContext Session::new_creation()
{
    begin_open({});
    m_draft = {};
    m_group.reset();
    m_selected.fill(false);
    m_draft_dirty = false;
    ++m_draft_revision;
    ++m_selection_revision;
    ++m_model_selection_revision;
    return context();
}

bool Session::matches(const CallbackContext& context) const
{
    return context.epoch == m_epoch && context.group_id == m_opening_id;
}

bool Session::apply_snapshot(GroupSnapshot snapshot, const CallbackContext& callback, bool restore_draft,
                             const InputDraft* saved_input)
{
    if (!matches(callback) || snapshot.id != m_opening_id || snapshot.input_epoch.empty()) return false;
    if (m_group && m_group->id == snapshot.id && snapshot.revision < m_group->revision) return false;
    const bool inputs_changed = m_group && m_group->id == snapshot.id &&
                                m_group->input_epoch != snapshot.input_epoch;
    for (ViewId view : AllViews) {
        auto& incoming = snapshot.views[index(view)];
        if (incoming.canonical_version_id.empty()) incoming.canonical_version_id = incoming.version_id;
        const auto slot = index(view);
        const bool acknowledged = saved_input && !restore_draft &&
            m_draft.uploads[slot] == saved_input->uploads[slot] &&
            m_draft.upload_revisions[slot] == saved_input->upload_revisions[slot];
        if (m_invalidated_uploads[slot] && acknowledged &&
            (incoming.canonical_version_id.empty() || incoming.canonical_version_id != m_invalidated_versions[slot])) {
            m_invalidated_uploads[slot] = false;
            m_draft.replaced_uploads[slot] = false;
        }
        if (m_invalidated_uploads[slot]) incoming = {};
        if (!incoming.usable && !incoming.canonical_version_id.empty() &&
            !m_invalidated_uploads[slot] && m_group && m_group->id == snapshot.id) {
            const auto& visible = m_group->views[index(view)];
            if (visible.usable) {
                const bool same_verified_asset = !incoming.display_error &&
                    incoming.canonical_version_id == visible.version_id && incoming.sha256 == visible.sha256;
                incoming.version_id = visible.version_id;
                incoming.sha256 = visible.sha256;
                incoming.image_path = visible.image_path;
                incoming.usable = true;
                if (same_verified_asset) incoming.asset_pending = false;
            }
        }
        if (!incoming.usable) { incoming.version_id.clear(); incoming.image_path.clear(); }
    }
    if (restore_draft || (inputs_changed && (saved_input || !m_draft_dirty))) {
        // A save acknowledges its captured input, not edits made while its
        // request/cache verification was running. Still adopt canonical upload
        // paths for slots that have not changed since that request.
        auto uploads = snapshot.uploads;
        auto design = snapshot.saved_draft;
        if (saved_input && !restore_draft) {
            if (m_draft.design != saved_input->design) design = m_draft.design;
            for (ViewId view : AllViews)
                if (m_draft.uploads[index(view)] != saved_input->uploads[index(view)] ||
                    m_draft.upload_revisions[index(view)] != saved_input->upload_revisions[index(view)])
                    uploads[index(view)] = m_draft.uploads[index(view)];
        }
        m_draft.uploads = std::move(uploads);
        m_draft.design = std::move(design);
        if (restore_draft) {
            m_selected.fill(false);
            m_draft.replaced_uploads.fill(false);
            m_draft.upload_revisions.fill(0);
        }
        for (ViewId view : AllViews)
            if (snapshot.pending_views[index(view)])
                m_selected[index(view)] = true;
        m_draft_dirty = m_draft.design != snapshot.saved_draft || m_draft.uploads != snapshot.uploads ||
            std::any_of(m_invalidated_uploads.begin(), m_invalidated_uploads.end(), [](bool flag) { return flag; });
        m_pending_click.reset();
        ++m_draft_revision;
        ++m_selection_revision;
    }
    // A group/assets GET may have started before the model POST was accepted.
    // Its old task must not undo the already verified acceptance projection.
    if (!m_accepted_model && m_group && snapshot.model_job_id == m_group->model_job_id &&
        snapshot.model_updated_at < m_group->model_updated_at) {
        ModelJobProjection current;
        current.id = m_group->model_job_id; current.state = m_group->model_state;
        current.phase = m_group->model_phase; current.message = m_group->model_message;
        current.updated_at = m_group->model_updated_at; current.error_code = m_group->model_error_code;
        current.error_ambiguous = m_group->model_error_ambiguous; current.resumable = m_group->model_resumable;
        m_accepted_model = std::move(current);
    }
    if (m_accepted_model && snapshot.model_job_id == m_accepted_model->id &&
        snapshot.model_updated_at >= m_accepted_model->updated_at)
        m_accepted_model.reset();
    m_group = std::move(snapshot);
    for (ViewId view : AllViews) {
        const auto slot = index(view);
        if (!m_model_seen[slot] && model_view_usable(view)) {
            m_model_seen[slot] = m_model_selected[slot] = true;
            ++m_model_selection_revision;
        }
    }
    if (m_accepted_model) apply_model_job(*m_accepted_model, callback);
    return true;
}

bool Session::apply_model_job(const ModelJobProjection& job, const CallbackContext& callback)
{
    if (!matches(callback) || !m_group || m_group->id != callback.group_id || job.id.empty() || job.state.empty())
        return false;
    if (m_group->model_job_id == job.id && job.updated_at < m_group->model_updated_at)
        return true; // Identity accepted; a newer projection is already installed.
    // Copy first: the caller may be reapplying the retained projection above.
    m_accepted_model = job;
    m_group->model_job_id = job.id;
    m_group->model_state = job.state;
    m_group->model_phase = job.phase;
    m_group->model_updated_at = job.updated_at;
    m_group->model_message = job.message;
    m_group->model_error_code = job.error_code;
    m_group->model_error_ambiguous = job.error_ambiguous;
    m_group->model_resumable = job.resumable;
    m_group->model_busy = job.state == "queued" || job.state == "running" || job.state == "stopping";
    return true;
}

CommandContext Session::command_context() const
{
    return {context(), m_draft_revision, m_selection_revision, m_group ? m_group->revision : 0,
            m_model_selection_revision};
}

bool Session::matches_model_command(const CommandContext& command) const
{
    return matches_command(command, false) && command.model_selection_revision == m_model_selection_revision;
}

bool Session::matches_command(const CommandContext& command, bool uses_selection) const
{
    return matches(command.callback) && command.draft_revision == m_draft_revision &&
           (!uses_selection || command.selection_revision == m_selection_revision) &&
           command.group_revision == (m_group ? m_group->revision : 0);
}

bool Session::toggle(ViewId view)
{
    if (!m_group) return false;
    const auto& design = m_group->views.at(index(view));
    if (!design.usable && !design_attempt_failed(design.attempt_state) && !design.display_error) return false;
    if (m_group->pending_views[index(view)] && m_selected[index(view)]) return false;
    m_selected[index(view)] = !m_selected[index(view)];
    ++m_selection_revision;
    return true;
}

std::vector<ViewId> Session::effective_views() const
{
    std::vector<ViewId> result;
    const bool images = std::any_of(m_draft.uploads.begin(), m_draft.uploads.end(), [](const auto& path) { return !path.empty(); });
    for (ViewId view : AllViews)
        if (m_selected[index(view)] && (!images || !m_draft.uploads[index(view)].empty())) result.push_back(view);
    if (result.empty()) for (ViewId view : AllViews)
        if (!images || !m_draft.uploads[index(view)].empty()) result.push_back(view);
    return result;
}

bool Session::model_view_usable(ViewId view) const
{
    if (!m_group || m_invalidated_uploads[index(view)]) return false;
    const auto& image = m_group->views[index(view)];
    return image.usable && !image.asset_pending && !image.display_error &&
        !m_group->pending_views[index(view)] && !image.version_id.empty() && !image.image_path.empty() &&
        image.version_id == image.canonical_version_id;
}

std::array<bool, 4> Session::model_selection() const
{
    std::array<bool, 4> result {};
    if (m_group) for (ViewId view : AllViews) {
        const auto slot = index(view);
        result[slot] = !m_invalidated_uploads[slot] &&
            !m_group->views[slot].canonical_version_id.empty() && (slot == 0 || m_model_selected[slot]);
    }
    return result;
}

bool Session::can_toggle_model_view(ViewId view) const
{
    // A previously selected auxiliary may be removed after it becomes unavailable.
    return view != ViewId::Front && (model_selection()[index(view)] || model_view_usable(view));
}

bool Session::toggle_model_view(ViewId view)
{
    if (!can_toggle_model_view(view)) return false;
    const auto slot = index(view);
    m_model_selected[slot] = !m_model_selected[slot];
    ++m_model_selection_revision;
    return true;
}

std::vector<ViewId> Session::effective_model_views() const
{
    const auto selected = model_selection();
    std::vector<ViewId> result;
    for (ViewId view : AllViews) if (selected[index(view)]) result.push_back(view);
    return result;
}

std::uint64_t Session::begin_model_selection()
{
    m_pending_click.reset();
    m_model_candidate = ModelSelectionCandidate {++m_candidate_sequence, 0, command_context(), m_model_selected};
    m_model_candidate->selected[0] = true;
    return m_model_candidate->id;
}

bool Session::toggle_candidate_view(ViewId view)
{
    if (!m_model_candidate || !matches_model_command(m_model_candidate->command) || view == ViewId::Front) return false;
    auto& candidate = *m_model_candidate;
    const auto slot = index(view);
    if (!candidate.selected[slot] && !model_view_usable(view)) return false;
    candidate.selected[slot] = !candidate.selected[slot];
    ++candidate.revision;
    return true;
}

std::vector<ViewId> Session::candidate_model_views() const
{
    std::vector<ViewId> result;
    if (m_model_candidate) for (ViewId view : AllViews)
        if (m_model_candidate->selected[index(view)]) result.push_back(view);
    return result;
}

Eligibility Session::candidate_model_eligibility(const Capabilities& capabilities, const HostGuards& guards) const
{
    if (m_model_candidate && matches_model_command(m_model_candidate->command))
        return eligibility_for(capabilities, guards, m_model_candidate->selected);
    Eligibility result;
    result.model_reason = "draft_changed";
    return result;
}

Eligibility Session::model_selection_entry_eligibility(const Capabilities& capabilities, const HostGuards& guards) const
{
    // An unavailable auxiliary may be removed inside the confirmation. All
    // creation-wide guards and the mandatory main design still apply here.
    return eligibility_for(capabilities, guards, {true, false, false, false}, true);
}

std::optional<LocalDraft> Session::candidate_local_draft() const
{
    if (!m_model_candidate || !matches_model_command(m_model_candidate->command)) return std::nullopt;
    auto local = local_draft();
    local.model_selected = m_model_candidate->selected;
    return local;
}

bool Session::commit_model_selection(std::uint64_t id, std::uint64_t revision)
{
    if (!m_model_candidate || m_model_candidate->id != id || m_model_candidate->revision != revision ||
        !matches_model_command(m_model_candidate->command)) return false;
    m_model_selected = m_model_candidate->selected;
    ++m_model_selection_revision;
    m_model_candidate.reset();
    return true;
}

bool Session::toggle_selection(ViewId view, SelectionContext selection_context)
{
    const auto slot = index(view);
    if (selection_context == SelectionContext::Model) return toggle_model_view(view);
    if (selection_context == SelectionContext::DesignForNextModel && view != ViewId::Front &&
        !model_view_usable(view)) {
        if (!m_group || !m_selected[slot] || m_invalidated_uploads[slot] ||
            m_group->views[slot].canonical_version_id.empty()) return false;
        // Keep pending regeneration facts intact. Only this explicit next-model
        // selection may remove a failed pending auxiliary from the 2D cards.
        m_selected[slot] = false;
        ++m_selection_revision;
    } else if (!toggle(view)) return false;
    if (selection_context == SelectionContext::DesignForNextModel) {
        auto next = m_model_selected;
        for (ViewId direction : AllViews)
            next[index(direction)] = direction == ViewId::Front || m_selected[index(direction)];
        if (next != m_model_selected) {
            m_model_selected = next;
            ++m_model_selection_revision;
        }
    }
    return true;
}

void Session::click(ViewId view, std::uint64_t now_ms, std::uint64_t double_click_ms,
                    SelectionContext selection_context)
{
    flush_click(now_ms);
    if (m_pending_click) toggle_selection(m_pending_click->view, m_pending_click->context);
    m_pending_click = PendingClick {view, now_ms + double_click_ms, selection_context};
}

bool Session::flush_click(std::uint64_t now_ms)
{
    if (!m_pending_click || now_ms < m_pending_click->due) return false;
    const auto click = *m_pending_click;
    m_pending_click.reset();
    return toggle_selection(click.view, click.context);
}

std::optional<ViewId> Session::double_click(ViewId view)
{
    if (m_pending_click && m_pending_click->view != view)
        toggle_selection(m_pending_click->view, m_pending_click->context);
    m_pending_click.reset();
    if (!m_group || !m_group->views.at(index(view)).usable) return std::nullopt;
    return view;
}

std::string Session::input_rejection() const
{
    if (m_draft.design.prompt.size() > 2000) return "prompt_too_long";
    if (m_draft.design.custom_style.size() > 1000) return "custom_style_too_long";
    if (m_draft.design.style == "custom" &&
        m_draft.design.custom_style.find_first_not_of(" \r\n\t") == std::string::npos) return "custom_style_empty";
    const bool any_upload = std::any_of(m_draft.uploads.begin(), m_draft.uploads.end(),
        [](const std::string& value) { return !value.empty(); });
    if (any_upload && m_draft.uploads[0].empty()) return "missing_front";
    if (!any_upload && m_draft.design.prompt.find_first_not_of(" \r\n\t") == std::string::npos) return "empty_input";
    return {};
}

Eligibility Session::eligibility(const Capabilities& capabilities, const HostGuards& guards) const
{
    return eligibility_for(capabilities, guards, model_selection());
}

Eligibility Session::eligibility_for(const Capabilities& capabilities, const HostGuards& guards,
                                    const std::array<bool, 4>& selected, bool selection_entry) const
{
    Eligibility result;
    if (m_group && m_group->id != m_opening_id) {
        result.design_reason = result.model_reason = "history_loading";
        return result;
    }
    const bool busy = m_group && (!m_group->active_operation_id.empty() || m_group->model_busy);
    bool recoverable = false;
    bool unknown_design = false, selected_unknown_design = false;
    bool missing = !model_view_usable(ViewId::Front), pending = false;
    std::vector<ViewId> model_views;
    for (ViewId view : AllViews) if (selected[index(view)]) model_views.push_back(view);
    for (ViewId view : model_views) {
        missing = missing || !model_view_usable(view);
        pending = pending || (m_group && m_group->pending_views[index(view)]);
    }
    if (m_group) for (const auto view : AllViews) {
        const auto slot = index(view);
        const auto& state = m_group->views[slot].attempt_state;
        recoverable = recoverable || design_attempt_recoverable(state);
        unknown_design = unknown_design || state == "ambiguous";
        selected_unknown_design = selected_unknown_design || (selected[slot] && state == "ambiguous");
        result.retry = result.retry || state == "failed" || state == "stopped" || state == "blocked";
    }
    result.stop = busy && !m_group->stop_intent;
    result.recover = recoverable && !busy;
    // A missing image can precede workbench_guard in model_reason. Project
    // interaction directly from the guards, not from the first error string.
    result.click_generate_design = !busy && guards.can_start;
    result.click_generate_model = result.click_generate_design && !recoverable;
    const bool any_upload = std::any_of(m_draft.uploads.begin(), m_draft.uploads.end(),
                                        [](const std::string& value) { return !value.empty(); });
    const bool valid = input_rejection().empty();
    const bool initial = !m_group || !m_group->published;
    const bool image_capable = initial ? (any_upload ? capabilities.initial_references : capabilities.initial_text)
                                      : capabilities.edit_current;
    result.design_reason = busy ? "group_busy" : !guards.can_start ? "workbench_guard" :
        recoverable ? "recovery_required" :
        !capabilities.service_available ? "service_unavailable" : any_upload && m_draft.uploads[0].empty() ? "missing_front" : !valid ? "invalid_input" :
        !image_capable ? "unsupported_multiview" : "";
    result.generate_design = result.design_reason.empty();
    result.model_reason = busy ? "group_busy" : recoverable ? "recovery_required" :
         selected_unknown_design ? "design_result_unknown" :
        pending ? "design_regeneration_required" : missing ? "missing_current_view" :
        m_draft.uploads != m_group->uploads || !m_group->latest_submitted_draft ||
            m_draft.design != *m_group->latest_submitted_draft ? "draft_changed" : !guards.can_start ? "workbench_guard" :
        !guards.model_options_valid ? "invalid_model_options" :
        !capabilities.service_available || !guards.model_service_available ? "service_unavailable" :
        !(selection_entry ? (capabilities.model_image_counts_known ?
          std::any_of(capabilities.model_image_counts.begin(), capabilities.model_image_counts.end(), [](bool supported) { return supported; }) :
          capabilities.four_view_model) : model_views.size() >= 1 && model_views.size() <= 4 &&
          (capabilities.model_image_counts_known ? capabilities.model_image_counts[model_views.size() - 1] :
           model_views.size() == 4 && capabilities.four_view_model)) ? "unsupported_multiview" : "";
    result.generate_model = result.model_reason.empty();
    result.retry = result.retry && !busy && !recoverable && !unknown_design && guards.can_start && capabilities.service_available;
    return result;
}

LocalDraft Session::local_draft() const
{
    return {m_draft, m_invalidated_uploads, m_model_selected, m_model_seen, m_invalidated_versions, m_draft_dirty};
}

void Session::restore_local_draft(const LocalDraft& local)
{
    m_draft.geometry = local.input.geometry;
    if (local.dirty) {
        m_draft = local.input;
        m_draft_dirty = true;
        m_invalidated_uploads = local.invalidated_uploads;
        m_invalidated_versions = local.invalidated_versions;
        if (m_group) for (ViewId view : AllViews)
            if (m_invalidated_uploads[index(view)]) m_group->views[index(view)] = {};
        ++m_draft_revision;
    }
    m_model_selected = local.model_selected; m_model_seen = local.model_seen;
    ++m_model_selection_revision;
}

} // namespace Slic3r::GUI::AdvancedCreation
