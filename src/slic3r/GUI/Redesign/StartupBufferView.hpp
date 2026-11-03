#pragma once

#include "RedesignTheme.hpp"
#include "HonorBrandMark.hpp"
#include "../I18N.hpp"
#include "../BitmapCache.hpp"
#include "../Widgets/Button.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

class StartupBufferView final : public wxPanel
{
public:
    StartupBufferView(wxWindow* parent, wxTopLevelWindow* frame, bool show_brand = true)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN)
        , m_frame(frame), m_show_brand(show_brand)
    {
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_toolbar = new wxPanel(this, wxID_ANY);
        m_toolbar->SetBackgroundColour(wxColour(29, 29, 32));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->Add(new HonorBrandMark(m_toolbar), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        actions->AddStretchSpacer();
        add_action(actions, "redesign_startup_minimize", _L("Minimize"), [this] {
            if (frame_available()) m_frame->Iconize();
        });
        m_maximize = add_action(actions, "redesign_startup_maximize", _L("Maximize"), [this] {
            if (frame_available()) m_frame->Maximize(!m_frame->IsMaximized());
        });
        add_action(actions, "redesign_startup_close", _L("Close"), [this] {
            if (frame_available()) m_frame->Close();
        }, true);
        m_right_spacing = actions->AddSpacer(0);
        m_toolbar->SetSizer(actions);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(m_toolbar, 0, wxEXPAND);
        layout->AddStretchSpacer();
        SetSizer(layout);
        load_logo();
        rescale();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            if (frame_available())
                m_maximize->SetToolTip(m_frame->IsMaximized() ? _L("Restore") : _L("Maximize"));
            const auto size = GetClientSize();
            if (size != m_surface_size) {
                m_surface_size = size;
                Refresh(false);
            }
            event.Skip();
        });
#ifdef __WXMSW__
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            load_logo();
            rescale();
            event.Skip();
        });
        m_toolbar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (frame_available() && !m_frame->IsMaximized())
                ::SendMessageW(static_cast<HWND>(m_frame->GetHandle()), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        });
#endif
        m_toolbar->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) {
            if (frame_available()) m_frame->Maximize(!m_frame->IsMaximized());
        });
    }

    bool set_message(const wxString& message)
    {
        if (m_message == message) return false;
        wxClientDC dc(this);
        const auto previous = fitted_label(dc, m_message, 20, 312, wxFONTWEIGHT_NORMAL).bounds;
        const auto next = fitted_label(dc, message, 20, 312, wxFONTWEIGHT_NORMAL).bounds;
        m_message = message;
        const auto dirty = previous.Union(next).Inflate(FromDIP(2)).Intersect(GetClientRect());
        if (!dirty.IsEmpty()) RefreshRect(dirty, false);
        return true;
    }

private:
    bool frame_available() const { return m_frame && !m_frame->IsBeingDeleted(); }

    void load_logo()
    {
        BitmapCache cache;
        const unsigned size = unsigned(std::max(1, FromDIP(180)));
        if (const wxBitmap* bitmap = cache.load_png("startup_logo", size, size))
            m_logo = *bitmap;
        else
            m_logo = wxNullBitmap;
    }

    Button* add_action(wxBoxSizer* row, const wxString& icon, const wxString& tooltip,
                       std::function<void()> action, bool close = false)
    {
        auto* button = new Button(m_toolbar, wxEmptyString, icon, wxBORDER_NONE, 20);
        button->SetName(tooltip);
        button->SetToolTip(tooltip);
        button->SetCornerRadius(0);
        button->SetBorderWidth(0);
        button->SetPaddingSize(wxSize(0, 0));
        button->SetBackgroundColour(m_toolbar->GetBackgroundColour());
        button->SetBackgroundColor(StateColor(
            std::make_pair(close ? wxColour(170, 45, 55) : wxColour(58, 58, 62), int(StateColor::Pressed)),
            std::make_pair(close ? wxColour(192, 54, 64) : wxColour(48, 48, 52), int(StateColor::Hovered)),
            std::make_pair(wxColour(29, 29, 32), int(StateColor::Normal))));
        button->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        row->Add(button, 0);
        m_actions.push_back(button);
        return button;
    }

    void rescale()
    {
        m_toolbar->SetMinSize(wxSize(-1, FromDIP(40)));
        m_right_spacing->AssignSpacer(FromDIP(2), 0);
        for (auto* button : m_actions) {
            button->Rescale();
            button->SetMinSize(FromDIP(wxSize(40, 40)));
            button->SetMaxSize(FromDIP(wxSize(40, 40)));
        }
        Layout();
        Refresh(false);
    }

    wxFont font(int pixels, wxFontWeight weight = wxFONTWEIGHT_NORMAL) const
    {
        wxFontInfo info(pixels * 0.75);
        info.Family(wxFONTFAMILY_SWISS).Weight(weight);
        const wxString family = RedesignTheme::font_family();
        if (!family.empty()) info.FaceName(family);
        wxFont result(info);
        result.SetPixelSize(wxSize(0, FromDIP(pixels)));
        return result;
    }

    struct FittedLabel {
        wxString text;
        wxRect bounds;
    };

    FittedLabel fitted_label(wxDC& dc, const wxString& text, int pixels, int offset,
                             wxFontWeight weight) const
    {
        const auto bounds = GetClientSize();
        const int top = std::max(FromDIP(64), bounds.y / 2 - FromDIP(176));
        dc.SetFont(font(pixels, weight));
        while (dc.GetTextExtent(text).x > bounds.x - FromDIP(48) && pixels > 16)
            dc.SetFont(font(--pixels, weight));
        const auto fitted = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END,
            std::max(1, bounds.x - FromDIP(48)));
        const auto extent = dc.GetTextExtent(fitted);
        return {fitted, wxRect((bounds.x - extent.x) / 2, top + FromDIP(offset), extent.x, extent.y)};
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(RedesignTheme::flow_background_colour()));
        dc.Clear();
        if (!m_show_brand) return;
        const wxSize bounds = GetClientSize();
        if (bounds.x <= 0 || bounds.y <= 0) return;

        // Figma 17:2063 centers a fixed-size brand group in the full window.
        const int top = std::max(FromDIP(64), bounds.y / 2 - FromDIP(176));
        const int icon = FromDIP(180);
        auto graphics = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (graphics && !m_logo.IsOk()) {
            graphics->SetPen(*wxTRANSPARENT_PEN);
            graphics->SetBrush(wxBrush(wxColour(116, 116, 119)));
            graphics->DrawRoundedRectangle((bounds.x - icon) / 2.0, top, icon, icon, FromDIP(24));
        }
        graphics.reset();
        if (m_logo.IsOk())
            dc.DrawBitmap(m_logo, (bounds.x - m_logo.GetWidth()) / 2, top, true);
        const auto label = [&](const wxString& text, int pixels, int offset,
                               const wxColour& colour, wxFontWeight weight) {
            const auto fitted = fitted_label(dc, text, pixels, offset, weight);
            dc.SetTextForeground(colour);
            dc.DrawText(fitted.text, fitted.bounds.GetPosition());
        };
        label(wxString::FromUTF8(SLIC3R_APP_NAME), 44, 232, *wxWHITE, wxFONTWEIGHT_SEMIBOLD);
        label(m_message, 20, 312, wxColour(127, 127, 130), wxFONTWEIGHT_NORMAL);
    }

    wxWeakRef<wxTopLevelWindow> m_frame;
    wxPanel* m_toolbar = nullptr;
    wxSizerItem* m_right_spacing = nullptr;
    Button* m_maximize = nullptr;
    std::vector<Button*> m_actions;
    wxString m_message;
    wxBitmap m_logo;
    wxSize m_surface_size;
    bool m_show_brand = true;
};

}
