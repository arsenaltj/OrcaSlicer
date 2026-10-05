#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/AI/ColorMatching/LocalPrintColorPanel.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelImageDisplayCopy.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <glad/gl.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/collpane.h>
#include <wx/colordlg.h>
#include <wx/clrpicker.h>
#include <wx/dataobj.h>
#include <wx/dnd.h>
#include <wx/textentry.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <cmath>


#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <utility>


#include "ModelGenerationInputStyle.hpp"
#include "ModelGenerationInputWelcome.hpp"
namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

namespace {
// Local drawer affordance; the library remains the only content/selection owner.
class ImageDrawerHandle final : public wxControl, public AIThemeOwner {
public:
    ImageDrawerHandle(wxWindow* parent, std::function<void()> toggle)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          m_toggle(std::move(toggle))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(25, 131)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        reload_bitmaps();
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            reload_bitmaps();
            SetMinSize(FromDIP(wxSize(25, 131)));
            event.Skip();
        });
        set_open(false);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { SetFocus(); m_toggle(); });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::background)); dc.Clear();
            // Figma wrapper rotate(180deg) scaleY(-1) equals a horizontal mirror.
            dc.DrawBitmap(m_body, 0, 0, true);
            const wxBitmap& arrow = m_open ? m_arrow_open : m_arrow_closed;
            const wxSize size = GetClientSize();
            dc.DrawBitmap(arrow, (size.x - arrow.GetWidth()) / 2,
                (size.y - arrow.GetHeight()) / 2, true);
            if (HasFocus()) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(ModelGenerationInputStyle::yellow, FromDIP(1)));
                dc.DrawRoundedRectangle(GetClientRect().Deflate(FromDIP(1)), FromDIP(4));
            }
        });
    }
    bool AcceptsFocus() const override { return true; }
    void apply_ai_theme(bool) override { SetBackgroundColour(ModelGenerationInputStyle::background); Refresh(false); }
    void set_open(bool open) {
        m_open = open;
        SetName(open ? _L("收起我的图片") : _L("展开我的图片"));
        SetToolTip(open ? _L("收起我的图片（Esc）") : _L("展开我的图片；选择后可打开历史设计"));
        Refresh(false);
    }
private:
    void reload_bitmaps() {
        m_body = wxBitmap(create_scaled_bitmap("figma-ux/drawer-handle", this, 131).ConvertToImage().Mirror(true));
        m_arrow_open = create_scaled_bitmap("figma-ux/drawer-handle-arrow", this, 16);
        m_arrow_closed = wxBitmap(m_arrow_open.ConvertToImage().Mirror(true));
    }
    wxBitmap m_body, m_arrow_open, m_arrow_closed;
    std::function<void()> m_toggle;
    bool m_open {false};
};

// A leaf entry for the model tool rail; reuse Button's focus, activation and
// rounded state painting, with the original 24 DIP arrow in its trailing slot.
class ModelToolsBeautyEntry final : public Button {
public:
    explicit ModelToolsBeautyEntry(wxWindow* parent) : Button(parent, _L("3D 美颜工作台")),
        m_arrow(this, "figma-ux/model-tools-beauty-arrow", 24) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            StaticBox::render(dc);
            dc.SetFont(GetFont());
            dc.SetTextForeground(!IsEnabled() ? ModelGenerationInputStyle::secondary :
                m_current ? ModelGenerationInputStyle::yellow : ModelGenerationInputStyle::text);
            const wxSize size = GetClientSize();
            const auto text = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(1, size.x - FromDIP(72)));
            dc.DrawText(text, FromDIP(16), (size.y - dc.GetTextExtent(text).y) / 2);
            dc.DrawBitmap(m_arrow.bmp(), size.x - FromDIP(16) - m_arrow.GetBmpWidth(),
                (size.y - m_arrow.GetBmpHeight()) / 2, true);
        });
    }
    void set_current(bool current) { if (m_current != current) { m_current = current; Refresh(); } }
private:
    ScalableBitmap m_arrow;
    bool m_current {false};
};

class InputImageDropTarget final : public wxFileDropTarget {
public:
    InputImageDropTarget(std::function<bool()> enabled, std::function<bool(const wxArrayString&)> accept)
        : m_enabled(std::move(enabled)), m_accept(std::move(accept)) {}
    wxDragResult OnDragOver(wxCoord, wxCoord, wxDragResult) override { return m_enabled() ? wxDragCopy : wxDragNone; }
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override { return m_enabled() && m_accept(files); }
private:
    std::function<bool()> m_enabled;
    std::function<bool(const wxArrayString&)> m_accept;
};
}

void ModelGenerationPanel::build_page()
{
    using namespace ModelGenerationInputStyle;
    SetBackgroundColour(background);
    auto* root = new wxBoxSizer(wxHORIZONTAL);
    m_workflow_panel = build_workflow_panel(this);
    root->Add(m_workflow_panel, 0, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(12));
    auto* content = new Surface(this);
    content->SetBackgroundColour(background);
    content->SetMinSize(wxSize(1, 1));
    auto* views = new wxBoxSizer(wxVERTICAL);
    m_input_welcome = new Welcome(content);
    m_input_results = build_preview_panel(content);
    m_input_results->SetMinSize(wxSize(1, 1));
    views->Add(m_input_welcome, 1, wxEXPAND);
    views->Add(m_input_results, 1, wxEXPAND);
    // An empty model is a real destination, not a disabled or redirected tab.
    m_model_empty = new RoundedPanel(content);
    auto* empty = new wxBoxSizer(wxVERTICAL);
    empty->AddStretchSpacer();
    auto* title = new Label(m_model_empty, _L("还没有可查看的 3D 模型"), LB_AUTO_WRAP);
    title->SetMinSize(wxSize(1, -1));
    empty->Add(title, 0, wxEXPAND | wxALL, FromDIP(24));
    auto* hint = new Label(m_model_empty,
        _L("从图片或文字开始创作，或打开已有资产。模型生成中可返回图像页查看进度。"), LB_AUTO_WRAP);
    hint->SetName("input_secondary");
    hint->SetMinSize(wxSize(1, -1));
    empty->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(24));
    for (const auto& entry : std::array<std::pair<wxString, WorkspaceAction>, 2>{{
             {_L("去图像创作"), WorkspaceAction::ShowImage},
             {_L("打开我的资产"), WorkspaceAction::ShowLibrary}}}) {
        auto* button = new Button(m_model_empty, entry.first);
        button->SetName(entry.second == WorkspaceAction::ShowImage ? "input_primary" : "input_field");
        button->SetPaddingSize(FromDIP(wxSize(16, 12)));
        empty->Add(button, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(24));
        button->Bind(wxEVT_BUTTON, [this, action = entry.second](wxCommandEvent&) {
            dispatch_workspace_action(action);
        });
    }
    empty->AddStretchSpacer();
    m_model_empty->SetSizer(empty);
    m_model_empty->Hide();
    views->Add(m_model_empty, 1, wxEXPAND);
    content->SetSizer(views);
    root->Add(content, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    m_library_drawer_toggle = new ImageDrawerHandle(this, [this] {
        set_library_drawer(!m_library_drawer_open);
    });
    root->Add(m_library_drawer_toggle, 0, wxALIGN_CENTER_VERTICAL);
    m_library_drawer_popup = new wxPopupWindow(this, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS);
    m_library_drawer_popup->SetBackgroundColour(background);
    auto* drawer_background = new Surface(m_library_drawer_popup);
    auto* drawer_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_library_drawer_popup_toggle = new ImageDrawerHandle(drawer_background, [this] {
        set_library_drawer(false);
    });
    static_cast<ImageDrawerHandle*>(m_library_drawer_popup_toggle)->set_open(true);
    drawer_sizer->Add(m_library_drawer_popup_toggle, 0, wxALIGN_CENTER_VERTICAL);
    m_library_drawer_host = new Surface(drawer_background);
    m_library_drawer_host->SetMinSize(FromDIP(wxSize(392, 1)));
    m_library_drawer_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    drawer_sizer->Add(m_library_drawer_host, 1, wxEXPAND);
    drawer_background->SetSizer(drawer_sizer);
    auto* popup_sizer = new wxBoxSizer(wxVERTICAL);
    popup_sizer->Add(drawer_background, 1, wxEXPAND);
    m_library_drawer_popup->SetSizer(popup_sizer);
    m_library_drawer_popup->Hide();
    SetSizer(root);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (m_library_drawer_open) {
            if (GetClientSize().x < FromDIP(1160)) set_library_drawer(false);
            else position_library_drawer();
        }
        // Switch only when the available width crosses the tool-rail boundary.
        if (m_finishing_workbench && m_workflow_panel &&
            m_workflow_panel->IsShown() != (GetClientSize().x >= FromDIP(1180)))
            refresh_input_layout();
        event.Skip();
    });
    wxWeakRef<ModelGenerationPanel> weak(this);
    wxGetTopLevelParent(this)->Bind(wxEVT_MOVE, [weak](wxMoveEvent& event) {
        if (weak && weak->m_library_drawer_open) weak->position_library_drawer();
        event.Skip();
    });
    m_library_drawer_popup->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (m_library_drawer_open && event.CmdDown() &&
            !event.AltDown() && !event.ShiftDown() &&
            (event.GetKeyCode() == WXK_PAGEUP || event.GetKeyCode() == WXK_PAGEDOWN)) {
            // The popup has no native notebook ancestor. Restore its projection
            // before sending the same window-change navigation from the owner.
            const bool forward = event.GetKeyCode() == WXK_PAGEDOWN;
            set_library_drawer(false);
            Navigate(wxNavigationKeyEvent::WinChange |
                (forward ? wxNavigationKeyEvent::IsForward : wxNavigationKeyEvent::IsBackward));
            return;
        }
        if (m_library_drawer_open && event.GetKeyCode() == WXK_TAB &&
            !event.AltDown() && !event.CmdDown()) {
            // The popup is its own top-level window: native traversal stops at
            // its last control and can move backwards into the obscured GL host.
            wxWindow* last = nullptr;
            for (auto it = m_library_drawer_pager.rbegin(); it != m_library_drawer_pager.rend(); ++it) {
                if (*it && (*it)->IsShownOnScreen() && (*it)->IsEnabled()) { last = *it; break; }
            }
            if (!last && m_library_drawer_use->IsShownOnScreen() && m_library_drawer_use->IsEnabled())
                last = m_library_drawer_use;
            if (!last) {
                for (auto it = m_library_thumbnails.rbegin(); it != m_library_thumbnails.rend(); ++it) {
                    if (*it && (*it)->IsShownOnScreen() && (*it)->IsEnabled()) { last = *it; break; }
                }
            }
            if (!last) last = m_library_search;
            wxWindow* focused = wxWindow::FindFocus();
            if (event.ShiftDown() && focused == m_library_drawer_popup_toggle) {
                last->SetFocus();
                return;
            }
            if (!event.ShiftDown() && focused == last) {
                m_library_drawer_popup_toggle->SetFocus();
                return;
            }
        }
        if (event.GetKeyCode() == WXK_ESCAPE &&
            (!m_library_search->HasFocus() || m_library_search->IsEmpty())) {
            set_library_drawer(false);
            return;
        }
        if (m_library_drawer_popup_toggle->HasFocus() && !event.AltDown() && !event.CmdDown() &&
            (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
            set_library_drawer(false);
            return;
        }
        event.Skip();
    });
    const auto drop_target = [this]() { return new InputImageDropTarget(
        [this]() { return input_editable(); }, [this](const wxArrayString& files) { return accept_input_files(files); }); };
    m_choose_image->SetDropTarget(drop_target());
    m_input_welcome->SetDropTarget(drop_target());
    content->SetDropTarget(drop_target());
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        // Handle before dialog navigation consumes Enter; other controls retain their keys.
        if (m_library_drawer_toggle->HasFocus() && !event.AltDown() && !event.CmdDown() &&
            (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
            set_library_drawer(!m_library_drawer_open);
            return;
        }
        if (m_library_drawer_open && event.GetKeyCode() == WXK_ESCAPE &&
            (!m_library_search->HasFocus() || m_library_search->IsEmpty())) {
            set_library_drawer(false);
            return;
        }
        // Editable fields own text paste and IME. Never consume their shortcut.
        auto* focus = wxWindow::FindFocus();
        if (m_workspace_view == ModelGenerationPresentation::WorkspaceView::Image &&
            event.GetKeyCode() == 'V' && event.CmdDown() && !event.AltDown() &&
            !event.ShiftDown() && !dynamic_cast<wxTextEntryBase*>(focus) &&
            !dynamic_cast<ComboBox*>(focus)) {
            paste_input_image();
            return;
        }
        event.Skip();
    });
    refresh_ai_appearance(this);
}

void ModelGenerationPanel::apply_ai_theme(bool update_fonts)
{
    SetBackgroundColour(ModelGenerationInputStyle::background);
    for (auto* child : GetChildren()) refresh_ai_appearance(child, update_fonts);
    Refresh(false);
}

void ModelGenerationPanel::navigate_workspace(WorkspaceAction action)
{
    initialize_page();
    if (action == WorkspaceAction::ShowImage && !m_model_preview_ready)
        m_preserve_image_on_model_ready = true;
    dispatch_workspace_action(action);
}

void ModelGenerationPanel::dispatch_workspace_action(WorkspaceAction action)
{
    if (m_shutdown || !m_page_initialized) return;
    if (m_library_drawer_open) set_library_drawer(false);
    if (action == WorkspaceAction::Prepare) {
        if (m_prepare_navigation) m_prepare_navigation();
        return;
    }
    select_workspace_view(workspace_destination(action, m_workspace_view, m_model_preview_ready));
}

void ModelGenerationPanel::set_library_drawer(bool open)
{
    if (!m_page_initialized || !m_library_drawer_popup || open == m_library_drawer_open) return;
    if (open && m_finishing_workbench) set_finishing_workbench(false);
    // At compact widths use the same full library, without squeezing the GL/image host.
    if (open && GetClientSize().x < FromDIP(1160)) {
        select_workspace_view(WorkspaceView::Library);
        return;
    }
    if (open && m_workspace_view == WorkspaceView::Library) select_workspace_view(m_result_view);
    cancel_library_loading();
    m_library_filter_timer.Stop();
    m_library_drawer_open = open;
    if (open) {
        m_library_saved_query = m_library_search->GetValue();
        m_library_search->SetHint(_L("搜索图片 / Task ID"));
        m_library_saved_category = m_library_category;
        m_library_saved_page = m_library_page;
        m_library_saved_selection = m_library_selected_asset_id;
        const int index = m_preview_book->FindPage(m_library_view_page);
        if (index != wxNOT_FOUND) m_preview_book->RemovePage(index);
        m_library_view_page->Reparent(m_library_drawer_host);
        m_library_drawer_host->GetSizer()->Add(m_library_view_page, 1, wxEXPAND);
        m_library_search->ChangeValue(wxEmptyString);
        m_library_category = ModelLibraryCategory::Design;
        m_library_page = 0;
        m_library_selected_asset_id.clear();
    } else {
        m_library_search->SetHint(wxEmptyString);
        m_library_view_page->Hide();
        m_library_drawer_host->GetSizer()->Detach(m_library_view_page);
        m_library_view_page->Reparent(m_preview_book);
        m_preview_book->InsertPage(1, m_library_view_page, _L("历史资产"), false);
        m_library_search->ChangeValue(m_library_saved_query);
        m_library_category = m_library_saved_category;
        m_library_page = m_library_saved_page;
        m_library_selected_asset_id = m_library_saved_selection;
    }
    m_library_page_size = open ? 10 : 12;
    // Snapshot refresh anchors must use the restored projection, never the
    // drawer's indices with the full library's page size (or vice versa).
    m_library_filtered_indices = filter_model_library(m_library_entries, m_library_category,
        m_library_search->GetValue(), m_library_drawer_open);
    for (size_t i = 0; i < m_library_filter_buttons.size(); ++i) {
        auto* button = m_library_filter_buttons[i];
        button->SetName(i == static_cast<size_t>(m_library_category) ? "input_quiet" : "input_field");
        ModelGenerationInputStyle::apply(button, false);
    }
    update_drawer_selection();
    for (auto* control : m_library_full_controls) control->Show(!open);
    m_library_drawer_footer->Show(open);
    m_library_title->SetLabel(open ? _L("我的图片") : _L("我的资产"));
    m_library_drawer_toggle->Show(!open);
    if (open) {
        position_library_drawer();
        m_library_view_page->Show();
        m_library_drawer_popup->Layout();
    } else {
        m_library_view_page->SetSize(m_preview_book->GetClientSize());
        m_library_view_page->Layout();
    }
    // Do not expose cards from the other projection while the new scan runs.
    refresh_library(true);
    m_library_drawer_popup->Show(open);
    m_library_refresh_pending = true;
    refresh_controls();
    Layout();
    if (open) {
        m_library_drawer_popup->Layout();
        load_library_entries();
        m_library_search->SetFocus();
    } else {
        m_library_drawer_toggle->SetFocus();
    }
}

void ModelGenerationPanel::position_library_drawer()
{
    if (!m_library_drawer_popup) return;
    const int width = FromDIP(417);
    const int inset = FromDIP(12);
    const wxSize panel = GetClientSize();
    m_library_drawer_popup->SetSize(wxRect(
        ClientToScreen(wxPoint(panel.x - width - inset, inset)),
        wxSize(width, std::max(FromDIP(1), panel.y - 2 * inset))));
}

void ModelGenerationPanel::select_workspace_view(WorkspaceView view)
{
    if (!m_preview_book || !m_expand_images) return;
    if (view == WorkspaceView::Image && m_finishing_workbench) set_finishing_workbench(false);
    m_workspace_view = view;
    if (view != WorkspaceView::Library) m_result_view = view;
    m_selecting_workspace = true;
    const int page = m_preview_book->FindPage(view == WorkspaceView::Library ? m_library_view_page : m_model_page);
    if (page != wxNOT_FOUND) m_preview_book->SetSelection(page);
    m_expand_images->SetValue(m_result_view == WorkspaceView::Image);
    m_expand_images->SetLabel(m_result_view == WorkspaceView::Image ? _L("返回模型对照") : _L("展开图片"));
    m_selecting_workspace = false;
    refresh_controls();
}

void ModelGenerationPanel::refresh_input_layout()
{
    using namespace ModelGenerationInputStyle;
    if (!m_input_welcome || !m_preview_book) return;
    // MSW native edit controls restore system text colors when re-enabled.
    // Reapply their field role only on that transition, never on every poll.
    const std::array<wxTextCtrl*, 3> fields {m_prompt, m_custom_style, m_prepared_prompt};
    for (size_t i = 0; i < fields.size(); ++i) {
        if (m_input_text_enabled[i] != fields[i]->IsThisEnabled()) {
            m_input_text_enabled[i] = fields[i]->IsThisEnabled();
            apply_control(fields[i], Role::Field, false, true);
        }
    }
    const auto view = workspace_presentation(m_workspace_view,
        m_reference_image.IsOk() || m_style_preview_image.IsOk(), m_model_preview_ready,
        m_busy, m_awaiting_confirmation, m_ready, static_cast<bool>(m_prepare_navigation));
    m_workspace_view = view.view;
    const bool empty_model = view.view == WorkspaceView::Model && !m_model_preview_ready;
    bool layout_changed = m_input_welcome->Show(view.welcome);
    layout_changed |= m_model_empty->Show(empty_model);
    layout_changed |= m_workflow_panel->Show(!empty_model && view.view != WorkspaceView::Library &&
        (!m_finishing_workbench || GetClientSize().x >= FromDIP(1180)));
    layout_changed |= m_input_results->Show(!view.welcome && !empty_model);
    layout_changed |= m_input_journey->Show(view.journey && view.view != WorkspaceView::Model);
    const bool library_view = view.view == WorkspaceView::Library;
    const bool model_overview = view.view == WorkspaceView::Model && m_model_preview_ready;
    const bool result_chrome = !library_view && !m_finishing_workbench && !model_overview;
    for (wxWindow* control : std::array<wxWindow*, 4>{m_preview_kind, m_preview_stage_hint,
                                                     m_preview_message, m_result_summary})
        layout_changed |= control->Show(result_chrome);
    // The library owns its header and status; reserve no empty result toolbar.
    auto* header_item = m_preview_kind->GetParent()->GetSizer()->GetItem(m_preview_kind->GetContainingSizer());
    const int header_border = result_chrome ? FromDIP(18) : 0;
    if (header_item && header_item->GetBorder() != header_border) {
        header_item->SetBorder(header_border);
        layout_changed = true;
    }
    const bool image_tools = view.view == WorkspaceView::Image && !m_finishing_workbench;
    for (wxWindow* control : std::array<wxWindow*, 4>{m_zoom_out, m_zoom_fit, m_zoom_in, m_preview_zoom})
        layout_changed |= control->Show(image_tools);
    layout_changed |= m_preview_details_pane->Show(image_tools && m_model_views_available);
    layout_changed |= m_input_form->Show(!model_overview);
    layout_changed |= m_input_actions->Show(!model_overview || m_busy);
    const bool overview_visibility_changed = m_model_overview->Show(model_overview);
    layout_changed |= overview_visibility_changed;
    layout_changed |= m_finishing_shortcut->Show(!library_view && !model_overview && !m_finishing_workbench && m_model_preview_ready);
    layout_changed |= m_expand_images->Show(image_tools && m_model_preview_ready);
    // The overview owns the model facts; leave the central GL canvas for viewing it.
    layout_changed |= m_model_stats->Show(!model_overview);
    if (model_overview) {
        auto update_overview_label = [&layout_changed](wxStaticText* target, const wxString& value) {
            if (target->GetLabel() == value) return;
            target->SetLabel(value);
            target->InvalidateBestSize();
            layout_changed = true;
        };
        auto unwrapped_text = [](wxStaticText* source) {
            // The hidden result label may have been laid out at one pixel wide.
            // Its displayed label contains wrapping newlines, not the model facts.
            auto* label = dynamic_cast<Label*>(source);
            if (!label) return source->GetLabel();
            const int width = label->GetSize().x;
            label->Wrap(-1);
            const wxString value = label->GetLabel();
            label->Wrap(width);
            return value;
        };
        update_overview_label(m_model_overview_action_status, unwrapped_text(m_status));
        update_overview_label(m_model_overview_stats, unwrapped_text(m_model_stats));
        // The report remains bound to the selected saved artifact while the
        // canvas can show an unsaved candidate or a comparison with its source.
        const bool unsaved_finishing = m_finishing_running || !m_finishing_candidate.empty() ||
            (m_beauty_controls && m_beauty_controls->has_changes());
        const wxString report_status = unwrapped_text(m_model_quality_status);
        update_overview_label(m_model_overview_quality_status, unsaved_finishing
            ? _L("原保存版本 · ") + report_status : report_status);
        const auto check_risks = model_check_risks(m_model_quality);
        wxString report_summary = check_risks.empty()
            ? unwrapped_text(m_model_quality_summary)
            : wxString::Format(_L("本次有 %llu 项风险提醒，完整列于下方。"),
                static_cast<unsigned long long>(check_risks.size()));
        if (unsaved_finishing)
            report_summary = _L("以下检查信息属于原保存版本；当前修改尚未保存。保存为新版本后需重新检查。\n") +
                report_summary;
        update_overview_label(m_model_overview_quality_summary, report_summary);
        const wxString beauty_label = m_finishing_workbench
            ? _L("当前：3D 美颜工作台") : _L("3D 美颜工作台");
        if (m_model_overview_beauty->GetLabel() != beauty_label) {
            m_model_overview_beauty->SetLabel(beauty_label);
            layout_changed = true;
        }
        m_model_overview_beauty->Enable(m_finishing_shortcut->IsEnabled());
        m_model_overview_beauty->SetToolTip(m_finishing_workbench
            ? _L("当前正在美颜工作台；右侧返回按钮可回到模型总览，保留编辑和视角。")
            : m_finishing_shortcut->GetToolTipText());
        static_cast<ModelToolsBeautyEntry*>(m_model_overview_beauty)->set_current(m_finishing_workbench);
        refresh_model_tools();
        m_model_overview_check->Enable(m_recheck_model->IsEnabled());
        m_model_overview_check->SetLabel(unsaved_finishing
            ? _L("先保存修改再检查") : _L("检查当前模型"));
        m_model_overview_check->SetToolTip(unsaved_finishing
            ? _L("编辑与预览尚未保存；请先保存为新版本，再检查该版本。原保存版本的报告保留。")
            : m_recheck_model->IsEnabled()
                ? _L("检查当前模型的结构风险，不调用付费 AI，也不自动修复或切片。")
                : _L("当前模型不能运行服务端检查；加入工程后可使用原生模型检查。未检查不代表检查通过。"));
    }
    if (m_workspace_changed) m_workspace_changed();
    const auto encoded = m_prompt->GetValue().ToUTF8();
    const auto count = wxString::Format(_L("%llu / 2000 字节（UTF-8）"),
        static_cast<unsigned long long>(encoded ? encoded.length() : 0));
    if (m_prompt_count->GetLabel() != count) m_prompt_count->SetLabel(count);
    // Once a design can advance to 3D, regenerating it is a secondary action.
    // Keep the role on the control so a later theme refresh preserves it.
    const bool regenerate = m_generate->IsShown();
    m_preprocess->SetName(regenerate ? "input_field" : "input_primary");
    apply_control(m_preprocess, regenerate ? Role::Field : Role::PrimaryAction);
    apply_control(m_generate, Role::PrimaryAction);
    apply_control(m_import, Role::PrimaryAction);
    const wxString previous_status = m_status->GetLabel();
    if (!m_awaiting_confirmation && !m_busy && !m_ready && !m_model_preview_ready) {
        m_preprocess->SetLabel(_L("生成 2D 设计图"));
        m_preprocess->Show();
        if (m_service_available) {
            if (encoded && encoded.length() > MAX_MODEL_INPUT_BYTES)
                m_status->SetLabel(_L("描述超过 2000 字节，请精简后生成；文字已完整保留。"));
            else if (m_prompt->GetValue().Strip(wxString::both).empty() && !has_image_input())
                m_status->SetLabel(_L("请上传图片或输入描述词"));
            else if (m_status->GetLabel() == _L("空闲") ||
                     m_status->GetLabel() == _L("描述超过 2000 字节，请精简后生成；文字已完整保留。") ||
                     m_status->GetLabel() == _L("请上传图片或输入描述词") ||
                     m_status->GetLabel() == _L("输入已就绪，可生成 2D 设计图") ||
                     m_status->GetLabel() == _L("请选择风格，然后生成 2D 设计图") ||
                     m_status->GetLabel() == _L("请完善输入和风格设置"))
                m_status->SetLabel(current_style().empty() ? _L("请选择风格，然后生成 2D 设计图") :
                    m_preprocess->IsEnabled() ? _L("输入已就绪，可生成 2D 设计图") :
                    _L("请完善输入和风格设置"));
        }
    }
    if (m_status->GetLabel() != previous_status) {
        m_status->InvalidateBestSize();
        m_status->GetParent()->Layout();
        layout_changed = true;
    }
    if (layout_changed) {
        Layout();
        m_workflow_panel->Layout();
        m_model_overview->Layout();
        static_cast<wxScrolledWindow*>(m_model_overview)->FitInside();
        sync_model_overview_scroll_track();
        sync_input_form_scroll_track();
        m_input_welcome->GetParent()->Layout();
        // MSW can retain pixels from controls uncovered by the resized GL area.
        // Repaint only when the workspace layout actually changes.
        m_input_results->Refresh();
    }
    if (overview_visibility_changed && model_overview) {
        // Hidden labels can measure before the scroller has its final width.
        // Rewrap after the first visible layout so the cached best height does
        // not leave a large gap or push the check action under the footer.
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->m_model_overview->IsShown()) return;
            for (wxStaticText* text : {weak->m_model_overview_action_status, weak->m_model_overview_stats,
                                       weak->m_model_overview_quality_status,
                                       weak->m_model_overview_quality_summary}) {
                static_cast<Label*>(text)->Wrap(text->GetSize().x);
                text->InvalidateBestSize();
            }
            weak->m_model_overview->Layout();
            static_cast<wxScrolledWindow*>(weak->m_model_overview)->FitInside();
            weak->sync_model_overview_scroll_track();
        });
    }
}
void ModelGenerationPanel::refresh_input_image_thumbnail()
{
    if (!m_choose_image) return;
    const bool has_image = !m_selected_image_path.empty() && m_reference_image.IsOk();
    const int extent = m_choose_image->FromDIP(128);
    if (m_input_thumbnail_extent == extent &&
        ((!has_image && !m_input_thumbnail_source.IsOk()) ||
         (has_image && m_input_thumbnail_source.IsOk() &&
          m_input_thumbnail_source.GetData() == m_reference_image.GetData())))
        return;

    // Keep a shared decoded image reference so same-size replacements cannot
    // reuse the previous thumbnail. No additional disk reads or task state.
    m_input_thumbnail_extent = extent;
    m_input_thumbnail_source = has_image ? m_reference_image : wxImage();
    m_choose_image->SetPaddingSize(m_choose_image->FromDIP(wxSize(12, 4)));
    if (has_image) {
        const double scale = std::min(double(extent) / m_reference_image.GetWidth(),
                                      double(extent) / m_reference_image.GetHeight());
        const int width = std::max(1, int(std::lround(m_reference_image.GetWidth() * scale)));
        const int height = std::max(1, int(std::lround(m_reference_image.GetHeight() * scale)));
        m_choose_image->SetLabel(_L("更换图片"));
        m_choose_image->SetIcon(wxBitmap(m_reference_image.Scale(width, height, wxIMAGE_QUALITY_HIGH)));
        m_choose_image->SetMinSize(wxSize(-1, m_choose_image->FromDIP(160)));
        m_choose_image->SetToolTip(_L("当前参考图；点击更换，或拖入一张 PNG/JPEG 图片。"));
    } else {
        m_choose_image->SetLabel(_L("点击或拖入图片"));
        m_choose_image->SetIcon(create_scaled_bitmap("figma-ux/add", m_choose_image, 26));
        m_choose_image->SetMinSize(wxSize(-1, m_choose_image->FromDIP(68)));
        m_choose_image->SetToolTip(_L("选择或拖入一张 PNG/JPEG 图片。"));
    }
    if (m_input_form) {
        m_input_form->Layout();
        m_input_form->FitInside();
        sync_input_form_scroll_track();
    }
}

wxWindow* ModelGenerationPanel::build_workflow_panel(wxWindow* parent)
{
    auto* panel = new ModelGenerationInputStyle::RoundedPanel(parent);
    panel->SetMinSize(wxSize(FromDIP(330), -1));
    panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* journey = new wxPanel(panel);
    m_input_journey = journey;
    journey->SetBackgroundColour(wxColour(244, 248, 248));
    auto* journey_sizer = new wxBoxSizer(wxVERTICAL);
    auto* step_row = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 4> step_names = {
        _L("1 输入"), _L("2 图片确认"), _L("3 生成 3D"), _L("4 查看")
    };
    for (size_t index = 0; index < step_names.size(); ++index) {
        m_step_labels[index] = new wxStaticText(journey, wxID_ANY, step_names[index],
                                                wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_step_labels[index]->SetForegroundColour(index == 0 ? wxColour(24, 112, 105) : wxColour(132, 143, 145));
        wxFont step_font = m_step_labels[index]->GetFont();
        step_font.SetWeight(index == 0 ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_step_labels[index]->SetFont(step_font);
        step_row->Add(m_step_labels[index], 1, wxALIGN_CENTER_VERTICAL);
    }
    journey_sizer->Add(step_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    m_workflow_steps = new wxStaticText(journey, wxID_ANY, _L("输入文字、图片，或同时使用两者"));
    m_workflow_steps->SetForegroundColour(wxColour(91, 104, 107));
    m_workflow_steps->Wrap(FromDIP(330));
    journey_sizer->Add(m_workflow_steps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto* progress_header = new wxBoxSizer(wxHORIZONTAL);
    m_workflow_phase = new wxStaticText(journey, wxID_ANY, _L("检查本地服务"));
    wxFont workflow_font = m_workflow_phase->GetFont();
    workflow_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_workflow_phase->SetFont(workflow_font);
    m_progress_percent = new wxStaticText(journey, wxID_ANY, "0%");
    m_progress_percent->SetForegroundColour(wxColour(91, 104, 107));
    progress_header->Add(m_workflow_phase, 1, wxALIGN_CENTER_VERTICAL);
    progress_header->Add(m_progress_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    journey_sizer->Add(progress_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    m_generation_progress = new wxGauge(journey, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(6)));
    m_generation_progress->SetValue(0);
    journey_sizer->Add(m_generation_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    journey_sizer->AddSpacer(FromDIP(8));
    journey->SetSizer(journey_sizer);
    outer->Add(journey, 0, wxEXPAND);

    auto* scroll = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxVSCROLL | wxBORDER_NONE);
    m_input_form = scroll;
    scroll->SetBackgroundColour(wxColour(250, 251, 251));
    scroll->SetScrollRate(0, FromDIP(12));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(section_label(scroll, _L("上传图片")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    sizer->AddSpacer(FromDIP(12));
    auto* image_row = new wxBoxSizer(wxHORIZONTAL);
    m_choose_image = new Button(scroll, _L("点击或拖入图片"), "figma-ux/add", 0, 26);
    m_choose_image->SetMinSize(wxSize(-1, FromDIP(68)));
    m_choose_image->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        m_input_thumbnail_extent = 0;
        refresh_input_image_thumbnail();
        event.Skip();
    });
    m_clear_image = new Button(scroll, _L("移除"));
    m_clear_image->SetName("input_field");
    m_clear_image->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_paste_image = new Button(scroll, _L("粘贴图片"));
    m_paste_image->SetName("input_field");
    m_paste_image->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_paste_image->SetToolTip(_L("粘贴剪贴板图片或单个图片文件；描述词框内 Ctrl+V 仍粘贴文字。"));
    m_paste_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { paste_input_image(); });
    m_selected_image = new wxStaticText(scroll, wxID_ANY, _L("未选择图片"),
                                        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_selected_image->SetMinSize(wxSize(FromDIP(70), -1));
    m_choose_image->SetVertical();
    sizer->Add(m_choose_image, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* supported = new wxStaticText(scroll, wxID_ANY, _L("PNG、JPG、JPEG · 最大 20 MB"));
    supported->SetToolTip(_L("宽高至少 64 px，总像素不超过 1677 万；暂不支持 WebP。"));
    sizer->Add(supported, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));
    image_row->Add(m_paste_image, 0, wxRIGHT, FromDIP(6));
    image_row->Add(m_clear_image, 0, wxRIGHT, FromDIP(6));
    image_row->Add(m_selected_image, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(image_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    sizer->AddSpacer(FromDIP(10));
    m_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("描述词"));
    sizer->Add(m_prompt_label, 0, wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(4));
    m_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(66)),
                              wxTE_MULTILINE);
    m_prompt->SetHint(_L("描述你想创作的内容，例如：一只可爱的小猫。"));
    sizer->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    m_prompt_count = new wxStaticText(scroll, wxID_ANY, _L("0 / 2000 字节（UTF-8）"));
    sizer->Add(m_prompt_count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* style_row = new wxBoxSizer(wxHORIZONTAL);
    auto* style_label = new wxStaticText(scroll, wxID_ANY, _L("风格"));
    wxArrayString styles;
    styles.Add(_L("单色写实"));
    styles.Add(_L("多色写实"));
    styles.Add(_L("多色风格化"));
    m_style = new ComboBox(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(215, 42)), 0, nullptr, wxCB_READONLY);
    for (const auto& style : styles) m_style->Append(style);
    m_style->SetSelection(0);
    auto* style_icon = new wxStaticBitmap(scroll, wxID_ANY, create_scaled_bitmap("figma-ux/emoji", scroll, 24));
    style_row->Add(style_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    style_row->Add(style_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    style_row->Add(m_style, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(style_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));
    wxArrayString stylized;
    for (const char* style : {"portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom"})
        stylized.Add(ModelGenerationPresentation::style_label(style));
    m_stylized_style = new ComboBox(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(280, 42)), 0, nullptr, wxCB_READONLY);
    for (const auto& style : stylized) m_stylized_style->Append(style);
    m_stylized_style->SetSelection(1);
    m_stylized_style->SetToolTip(_L("选择多色风格化的具体表现方式"));
    sizer->Add(m_stylized_style, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    m_custom_style_panel = new wxPanel(scroll);
    m_custom_style_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* custom_style_sizer = new wxBoxSizer(wxVERTICAL);
    auto* custom_style_label = new wxStaticText(m_custom_style_panel, wxID_ANY, _L("自定义风格描述"));
    m_custom_style = new wxTextCtrl(m_custom_style_panel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(-1, FromDIP(50)), wxTE_MULTILINE);
    m_custom_style->SetHint(_L("描述外观即可；系统会保留主体、构图和可见元素"));
    m_custom_style->SetMaxLength(240);
    custom_style_sizer->Add(custom_style_label, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    custom_style_sizer->Add(m_custom_style, 0, wxEXPAND);
    m_custom_style_panel->SetSizer(custom_style_sizer);
    m_custom_style_panel->Hide();
    sizer->Add(m_custom_style_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    m_prepare_base = new wxCheckBox(scroll, wxID_ANY, _L("打印准备时添加底座"));
    m_prepare_base->SetValue(false);
    m_prepare_base->SetToolTip(_L("仅为当前作品记录准备意图，不改变 AI 输入或重新生成。加入工程后确认底座模板和总高度，再显式应用；重新打开历史作品默认关闭。"));
    sizer->Add(m_prepare_base, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    m_style_recommendation_panel = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_style_recommendation_panel->SetBackgroundColour(wxColour(239, 248, 246));
    auto* style_recommendation_sizer = new wxBoxSizer(wxVERTICAL);
    m_style_recommendation_title = new wxStaticText(m_style_recommendation_panel, wxID_ANY, _L("正在推荐风格..."));
    wxFont recommendation_font = m_style_recommendation_title->GetFont();
    recommendation_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_style_recommendation_title->SetFont(recommendation_font);
    m_style_recommendation_title->SetForegroundColour(wxColour(31, 97, 90));
    style_recommendation_sizer->Add(m_style_recommendation_title, 0, wxEXPAND | wxALL, FromDIP(8));
    m_style_recommendation_reason = new wxStaticText(m_style_recommendation_panel, wxID_ANY, wxEmptyString);
    m_style_recommendation_reason->SetForegroundColour(wxColour(75, 91, 94));
    m_style_recommendation_reason->Wrap(FromDIP(290));
    style_recommendation_sizer->Add(m_style_recommendation_reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    auto* style_alternatives = new wxBoxSizer(wxHORIZONTAL);
    m_style_recommendation_alternative_label = new wxStaticText(
        m_style_recommendation_panel, wxID_ANY, _L("也可以："));
    style_alternatives->Add(m_style_recommendation_alternative_label, 0,
                            wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    for (wxButton*& button : m_style_recommendation_alternatives) {
        button = new wxButton(m_style_recommendation_panel, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        style_alternatives->Add(button, 0, wxRIGHT, FromDIP(6));
    }
    style_recommendation_sizer->Add(style_alternatives, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_style_recommendation_panel->SetSizer(style_recommendation_sizer);
    m_style_recommendation_panel->Hide();
    sizer->AddSpacer(FromDIP(6));
    sizer->Add(m_style_recommendation_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_upload_notice = new wxStaticText(scroll, wxID_ANY, _L("仅会将选中的图片和文字描述发送给 AI。"));
    m_upload_notice->Wrap(FromDIP(310));
    m_upload_notice->SetForegroundColour(wxColour(91, 104, 107));
    sizer->AddSpacer(FromDIP(4));
    sizer->Add(m_upload_notice, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));

    auto* creation_colors = new wxStaticText(scroll, wxID_ANY,
        _L("生成保留自然颜色和细节；完成后再到 Orca 匹配打印耗材。"));
    creation_colors->Wrap(FromDIP(310));
    sizer->Add(creation_colors, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_model_settings_panel = new wxPanel(scroll);
    m_model_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* model_settings_sizer = new wxBoxSizer(wxVERTICAL);
    model_settings_sizer->Add(section_label(m_model_settings_panel, _L("3D 生成设置")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* provider_row = new wxBoxSizer(wxHORIZONTAL);
    provider_row->Add(new wxStaticText(m_model_settings_panel, wxID_ANY, _L("模型服务")),
                      0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_provider = new wxChoice(m_model_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                              wxArrayString {wxString("Tripo"), _L("腾讯混元3D")});
    m_provider->SetMinSize(wxSize(1, -1));
    m_provider->SetSelection(0);
    provider_row->Add(m_provider, 1, wxALIGN_CENTER_VERTICAL);
    model_settings_sizer->Add(provider_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* quality_row = new wxBoxSizer(wxHORIZONTAL);
    auto* quality_label = new wxStaticText(m_model_settings_panel, wxID_ANY, _L("目标面数"));
    wxArrayString quality_levels;
    quality_levels.Add(_L("30 万面"));
    quality_levels.Add(_L("100 万面（推荐）"));
    quality_levels.Add(_L("200 万面（需精细几何）"));
    m_quality = new wxChoice(m_model_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, quality_levels);
    // The workflow sidebar has a bounded width. Let the sizer shrink choices
    // instead of growing the scrolled content beyond its visible right edge.
    m_quality->SetMinSize(wxSize(1, -1));
    m_quality->SetSelection(1);
    m_quality->SetToolTip(
        _L("面数是生成目标，实际结果可能不同。200 万面需 Tripo v3.1 精细几何；更多面数会增加下载、预览和导入耗时。"));
    quality_row->Add(quality_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    quality_row->Add(m_quality, 1, wxALIGN_CENTER_VERTICAL);
    model_settings_sizer->Add(quality_row, 0, wxEXPAND);
    auto add_option = [&](const wxString& label, const wxArrayString& choices) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(m_model_settings_panel, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        auto* choice = new wxChoice(m_model_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
        choice->SetMinSize(wxSize(1, -1));
        choice->SetSelection(0);
        row->Add(choice, 1, wxALIGN_CENTER_VERTICAL);
        model_settings_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(6));
        return choice;
    };
    m_geometry_quality = add_option(_L("几何"), {_L("标准"), _L("精细")});
    m_texture_quality = add_option(_L("纹理"), {_L("标准"), _L("高清"), _L("8K")});
    m_output_format = add_option(_L("格式"), {_L("GLB"), _L("OBJ（另建转换任务）")});
    m_generation_cost = new wxStaticText(m_model_settings_panel, wxID_ANY, wxEmptyString,
                                         wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    m_generation_cost->SetMinSize(wxSize(1, -1));
    model_settings_sizer->Add(m_generation_cost, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_model_settings_panel->SetSizer(model_settings_sizer);
    sizer->Add(m_model_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    sizer->Add(build_import_settings(scroll), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_preprocess_section = section_label(scroll, _L("确认提示词"));
    sizer->Add(m_preprocess_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("用于 3D 生成的提示词"));
    sizer->Add(m_prepared_prompt_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(72)), wxTE_MULTILINE);
    sizer->Add(m_prepared_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    scroll->SetSizer(sizer);
    scroll->FitInside();
    outer->Add(scroll, 1, wxEXPAND);
    auto* overview = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_model_overview = overview;
    overview->SetScrollRate(0, FromDIP(12));
    auto* overview_sizer = new wxBoxSizer(wxVERTICAL);
    // The model tool rail follows the Figma groups. Statuses remain model data,
    // never the sample risk counts or six-color palette from the design.
    auto add_heading = [&](const wxString& title, const std::string& icon = std::string{},
                           const wxString& hint = wxEmptyString) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(section_label(overview, title), 0, wxALIGN_CENTER_VERTICAL);
        if (!icon.empty()) {
            auto* about = new wxStaticBitmap(overview, wxID_ANY, create_scaled_bitmap(icon, overview, 16));
            about->SetToolTip(hint);
            row->Add(about, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        }
        overview_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(16));
    };
    auto add_action = [&](Button* button) {
        button->SetName("input_field");
        button->SetPaddingSize(FromDIP(wxSize(16, 10)));
        button->SetMinSize(wxSize(-1, FromDIP(56)));
        overview_sizer->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    };
    auto add_note = [&](Label* label) {
        label->SetMinSize(wxSize(1, -1));
        label->SetName("input_secondary");
        overview_sizer->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    };
    add_heading(_L("检查修复"), "figma-ux/model-tools-check-about",
        _L("检查只提示当前模型风险，不自动修复；壁厚和悬垂还需实际尺寸和打印条件。"));
    m_model_overview_check = new Button(overview, _L("检查当前模型"));
    m_model_overview_check->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recheck_model, this);
    add_action(m_model_overview_check);
    m_model_overview_quality_status = new Label(overview, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1)));
    add_note(static_cast<Label*>(m_model_overview_quality_status));
    m_model_overview_quality_summary = new Label(overview, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1)));
    add_note(static_cast<Label*>(m_model_overview_quality_summary));
    m_model_overview_risks = new ModelGenerationInputStyle::RoundedPanel(overview);
    m_model_overview_risks->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_model_overview_risks->Hide();
    overview_sizer->Add(m_model_overview_risks, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_model_overview_check_details = new Button(overview, _L("查看检查范围与指标"));
    add_action(m_model_overview_check_details);
    m_model_overview_check_details->Hide();
    m_model_overview_check_metrics = new Label(overview, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1)));
    add_note(static_cast<Label*>(m_model_overview_check_metrics));
    m_model_overview_check_metrics->Hide();
    m_model_overview_check_details->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_model_overview_check_expanded = !m_model_overview_check_expanded;
        m_model_overview_check_metrics->Show(m_model_overview_check_expanded);
        m_model_overview_check_details->SetLabel(m_model_overview_check_expanded
            ? _L("收起检查范围与指标") : _L("查看检查范围与指标"));
        m_model_overview->Layout();
        static_cast<wxScrolledWindow*>(m_model_overview)->FitInside();
        sync_model_overview_scroll_track();
    });

    add_heading(_L("多色模型"));
    m_model_overview_color_match = new Button(overview, _L("匹配打印颜色"));
    m_model_overview_color_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        open_model_color_matching(m_model_overview_color_match);
    });
    add_action(m_model_overview_color_match);
    add_note(new Label(overview, _L("使用当前保存版本匹配耗材，编辑中的修改需先保存。"),
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1))));

    add_heading(_L("3D 美颜"), "figma-ux/model-tools-beauty-about",
        _L("原件保留；预览、对照并确认后保存为独立版本。"));
    m_model_overview_beauty = new ModelToolsBeautyEntry(overview);
    m_model_overview_beauty->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_finishing_workbench) open_model_finishing();
    });
    add_action(m_model_overview_beauty);
    add_heading(_L("切片"));
    m_model_overview_slice = new Button(overview, _L("导入切片"));
    m_model_overview_slice->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        if (m_model_overview_slice->IsEnabled() && m_import->IsEnabled()) on_import(event);
    });
    add_action(m_model_overview_slice);
    m_model_overview_slice->SetName("input_quiet");
    add_note(new Label(overview, _L("进入准备页设置尺寸与底座，再确认切片。"),
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1))));

    m_model_overview_action_status = new Label(overview, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1)));
    add_note(static_cast<Label*>(m_model_overview_action_status));

    add_heading(_L("当前模型信息"));
    m_model_overview_stats = new Label(overview, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(285, -1)));
    add_note(static_cast<Label*>(m_model_overview_stats));
    overview->SetSizer(overview_sizer);
    attach_model_overview_scroll_track();
    overview->Hide();
    outer->Add(overview, 1, wxEXPAND);

    auto* action_panel = new wxPanel(panel);
    m_input_actions = action_panel;
    action_panel->SetBackgroundColour(*wxWHITE);
    auto* action_panel_sizer = new wxBoxSizer(wxVERTICAL);
    auto* status_row = new wxBoxSizer(wxVERTICAL);
    // Keep the full status above the diagnostics action. The shared Label
    // retains its unwrapped text and reflows it when the available width changes.
    m_status = new Label(action_panel, wxGetApp().normal_font(), _L("空闲"),
                         LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(310, -1)));
    m_status->SetMinSize(wxSize(1, -1));
    m_workflow_steps->Wrap(FromDIP(330));
    m_workflow_steps->InvalidateBestSize();
    m_status->SetForegroundColour(wxColour(60, 75, 78));
    auto* open_diagnostics = new Button(scroll, _L("打开诊断日志"));
    open_diagnostics->SetName("input_field");
    open_diagnostics->SetToolTip(_L("打开 AI 后台日志目录；反馈问题时请发送 orca-ai-sidecar.log 和诊断 ID"));
    status_row->Add(m_status, 0, wxEXPAND);
    sizer->Add(open_diagnostics, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));
    action_panel_sizer->Add(status_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* action_buttons = new wxBoxSizer(wxVERTICAL);
    m_preprocess = new Button(action_panel, _L("生成 2D 设计图"));
    m_preprocess->SetMinSize(wxSize(-1, FromDIP(44)));
    m_generate = new Button(action_panel, _L("确认并生成 3D"));
    m_generate->SetName("input_field");
    m_generate->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_generate->SetMinSize(wxSize(-1, FromDIP(44)));
    m_stop = new Button(action_panel, _L("停止生成"));
    m_stop->SetName("input_field");
    m_stop->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_stop->SetMinSize(wxSize(-1, FromDIP(44)));
    m_retry_service = new Button(action_panel, _L("重新检测服务"));
    m_retry_service->SetName("input_field");
    m_retry_service->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_retry_service->SetMinSize(wxSize(-1, FromDIP(44)));
    m_import = new Button(action_panel, _L("将此模型加入工程"));
    m_import->SetName("input_field");
    m_import->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_import->SetMinSize(wxSize(-1, FromDIP(44)));
    m_discard = new Button(action_panel, _L("修改输入，重新设计"));
    m_discard->SetName("input_field");
    m_discard->SetPaddingSize(FromDIP(wxSize(12, 9)));
    m_discard->SetMinSize(wxSize(-1, FromDIP(44)));
    action_buttons->Add(m_preprocess, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    action_buttons->Add(m_generate, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    action_buttons->Add(m_stop, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    action_buttons->Add(m_retry_service, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    action_buttons->Add(m_import, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    action_buttons->Add(m_discard, 0, wxEXPAND);
    action_panel_sizer->Add(action_buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    action_panel->SetSizer(action_panel_sizer);
    outer->Add(action_panel, 0, wxEXPAND);
    panel->SetSizer(outer);
    m_choose_image->SetName("input_field");
    m_prompt_count->SetName("input_secondary");
    m_preprocess->SetName("input_primary");
    m_generate->SetName("input_primary");
    m_import->SetName("input_primary");
    refresh_ai_appearance(panel);

    m_prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (input_editable() && m_job_id.empty() && !m_ready && m_service_available)
            m_status->SetLabel(_L("空闲"));
        refresh_controls();
    });
    m_style->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        select_style(current_style(), true);
    });
    m_stylized_style->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { select_style(current_style(), true); });
    for (size_t index = 0; index < m_style_recommendation_alternatives.size(); ++index) {
        m_style_recommendation_alternatives[index]->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (index < m_style_recommendation.alternatives.size())
                select_style(m_style_recommendation.alternatives[index], true);
        });
    }
    m_custom_style->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    for (auto* choice : {m_provider, m_quality, m_geometry_quality, m_texture_quality, m_output_format})
        choice->Bind(wxEVT_CHOICE, [this, choice](wxCommandEvent&) {
            if (choice == m_provider && !m_busy) {
                ++m_sequence; // Ignore callbacks from the previously selected provider.
                m_submission_state.clear();
                if (m_awaiting_confirmation) {
                    m_status->SetLabel(_L("模型服务已切换，请确认设置后继续。"));
                    m_result_summary->SetLabel(_L("当前设计图已保留；生成需要再次确认。"));
                }
            }
            m_legacy_generation_defaults = false;
            refresh_provider_options();
            persist_generation_options();
        });
    m_choose_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_choose_image, this);
    m_clear_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_clear_image, this);
    m_import_color_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_preprocess->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_preprocess, this);
    m_generate->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_generate, this);
    m_stop->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_stop, this);
    m_retry_service->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_retry_service, this);
    m_import->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_import, this);
    m_discard->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_discard, this);
    open_diagnostics->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const boost::filesystem::path log_directory = boost::filesystem::path(Slic3r::data_dir()) / "log";
        boost::system::error_code ec;
        boost::filesystem::create_directories(log_directory, ec);
        wxString path = from_path(log_directory);
        if (!path.empty() && !wxFileName::IsPathSeparator(path.Last()))
            path += wxFileName::GetPathSeparator();
        if (ec || !wxLaunchDefaultApplication(path))
            show_error(this, _L("无法打开诊断日志目录：") + from_path(log_directory));
    });
    return panel;
}


} // namespace Slic3r::GUI
