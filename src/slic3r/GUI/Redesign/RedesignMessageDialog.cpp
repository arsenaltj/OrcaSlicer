#include "RedesignMessageDialog.hpp"

#include "RedesignTheme.hpp"
#include "../I18N.hpp"

#include <algorithm>
#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/region.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r::GUI {
namespace {

constexpr int kDialogWidth = 560;
constexpr int kMessageWidth = 408;
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
    RedesignDialogActionButton(wxWindow* parent, wxWindowID id, const wxString& caption, bool primary)
        : wxPanel(parent, id, wxDefaultPosition, parent->FromDIP(wxSize(112, 38)), wxBORDER_NONE)
        , m_primary(primary)
    {
        SetLabel(caption);
        SetCanFocus(true);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        RedesignTheme::style_medium_text(this,
            primary ? wxColour(20, 20, 20) : RedesignTheme::primary_text_colour(), 10);
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
            if (IsEnabled() && (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
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
        SetMinSize(FromDIP(wxSize(112, 38)));
        SetMaxSize(FromDIP(wxSize(112, 38)));
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

        wxColour face;
        wxColour foreground;
        if (!IsEnabled()) {
            face = m_primary ? wxColour(91, 80, 43) : wxColour(43, 43, 47);
            foreground = m_primary ? wxColour(25, 25, 25, 150) : wxColour(255, 255, 255, 78);
        } else if (m_primary) {
            face = m_pressed ? wxColour(231, 168, 20) :
                   m_hovered ? wxColour(255, 207, 76) : RedesignTheme::accent_colour();
            foreground = wxColour(20, 20, 20);
        } else {
            face = m_pressed ? wxColour(52, 52, 56) :
                   m_hovered ? wxColour(47, 47, 51) : RedesignTheme::control_colour();
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
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE) {
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
        dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
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
                                             const wxString& caption, long style)
    : DPIDialog(parent, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize,
                wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_SHAPED)
{
    m_cancel_result = (style & wxNO) ? wxID_NO : wxID_CANCEL;
    m_default_result = (style & wxYES) ? wxID_YES : wxID_OK;
    SetBackgroundColour(RedesignTheme::panel_colour());

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_title_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_title_bar->SetBackgroundColour(RedesignTheme::panel_colour());
    m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_title_bar->SetSizer(title_sizer);
    m_title = new wxStaticText(m_title_bar, wxID_ANY, caption, wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_END);
    RedesignTheme::style_medium_text(m_title, RedesignTheme::primary_text_colour(), 12);
    title_sizer->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    m_close_button = new RedesignDialogCloseButton(m_title_bar);
    title_sizer->Add(m_close_button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    root->Add(m_title_bar, 0, wxEXPAND);

    auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxBORDER_NONE);
    divider->SetBackgroundColour(RedesignTheme::divider_colour());
    root->Add(divider, 0, wxEXPAND);

    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_icon = new DialogStatusIcon(this, (style & wxICON_WARNING) != 0);
    content_sizer->Add(m_icon, 0, wxTOP, FromDIP(2));
    m_message = new wxStaticText(this, wxID_ANY, message);
    RedesignTheme::style_text(m_message, RedesignTheme::primary_text_colour(), 10);
    m_message->SetMinSize(wxSize(FromDIP(kMessageWidth), -1));
    m_message->Wrap(FromDIP(kMessageWidth));
    content_sizer->Add(m_message, 1, wxLEFT, FromDIP(16));
    root->Add(content_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(24));

    m_action_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_action_sizer->AddStretchSpacer(1);
    if (style & wxYES)
        add_action_button(wxID_YES, _L("Yes"), true);
    if (style & wxNO)
        add_action_button(wxID_NO, _L("No"), false);
    if (style & wxOK)
        add_action_button(wxID_OK, _L("OK"), true);
    if (style & wxCANCEL)
        add_action_button(wxID_CANCEL, _L("Cancel"), false);
    root->Add(m_action_sizer, 0, wxEXPAND | wxALL, FromDIP(24));

    SetSizer(root);
    SetMinSize(wxSize(FromDIP(kDialogWidth), -1));
    root->SetSizeHints(this);
    SetSize(wxSize(FromDIP(kDialogWidth), GetSize().y));
    Layout();
    update_shape();
    CentreOnParent();

    bind_title_drag(m_title_bar);
    bind_title_drag(m_title);
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
    auto* button = new RedesignDialogActionButton(this, id, label, primary);
    button->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { finish_with(id); });
    m_action_sizer->Add(button, 0, wxLEFT, FromDIP(12));
    m_action_buttons.push_back(button);
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
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(kCornerRadius));
    dc.SelectObject(wxNullBitmap);
    SetShape(wxRegion(mask, *wxBLACK));
}

void RedesignMessageDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    (void) suggested_rect;
    m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
    m_close_button->rescale();
    static_cast<DialogStatusIcon*>(m_icon)->rescale();
    m_message->SetMinSize(wxSize(FromDIP(kMessageWidth), -1));
    m_message->Wrap(FromDIP(kMessageWidth));
    for (RedesignDialogActionButton* button : m_action_buttons)
        button->rescale();
    SetMinSize(wxSize(FromDIP(kDialogWidth), -1));
    GetSizer()->SetSizeHints(this);
    SetSize(wxSize(FromDIP(kDialogWidth), GetSize().y));
    Layout();
    update_shape();
}

}
