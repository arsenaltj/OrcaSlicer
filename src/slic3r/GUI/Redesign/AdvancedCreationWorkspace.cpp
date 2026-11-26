#include "AdvancedCreationWorkspace.hpp"
#include "AdvancedCreationWindowEvents.hpp"
#include "AdvancedPreviewGesture.hpp"
#include "AdvancedPreviewLayout.hpp"
#include "InlineModelGenerationView.hpp"
#include "../AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "RedesignWidgets.hpp"
#include "RedesignMessageDialog.hpp"
#include "SmartCreationControls.hpp"
#include "../AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "../AI/Text/AITextClient.hpp"
#include "../AISidecarClient.hpp"
#include "../GUI_App.hpp"
#include "../wxExtensions.hpp"
#include "../Presentation/AsyncPreviewImage.hpp"
#include <wx/display.h>
#include <wx/choice.h>
#include <wx/control.h>
#include <wx/cursor.h>
#include <wx/geometry.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/popupwin.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wupdlock.h>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r::GUI {
using namespace AdvancedCreation;
using namespace RedesignTheme;
namespace {
wxString t(const char* value) { return wxString::FromUTF8(value); }
wxString title(ViewId view)
{
    static const std::array<wxString, 4> names {t("主视角"), t("左视角"), t("后视角"), t("右视角")};
    return names.at(index(view));
}
wxString image_model_display_name(const std::string& model_id)
{
    const auto separator = model_id.find_last_of('/');
    std::string name = model_id.substr(separator == std::string::npos ? 0 : separator + 1);
    const std::string suffix = "-PC-YOYO";
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    return wxString::FromUTF8(name);
}
std::uint64_t now_ms()
{ return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int double_click_ms()
{
#ifdef __WXMSW__
    return static_cast<int>(GetDoubleClickTime());
#else
    return 500;
#endif
}
wxString reason_text(const std::string& reason)
{
    if (reason == "unsupported_multiview") return t("当前模型尚未确认支持多视角，请检查供应商配置。");
    if (reason == "service_unavailable") return t("等待本地 AI 服务连接。");
    if (reason == "missing_current_view") return t("请先生成可用的主视角及所选设计图。");
    if (reason == "recovery_required") return t("存在未完成的请求，请先恢复原任务结果。");
    if (reason == "invalid_input") return t("请上传主参考图或填写描述，描述最多 2000 字符。");
    if (reason == "missing_front") return t("请先上传主视角图片。");
    if (reason == "stale_confirmation") return t("所选图片或确认计划已变化，请重新确认。");
    if (reason == "history_loading") return t("正在恢复整组历史…");
    return wxEmptyString;
}
class ImageDrop final : public wxFileDropTarget {
public:
    explicit ImageDrop(std::function<void(wxString)> accept) : m_accept(std::move(accept)) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& paths) override {
        if (paths.size() != 1) return false;
        m_accept(paths.front()); return true;
    }
private:
    std::function<void(wxString)> m_accept;
};

wxColour black_overlay(const wxColour& base, int alpha)
{
    const auto channel = [alpha](unsigned char value) {
        return static_cast<unsigned char>((static_cast<int>(value) * (255 - alpha) + 127) / 255);
    };
    return wxColour(channel(base.Red()), channel(base.Green()), channel(base.Blue()));
}

wxColour upload_outer_colour() { return black_overlay(panel_colour(), 102); }
wxColour upload_slot_colour() { return black_overlay(upload_outer_colour(), 38); }


wxFont figma_font(wxWindow* window, int pixel_size, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
{
    wxFontInfo info(10);
    info.Family(wxFONTFAMILY_SWISS);
    const wxString family = font_family();
    if (!family.empty()) info.FaceName(family);
    wxFont font(info);
    font.SetPixelSize(wxSize(0, window->FromDIP(pixel_size)));
    font.SetWeight(weight);
    return font;
}

class UploadInfoPopup final : public wxPopupTransientWindow {
public:
    explicit UploadInfoPopup(wxWindow* owner) : wxPopupTransientWindow(owner, wxBORDER_SIMPLE)
    {
        const wxColour face(248, 248, 248);
        const wxColour text(32, 32, 32);
        const wxColour secondary(88, 88, 88);
        SetBackgroundColour(face);
        auto* content = new wxBoxSizer(wxVERTICAL);

        auto* heading = new wxStaticText(this, wxID_ANY, t("多视角图片上传"));
        heading->SetBackgroundColour(face);
        heading->SetFont(figma_font(heading, 13, wxFONTWEIGHT_MEDIUM));
        heading->SetForegroundColour(text);
        content->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        const auto add_text = [this, content, face](const wxString& text, const wxColour& colour, int top) {
            auto* label = new wxStaticText(this, wxID_ANY, text);
            label->SetBackgroundColour(face);
            label->SetFont(figma_font(label, 12));
            label->SetForegroundColour(colour);
            content->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                         FromDIP(top));
        };
        add_text(t("主视图为必填的主参考图，不要求物体必须朝向\n"
                   "语义正面。左、后、右视图可选，用于补充对应\n"
                   "方向的外观细节。"),
                 text, 10);
        add_text(t("点击任一视角上传，也可以将图片拖入对应位置；\n"
                   "点击已有图片可以替换，点击右上角可以移除。"),
                 text, 8);
        add_text(t("支持 PNG、JPG、JPEG 和静态 WebP，单张最大\n20 MB。"),
                 secondary, 8);
        content->AddSpacer(FromDIP(16));
        SetSizer(content);
        content->Fit(this);
    }
};

class UploadInfoIcon final : public wxPanel {
public:
    explicit UploadInfoIcon(wxWindow* parent) : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(16, 16)));
        SetMaxSize(FromDIP(wxSize(16, 16)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetName("advanced-upload-info");
        m_info_bg = create_scaled_bitmap("advanced_upload_info_bg", this, 16);
        m_info_outline = create_scaled_bitmap("advanced_upload_info_outline", this, 12);
        m_info_stem = create_scaled_bitmap("advanced_upload_info_stem", this, 5);
        m_info_dot = create_scaled_bitmap("advanced_upload_info_dot", this, 1);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            show_popup();
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            if (m_popup) m_popup->Dismiss();
            event.Skip();
        });
    }

private:
    void show_popup()
    {
        if (!m_popup) m_popup = new UploadInfoPopup(this);
        wxPoint position = ClientToScreen(wxPoint(0, GetClientSize().y + FromDIP(6)));
        const wxSize size = m_popup->GetSize();
        const int display_index = wxDisplay::GetFromWindow(this);
        if (display_index != wxNOT_FOUND) {
            const wxRect bounds = wxDisplay(display_index).GetClientArea();
            position.x = std::clamp(position.x, bounds.x,
                                    std::max(bounds.x, bounds.GetRight() - size.x + 1));
            position.y = std::clamp(position.y, bounds.y,
                                    std::max(bounds.y, bounds.GetBottom() - size.y + 1));
        }
        m_popup->Move(position);
        m_popup->Popup();
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour()));
        dc.Clear();
        dc.DrawBitmap(m_info_bg, 0, 0, true);
        dc.DrawBitmap(m_info_outline, FromDIP(2), FromDIP(2), true);
        dc.DrawBitmap(m_info_stem, FromDIP(7), FromDIP(5), true);
        dc.DrawBitmap(m_info_dot, FromDIP(7), FromDIP(11), true);
    }

    wxBitmap m_info_bg;
    wxBitmap m_info_outline;
    wxBitmap m_info_stem;
    wxBitmap m_info_dot;
    UploadInfoPopup* m_popup {nullptr};
};

class AdvancedTitleHeading final : public wxPanel {
public:
    explicit AdvancedTitleHeading(wxWindow* parent) : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(312, 20))); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetToolTip(t("分别配置多视角2D设计图和四视角3D模型生成。"));
        m_info_bg = create_scaled_bitmap("advanced_upload_info_bg", this, 16);
        m_info_outline = create_scaled_bitmap("advanced_upload_info_outline", this, 12);
        m_info_stem = create_scaled_bitmap("advanced_upload_info_stem", this, 5);
        m_info_dot = create_scaled_bitmap("advanced_upload_info_dot", this, 1);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
            dc.SetFont(figma_font(this, 14, wxFONTWEIGHT_MEDIUM));
            dc.SetTextForeground(wxColour(255, 255, 255, 230));
            dc.DrawText(t("生成模型"), 0, 0);
            const int icon_x = FromDIP(60);
            dc.DrawBitmap(m_info_bg, icon_x, FromDIP(2), true);
            dc.DrawBitmap(m_info_outline, icon_x + FromDIP(2), FromDIP(4), true);
            dc.DrawBitmap(m_info_stem, icon_x + FromDIP(7), FromDIP(7), true);
            dc.DrawBitmap(m_info_dot, icon_x + FromDIP(7), FromDIP(13), true);
        });
    }
private:
    wxBitmap m_info_bg, m_info_outline, m_info_stem, m_info_dot;
};

class AdvancedPromptTextCtrl final : public wxTextCtrl {
public:
    explicit AdvancedPromptTextCtrl(wxWindow* parent, const wxSize& size)
    {
        Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size,
               wxTE_MULTILINE | wxTE_NO_VSCROLL | wxBORDER_NONE);
#ifdef __WXMSW__
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || event.GetWheelDelta() <= 0) {
                event.Skip();
                return;
            }
            m_wheel_rotation += event.GetWheelRotation();
            const int steps = m_wheel_rotation / event.GetWheelDelta();
            m_wheel_rotation %= event.GetWheelDelta();
            if (event.IsPageScroll()) {
                for (int i = 0; i < std::abs(steps); ++i)
                    ::SendMessageW(GetHandle(), EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            } else if (steps != 0) {
                ::SendMessageW(GetHandle(), EM_LINESCROLL, 0, -steps * event.GetLinesPerAction());
            }
        });
#endif
    }

    void set_hint_colour(const wxColour& colour) { m_hint_colour = colour; }

    void set_persistent_hint(const wxString& hint)
    {
#ifdef __WXMSW__
        m_persistent_hint = hint;
        Bind(wxEVT_TEXT, [this](wxCommandEvent& event) { Refresh(false); event.Skip(); });
#else
        SetHint(hint);
#endif
    }
    bool SetForegroundColour(const wxColour& colour) override
    {
        return wxTextCtrl::SetForegroundColour(m_hint_colour.IsOk() && colour == *wxLIGHT_GREY
            ? m_hint_colour : colour);
    }

#ifdef __WXMSW__
    WXDWORD MSWGetStyle(long style, WXDWORD* exstyle) const override
    {
        return wxTextCtrl::MSWGetStyle(style, exstyle) | ES_AUTOVSCROLL;
    }

    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wparam, WXLPARAM lparam) override
    {
        if (m_persistent_hint.empty()) return wxTextCtrl::MSWWindowProc(message, wparam, lparam);
        if (message == WM_IME_STARTCOMPOSITION) {
            m_composing = true;
            Refresh(false);
        }
        const auto result = wxTextCtrl::MSWWindowProc(message, wparam, lparam);
        if (message == WM_IME_ENDCOMPOSITION) {
            m_composing = false;
            Refresh(false);
        }
        if (message == WM_PAINT && !m_composing && IsEmpty()) {
            // Paint after the native multiline edit fills its own client area.
            // A sibling overlay is covered by the focused edit on Windows.
            RECT bounds {};
            ::SendMessageW(GetHandle(), EM_GETRECT, 0, reinterpret_cast<LPARAM>(&bounds));
            wxClientDC dc(this);
            dc.SetFont(GetFont());
            dc.SetTextForeground(m_hint_colour);
            const int width = std::max(0, int(bounds.right - bounds.left));
            dc.DrawText(wxControl::Ellipsize(m_persistent_hint, dc, wxELLIPSIZE_END, width), bounds.left, bounds.top);
        }
        return result;
    }
#endif

private:
#ifdef __WXMSW__
    int m_wheel_rotation {0};
    bool m_composing {false};
    wxString m_persistent_hint;
#endif
    wxColour m_hint_colour;
};

wxFont advanced_provider_status_font(wxWindow* window)
{
    auto font = smart_creation_font(12, wxFONTWEIGHT_MEDIUM);
    font.SetPointSize(9);
    return font;
}

wxSize advanced_provider_status_size(wxWindow* window, const wxString& caption, bool ready)
{
    wxClientDC dc(window);
    dc.SetFont(advanced_provider_status_font(window));
    return wxSize(std::max(window->FromDIP(70), dc.GetTextExtent(caption).x +
                          window->FromDIP(16 + (ready ? 18 : 0))), window->FromDIP(22));
}

void paint_advanced_provider_status(wxDC& dc, wxGraphicsContext& gc, wxWindow* window,
                                    const wxPoint& position, const wxString& caption, bool ready)
{
    const wxSize size = advanced_provider_status_size(window, caption, ready);
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.SetBrush(wxBrush(wxColour(40, 40, 41)));
    gc.DrawRoundedRectangle(position.x, position.y, size.x, size.y, size.y / 2.0);
    int text_x = position.x + window->FromDIP(8);
    if (ready) {
        static const wxImage icon(wxString::FromUTF8(
            Slic3r::resources_dir() + "/images/redesign_provider_configured.png"));
        const int icon_size = window->FromDIP(14);
        if (icon.IsOk())
            gc.DrawBitmap(wxBitmap(icon.Scale(icon_size, icon_size, wxIMAGE_QUALITY_HIGH)),
                          text_x, position.y + (size.y - icon_size) / 2.0, icon_size, icon_size);
        text_x += window->FromDIP(18);
    }
    dc.SetFont(advanced_provider_status_font(window));
    dc.SetTextForeground(wxColour(204, 204, 204));
    dc.DrawText(caption, text_x, position.y + (size.y - dc.GetTextExtent(caption).y) / 2);
}

class AdvancedChoicePopup;

class AdvancedChoicePicker final : public wxPanel {
public:
    using SelectionChanged = std::function<void(int)>;

    AdvancedChoicePicker(wxWindow* parent, wxArrayString choices, SelectionChanged on_selection, int height = 48)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(height)))
        , m_choices(std::move(choices)), m_on_selection(std::move(on_selection)), m_height(height)
    {
        SetMinSize(wxSize(-1, FromDIP(m_height)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(panel_colour());
        style_smart_creation_text(this, 14, primary_text_colour());
        SetCanFocus(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) { m_hovered = true; Refresh(); event.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = false; m_pressed = false; Refresh(); event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (!IsEnabled() || !m_interactive) return;
            m_pressed = true; SetFocus(); Refresh(); event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled() || !m_interactive) return;
            m_pressed = false; SetFocus(); Refresh(); toggle_popup();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (!IsEnabled() || !m_interactive) { event.Skip(); return; }
            switch (event.GetKeyCode()) {
            case WXK_UP: set_selection(std::max(0, m_selection - 1)); break;
            case WXK_DOWN: set_selection(std::min(static_cast<int>(m_choices.size()) - 1, m_selection + 1)); break;
            case WXK_RETURN:
            case WXK_SPACE: toggle_popup(); break;
            case WXK_ESCAPE: dismiss_popup(); break;
            case WXK_TAB: dismiss_popup(); HandleAsNavigationKey(event); break;
            default: event.Skip(); break;
            }
        });
    }

    int selection() const { return m_selection; }
    void set_choices(wxArrayString choices, int selection);
    void set_status(const wxString& caption, bool ready);
    void set_placeholder(wxString placeholder) { m_placeholder = std::move(placeholder); Refresh(); }
    void set_interactive(bool interactive) { m_interactive = interactive; Refresh(); }
    void set_selection(int selection, bool notify = true)
    {
        if (selection < 0 || selection >= static_cast<int>(m_choices.size())) return;
        if (m_selection == selection) return;
        m_selection = selection;
        Refresh();
        if (notify && m_on_selection) m_on_selection(m_selection);
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        const bool active = IsEnabled() && m_interactive;
        const wxColour face = !active ? wxColour(40, 40, 43) : m_pressed ? wxColour(47, 47, 51) :
                              m_hovered ? wxColour(35, 35, 39) : control_colour();
        gc->SetPen(FindFocus() == this ? wxPen(accent_colour(), std::max(1, FromDIP(1))) : *wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(face));
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(10));
        dc.SetFont(GetFont());
        dc.SetTextForeground(active ? primary_text_colour() : wxColour(255, 255, 255, 82));
        const int text_x = FromDIP(16);
        if (m_selection >= 0 && m_selection < static_cast<int>(m_choices.size())) {
            const wxSize extent = dc.GetTextExtent(m_choices[m_selection]);
            dc.DrawText(m_choices[m_selection], text_x, (size.y - extent.y) / 2);
            if (!m_status_caption.empty()) {
                const wxSize badge_size = advanced_provider_status_size(this, m_status_caption, m_status_ready);
                paint_advanced_provider_status(dc, *gc, this,
                    wxPoint(text_x + extent.x + FromDIP(12), (size.y - badge_size.y) / 2),
                    m_status_caption, m_status_ready);
            }
        } else if (!m_placeholder.empty()) {
            const wxSize extent = dc.GetTextExtent(m_placeholder);
            dc.DrawText(m_placeholder, text_x, (size.y - extent.y) / 2);
        }
        const int arrow_x = size.x - FromDIP(20), centre_y = size.y / 2;
        dc.SetPen(wxPen(active ? secondary_text_colour() : wxColour(255, 255, 255, 70),
                        std::max(1, FromDIP(1))));
        dc.DrawLine(arrow_x - FromDIP(4), centre_y - FromDIP(2), arrow_x, centre_y + FromDIP(2));
        dc.DrawLine(arrow_x, centre_y + FromDIP(2), arrow_x + FromDIP(4), centre_y - FromDIP(2));
    }
    void toggle_popup();
    void dismiss_popup();
    void select_from_popup(int selection) { set_selection(selection); }

    wxArrayString m_choices;
    wxString m_placeholder;
    wxString m_status_caption;
    bool m_status_ready {false};
    SelectionChanged m_on_selection;
    int m_selection {0};
    AdvancedChoicePopup* m_popup {nullptr};
    bool m_interactive {true};
    bool m_hovered {false};
    bool m_pressed {false};
    int m_height {48};
    friend class AdvancedChoicePopup;
};

class AdvancedChoicePopup final : public wxPopupTransientWindow {
public:
    AdvancedChoicePopup(AdvancedChoicePicker* owner, const wxArrayString& choices)
        : wxPopupTransientWindow(owner, wxBORDER_NONE), m_owner(owner)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT); SetBackgroundColour(wxColour(38, 38, 38));
        auto* content = new wxBoxSizer(wxVERTICAL); content->AddSpacer(owner->FromDIP(10));
        for (std::size_t index = 0; index < choices.size(); ++index) {
            auto* row = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, owner->FromDIP(41)));
            row->SetMinSize(wxSize(-1, owner->FromDIP(41)));
            const bool selected = static_cast<int>(index) == owner->selection();
            const wxColour row_face = selected ? wxColour(56, 52, 39) : wxColour(38, 38, 38);
            row->SetBackgroundColour(row_face);
            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* row_label = new wxStaticText(row, wxID_ANY, choices[index]);
            row_label->SetBackgroundColour(row_face);
            style_smart_creation_text(row_label, 14, selected ? accent_colour() : primary_text_colour());
            const bool show_status = selected && !owner->m_status_caption.empty();
            row_sizer->Add(row_label, show_status ? 0 : 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(16));
            wxPanel* badge = nullptr;
            if (show_status) {
                badge = new wxPanel(row, wxID_ANY, wxDefaultPosition,
                    advanced_provider_status_size(owner, owner->m_status_caption, owner->m_status_ready));
                badge->SetBackgroundStyle(wxBG_STYLE_PAINT); badge->SetBackgroundColour(row_face);
                badge->Bind(wxEVT_PAINT, [badge, caption = owner->m_status_caption,
                                         ready = owner->m_status_ready](wxPaintEvent&) {
                    wxAutoBufferedPaintDC dc(badge); dc.SetBackground(wxBrush(badge->GetBackgroundColour())); dc.Clear();
                    auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
                    if (gc) paint_advanced_provider_status(dc, *gc, badge, wxPoint(0, 0), caption, ready);
                });
                const int selection = static_cast<int>(index);
                badge->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
                row_sizer->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(12));
            }
            row->SetSizer(row_sizer);
            const int selection = static_cast<int>(index);
            row->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            row_label->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            auto set_hover = [row, row_label, badge, selected](bool hover) {
                const wxColour face = hover ? wxColour(53, 53, 57) :
                                      selected ? wxColour(56, 52, 39) : wxColour(38, 38, 38);
                row->SetBackgroundColour(face); row_label->SetBackgroundColour(face);
                row->Refresh(); row_label->Refresh();
                if (badge) { badge->SetBackgroundColour(face); badge->Refresh(); }
            };
            for (wxWindow* target : {static_cast<wxWindow*>(row), static_cast<wxWindow*>(row_label), static_cast<wxWindow*>(badge)}) {
                if (!target) continue;
                target->Bind(wxEVT_ENTER_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(true); event.Skip(); });
                target->Bind(wxEVT_LEAVE_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(false); event.Skip(); });
            }
            content->Add(row, 0, wxEXPAND);
        }
        content->AddSpacer(owner->FromDIP(10)); SetSizer(content);
        SetSize(wxSize(owner->GetSize().x, owner->FromDIP(20 + static_cast<int>(choices.size()) * 41)));
        Bind(wxEVT_PAINT, [this, owner](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(wxColour(38, 38, 38))); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(wxColour(38, 38, 38)));
            const wxSize size = GetClientSize(); gc->DrawRoundedRectangle(0, 0, size.x, size.y, owner->FromDIP(12));
        });
    }
private:
    void choose(int selection) { auto* owner = m_owner; Dismiss(); owner->select_from_popup(selection); }
    AdvancedChoicePicker* m_owner;
};

void AdvancedChoicePicker::toggle_popup()
{
    if (m_choices.empty()) return;
    if (m_popup) {
        if (m_popup->IsShown()) { m_popup->Dismiss(); return; }
        m_popup->Destroy(); m_popup = nullptr;
    }
    m_popup = new AdvancedChoicePopup(this, m_choices);
    m_popup->Position(ClientToScreen(wxPoint(0, GetClientSize().y + FromDIP(12))), wxSize(0, 0));
    m_popup->Popup(this);
}

void AdvancedChoicePicker::dismiss_popup()
{
    if (m_popup && m_popup->IsShown()) m_popup->Dismiss();
}

void AdvancedChoicePicker::set_choices(wxArrayString choices, int selection)
{
    const int next=choices.empty() ? -1 : std::clamp(selection,0,static_cast<int>(choices.size())-1);
    if (m_choices==choices && m_selection==next) return;
    if (m_choices != choices && m_popup) { m_popup->Destroy(); m_popup = nullptr; }
    m_choices = std::move(choices);
    m_selection = next;
    Refresh();
}

void AdvancedChoicePicker::set_status(const wxString& caption, bool ready)
{
    if (m_status_caption == caption && m_status_ready == ready) return;
    if (m_popup) { m_popup->Destroy(); m_popup = nullptr; }
    m_status_caption = caption; m_status_ready = ready; Refresh();
}

class AdvancedStageTabs final : public wxPanel {
public:
    explicit AdvancedStageTabs(wxWindow* parent, std::function<void(bool)> select)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(312, 40))), m_select(std::move(select))
    {
        SetMinSize(parent->FromDIP(wxSize(312, 40))); SetBackgroundStyle(wxBG_STYLE_PAINT); SetCanFocus(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) { set_model_tab(event.GetX() >= GetClientSize().x / 2, true); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_LEFT) set_model_tab(false, true);
            else if (event.GetKeyCode() == WXK_RIGHT) set_model_tab(true, true);
            else event.Skip();
        });
    }
    void set_model_tab(bool model, bool notify = false)
    {
        if (m_model == model) return;
        m_model = model; Refresh();
        if (notify && m_select) m_select(model);
    }
private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(panel_colour())); dc.Clear();
        const auto size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
        const wxColour base = black_overlay(panel_colour(), 102);
        gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(base));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
        const int half = size.x / 2, inset = FromDIP(3);
        const int selected_x = m_model ? half : inset;
        const int selected_w = m_model ? size.x - half - inset : half - inset;
        gc->SetBrush(wxBrush(smart_creation_opaque_colour(wxColour(255, 255, 255, 38), base)));
        gc->DrawRoundedRectangle(selected_x, inset, selected_w, size.y - inset * 2, FromDIP(10));
        dc.SetFont(smart_creation_font(14, wxFONTWEIGHT_MEDIUM));
        const std::array<wxString, 2> labels {t("生成2D设计图"), t("生成3D模型")};
        for (int i = 0; i < 2; ++i) {
            const bool selected = (i == 1) == m_model;
            dc.SetTextForeground(selected ? *wxWHITE :
                smart_creation_opaque_colour(wxColour(255, 255, 255, 115), base));
            const auto extent = dc.GetTextExtent(labels[i]);
            dc.DrawText(labels[i], i * half + (half - extent.x) / 2, (size.y - extent.y) / 2);
        }
    }
    std::function<void(bool)> m_select;
    bool m_model {false};
};

class UploadSlot final : public wxPanel {
public:
    UploadSlot(wxWindow* parent, ViewId view, std::function<void()> choose, std::function<void()> remove)
        : wxPanel(parent), m_view(view), m_choose(std::move(choose)), m_remove(std::move(remove)), m_preview(this)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        if (view == ViewId::Left) m_direction_icon = create_scaled_bitmap("advanced_upload_left", this, 16);
        if (view == ViewId::Back) {
            const wxImage source = create_scaled_bitmap("advanced_upload_back", this, 16).ConvertToImage();
            if (source.IsOk()) m_direction_icon = wxBitmap(source.Rotate90(true));
        }
        if (view == ViewId::Right) m_direction_icon = create_scaled_bitmap("advanced_upload_right", this, 16);
        if (view == ViewId::Front) m_upload_icon = create_scaled_bitmap("advanced_upload_action", this, 16);
        m_remove_bg = create_scaled_bitmap("advanced_upload_remove_bg", this, 18);
        const wxImage remove_source = create_scaled_bitmap("advanced_upload_remove_x", this, 11).ConvertToImage();
        if (remove_source.IsOk()) {
            const wxPoint center(remove_source.GetWidth() / 2, remove_source.GetHeight() / 2);
            m_remove_x = wxBitmap(remove_source.Rotate(std::acos(-1.0) / 4.0, center, true));
        }
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { prepare_image(); Refresh(false); event.Skip(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (!m_editable) return;
            if (!m_path.empty() && remove_rect().Contains(event.GetPosition())) m_remove();
            else if (slot_rect().Contains(event.GetPosition())) m_choose();
        });
        update_tooltip();
    }

    void display(const std::string& path, bool editable)
    {
        // A snapshot may precede its cache download. The asynchronous channel
        // deduplicates installed/pending keys and retries failed keys, retaining
        // the previous complete bitmap without reading the file on the GUI.
        const bool changed = path != m_path;
        m_path = path;
        prepare_image();
        if (!changed && m_editable == editable && path == m_painted_path) return;
        m_editable = editable;
        m_painted_path = path;
        SetCursor(wxCursor(editable ? wxCURSOR_HAND : wxCURSOR_ARROW));
        update_tooltip();
        Refresh();
    }

    void set_active(bool active) { m_preview.set_active(active); if (active) prepare_image(); }
    void invalidate_image() { ++m_image_revision; prepare_image(); }
private:
    void prepare_image() {
        const auto slot=slot_rect();
        m_preview.request({m_path, std::to_string(m_image_revision), slot.GetSize(), int(std::lround(96*GetDPIScaleFactor())), FromDIP(8), true});
    }
    wxRect slot_rect() const
    {
        const int top_overflow = FromDIP(m_view == ViewId::Front ? 6 : 5);
        const int right_overflow = FromDIP(6);
        const wxSize size = GetClientSize();
        return wxRect(0, top_overflow, std::max(0, size.x - right_overflow),
                      std::max(0, size.y - top_overflow));
    }

    wxRect remove_rect() const
    {
        const wxRect slot = slot_rect();
        const int size = FromDIP(18);
        const int right_overlap = FromDIP(6);
        return wxRect(slot.GetRight() - size + right_overlap + 1, 0, size, size);
    }

    wxString label() const
    {
        if (m_view == ViewId::Front) return t("主视图");
        if (m_view == ViewId::Left) return t("左视图");
        if (m_view == ViewId::Back) return t("后视图");
        return t("右视图");
    }

    void update_tooltip()
    {
        if (!m_editable) SetToolTip(t("当前生成任务进行中，暂不可修改图片。"));
        else if (m_path.empty()) SetToolTip(t("点击或拖入") + label());
        else SetToolTip(t("点击替换图片；悬停后点击右上角可移除。"));
    }

    void draw_empty(wxDC& dc, wxGraphicsContext& gc)
    {
        const wxRect slot = slot_rect();
        const wxBitmap& icon = m_view == ViewId::Front ? m_upload_icon : m_direction_icon;
        const int content_height = FromDIP(36);
        const int icon_y = slot.y + (slot.height - content_height) / 2;
        if (icon.IsOk()) {
            gc.DrawBitmap(icon, slot.x + (slot.width - FromDIP(16)) / 2.0,
                          icon_y, FromDIP(16), FromDIP(16));
        }
        dc.SetFont(figma_font(this, 13));
        dc.SetTextForeground(wxColour(255, 255, 255, 204));
        const wxString caption = label();
        dc.DrawText(caption, slot.x + (slot.width - dc.GetTextExtent(caption).x) / 2,
                    icon_y + FromDIP(20));
    }

    void draw_image(wxGraphicsContext& gc)
    {
        const wxRect slot = slot_rect();
        const auto& bitmap=m_preview.bitmap();
        if (bitmap.IsOk()) gc.DrawBitmap(bitmap, slot.x, slot.y, slot.width, slot.height);
    }

    void draw_remove(wxGraphicsContext& gc)
    {
        if (!m_editable || !m_remove_bg.IsOk()) return;
        const wxRect button = remove_rect();
        gc.DrawBitmap(m_remove_bg, button.x, button.y, button.width, button.height);
        if (m_remove_x.IsOk()) {
            const int x = button.x + (button.width - m_remove_x.GetWidth()) / 2;
            const int y = button.y + (button.height - m_remove_x.GetHeight()) / 2;
            gc.DrawBitmap(m_remove_x, x, y, m_remove_x.GetWidth(), m_remove_x.GetHeight());
        }
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(upload_outer_colour()));
        dc.Clear();
        const auto size = GetClientSize();
        if (size.x < 2 || size.y < 2) return;
        const wxRect slot = slot_rect();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetBrush(wxBrush(upload_slot_colour()));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(slot.x, slot.y, slot.width, slot.height, FromDIP(8));
        if (!m_path.empty() && m_preview.bitmap().IsOk()) draw_image(*gc);
        else draw_empty(dc, *gc);
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(wxColour(255, 255, 255, 77), std::max(1, FromDIP(1)), wxPENSTYLE_SHORT_DASH));
        gc->DrawRoundedRectangle(slot.x + 0.5, slot.y + 0.5, slot.width - 1.0, slot.height - 1.0, FromDIP(8));
        if (!m_path.empty() && m_preview.bitmap().IsOk()) draw_remove(*gc);
    }

    ViewId m_view;
    std::function<void()> m_choose;
    std::function<void()> m_remove;
    std::string m_path;
    std::string m_painted_path;
    std::uint64_t m_image_revision {0};
    Presentation::AsyncPreviewImage m_preview;
    wxBitmap m_direction_icon;
    wxBitmap m_upload_icon;
    wxBitmap m_remove_bg;
    wxBitmap m_remove_x;
    bool m_editable {true};
};

class UploadAreaPanel final : public wxPanel {
public:
    UploadAreaPanel(wxWindow* parent, std::function<void(ViewId)> choose,
                    std::function<void(ViewId)> remove, std::function<void(ViewId, wxString)> accept)
        : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(312, 240)));
        SetWindowStyleFlag(GetWindowStyleFlag() | wxCLIP_CHILDREN);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        for (auto view : AllViews) {
            auto* slot = new UploadSlot(this, view, [choose, view] { choose(view); }, [remove, view] { remove(view); });
            slot->SetName(t("advanced-upload-") + t(view_name(view)));
            slot->SetDropTarget(new ImageDrop([accept, view](wxString path) { accept(view, std::move(path)); }));
            m_slots[index(view)] = slot;
        }
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { layout_slots(); event.Skip(); });
    }

    void display(ViewId view, const std::string& path, bool editable)
    {
        m_slots[index(view)]->display(path, editable);
    }
    void set_active(bool active) { for (auto* slot:m_slots) slot->set_active(active); }
    void invalidate_image(ViewId view) { m_slots[index(view)]->invalidate_image(); }

private:
    int dip(double value) const { return static_cast<int>(std::lround(value * GetDPIScaleFactor())); }

    void layout_slots()
    {
        const auto size = GetClientSize();
        const auto dpi = GetDPIScaleFactor();
        if (size == m_layout_size && dpi == m_layout_dpi) return;
        m_layout_size = size; m_layout_dpi = dpi;
        m_slots[index(ViewId::Front)]->SetSize(dip(8), dip(2), dip(150), dip(114));
        m_slots[index(ViewId::Left)]->SetSize(dip(160), dip(3), dip(150), dip(113));
        m_slots[index(ViewId::Back)]->SetSize(dip(8), dip(119), dip(150), dip(113));
        m_slots[index(ViewId::Right)]->SetSize(dip(160), dip(119), dip(150), dip(113));
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour()));
        dc.Clear();
        const auto size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(upload_outer_colour()));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
    }

    std::array<UploadSlot*, 4> m_slots {};
    wxSize m_layout_size;
    double m_layout_dpi {0};
};

// The text endpoint accepts PNG/JPEG. Keep an advanced-only decoded PNG alive
// until the asynchronous request finishes, including cancellation callbacks.
struct PolishReference {
    wxString path;
    ~PolishReference() { if (!path.empty()) wxRemoveFile(path); }
};

class DesignIconButton final : public wxControl {
public:
    DesignIconButton(wxWindow* parent, std::string icon, int size, int icon_height,
                     wxColour face = wxColour(0, 0, 0, 166), std::string active_icon = {})
        : wxControl(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(size, size)), wxBORDER_NONE),
          m_icon(std::move(icon)), m_active_icon(std::move(active_icon)), m_icon_height(icon_height), m_face(face) {
        SetBackgroundStyle(wxBG_STYLE_PAINT); SetBackgroundColour(parent->GetBackgroundColour());
        SetMinSize(FromDIP(wxSize(size, size))); SetCursor(wxCursor(wxCURSOR_HAND));
        reload();
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) { reload(); Refresh(); event.Skip(); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
            const auto size = GetClientSize();
            gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(m_face));
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(6));
            const auto& icon = m_selected && m_active_bitmap.IsOk() ? m_active_bitmap : m_bitmap;
            if (icon.IsOk()) {
                const double height = FromDIP(m_icon_height), width = height * icon.GetWidth() / icon.GetHeight();
                gc->DrawBitmap(icon, (size.x - width) / 2, (size.y - height) / 2, width, height);
            }
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& mouse) {
            if (!IsEnabled() || (m_activation_guard && !m_activation_guard(mouse))) return;
            wxCommandEvent event(wxEVT_BUTTON, GetId()); event.SetEventObject(this); ProcessWindowEvent(event);
        });
        Bind(wxEVT_LEFT_DCLICK, [](wxMouseEvent&) {});
    }
    void set_selected(bool selected) { if (m_selected != selected) { m_selected = selected; Refresh(false); } }
    void set_activation_guard(std::function<bool(const wxMouseEvent&)> guard) { m_activation_guard = std::move(guard); }
private:
    void reload() {
        m_bitmap = create_scaled_bitmap("figma-ux/advanced-2d-" + m_icon, this, m_icon_height);
        if (m_bitmap.IsOk() && (m_icon == "local-close" || m_icon == "local-send")) {
            auto image = m_bitmap.ConvertToImage();
            if (m_icon == "local-send") image = image.Rotate90(true);
            if (!image.HasAlpha()) image.InitAlpha();
            auto* alpha = image.GetAlpha();
            for (int pixel = 0; pixel < image.GetWidth() * image.GetHeight(); ++pixel)
                alpha[pixel] = static_cast<unsigned char>(alpha[pixel] * 204 / 255);
            m_bitmap = wxBitmap(image);
        }
        if (!m_active_icon.empty()) m_active_bitmap = create_scaled_bitmap("figma-ux/advanced-2d-" + m_active_icon, this, m_icon_height);
    }
    std::string m_icon, m_active_icon;
    int m_icon_height;
    wxColour m_face;
    wxBitmap m_bitmap, m_active_bitmap;
    std::function<bool(const wxMouseEvent&)> m_activation_guard;
    bool m_selected {false};
};

// The viewport owns the pixels and hit testing. Each thumbnail retains its own
// replaceable image request so switching views does not reload the whole strip.
class PreviewThumbnail {
public:
    explicit PreviewThumbnail(wxWindow* owner) : m_preview(owner) {}

    void display(const std::string& path, const std::string& revision, wxSize size, int dpi, int radius)
    {
        m_path = path;
        m_revision = revision;
        m_preview.request({path, revision, size, dpi, radius, true, Presentation::PreviewCropAlignment::Center});
    }

    void set_active(bool active) { m_preview.set_active(active); }
    const wxBitmap& bitmap() const { return m_preview.bitmap_for(m_path, m_revision, Presentation::PreviewCropAlignment::Center); }

private:
    Presentation::AsyncPreviewImage m_preview;
    std::string m_path, m_revision;
};

class LocalDesignPopover final : public wxPopupWindow {
public:
    LocalDesignPopover(wxWindow* parent, std::function<void()> close, std::function<void(std::string)> send)
        : wxPopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS | wxCLIP_CHILDREN) {
        SetSize(parent->FromDIP(wxSize(360,164)));
        SetName("advanced-local-design-popover"); SetBackgroundStyle(wxBG_STYLE_PAINT); SetBackgroundColour(wxColour(23,23,23));
        m_star = new DesignIconButton(this,"local-header-star",28,16,wxColour(0,0,0,115)); m_star->Enable(false);
        m_close = new DesignIconButton(this,"local-close",24,11,wxColour(23,23,23)); m_close->SetName("advanced-local-design-close");
        m_send = new DesignIconButton(this,"local-send",20,19,wxColour(14,14,14)); m_send->SetName("advanced-local-design-send");
        auto* text = new AdvancedPromptTextCtrl(this, wxDefaultSize); m_text = text;
        m_text->SetName("advanced-local-design-text"); m_text->SetBackgroundColour(wxColour(14,14,14));
        style_smart_creation_text(m_text,14,wxColour(255,255,255,230));
        text->set_hint_colour(smart_creation_opaque_colour(wxColour(255,255,255,115), wxColour(14,14,14)));
        text->set_persistent_hint(t("描述你的需求，需求越详细，生成效果越好哦"));
        m_close->Bind(wxEVT_BUTTON,[close](wxCommandEvent&) { if (close) close(); });
        m_send->Bind(wxEVT_BUTTON,[this,send](wxCommandEvent&) { if (send) send(m_text->GetValue().ToUTF8().data()); });
        Bind(wxEVT_SIZE,[this](wxSizeEvent& event) { layout(); event.Skip(); });
        Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
            const auto size = GetClientSize();
            gc->SetBrush(wxBrush(wxColour(23,23,23))); gc->SetPen(wxPen(wxColour(47,47,47),FromDIP(1)));
            gc->DrawRoundedRectangle(.5,.5,size.x-1,size.y-1,FromDIP(8));
            gc->SetBrush(wxBrush(wxColour(14,14,14)));
            gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(wxColour(255,255,255,26)).Width(FromDIP(1)*1.3)));
            gc->DrawRoundedRectangle(FromDIP(8),FromDIP(52),size.x-FromDIP(16),size.y-FromDIP(64),FromDIP(8));
        });
        Hide(); layout();
    }
    void clear() {
        Hide();
        m_text->ChangeValue(wxEmptyString);
        set_busy(false);
        m_text->Refresh(false);
    }
    void focus_input() { m_text->SetFocus(); }
    void set_busy(bool busy) {
        if (m_text->IsEditable() == busy) m_text->SetEditable(!busy);
        if (m_send->IsEnabled() == busy) m_send->Enable(!busy);
    }
private:
    void layout() {
        const auto size = GetClientSize();
        m_star->SetSize(FromDIP(8),FromDIP(12),FromDIP(28),FromDIP(28));
        m_close->SetSize(size.x-FromDIP(36),FromDIP(14),FromDIP(24),FromDIP(24));
        m_text->SetSize(FromDIP(20),FromDIP(64),std::max(0,size.x-FromDIP(40)),std::max(0,size.y-FromDIP(108)));
        m_send->SetSize(size.x-FromDIP(40),size.y-FromDIP(44),FromDIP(20),FromDIP(20));
        m_send->Raise();
    }
    DesignIconButton *m_star, *m_close, *m_send;
    wxTextCtrl* m_text;
};

class DesignDetailPreview final : public wxPanel {
public:
    DesignDetailPreview(wxWindow* parent, std::string path, std::string revision)
        : wxPanel(parent), m_preview(this), m_path(std::move(path)), m_revision(std::move(revision))
    {
        SetName("advanced-design-detail-preview");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            const auto size = GetClientSize();
            m_offset.m_x += (size.x - m_viewport.x) * 0.5;
            m_offset.m_y += (size.y - m_viewport.y) * 0.5;
            m_viewport = size;
            clamp_offset();
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) { zoom_at(event); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (m_zoom <= 1.0 || !m_preview.bitmap().IsOk() || HasCapture()) return;
            m_pointer = event.GetPosition();
            CaptureMouse();
            SetCursor(wxCursor(wxCURSOR_SIZING));
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!HasCapture()) return;
            if (!event.LeftIsDown()) { end_drag(); return; }
            const auto pointer = event.GetPosition();
            m_offset.m_x += pointer.x - m_pointer.x;
            m_offset.m_y += pointer.y - m_pointer.y;
            m_pointer = pointer;
            clamp_offset();
            Refresh(false);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { end_drag(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { SetCursor(wxNullCursor); });
    }

    ~DesignDetailPreview() override { end_drag(); }

    void load()
    {
        // Keep one bounded bitmap at up to source resolution. Pointer input only redraws it.
        Presentation::PreviewImageRequest request;
        request.path = m_path;
        request.revision = m_revision;
        request.bounds = wxSize(3072, 3072);
        request.limit_to_source = true;
        m_preview.request(std::move(request));
    }

private:
    struct ImageExtent { double width {0.0}; double height {0.0}; };

    ImageExtent image_size() const
    {
        const auto& bitmap = m_preview.bitmap();
        const auto size = GetClientSize();
        if (!bitmap.IsOk() || size.x <= 0 || size.y <= 0) return {};
        const double scale = m_zoom * std::min(double(size.x) / bitmap.GetWidth(), double(size.y) / bitmap.GetHeight());
        return {bitmap.GetWidth() * scale, bitmap.GetHeight() * scale};
    }

    void clamp_offset()
    {
        const auto image = image_size();
        const auto size = GetClientSize();
        const auto position = [](double offset, double spare) {
            return spare >= 0 ? spare * 0.5 : std::clamp(offset, spare, 0.0);
        };
        m_offset.m_x = position(m_offset.m_x, size.x - image.width);
        m_offset.m_y = position(m_offset.m_y, size.y - image.height);
    }

    void zoom_at(const wxMouseEvent& event)
    {
        if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || event.GetWheelDelta() <= 0 || !m_preview.bitmap().IsOk()) return;
        const double next = std::clamp(m_zoom * std::pow(1.2, double(event.GetWheelRotation()) / event.GetWheelDelta()), 1.0, 4.0);
        if (next == m_zoom) return;
        clamp_offset();
        const auto pointer = event.GetPosition();
        const double ratio = next / m_zoom;
        m_offset.m_x = pointer.x - (pointer.x - m_offset.m_x) * ratio;
        m_offset.m_y = pointer.y - (pointer.y - m_offset.m_y) * ratio;
        m_zoom = next;
        clamp_offset();
        Refresh(false);
    }

    void end_drag()
    {
        if (HasCapture()) ReleaseMouse();
        SetCursor(wxNullCursor);
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour()));
        dc.Clear();
        const auto& bitmap = m_preview.bitmap();
        if (!bitmap.IsOk()) return;
        auto context = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!context) return;
        const auto size = image_size();
        clamp_offset();
        context->SetInterpolationQuality(wxINTERPOLATION_GOOD);
        context->DrawBitmap(bitmap, m_offset.m_x, m_offset.m_y, size.width, size.height);
    }

    Presentation::AsyncPreviewImage m_preview;
    std::string m_path, m_revision;
    wxSize m_viewport {0, 0};
    wxPoint m_pointer;
    wxPoint2DDouble m_offset {0.0, 0.0};
    double m_zoom {1.0};
};

// Rendering only. The Session owns selection and the click/double-click state machine.
class ViewCard final : public wxPanel {
public:
    ViewCard(wxWindow* parent, wxString caption, bool upload = false)
        : wxPanel(parent), m_caption(std::move(caption)), m_preview(this), m_loading_timer(this), m_upload(upload) {
        SetMinSize(wxSize(0, 0)); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour()); SetCursor(wxCursor(wxCURSOR_HAND));
        style_text(this, primary_text_colour(), 11);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            update_layout();
            prepare_image();
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) { update_hover(true, event.GetPosition()); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { if (m_design_preview) update_hover(true, event.GetPosition()); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { update_hover(false, wxDefaultPosition); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (!m_design_preview) { event.Skip(); return; }
            update_hover(true, event.GetPosition());
            m_pressed = hit(event.GetPosition());
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (!m_design_preview) {
                if (m_card_click) m_card_click(); else event.Skip();
                return;
            }
            const auto pressed = std::exchange(m_pressed, Hit::None);
            const wxPoint point = event.GetPosition();
            update_hover(true, point);
            m_last_released = hit(point) == pressed ? pressed : Hit::None;
            if (m_last_released == Hit::None) return;
            if (pressed == Hit::Picture) {
                if (m_picture_selectable && m_card_click) m_card_click();
            } else if (pressed == Hit::Regenerate) {
                if (m_local_enabled && m_local_action && accept_control(event)) m_local_action();
            } else if (pressed >= Hit::FirstCheckbox) {
                const auto i = static_cast<int>(pressed) - static_cast<int>(Hit::FirstCheckbox);
                if (m_thumbnail_checks_enabled[i] && m_thumbnail_selection_action && m_checkbox_guard(event))
                    m_thumbnail_selection_action(AllViews[i]);
            } else if (m_thumbnail_action && accept_control(event)) {
                m_thumbnail_action(AllViews[static_cast<int>(pressed) - static_cast<int>(Hit::FirstThumbnail)]);
            }
        });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& event) {
            if (m_design_preview) {
                m_pressed = Hit::None;
                update_hover(true, event.GetPosition());
                if (std::exchange(m_last_released, Hit::None) != Hit::Picture ||
                    hit(event.GetPosition()) != Hit::Picture || !accept_control(event)) return;
            }
            if (m_card_double_click) m_card_double_click(); else event.Skip();
        });
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            if (m_design_preview) {
                reload_preview_icons();
                update_layout();
                prepare_image();
                Refresh(false);
            }
            event.Skip();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            m_spinner_phase = (m_spinner_phase + 1) % 24;
            Refresh(false);
        }, m_loading_timer.GetId());
    }
    ~ViewCard() override { m_loading_timer.Stop(); }
    void set_card_actions(std::function<void()> click, std::function<void()> double_click)
    {
        m_card_click = std::move(click);
        m_card_double_click = std::move(double_click);
    }
    void set_local_action(std::function<void()> action) { m_local_action = std::move(action); }
    void set_activation_guard(std::function<bool(const wxMouseEvent&)> guard) { m_activation_guard = std::move(guard); }
    void set_thumbnail_action(std::function<void(ViewId)> action)
    {
        m_thumbnail_action = std::move(action);
        for (auto& thumbnail : m_thumbnails) thumbnail = std::make_unique<PreviewThumbnail>(this);
    }
    void set_picture_selectable(bool selectable)
    {
        if (m_picture_selectable == selectable) return;
        m_picture_selectable = selectable;
        update_hover(m_hover, ScreenToClient(wxGetMousePosition()));
    }
    void set_thumbnail_selection_action(std::function<void(ViewId)> action,
                                        std::function<bool(const wxMouseEvent&)> guard)
    {
        m_thumbnail_selection_action = std::move(action);
        m_checkbox_guard = std::move(guard);
    }
    void set_model_input_preview(bool enabled)
    {
        if (m_model_input_preview == enabled) return;
        m_model_input_preview = enabled;
        clear_pointer();
        Refresh(false);
    }
    void set_thumbnail_selection(const std::array<bool, 4>& selected, const std::array<bool, 4>& enabled)
    {
        if (selected == m_thumbnail_selected && enabled == m_thumbnail_checks_enabled) return;
        m_thumbnail_selected = selected;
        m_thumbnail_checks_enabled = enabled;
        update_hover(m_hover, ScreenToClient(wxGetMousePosition()));
        Refresh(false);
    }
    void set_design_preview(bool enabled)
    {
        if (m_design_preview == enabled) return;
        m_design_preview = enabled;
        clear_pointer();
        SetCursor(wxCursor(enabled ? wxCURSOR_ARROW : wxCURSOR_HAND));
        m_pointer_actionable = false;
        if (enabled) reload_preview_icons();
        update_layout();
        prepare_image();
        Refresh(false);
    }
    void set_caption(const wxString& caption)
    {
        if (m_caption == caption) return;
        m_caption = caption;
        Refresh(false);
    }
    void display_thumbnail(ViewId view, const std::string& path, const std::string& revision, bool selected)
    {
        const auto i = index(view);
        const auto& data = m_thumbnail_data[i];
        if (data.path != path || data.revision != revision) {
            m_thumbnail_data[i] = {path, revision};
            prepare_thumbnail(i);
            Refresh(false);
        }
        if (selected && m_thumbnail_view != view) { m_thumbnail_view = view; Refresh(false); }
    }
    void set_local_available(bool visible, bool enabled)
    {
        if (m_local_visible == visible && m_local_enabled == enabled) return;
        m_local_visible = visible;
        m_local_enabled = enabled;
        Refresh(false);
    }
    wxRect local_button_bounds() const { return m_controls.regenerate; }
    void set_confirmation_caption() { m_confirmation_caption = true; }
    void display(const std::string& path, bool selected, wxString status, bool loading = false, std::string revision = {}) {
        const bool asset_changed = path != m_path || revision != m_revision;
        if (asset_changed) {
            m_path = path;
            m_revision = std::move(revision);
            prepare_image();
        }
        if (!asset_changed && m_selected == selected && m_status == status && m_loading == loading) return;
        m_selected = selected;
        m_status = std::move(status);
        if (m_loading != loading) {
            m_loading = loading;
            m_spinner_phase = 0;
            if (m_loading && m_active) m_loading_timer.Start(80);
            else m_loading_timer.Stop();
        }
        Refresh();
    }
    void set_active(bool active)
    {
        if (m_active != active) {
            m_active = active;
            if (!active) clear_pointer();
        }
        m_preview.set_active(active);
        for (auto& thumbnail : m_thumbnails) if (thumbnail) thumbnail->set_active(active);
        if (active) {
            prepare_image();
            if (m_loading && !m_loading_timer.IsRunning()) m_loading_timer.Start(80);
        } else m_loading_timer.Stop();
    }
    void clear_pointer()
    {
        m_pressed = m_last_released = Hit::None;
        update_hover(false, wxDefaultPosition);
    }
private:
    enum class Hit { None, Picture, Regenerate, FirstThumbnail, FirstCheckbox = FirstThumbnail + 4 };

    void reload_preview_icons()
    {
        m_star = create_scaled_bitmap("figma-ux/advanced-2d-card-regenerate", this, 14);
        if (m_thumbnail_action) m_eye = create_scaled_bitmap("figma-ux/advanced-2d-thumbnail-eye", this, 14);
        m_title_font = smart_creation_font(12, wxFONTWEIGHT_MEDIUM);
        m_label_font = smart_creation_font(12);
    }
    void update_layout()
    {
        if (!m_design_preview) return;
        const auto grip = [this](int n) { return FromDIP(n); };
        m_pressed = m_last_released = Hit::None;
        m_controls = design_preview_controls(GetClientSize(), bool(m_thumbnail_action), grip);
        for (size_t i = 0; i < m_thumbnails.size(); ++i) prepare_thumbnail(i);
        update_hover(m_hover, ScreenToClient(wxGetMousePosition()));
    }
    void prepare_thumbnail(size_t i)
    {
        if (!m_thumbnails[i]) return;
        const auto& data = m_thumbnail_data[i];
        m_thumbnails[i]->display(data.path, data.revision, m_controls.thumbnails[i].GetSize(),
                                 GetDPI().x, FromDIP(8));
    }
    void prepare_image()
    {
        const auto size = GetClientSize();
        if (m_design_preview) {
            m_preview.request({m_path, m_revision, size, GetDPI().x, FromDIP(16), true, Presentation::PreviewCropAlignment::Center});
        } else {
            m_preview.request({m_path, m_revision, {std::max(0, size.x - FromDIP(20)), std::max(0, size.y - FromDIP(47))},
                int(std::lround(96 * GetDPIScaleFactor()))});
        }
    }
    bool accept_control(const wxMouseEvent& event) const { return !m_activation_guard || m_activation_guard(event); }
    bool star_visible() const
    {
        return m_local_visible && m_local_enabled && (m_path.empty() || m_hover);
    }
    Hit hit(wxPoint point) const
    {
        if (!GetClientRect().Contains(point)) return Hit::None;
        if (const auto thumb = m_controls.thumbnail_at(point, m_model_input_preview)) {
            const auto first = thumb->selection ? Hit::FirstCheckbox : Hit::FirstThumbnail;
            return static_cast<Hit>(static_cast<int>(first) + thumb->index);
        }
        if (star_visible() && m_controls.regenerate.Contains(point)) return Hit::Regenerate;
        return Hit::Picture;
    }
    void update_hover(bool inside, wxPoint point)
    {
        inside = inside && GetClientRect().Contains(point);
        const auto thumb = inside && m_design_preview ? m_controls.thumbnail_at(point, m_model_input_preview) : std::nullopt;
        const std::optional<size_t> thumbnail = thumb ? std::optional<size_t>(thumb->index) : std::nullopt;
        const bool over_check = thumb && thumb->selection;
        const bool repaint = (!m_model_input_preview && m_hover != inside) ||
            m_hovered_thumbnail != thumbnail || m_hovered_checkbox != over_check;
        m_hover = inside;
        m_hovered_thumbnail = thumbnail;
        m_hovered_checkbox = over_check;
        if (m_design_preview) {
            const bool actionable = inside && (thumb ? (!over_check || m_thumbnail_checks_enabled[thumb->index]) :
                (star_visible() && m_controls.regenerate.Contains(point) ? m_local_enabled : m_picture_selectable));
            if (actionable != m_pointer_actionable) {
                m_pointer_actionable = actionable;
                SetCursor(wxCursor(actionable ? wxCURSOR_HAND : wxCURSOR_ARROW));
            }
        }
        if (repaint) Refresh(false);
    }
    static void draw_chip(wxGraphicsContext& gc, const wxRect& rect, int radius, const wxColour& colour)
    {
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(colour));
        gc.DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, std::min({radius, rect.width / 2, rect.height / 2}));
    }
    static void draw_centered_text(wxGraphicsContext& gc, const wxRect& rect, const wxString& text, const wxFont& font)
    {
        gc.SetFont(font, *wxWHITE);
        wxDouble width, height;
        gc.GetTextExtent(text, &width, &height);
        gc.DrawText(text, rect.x + (rect.width - width) / 2.0, rect.y + (rect.height - height) / 2.0);
    }
    void draw_model_check(wxGraphicsContext& gc, const wxRect& rect, bool selected, bool enabled)
    {
        if (rect.width < 4) return;
        const int radius = std::min(FromDIP(5), rect.width / 4);
        const auto fill = selected ? (enabled ? accent_colour() : wxColour(170, 156, 91)) : wxColour(31, 31, 31, 216);
        draw_chip(gc, rect, radius, fill);
        if (selected) {
            gc.SetPen(wxPen(wxColour(30, 28, 18), std::min(FromDIP(2), std::max(1, rect.width / 6))));
            const double x = rect.x, y = rect.y, w = rect.width;
            wxPoint2DDouble tick[] = {{x + 0.24*w, y + 0.48*w}, {x + 0.43*w, y + 0.67*w}, {x + 0.77*w, y + 0.30*w}};
            gc.StrokeLines(3, tick);
        } else {
            gc.SetBrush(*wxTRANSPARENT_BRUSH);
            gc.SetPen(wxPen(enabled ? *wxWHITE : wxColour(118, 118, 121), 1));
            gc.DrawRoundedRectangle(rect.x + 0.5, rect.y + 0.5, rect.width - 1, rect.height - 1, radius);
        }
    }
    void draw_thumbnail(wxGraphicsContext& gc, size_t i)
    {
        const auto& rect = m_controls.thumbnails[i];
        if (rect.IsEmpty()) return;
        const int radius = std::min(FromDIP(8), rect.width / 2);
        const auto& bitmap = m_thumbnails[i]->bitmap();
        if (bitmap.IsOk()) gc.DrawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height);
        else draw_chip(gc, rect, radius, wxColour(37, 37, 37));
        if (m_hovered_thumbnail == i && !m_hovered_checkbox) {
            if (!m_model_input_preview) draw_chip(gc, rect, radius, wxColour(0, 0, 0, 102));
            const int side = std::min(FromDIP(14), rect.width);
            if (m_eye.IsOk()) gc.DrawBitmap(m_eye, rect.x + (rect.width - side) / 2.0,
                                           rect.y + (rect.height - side) / 2.0, side, side);
        }
        const double border = GetDPIScaleFactor() * (AllViews[i] == m_thumbnail_view ? 1.0 : 0.5);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(gc.CreatePen(wxGraphicsPenInfo(*wxWHITE).Width(border)));
        gc.DrawRoundedRectangle(rect.x + border / 2, rect.y + border / 2, rect.width - border,
                                rect.height - border, std::max(0.0, radius - border / 2));
        if (m_model_input_preview) {
            auto check = m_controls.thumbnail_checks[i];
            check.Deflate(FromDIP(1));
            draw_model_check(gc, check, m_thumbnail_selected[i], m_thumbnail_checks_enabled[i]);
        }
    }
    void draw_thumbnail_tooltip(wxGraphicsContext& gc, size_t i)
    {
        const auto& from = m_controls.thumbnails[i];
        const int width = std::min(FromDIP(52), GetClientSize().x);
        const int height = FromDIP(28), tip = FromDIP(5);
        const int center = from.x + from.width / 2;
        const int y = from.y - FromDIP(2) - tip - height;
        if (y < m_controls.title.GetBottom() + FromDIP(2)) return;
        const wxRect body(std::clamp(center - width / 2, 0, GetClientSize().x - width), y, width, height);
        const wxColour colour(18, 18, 18);
        draw_chip(gc, body, FromDIP(6), colour);
        auto triangle = gc.CreatePath();
        const int bottom = y + height;
        triangle.MoveToPoint(center - FromDIP(5), bottom);
        triangle.AddLineToPoint(center, bottom + tip);
        triangle.AddLineToPoint(center + FromDIP(5), bottom);
        triangle.CloseSubpath();
        gc.FillPath(triangle);
        draw_centered_text(gc, body, title(AllViews[i]), m_label_font);
    }
    void paint_design(wxAutoBufferedPaintDC& dc, wxGraphicsContext& gc, wxSize size)
    {
        const wxRect whole(wxPoint(0, 0), size);
        const int radius = std::min({FromDIP(16), size.x / 2, size.y / 2});
        draw_chip(gc, whole, radius, panel_colour());
        const auto& bitmap = m_preview.bitmap_for(m_path, m_revision, Presentation::PreviewCropAlignment::Center);
        if (bitmap.IsOk()) {
            gc.DrawBitmap(bitmap, 0, 0, size.x, size.y);
            if (m_hover && !m_model_input_preview) {
                wxGraphicsGradientStops stops(wxColour(0, 0, 0, 153), wxColour(0, 0, 0, 153));
                stops.Add(wxColour(42, 42, 42, 85), 0.076923);
                stops.Add(wxColour(0, 0, 0, 0), 0.163462);
                stops.Add(wxColour(0, 0, 0, 0), 0.836538);
                stops.Add(wxColour(19, 19, 19, 76), 0.918176);
                gc.SetBrush(gc.CreateLinearGradientBrush(0, 0, 0, size.y, stops));
                gc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
            }
        }
        if (m_loading) draw_spinner(gc, size.x * 0.5, size.y * 0.5);
        if (!bitmap.IsOk() || (!m_status.empty() && m_status != t("设计图"))) {
            const auto status = m_status.empty() || m_status == t("设计图") ? t("等待生成") : m_status;
            dc.SetFont(m_label_font);
            const int inset = FromDIP(12);
            const auto text = wxControl::Ellipsize(status, dc, wxELLIPSIZE_END, std::max(0, size.x - 2 * inset));
            const int height = FromDIP(24);
            const int width = std::min(size.x, dc.GetTextExtent(text).x + FromDIP(12));
            int y = (size.y - height) / 2 + (m_loading ? FromDIP(32) : 0);
            int x = (size.x - width) / 2;
            if (bitmap.IsOk() && !m_loading) {
                x = std::max(0, size.x - inset - width);
                const int controls_top = m_thumbnail_action ? m_controls.thumbnails.front().y : size.y - inset;
                y = std::max(0, controls_top - height - FromDIP(4));
            }
            const wxRect status_bounds(x, std::clamp(y, 0, std::max(0, size.y - height)), width, height);
            if (bitmap.IsOk()) draw_chip(gc, status_bounds, FromDIP(6), wxColour(0, 0, 0, 166));
            draw_centered_text(gc, status_bounds, text, m_label_font);
        }
        draw_chip(gc, m_controls.title, FromDIP(8), wxColour(0, 0, 0, 166));
        draw_centered_text(gc, m_controls.title, m_caption, m_title_font);
        if (star_visible() && m_star.IsOk()) {
            const auto& button = m_controls.regenerate;
            draw_chip(gc, button, FromDIP(6), wxColour(0, 0, 0, 115));
            const double height = std::min(FromDIP(14), button.height);
            const double width = height * m_star.GetWidth() / m_star.GetHeight();
            gc.DrawBitmap(m_star, button.x + (button.width - width) / 2, button.y + (button.height - height) / 2, width, height);
        }
        for (size_t i = 0; i < m_thumbnails.size(); ++i)
            if (m_thumbnails[i]) draw_thumbnail(gc, i);
        if (m_hovered_thumbnail) draw_thumbnail_tooltip(gc, *m_hovered_thumbnail);
        if (m_selected) {
            const int stroke = FromDIP(2);
            const double inset = stroke / 2.0;
            if (size.x > stroke && size.y > stroke) {
                gc.SetBrush(*wxTRANSPARENT_BRUSH);
                gc.SetPen(wxPen(accent_colour(), stroke));
                gc.DrawRoundedRectangle(inset, inset, size.x - stroke, size.y - stroke, std::max(0.0, radius - inset));
            }
            const int edge = std::min({FromDIP(24), size.x / 4, size.y / 4});
            const int margin = std::min(FromDIP(10), size.x / 12);
            draw_model_check(gc, {size.x - margin - edge, margin, edge, edge}, true, true);
        }
    }
    void draw_spinner(wxGraphicsContext& gc, double x, double y)
    {
        constexpr double pi = 3.14159265358979323846;
        const double radius = FromDIP(16), outside = radius + FromDIP(7);
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(wxColour(20, 20, 22, 184)));
        gc.DrawEllipse(x - outside, y - outside, outside * 2, outside * 2);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.SetPen(wxPen(wxColour(255, 255, 255, 58), FromDIP(3)));
        gc.DrawEllipse(x - radius, y - radius, radius * 2, radius * 2);
        const double start = m_spinner_phase * pi / 12.0;
        auto arc = gc.CreatePath();
        arc.AddArc(x, y, radius, start, start + pi * 1.35, true);
        gc.SetPen(wxPen(accent_colour(), FromDIP(3)));
        gc.StrokePath(arc);
    }
    void paint() {
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(background_colour())); dc.Clear();
        const auto size = GetClientSize(); if (size.x < 2 || size.y < 2) return;
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
        if (m_design_preview) { paint_design(dc, *gc, size); return; }
        gc->SetBrush(wxBrush(panel_colour()));
        gc->SetPen(wxPen(m_selected ? accent_colour() : m_hover ? wxColour(104, 104, 110) : wxColour(64, 64, 69), FromDIP(m_selected ? 2 : 1)));
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(12));
        const LayoutRect image_area {FromDIP(10), FromDIP(10), std::max(0, size.x - FromDIP(20)),
                                     std::max(0, size.y - FromDIP(47))};
        const auto& bitmap = m_preview.bitmap_for(m_path, m_revision, Presentation::PreviewCropAlignment::Upper);
        const auto bounds = contain_image(image_area, bitmap.IsOk() ? bitmap.GetWidth() : 0,
                                          bitmap.IsOk() ? bitmap.GetHeight() : 0);
        if (bounds.width > 0 && bounds.height > 0) {
            gc->DrawBitmap(bitmap, bounds.x, bounds.y, bounds.width, bounds.height);
        } else if (!m_loading) {
            dc.SetFont(GetFont()); dc.SetTextForeground(secondary_text_colour());
            const wxString placeholder = m_upload ? t("+ 上传图片") : t("等待生成"); const auto extent = dc.GetTextExtent(placeholder);
            dc.DrawText(placeholder, (size.x - extent.x) / 2, std::max(0, (size.y - extent.y) / 2 - FromDIP(12)));
        }
        if (m_loading && image_area.width > FromDIP(36) && image_area.height > FromDIP(36))
            draw_spinner(*gc, image_area.x + image_area.width * 0.5, image_area.y + image_area.height * 0.5);
        dc.SetFont(GetFont()); dc.SetTextForeground(primary_text_colour());
        const auto inset = FromDIP(m_confirmation_caption ? 6 : 12);
        const auto caption = wxControl::Ellipsize(m_caption, dc, wxELLIPSIZE_END, std::max(0, size.x - 2 * inset));
        const int caption_x = m_confirmation_caption ? (size.x - dc.GetTextExtent(caption).x) / 2 : inset;
        dc.DrawText(caption, caption_x, size.y - FromDIP(29));
        dc.SetTextForeground(secondary_text_colour());
        const auto status = wxControl::Ellipsize(m_status, dc, wxELLIPSIZE_END, std::max(0, size.x - FromDIP(102)));
        const auto extent = dc.GetTextExtent(status);
        dc.DrawText(status, size.x - extent.x - FromDIP(14), size.y - FromDIP(29));
        if (m_selected) {
            gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(accent_colour()));
            gc->DrawRoundedRectangle(size.x - FromDIP(34), FromDIP(10), FromDIP(24), FromDIP(24), FromDIP(7));
            gc->SetPen(wxPen(wxColour(30, 28, 18), FromDIP(2)));
            gc->StrokeLine(size.x - FromDIP(28), FromDIP(21), size.x - FromDIP(23), FromDIP(26));
            gc->StrokeLine(size.x - FromDIP(23), FromDIP(26), size.x - FromDIP(16), FromDIP(17));
        }
    }
    wxString m_caption, m_status;
    bool m_confirmation_caption {false};
    std::string m_path, m_revision;
    Presentation::AsyncPreviewImage m_preview;
    bool m_active {true};
    wxTimer m_loading_timer;
    int m_spinner_phase {0};
    bool m_selected {false}, m_hover {false}, m_loading {false}, m_upload {false};
    DesignPreviewControls m_controls;
    wxBitmap m_star, m_eye;
    wxFont m_title_font, m_label_font;
    bool m_design_preview {false}, m_local_visible {false}, m_local_enabled {false}, m_pointer_actionable {false};
    Hit m_pressed {Hit::None}, m_last_released {Hit::None};
    bool m_picture_selectable {false}, m_model_input_preview {false}, m_hovered_checkbox {false};
    std::array<bool, 4> m_thumbnail_selected {}, m_thumbnail_checks_enabled {};
    std::optional<size_t> m_hovered_thumbnail;
    ViewId m_thumbnail_view {ViewId::Front};
    struct ThumbnailData { std::string path, revision; };
    std::array<ThumbnailData, 4> m_thumbnail_data;
    std::array<std::unique_ptr<PreviewThumbnail>, 4> m_thumbnails;
    std::function<void()> m_card_click, m_card_double_click, m_local_action;
    std::function<void(ViewId)> m_thumbnail_action, m_thumbnail_selection_action;
    std::function<bool(const wxMouseEvent&)> m_activation_guard, m_checkbox_guard;
};

struct HistorySetPreview {
    std::string id;
    std::string display_name;
    std::uint64_t revision {0};
    wxString caption;
    wxString prompt;
    std::array<std::string, 4> images;
    std::size_t available {0};
};

class HistorySetCard final : public wxPanel {
public:
    HistorySetCard(wxWindow* parent, HistorySetPreview preview, bool selected,
                   std::function<void(const std::string&)> open,
                   std::function<void(const std::string&, std::uint64_t, std::string)> rename,
                   std::function<void(const std::string&, wxPoint)> context_menu)
        : wxPanel(parent), m_preview(std::move(preview)), m_open_timer(this), m_selected(selected),
          m_open(std::move(open)), m_rename(std::move(rename)), m_context_menu(std::move(context_menu))
    {
        SetName(t("advanced-history-group-") + t(m_preview.id.c_str()));
        SetMinSize(FromDIP(wxSize(132, 164)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(panel_colour());
        SetCanFocus(true);
        SetCursor(wxCursor(wxCURSOR_HAND));
        style_text(this, primary_text_colour(), 10);
        m_editor = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                  wxTE_PROCESS_ENTER | wxBORDER_NONE);
        m_editor->SetBackgroundColour(control_colour());
        style_text(m_editor, primary_text_colour(), 10);
        m_editor->Hide();
        for (auto& image:m_images) image=std::make_unique<Presentation::AsyncPreviewImage>(this);
        const wxString details = m_preview.caption + t("\n") +
            wxString::Format(t("%llu/4 个视角"), static_cast<unsigned long long>(m_preview.available));
        SetToolTip(details);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            prepare_images();
            layout_editor();
            event.Skip();
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(false); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(false); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { SetFocus(); event.Skip(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (!IsEnabled() || m_editing) return;
            if (title_rect().Contains(event.GetPosition())) m_open_timer.StartOnce(double_click_ms());
            else if (m_open) m_open(m_preview.id);
        });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& event) {
            if (!IsEnabled() || !title_rect().Contains(event.GetPosition())) return;
            m_open_timer.Stop();
            begin_edit();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (IsEnabled() && !m_editing && m_open) m_open(m_preview.id);
        }, m_open_timer.GetId());
        Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent& event) {
            if (!IsEnabled() || m_editing || !m_context_menu) { event.Skip(); return; }
            m_open_timer.Stop();
            const auto id = m_preview.id;
            const auto position = event.GetPosition() == wxDefaultPosition
                ? ClientToScreen(wxPoint(GetClientSize().x / 2, GetClientSize().y / 2)) : event.GetPosition();
            // A refresh during the native menu may destroy this card.
            const auto context_menu = m_context_menu;
            context_menu(id, position);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (IsEnabled() && (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
                if (m_open) m_open(m_preview.id);
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        m_editor->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
            finish_edit(true);
            event.Skip();
        });
        m_editor->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) {
                finish_edit(true);
                return;
            }
            if (event.GetKeyCode() == WXK_ESCAPE) { finish_edit(false); return; }
            event.Skip();
        });
    }

    ~HistorySetCard() override { m_open_timer.Stop(); }

private:
    wxRect title_rect() const
    {
        const auto size = GetClientSize();
        const int margin = FromDIP(8);
        const int gap = FromDIP(4);
        const int mosaic_edge = std::max(0, std::min(size.x - margin * 2, size.y - FromDIP(54)));
        const int text_y = margin + mosaic_edge + FromDIP(7);
        return {margin, text_y, std::max(0, size.x - margin * 2), FromDIP(20)};
    }

    void layout_editor()
    {
        if (m_editor) m_editor->SetSize(title_rect());
    }

    void begin_edit()
    {
        if (m_editing || !m_editor) return;
        m_editing = true;
        m_editor->ChangeValue(m_preview.display_name.empty() ? m_preview.caption : t(m_preview.display_name.c_str()));
        layout_editor();
        m_editor->Show();
        m_editor->SetFocus();
        m_editor->SelectAll();
        Refresh(false);
    }

    void finish_edit(bool save)
    {
        if (!m_editing || !m_editor) return;
        wxString value = m_editor->GetValue();
        value.Trim(true).Trim(false);
        m_editing = false;
        m_editor->Hide();
        Refresh(false);
        if (!save || !m_rename) return;
        const wxCharBuffer encoded = value.ToUTF8();
        auto rename = m_rename;
        rename(m_preview.id, m_preview.revision, encoded.data() ? std::string(encoded.data()) : std::string());
    }

public:
    void set_active(bool active) { for (auto& image:m_images) image->set_active(active); if (active) prepare_images(); }
private:
    void prepare_images() {
        const auto size=GetClientSize();
        const int edge=std::max(0,(std::min(size.x-FromDIP(16),size.y-FromDIP(54))-FromDIP(4))/2);
        for (size_t i=0;i<m_images.size();++i)
            m_images[i]->request({m_preview.images[i], m_preview.images[i], {edge,edge}, int(std::lround(96*GetDPIScaleFactor()))});
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour()));
        dc.Clear();
        const auto size = GetClientSize();
        if (size.x <= FromDIP(20) || size.y <= FromDIP(70)) return;
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        const wxColour border = m_selected ? accent_colour() :
            (m_hover || HasFocus()) ? wxColour(104, 104, 110) : control_colour();
        gc->SetBrush(wxBrush(control_colour()));
        gc->SetPen(wxPen(border, FromDIP(m_selected ? 2 : 1)));
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(8));

        const int margin = FromDIP(8);
        const int gap = FromDIP(4);
        const int mosaic_edge = std::max(0, std::min(size.x - margin * 2, size.y - FromDIP(54)));
        const int mosaic_x = std::max(0, (size.x - mosaic_edge) / 2);
        const int cell_edge = std::max(0, (mosaic_edge - gap) / 2);
        static const std::array<const char*, 4> placeholders {"主", "左", "后", "右"};
        for (std::size_t i = 0; i < 4; ++i) {
            const int col = static_cast<int>(i % 2), row = static_cast<int>(i / 2);
            const wxRect cell(mosaic_x + col * (cell_edge + gap), margin + row * (cell_edge + gap),
                              cell_edge, cell_edge);
            gc->SetBrush(wxBrush(wxColour(22, 22, 25)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(cell.x, cell.y, cell.width, cell.height, FromDIP(4));
            const auto& bitmap=m_images[i]->bitmap();
            if (bitmap.IsOk()) {
                const auto fitted=contain_image({cell.x,cell.y,cell.width,cell.height},bitmap.GetWidth(),bitmap.GetHeight());
                gc->DrawBitmap(bitmap,fitted.x,fitted.y,fitted.width,fitted.height);
            } else {
                dc.SetFont(GetFont());
                dc.SetTextForeground(secondary_text_colour());
                dc.DrawLabel(t(placeholders[i]), cell, wxALIGN_CENTER);
            }
        }

        dc.SetFont(GetFont());
        dc.SetTextForeground(primary_text_colour());
        const int text_y = margin + mosaic_edge + FromDIP(7);
        if (!m_editing)
            dc.DrawText(wxControl::Ellipsize(m_preview.caption, dc, wxELLIPSIZE_END,
                        std::max(0, size.x - margin * 2)), margin, text_y);
        dc.SetTextForeground(m_selected ? accent_colour() : secondary_text_colour());
        const wxString state = m_selected
            ? t("当前套图")
            : wxString::Format(t("%llu/4 个视角"), static_cast<unsigned long long>(m_preview.available));
        dc.DrawText(state, margin, text_y + FromDIP(20));
    }

    HistorySetPreview m_preview;
    std::array<std::unique_ptr<Presentation::AsyncPreviewImage>, 4> m_images;
    wxTimer m_open_timer;
    wxTextCtrl* m_editor {nullptr};
    bool m_selected {false};
    bool m_hover {false};
    bool m_editing {false};
    std::function<void(const std::string&)> m_open;
    std::function<void(const std::string&, std::uint64_t, std::string)> m_rename;
    std::function<void(const std::string&, wxPoint)> m_context_menu;
};
}

struct AdvancedCreationWorkspace::Impl {
    AdvancedCreationWorkspace& page;
    AdvancedCreationFeatureHost& host;
    std::array<ViewCard*, 4> cards {};
    ViewCard* single_card {nullptr};
    wxPanel* preview_navigation {nullptr};
    DesignIconButton *preview_grid_button {nullptr}, *preview_single_button {nullptr};
    LocalDesignPopover* local_popover {nullptr};
    bool preview_single {false}, preview_entered_single {false}, local_pending {false};
    ViewId preview_view {ViewId::Front}, local_view {ViewId::Front};
    CallbackContext preview_context;
    AdvancedPreviewClickSequence preview_click_sequence, checkbox_click_sequence;
    std::uint64_t local_sequence {0};
    UploadAreaPanel* uploads {nullptr};
    ModelGenerationFeatureHost& model_host;
    std::unique_ptr<AITextClient> text_client;
    std::shared_ptr<AITextClient::RequestHandle> polish_request;
    wxPanel* creation {nullptr};
    bool polish_pending {false}, closing {false};
    // Worker callbacks carry this token without touching wx tracker lists.
    std::shared_ptr<int> lifetime {std::make_shared<int>(0)};
    std::function<void(const ModelGenerationUIState&)> model_state_changed;
    std::function<void(const ModelJobEvent&)> model_job_event;
    wxPanel* grid {nullptr};
    InlineModelGenerationView* generation_view {nullptr};
    GenerationCompletionActions* completion_actions {nullptr};
    std::function<void()> open_workbench, completed;
    std::function<void(const std::string&)> generation_feedback;
    std::array<std::string, 4> announced_design_failures;
    std::function<void()> return_creation;
    std::function<bool()> can_return;
    bool presented {false};
    wxScrolledWindow* input_scroll {nullptr};
    wxPanel* input_content {nullptr};
    wxPanel* design_inputs {nullptr};
    wxPanel* model_inputs {nullptr};
    wxPanel* action_panel {nullptr};
    AdvancedStageTabs* stage_tabs {nullptr};
    wxPanel* history_panel {nullptr};
    wxScrolledWindow* history_scroll {nullptr};
    wxFlexGridSizer* history_rows {nullptr};
    wxTextCtrl* history_search {nullptr};
    wxStaticText* history_status {nullptr};
    wxStaticText* history_page_label {nullptr};
    RoundedActionButton* history_refresh {nullptr};
    RoundedActionButton* history_first {nullptr};
    RoundedActionButton* history_previous {nullptr};
    RoundedActionButton* history_next {nullptr};
    RoundedActionButton* history_last {nullptr};
    wxTextCtrl* prompt {nullptr};
    wxStaticText* status {nullptr};
    wxStaticText* count {nullptr};
    wxStaticText* count_error {nullptr};
    AdvancedChoicePicker* provider {nullptr};
    wxStaticText* image_model_label {nullptr};
    AdvancedChoicePicker* image_model {nullptr};
    std::vector<std::string> image_model_ids;
    AdvancedChoicePicker* geometry_provider {nullptr};
    AdvancedChoicePicker* face_limit {nullptr};
    wxTextCtrl* custom_style {nullptr};
    wxPanel* custom_style_panel {nullptr};
    wxStaticText* style_selection {nullptr};
    wxStaticText* geometry_quality_value {nullptr};
    wxStaticText* texture_quality_value {nullptr};
    wxStaticText* output_format_value {nullptr};
    wxGridSizer* style_grid {nullptr};
    std::array<SmartStyleCard*, smart_creation_styles.size()> style_cards {};
    std::array<RoundedActionButton*, 4> style_filters {};
    RoundedActionButton* polish {nullptr};
    wxStaticText* creation_lock_hint {nullptr};
    wxString creation_lock_hint_text;
    RoundedActionButton* fresh_button {nullptr};
    RoundedActionButton* generate {nullptr};
    RoundedActionButton* model {nullptr};
    RoundedActionButton* stop {nullptr};
    RoundedActionButton* drawer {nullptr};
    RoundedActionButton* reconcile {nullptr};
    wxTimer click_timer;
    std::string history_signature;
    std::size_t history_page {0};
    std::size_t history_pages {1};
    int style_filter {0};
    bool model_tab {false};
    bool service_available {false};
    Presentation::RefreshScheduler refresh;
    wxWeakRef<wxWindow> activity_owner;
    bool layout_changed {true}, history_dirty {true}, history_layout_changed {false};
    bool applying_layout {false};
    wxSize input_layout_size, grid_layout_size;
    std::optional<int> applied_style_filter;
    std::string applied_style;
    void label(wxWindow* control, const wxString& value) {
        if (control->GetLabel()==value) return;
        control->SetLabel(value); layout_changed=true;
    }
    void show(wxWindow* control, bool visible) {
        if (control->IsShown()==visible) return;
        control->Show(visible); layout_changed=true;
    }
    void enable(wxWindow* control, bool enabled) {
        if (control->IsThisEnabled()==enabled) return;
        control->Enable(enabled); control->Refresh(false);
    }
    void sync_image_activity(bool presented = true) {
        auto* owner=wxDynamicCast(activity_owner.get(),wxTopLevelWindow);
        const bool active=presented && !closing && page.IsShownOnScreen() && (!owner || !owner->IsIconized());
        std::function<void(wxWindow*)> visit=[&](wxWindow* window) {
            const bool visible=active && window->IsShownOnScreen();
            if (auto* card=dynamic_cast<ViewCard*>(window)) card->set_active(visible);
            else if (auto* slot=dynamic_cast<UploadSlot*>(window)) slot->set_active(visible);
            else if (auto* card=dynamic_cast<HistorySetCard*>(window)) card->set_active(visible);
            for (auto* child:window->GetChildren()) visit(child);
        };
        visit(&page);
    }
    void set_presented(bool visible) {
        if (closing) return;
        presented = visible;
        if (!visible) close_local_popover();
        sync_image_activity(visible);
        if (generation_view) generation_view->set_presented(visible && model_tab && generation_view->has_content());
        if (visible) refresh.resume();
    }
    void on_page_show(bool shown) {
        if (closing) return;
        if (shown) switch_stage(false, true);
        else if (polish_request) {
            polish_request->cancel();
            polish_request.reset();
            polish_pending = false;
        }
        set_presented(shown);
    }
    void on_input_size(wxSizeEvent& event) {
        event.Skip();
        if (closing) return;
        const auto size = input_scroll->GetClientSize();
        if (size == input_layout_size) return;
        input_layout_size = size;
        if (!applying_layout) refresh.request(Presentation::Dirty::Layout);
    }
    void on_grid_size(wxSizeEvent& event) {
        event.Skip();
        if (closing) return;
        const auto size = grid->GetClientSize();
        if (size == grid_layout_size) return;
        grid_layout_size = size;
        if (!applying_layout) refresh.request(Presentation::Dirty::Layout);
    }
    void on_iconize(wxIconizeEvent& event) {
        event.Skip(); set_presented(!event.IsIconized());
    }
    void on_move(wxMoveEvent& event) {
        event.Skip();
        if (!closing) position_local_popover();
    }
    struct ProviderStatus {
        std::string provider;
        std::string state;
        std::string connection_state;
        std::uint64_t sequence {0};
    } image_status, geometry_status;

    Impl(AdvancedCreationWorkspace& owner, AdvancedCreationFeatureHost& feature, std::function<void()> show_smart,
         ModelGenerationFeatureHost& shared_models,
         std::function<void(const ModelGenerationUIState&)> model_state,
         std::function<void(const ModelJobEvent&)> job_event)
        : page(owner), host(feature), model_host(shared_models),
          model_state_changed(std::move(model_state)), model_job_event(std::move(job_event)), click_timer(&owner) {
        activity_owner = wxGetTopLevelParent(&page);
        refresh.configure([this] {
            auto* owner=wxDynamicCast(activity_owner.get(),wxTopLevelWindow);
            return !closing && page.IsShownOnScreen() && (!owner || !owner->IsIconized());
        }, [this](Presentation::Dirty flags) { apply_snapshot(flags); }, "advanced-creation");
        if (activity_owner) {
            activity_owner->Bind(wxEVT_ICONIZE, &Impl::on_iconize, this);
            activity_owner->Bind(wxEVT_MOVE, &Impl::on_move, this);
        }
        page.SetBackgroundColour(background_colour());
        auto* root = new wxBoxSizer(wxVERTICAL); page.SetSizer(root);
        creation = new wxPanel(&page); creation->SetBackgroundColour(background_colour());
        auto* outer = new wxBoxSizer(wxHORIZONTAL); creation->SetSizer(outer); root->Add(creation, 1, wxEXPAND);
        text_client = std::make_unique<AITextClient>(AISidecarClient::default_endpoint());
        host.set_start_guard([this] { return !polish_pending && !model_host.creation_editing_locked() && model_host.can_replace_asset(); });
        host.set_generation_guard([this] {
            return !model_tab || !generation_view || !generation_view->loading();
        });
        host.set_editing_guard([this] { return !model_host.creation_editing_locked(); });
        wxWeakRef<AdvancedCreationWorkspace> weak(&page);
        host.set_library_commit_listener([weak](ModelLibraryCommit event) {
            if (!weak || weak->m_impl->closing) return;
            event.foreground = event.foreground && weak->IsShownOnScreen();
            weak->m_impl->model_host.publish_library_commit(std::move(event));
        });
        host.set_model_state_listener([weak](const ModelGenerationUIState& state, bool) {
            if (!weak || weak->m_impl->closing) return;
            auto& self = *weak->m_impl;
            if (self.model_state_changed) self.model_state_changed(state);
            self.render();
        });
        host.set_model_job_listener([weak](const ModelJobEvent& event) {
            if (!weak || weak->m_impl->closing || !weak->m_impl->host.session().matches(event.context)) return;
            if (weak->m_impl->model_job_event) weak->m_impl->model_job_event(event);
        });
        auto* sidebar = new RoundedPanel(creation, wxDefaultSize, panel_colour(), background_colour(), 12);
        sidebar->SetWindowStyleFlag(sidebar->GetWindowStyleFlag() | wxCLIP_CHILDREN);
        sidebar->SetMinSize(page.FromDIP(wxSize(344, -1))); outer->Add(sidebar, 0, wxEXPAND | wxRIGHT, page.FromDIP(16));
        auto* left = new wxBoxSizer(wxVERTICAL); sidebar->SetSizer(left);
        auto* title_heading = new AdvancedTitleHeading(sidebar);
        left->Add(title_heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        stage_tabs = new AdvancedStageTabs(sidebar, [this](bool selected_model) { switch_stage(selected_model); });
        stage_tabs->SetName("advanced-creation-stage-tabs");
        left->Add(stage_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        input_scroll = new wxScrolledWindow(sidebar, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
        input_scroll->SetMinSize(wxSize(0, 0));
        input_scroll->SetBackgroundColour(panel_colour()); input_scroll->SetScrollRate(0, page.FromDIP(12));
        input_scroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER); left->Add(input_scroll, 1, wxEXPAND);
        auto* scroll = new wxPanel(input_scroll); input_content = scroll; scroll->SetBackgroundColour(panel_colour());
        input_scroll->Bind(wxEVT_SIZE, &Impl::on_input_size, this);
        auto* input_pages = new wxBoxSizer(wxVERTICAL); scroll->SetSizer(input_pages);
        design_inputs = new wxPanel(scroll); design_inputs->SetBackgroundColour(panel_colour());
        model_inputs = new wxPanel(scroll); model_inputs->SetBackgroundColour(panel_colour());
        input_pages->Add(design_inputs, 0, wxEXPAND); input_pages->Add(model_inputs, 0, wxEXPAND);
        auto* inputs = new wxBoxSizer(wxVERTICAL); design_inputs->SetSizer(inputs);
        auto* upload_header = new wxBoxSizer(wxHORIZONTAL);
        auto* upload_title = new wxStaticText(design_inputs, wxID_ANY, t("上传多视角图片"));
        upload_title->SetBackgroundColour(panel_colour());
        upload_title->SetForegroundColour(wxColour(255, 255, 255, 230));
        upload_title->SetFont(figma_font(upload_title, 14, wxFONTWEIGHT_MEDIUM));
        upload_header->Add(upload_title, 0, wxALIGN_CENTER_VERTICAL);
        auto* upload_info = new UploadInfoIcon(design_inputs);
        upload_header->Add(upload_info, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, page.FromDIP(4));
        upload_header->AddStretchSpacer();
        auto* fresh = new RoundedActionButton(design_inputs, t("新建创作"), true, 32);
        fresh_button = fresh;
        fresh->SetName("advanced-design-new-creation");
        fresh->SetMinSize(page.FromDIP(wxSize(112, 32)));
        fresh->SetMaxSize(page.FromDIP(wxSize(112, 32)));
        fresh->set_corner_radius(16);
        style_smart_creation_text(fresh, 13, wxColour(20, 20, 20), wxFONTWEIGHT_MEDIUM);
        fresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (model_host.creation_editing_locked()) return;
            click_timer.Stop();
            if (polish_request) { polish_request->cancel(); polish_request.reset(); }
            polish_pending = false;
            host.new_creation();
            reset_preview();
            switch_stage(false, true);
        });
        upload_header->Add(fresh, 0, wxALIGN_CENTER_VERTICAL);
        inputs->Add(upload_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        inputs->AddSpacer(page.FromDIP(12));
        uploads = new UploadAreaPanel(design_inputs, [this](ViewId view) { choose(view); }, [this](ViewId view) {
            if (!input_editable()) return;
            host.session().set_upload(view, {});
            host.changed();
        }, [this](ViewId view, wxString path) { accept(view, path); });
        inputs->Add(uploads, 0, wxEXPAND | wxLEFT | wxRIGHT, page.FromDIP(16));
        inputs->AddSpacer(page.FromDIP(16));
        auto* prompt_header = new wxBoxSizer(wxHORIZONTAL);
        auto* prompt_heading = new wxStaticText(design_inputs, wxID_ANY, t("描述词"));
        style_smart_creation_text(prompt_heading, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        prompt_header->Add(prompt_heading, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, page.FromDIP(12));
        polish = new RoundedActionButton(design_inputs, t("AI润色"), true, 28);
        polish->SetMinSize(page.FromDIP(wxSize(96, 28))); polish->set_horizontal_padding(10);
        polish->set_icon(create_scaled_bitmap("redesign_ai_polish", polish, 14)); polish->set_icon_on_left(true);
        style_smart_creation_text(polish, 13, wxColour(20, 20, 20), wxFONTWEIGHT_MEDIUM);
        polish->SetToolTip(t("优化当前描述"));
        polish->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { polish_prompt(); });
        prompt_header->Add(polish, 0, wxALIGN_CENTER_VERTICAL); prompt_header->AddStretchSpacer();
        inputs->Add(prompt_header, 0, wxEXPAND | wxLEFT | wxRIGHT, page.FromDIP(16));
        auto* prompt_box = new RoundedPanel(design_inputs, wxDefaultSize, wxColour(21, 21, 23), panel_colour(), 12);
        prompt_box->SetBackgroundColour(wxColour(21, 21, 23));
        auto* prompt_sizer = new wxBoxSizer(wxVERTICAL); prompt_box->SetSizer(prompt_sizer);
        prompt = new AdvancedPromptTextCtrl(prompt_box, page.FromDIP(wxSize(-1, 48)));
        prompt->SetName("advanced-prompt"); prompt->SetBackgroundColour(wxColour(21, 21, 23));
        style_smart_creation_text(prompt, 13, wxColour(230, 230, 233));
        static_cast<AdvancedPromptTextCtrl*>(prompt)->set_hint_colour(
            smart_creation_opaque_colour(wxColour(255, 255, 255, 115), wxColour(21, 21, 23)));
        prompt->SetHint(t("描述你想创建的设计图，例如一只可爱的小猫"));
        prompt_sizer->Add(prompt, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(12));
        count = new wxStaticText(prompt_box, wxID_ANY, "0/2000");
        style_smart_creation_text(count, 12, wxColour(255, 255, 255, 115));
        count->SetToolTip(t("上限 2000 字符，以输入框容量计数为准。"));
        prompt_sizer->Add(count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM | wxTOP, page.FromDIP(12));
        inputs->Add(prompt_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (!input_editable()) return;
            auto draft = host.session().draft().design; draft.prompt = prompt->GetValue().ToUTF8().data();
            if (host.session().set_design_draft(std::move(draft))) host.changed();
        });
        count_error = new wxStaticText(design_inputs, wxID_ANY, wxEmptyString);
        style_smart_creation_text(count_error, 12, wxColour(255, 128, 112));
        inputs->Add(count_error, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        auto* style_header = new wxBoxSizer(wxHORIZONTAL);
        auto* style_label = new wxStaticText(design_inputs, wxID_ANY, t("模型风格"));
        style_smart_creation_text(style_label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        style_header->Add(style_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, page.FromDIP(12));
        style_selection = new wxStaticText(design_inputs, wxID_ANY, wxEmptyString);
        style_smart_creation_text(style_selection, 12, secondary_text_colour());
        style_header->Add(style_selection, 0, wxALIGN_CENTER_VERTICAL);
        inputs->Add(style_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        auto* styles_panel = new wxPanel(design_inputs); styles_panel->SetBackgroundColour(panel_colour());
        auto* styles_sizer = new wxBoxSizer(wxVERTICAL); styles_panel->SetSizer(styles_sizer);
        auto* filters = new wxBoxSizer(wxHORIZONTAL);
        const std::array<const char*, 4> filter_labels {"全部", "雕塑", "多色写实", "多色风格化"};
        for (std::size_t i = 0; i < filter_labels.size(); ++i) {
            auto* filter = new RoundedActionButton(styles_panel, t(filter_labels[i]), false, 30);
            filter->set_horizontal_padding(14); filter->set_corner_radius(15);
            style_smart_creation_text(filter, 13, primary_text_colour(), wxFONTWEIGHT_MEDIUM);
            wxClientDC dc(filter); dc.SetFont(filter->GetFont());
            filter->SetMinSize(wxSize(dc.GetTextExtent(filter->GetLabel()).x + page.FromDIP(28), page.FromDIP(30)));
            filter->SetName(wxString::Format("advanced-style-filter-%d", int(i)));
            filter->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { style_filter = int(i); render(); });
            style_filters[i] = filter;
            filters->Add(filter, 0, i + 1 < filter_labels.size() ? wxRIGHT : 0, page.FromDIP(6));
        }
        styles_sizer->Add(filters, 0, wxEXPAND | wxBOTTOM, page.FromDIP(12));
        style_grid = new wxGridSizer(3, page.FromDIP(12), page.FromDIP(12));
        for (std::size_t i = 0; i < smart_creation_styles.size(); ++i) {
            const char* id = smart_creation_styles[i];
            auto* card = new SmartStyleCard(styles_panel, id, smart_creation_style_label(id), [this, id] {
                if (!input_editable()) return;
                auto draft = host.session().draft().design; draft.style = id;
                host.session().set_design_draft(std::move(draft)); host.changed();
            });
            style_cards[i] = card; style_grid->Add(card, 1, wxEXPAND);
        }
        styles_sizer->Add(style_grid, 0, wxEXPAND);
        inputs->Add(styles_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        custom_style_panel = new wxPanel(design_inputs); custom_style_panel->SetBackgroundColour(panel_colour());
        auto* custom_sizer = new wxBoxSizer(wxVERTICAL); custom_style_panel->SetSizer(custom_sizer);
        auto* custom_label = new wxStaticText(custom_style_panel, wxID_ANY, t("自定义风格描述"));
        style_smart_creation_text(custom_label, 13, primary_text_colour());
        custom_sizer->Add(custom_label, 0, wxBOTTOM, page.FromDIP(10));
        auto* custom_surface = new RoundedPanel(custom_style_panel, wxDefaultSize, control_colour(), panel_colour(), 8);
        custom_surface->SetBackgroundColour(control_colour()); auto* custom_input_sizer = new wxBoxSizer(wxVERTICAL);
        custom_surface->SetSizer(custom_input_sizer);
        custom_style = new AdvancedPromptTextCtrl(custom_surface, page.FromDIP(wxSize(-1, 76)));
        custom_style->SetHint(t("描述外观即可；系统会保留主体、构图和可见元素"));
        custom_style->SetBackgroundColour(control_colour()); style_smart_creation_text(custom_style, 13, wxColour(230, 230, 233));
        custom_input_sizer->Add(custom_style, 1, wxEXPAND | wxALL, page.FromDIP(12));
        custom_sizer->Add(custom_surface, 0, wxEXPAND);
        auto* custom_hint = new wxStaticText(custom_style_panel, wxID_ANY, t("必填，最多 1000 字节"));
        style_smart_creation_text(custom_hint, 12, secondary_text_colour()); custom_sizer->Add(custom_hint, 0, wxTOP, page.FromDIP(6));
        inputs->Add(custom_style_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        custom_style->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (!input_editable()) return;
            auto draft = host.session().draft().design; draft.custom_style = custom_style->GetValue().ToUTF8().data();
            if (host.session().set_design_draft(std::move(draft))) host.changed();
        });
        auto* image_label = new wxStaticText(design_inputs, wxID_ANY, t("图像模型选择"));
        style_smart_creation_text(image_label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        inputs->Add(image_label, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        provider = new AdvancedChoicePicker(design_inputs, {t("GPT"), t("YOYOClaw")}, [this](int selection) {
            if (!input_editable()) return;
            if (!host.session().set_design_provider(selection == 1 ? "yoyoclaw" : "gpt")) return;
            host.changed();
            if (selection == 1) host.refresh_image_catalog(); else host.refresh_capabilities("image");
            image_status.provider.clear(); refresh_provider_status(false);
        });
        provider->SetName("advanced-image-provider-choice");
        inputs->Add(provider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        image_model_label = new wxStaticText(design_inputs, wxID_ANY, t("生图模型"));
        style_smart_creation_text(image_model_label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        inputs->Add(image_model_label, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        image_model = new AdvancedChoicePicker(design_inputs, {}, [this](int selection) {
            if (!input_editable()) return;
            if (selection < 0 || selection >= static_cast<int>(image_model_ids.size())) return;
            auto draft = host.session().draft().design;
            draft.model = image_model_ids[selection];
            host.session().set_design_draft(std::move(draft)); host.changed();
        });
        image_model->SetName("advanced-image-model-choice");
        inputs->Add(image_model, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, page.FromDIP(16));
        image_model_label->Hide(); image_model->Hide();

        auto* model_input_sizer = new wxBoxSizer(wxVERTICAL); model_inputs->SetSizer(model_input_sizer);
        auto add_model_heading = [this, model_input_sizer](const wxString& caption) {
            auto* label = new wxStaticText(model_inputs, wxID_ANY, caption);
            style_smart_creation_text(label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
            model_input_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        };
        add_model_heading(t("3D模型选择"));
        geometry_provider = new AdvancedChoicePicker(model_inputs, {t("Tripo"), t("YOYOClaw")}, [this](int) { options_changed(); });
        geometry_provider->SetName("advanced-geometry-provider-choice");
        model_input_sizer->Add(geometry_provider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        add_model_heading(t("模型面数"));
        face_limit = new AdvancedChoicePicker(model_inputs,
            {t("100 万面（低模，推荐）"), t("200 万面（高模）")}, [this](int) { options_changed(); });
        face_limit->SetName("advanced-model-face-limit-choice");
        model_input_sizer->Add(face_limit, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        add_model_heading(t("生成设置"));
        auto* settings_surface = new RoundedPanel(model_inputs, wxDefaultSize, control_colour(), panel_colour(), 12);
        settings_surface->SetBackgroundColour(control_colour());
        auto* settings_grid = new wxFlexGridSizer(2, page.FromDIP(12), page.FromDIP(16)); settings_grid->AddGrowableCol(1, 1);
        settings_surface->SetSizer(settings_grid);
        auto add_readonly = [this, settings_surface, settings_grid](const char* name, wxStaticText*& value) {
            auto* label = new wxStaticText(settings_surface, wxID_ANY, t(name));
            style_smart_creation_text(label, 13, secondary_text_colour());
            value = new wxStaticText(settings_surface, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
            style_smart_creation_text(value, 13, primary_text_colour(), wxFONTWEIGHT_MEDIUM);
            settings_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            settings_grid->Add(value, 1, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
        };
        add_readonly("几何质量", geometry_quality_value);
        add_readonly("贴图质量", texture_quality_value);
        add_readonly("输出格式", output_format_value);
        model_input_sizer->Add(settings_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, page.FromDIP(16));

        action_panel = new wxPanel(sidebar); action_panel->SetBackgroundColour(panel_colour());
        auto* actions = new wxBoxSizer(wxVERTICAL); action_panel->SetSizer(actions);
        generate = new RoundedActionButton(action_panel, t("生成2D设计图"), true, 44);
        generate->SetName("advanced-generate-design"); generate->set_generation_gradient(true); generate->set_corner_radius(22);
        style_smart_creation_text(generate, 16, *wxWHITE, wxFONTWEIGHT_MEDIUM);
        actions->Add(generate, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, page.FromDIP(16));
        generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_generation(false); });
        model = new RoundedActionButton(action_panel, t("生成3D模型"), true, 44);
        model->SetName("advanced-generate-model"); model->set_generation_gradient(true); model->set_corner_radius(22);
        style_smart_creation_text(model, 16, *wxWHITE, wxFONTWEIGHT_MEDIUM);
        actions->Add(model, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, page.FromDIP(16));
        model->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_generation(true); });
        completion_actions = new GenerationCompletionActions(action_panel,
            [this] {
                if (model_host.creation_editing_locked()) {
                    if (can_return && can_return() && return_creation) return_creation();
                } else request_generation(true);
            },
            [this] { if (open_workbench) open_workbench(); });
        actions->Add(completion_actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        stop = new RoundedActionButton(action_panel, t("停止生成"), false, 32);
        stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.stop(); });
        actions->Add(stop, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        reconcile = new RoundedActionButton(action_panel, t("核对本次请求"), false, 32);
        reconcile->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (host.has_pending_receipt()) {
                host.reconcile_submission();
                return;
            }
            const auto& group = host.session().group();
            if (!model_tab && group) for (auto view : AllViews) {
                if (!design_attempt_recoverable(group->views[index(view)].attempt_state)) continue;
                host.recover(view);
                break;
            }
        });
        actions->Add(reconcile, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        creation_lock_hint = new wxStaticText(action_panel, wxID_ANY, wxEmptyString);
        creation_lock_hint->SetName("advanced-creation-lock-hint");
        style_smart_creation_text(creation_lock_hint, 12, secondary_text_colour());
        actions->Add(creation_lock_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        creation_lock_hint->Hide();
        left->Add(action_panel, 0, wxEXPAND);
        model_inputs->Hide(); model->Hide(); stop->Hide(); reconcile->Hide();
        auto* center = new wxPanel(creation); center->SetBackgroundColour(background_colour()); center->SetMinSize(wxSize(0, 0));
        auto* center_sizer = new wxBoxSizer(wxVERTICAL); center->SetSizer(center_sizer); outer->Add(center, 1, wxEXPAND);
        auto* modes = new wxBoxSizer(wxHORIZONTAL);
        auto* smart = new RoundedActionButton(center, t("智能模式"), false, 40);
        smart->Bind(wxEVT_BUTTON, [show_smart](wxCommandEvent&) { if (show_smart) show_smart(); });
        auto* advanced = new RoundedActionButton(center, t("高级模式"), true, 40);
        smart->SetMinSize(page.FromDIP(wxSize(150, 40))); advanced->SetMinSize(page.FromDIP(wxSize(150, 40)));
        modes->Add(smart, 0, wxRIGHT, page.FromDIP(6)); modes->Add(advanced);
        center_sizer->Add(modes, 0, wxALIGN_CENTER | wxBOTTOM, page.FromDIP(8));
        status = new wxStaticText(center, wxID_ANY, wxEmptyString); style_text(status, secondary_text_colour(), 10);
        status->SetMinSize(wxSize(0, -1)); center_sizer->Add(status, 0, wxEXPAND | wxBOTTOM, page.FromDIP(6));
        grid = new wxPanel(center);
        grid->SetBackgroundColour(background_colour()); grid->SetMinSize(wxSize(0, 0));
        center_sizer->Add(grid, 1, wxEXPAND);
        preview_navigation = new wxPanel(center); preview_navigation->SetBackgroundColour(background_colour());
        preview_navigation->SetMinSize(page.FromDIP(wxSize(0,52)));
        auto* preview_footer = new wxBoxSizer(wxVERTICAL); preview_navigation->SetSizer(preview_footer);
        auto* preview_modes = new RoundedPanel(preview_navigation,page.FromDIP(wxSize(68,36)),wxColour(31,31,31),background_colour(),8);
        preview_modes->SetMinSize(page.FromDIP(wxSize(68,36))); preview_modes->SetMaxSize(page.FromDIP(wxSize(68,36)));
        auto* mode_row = new wxBoxSizer(wxHORIZONTAL); preview_modes->SetSizer(mode_row);
        preview_single_button = new DesignIconButton(preview_modes,"preview-single",32,19,wxColour(31,31,31),"preview-single-active");
        preview_grid_button = new DesignIconButton(preview_modes,"preview-grid-inactive",32,19,wxColour(31,31,31),"preview-grid");
        preview_single_button->SetName("advanced-preview-single"); preview_single_button->SetToolTip(t("单图预览"));
        preview_grid_button->SetName("advanced-preview-grid"); preview_grid_button->SetToolTip(t("四格预览"));
        mode_row->AddSpacer(page.FromDIP(2));
        mode_row->Add(preview_grid_button, 0, wxTOP | wxBOTTOM, page.FromDIP(2));
        mode_row->Add(preview_single_button, 0, wxTOP | wxBOTTOM, page.FromDIP(2));
        mode_row->AddSpacer(page.FromDIP(2));
        preview_single_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { set_preview_mode(true); });
        preview_grid_button->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { set_preview_mode(false); });
        const auto accept_preview_click = [this](const wxMouseEvent& event) {
            return accept_click(preview_click_sequence, event);
        };
        preview_single_button->set_activation_guard(accept_preview_click);
        preview_grid_button->set_activation_guard(accept_preview_click);
        preview_footer->Add(preview_modes, 0, wxALIGN_CENTER_HORIZONTAL);
        preview_footer->AddSpacer(page.FromDIP(16));
        center_sizer->Add(preview_navigation, 0, wxEXPAND);
        single_card = new ViewCard(grid, title(preview_view));
        single_card->SetName("advanced-design-single");
        single_card->Hide();
        single_card->set_thumbnail_action([this](ViewId view) {
            if (!preview_single || preview_view == view || !showing_design_inputs()) return;
            preview_view = view;
            close_local_popover();
            render();
        });
        single_card->set_activation_guard(accept_preview_click);
        single_card->set_thumbnail_selection_action([this](ViewId view) {
            if (preview_single && can_select_model_inputs() && host.session().toggle_model_view(view)) host.changed();
        }, [this](const wxMouseEvent& event) { return accept_click(checkbox_click_sequence, event); });
        single_card->set_local_action([this] { open_local_popover(preview_view); });
        single_card->set_card_actions({}, [this] {
            host.session().cancel_pending_click();
            if (host.session().double_click(preview_view)) show_large(preview_view);
        });
        single_card->set_design_preview(true);
        local_popover = new LocalDesignPopover(grid,[this] { close_local_popover(); },
            [this](std::string text) { submit_local_design(std::move(text)); });
        generation_view = new InlineModelGenerationView(center, {
            [this] {
                const auto& state = host.model_state();
                if (state.job_state == "ready" && state.can_retry_model) host.retry_local_model();

            }, [this] { update_model_presentation(); },
            [this] { if (completed) completed(); }});
        center_sizer->Add(generation_view, 1, wxEXPAND); generation_view->Hide();
        for (auto view : AllViews) {
            const auto i = index(view); cards[i] = new ViewCard(grid, title(view)); cards[i]->SetName(t("advanced-design-") + t(view_name(view)));
            cards[i]->set_local_action([this, view] { open_local_popover(view); });
            cards[i]->set_activation_guard(accept_preview_click);
            cards[i]->set_design_preview(true);
            cards[i]->set_card_actions([this, view] {
                if (!can_select_model_inputs() || !host.session().can_toggle_model_view(view)) return;
                const auto before = host.session().model_selection();
                host.session().click(view, now_ms(), double_click_ms(), SelectionContext::Model);
                click_timer.StartOnce(double_click_ms() + 5);
                if (host.session().model_selection() != before) host.changed();
            }, [this, view] {
                click_timer.Stop();
                if (!can_select_model_inputs()) host.session().cancel_pending_click();
                const auto before = host.session().model_selection();
                const auto image = host.session().double_click(view);
                if (host.session().model_selection() != before) host.changed();
                if (image) show_large(view);
            });
        }
        grid->Bind(wxEVT_SIZE, &Impl::on_grid_size, this);
        auto* drawer_host = new wxPanel(creation); drawer_host->SetBackgroundColour(background_colour());
        auto* drawer_sizer = new wxBoxSizer(wxHORIZONTAL); drawer_host->SetSizer(drawer_sizer);
        outer->Add(drawer_host, 0, wxEXPAND | wxLEFT, page.FromDIP(10));
        drawer = new RoundedActionButton(drawer_host, t("›"), false, 64);
        drawer->SetMinSize(page.FromDIP(wxSize(24, 64)));
        drawer->SetMaxSize(page.FromDIP(wxSize(24, 64)));
        drawer->set_horizontal_padding(0);
        drawer->SetName("advanced-history-collapse"); drawer->SetToolTip(t("收起历史套图"));
        drawer->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(drawer); dc.SetBackground(wxBrush(background_colour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
            const double width = drawer->GetClientSize().x, height = drawer->GetClientSize().y;
            auto shape = gc->CreatePath();
            shape.MoveToPoint(width, 0); shape.AddLineToPoint(width * .25, height * .28);
            shape.AddQuadCurveToPoint(0, height * .5, width * .25, height * .72);
            shape.AddLineToPoint(width, height); shape.CloseSubpath();
            gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(panel_colour())); gc->FillPath(shape);
            gc->SetPen(wxPen(secondary_text_colour(), std::max(1, page.FromDIP(1))));
            const int direction = host.session().sidebar_expanded() ? 1 : -1;
            for (int step = 0; step < 2; ++step) {
                const double x = width * .4 + page.FromDIP(5) * step;
                gc->StrokeLine(x, height / 2 - page.FromDIP(4), x + direction * page.FromDIP(3), height / 2);
                gc->StrokeLine(x + direction * page.FromDIP(3), height / 2, x, height / 2 + page.FromDIP(4));
            }
            if (drawer->HasFocus()) {
                gc->SetBrush(*wxTRANSPARENT_BRUSH); gc->SetPen(wxPen(accent_colour(), std::max(1, page.FromDIP(1))));
                gc->DrawRoundedRectangle(1, 1, width - 2, height - 2, page.FromDIP(7));
            }
        });
        drawer_sizer->Add(drawer, 0, wxALIGN_CENTER_VERTICAL);
        history_panel = new RoundedPanel(drawer_host, wxDefaultSize, panel_colour(), background_colour(), 12);
        history_panel->SetMinSize(page.FromDIP(wxSize(320, -1)));
        history_panel->SetMaxSize(page.FromDIP(wxSize(320, -1)));
        drawer_sizer->Add(history_panel, 0, wxEXPAND);
        auto* history_sizer = new wxBoxSizer(wxVERTICAL); history_panel->SetSizer(history_sizer);
        auto* history_header = new wxBoxSizer(wxHORIZONTAL);
        auto* history_title = new wxStaticText(history_panel, wxID_ANY, t("历史套图"));
        history_title->SetFont(history_panel->GetFont());
        history_title->SetForegroundColour(wxColour(235, 235, 235));
        history_header->Add(history_title, 0, wxALIGN_CENTER_VERTICAL);
        history_header->AddStretchSpacer();
        history_sizer->Add(history_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(20));
        auto* search_row = new wxBoxSizer(wxHORIZONTAL);
        auto* search_box = new RoundedPanel(history_panel, wxDefaultSize, control_colour(), panel_colour(), 8);
        search_box->SetBackgroundColour(control_colour());
        auto* search_box_sizer = new wxBoxSizer(wxHORIZONTAL); search_box->SetSizer(search_box_sizer);
        auto* search_icon = new wxPanel(search_box, wxID_ANY, wxDefaultPosition, page.FromDIP(wxSize(20, 22)));
        search_icon->SetBackgroundStyle(wxBG_STYLE_PAINT);
        search_icon->Bind(wxEVT_PAINT, [this, search_icon](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(search_icon); dc.SetBackground(wxBrush(control_colour())); dc.Clear();
            dc.SetPen(wxPen(secondary_text_colour(), std::max(1, page.FromDIP(1)))); dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(page.FromDIP(8), page.FromDIP(9), page.FromDIP(5));
            dc.DrawLine(page.FromDIP(12), page.FromDIP(13), page.FromDIP(17), page.FromDIP(18));
        });
        search_box_sizer->Add(search_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, page.FromDIP(10));
        history_search = new wxTextCtrl(search_box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        history_search->SetName("advanced-history-search"); history_search->SetHint(t("搜索套图"));
        history_search->SetBackgroundColour(control_colour()); style_text(history_search, primary_text_colour(), 10);
        search_box_sizer->Add(history_search, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, page.FromDIP(8));
        search_box->SetMinSize(page.FromDIP(wxSize(-1, 40))); search_row->Add(search_box, 1, wxEXPAND);
        history_refresh = new RoundedActionButton(history_panel, wxEmptyString, false, 40);
        history_refresh->SetName("advanced-history-refresh"); history_refresh->SetToolTip(t("刷新套图"));
        history_refresh->SetMinSize(page.FromDIP(wxSize(40, 40))); history_refresh->SetMaxSize(page.FromDIP(wxSize(40, 40)));
        history_refresh->set_horizontal_padding(10); history_refresh->set_icon(create_scaled_bitmap("refresh", history_refresh, 16));
        history_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.refresh_history(); });
        search_row->Add(history_refresh, 0, wxLEFT, page.FromDIP(8));
        history_sizer->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(12));
        history_status = new wxStaticText(history_panel, wxID_ANY, t("正在读取套图…"));
        style_text(history_status, secondary_text_colour(), 10);
        history_sizer->Add(history_status, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(20));
        history_sizer->AddSpacer(page.FromDIP(8));
        history_scroll = new wxScrolledWindow(history_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
        history_scroll->SetBackgroundColour(panel_colour()); history_scroll->SetScrollRate(0, page.FromDIP(12));
        history_scroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        history_rows = new wxFlexGridSizer(2, page.FromDIP(8), page.FromDIP(12));
        history_rows->AddGrowableCol(0, 1); history_rows->AddGrowableCol(1, 1);
        history_scroll->SetSizer(history_rows);
        history_sizer->Add(history_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, page.FromDIP(20));
        auto* footer = new wxPanel(history_panel); footer->SetBackgroundColour(panel_colour());
        auto* pager = new wxBoxSizer(wxHORIZONTAL); footer->SetSizer(pager); pager->AddStretchSpacer();
        const auto add_page_button = [this, footer, pager](const char* name, const char* tooltip,
                                                           const char* icon, bool mirror,
                                                           std::function<void()> action) {
            auto* button = new RoundedActionButton(footer, wxEmptyString, false, 32);
            button->SetName(t(name)); button->SetToolTip(t(tooltip));
            button->SetMinSize(page.FromDIP(wxSize(32, 32))); button->SetMaxSize(page.FromDIP(wxSize(32, 32)));
            wxImage image = create_scaled_bitmap(icon, button, 16).ConvertToImage();
            if (mirror && image.IsOk()) image = image.Mirror(true);
            if (image.IsOk()) button->set_icon(wxBitmap(image));
            button->set_horizontal_padding(8);
            button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) { action(); });
            pager->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, page.FromDIP(3));
            return button;
        };
        history_first = add_page_button("advanced-history-first-page", "首页", "workbench_page_edge", true,
            [this] { history_page = 0; rebuild_history(); });
        history_previous = add_page_button("advanced-history-previous-page", "上一页", "workbench_page_step", true,
            [this] { if (history_page) --history_page; rebuild_history(); });
        history_page_label = new wxStaticText(footer, wxID_ANY, "1 / 1", wxDefaultPosition,
                                               wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        style_text(history_page_label, primary_text_colour(), 10);
        history_page_label->SetMinSize(page.FromDIP(wxSize(64, -1)));
        pager->Add(history_page_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, page.FromDIP(8));
        history_next = add_page_button("advanced-history-next-page", "下一页", "workbench_page_step", false,
            [this] { if (history_page + 1 < history_pages) ++history_page; rebuild_history(); });
        history_last = add_page_button("advanced-history-last-page", "末页", "workbench_page_edge", false,
            [this] { history_page = history_pages ? history_pages - 1 : 0; rebuild_history(); });
        pager->AddStretchSpacer();
        history_sizer->Add(footer, 0, wxEXPAND | wxALL, page.FromDIP(12));
        history_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { history_page = 0; rebuild_history(); });
        drawer->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            host.session().set_sidebar_expanded(!host.session().sidebar_expanded());
            render();
        });
        window_events.bind(page, click_timer.GetId(), [this](bool shown) { on_page_show(shown); },
            [this] { refresh.request(Presentation::Dirty::Layout | Presentation::Dirty::Content); },
            [this] { finish_grid_selection(now_ms()); });
    }

    AdvancedCreationWindowEvents window_events;

    ~Impl() { shutdown(); }
    void shutdown() {
        if (closing) return;
        closing = true;
        window_events.stop();
        if (input_scroll) input_scroll->Unbind(wxEVT_SIZE, &Impl::on_input_size, this);
        if (grid) grid->Unbind(wxEVT_SIZE, &Impl::on_grid_size, this);
        click_timer.Stop();
        lifetime.reset();
        if (activity_owner) {
            activity_owner->Unbind(wxEVT_ICONIZE,&Impl::on_iconize,this);
            activity_owner->Unbind(wxEVT_MOVE,&Impl::on_move,this);
        }
        refresh.stop();
        if (local_popover && local_popover->IsShown()) local_popover->Hide();
        sync_image_activity();
        if (generation_view) generation_view->shutdown();
        if (polish_request) polish_request->cancel();
        host.set_library_commit_listener({});
        host.set_model_state_listener({});
        host.set_start_guard({}); host.set_generation_guard({}); host.set_editing_guard({}); host.set_model_job_listener({}); host.set_listener({});
    }
    void switch_stage(bool selected_model, bool reset = false) {
        if (!reset && model_tab == selected_model) return;
        finish_grid_selection(std::numeric_limits<std::uint64_t>::max());
        click_timer.Stop();
        clear_preview_gestures();
        close_local_popover();
        if (!reset && model_tab && !selected_model) host.session().dismiss_initial_design_guidance();
        model_tab = selected_model;
        if (input_scroll) input_scroll->Scroll(0, 0);
        if (generate && model) render();
    }
    void refresh_styles() {
        if (!style_grid) return;
        const auto& selected_style = host.session().draft().design.style;
        for (auto* card:style_cards) enable(card,input_editable());
        if (applied_style_filter && *applied_style_filter==style_filter && applied_style==selected_style) return;
        applied_style_filter=style_filter; applied_style=selected_style;
        layout_changed=true;
        style_grid->Clear(false);
        for (std::size_t i = 0; i < style_cards.size(); ++i) {
            const int family = ModelGenerationPresentation::style_selection(smart_creation_styles[i]) + 1;
            const bool visible = style_filter == 0 || style_filter == family;
            style_cards[i]->Show(visible);
            style_cards[i]->Enable(input_editable());
            style_cards[i]->set_selected(selected_style == smart_creation_styles[i]);
            if (visible) style_grid->Add(style_cards[i], 1, wxEXPAND);
        }
        for (std::size_t i = 0; i < style_filters.size(); ++i) {
            const bool selected = int(i) == style_filter;
            auto* filter = style_filters[i];
            filter->set_secondary_face(selected ? wxColour(37, 111, 255) : wxColour(57, 57, 60));
            filter->set_text_colour(selected ? *wxWHITE :
                smart_creation_opaque_colour(wxColour(255, 255, 255, 179), wxColour(57, 57, 60)));
            filter->SetFont(smart_creation_font(13, selected ? wxFONTWEIGHT_MEDIUM : wxFONTWEIGHT_NORMAL));
        }
        if (style_selection) style_selection->SetLabel(t("已选：") + smart_creation_style_label(selected_style));
        if (custom_style_panel) custom_style_panel->Show(selected_style == "custom");
    }
    wxString provider_status_caption(const ProviderStatus& value) const {
        if (!service_available) return t("未连接");
        if (value.state.empty() || value.state == "checking") return t("检测中");
        if (value.state == "ready") return value.connection_state == "connected" ? t("已连接") : t("已配置");
        if (value.state == "unsupported_input") return t("所选图片生成不可用");
        if (value.state == "not_configured") return t("未配置");
        if (value.state == "empty") return t("无可用模型");
        return t("不可用");
    }
    void apply_provider_status(bool geometry) {
        auto* picker = geometry ? geometry_provider : provider;
        if (!picker) return;
        const auto& value = geometry ? geometry_status : image_status;
        picker->set_status(provider_status_caption(value), service_available && value.state == "ready");
    }
    void refresh_provider_status(bool geometry) {
        auto* picker = geometry ? geometry_provider : provider;
        if (!picker) return;
        auto& value = geometry ? geometry_status : image_status;
        const std::string selected_provider = geometry ? host.session().draft().geometry.provider : host.session().draft().design.provider;
        if (!service_available) {
            value.provider = selected_provider; value.state.clear(); value.connection_state.clear();
            apply_provider_status(geometry); return;
        }
        if (!geometry && selected_provider == "yoyoclaw") {
            const auto& catalog = host.image_catalog();
            value.provider = selected_provider;
            value.state = catalog.ready() ? "ready" : catalog.state;
            value.connection_state = catalog.connection_state;
            apply_provider_status(false);
            return;
        }
        if (value.provider == selected_provider && !value.state.empty()) {
            apply_provider_status(geometry); return;
        }
        value.provider = selected_provider; value.state = "checking"; value.connection_state.clear();
        const std::uint64_t sequence = ++value.sequence; apply_provider_status(geometry);
        if (geometry) host.refresh_capabilities("geometry");
        auto* target = &page;
        const auto alive = std::weak_ptr<int>(lifetime);
        host.get_provider_catalog(geometry ? "geometry" : "image", selected_provider,
            [target, alive, geometry, sequence, selected_provider](AIModelGenerationClient::ProviderCatalog catalog) {
                if (alive.expired()) return;
                auto& self = *target->m_impl;
                auto& status = geometry ? self.geometry_status : self.image_status;
                if (self.closing || status.sequence != sequence || status.provider != selected_provider) return;
                status.state = catalog.ready() ? "ready" : catalog.state;
                if (geometry && selected_provider == "yoyoclaw" && catalog.ready() && !catalog.named_four_view)
                    status.state = "unsupported_input";
                status.connection_state = catalog.connection_state;
                self.apply_provider_status(geometry);
            }, [target, alive, geometry, sequence, selected_provider](std::string) {
                if (alive.expired()) return;
                auto& self = *target->m_impl;
                auto& status = geometry ? self.geometry_status : self.image_status;
                if (self.closing || status.sequence != sequence || status.provider != selected_provider) return;
                status.state = "error"; status.connection_state.clear(); self.render();
            });
    }
    void set_service_available(bool available) {
        service_available = available;

        image_status.provider.clear(); image_status.state.clear(); ++image_status.sequence;
        geometry_status.provider.clear(); geometry_status.state.clear(); ++geometry_status.sequence;
        render();
    }
    void options_changed() {
        if (!geometry_provider || !face_limit || !input_editable()) return;
        auto options = host.session().draft().geometry;
        const bool yoyo = geometry_provider->selection() == 1;
        options.provider = yoyo ? "yoyoclaw" : "tripo"; options.model.clear();
        options.face_limit = face_limit->selection() == 1 ? 2000000 :
                             face_limit->selection() == 2 ? 300000 : 1000000;
        if (yoyo) {
            options.face_limit = 0;
            options.texture_quality = "standard";
        }
        options.geometry_quality = options.face_limit == 2000000 ? "detailed" : "standard";
        host.session().set_model_options(std::move(options)); host.changed();
    }
    bool navigation_allowed() {
        return !closing;
    }
    bool showing_design_inputs() const {
        return !model_tab || (generation_view && !generation_view->has_content());
    }
    bool can_select_model_inputs() const {
        return !closing && model_tab && showing_design_inputs() &&
            !host.draft_editing_locked() && !host.has_pending_receipt();
    }
    void finish_grid_selection(std::uint64_t time) {
        if (!can_select_model_inputs()) host.session().cancel_pending_click();
        else if (host.session().flush_click(time)) host.changed();
    }
    void update_model_presentation() {
        if (!generation_view || closing) return;
        const bool content = !showing_design_inputs();
        const bool complete = content && generation_view->ready();
        const bool readonly = model_host.creation_editing_locked();
        const bool can_restore = readonly && can_return && can_return();
        const bool actions_visible = readonly || complete;
        const wxString hint = !readonly ? wxString{} : can_restore ?
            t("当前为只读查看，点击“返回AI创作”恢复编辑") :
            t("当前为只读查看，工作台处理中，完成后可返回AI创作");
        const bool hint_changed = creation_lock_hint_text != hint;
        const bool completion_changed = completion_actions->IsShown() != actions_visible ||
            creation_lock_hint->IsShown() != readonly || hint_changed;
        std::unique_ptr<wxWindowUpdateLocker> content_handoff;
        if (generation_view->IsShown() != content)
            content_handoff = std::make_unique<wxWindowUpdateLocker>(grid->GetParent());
        show(grid, !content); show(generation_view, content);
        show(preview_navigation, !content);
        completion_actions->set_read_only(readonly);
        show(completion_actions, actions_visible);
        show(model, model_tab && !complete && !readonly);
        show(generate, !model_tab && !readonly);
        if (hint_changed) {
            creation_lock_hint_text = hint;
            creation_lock_hint->SetLabel(hint);
            creation_lock_hint->Wrap(page.FromDIP(312));
            layout_changed = true;
        }
        show(creation_lock_hint, readonly);
        const auto eligibility = host.eligibility();
        enable(generate, eligibility.click_generate_design); enable(model, eligibility.click_generate_model);
        completion_actions->set_enabled(readonly ? can_restore : eligibility.click_generate_model,
            readonly || (generation_view->ready() && host.model_state().can_open_workbench && !host.command_pending()));
        generation_view->set_presented(presented && content);
        if (completion_changed) {
            // The first-frame callback runs outside apply_snapshot(). Commit
            // only the footer geometry before its display protection ends.
            action_panel->GetParent()->Layout();
            action_panel->Layout();
        }
    }
    void apply_model_snapshot(bool new_generation = false) {
        if (!generation_view || closing) return;
        const auto context = host.session().context();
        auto state = host.model_state();
        std::optional<WorkbenchOpenRequest> request;
        const auto& group = host.session().group();
        if (!group || (!new_generation && group->model_job_id != state.job_id)) state = {};
        else if (host.model_artifact() && host.model_artifact()->job_id == state.job_id) {
            request.emplace(); request->entry = *host.model_artifact();
            request->artifact.local_path = request->entry.model_path;
            request->artifact.job_id = request->entry.job_id;
            request->sha256 = request->entry.content_sha256;
            request->origin = WorkbenchOpenRequest::Origin::Advanced;
            request->generation = state.model_generation_session;
            request->return_context = "advanced";
        }
        ModelGenerationStatusText::localize_advanced_model_state(state);
        generation_view->apply_snapshot({CreationSource::Advanced, context.group_id, context.epoch}, state, std::move(request));
        if (new_generation) generation_view->generation_started();
        update_model_presentation();
    }
    void polish_prompt() {
        if (!input_editable() || polish_pending) return;
        const auto context = host.session().command_context();
        AITextClient::PolishRequest request;
        request.user_description = host.session().draft().design.prompt;
        auto reference = std::make_shared<PolishReference>();
        const auto& upload = host.session().draft().uploads[0];
        if (!upload.empty()) {
            wxImage image(wxString(boost::filesystem::path(upload).wstring()));
            if (image.IsOk()) {
                const double scale = std::min(1.0, 2048.0 / std::max(image.GetWidth(), image.GetHeight()));
                if (scale < 1.0) image.Rescale(std::max(1, int(image.GetWidth() * scale)), std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
                reference->path = wxFileName::CreateTempFileName("orca-advanced-polish-");
            }
            if (!image.IsOk() || reference->path.empty() || !image.SaveFile(reference->path, wxBITMAP_TYPE_PNG)) {
                show_redesign_details(&page, t("参考图暂时无法读取，原图和描述已保留。"), t("AI 润色暂不可用")); return;
            }
            request.reference_image_path = boost::filesystem::path(reference->path.ToStdWstring());
            request.has_reference_image = true;
        }
        polish_pending = true; render();
        auto* target = &page;
        const auto alive = std::weak_ptr<int>(lifetime);
        polish_request = text_client->polish_image_prompt(request,
            [target, alive, context, reference](AITextClient::PolishResult result) {
                wxGetApp().CallAfter([target, alive, context, result] {
                    if (alive.expired() || target->m_impl->closing || !target->IsShownOnScreen()) return;
                    auto& self = *target->m_impl;
                    if (!self.host.session().matches(context.callback)) return;
                    self.polish_pending = false;
                    if (self.host.session().matches_command(context, false)) {
                        if (!result.optimized_prompt.empty() &&
                            show_generation_confirmation(target, t(result.optimized_prompt.c_str()), t("使用润色后的描述？")) == wxID_YES) {
                            if (!self.model_host.creation_editing_locked() && self.host.session().matches_command(context, false)) {
                                auto draft = self.host.session().draft().design; draft.prompt = result.optimized_prompt;
                                self.host.session().set_design_draft(std::move(draft)); self.host.changed();
                            }
                        } else if (result.optimized_prompt.empty()) {
                            wxString feedback;
                            for (const auto& warning : result.warnings) { if (!feedback.empty()) feedback += "\n"; feedback += t(warning.c_str()); }
                            if (feedback.empty()) feedback = result.status == "needs_input" ? t("请补充描述内容。") : t("描述存在歧义，请补充后重试。");
                            show_redesign_details(target, feedback, t("请补充润色描述"));
                        }
                    }
                    self.render();
                });
            }, [target, alive, context, reference](AITextClient::Error error) {
                wxGetApp().CallAfter([target, alive, context, error] {
                    if (alive.expired() || target->m_impl->closing || !target->IsShownOnScreen()) return;
                    if (!target->m_impl->host.session().matches(context.callback)) return;
                    target->m_impl->polish_pending = false;
                    const auto message = error.code == "description_too_long" ? t("描述超过 2000 字符，请精简后再润色。") :
                        error.code == "result_too_long" ? t("润色结果超过 2000 字符，已保留原描述。") : t(error.message.c_str());
                    show_redesign_details(target, message, t("AI 润色暂不可用")); target->render();
                });
            });
    }
    AdvancedCreationFeatureHost::Confirm model_confirmation() {
        auto* target = &page;
        const auto alive = std::weak_ptr<int>(lifetime);
        return [target, alive](const nlohmann::json& plan) {
            if (alive.expired()) return false;
            const auto& options = plan.at("options");
            wxString views;
            const auto& snapshot = plan.at("approved_multiview_snapshot");
            for (const auto& name : snapshot.at("selected_views")) {
                if (const auto view = parse_view(name.get<std::string>()); view) {
                    if (!views.empty()) views += t("、");
                    views += title(*view);
                }
            }
            auto message = wxString::Format(t("使用 %d 张设计图生成3D模型。"),
                static_cast<int>(snapshot.at("selected_views").size())) +
                t("\n生成方式：") + (snapshot.at("input_mode") == "image" ? t("单图生成") : t("多视图生成")) +
                t("\n生成范围：") + views + t("\n供应商：") +
                t(options.at("provider").get<std::string>().c_str()) + t("\n模型：") + t(options.at("geometry_model").get<std::string>().c_str()) +
                (options.at("face_limit") == 0 ? t("\n面数：供应商默认") :
                    wxString::Format(t("\n目标面数：%d"), options.at("face_limit").get<int>())) +
                t("\n本次最多提交1个3D任务，不会新增生图请求。");
            if (options.at("output_format") == "obj") message += t("\nOBJ 还需最多1次格式转换任务。");
            return show_generation_confirmation(target, message, t("确认生成3D模型"), 105, 240) == wxID_YES;
        };
    }

    AdvancedCreationFeatureHost::Confirm confirmation() {
        auto* target = &page;
        const auto alive = std::weak_ptr<int>(lifetime);
        return [target, alive](const nlohmann::json& plan) {
            if (alive.expired()) return false;
            const auto& draft = plan.at("draft");
            wxString views;
            for (const auto& value : plan.at("targets")) {
                const auto view = parse_view(value.get<std::string>()); if (!view) continue;
                if (!views.empty()) views += t("、"); views += title(*view);
            }
            const auto calls = plan.at("max_new_image_calls").get<int>();
            auto message = t("生成范围：") + views + t("\n图像供应商：") + t(draft.at("design_provider").get<std::string>().c_str()) +
                t("\n图像模型：") + image_model_display_name(draft.at("design_model").get<std::string>()) +
                (calls == 0 ? t("\n复用已取得的设计结果，不新增生图调用。确认后恢复。") :
                 wxString::Format(t("\n本次最多生成 %d 张设计图。确认后开始生成。"), calls));
            if (plan.contains("dependency_views") && !plan.at("dependency_views").empty())
                message += t("\n需先补生成主视角，再生成依赖它的视角；上方范围及张数已包含该主视角。");
            return show_generation_confirmation(target, message, t("确认生成2D设计图")) == wxID_YES;
        };
    }
    void request_generation(bool model_action) {
        if (closing) return;
        const auto eligible = host.eligibility();
        if (generation_feedback && (model_action ? eligible.generate_model : eligible.generate_design))
            generation_feedback({});
        const auto reject = [this, model_action](const std::string& reason) {
            if (model_action && reason == "design_result_unknown") {
                label(status, model_reason_text(reason, host.session().effective_model_views()));
            } else if (generation_feedback) generation_feedback(reason);
        };
        if (model_action && generation_view && generation_view->has_content()) open_model_selection();
        else if (model_action) host.generate_model(model_confirmation(), reject);
        else host.generate_design(confirmation(), reject);
    }
    wxString model_reason_text(const std::string& reason, const std::vector<ViewId>& selected) const {
        if (reason != "design_result_unknown") return reason_text(reason);
        if (const auto& group = host.session().group()) {
            wxString affected;
            for (const auto view : selected) {
                if (group->views[index(view)].attempt_state != "ambiguous") continue;
                if (view == ViewId::Front)
                    return t("主视角的上次生成结果未确认，请返回2D，点击主视角的星芒重新生成。");
                if (!affected.empty()) affected += t("、");
                affected += title(view);
            }
            if (!affected.empty())
                return affected + t("的上次生成结果未确认。请取消勾选这些视角，或返回2D点击对应星芒重新生成。");
        }
        return t("所选视角的上次生成结果未确认，请重新核对选图。");
    }
    void open_model_selection() {
        const auto reject = [this](const std::string& reason) {
            if (reason == "design_result_unknown") {
                label(status, model_reason_text(reason, host.session().effective_model_views()));
            } else if (generation_feedback) generation_feedback(reason);
        };
        if (!host.begin_model_selection(reject)) return;
        auto* top = wxGetTopLevelParent(&page);
        wxDialog shade(top,wxID_ANY,{},wxDefaultPosition,wxDefaultSize,wxBORDER_NONE|wxFRAME_NO_TASKBAR|wxFRAME_FLOAT_ON_PARENT);
        shade.SetBackgroundColour(*wxBLACK); shade.SetName("generation-confirm-backdrop");
        if (top) shade.SetSize(top->GetScreenRect());
        const bool dimmed = top && shade.SetTransparent(140);
        if (dimmed) shade.ShowWithoutActivating();
        RedesignMessageDialog dialog(dimmed?static_cast<wxWindow*>(&shade):&page,wxEmptyString,t("确认生成3D模型"),
            wxYES_NO,RedesignMessageDialog::Appearance::GenerationConfirmation,0,false,
            {{wxID_NO,t("取消"),false},{wxID_YES,t("确认生成"),true}});
        dialog.set_dismiss_result(wxID_NO); dialog.enable_action(wxID_YES,false);
        dialog.SetName("advanced-model-selection-confirmation");
        struct SelectionDialogState {
            std::array<ViewCard*,4> cards {};
            wxStaticText* summary {nullptr};
            nlohmann::json plan;
            bool submitted {false};
        };
        auto state = std::make_shared<SelectionDialogState>();
        auto dialog_lifetime = std::make_shared<int>(0);
        const auto dialog_alive = std::weak_ptr<int>(dialog_lifetime);
        const auto page_alive = std::weak_ptr<int>(lifetime);
        wxWeakRef<RedesignMessageDialog> weak_dialog(&dialog);
        auto* body = new wxPanel(&dialog); body->SetBackgroundColour(dialog.GetBackgroundColour());
        auto* body_layout = new wxBoxSizer(wxVERTICAL); body->SetSizer(body_layout);
        auto* card_row = new wxBoxSizer(wxHORIZONTAL); body_layout->Add(card_row,0,wxEXPAND|wxBOTTOM,page.FromDIP(12));
        state->summary = new wxStaticText(body, wxID_ANY, wxEmptyString);
        style_text(state->summary,primary_text_colour(),10); body_layout->Add(state->summary,0,wxEXPAND);
        const auto update = [this,state,dialog_alive,page_alive,weak_dialog] {
            if (dialog_alive.expired() || page_alive.expired() || !weak_dialog || !host.session().model_candidate()) return;
            const auto& candidate = *host.session().model_candidate();
            const auto& group = *host.session().group();
            for (auto view : AllViews) {
                const auto i = index(view); const auto& image = group.views[i];
                state->cards[i]->display(image.usable?image.image_path:"",candidate.selected[i],
                    wxEmptyString, false, image.sha256);
            }
            const auto count = host.session().candidate_model_views().size();
            state->plan = nullptr; weak_dialog->enable_action(wxID_YES,false);
            state->summary->SetLabel(wxString::Format(t("本次选择 %d 张设计图（%s），正在核对生成计划…"),
                static_cast<int>(count),count==1?t("单图生成"):t("多视图生成")));
            state->summary->Wrap(page.FromDIP(408));
            weak_dialog->fit_confirmation_content();
            host.prepare_model_selection([this,state,dialog_alive,page_alive,weak_dialog](nlohmann::json plan) {
                if (dialog_alive.expired() || page_alive.expired() || !weak_dialog) return;
                state->plan = std::move(plan);
                const auto& options = state->plan.at("options");
                const auto& snapshot = state->plan.at("approved_multiview_snapshot");
                wxString views;
                for (const auto& name : snapshot.at("selected_views")) {
                    if (auto view = parse_view(name.get<std::string>()); view) {
                        if (!views.empty()) views += t("、"); views += title(*view);
                    }
                }
                auto message = wxString::Format(t("使用 %d 张设计图生成3D模型。"),static_cast<int>(snapshot.at("selected_views").size()))+
                    t("\n生成方式：")+(snapshot.at("input_mode")=="image"?t("单图生成"):t("多视图生成"))+
                    t("\n生成范围：")+views+t("\n供应商：")+t(options.at("provider").get<std::string>().c_str())+
                    t("\n模型：")+t(options.at("geometry_model").get<std::string>().c_str())+
                    (options.at("face_limit")==0?t("\n面数：供应商默认"):
                        wxString::Format(t("\n目标面数：%d"),options.at("face_limit").get<int>()))+
                    t("\n本次最多提交1个3D任务，不会新增生图请求。");
                if (options.at("output_format")=="obj") message += t("\nOBJ 还需最多1次格式转换任务。");
                state->summary->SetLabel(message); state->summary->Wrap(page.FromDIP(408));
                weak_dialog->enable_action(wxID_YES,true);
                weak_dialog->fit_confirmation_content();
            },[this,state,dialog_alive,page_alive,weak_dialog](std::string reason) {
                if (dialog_alive.expired() || page_alive.expired() || !weak_dialog) return;
                auto message = model_reason_text(reason, host.session().candidate_model_views());
                if (message.empty()) message = ModelGenerationStatusText::localized_advanced_service_error(reason);
                state->summary->SetLabel(message); state->summary->Wrap(state->summary->FromDIP(408));
                weak_dialog->enable_action(wxID_YES,false);
                weak_dialog->fit_confirmation_content();
            });
        };
        for (auto view : AllViews) {
            const auto i = index(view);
            auto* card = new ViewCard(body, view == ViewId::Front ? t("主视角(必选)") : title(view));
            state->cards[i] = card;
            card->set_confirmation_caption();
            card->SetName(t("advanced-model-candidate-")+t(view_name(view)));
            card->SetMinSize(page.FromDIP(wxSize(98,132))); card_row->Add(card,1,wxEXPAND|(i?wxLEFT:0),i?page.FromDIP(5):0);
            card->Bind(wxEVT_LEFT_UP,[this,view,update](wxMouseEvent&) { if (host.session().toggle_candidate_view(view)) update(); });
            card->Bind(wxEVT_LEFT_DCLICK,[](wxMouseEvent&) {});
        }
        dialog.set_confirmation_content(body,[this,state,reject] {
            if (state->plan.is_null()) return false;
            state->submitted = host.confirm_model_selection(state->plan,reject);
            return state->submitted;
        });
        update(); dialog.ShowModal(); dialog_lifetime.reset();
        if (!state->submitted) host.cancel_model_selection();
    }
    bool accept_click(AdvancedPreviewClickSequence& sequence, const wxMouseEvent& event) const {
        auto* source = dynamic_cast<wxWindow*>(event.GetEventObject());
        if (!source) return false;
        const auto point = source->ClientToScreen(event.GetPosition());
        return sequence.accept(now_ms(), point.x, point.y, double_click_ms(), page.FromDIP(4));
    }
    void clear_preview_gestures() {
        for (auto* card : cards) if (card) card->clear_pointer();
        if (single_card) single_card->clear_pointer();
        checkbox_click_sequence.reset();
    }
    void reset_preview() {
        preview_single = false; preview_entered_single = false; preview_view = ViewId::Front;
        clear_preview_gestures();
        preview_click_sequence.reset();
        preview_context = host.session().context(); close_local_popover(); layout_changed = true;
    }
    void set_preview_mode(bool single) {
        if (!showing_design_inputs() || preview_single == single) return;
        finish_grid_selection(std::numeric_limits<std::uint64_t>::max());
        click_timer.Stop();
        clear_preview_gestures();
        preview_single = single;
        if (single && !preview_entered_single) { preview_view = ViewId::Front; preview_entered_single = true; }
        if (single && local_popover->IsShown() && local_view != preview_view) close_local_popover();
        layout_changed = true; render();
    }
    void close_local_popover() {
        ++local_sequence; local_pending = false;
        if (local_popover) local_popover->clear();
    }
    void position_local_popover(bool opening = false) {
        if (!local_popover || (!opening && !local_popover->IsShown())) return;
        const auto size = grid->GetClientSize();
        const auto* card = !model_tab && preview_single ? single_card : cards[index(local_view)];
        const auto anchor = card->GetPosition() + card->local_button_bounds().GetPosition();
        const auto extent = page.FromDIP(wxSize(360, 164));
        const int width = std::min(extent.x, size.x), height = std::min(extent.y, size.y);
        const int x = std::clamp(anchor.x, 0, std::max(0, size.x - width));
        const int y = std::clamp(anchor.y - page.FromDIP(8) - height, 0, std::max(0, size.y - height));
        const auto origin = grid->ClientToScreen(wxPoint(x,y));
        local_popover->SetSize(origin.x,origin.y,width,height);
        if (local_popover->IsShown()) { local_popover->Raise(); local_popover->Refresh(false); }
    }
    void open_local_popover(ViewId view) {
        if (model_tab || !input_editable() || !host.session().has_local_design_target(view)) return;
        close_local_popover(); local_view = view; position_local_popover(true);
        local_popover->Show(); local_popover->Raise(); local_popover->Refresh(false); local_popover->focus_input();
    }
    void submit_local_design(std::string text) {
        if (local_pending || !local_popover->IsShown() || model_tab) return;
        local_pending = true; local_popover->set_busy(true);
        const auto sequence = local_sequence; const auto context = host.session().context();
        const auto alive = std::weak_ptr<int>(lifetime);
        host.generate_single_design(local_view,std::move(text),[this,alive,context,sequence](bool accepted) {
            if (alive.expired() || closing || sequence != local_sequence || !host.session().matches(context)) return;
            local_pending = false;
            if (accepted) close_local_popover(); else local_popover->set_busy(host.has_pending_receipt());
        },[this,alive](const std::string& reason) { if (!alive.expired() && generation_feedback) generation_feedback(reason); });
    }
    bool input_editable() const {
        return !model_host.creation_editing_locked() && !polish_pending && model_host.can_replace_asset() && !host.draft_editing_locked() && !host.has_pending_receipt() && (!host.session().group() ||
               (host.session().group()->active_operation_id.empty() && !host.session().group()->model_busy));
    }
    void choose(ViewId view) {
        if (!input_editable()) return;
        if (view != ViewId::Front && host.session().draft().uploads[0].empty()) {
            if (generation_feedback) generation_feedback("missing_front"); return;
        }
        wxFileDialog picker(&page, t("选择") + title(view), wxEmptyString, wxEmptyString,
            t("PNG、JPEG、静态 WebP|*.png;*.jpg;*.jpeg;*.webp"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (picker.ShowModal() == wxID_OK) accept(view, picker.GetPath());
    }
    void accept(ViewId view, const wxString& path) {
        if (!input_editable()) return;
        const auto upload_path = boost::filesystem::path(path.ToStdWstring()).string();
        if (auto reason = host.session().upload_change_rejection(view,upload_path,true); !reason.empty()) {
            if (generation_feedback) generation_feedback(reason); return;
        }
        bool valid = false;
        try { valid = ModelGenerationPresentation::is_supported_image(boost::filesystem::path(path.ToStdWstring())); }
        catch (const boost::filesystem::filesystem_error&) {}
        if (!valid) {
            show_redesign_details(&page, t("请选择宽高至少 64 px、不超过 20MB 的 PNG、JPEG 或静态 WebP 图片。"), t("图片无法使用")); return;
        }
        host.session().set_upload(view, upload_path, true);
        uploads->invalidate_image(view); host.changed();
    }
    void layout_inputs() {
        if (!input_content || !input_content->GetSizer()) return;
        const auto client = input_scroll->GetClientSize();
        const int height = std::max(client.y, input_content->GetSizer()->CalcMin().y);
        int x = 0, y = 0;
        input_scroll->CalcScrolledPosition(0, 0, &x, &y);
        const wxRect bounds(x,y,std::max(0,client.x),height);
        if (input_content->GetRect()!=bounds) input_content->SetSize(bounds);
        input_content->Layout();
        if (input_scroll->GetVirtualSize()!=wxSize(std::max(0,client.x),height))
            input_scroll->SetVirtualSize(std::max(0,client.x),height);
    }
    void layout_grid()
    {
        const auto size = grid->GetClientSize();
        const bool single = preview_single;
        // Keep the original sizing budget, then center the fitted preview in the viewport.
        const int height_reserve = std::clamp(size.y - page.FromDIP(single ? 740 : 708), page.FromDIP(8), page.FromDIP(single ? 76 : 108));
        const wxSize available(size.x, std::max(0, size.y - height_reserve));
        if (single) {
            const auto preferred = page.FromDIP(wxSize(548, 740));
            const auto bounds = center_design_preview(size, fit_design_preview(available, preferred));
            if (single_card->GetRect() != bounds) single_card->SetSize(bounds);
        }
        show(single_card, single);
        const auto design = design_preview_grid(size, available.y, page.FromDIP(wxSize(260, 350)), page.FromDIP(8));
        for (auto view : AllViews) {
            const auto i = index(view);
            show(cards[i], !single);
            if (single) continue;
            const auto& bounds = design[i];
            if (cards[i]->GetRect() != bounds) cards[i]->SetSize(bounds);
        }
        position_local_popover();
    }
    void show_large(ViewId view) {
        if (!host.session().group()) return;
        const auto& design = host.session().group()->views[index(view)];
        if (!design.usable) return;
        wxDialog dialog(&page, wxID_ANY, title(view), wxDefaultPosition, page.FromDIP(wxSize(900, 800)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        const int display_index = wxDisplay::GetFromWindow(&page);
        const auto area = wxDisplay(static_cast<unsigned>(display_index == wxNOT_FOUND ? 0 : display_index)).GetClientArea();
        dialog.SetSize(std::min(dialog.GetSize().x, area.width - page.FromDIP(32)), std::min(dialog.GetSize().y, area.height - page.FromDIP(32)));
        dialog.SetBackgroundColour(background_colour());
        auto* sizer = new wxBoxSizer(wxVERTICAL); dialog.SetSizer(sizer);
        auto* image = new DesignDetailPreview(&dialog, design.image_path, design.sha256);
        wxWeakRef<DesignDetailPreview> weak_image(image);
        dialog.Bind(wxEVT_SHOW, [weak_image](wxShowEvent& event) {
            event.Skip();
            if (event.IsShown() && weak_image) weak_image->CallAfter([weak_image] {
                if (weak_image && weak_image->IsShownOnScreen()) weak_image->load();
            });
        });
        sizer->Add(image, 1, wxEXPAND | wxALL, page.FromDIP(12));
        auto* close = new RoundedActionButton(&dialog, t("关闭"), false, 36);
        close->SetMinSize(page.FromDIP(wxSize(120, 36)));
        close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        sizer->Add(close, 0, wxALIGN_CENTER | wxBOTTOM, page.FromDIP(12));
        dialog.SetEscapeId(close->GetId()); dialog.CentreOnParent(); dialog.ShowModal();
    }
    std::string history_view_signature() const {
        std::string signature;
        const auto append = [&signature](const std::string& value) {
            signature += std::to_string(value.size()) + ":" + value;
        };
        if (host.history().is_array()) {
            for (const auto& entry : host.history()) {
                try {
                    append(entry.value("id", std::string()));
                    append(entry.value("display_name", std::string()));
                    const auto& views = entry.at("views");
                    for (auto view : AllViews) {
                        const auto& state = views.at(view_name(view));
                        const auto version = state.value("current_version_id", std::string());
                        append(version);
                    }
                } catch (const nlohmann::json::exception&) {
                    append("damaged-history-entry");
                }
            }
        }
        const auto* group = host.session().group() ? &*host.session().group() : nullptr;
        if (!group) return signature;
        append(group->id);
        append(group->display_name);
        for (const auto& view : group->views) append(view.version_id);
        return signature;
    }
    void open_history_folder(const std::string& id) {
        if (closing || !presented || !page.IsShownOnScreen() ||
            !AdvancedMultiviewClient::valid_id(id) || !navigation_allowed()) return;
        const auto directory = ModelGenerationPresentation::generated_models_root() / "advanced-multiview" / id / "designs";
        boost::system::error_code error;
        const auto status = boost::filesystem::status(directory, error);
        wxString message;
        // Boost on Windows may retain ENOENT for a file_not_found status.
        if (status.type() == boost::filesystem::file_not_found &&
            (!error || error == boost::system::errc::no_such_file_or_directory)) {
            message = t("未找到该套图的本地设计图文件夹。");
        } else {
            if (!error && boost::filesystem::is_directory(status)) {
                // Check access without scanning versions or changing any files.
                (void)boost::filesystem::directory_iterator(directory, error);
                if (!error && wxLaunchDefaultApplication(wxString(directory.wstring()))) return;
            }
            message = t("无法打开设计图文件夹，请检查目录是否可访问。");
        }
        RedesignMessageDialog dialog(wxGetTopLevelParent(&page), message, t("在文件夹中打开"), wxOK | wxICON_WARNING);
        dialog.ShowModal();
    }
    void show_history_context_menu(const std::string& id, wxPoint screen_position) {
        if (closing || !presented || !page.IsShownOnScreen() ||
            !AdvancedMultiviewClient::valid_id(id) || !navigation_allowed()) return;
        wxWindow* const menu_host = wxGetTopLevelParent(&page);
        if (!menu_host) return;
        wxMenu menu;
        menu.Append(wxID_OPEN, t("在文件夹中打开"));
        const auto alive = std::weak_ptr<int>(lifetime);
        menu.Bind(wxEVT_MENU, [this, alive, id](wxCommandEvent&) {
            if (!alive.expired()) open_history_folder(id);
        }, wxID_OPEN);
        // The top-level window survives card replacement while PopupMenu pumps events.
        menu_host->PopupMenu(&menu, menu_host->ScreenToClient(screen_position));
    }
    void rebuild_history() { history_dirty=true; render(); }
    void apply_history() {
        history_dirty=false; history_layout_changed=true; layout_changed=true;
        if (!history_rows || !history_search) return;
        std::vector<HistorySetPreview> entries;
        const auto query = history_search->GetValue().Lower();
        const auto cache = ModelGenerationPresentation::generated_models_root() / "advanced-cache";
        if (host.history().is_array()) {
            for (const auto& entry : host.history()) {
                try {
                    HistorySetPreview preview;
                    preview.id = entry.value("id", std::string());
                    preview.revision = entry.value("revision", std::uint64_t(0));
                    if (!AdvancedMultiviewClient::valid_id(preview.id) || !entry.contains("saved_draft") ||
                        !entry["saved_draft"].is_object() || !entry.contains("views") || !entry["views"].is_object())
                        continue;
                    preview.display_name = entry.value("display_name", std::string());
                    preview.prompt = t(entry["saved_draft"].value("prompt", std::string()).c_str());
                    for (auto view : AllViews) {
                        const auto slot = index(view);
                        const auto name = view_name(view);
                        if (!entry["views"].contains(name) || !entry["views"][name].is_object()) continue;
                        const auto& result = entry["views"][name];
                        if (!result.contains("current_version_id") || !result["current_version_id"].is_string())
                            continue;
                        const auto version = result["current_version_id"].get<std::string>();
                        if (!AdvancedMultiviewClient::valid_id(version)) continue;
                        preview.images[slot] = AdvancedMultiviewClient::asset_path(cache, preview.id, version).string();
                        ++preview.available;
                    }
                    if (host.session().group() && host.session().group()->id == preview.id) {
                        preview.revision = host.session().group()->revision;
                        preview.display_name = host.session().group()->display_name;
                        preview.available = 0;
                        for (auto view : AllViews) {
                            const auto slot = index(view);
                            const auto& current = host.session().group()->views[slot];
                            if (!current.usable) continue;
                            preview.images[slot] = current.image_path;
                            ++preview.available;
                        }
                    }
                    preview.caption = preview.display_name.empty() ? preview.prompt : t(preview.display_name.c_str());
                    if (preview.caption.empty()) preview.caption = t("未命名套图");
                    if (!query.empty() && !preview.caption.Lower().Contains(query) && !preview.prompt.Lower().Contains(query))
                        continue;
                    entries.push_back(std::move(preview));
                } catch (const nlohmann::json::exception&) {
                    // A damaged set stays on disk; a later direct open reports its precise failure.
                }
            }
        }

        constexpr std::size_t page_size = 8;
        history_pages = std::max<std::size_t>(1, (entries.size() + page_size - 1) / page_size);
        history_page = std::min(history_page, history_pages - 1);
        history_rows->Clear(true);
        const auto selected = host.session().group() ? host.session().group()->id : std::string();
        const auto begin = history_page * page_size;
        const auto end = std::min(entries.size(), begin + page_size);
        for (auto i = begin; i < end; ++i) {
            const bool current = entries[i].id == selected;
            auto* card = new HistorySetCard(history_scroll, std::move(entries[i]), current,
                [this](const std::string& id) {
                    host.open_group(id);
                },
                [this](const std::string& id, std::uint64_t revision, std::string name) {
                    if (host.session().group() && host.session().group()->id == id)
                        revision = host.session().group()->revision;
                    host.rename_group(id, revision, std::move(name));
                },
                [this](const std::string& id, wxPoint position) { show_history_context_menu(id, position); });
            card->Enable(navigation_allowed());
            history_rows->Add(card, 1, wxEXPAND);
        }
        history_page_label->SetLabel(wxString::Format("%llu / %llu",
            static_cast<unsigned long long>(history_page + 1), static_cast<unsigned long long>(history_pages)));
        history_first->Enable(history_page > 0); history_previous->Enable(history_page > 0);
        history_next->Enable(history_page + 1 < history_pages); history_last->Enable(history_page + 1 < history_pages);
        for (auto* button : {history_first, history_previous, history_next, history_last}) button->Refresh(false);
        const wxString status_text = entries.empty()
            ? (query.empty() ? t("还没有历史套图") : t("没有匹配的套图"))
            : wxString::Format(t("%llu 组套图"), static_cast<unsigned long long>(entries.size()));
        history_status->SetLabel(status_text);
        history_signature = history_view_signature();
    }
    void render() { if (!closing) refresh.request(Presentation::Dirty::Content); }
    void apply_snapshot(Presentation::Dirty flags) {
        if (closing) return;
        layout_changed=layout_changed || Presentation::has(flags,Presentation::Dirty::Layout);
        if (presented && !host.command_pending() && !host.has_pending_receipt() &&
            host.session().consume_initial_design_guidance())
            switch_stage(true);
        const auto& session = host.session(); const auto& draft = session.draft(); const auto eligibility = host.eligibility();
        stage_tabs->set_model_tab(model_tab);
        show(design_inputs, !model_tab); show(model_inputs, model_tab);
        const bool expanded = session.sidebar_expanded();
        show(history_panel, expanded); label(drawer, expanded ? t("›") : t("‹"));
        const wxString drawer_name = expanded ? "advanced-history-collapse" : "advanced-history-expand";
        if (drawer->GetName() != drawer_name) {
            drawer->SetName(drawer_name);
            drawer->SetToolTip(expanded ? t("收起历史套图") : t("展开历史套图"));
        }
        const auto context = session.context();
        if (preview_context.epoch != context.epoch) {
            if (context.group_id.empty() || (!preview_context.group_id.empty() && preview_context.group_id != context.group_id))
                reset_preview();
            else close_local_popover();
            preview_context = context;
        }
        preview_grid_button->set_selected(!preview_single); preview_single_button->set_selected(preview_single);
        const auto card_selection = model_tab ? session.model_selection() : std::array<bool,4>{};
        std::array<bool, 4> model_input_enabled {};
        if (can_select_model_inputs()) {
            for (auto view : AllViews) model_input_enabled[index(view)] = session.can_toggle_model_view(view);
        }
        single_card->set_model_input_preview(model_tab);
        single_card->set_thumbnail_selection(card_selection, model_input_enabled);
        for (auto view : AllViews) {
            const auto i = index(view);
            uploads->display(view, draft.uploads[i], input_editable());
            wxString state, detail; std::string path; bool loading = false;
            if (session.group()) {
                const auto& result = session.group()->views[i]; path = result.usable ? result.image_path : "";
                const auto& attempt = result.attempt_state;
                if (attempt == "queued") {
                    state = t("排队中");
                    detail = t("等待前一个请求结束后开始生成。");
                } else if (design_attempt_generating(attempt) || result.asset_pending) {
                    state = t("生成中…"); loading = true;
                } else if (attempt == "ambiguous") {
                    state = t("结果未确认");
                    detail = t("上次生成结果未确认，可以重新生成。");
                } else if (design_attempt_recoverable(attempt)) {
                    state = t("结果待恢复");
                    detail = t("请先点击「核对本次请求」恢复已取得的结果。");
                } else if (design_attempt_failed(attempt) || result.display_error) {
                    state = t("生成失败");
                    detail = result.display_error ? t("图片获取失败，已取得的结果和文件保留。") : t(result.error_message.c_str());
                    if (detail.empty()) detail = t(result.error_code.c_str());
                    if (detail.empty()) detail = t("本次未成功生成该视角。");
                    if (!model_tab) detail += t("\n可通过左下角星芒添加局部描述并重新生成。");
                } else if (result.usable) state = !model_tab && session.group()->pending_views[i] ? t("待重新生成") : t("设计图");
            }
            const auto revision = session.group() ? session.group()->views[i].sha256 : std::string{};
            const bool local_visible = !model_tab && session.has_local_design_target(view);
            if (view == local_view && !local_visible && local_popover->IsShown()) close_local_popover();
            const bool local_enabled = local_visible && input_editable();
            cards[i]->set_model_input_preview(model_tab);
            cards[i]->set_picture_selectable(model_input_enabled[i]);
            cards[i]->display(path, card_selection[i], state, loading, revision);
            cards[i]->set_local_available(local_visible, local_enabled);
            if (detail.empty()) cards[i]->UnsetToolTip(); else cards[i]->SetToolTip(detail);
            if (preview_single) {
                single_card->display_thumbnail(view, path, revision, view == preview_view);
                if (view == preview_view) {
                    single_card->set_caption(title(view));
                    single_card->display(path, false, state, loading, revision);
                    single_card->set_local_available(local_visible, local_enabled);
                    if (detail.empty()) single_card->UnsetToolTip(); else single_card->SetToolTip(detail);
                }
            }
        }
        if (local_popover->IsShown()) {
            if (model_tab || !input_editable()) local_popover->set_busy(true);
            else { local_pending = false; local_popover->set_busy(false); }
            local_popover->Raise();
        }
        const auto value = t(draft.design.prompt.c_str()); if (prompt->GetValue() != value) prompt->ChangeValue(value);
        enable(prompt, input_editable()); enable(provider, input_editable());
        const bool yoyo_image = draft.design.provider == "yoyoclaw";
        show(image_model_label, yoyo_image); show(image_model, yoyo_image);
        image_model_ids.clear(); wxArrayString image_model_choices; int image_model_selection = -1;
        const auto& image_catalog = host.image_catalog();
        if (yoyo_image && image_catalog.ready()) {
            for (const auto& item : image_catalog.models) {
                if (!item.available) continue;
                if (item.id == draft.design.model) image_model_selection = static_cast<int>(image_model_ids.size());
                image_model_ids.push_back(item.id);
                image_model_choices.Add(image_model_display_name(item.id));
            }
        }
        ProviderStatus model_status {"yoyoclaw", image_catalog.ready() ? "ready" : image_catalog.state,
                                     image_catalog.connection_state, 0};
        image_model->set_choices(std::move(image_model_choices), image_model_selection);
        image_model->set_placeholder(image_catalog.ready() ? t("请选择模型") : provider_status_caption(model_status));
        enable(image_model, yoyo_image && input_editable() && image_catalog.ready() && !image_model_ids.empty());
        enable(custom_style, input_editable());
        if (custom_style->GetValue() != t(draft.design.custom_style.c_str())) custom_style->ChangeValue(t(draft.design.custom_style.c_str()));
        const bool yoyo_geometry = draft.geometry.provider == "yoyoclaw";
        enable(geometry_provider, input_editable()); enable(face_limit, input_editable() && !yoyo_geometry);
        wxArrayString geometry_choices {t("Tripo"), t("YOYOClaw")};
        const int geometry_selection = yoyo_geometry ? 1 : 0;
        geometry_provider->set_choices(std::move(geometry_choices), geometry_selection);
        wxArrayString face_choices {yoyo_geometry ? t("供应商默认面数") : t("100 万面（低模，推荐）")};
        if (!yoyo_geometry) face_choices.Add(t("200 万面（高模）"));
        int face_selection = !yoyo_geometry && draft.geometry.face_limit == 2000000 ? 1 : 0;
        if (!yoyo_geometry && draft.geometry.face_limit == 300000) {
            face_choices.Add(t("30 万面（历史设置）")); face_selection = static_cast<int>(face_choices.size()) - 1;
        }
        face_limit->set_choices(std::move(face_choices), face_selection);
        label(geometry_quality_value, draft.geometry.geometry_quality == "detailed" ? t("精细") : t("标准"));
        label(texture_quality_value, draft.geometry.texture_quality == "extreme" ? t("极致") :
                                        draft.geometry.texture_quality == "detailed" ? t("精细") : t("标准"));
        label(output_format_value, t(draft.geometry.output_format == "obj" ? "OBJ" : "GLB"));
        enable(polish, input_editable() && draft.design.prompt.size() <= 2000 && (!draft.design.prompt.empty() || !draft.uploads[0].empty()));
        label(polish, polish_pending ? t("正在润色…") : t("AI润色"));
        provider->set_selection(draft.design.provider == "yoyoclaw" ? 1 : 0, false);
        refresh_styles(); refresh_provider_status(false); refresh_provider_status(true);
        const bool over_limit = draft.design.prompt.size() > 2000;
        label(count, wxString::Format("%zu/2000", draft.design.prompt.size()));
        count->SetForegroundColour(over_limit ? wxColour(255, 128, 112) :
            smart_creation_opaque_colour(wxColour(255, 255, 255, 115), wxColour(21, 21, 23))); count->Refresh();
        label(count_error, over_limit ? t("描述超过 2000 字符，请缩短后生成。") : wxEmptyString);
        show(count_error, over_limit);
        enable(generate, eligibility.click_generate_design); enable(model, eligibility.click_generate_model);
        const auto* group = session.group() ? &*session.group() : nullptr;
        show(stop, eligibility.stop); enable(stop, eligibility.stop);
        show(reconcile, host.has_pending_receipt() || (!model_tab && eligibility.recover));
        enable(reconcile, !host.command_pending());
        wxString status_text = model_tab ? model_reason_text(eligibility.model_reason, session.effective_model_views()) :
            reason_text(eligibility.design_reason);
        if (group && group->model_state == "ready" && eligibility.model_reason.empty())
            status_text = t("已有3D模型，可查看生成时的设计图版本。");
        const auto raw_model_state = host.model_state();
        auto model_display = raw_model_state;
        const bool current_model = group && !group->model_job_id.empty() && group->model_job_id == model_display.job_id;
        if (current_model) ModelGenerationStatusText::localize_advanced_model_state(model_display);
        if (group && group->model_busy)
            status_text = current_model ? t(model_display.status_text.c_str()) : t("正在处理当前3D模型任务…");
        if (group && !group->active_operation_id.empty()) status_text = group->execution_state == "queued" ?
            t("排队中：等待前一个请求结束，随后开始生成设计图。") : t("正在生成设计图，成功结果会逐张更新。");
        if (group && group->stop_intent) status_text = t("已停止本地后续生成；远端已发请求可能仍完成。原图和已取得结果保留。");
        if (!host.message().empty()) {
            const auto stage = raw_model_state.stage;
            const bool model_notice = model_tab && group && (group->model_busy || (current_model &&
                (stage == ModelGenerationUIStage::GeneratingModel || stage == ModelGenerationUIStage::LoadingModel ||
                 stage == ModelGenerationUIStage::Stopping || stage == ModelGenerationUIStage::Stopped ||
                 stage == ModelGenerationUIStage::Failed)));
            status_text = model_notice ? (current_model ? t(model_display.status_text.c_str()) :
                t("正在处理当前3D模型任务…")) : t(host.message().c_str());
        }
        if (presented && !model_tab && group && group->active_operation_id.empty() && !host.has_pending_receipt()) {
            std::string feedback;
            bool unknown = false;
            for (size_t i = 0; i < group->views.size(); ++i) {
                const auto& view = group->views[i];
                unknown = unknown || view.attempt_state == "ambiguous";
                if (view.attempt_state != "failed" && view.attempt_state != "ambiguous" &&
                    !design_attempt_recoverable(view.attempt_state)) continue;
                const auto key = view.operation_id + "/" + view.attempt_state;
                if (announced_design_failures[i] == key) continue;
                announced_design_failures[i] = key;
                if (design_attempt_recoverable(view.attempt_state)) feedback = "recovery_required";
                else if (view.attempt_state == "ambiguous" && feedback != "recovery_required") feedback = "design_result_unknown";
                else if (feedback.empty()) feedback = view.error_code == "image_connection_failed" ?
                    "design_connection_failed" : "design_generation_failed";
            }
            if (unknown && status_text.empty()) status_text = t("上次生成结果未确认，可以重新生成。");
            if (!feedback.empty() && generation_feedback && !model_host.creation_editing_locked()) generation_feedback(feedback);
        }
        label(status, model_host.creation_editing_locked() ? wxString{} : status_text);
        enable(fresh_button, !model_host.creation_editing_locked());
        if (history_dirty || history_view_signature() != history_signature) apply_history();
        enable(history_refresh, !host.command_pending());
        apply_model_snapshot();
        for (auto* child : history_scroll->GetChildren())
            if (auto* card = dynamic_cast<HistorySetCard*>(child)) enable(card, navigation_allowed());
        if (layout_changed) {
            const auto started=std::chrono::steady_clock::now();
            applying_layout=true;
            page.Layout(); layout_inputs(); layout_grid();
            if (history_layout_changed) { history_scroll->Layout(); history_scroll->FitInside(); }
            wxString trace;
            if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE",&trace) && trace=="1")
                BOOST_LOG_TRIVIAL(info) << "[UiAdvancedLayout] elapsed_ms="
                    << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            layout_changed=false; history_layout_changed=false;
            applying_layout=false;
        }
        sync_image_activity();
    }
};

AdvancedCreationWorkspace::AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
                                                     std::function<void()> show_smart, ModelGenerationFeatureHost& shared_models,
                                                     std::function<void(const ModelGenerationUIState&)> model_state_changed,
                                                     std::function<void(const AdvancedCreation::ModelJobEvent&)> model_job_event,
                                                     std::function<void()> open_workbench, std::function<void()> completed,
                                                     std::function<void(const std::string&)> generation_feedback)
    : wxPanel(parent), m_impl(std::make_unique<Impl>(*this, host, std::move(show_smart), shared_models, std::move(model_state_changed), std::move(model_job_event)))
{
    m_impl->open_workbench = std::move(open_workbench);
    m_impl->completed = std::move(completed);
    m_impl->generation_feedback = std::move(generation_feedback);
    auto* target = this;
    const auto alive = std::weak_ptr<int>(m_impl->lifetime);
    host.set_listener([target, alive] { if (!alive.expired()) target->render(); });
}
AdvancedCreationWorkspace::~AdvancedCreationWorkspace()
{
    m_impl->shutdown();
}
void AdvancedCreationWorkspace::render() { m_impl->render(); }
void AdvancedCreationWorkspace::set_presented(bool visible) { m_impl->set_presented(visible); }
void AdvancedCreationWorkspace::set_service_available(bool available) { m_impl->set_service_available(available); }
bool AdvancedCreationWorkspace::navigation_allowed() { return m_impl->navigation_allowed(); }
void AdvancedCreationWorkspace::set_creation_return_handler(std::function<void()> action, std::function<bool()> allowed) {
    m_impl->return_creation = std::move(action); m_impl->can_return = std::move(allowed);
}
void AdvancedCreationWorkspace::shutdown() { m_impl->shutdown(); }
void AdvancedCreationWorkspace::apply_model_state(const ModelGenerationUIState&, bool new_generation) {
    m_impl->apply_model_snapshot(new_generation);
}
std::optional<WorkbenchOpenRequest> AdvancedCreationWorkspace::displayed_artifact() const {
    return m_impl->generation_view ? m_impl->generation_view->displayed_artifact() : std::nullopt;
}
}
