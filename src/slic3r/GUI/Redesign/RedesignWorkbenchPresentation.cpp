#include "RedesignShell.hpp"
#include "AdvancedCreationWorkspace.hpp"
#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../AI/ModelGeneration/WorkbenchImportPresentation.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/MainFrameWorkspaceSupport.hpp"
#include "../I18N.hpp"
#include "../BBLTopbar.hpp"
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dcscreen.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/toplevel.h>
#include <wx/activityindicator.h>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {

namespace {
class RetainedWorkbenchPageFrame final : public wxFrame {
public:
    explicit RetainedWorkbenchPageFrame(wxWindow* owner)
        : wxFrame(owner, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxCLIP_CHILDREN)
    {
        SetBackgroundColour(RedesignTheme::background_colour());
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            if (!m_snapshot.IsOk()) return;
            const auto size = GetClientSize();
            const double scale = std::min(double(size.x) / m_snapshot.GetWidth(),
                double(size.y) / m_snapshot.GetHeight());
            dc.SetUserScale(scale, scale);
            dc.DrawBitmap(m_snapshot, 0, 0, false);
        });
    }
    bool retain(const wxRect& bounds) {
        if (bounds.IsEmpty()) return false;
        wxBitmap snapshot(bounds.width, bounds.height);
        wxMemoryDC memory(snapshot);
        wxScreenDC screen;
        const bool captured = memory.Blit(0, 0, bounds.width, bounds.height, &screen, bounds.x, bounds.y);
        memory.SelectObject(wxNullBitmap);
        if (!captured) return false;
        m_snapshot = snapshot;
        return true;
    }
    void release() { m_snapshot = wxNullBitmap; }
private:
    wxBitmap m_snapshot;
};

// Keep the retained source's colors intact while consuming its input. Disabling
// the native subtree would repaint it in disabled colors during preparation.
class RetainedPageInputFilter final : public wxEventFilter {
public:
    RetainedPageInputFilter(wxWindow* root, wxWindow* retained, wxWindow* feedback)
        : m_root(root), m_retained(retained), m_feedback(feedback) { wxEvtHandler::AddFilter(this); }
    ~RetainedPageInputFilter() override { wxEvtHandler::RemoveFilter(this); }
    int FilterEvent(wxEvent& event) override {
        auto* window = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (window && m_feedback && (window == m_feedback || m_feedback->IsDescendant(window)))
            return Event_Skip;
        if (window && (window == m_root || m_root->IsDescendant(window) ||
            window == m_retained || m_retained->IsDescendant(window)) &&
            (dynamic_cast<wxMouseEvent*>(&event) || dynamic_cast<wxKeyEvent*>(&event)))
            return Event_Processed;
        return Event_Skip;
    }
private:
    wxWindow* m_root;
    wxWindow* m_retained;
    wxWindow* m_feedback;
};
}

bool RedesignShell::begin_workbench_presentation(bool history_loading)
{
    if (m_workbench_transition) return true;
    auto* source = active_page_focus_root();
    auto* content = m_content_host ? m_content_host->GetSizer() : nullptr;
    if (!source || !content) return false;
    auto* item = content->GetItem(source);
    if (!item) return false;
    remember_active_page_focus();
    const auto token = m_page_transition.begin(
        {int(m_active_page), int(m_model_view)}, {int(Page::Model), int(ModelView::Workbench)});
    if (!token) return false;
    if (!m_pending_workbench_return_advanced &&
        !(m_active_page == Page::Model && m_model_view != ModelView::Result && m_workbench_return_advanced))
        m_pending_workbench_return_advanced = m_advanced_mode;
    m_workbench_transition = *token;
    m_workbench_source_index = 0;
    for (auto* candidate : content->GetChildren()) {
        if (candidate == item) break;
        ++m_workbench_source_index;
    }
    m_workbench_source_proportion = item->GetProportion();
    m_workbench_source_flags = item->GetFlag();
    m_workbench_source_border = item->GetBorder();
    if (!m_workbench_transition_cover) {
        m_workbench_transition_cover = new RetainedWorkbenchPageFrame(wxGetTopLevelParent(this));
        m_workbench_transition_cover->SetName("workbench-open-transition");
        m_workbench_transition_cover->SetBackgroundColour(RedesignTheme::background_colour());
        m_workbench_transition_cover->SetSizer(new wxBoxSizer(wxVERTICAL));
        m_workbench_transition_cover->Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& event) { event.Veto(); });
        m_workbench_transition_timer.SetOwner(this, wxWindow::NewControlId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!m_workbench_transition) return;
            auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
            const bool paused = !owner || owner->IsIconized() || !IsShownOnScreen();
            layout_workbench_transition_cover();
            if (!m_history_rollback_pending &&
                m_workbench_presentation_deadline.observe(RedesignPresentationDeadline::Clock::now(), paused))
                finish_workbench_presentation(false);
        }, m_workbench_transition_timer.GetId());
    }
    auto* retained = static_cast<RetainedWorkbenchPageFrame*>(m_workbench_transition_cover);
    const auto source_bounds = workbench_transition_bounds();
    if (!retained->retain(source_bounds)) {
        m_page_transition.rollback(*token);
        m_workbench_transition.reset();
        return false;
    }
    // Present the completed source pixels before native reparenting can expose
    // an empty backing surface. The real page remains retained for rollback.
    m_workbench_transition_cover->SetSize(source_bounds);
    m_workbench_transition_cover->ShowWithoutActivating();
    m_workbench_transition_cover->Refresh(false);
    m_workbench_transition_cover->Update();
    m_workbench_source_reparented = source != m_workbench_page;
    if (m_workbench_source_reparented) source->Hide();
    if (m_workbench_source_reparented &&
        (!content->Detach(source) || !source->Reparent(m_workbench_transition_cover))) {
        if (!content->GetItem(source)) content->Insert(m_workbench_source_index, source,
            m_workbench_source_proportion, m_workbench_source_flags, m_workbench_source_border);
        m_page_transition.rollback(*token);
        m_workbench_transition.reset();
        m_workbench_transition_cover->Hide();
        retained->release();
        source->Show();
        return false;
    }
    m_workbench_transition_source = source;
    if (m_workbench_source_reparented)
        m_workbench_transition_cover->GetSizer()->Add(source, 1, wxEXPAND);
    m_workbench_transition_input_filter = std::make_unique<RetainedPageInputFilter>(this, source,
        history_loading ? m_history_loading : nullptr);
    m_workbench_prepared_size = wxSize();
    m_workbench_history_loading = history_loading;
    if (history_loading) {
        m_history_started = std::chrono::steady_clock::now();
        m_history_elapsed->SetLabel(_L("已用时 0.0 秒"));
        m_history_cancel->Enable();
        update_history_loading(ModelHistoryOpenEvent::Stage::Reading);
        m_history_activity->Start();
    }
    // The retained pixels include the AI settings sibling. Hide it underneath
    // before measuring the target, rather than waiting until the route commits.
    sync_page_settings_visibility(Page::Model);
    Layout();
    m_workbench_presentation_deadline.begin(RedesignPresentationDeadline::Clock::now());
    layout_workbench_transition_cover();
    m_workbench_transition_timer.Start(50);
    if (history_loading) {
        layout_history_loading();
        m_history_loading->Update();
        return true;
    }
    return continue_workbench_presentation();
}

bool RedesignShell::continue_workbench_presentation()
{
    if (!m_workbench_transition || m_history_rollback_pending) return false;
    const auto token = *m_workbench_transition;
    m_workbench_history_loading = false;
    if (m_history_open_request) update_history_loading(ModelHistoryOpenEvent::Stage::Displaying);
    wxWeakRef<RedesignShell> weak(this);
    m_model_generation_host->prepare_workbench_presentation([weak, token] {
        if (weak && weak->m_page_transition.current(token) && weak->m_workbench_transition == token)
            weak->finish_workbench_presentation(true);
    });
    restore_smart_model_history();
    if (!m_model_generation_host->request_open_workbench()) {
        finish_workbench_presentation(false);
        return false;
    }
    m_workbench_page->Show();
    m_content_host->Layout();
    m_workbench_page->Layout();
    layout_workbench_transition_cover();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchPresentation] preparing token=" << token;
    return true;
}

void RedesignShell::layout_workbench_transition_cover()
{
    if (!m_workbench_transition || !m_workbench_transition_cover) return;
    auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (!owner || owner->IsIconized() || !m_content_host->IsShownOnScreen()) {
        m_workbench_transition_cover->Hide();
        if (m_history_loading) m_history_loading->Hide();
        return;
    }
    const auto size = m_content_host->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;
    const auto bounds = workbench_transition_bounds();
    const bool revealing = !m_workbench_transition_cover->IsShown();
    if (m_workbench_transition_cover->GetRect() != bounds) {
        m_workbench_transition_cover->SetSize(bounds);
        m_workbench_transition_cover->Layout();
    }
    if (revealing) m_workbench_transition_cover->ShowWithoutActivating();
    if (!m_workbench_history_loading && !m_history_rollback_pending &&
        (m_workbench_prepared_size != size || revealing)) {
        m_workbench_prepared_size = size;
        m_workbench_page->SetSize(wxRect(wxPoint(), size));
        m_workbench_page->Layout();
        if (m_model_generation_host) m_model_generation_host->set_workbench_presented(true);
    }
    layout_history_loading();
}

void RedesignShell::sync_page_settings_visibility(Page page)
{
    if (!m_image_settings_panel) return;
    const bool visible = page == Page::Image && !m_advanced_mode;
    if (m_image_settings_panel->IsShown() == visible) return;
    m_image_settings_panel->Show(visible);
    // Preserve its controls/draft/scroll position; only the shared outer layout
    // changes. Every navigation path measures the same final content viewport.
    Layout();
}

wxRect RedesignShell::workbench_transition_bounds() const
{
    if (!m_content_host) return {};
    wxRect bounds(m_content_host->ClientToScreen(wxPoint()), m_content_host->GetClientSize());
    return workbench_retained_page_bounds(bounds, m_image_settings_panel && m_image_settings_panel->IsShown()
        ? m_image_settings_panel->GetScreenRect() : wxRect{});
}

void RedesignShell::update_history_loading(ModelHistoryOpenEvent::Stage stage)
{
    if (!m_history_loading || !m_history_open_request || m_history_rollback_pending) return;
    wxString label;
    switch (stage) {
    case ModelHistoryOpenEvent::Stage::Reading: label = _L("正在读取模型与恢复外观…"); break;
    case ModelHistoryOpenEvent::Stage::Validating: label = _L("正在验证修复成果…"); break;
    case ModelHistoryOpenEvent::Stage::Preview: label = _L("正在准备模型预览…"); break;
    case ModelHistoryOpenEvent::Stage::Displaying: label = _L("正在等待工作台显示…"); break;
    }
    if (m_history_stage->GetLabel() != label) {
        m_history_stage->SetLabel(label);
        m_history_loading->Layout();
        m_history_loading->Refresh(false);
    }
}

void RedesignShell::layout_history_loading()
{
    if (!m_history_loading || !m_history_open_request || !m_workbench_transition) return;
    auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (!owner || owner->IsIconized() || !IsShownOnScreen()) { m_history_loading->Hide(); return; }
    const auto elapsed = wxString::Format(_L("已用时 %.1f 秒"),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_history_started).count());
    if (m_history_elapsed->GetLabel() != elapsed) m_history_elapsed->SetLabel(elapsed);
    const wxRect owner_bounds(owner->ClientToScreen(wxPoint()), owner->GetClientSize());
    auto* viewport = m_model_generation_host->workbench_import_viewport();
    const auto viewport_bounds = viewport && viewport->IsShownOnScreen()
        ? wxRect(viewport->ClientToScreen(wxPoint()), viewport->GetClientSize()) : workbench_transition_bounds();
    const auto bounds = workbench_import_card_bounds(viewport_bounds, owner_bounds, m_history_loading->GetBestSize());
    if (bounds.IsEmpty()) { m_history_loading->Hide(); return; }
    if (m_history_loading->GetRect() != bounds) {
        m_history_loading->SetSize(bounds);
        m_history_loading->Layout();
        apply_workbench_floating_card_shape(m_history_loading, 8, wxColour(72, 72, 78));
    }
    if (!m_history_loading->IsShown()) m_history_loading->ShowWithoutActivating();
    if (m_history_loading->IsEnabled()) m_history_loading->Raise();
}

void RedesignShell::finish_history_loading()
{
    if (m_history_activity) m_history_activity->Stop();
    if (m_history_loading) m_history_loading->Hide();
    m_history_open_asset.clear();
}

void RedesignShell::restore_workbench_source_page()
{
    m_workbench_transition_input_filter.reset();
    auto* source = m_workbench_transition_source;
    m_workbench_transition_source = nullptr;
    if (!source || !m_workbench_transition_cover || !m_workbench_source_reparented) return;
    m_workbench_source_reparented = false;
    source->Hide();
    m_workbench_transition_cover->GetSizer()->Detach(source);
    source->Reparent(m_content_host);
    auto* sizer = m_content_host->GetSizer();
    if (!sizer->GetItem(source)) sizer->Insert(std::min(m_workbench_source_index, sizer->GetItemCount()),
        source, m_workbench_source_proportion, m_workbench_source_flags, m_workbench_source_border);
}

void RedesignShell::finish_workbench_presentation(bool ready, bool report_failure)
{
    if (!m_workbench_transition || (ready && m_history_rollback_pending)) return;
    if (!ready && m_history_open_request && !m_history_rollback_pending) {
        m_history_rollback_pending = true;
        m_model_generation_host->cancel_workbench_presentation();
        m_history_stage->SetLabel(_L("正在取消加载…"));
        m_history_cancel->Disable();
        m_history_loading->Layout();
        m_history_loading->Refresh(false);
        wxWeakRef<RedesignShell> weak(this);
        m_model_generation_host->finish_history_model_open(false, [weak, report_failure] {
            if (!weak || !weak->m_workbench_transition) return;
            weak->m_history_open_request = 0;
            weak->m_history_rollback_pending = false;
            weak->m_model_generation_state = weak->m_model_generation_host->snapshot();
            weak->finish_workbench_presentation(false, report_failure);
        });
        return;
    }
    const auto token = *m_workbench_transition;
    auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (ready && (!owner || owner->IsIconized() || !IsShownOnScreen())) return;
    if (ready && m_workbench_prepared_size != m_content_host->GetClientSize()) {
        layout_workbench_transition_cover();
        return;
    }
    m_workbench_transition_timer.Stop();
    const auto source = m_page_transition.source();
    m_model_generation_host->cancel_workbench_presentation();
    if (ready) {
        m_page_transition.mark_ready(token);
        ready = m_page_transition.commit(token);
    }
    if (!ready) m_page_transition.rollback(token);
    m_workbench_transition.reset();
    m_workbench_history_loading = false;
    if (ready && m_history_open_request) {
        m_model_generation_host->finish_history_model_open(true);
        m_history_open_request = 0;
        m_model_generation_state = m_model_generation_host->snapshot();
        m_model_route_locked = true;
        m_model_route_session = m_model_generation_state.model_generation_session;
        m_model_route_id = model_generation_route_id(m_model_generation_state);
    }
    if (ready) {
        if (m_pending_workbench_return_advanced) m_workbench_return_advanced = m_pending_workbench_return_advanced;
        m_image_page_view_only = false;
        m_active_page = Page::Model;
        m_last_workbench_page = Page::Model;
        m_model_view = ModelView::Workbench;
        m_active_tab_id = TAB_ID_GENERATE_3D;
        for (auto* page : m_pages) if (page && page != m_workbench_transition_source) page->Hide();
    } else {
        m_workbench_page->Hide();
        m_active_page = static_cast<Page>(source.page);
        m_model_view = static_cast<ModelView>(source.view);
    }
    m_pending_workbench_return_advanced.reset();
    restore_workbench_source_page();
    if (m_advanced_workspace) m_advanced_workspace->Show(m_active_page == Page::Image && m_advanced_mode);
    if (m_advanced_mode) m_image_page->Hide();
    sync_page_settings_visibility(m_active_page);
    refresh_navigation();
    refresh_workflow_layout();
    m_model_generation_host->set_workbench_presented(ready ||
        (m_active_page == Page::Model && m_model_view == ModelView::Workbench));
    if (!ready && m_active_page == Page::Image) mount_smart_model_history();
    restore_active_page_focus();
    if (!ready) present_active_page(true);
    m_workbench_transition_cover->Hide();
    finish_history_loading();
    static_cast<RetainedWorkbenchPageFrame*>(m_workbench_transition_cover)->release();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchPresentation] " << (ready ? "committed" : "rolled back") << " token=" << token;
    if (!ready && report_failure)
        wxMessageBox(_L("工作台界面准备未完成，请重新点击打开工作台。"), _L("工作台"), wxOK | wxICON_WARNING, this);
}

} // namespace Slic3r::GUI
