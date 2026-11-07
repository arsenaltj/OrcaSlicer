#pragma once

#include "HonorBrandMark.hpp"
#include "RedesignTheme.hpp"

#include "../Widgets/Button.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/Label.hpp"
#include "../Widgets/StaticBox.hpp"
#include "../Widgets/TextInput.hpp"

#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/toplevel.h>

#ifdef __WXMSW__
#include "../dark_mode.hpp"
#endif

namespace Slic3r::GUI {

inline long printer_transfer_dialog_style(bool redesign)
{
    return redesign ? (wxBORDER_NONE | wxFRAME_NO_TASKBAR) : (wxCAPTION | wxCLOSE_BOX);
}

class PrinterTransferTitleBar final : public wxPanel
{
public:
    PrinterTransferTitleBar(wxWindow* parent, const wxString& title)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundColour(RedesignTheme::panel_colour());
        auto* layout = new wxBoxSizer(wxHORIZONTAL);
        layout->Add(new HonorBrandMark(this), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));

        auto* caption = new wxStaticText(this, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                                         wxST_ELLIPSIZE_END);
        RedesignTheme::style_medium_text(caption, RedesignTheme::primary_text_colour(), 12);
        layout->Add(caption, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

        auto* close = new Button(this, wxEmptyString, "figma_ux_0929_dialog_close", wxBORDER_NONE, 16);
        close->SetCanFocus(false);
        close->SetMinSize(FromDIP(wxSize(40, 40)));
        close->SetMaxSize(FromDIP(wxSize(40, 40)));
        close->SetBorderWidth(0);
        close->SetCornerRadius(FromDIP(6));
        close->SetBackgroundColor(RedesignTheme::panel_colour());
        close->SetBorderColor(RedesignTheme::panel_colour());
        close->Bind(wxEVT_BUTTON, [parent](wxCommandEvent&) {
            if (auto* top = wxDynamicCast(parent, wxTopLevelWindow))
                top->Close();
        });
        layout->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

        SetSizer(layout);
        SetMinSize(wxSize(-1, FromDIP(52)));
        bind_drag(this);
        bind_drag(caption);
#ifdef __WXMSW__
        Bind(wxEVT_DPI_CHANGED, [this, close](wxDPIChangedEvent& event) {
            SetMinSize(wxSize(-1, FromDIP(52)));
            close->SetMinSize(FromDIP(wxSize(40, 40)));
            close->SetMaxSize(FromDIP(wxSize(40, 40)));
            close->Rescale();
            Layout();
            event.Skip();
        });
#endif
    }

private:
    void bind_drag(wxWindow* source)
    {
        source->Bind(wxEVT_LEFT_DOWN, [this, source](wxMouseEvent& event) {
            m_drag_source = source;
            m_drag_offset = source->ClientToScreen(event.GetPosition()) - GetParent()->GetPosition();
            if (!source->HasCapture())
                source->CaptureMouse();
        });
        source->Bind(wxEVT_MOTION, [this, source](wxMouseEvent& event) {
            if (m_drag_source == source && event.Dragging() && event.LeftIsDown() && source->HasCapture())
                GetParent()->Move(source->ClientToScreen(event.GetPosition()) - m_drag_offset);
            else
                event.Skip();
        });
        source->Bind(wxEVT_LEFT_UP, [this, source](wxMouseEvent&) {
            if (source->HasCapture())
                source->ReleaseMouse();
            if (m_drag_source == source)
                m_drag_source = nullptr;
        });
        source->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this, source](wxMouseCaptureLostEvent&) {
            if (m_drag_source == source)
                m_drag_source = nullptr;
        });
    }

    wxWindow* m_drag_source { nullptr };
    wxPoint m_drag_offset;
};

inline void apply_printer_transfer_redesign_theme(wxWindow* window)
{
    if (!window)
        return;

    const wxColour panel = RedesignTheme::panel_colour();
    const wxColour control = RedesignTheme::control_colour();
    const wxColour divider = RedesignTheme::divider_colour();
    window->SetBackgroundColour(panel);

    if (auto* text = wxDynamicCast(window, wxStaticText))
        RedesignTheme::style_text(text, RedesignTheme::primary_text_colour(), 10);
    if (auto* checkbox = wxDynamicCast(window, wxCheckBox))
        RedesignTheme::style_text(checkbox, RedesignTheme::primary_text_colour(), 10);
    if (auto* box = dynamic_cast<StaticBox*>(window)) {
        box->SetBackgroundColor(panel);
        box->SetBorderColor(divider);
    }
    if (auto* input = dynamic_cast<TextInput*>(window)) {
        input->SetBackgroundColor(control);
        input->SetBorderColor(divider);
        input->SetTextColor(RedesignTheme::primary_text_colour());
        input->SetLabelColor(RedesignTheme::secondary_text_colour());
    }
    if (auto* combo = dynamic_cast<ComboBox*>(window)) {
        combo->SetBackgroundColor(control);
        combo->SetBorderColor(divider);
        combo->SetTextColor(RedesignTheme::primary_text_colour());
        combo->GetDropDown().SetBackgroundColour(panel);
        combo->GetDropDown().SetBorderColor(panel);
        combo->GetDropDown().SetSelectorBackgroundColor(wxColour(64, 61, 44));
        combo->GetDropDown().SetTextColor(RedesignTheme::primary_text_colour());
    }
    if (auto* button = dynamic_cast<Button*>(window)) {
        button->SetBackgroundColor(control);
        button->SetBorderColor(divider);
        button->SetTextColor(RedesignTheme::primary_text_colour());
        button->SetCornerRadius(window->FromDIP(6));
    }

#ifdef __WXMSW__
    if (wxDynamicCast(window, wxScrolledWindow) && window->GetHandle())
        NppDarkMode::EnableDarkScrollBarForWindowAndChildren(static_cast<HWND>(window->GetHandle()));
#endif

    for (wxWindow* child : window->GetChildren())
        apply_printer_transfer_redesign_theme(child);
    window->Refresh(false);
}

inline void style_printer_transfer_primary_button(Button* button)
{
    if (!button)
        return;
    button->SetBackgroundColor(RedesignTheme::accent_colour());
    button->SetBorderColor(RedesignTheme::accent_colour());
    button->SetTextColor(wxColour(22, 22, 25));
    button->SetCornerRadius(button->FromDIP(7));
    button->SetMinSize(button->FromDIP(wxSize(220, 42)));
}

} // namespace Slic3r::GUI
