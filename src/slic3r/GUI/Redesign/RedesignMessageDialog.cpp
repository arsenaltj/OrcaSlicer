#include "RedesignMessageDialog.hpp"
#include "../AI/ModelGeneration/ModelGenerationConfirmation.hpp"

#include "RedesignTheme.hpp"
#include "../I18N.hpp"
#include "../Widgets/Label.hpp"

#include <algorithm>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/checkbox.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/region.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/utils.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

namespace Slic3r::GUI {

namespace {
#ifdef __WXMSW__
void set_dark_dialog_border(wxWindow* window, const wxColour& color)
{
    if (!window || !window->GetHandle()) return;
    HMODULE library = ::LoadLibraryW(L"dwmapi.dll");
    if (!library) return;
    using SetAttribute = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto* set_attribute = reinterpret_cast<SetAttribute>(::GetProcAddress(library, "DwmSetWindowAttribute"));
    if (set_attribute) {
        constexpr DWORD border_color_attribute = 34; // DWMWA_BORDER_COLOR on Windows 11.
        const COLORREF border = RGB(color.Red(), color.Green(), color.Blue());
        set_attribute(static_cast<HWND>(window->GetHandle()), border_color_attribute, &border, sizeof(border));
    }
    ::FreeLibrary(library);
}
#endif
}

class RedesignDetailsContentPanel final : public wxPanel
{
public:
    RedesignDetailsContentPanel(wxWindow* parent, const wxString& text)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_text(text)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::panel_colour());
        RedesignTheme::style_text(this, RedesignTheme::primary_text_colour(), 12);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

    void set_text(const wxString& text, int width)
    {
        m_text = text;
        m_wrap_width = std::max(1, width);
        rebuild_lines();
        Refresh(false);
    }

    int line_height() const { return m_line_height; }

private:
    wxSize DoGetBestSize() const override
    {
        return wxSize(std::max(1, m_wrap_width),
                      std::max(1, m_line_height * static_cast<int>(m_lines.size())));
    }

    void rebuild_lines()
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());

        wxString wrapped;
        Label::split_lines(dc, std::max(1, m_wrap_width), m_text, wrapped);
        m_lines.clear();
        wxString current;
        for (size_t i = 0; i < wrapped.length(); ++i) {
            const wxUniChar character = wrapped[i];
            if (character == '\n') {
                m_lines.push_back(current);
                current.clear();
            } else if (character != '\r') {
                current += character;
            }
        }
        if (!current.empty() || m_lines.empty())
            m_lines.push_back(current);

        const wxSize extent = dc.GetTextExtent("Ag");
        m_line_height = std::max(1, extent.y + FromDIP(2));
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
        dc.Clear();
        dc.SetFont(GetFont());
        dc.SetTextForeground(GetForegroundColour());
        for (size_t index = 0; index < m_lines.size(); ++index)
            dc.DrawText(m_lines[index], 0, static_cast<int>(index) * m_line_height);
    }

    wxString m_text;
    std::vector<wxString> m_lines;
    int m_wrap_width {1};
    int m_line_height {1};
};

class RedesignDetailsViewport final : public wxPanel
{
public:
    RedesignDetailsViewport(wxWindow* parent, wxWindowID id,
                            const wxPoint& position, const wxSize& size, long style)
        : wxPanel(parent, id, position, size, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::panel_colour());

        m_scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          style | wxCLIP_CHILDREN | wxBORDER_NONE);
        m_scroller->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_scroller->SetBackgroundColour(RedesignTheme::panel_colour());
        m_scroller->SetScrollRate(0, FromDIP(12));
        m_scroller->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);

        m_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_bar->SetBackgroundColour(RedesignTheme::panel_colour());
        m_bar->SetCursor(wxCursor(wxCURSOR_HAND));
        m_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_bar(); });
        m_bar->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_bar_hovered = true;
            m_bar->Refresh(false);
            event.Skip();
        });
        m_bar->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_bar_hovered = false;
            m_bar->Refresh(false);
            event.Skip();
        });
        m_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { begin_bar_pointer(event); });
        m_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { drag_bar_pointer(event); });
        m_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { end_bar_pointer(); });
        m_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { end_bar_pointer(); });
        m_bar->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            // The themed bar is a sibling of the scroller. Forward the wheel
            // to the real scrolled window without making the bar part of the
            // moving content hierarchy.
            event.Skip(false);
            wxMouseEvent forwarded(event);
            forwarded.SetEventObject(m_scroller);
            m_scroller->GetEventHandler()->ProcessEvent(forwarded);
        });

        const auto refresh_bar = [this](wxScrollWinEvent& event) {
            update_bar();
            event.Skip();
        };
        m_scroller->Bind(wxEVT_SCROLLWIN_TOP, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_BOTTOM, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_LINEUP, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_LINEDOWN, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_PAGEUP, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_PAGEDOWN, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_THUMBTRACK, refresh_bar);
        m_scroller->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, refresh_bar);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            layout_children();
            event.Skip();
        });
        layout_children();
    }

    wxScrolledWindow* scroller() const { return m_scroller; }

    void set_content_window(wxWindow* content)
    {
        if (auto* details = dynamic_cast<RedesignDetailsContentPanel*>(content))
            m_scroller->SetScrollRate(0, std::max(1, details->line_height()));
    }

    void FitInside()
    {
        m_scroller->Layout();
        m_scroller->FitInside();
        update_bar();
    }

private:
    wxRect thumb_rect() const
    {
        const int height = m_bar->GetClientSize().y;
        const int virtual_height = std::max(1, m_scroller->GetVirtualSize().y);
        const int visible_height = m_scroller->GetClientSize().y;
        const int thumb_height = std::min(height,
            std::max(FromDIP(24), height * visible_height / virtual_height));
        int view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
        m_scroller->GetViewStart(&view_x, &view_y);
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int range = std::max(1, virtual_height - visible_height);
        const int top = std::clamp(int(double(view_y) * unit_y * (height - thumb_height) / range),
                                   0, std::max(0, height - thumb_height));
        return wxRect(FromDIP(3), top, FromDIP(4), thumb_height);
    }

    void paint_bar()
    {
        wxAutoBufferedPaintDC dc(m_bar);
        dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
        dc.Clear();
        if (!m_bar->IsShown() || m_scroller->GetVirtualSize().y <= m_scroller->GetClientSize().y)
            return;

        const auto thumb = thumb_rect();
        const wxColour thumb_colour = m_dragging ? RedesignTheme::accent_colour() :
            m_bar_hovered ? wxColour(150, 150, 155) : wxColour(105, 105, 112);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(thumb_colour));
        dc.DrawRoundedRectangle(thumb, FromDIP(2));
    }

    void begin_bar_pointer(wxMouseEvent& event)
    {
        const auto thumb = thumb_rect();
        if (thumb.Contains(event.GetPosition())) {
            m_drag_offset = event.GetY() - thumb.y;
            m_dragging = true;
            m_bar->CaptureMouse();
            m_bar->Refresh(false);
            return;
        }

        int view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
        m_scroller->GetViewStart(&view_x, &view_y);
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int page = m_scroller->GetClientSize().y / std::max(1, unit_y);
        m_scroller->Scroll(view_x, std::max(0, view_y + (event.GetY() < thumb.y ? -page : page)));
        update_bar();
    }

    void drag_bar_pointer(wxMouseEvent& event)
    {
        if (!m_dragging || !m_bar->HasCapture())
            return;

        const int travel = m_bar->GetClientSize().y - thumb_rect().height;
        int view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
        m_scroller->GetViewStart(&view_x, &view_y);
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int range = std::max(0, m_scroller->GetVirtualSize().y - m_scroller->GetClientSize().y);
        const int position = std::clamp(event.GetY() - m_drag_offset, 0, std::max(0, travel));
        m_scroller->Scroll(view_x, travel > 0 ?
            int(double(position) * range / travel / std::max(1, unit_y) + .5) : 0);
        update_bar();
    }

    void end_bar_pointer()
    {
        if (m_bar->HasCapture())
            m_bar->ReleaseMouse();
        if (m_dragging) {
            m_dragging = false;
            m_bar->Refresh(false);
        }
    }

    void layout_children()
    {
        if (!m_scroller || !m_bar)
            return;
        const wxSize size = GetClientSize();
        const int width = FromDIP(10);
        m_scroller->SetSize(0, 0, std::max(0, size.x - width), std::max(0, size.y));
        m_bar->SetSize(std::max(0, size.x - width), 0, width, std::max(0, size.y));
        m_bar->Raise();
        update_bar();
    }

    void update_bar()
    {
        if (!m_bar || !m_scroller)
            return;
        m_bar->Show(m_scroller->GetVirtualSize().y > m_scroller->GetClientSize().y);
        m_bar->Raise();
        m_bar->Refresh(false);
    }

    wxScrolledWindow* m_scroller {nullptr};
    wxPanel* m_bar {nullptr};
    int m_drag_offset {0};
    bool m_dragging {false};
    bool m_bar_hovered {false};
};

namespace {

constexpr int kCornerRadius = 10;

class DialogStatusIcon final : public wxPanel
{
public:
    DialogStatusIcon(wxWindow* parent, bool warning)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(36, 36)), wxBORDER_NONE)
        , m_warning(warning)
    {
        SetMinSize(FromDIP(wxSize(36, 36)));
        SetMaxSize(FromDIP(wxSize(36, 36)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

    void rescale()
    {
        SetMinSize(FromDIP(wxSize(36, 36)));
        SetMaxSize(FromDIP(wxSize(36, 36)));
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxColour accent = m_warning ? wxColour(235, 174, 78) : RedesignTheme::accent_colour();
        gc->SetBrush(wxBrush(accent));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawEllipse(0, 0, size.x, size.y);

        wxFontInfo info(17);
        info.Family(wxFONTFAMILY_SWISS).Bold();
        const wxString family = RedesignTheme::font_family();
        if (!family.empty())
            info.FaceName(family);
        dc.SetFont(wxFont(info));
        dc.SetTextForeground(wxColour(25, 25, 27));
        const wxString glyph = m_warning ? "!" : "?";
        const wxSize extent = dc.GetTextExtent(glyph);
        dc.DrawText(glyph, (size.x - extent.x) / 2, (size.y - extent.y) / 2);
    }

    bool m_warning { false };
};

}

class RedesignDialogActionButton final : public wxPanel
{
public:
    RedesignDialogActionButton(wxWindow* parent, wxWindowID id, const wxString& caption, bool primary,
                               int width = 112, bool design_confirmation = false)
        : wxPanel(parent, id, wxDefaultPosition, parent->FromDIP(wxSize(width, design_confirmation ? 48 : 38)), wxBORDER_NONE)
        , m_primary(primary), m_width(width), m_design_confirmation(design_confirmation)
    {
        SetLabel(caption);
        SetCanFocus(true);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        RedesignTheme::style_medium_text(this,
            primary ? wxColour(20, 20, 20) : RedesignTheme::primary_text_colour(),
            design_confirmation ? 15 : width == 170 ? 18 : 10);
        rescale();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = true;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = false;
            m_pressed = false;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (!IsEnabled())
                return;
            m_pressed = true;
            SetFocus();
            Refresh();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled() || !m_pressed)
                return;
            m_pressed = false;
            Refresh();
            wxCommandEvent command(wxEVT_BUTTON, GetId());
            command.SetEventObject(this);
            ProcessWindowEvent(command);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (IsEnabled() && (event.GetKeyCode() == WXK_RETURN ||
                                event.GetKeyCode() == WXK_NUMPAD_ENTER || event.GetKeyCode() == WXK_SPACE)) {
                wxCommandEvent command(wxEVT_BUTTON, GetId());
                command.SetEventObject(this);
                ProcessWindowEvent(command);
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
    }

    void rescale()
    {
        SetMinSize(FromDIP(wxSize(m_width, m_design_confirmation ? 48 : 38)));
        SetMaxSize(FromDIP(wxSize(m_width, m_design_confirmation ? 48 : 38)));
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        wxColour face;
        wxColour foreground;
        if (!IsEnabled()) {
            face = m_primary ? wxColour(91, 80, 43) : wxColour(43, 43, 47);
            foreground = m_primary ? wxColour(25, 25, 25, 150) : wxColour(255, 255, 255, 78);
        } else if (m_primary) {
            face = m_pressed ? wxColour(231, 168, 20) :
                   m_hovered ? wxColour(255, 207, 76) :
                   m_design_confirmation ? wxColour(255, 202, 79) : RedesignTheme::accent_colour();
            foreground = wxColour(20, 20, 20);
        } else {
            face = m_design_confirmation
                ? (m_pressed ? wxColour(75, 75, 77) : m_hovered ? wxColour(101, 101, 103) : wxColour(88, 88, 90))
                : (m_pressed ? wxColour(52, 52, 56) : m_hovered ? wxColour(47, 47, 51) : RedesignTheme::control_colour());
            foreground = RedesignTheme::primary_text_colour();
        }

        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetBrush(wxBrush(face));
        gc->SetPen(FindFocus() == this ? wxPen(RedesignTheme::accent_colour(), std::max(1, FromDIP(1))) :
                                        *wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(8));
        dc.SetFont(GetFont());
        dc.SetTextForeground(foreground);
        const wxSize extent = dc.GetTextExtent(GetLabel());
        dc.DrawText(GetLabel(), (size.x - extent.x) / 2, (size.y - extent.y) / 2);
    }

    bool m_primary { false };
    int m_width { 112 };
    bool m_design_confirmation { false };
    bool m_hovered { false };
    bool m_pressed { false };
};

class RedesignDialogCloseButton final : public wxPanel
{
public:
    explicit RedesignDialogCloseButton(wxWindow* parent)
        : wxPanel(parent, wxID_CLOSE, wxDefaultPosition, parent->FromDIP(wxSize(32, 32)), wxBORDER_NONE)
    {
        SetCanFocus(true);
        SetToolTip(_L("Close"));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        rescale();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) { m_hovered = true; Refresh(); event.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) { m_hovered = false; m_pressed = false; Refresh(); event.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { m_pressed = true; SetFocus(); Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!m_pressed)
                return;
            m_pressed = false;
            Refresh();
            wxCommandEvent command(wxEVT_BUTTON, GetId());
            command.SetEventObject(this);
            ProcessWindowEvent(command);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER ||
                event.GetKeyCode() == WXK_SPACE) {
                wxCommandEvent command(wxEVT_BUTTON, GetId());
                command.SetEventObject(this);
                ProcessWindowEvent(command);
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
    }

    void rescale()
    {
        SetMinSize(FromDIP(wxSize(32, 32)));
        SetMaxSize(FromDIP(wxSize(32, 32)));
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        if (m_hovered || m_pressed) {
            gc->SetBrush(wxBrush(m_pressed ? wxColour(60, 60, 64) : wxColour(49, 49, 53)));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(6));
        }
        gc->SetPen(wxPen(m_hovered ? RedesignTheme::primary_text_colour() : RedesignTheme::secondary_text_colour(),
                         std::max(1, FromDIP(1))));
        const double inset = FromDIP(10);
        gc->StrokeLine(inset, inset, size.x - inset, size.y - inset);
        gc->StrokeLine(size.x - inset, inset, inset, size.y - inset);
        if (FindFocus() == this) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(RedesignTheme::accent_colour(), std::max(1, FromDIP(1))));
            gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(6));
        }
    }

    bool m_hovered { false };
    bool m_pressed { false };
};

RedesignMessageDialog::RedesignMessageDialog(wxWindow* parent, const wxString& message,
                                             const wxString& caption, long style, bool compact, bool scroll_message)
    : RedesignMessageDialog(parent, message, caption, style,
        compact ? Appearance::Compact : Appearance::Standard, 105, scroll_message)
{
}

RedesignMessageDialog::RedesignMessageDialog(wxWindow* parent, const wxString& message,
                                             const wxString& caption, long style, Appearance appearance,
                                             int minimum_message_height, bool scroll_message,
                                             const std::vector<RedesignDialogAction>& actions,
                                             const wxString& checkbox_label,
                                             bool checkbox_checked,
                                             ContentMode content_mode, bool inline_confirmation_footer)
    : DPIDialog(parent, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize,
                wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_SHAPED)
    , m_appearance(appearance), m_content_mode(content_mode), m_original_message(message)
{
    m_inline_confirmation_footer = inline_confirmation_footer;
    const bool design = appearance == Appearance::GenerationConfirmation;
    const bool compact = appearance != Appearance::Standard;
    m_dialog_width = design ? 480 : compact ? 400 : 560;
    m_message_width = design ? 432 : compact ? 352 : 408;
    m_message_min_height = design ? std::max(0, minimum_message_height) : -1;
    m_cancel_result = redesign_dialog_dismiss_result(style);
    m_default_result = redesign_dialog_default_result(style);
    const int action_count = actions.empty() ? int(bool(style & wxYES)) + int(bool(style & wxNO)) +
        int(bool(style & wxOK)) + int(bool(style & wxCANCEL)) : int(actions.size());
    // Keep the 480-DIP face and readable labels for three-way decisions.
    m_stacked_actions = design && action_count > 2;
    // Opaque dark-glass face keeps native text crisp; only the separate backdrop
    // dims the workspace, never the dialog controls themselves.
    SetBackgroundColour(design ? wxColour(53, 53, 55) : RedesignTheme::panel_colour());
    if (design) {
        SetName("generation-confirmation");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
#ifdef __WXMSW__
        SetDoubleBuffered(true);
        set_dark_dialog_border(this, GetBackgroundColour());
#endif
        Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {});
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const auto size = GetClientSize();
            gc->SetBrush(wxBrush(GetBackgroundColour()));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
        });
    }

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_title_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_title_bar->SetBackgroundColour(GetBackgroundColour());
    m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title_bar->SetSizer(title_sizer);
    m_title = new wxStaticText(m_title_bar, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_END);
    RedesignTheme::style_medium_text(m_title, RedesignTheme::primary_text_colour(), 12);
    title_sizer->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(design ? 24 : 20));
    m_close_button = new RedesignDialogCloseButton(m_title_bar);
    title_sizer->Add(m_close_button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(design ? 20 : 10));
    root->Add(m_title_bar, 0, wxEXPAND | (design ? wxLEFT | wxRIGHT | wxTOP : 0), design ? FromDIP(1) : 0);

    auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxBORDER_NONE);
    divider->SetBackgroundColour(RedesignTheme::divider_colour());
    if (!compact && !inline_confirmation_footer) root->Add(divider, 0, wxEXPAND);
    else divider->Hide();

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_icon = new DialogStatusIcon(this, (style & wxICON_WARNING) != 0);
    if (!compact) content_sizer->Add(m_icon, 0, wxTOP, FromDIP(2));
    else m_icon->Hide();
    const bool owner_drawn_details = scroll_message && m_content_mode == ContentMode::OwnerDrawnDetails;
    if (owner_drawn_details) {
        m_details_viewport = new RedesignDetailsViewport(this, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_message_view = m_details_viewport->scroller();
        m_message_view->SetBackgroundColour(RedesignTheme::panel_colour());
        m_details_content = new RedesignDetailsContentPanel(m_message_view, message);
        m_message_window = m_details_content;
        m_details_viewport->set_content_window(m_details_content);
    } else if (scroll_message) {
        // Keep the established confirmation-dialog body.  The custom
        // owner-drawn viewport is reserved for the SmartSlice details view;
        // generation confirmations still rely on wxStaticText's native
        // sizing and the confirmation-specific wrapping path below.
        m_message_view = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_message_view->SetBackgroundColour(RedesignTheme::panel_colour());
        m_message_view->SetScrollRate(0, FromDIP(12));
        m_message = new wxStaticText(m_message_view, wxID_ANY, message);
        m_message_window = m_message;
    } else {
        m_message = new wxStaticText(this, wxID_ANY, message);
        m_message_window = m_message;
    }
    if (m_message)
        m_message->SetBackgroundColour(GetBackgroundColour());
    if (!m_details_content)
        RedesignTheme::style_text(m_message_window, RedesignTheme::primary_text_colour(), compact ? 12 : 10);
    m_message_window->SetMinSize(wxSize(FromDIP(m_message_width),
        m_message_min_height < 0 ? -1 : FromDIP(m_message_min_height)));
    wrap_message();
    if (m_message_view) {
        auto* body = new wxBoxSizer(wxVERTICAL);
        body->Add(m_message_window, 0, wxEXPAND);
        m_message_view->SetSizer(body);
        wxWindow* scroll_host = m_details_viewport ? static_cast<wxWindow*>(m_details_viewport) :
            static_cast<wxWindow*>(m_message_view);
        scroll_host->SetMinSize(wxSize(FromDIP(m_message_width),
            std::min(m_message_window->GetBestSize().y, FromDIP(240))));
        content_sizer->Add(scroll_host, 1, wxEXPAND);
    } else content_sizer->Add(m_message_window, 1, wxLEFT, FromDIP(compact ? 0 : 16));
    root->AddSpacer(FromDIP(compact ? 8 : 24));
    root->Add(content_sizer, scroll_message ? 1 : 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
    if (!checkbox_label.empty()) {
        m_checkbox = new wxCheckBox(this, wxID_ANY, checkbox_label);
        m_checkbox->SetValue(checkbox_checked);
        RedesignTheme::style_text(m_checkbox, RedesignTheme::secondary_text_colour(), 10);
        if (!inline_confirmation_footer)
            root->Add(m_checkbox, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
    }

    m_action_sizer = new wxBoxSizer(m_stacked_actions ? wxVERTICAL : wxHORIZONTAL);
    if ((!compact || design) && !m_stacked_actions && !inline_confirmation_footer) m_action_sizer->AddStretchSpacer(1);
    if (!actions.empty()) {
        for (const auto& action : actions)
            add_action_button(action.id, action.label, action.primary);
    } else {
        if ((style & wxYES) && !compact)
            add_action_button(wxID_YES, _L("Yes"), true);
        if (style & wxNO)
            add_action_button(wxID_NO, _L("No"), false);
        if ((style & wxYES) && compact)
            add_action_button(wxID_YES, _L("Yes"), true);
        if (style & wxOK)
            add_action_button(wxID_OK, _L("OK"), true);
        if (style & wxCANCEL)
            add_action_button(wxID_CANCEL, _L("Cancel"), false);
    }
    if (design) {
        if (!m_stacked_actions) m_action_sizer->AddStretchSpacer(1);
        root->AddSpacer(FromDIP(16));
        root->Add(m_action_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(24));
        root->AddSpacer(FromDIP(20));
    } else if (inline_confirmation_footer) {
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        if (m_checkbox) footer->Add(m_checkbox, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        footer->AddStretchSpacer(1);
        footer->Add(m_action_sizer, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(footer, 0, wxEXPAND | wxALL, FromDIP(24));
    } else {
        root->Add(m_action_sizer, 0, wxEXPAND | wxALL, FromDIP(24));
    }

    SetSizer(root);
    SetMinSize(wxSize(FromDIP(m_dialog_width), -1));
    root->SetSizeHints(this);
    SetSize(wxSize(m_inline_confirmation_footer ?
        std::max(FromDIP(m_dialog_width), GetSizer()->GetMinSize().x) : FromDIP(m_dialog_width), GetSize().y));
    if (m_message_view) {
        wxWindow* scroll_host = m_details_viewport ? static_cast<wxWindow*>(m_details_viewport) :
            static_cast<wxWindow*>(m_message_view);
        scroll_host->SetMinSize(wxSize(FromDIP(m_message_width), FromDIP(48)));
        SetMinSize(FromDIP(wxSize(m_dialog_width, 200)));
        const int available = wxGetTopLevelParent(parent)->GetClientSize().y - FromDIP(32);
        SetSize(wxSize(GetSize().x, std::max(FromDIP(200), std::min(GetSize().y, available))));
        if (!m_details_viewport)
            m_message_view->FitInside();
    }
    Layout();
    if (m_details_viewport) {
        // Calculate the virtual scroll height only after the dialog and its
        // child sizers have their final client sizes.  Doing this earlier can
        // leave a stale range and clip the first/last text row after scrolling.
        m_details_viewport->FitInside();
    }
    update_shape();
    CentreOnParent();

    if (!design) {
        bind_title_drag(m_title_bar);
        bind_title_drag(m_title);
    }
    m_close_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { finish_with(m_cancel_result); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { finish_with(m_cancel_result); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            finish_with(m_cancel_result);
            return;
        }
        if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) {
            wxWindow* focus = FindFocus();
            if (focus == m_close_button ||
                std::find(m_action_buttons.begin(), m_action_buttons.end(), focus) != m_action_buttons.end()) {
                event.Skip();
                return;
            }
            finish_with(m_default_result);
            return;
        }
        event.Skip();
    });
    for (RedesignDialogActionButton* button : m_action_buttons) {
        if (button->GetId() == m_default_result) {
            button->SetFocus();
            break;
        }
    }
}

void RedesignMessageDialog::add_action_button(wxWindowID id, const wxString& label, bool primary)
{
    const bool design = m_appearance == Appearance::GenerationConfirmation;
    const bool compact = m_appearance != Appearance::Standard;
    auto* button = new RedesignDialogActionButton(this, id, label, primary,
        m_stacked_actions ? 432 : design ? 200 : compact ? 170 : m_inline_confirmation_footer ? 96 : 112, design);
    if (design) button->SetName(primary ? "generation-confirm-yes" : "generation-confirm-no");
    button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { finish_with(id); });
    m_action_sizer->Add(button, 0, m_stacked_actions ? wxTOP : wxLEFT,
        FromDIP(compact && m_action_buttons.empty() ? 0 : m_stacked_actions ? 12 : design ? 24 : 12));
    m_action_buttons.push_back(button);
}

void RedesignMessageDialog::set_action_label(wxWindowID id, const wxString& label)
{
    for (auto* button : m_action_buttons)
        if (button->GetId() == id) { button->SetLabel(label); button->Refresh(false); }
}

void RedesignMessageDialog::enable_action(wxWindowID id, bool enabled)
{
    for (auto* button : m_action_buttons)
        if (button->GetId() == id) { button->Enable(enabled); button->Refresh(false); }
}

bool RedesignMessageDialog::checkbox_checked() const
{
    return m_checkbox != nullptr && m_checkbox->GetValue();
}

void RedesignMessageDialog::bind_title_drag(wxWindow* window)
{
    window->Bind(wxEVT_LEFT_DOWN, [this, window](wxMouseEvent& event) {
        m_drag_source = window;
        m_drag_offset = window->ClientToScreen(event.GetPosition()) - GetPosition();
        if (!window->HasCapture())
            window->CaptureMouse();
    });
    window->Bind(wxEVT_MOTION, [this, window](wxMouseEvent& event) {
        if (m_drag_source == window && event.Dragging() && event.LeftIsDown() && window->HasCapture())
            Move(window->ClientToScreen(event.GetPosition()) - m_drag_offset);
        else
            event.Skip();
    });
    window->Bind(wxEVT_LEFT_UP, [this, window](wxMouseEvent&) {
        if (window->HasCapture())
            window->ReleaseMouse();
        if (m_drag_source == window)
            m_drag_source = nullptr;
    });
    window->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this, window](wxMouseCaptureLostEvent&) {
        if (m_drag_source == window)
            m_drag_source = nullptr;
    });
}

void RedesignMessageDialog::finish_with(int result)
{
    if (IsModal())
        EndModal(result);
    else {
        SetReturnCode(result);
        Hide();
    }
}

void RedesignMessageDialog::update_shape()
{
    const wxSize size = GetSize();
    if (size.x <= 0 || size.y <= 0)
        return;
    wxBitmap mask(size.x, size.y, 1);
    wxMemoryDC dc(mask);
    dc.SetBackground(*wxBLACK_BRUSH);
    dc.Clear();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(*wxWHITE_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(m_appearance == Appearance::GenerationConfirmation ? 12 : kCornerRadius));
    dc.SelectObject(wxNullBitmap);
    SetShape(wxRegion(mask, *wxBLACK));
}

void RedesignMessageDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    (void) suggested_rect;
    m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
    m_close_button->rescale();
    static_cast<DialogStatusIcon*>(m_icon)->rescale();
    if (m_message_window)
        m_message_window->SetMinSize(wxSize(FromDIP(m_message_width),
            m_message_min_height < 0 ? -1 : FromDIP(m_message_min_height)));
    wrap_message();
    for (RedesignDialogActionButton* button : m_action_buttons)
        button->rescale();
    SetMinSize(wxSize(FromDIP(m_dialog_width), -1));
    GetSizer()->SetSizeHints(this);
    SetSize(wxSize(m_inline_confirmation_footer ?
        std::max(FromDIP(m_dialog_width), GetSizer()->GetMinSize().x) : FromDIP(m_dialog_width), GetSize().y));
    Layout();
    if (m_details_viewport) {
        m_details_viewport->FitInside();
    }
    update_shape();
#ifdef __WXMSW__
    if (m_appearance == Appearance::GenerationConfirmation)
        set_dark_dialog_border(this, GetBackgroundColour());
#endif
}

void RedesignMessageDialog::wrap_message()
{
    if (m_details_content) {
        m_details_content->set_text(m_original_message, FromDIP(m_message_width));
        if (m_details_viewport)
            m_details_viewport->set_content_window(m_details_content);
        return;
    }

    if (!m_message)
        return;
    m_message->SetLabel(m_original_message);
    if (m_appearance == Appearance::GenerationConfirmation) {
        // wxStaticText::Wrap cannot break a filename without spaces and can
        // discard following lines. Reuse Orca's CJK/long-word wrapping instead.
        wxClientDC dc(m_message);
        dc.SetFont(m_message->GetFont());
        wxString wrapped;
        Label::split_lines(dc, FromDIP(m_message_width), m_original_message, wrapped);
        m_message->SetLabel(wrapped);
    } else {
        m_message->Wrap(FromDIP(m_message_width));
    }
}

int show_generation_confirmation(wxWindow* parent, const wxString& message,
                                 const wxString& caption, int minimum_message_height)
{
    auto* top = wxGetTopLevelParent(parent);
    wxDialog shade(top, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
                   wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT);
    shade.SetBackgroundColour(*wxBLACK);
    shade.SetName("generation-confirm-backdrop");
    if (top) shade.SetSize(top->GetScreenRect());
    const bool dimmed = top && shade.SetTransparent(140);
    if (dimmed) shade.ShowWithoutActivating();
    ModelGenerationConfirmation dialog(dimmed ? static_cast<wxWindow*>(&shade) : parent,
        message, caption, _L("Yes"), minimum_message_height);
    dialog.set_action_label(wxID_NO, _L("No"));
    return dialog.ShowModal();
}

int show_redesign_confirmation(wxWindow* parent, const wxString& message,
                              const wxString& caption, const RedesignConfirmationOptions& options)
{
    auto* top = wxGetTopLevelParent(parent);
    wxDialog shade(top, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
                   wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT);
    shade.SetBackgroundColour(*wxBLACK);
    shade.SetName("generation-confirm-backdrop");
    if (top) shade.SetSize(top->GetScreenRect());
    const bool dimmed = top && shade.SetTransparent(140);
    if (dimmed) shade.ShowWithoutActivating();
    RedesignMessageDialog dialog(dimmed ? static_cast<wxWindow*>(&shade) : parent,
        message, caption, options.style,
        options.standard_layout ? RedesignMessageDialog::Appearance::Standard :
            RedesignMessageDialog::Appearance::GenerationConfirmation,
        options.minimum_message_height, false, options.actions, options.checkbox_label, options.checkbox_checked,
        RedesignMessageDialog::ContentMode::Native, options.inline_confirmation_footer);
    if (options.dismiss_result != wxID_NONE) dialog.set_dismiss_result(options.dismiss_result);
    const int result = dialog.ShowModal();
    if (options.checkbox_state != nullptr)
        *options.checkbox_state = dialog.checkbox_checked();
    return result;
}

int show_redesign_details(wxWindow* parent, const wxString& message,
                          const wxString& caption)
{
    auto* top = wxGetTopLevelParent(parent);
    wxDialog shade(top, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
                   wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT);
    shade.SetBackgroundColour(*wxBLACK);
    shade.SetName("generation-confirm-backdrop");
    if (top) shade.SetSize(top->GetScreenRect());
    const bool dimmed = top && shade.SetTransparent(140);
    if (dimmed) shade.ShowWithoutActivating();

    RedesignMessageDialog dialog(dimmed ? static_cast<wxWindow*>(&shade) : parent,
        message, caption, wxOK, RedesignMessageDialog::Appearance::Compact,
        0, true, {}, wxEmptyString, false,
        RedesignMessageDialog::ContentMode::OwnerDrawnDetails);
    dialog.set_action_label(wxID_OK, _L("关闭"));
    return dialog.ShowModal();
}

}
