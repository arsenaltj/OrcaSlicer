#include "AdvancedCreationWorkspace.hpp"
#include "RedesignWidgets.hpp"
#include "RedesignMessageDialog.hpp"
#include "SmartCreationControls.hpp"
#include "../AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../AI/Text/AITextClient.hpp"
#include "../AISidecarClient.hpp"
#include "../GUI_App.hpp"
#include "../wxExtensions.hpp"
#include <wx/display.h>
#include <wx/choice.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/dnd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/popupwin.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <algorithm>
#include <chrono>
#include <cmath>
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
    static const std::array<wxString, 4> names {t("正视角"), t("左视角"), t("后视角"), t("右视角")};
    return names.at(index(view));
}
wxString central_title(ViewId view)
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
    if (reason == "missing_current_view") return t("请先补齐四个视角的设计图。");
    if (reason == "recovery_required") return t("存在未完成的请求，请先恢复原任务结果。");
    if (reason == "invalid_input") return t("请上传主参考图或填写描述，描述最多 2000 字节。");
    if (reason == "missing_front") return t("已上传辅助视角，请补充必填的主参考图。");
    if (reason == "design_regeneration_required") return t("上传图片或风格已修改，请先重新生成对应设计图。");
    if (reason == "draft_changed") return t("输入已修改，请先生成对应设计图。");
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

wxBitmap cover_bitmap(const wxImage& source, int width, int height, int radius)
{
    if (!source.IsOk() || width <= 0 || height <= 0) return {};

    const double scale = std::max(static_cast<double>(width) / source.GetWidth(),
                                  static_cast<double>(height) / source.GetHeight());
    const int scaled_width = std::max(width, static_cast<int>(std::ceil(source.GetWidth() * scale)));
    const int scaled_height = std::max(height, static_cast<int>(std::ceil(source.GetHeight() * scale)));
    wxImage scaled = source.Scale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH);
    if (!scaled.IsOk()) return {};

    const int crop_x = std::max(0, (scaled_width - width) / 2);
    // Figma's portrait references keep the subject's head near the upper third.
    const int crop_y = std::max(0, static_cast<int>(std::lround((scaled_height - height) * 0.1)));
    wxImage cropped = scaled.GetSubImage(wxRect(crop_x, crop_y, width, height));
    if (!cropped.IsOk()) return {};
    if (!cropped.HasAlpha()) cropped.InitAlpha();

    const int clipped_radius = std::clamp(radius, 0, std::min(width, height) / 2);
    if (clipped_radius > 0) {
        unsigned char* alpha = cropped.GetAlpha();
        const double center = clipped_radius - 0.5;
        const double limit = clipped_radius * clipped_radius;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                double dx = 0.0;
                double dy = 0.0;
                if (x < clipped_radius) dx = center - x;
                else if (x >= width - clipped_radius) dx = x - (width - clipped_radius - 0.5);
                if (y < clipped_radius) dy = center - y;
                else if (y >= height - clipped_radius) dy = y - (height - clipped_radius - 0.5);
                if (dx > 0.0 && dy > 0.0 && dx * dx + dy * dy > limit)
                    alpha[y * width + x] = 0;
            }
        }
    }
    return wxBitmap(cropped);
}

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
#endif

private:
#ifdef __WXMSW__
    int m_wheel_rotation {0};
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
    if (m_choices != choices && m_popup) { m_popup->Destroy(); m_popup = nullptr; }
    m_choices = std::move(choices);
    m_selection = selection >= 0 && selection < static_cast<int>(m_choices.size()) ? selection : -1;
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
        : wxPanel(parent), m_view(view), m_choose(std::move(choose)), m_remove(std::move(remove))
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
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { m_bitmap = {}; Refresh(); event.Skip(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (!m_editable) return;
            if (!m_path.empty() && remove_rect().Contains(event.GetPosition())) m_remove();
            else if (slot_rect().Contains(event.GetPosition())) m_choose();
        });
        update_tooltip();
    }

    void display(const std::string& path, bool editable)
    {
        // Snapshots can expose the immutable upload path before its cache
        // download completes. Reload when that same path becomes available.
        boost::system::error_code cache_error;
        const bool reload = path != m_path || (!path.empty() && !m_image.IsOk() &&
            boost::filesystem::is_regular_file(boost::filesystem::path(path), cache_error));
        if (reload) {
            m_path = path;
            m_image = {};
            m_bitmap = {};
            if (!path.empty()) m_image.LoadFile(wxString(boost::filesystem::path(path).wstring()));
        }
        if (!reload && m_editable == editable && path == m_painted_path) return;
        m_editable = editable;
        m_painted_path = path;
        SetCursor(wxCursor(editable ? wxCURSOR_HAND : wxCURSOR_ARROW));
        update_tooltip();
        Refresh();
    }

private:
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
        if (!m_bitmap.IsOk() || m_bitmap.GetWidth() != slot.width || m_bitmap.GetHeight() != slot.height)
            m_bitmap = cover_bitmap(m_image, slot.width, slot.height, FromDIP(8));
        if (m_bitmap.IsOk()) gc.DrawBitmap(m_bitmap, slot.x, slot.y, slot.width, slot.height);
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
        if (!m_path.empty() && m_image.IsOk()) draw_image(*gc);
        else draw_empty(dc, *gc);
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->SetPen(wxPen(wxColour(255, 255, 255, 77), std::max(1, FromDIP(1)), wxPENSTYLE_SHORT_DASH));
        gc->DrawRoundedRectangle(slot.x + 0.5, slot.y + 0.5, slot.width - 1.0, slot.height - 1.0, FromDIP(8));
        if (!m_path.empty() && m_image.IsOk()) draw_remove(*gc);
    }

    ViewId m_view;
    std::function<void()> m_choose;
    std::function<void()> m_remove;
    std::string m_path;
    std::string m_painted_path;
    wxImage m_image;
    wxBitmap m_bitmap;
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

private:
    int dip(double value) const { return static_cast<int>(std::lround(value * GetDPIScaleFactor())); }

    void layout_slots()
    {
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
};

// The text endpoint accepts PNG/JPEG. Keep an advanced-only decoded PNG alive
// until the asynchronous request finishes, including cancellation callbacks.
struct PolishReference {
    wxString path;
    ~PolishReference() { if (!path.empty()) wxRemoveFile(path); }
};

// Rendering only. The Session owns selection and the click/double-click state machine.
class ViewCard final : public wxPanel {
public:
    ViewCard(wxWindow* parent, wxString caption, bool upload = false)
        : wxPanel(parent), m_caption(std::move(caption)), m_loading_timer(this), m_upload(upload) {
        SetMinSize(wxSize(0, 0)); SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour()); SetCursor(wxCursor(wxCURSOR_HAND));
        style_text(this, primary_text_colour(), 11);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { m_bitmap = {}; Refresh(); event.Skip(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            m_spinner_phase = (m_spinner_phase + 1) % 24;
            Refresh(false);
        }, m_loading_timer.GetId());
    }
    ~ViewCard() override { m_loading_timer.Stop(); }
    void display(const std::string& path, bool selected, wxString status, bool loading = false) {
        if (path != m_path) {
            m_path = path; m_image = {}; m_bitmap = {};
            if (!path.empty()) m_image.LoadFile(wxString(boost::filesystem::path(path).wstring()));
        }
        if (m_selected == selected && m_status == status && path == m_painted_path && m_loading == loading) return;
        m_painted_path = path; m_selected = selected; m_status = std::move(status);
        if (m_loading != loading) {
            m_loading = loading;
            m_spinner_phase = 0;
            if (m_loading) m_loading_timer.Start(80);
            else m_loading_timer.Stop();
        }
        Refresh();
    }
private:
    void paint() {
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(background_colour())); dc.Clear();
        const auto size = GetClientSize(); if (size.x < 2 || size.y < 2) return;
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if (!gc) return;
        gc->SetBrush(wxBrush(panel_colour()));
        gc->SetPen(wxPen(m_selected ? accent_colour() : m_hover ? wxColour(104, 104, 110) : wxColour(64, 64, 69), FromDIP(m_selected ? 2 : 1)));
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(12));
        const LayoutRect image_area {FromDIP(10), FromDIP(10), std::max(0, size.x - FromDIP(20)),
                                     std::max(0, size.y - FromDIP(47))};
        const auto bounds = contain_image(image_area,
                                          m_image.IsOk() ? m_image.GetWidth() : 0, m_image.IsOk() ? m_image.GetHeight() : 0);
        if (bounds.width > 0 && bounds.height > 0) {
            if (!m_bitmap.IsOk() || m_bitmap.GetWidth() != bounds.width || m_bitmap.GetHeight() != bounds.height)
                m_bitmap = wxBitmap(m_image.Scale(bounds.width, bounds.height, wxIMAGE_QUALITY_HIGH));
            dc.DrawBitmap(m_bitmap, bounds.x, bounds.y, true);
        } else if (!m_loading) {
            dc.SetFont(GetFont()); dc.SetTextForeground(secondary_text_colour());
            const wxString placeholder = m_upload ? t("+ 上传图片") : t("等待生成"); const auto extent = dc.GetTextExtent(placeholder);
            dc.DrawText(placeholder, (size.x - extent.x) / 2, std::max(0, (size.y - extent.y) / 2 - FromDIP(12)));
        }
        if (m_loading && image_area.width > FromDIP(36) && image_area.height > FromDIP(36)) {
            constexpr double pi = 3.14159265358979323846;
            const double center_x = image_area.x + image_area.width * 0.5;
            const double center_y = image_area.y + image_area.height * 0.5;
            const double radius = FromDIP(16);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(20, 20, 22, 184)));
            gc->DrawEllipse(center_x - radius - FromDIP(7), center_y - radius - FromDIP(7),
                            (radius + FromDIP(7)) * 2, (radius + FromDIP(7)) * 2);
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(wxColour(255, 255, 255, 58), FromDIP(3)));
            gc->DrawEllipse(center_x - radius, center_y - radius, radius * 2, radius * 2);
            const double start = m_spinner_phase * pi / 12.0;
            auto arc = gc->CreatePath();
            arc.AddArc(center_x, center_y, radius, start, start + pi * 1.35, true);
            gc->SetPen(wxPen(accent_colour(), FromDIP(3)));
            gc->StrokePath(arc);
        }
        dc.SetFont(GetFont()); dc.SetTextForeground(primary_text_colour());
        dc.DrawText(wxControl::Ellipsize(m_caption, dc, wxELLIPSIZE_END, std::max(0, size.x - FromDIP(24))), FromDIP(12), size.y - FromDIP(29));
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
    std::string m_path, m_painted_path;
    wxImage m_image;
    wxBitmap m_bitmap;
    wxTimer m_loading_timer;
    int m_spinner_phase {0};
    bool m_selected {false}, m_hover {false}, m_loading {false}, m_upload {false};
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
                   std::function<void(const std::string&, std::uint64_t, std::string)> rename)
        : wxPanel(parent), m_preview(std::move(preview)), m_open_timer(this), m_selected(selected),
          m_open(std::move(open)), m_rename(std::move(rename))
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
        for (std::size_t i = 0; i < m_preview.images.size(); ++i) {
            if (!m_preview.images[i].empty())
                m_images[i].LoadFile(wxString(boost::filesystem::path(m_preview.images[i]).wstring()));
        }
        const wxString details = m_preview.caption + t("\n") +
            wxString::Format(t("%llu/4 个视角"), static_cast<unsigned long long>(m_preview.available));
        SetToolTip(details);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            m_bitmap_size = wxDefaultSize;
            m_bitmaps.fill(wxBitmap());
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

    void rebuild_bitmaps(const wxSize& cell)
    {
        if (m_bitmap_size == cell) return;
        m_bitmap_size = cell;
        for (std::size_t i = 0; i < m_images.size(); ++i) {
            m_bitmaps[i] = wxBitmap();
            if (!m_images[i].IsOk()) continue;
            const auto fitted = contain_image({0, 0, cell.x, cell.y}, m_images[i].GetWidth(), m_images[i].GetHeight());
            if (fitted.width > 0 && fitted.height > 0)
                m_bitmaps[i] = wxBitmap(m_images[i].Scale(fitted.width, fitted.height, wxIMAGE_QUALITY_HIGH));
        }
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
        rebuild_bitmaps({cell_edge, cell_edge});
        static const std::array<const char*, 4> placeholders {"正", "左", "后", "右"};
        for (std::size_t i = 0; i < 4; ++i) {
            const int col = static_cast<int>(i % 2), row = static_cast<int>(i / 2);
            const wxRect cell(mosaic_x + col * (cell_edge + gap), margin + row * (cell_edge + gap),
                              cell_edge, cell_edge);
            gc->SetBrush(wxBrush(wxColour(22, 22, 25)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(cell.x, cell.y, cell.width, cell.height, FromDIP(4));
            if (m_bitmaps[i].IsOk()) {
                dc.DrawBitmap(m_bitmaps[i], cell.x + (cell.width - m_bitmaps[i].GetLogicalWidth()) / 2,
                              cell.y + (cell.height - m_bitmaps[i].GetLogicalHeight()) / 2, true);
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
    std::array<wxImage, 4> m_images;
    std::array<wxBitmap, 4> m_bitmaps;
    wxSize m_bitmap_size {wxDefaultSize};
    wxTimer m_open_timer;
    wxTextCtrl* m_editor {nullptr};
    bool m_selected {false};
    bool m_hover {false};
    bool m_editing {false};
    std::function<void(const std::string&)> m_open;
    std::function<void(const std::string&, std::uint64_t, std::string)> m_rename;
};
}

struct AdvancedCreationWorkspace::Impl {
    AdvancedCreationWorkspace& page;
    AdvancedCreationFeatureHost& host;
    std::array<ViewCard*, 4> cards {};
    UploadAreaPanel* uploads {nullptr};
    std::array<wxStaticText*, 4> tags {};
    std::array<std::uint64_t, 4> suppress_until {};
    std::unique_ptr<AITextClient> text_client;
    std::shared_ptr<AITextClient::RequestHandle> polish_request;
    wxPanel* creation {nullptr};
    bool polish_pending {false}, closing {false};
    std::function<bool()> can_replace_asset;
    wxPanel* grid {nullptr};
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
    RoundedActionButton* view_model {nullptr};
    RoundedActionButton* resume {nullptr};
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
    struct ProviderStatus {
        std::string provider;
        std::string state;
        std::string connection_state;
        std::uint64_t sequence {0};
    } image_status, geometry_status;

    Impl(AdvancedCreationWorkspace& owner, AdvancedCreationFeatureHost& feature, std::function<void()> show_smart,
         std::function<bool()> can_replace)
        : page(owner), host(feature), can_replace_asset(std::move(can_replace)), click_timer(&owner) {
        page.SetBackgroundColour(background_colour());
        auto* root = new wxBoxSizer(wxVERTICAL); page.SetSizer(root);
        creation = new wxPanel(&page); creation->SetBackgroundColour(background_colour());
        auto* outer = new wxBoxSizer(wxHORIZONTAL); creation->SetSizer(outer); root->Add(creation, 1, wxEXPAND);
        text_client = std::make_unique<AITextClient>(AISidecarClient::default_endpoint());
        host.set_start_guard([this] { return !polish_pending && (!can_replace_asset || can_replace_asset()); });
        page.Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
            if (!event.IsShown() && polish_request) { polish_request->cancel(); polish_request.reset(); polish_pending = false; }
            if (event.IsShown()) switch_stage(false, true);
            event.Skip();
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
        input_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { layout_inputs(); event.Skip(); });
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
        fresh->SetName("advanced-design-new-creation");
        fresh->SetMinSize(page.FromDIP(wxSize(112, 32)));
        fresh->SetMaxSize(page.FromDIP(wxSize(112, 32)));
        fresh->set_corner_radius(16);
        style_smart_creation_text(fresh, 13, wxColour(20, 20, 20), wxFONTWEIGHT_MEDIUM);
        fresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            click_timer.Stop(); suppress_until.fill(0);
            if (polish_request) { polish_request->cancel(); polish_request.reset(); }
            polish_pending = false;
            host.new_creation();
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
        auto* tag_row = new wxBoxSizer(wxHORIZONTAL);
        for (auto view : AllViews) {
            tags[index(view)] = new wxStaticText(prompt_box, wxID_ANY, title(view));
            tags[index(view)]->SetBackgroundColour(wxColour(77, 65, 32));
            style_smart_creation_text(tags[index(view)], 12, accent_colour(), wxFONTWEIGHT_MEDIUM);
            tag_row->Add(tags[index(view)], 0, wxRIGHT, page.FromDIP(6));
        }
        prompt_sizer->Add(tag_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(12));
        prompt = new AdvancedPromptTextCtrl(prompt_box, page.FromDIP(wxSize(-1, 48)));
        prompt->SetName("advanced-prompt"); prompt->SetBackgroundColour(wxColour(21, 21, 23));
        style_smart_creation_text(prompt, 13, wxColour(230, 230, 233));
        static_cast<AdvancedPromptTextCtrl*>(prompt)->set_hint_colour(
            smart_creation_opaque_colour(wxColour(255, 255, 255, 115), wxColour(21, 21, 23)));
        prompt->SetHint(t("描述你想创建的设计图，例如一只可爱的小猫"));
        prompt_sizer->Add(prompt, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(12));
        count = new wxStaticText(prompt_box, wxID_ANY, "0/2000");
        style_smart_creation_text(count, 12, wxColour(255, 255, 255, 115));
        count->SetToolTip(t("描述容量按 UTF-8 字节计算；汉字通常占 3 字节，英文通常占 1 字节。视角标签不计入。"));
        prompt_sizer->Add(count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM | wxTOP, page.FromDIP(12));
        inputs->Add(prompt_box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            auto draft = host.session().draft().design; draft.prompt = prompt->GetValue().ToUTF8().data();
            host.session().set_design_draft(std::move(draft)); host.changed();
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
            filter->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { style_filter = int(i); refresh_styles(); });
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
                host.session().set_design_draft(std::move(draft)); host.changed(); refresh_styles();
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
            auto draft = host.session().draft().design; draft.custom_style = custom_style->GetValue().ToUTF8().data();
            host.session().set_design_draft(std::move(draft)); host.changed();
        });
        auto* image_label = new wxStaticText(design_inputs, wxID_ANY, t("图像模型选择"));
        style_smart_creation_text(image_label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        inputs->Add(image_label, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        provider = new AdvancedChoicePicker(design_inputs, {t("GPT"), t("YOYOClaw")}, [this](int selection) {
            auto draft = host.session().draft().design; draft.provider = selection == 1 ? "yoyoclaw" : "gpt"; draft.model.clear();
            host.session().set_design_draft(std::move(draft)); host.changed();
            if (selection == 1) host.refresh_image_catalog(); else host.refresh_capabilities();
            image_status.provider.clear(); refresh_provider_status(false);
        });
        provider->SetName("advanced-image-provider-choice");
        inputs->Add(provider, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        image_model_label = new wxStaticText(design_inputs, wxID_ANY, t("生图模型"));
        style_smart_creation_text(image_model_label, 14, wxColour(255, 255, 255, 230), wxFONTWEIGHT_MEDIUM);
        inputs->Add(image_model_label, 0, wxLEFT | wxRIGHT | wxTOP, page.FromDIP(16));
        image_model = new AdvancedChoicePicker(design_inputs, {}, [this](int selection) {
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
        geometry_provider = new AdvancedChoicePicker(model_inputs, {t("Tripo")}, [this](int) { options_changed(); });
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
        generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.generate_design(confirmation()); });
        model = new RoundedActionButton(action_panel, t("生成3D模型"), true, 44);
        model->SetName("advanced-generate-model"); model->set_generation_gradient(true); model->set_corner_radius(22);
        style_smart_creation_text(model, 16, *wxWHITE, wxFONTWEIGHT_MEDIUM);
        actions->Add(model, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, page.FromDIP(16));
        model->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.generate_model(model_confirmation()); });
        view_model = new RoundedActionButton(action_panel, t("查看3D模型"), false, 32);
        view_model->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_model(); });
        actions->Add(view_model, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        resume = new RoundedActionButton(action_panel, t("恢复原3D任务"), false, 32);
        resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.resume_model(); });
        actions->Add(resume, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        stop = new RoundedActionButton(action_panel, t("停止生成"), false, 32);
        stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.stop(); });
        actions->Add(stop, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        reconcile = new RoundedActionButton(action_panel, t("恢复已确认请求"), false, 32);
        reconcile->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { host.reconcile_submission(); });
        actions->Add(reconcile, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, page.FromDIP(16));
        left->Add(action_panel, 0, wxEXPAND);
        model_inputs->Hide(); model->Hide(); view_model->Hide(); resume->Hide(); stop->Hide(); reconcile->Hide();
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
        grid = new wxPanel(center); grid->SetBackgroundColour(background_colour()); grid->SetMinSize(wxSize(0, 0));
        center_sizer->Add(grid, 1, wxEXPAND);
        for (auto view : AllViews) {
            const auto i = index(view); cards[i] = new ViewCard(grid, central_title(view)); cards[i]->SetName(t("advanced-design-") + t(view_name(view)));
            cards[i]->Bind(wxEVT_LEFT_UP, [this, view, i](wxMouseEvent&) {
                if (now_ms() <= suppress_until[i]) { suppress_until[i] = 0; return; }
                host.session().click(view, now_ms(), double_click_ms()); click_timer.StartOnce(double_click_ms() + 5); render();
            });
            cards[i]->Bind(wxEVT_LEFT_DCLICK, [this, view, i](wxMouseEvent&) {
                suppress_until[i] = now_ms() + double_click_ms(); click_timer.Stop();
                if (host.session().double_click(view)) show_large(view);
                suppress_until[i] = 0;
                render();
            });
        }
        grid->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { layout_grid(); event.Skip(); });
        page.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { host.session().flush_click(now_ms()); render(); }, click_timer.GetId());
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
            history_panel->Show(host.session().sidebar_expanded()); drawer->SetLabel(host.session().sidebar_expanded() ? t("›") : t("‹"));
            drawer->SetName(host.session().sidebar_expanded() ? "advanced-history-collapse" : "advanced-history-expand");
            drawer->SetToolTip(host.session().sidebar_expanded() ? t("收起历史套图") : t("展开历史套图")); drawer->Refresh();
            page.Layout(); layout_grid();
        });
    }

    ~Impl() { shutdown(); }
    void shutdown() {
        if (closing) return; closing = true;
        click_timer.Stop();
        if (polish_request) polish_request->cancel();
        host.set_start_guard({}); host.set_listener({});
    }
    void switch_stage(bool selected_model, bool reset = false) {
        if (!reset && model_tab == selected_model) return;
        model_tab = selected_model;
        if (stage_tabs) stage_tabs->set_model_tab(model_tab);
        if (design_inputs) design_inputs->Show(!model_tab);
        if (model_inputs) model_inputs->Show(model_tab);
        if (generate) generate->Show(!model_tab);
        if (model) model->Show(model_tab);
        if (input_scroll) input_scroll->Scroll(0, 0);
        if (input_content) input_content->Layout();
        if (action_panel) action_panel->Layout();
        page.Layout(); layout_inputs();
        if (generate && model) render();
    }
    void refresh_styles() {
        if (!style_grid) return;
        const auto& selected_style = host.session().draft().design.style;
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
        if (design_inputs) design_inputs->Layout();
        layout_inputs();
    }
    wxString provider_status_caption(const ProviderStatus& value) const {
        if (!service_available) return t("未连接");
        if (value.state.empty() || value.state == "checking") return t("检测中");
        if (value.state == "ready") return value.connection_state == "connected" ? t("已连接") : t("已配置");
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
        const std::string selected_provider = geometry ? "tripo" : host.session().draft().design.provider;
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
        wxWeakRef<AdvancedCreationWorkspace> weak(&page);
        host.get_provider_catalog(geometry ? "geometry" : "image", selected_provider,
            [weak, geometry, sequence, selected_provider](AIModelGenerationClient::ProviderCatalog catalog) {
                if (!weak) return;
                auto& self = *weak->m_impl;
                auto& status = geometry ? self.geometry_status : self.image_status;
                if (self.closing || status.sequence != sequence || status.provider != selected_provider) return;
                status.state = catalog.ready() ? "ready" : catalog.state;
                status.connection_state = catalog.connection_state;
                self.apply_provider_status(geometry);
            }, [weak, geometry, sequence, selected_provider](std::string) {
                if (!weak) return;
                auto& self = *weak->m_impl;
                auto& status = geometry ? self.geometry_status : self.image_status;
                if (self.closing || status.sequence != sequence || status.provider != selected_provider) return;
                status.state = "error"; status.connection_state.clear(); self.apply_provider_status(geometry);
            });
    }
    void set_service_available(bool available) {
        service_available = available;
        image_status.provider.clear(); image_status.state.clear(); ++image_status.sequence;
        geometry_status.provider.clear(); geometry_status.state.clear(); ++geometry_status.sequence;
        refresh_provider_status(false); refresh_provider_status(true);
    }
    void options_changed() {
        if (!geometry_provider || !face_limit) return;
        auto options = host.session().draft().geometry;
        options.provider = "tripo"; options.model.clear();
        options.face_limit = face_limit->selection() == 1 ? 2000000 :
                             face_limit->selection() == 2 ? 300000 : 1000000;
        options.geometry_quality = options.face_limit == 2000000 ? "detailed" : "standard";
        host.session().set_model_options(std::move(options)); host.changed();
    }
    bool navigation_allowed() { return true; }
    void open_model() {
        if (!host.session().group() || !input_editable()) return;
        host.show_model();
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
        polish_pending = true; render(); wxWeakRef<AdvancedCreationWorkspace> weak(&page);
        polish_request = text_client->polish_image_prompt(request,
            [weak, context, reference](AITextClient::PolishResult result) {
                if (weak) weak->CallAfter([weak, context, result] {
                    if (!weak || weak->m_impl->closing || !weak->IsShownOnScreen()) return;
                    auto& self = *weak->m_impl;
                    if (!self.host.session().matches(context.callback)) return;
                    self.polish_pending = false;
                    if (self.host.session().matches_command(context, false)) {
                        if (!result.optimized_prompt.empty() &&
                            show_generation_confirmation(weak, t(result.optimized_prompt.c_str()), t("使用润色后的描述？")) == wxID_YES) {
                            if (self.host.session().matches_command(context, false)) {
                                auto draft = self.host.session().draft().design; draft.prompt = result.optimized_prompt;
                                self.host.session().set_design_draft(std::move(draft)); self.host.changed();
                            }
                        } else if (result.optimized_prompt.empty()) {
                            wxString feedback;
                            for (const auto& warning : result.warnings) { if (!feedback.empty()) feedback += "\n"; feedback += t(warning.c_str()); }
                            if (feedback.empty()) feedback = result.status == "needs_input" ? t("请补充描述内容。") : t("描述存在歧义，请补充后重试。");
                            show_redesign_details(weak, feedback, t("请补充润色描述"));
                        }
                    }
                    self.render();
                });
            }, [weak, context, reference](AITextClient::Error error) {
                if (weak) weak->CallAfter([weak, context, error] {
                    if (!weak || weak->m_impl->closing || !weak->IsShownOnScreen()) return;
                    if (!weak->m_impl->host.session().matches(context.callback)) return;
                    weak->m_impl->polish_pending = false;
                    show_redesign_details(weak, t(error.message.c_str()), t("AI 润色暂不可用")); weak->render();
                });
            });
    }
    AdvancedCreationFeatureHost::Confirm model_confirmation() {
        wxWeakRef<AdvancedCreationWorkspace> weak(&page);
        return [weak](const nlohmann::json& plan) {
            if (!weak) return false;
            const auto& options = plan.at("options");
            auto message = t("使用正、左、后、右四张设计图生成同一个3D模型。\n供应商：") +
                t(options.at("provider").get<std::string>().c_str()) + t("\n模型：") + t(options.at("geometry_model").get<std::string>().c_str()) +
                wxString::Format(t("\n目标面数：%d\n本次最多提交1个3D任务，不会新增生图请求。"), options.at("face_limit").get<int>());
            if (options.at("output_format") == "obj") message += t("\nOBJ 还需最多1次格式转换任务。");
            return show_generation_confirmation(weak, message, t("确认生成3D模型")) == wxID_YES;
        };
    }

    AdvancedCreationFeatureHost::Confirm confirmation() {
        wxWeakRef<AdvancedCreationWorkspace> weak(&page);
        return [weak](const nlohmann::json& plan) {
            if (!weak) return false;
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
            return show_generation_confirmation(weak, message, t("确认生成2D设计图")) == wxID_YES;
        };
    }
    bool input_editable() const {
        return !polish_pending && (!can_replace_asset || can_replace_asset()) && !host.command_pending() && !host.has_pending_receipt() && (!host.session().group() ||
               (host.session().group()->active_operation_id.empty() && !host.session().group()->model_busy));
    }
    void choose(ViewId view) {
        if (!input_editable()) return;
        wxFileDialog picker(&page, t("选择") + title(view), wxEmptyString, wxEmptyString,
            t("PNG、JPEG、静态 WebP|*.png;*.jpg;*.jpeg;*.webp"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (picker.ShowModal() == wxID_OK) accept(view, picker.GetPath());
    }
    void accept(ViewId view, const wxString& path) {
        if (!input_editable()) return;
        bool valid = false;
        try { valid = ModelGenerationPresentation::is_supported_image(boost::filesystem::path(path.ToStdWstring())); }
        catch (const boost::filesystem::filesystem_error&) {}
        if (!valid) {
            show_redesign_details(&page, t("请选择宽高至少 64 px、不超过 20MB 的 PNG、JPEG 或静态 WebP 图片。"), t("图片无法使用")); return;
        }
        host.session().set_upload(view, boost::filesystem::path(path.ToStdWstring()).string()); host.changed();
    }
    void layout_inputs() {
        if (!input_content || !input_content->GetSizer()) return;
        const auto client = input_scroll->GetClientSize();
        const int height = std::max(client.y, input_content->GetSizer()->CalcMin().y);
        int x = 0, y = 0;
        input_scroll->CalcScrolledPosition(0, 0, &x, &y);
        input_content->SetSize(x, y, std::max(0, client.x), height);
        input_content->Layout();
        input_scroll->SetVirtualSize(std::max(0, client.x), height);
    }
    void layout_grid() {
        const auto size = grid->GetClientSize();
        const auto layout = grid_layout(size.x, size.y, page.FromDIP(4), page.FromDIP(14), page.FromDIP(34));
        for (auto view : AllViews) {
            const auto i = index(view); const auto& rect = layout.cards[i];
            cards[i]->SetSize(rect.x, rect.y, rect.width, rect.height);
        }
    }
    void show_large(ViewId view) {
        if (!host.session().group()) return;
        const auto path = host.session().group()->views[index(view)].image_path;
        wxDialog dialog(&page, wxID_ANY, title(view), wxDefaultPosition, page.FromDIP(wxSize(900, 800)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        const int display_index = wxDisplay::GetFromWindow(&page);
        const auto area = wxDisplay(static_cast<unsigned>(display_index == wxNOT_FOUND ? 0 : display_index)).GetClientArea();
        dialog.SetSize(std::min(dialog.GetSize().x, area.width - page.FromDIP(32)), std::min(dialog.GetSize().y, area.height - page.FromDIP(32)));
        dialog.SetBackgroundColour(background_colour());
        auto* sizer = new wxBoxSizer(wxVERTICAL); dialog.SetSizer(sizer);
        auto* image = new ViewCard(&dialog, title(view)); image->display(path, false, wxEmptyString);
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
    void rebuild_history() {
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
                [this](const std::string& id) { host.open_group(id); },
                [this](const std::string& id, std::uint64_t revision, std::string name) {
                    if (host.session().group() && host.session().group()->id == id)
                        revision = host.session().group()->revision;
                    host.rename_group(id, revision, std::move(name));
                });
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
        history_scroll->FitInside(); history_scroll->Layout();
        history_panel->Layout();
        history_signature = history_view_signature();
    }
    void render() {
        if (closing) return;
        const auto& session = host.session(); const auto& draft = session.draft(); const auto eligibility = host.eligibility();
        const auto selected = session.effective_views(); const bool all = selected.size() == 4;
        for (auto view : AllViews) {
            const auto i = index(view);
            tags[i]->SetLabel(all && i == 0 ? t(" 全部视角 ") : t(" ") + title(view) + t(" "));
            tags[i]->Show(all ? i == 0 : session.selection()[i]);
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
                } else if (design_attempt_failed(attempt) || result.display_error) {
                    state = t("生成失败");
                    detail = result.display_error ? t("图片获取失败，已取得的结果和文件保留。") : t(result.error_message.c_str());
                    if (detail.empty()) detail = t(result.error_code.c_str());
                    if (detail.empty()) detail = t("本次未成功生成该视角。");
                    detail += result.usable ? t("\n原图已保留，可选中后使用当前模型重新生成。") :
                                              t("\n可选中该空位，使用当前模型补生成。");
                } else if (result.usable) state = session.group()->pending_views[i] ? t("待重新生成") : t("设计图");
            }
            cards[i]->display(path, session.selection()[i], state, loading);
            if (detail.empty()) cards[i]->UnsetToolTip(); else cards[i]->SetToolTip(detail);
        }
        const auto value = t(draft.design.prompt.c_str()); if (prompt->GetValue() != value) prompt->ChangeValue(value);
        prompt->Enable(input_editable()); provider->Enable(input_editable());
        const bool yoyo_image = draft.design.provider == "yoyoclaw";
        image_model_label->Show(yoyo_image); image_model->Show(yoyo_image);
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
        image_model->Enable(yoyo_image && input_editable() && image_catalog.ready() && !image_model_ids.empty());
        custom_style->Enable(input_editable());
        if (custom_style->GetValue() != t(draft.design.custom_style.c_str())) custom_style->ChangeValue(t(draft.design.custom_style.c_str()));
        geometry_provider->Enable(input_editable()); face_limit->Enable(input_editable());
        wxArrayString geometry_choices {t("Tripo")}; int geometry_selection = 0;
        if (draft.geometry.provider == "yoyoclaw") {
            geometry_choices.Add(t("YOYOClaw（历史设置）")); geometry_selection = 1;
        }
        geometry_provider->set_choices(std::move(geometry_choices), geometry_selection);
        wxArrayString face_choices {t("100 万面（低模，推荐）"), t("200 万面（高模）")};
        int face_selection = draft.geometry.face_limit == 2000000 ? 1 : 0;
        if (draft.geometry.face_limit == 300000) {
            face_choices.Add(t("30 万面（历史设置）")); face_selection = 2;
        }
        face_limit->set_choices(std::move(face_choices), face_selection);
        geometry_quality_value->SetLabel(draft.geometry.geometry_quality == "detailed" ? t("精细") : t("标准"));
        texture_quality_value->SetLabel(draft.geometry.texture_quality == "extreme" ? t("极致") :
                                        draft.geometry.texture_quality == "detailed" ? t("精细") : t("标准"));
        output_format_value->SetLabel(t(draft.geometry.output_format == "obj" ? "OBJ" : "GLB"));
        polish->Enable(input_editable() && draft.design.prompt.size() <= 2000 && (!draft.design.prompt.empty() || !draft.uploads[0].empty()));
        polish->SetLabel(polish_pending ? t("正在润色…") : t("AI润色"));
        provider->set_selection(draft.design.provider == "yoyoclaw" ? 1 : 0, false);
        refresh_styles(); refresh_provider_status(false); refresh_provider_status(true);
        const bool over_limit = draft.design.prompt.size() > 2000;
        count->SetLabel(wxString::Format("%zu/2000", draft.design.prompt.size()));
        count->SetForegroundColour(over_limit ? wxColour(255, 128, 112) :
            smart_creation_opaque_colour(wxColour(255, 255, 255, 115), wxColour(21, 21, 23))); count->Refresh();
        count_error->SetLabel(over_limit ? t("描述超过 2000 字节，请缩短后生成。") : wxEmptyString);
        count_error->Show(over_limit);
        generate->Show(!model_tab); model->Show(model_tab);
        generate->Enable(eligibility.generate_design); model->Enable(eligibility.generate_model);
        const auto* group = session.group() ? &*session.group() : nullptr;
        view_model->Show(model_tab && group && group->model_state == "ready"); view_model->Enable(input_editable());
        resume->Show(model_tab && group && group->model_resumable &&
                     (group->model_state == "failed" || group->model_state == "stopped")); resume->Enable(input_editable());
        stop->Show(eligibility.stop); stop->Enable(eligibility.stop);
        reconcile->Show(host.has_pending_receipt()); reconcile->Enable(!host.command_pending());
        wxString status_text = reason_text(model_tab ? eligibility.model_reason : eligibility.design_reason);
        if (group && group->model_state == "ready" && eligibility.model_reason.empty())
            status_text = t("已有3D模型，可查看生成时的四视角版本。");
        if (group && group->model_busy) status_text = t(group->model_message.c_str());
        if (group && !group->active_operation_id.empty()) status_text = group->execution_state == "queued" ?
            t("排队中：等待前一个请求结束，随后开始生成设计图。") : t("正在生成设计图，成功结果会逐张更新。");
        if (group && group->stop_intent) status_text = t("已停止本地后续生成；远端已发请求可能仍完成。原图和已取得结果保留。");
        if (!host.message().empty()) status_text = t(host.message().c_str());
        status->SetLabel(status_text);
        if (history_view_signature() != history_signature) rebuild_history();
        history_refresh->Enable(!host.command_pending()); history_refresh->Refresh(false);
        for (auto* child : history_scroll->GetChildren())
            if (auto* card = dynamic_cast<HistorySetCard*>(child)) card->Enable(navigation_allowed());
        prompt->GetParent()->Layout();
        if (design_inputs) design_inputs->Layout();
        if (model_inputs) model_inputs->Layout();
        if (action_panel) action_panel->Layout();
        page.Layout(); layout_inputs(); layout_grid();
        // These actions are painted panels: wxPanel::Enable does not repaint their custom face.
        for (auto* button : {generate, model, polish, view_model, resume, stop, reconcile}) button->Refresh();
    }
};

AdvancedCreationWorkspace::AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
    std::function<void()> show_smart, std::function<bool()> can_replace)
    : wxPanel(parent), m_impl(std::make_unique<Impl>(*this, host, std::move(show_smart), std::move(can_replace)))
{
    wxWeakRef<AdvancedCreationWorkspace> weak(this);
    host.set_listener([weak] { if (weak) weak->render(); });
}
AdvancedCreationWorkspace::~AdvancedCreationWorkspace() = default;
void AdvancedCreationWorkspace::render() { m_impl->render(); }
void AdvancedCreationWorkspace::set_service_available(bool available) { m_impl->set_service_available(available); }
bool AdvancedCreationWorkspace::navigation_allowed() { return m_impl->navigation_allowed(); }
void AdvancedCreationWorkspace::shutdown() { m_impl->shutdown(); }
}
