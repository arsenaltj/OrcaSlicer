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

bool Session::set_upload(ViewId view, std::string path)
{
    if (m_draft.uploads.at(index(view)) == path) return false;
    m_draft.uploads[index(view)] = std::move(path);
    if (m_group && m_group->views[index(view)].usable && !m_selected[index(view)]) {
        m_selected[index(view)] = true;
        ++m_selection_revision;
    }
    m_draft_dirty = true;
    ++m_draft_revision;
    return true;
}

void Session::mark_draft_saved(const DesignDraft& saved)
{
    if (saved == m_draft.design) m_draft_dirty = false;
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
    return context();
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
    return context();
}

bool Session::matches(const CallbackContext& context) const
{
    return context.epoch == m_epoch && context.group_id == m_opening_id;
}

bool Session::apply_snapshot(GroupSnapshot snapshot, const CallbackContext& callback, bool restore_draft)
{
    if (!matches(callback) || snapshot.id != m_opening_id || snapshot.input_epoch.empty()) return false;
    if (m_group && m_group->id == snapshot.id && snapshot.revision < m_group->revision) return false;
    const bool inputs_changed = m_group && m_group->id == snapshot.id &&
                                m_group->input_epoch != snapshot.input_epoch;
    if (restore_draft || inputs_changed) {
        m_draft.uploads = snapshot.uploads;
        m_draft.design = snapshot.saved_draft;
        if (restore_draft) m_selected.fill(false);
        for (ViewId view : AllViews)
            if (snapshot.pending_views[index(view)] && snapshot.views[index(view)].usable)
                m_selected[index(view)] = true;
        m_draft_dirty = false;
        m_pending_click.reset();
        ++m_draft_revision;
        ++m_selection_revision;
    }
    m_group = std::move(snapshot);
    return true;
}

CommandContext Session::command_context() const
{
    return {context(), m_draft_revision, m_selection_revision, m_group ? m_group->revision : 0};
}

bool Session::matches_command(const CommandContext& command, bool uses_selection) const
{
    return matches(command.callback) && command.draft_revision == m_draft_revision &&
           (!uses_selection || command.selection_revision == m_selection_revision) &&
           command.group_revision == (m_group ? m_group->revision : 0);
}

bool Session::toggle(ViewId view)
{
    if (!m_group || !m_group->views.at(index(view)).usable) return false;
    if (m_group->pending_views[index(view)] && m_selected[index(view)]) return false;
    m_selected[index(view)] = !m_selected[index(view)];
    ++m_selection_revision;
    return true;
}

std::vector<ViewId> Session::effective_views() const
{
    std::vector<ViewId> result;
    for (ViewId view : AllViews) if (m_selected[index(view)]) result.push_back(view);
    if (result.empty()) result.assign(AllViews.begin(), AllViews.end());
    return result;
}

void Session::click(ViewId view, std::uint64_t now_ms, std::uint64_t double_click_ms)
{
    flush_click(now_ms);
    if (m_pending_click) toggle(m_pending_click->view);
    m_pending_click = PendingClick {view, now_ms + double_click_ms};
}

bool Session::flush_click(std::uint64_t now_ms)
{
    if (!m_pending_click || now_ms < m_pending_click->due) return false;
    const ViewId view = m_pending_click->view;
    m_pending_click.reset();
    return toggle(view);
}

std::optional<ViewId> Session::double_click(ViewId view)
{
    if (m_pending_click && m_pending_click->view != view) toggle(m_pending_click->view);
    m_pending_click.reset();
    if (!m_group || !m_group->views.at(index(view)).usable) return std::nullopt;
    return view;
}

Eligibility Session::eligibility(const Capabilities& capabilities, const HostGuards& guards) const
{
    Eligibility result;
    if (m_group && m_group->id != m_opening_id) {
        result.design_reason = result.model_reason = "history_loading";
        return result;
    }
    const bool busy = m_group && (!m_group->active_operation_id.empty() || m_group->model_busy);
    bool unresolved = false, missing = !m_group;
    if (m_group) for (const DesignView& view : m_group->views) {
        missing = missing || !view.usable;
        unresolved = unresolved || view.attempt_state == "ambiguous" || view.attempt_state == "result_pending" ||
                     view.attempt_state == "result_saved";
        result.retry = result.retry || view.attempt_state == "failed" || view.attempt_state == "stopped" ||
                       view.attempt_state == "blocked";
    }
    result.stop = busy && !m_group->stop_intent;
    result.recover = unresolved && !busy;
    const bool any_upload = std::any_of(m_draft.uploads.begin(), m_draft.uploads.end(),
                                        [](const std::string& value) { return !value.empty(); });
    const bool valid = m_draft.design.prompt.size() <= 2000 && m_draft.design.custom_style.size() <= 1000 &&
        (m_draft.design.style != "custom" || m_draft.design.custom_style.find_first_not_of(" \r\n\t") != std::string::npos) &&
        (any_upload ? !m_draft.uploads[0].empty() : m_draft.design.prompt.find_first_not_of(" \r\n\t") != std::string::npos);
    const bool initial = !m_group || !m_group->published;
    const bool image_capable = initial ? (any_upload ? capabilities.initial_references : capabilities.initial_text)
                                      : capabilities.edit_current;
    bool missing_target = false;
    if (!initial && m_group) for (ViewId view : effective_views()) missing_target |= !m_group->views[index(view)].usable;
    result.design_reason = busy ? "group_busy" : unresolved ? "recovery_required" : !guards.can_start ? "workbench_guard" :
        !capabilities.service_available ? "service_unavailable" : any_upload && m_draft.uploads[0].empty() ? "missing_front" : !valid ? "invalid_input" :
        !image_capable ? "unsupported_multiview" : missing_target ? "missing_current_view" : "";
    result.generate_design = result.design_reason.empty();
    result.model_reason = busy ? "group_busy" : unresolved ? "recovery_required" : missing ? "missing_current_view" :
        std::any_of(m_group->pending_views.begin(), m_group->pending_views.end(), [](bool pending) { return pending; }) ?
            "design_regeneration_required" :
        !inputs_match_results() ? "draft_changed" : !guards.can_start ? "workbench_guard" :
        !guards.model_options_valid ? "invalid_model_options" :
        !capabilities.service_available || !guards.model_service_available ? "service_unavailable" :
        !capabilities.four_view_model ? "unsupported_multiview" : "";
    result.generate_model = result.model_reason.empty();
    result.retry = result.retry && !busy && !unresolved && guards.can_start && capabilities.service_available;
    return result;
}

} // namespace Slic3r::GUI::AdvancedCreation
