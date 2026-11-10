#include "RedesignShell.hpp"
#include "RedesignTheme.hpp"
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
    bool retain(wxWindow* source) {
        const auto bounds = source->GetScreenRect();
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
    explicit RetainedPageInputFilter(wxWindow* root) : m_root(root) { wxEvtHandler::AddFilter(this); }
    ~RetainedPageInputFilter() override { wxEvtHandler::RemoveFilter(this); }
    int FilterEvent(wxEvent& event) override {
        auto* window = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (window && (window == m_root || m_root->IsDescendant(window)) &&
            (dynamic_cast<wxMouseEvent*>(&event) || dynamic_cast<wxKeyEvent*>(&event)))
            return Event_Processed;
        return Event_Skip;
    }
private:
    wxWindow* m_root;
};
}

bool RedesignShell::begin_workbench_presentation()
{
    if (m_workbench_transition) return true;
    auto* source = active_page_focus_root();
    auto* content = m_content_host ? m_content_host->GetSizer() : nullptr;
    if (!source || source == m_workbench_page || !content) return false;
    auto* item = content->GetItem(source);
    if (!item) return false;
    remember_active_page_focus();
    const auto token = m_page_transition.begin(
        {int(m_active_page), int(m_model_view)}, {int(Page::Model), int(ModelView::Workbench)});
    if (!token) return false;
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
            if (m_workbench_presentation_deadline.observe(RedesignPresentationDeadline::Clock::now(), paused))
                finish_workbench_presentation(false);
        }, m_workbench_transition_timer.GetId());
    }
    auto* retained = static_cast<RetainedWorkbenchPageFrame*>(m_workbench_transition_cover);
    if (!retained->retain(source)) {
        m_page_transition.rollback(*token);
        m_workbench_transition.reset();
        return false;
    }
    // Present the completed source pixels before native reparenting can expose
    // an empty backing surface. The real page remains retained for rollback.
    m_workbench_transition_cover->SetSize(wxRect(m_content_host->ClientToScreen(wxPoint()),
        m_content_host->GetClientSize()));
    m_workbench_transition_cover->ShowWithoutActivating();
    m_workbench_transition_cover->Refresh(false);
    m_workbench_transition_cover->Update();
    source->Hide();
    if (!content->Detach(source) || !source->Reparent(m_workbench_transition_cover)) {
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
    m_workbench_transition_cover->GetSizer()->Add(source, 1, wxEXPAND);
    m_workbench_transition_input_filter = std::make_unique<RetainedPageInputFilter>(source);
    m_workbench_prepared_size = wxSize();
    wxWeakRef<RedesignShell> weak(this);
    m_model_generation_host->prepare_workbench_presentation([weak, token = *token] {
        if (weak && weak->m_page_transition.current(token) && weak->m_workbench_transition == token)
            weak->finish_workbench_presentation(true);
    });
    m_workbench_presentation_deadline.begin(RedesignPresentationDeadline::Clock::now());
    layout_workbench_transition_cover();
    present_active_page(true);

    restore_smart_model_history();
    if (!m_model_generation_host->request_open_workbench()) {
        finish_workbench_presentation(false);
        return false;
    }
    m_workbench_page->Show();
    m_content_host->Layout();
    m_workbench_page->Layout();
    m_workbench_transition_timer.Start(50);
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchPresentation] preparing token=" << *token;
    return true;
}

void RedesignShell::layout_workbench_transition_cover()
{
    if (!m_workbench_transition || !m_workbench_transition_cover) return;
    auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (!owner || owner->IsIconized() || !m_content_host->IsShownOnScreen()) {
        m_workbench_transition_cover->Hide();
        return;
    }
    const auto size = m_content_host->GetClientSize();
    if (size.x <= 0 || size.y <= 0) return;
    const wxRect bounds(m_content_host->ClientToScreen(wxPoint()), size);
    const bool revealing = !m_workbench_transition_cover->IsShown();
    if (m_workbench_transition_cover->GetRect() != bounds) {
        m_workbench_transition_cover->SetSize(bounds);
        m_workbench_transition_cover->Layout();
    }
    if (revealing) m_workbench_transition_cover->ShowWithoutActivating();
    if (m_workbench_prepared_size != size || revealing) {
        m_workbench_prepared_size = size;
        m_workbench_page->SetSize(wxRect(wxPoint(), size));
        m_workbench_page->Layout();
        if (m_model_generation_host) m_model_generation_host->set_workbench_presented(true);
    }
}

void RedesignShell::restore_workbench_source_page()
{
    m_workbench_transition_input_filter.reset();
    auto* source = m_workbench_transition_source;
    m_workbench_transition_source = nullptr;
    if (!source || !m_workbench_transition_cover) return;
    source->Hide();
    m_workbench_transition_cover->GetSizer()->Detach(source);
    source->Reparent(m_content_host);
    auto* sizer = m_content_host->GetSizer();
    if (!sizer->GetItem(source)) sizer->Insert(std::min(m_workbench_source_index, sizer->GetItemCount()),
        source, m_workbench_source_proportion, m_workbench_source_flags, m_workbench_source_border);
}

void RedesignShell::finish_workbench_presentation(bool ready, bool report_failure)
{
    if (!m_workbench_transition) return;
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
    if (ready) {
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
    restore_workbench_source_page();
    refresh_navigation();
    refresh_workflow_layout();
    m_model_generation_host->set_workbench_presented(ready);
    if (!ready && m_active_page == Page::Image) mount_smart_model_history();
    restore_active_page_focus();
    if (!ready) present_active_page(true);
    m_workbench_transition_cover->Hide();
    static_cast<RetainedWorkbenchPageFrame*>(m_workbench_transition_cover)->release();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchPresentation] " << (ready ? "committed" : "rolled back") << " token=" << token;
    if (!ready && report_failure)
        wxMessageBox(_L("工作台界面准备未完成，请重新点击打开工作台。"), _L("工作台"), wxOK | wxICON_WARNING, this);
}

} // namespace Slic3r::GUI
