#pragma once

#include "RedesignTheme.hpp"
#include "../AI/AIWindowAppearance.hpp"
#include "../I18N.hpp"
#include "../Widgets/Button.hpp"
#include "../Widgets/ComboBox.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/textctrl.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <algorithm>
#include <memory>

namespace Slic3r::GUI {

inline wxBitmap redesign_resource_bitmap(wxWindow* window, const char* name, const wxSize& dip)
{
    wxImage source(wxString::FromUTF8(Slic3r::resources_dir() + "/images/" + name));
    if (!source.IsOk()) return wxNullBitmap;
    const auto bounds = window->FromDIP(dip);
    const double scale = std::min(double(bounds.x) / source.GetWidth(), double(bounds.y) / source.GetHeight());
    wxBitmap result(source.Scale(std::max(1, int(source.GetWidth() * scale)),
        std::max(1, int(source.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH));
    result.SetScaleFactor(window->GetDPIScaleFactor());
    return result;
}

// Keep the accepted shell's scrollbar-free field while retaining native text
// editing, caret scrolling and undo. Input limits remain with the task owner.
class RedesignPromptTextCtrl final : public wxTextCtrl {
public:
    RedesignPromptTextCtrl(wxWindow* parent, const wxSize& size)
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
#ifdef __WXMSW__
    WXDWORD MSWGetStyle(long style, WXDWORD* exstyle) const override
    {
        return wxTextCtrl::MSWGetStyle(style, exstyle) | ES_AUTOVSCROLL;
    }
private:
    int m_wheel_rotation {0};
#endif
};

// The accepted shell's navigation appearance, with the existing Button's
// activation, keyboard traversal and command events retained.
class RedesignNavigationButton final : public Button, public AIThemeOwner {
public:
    RedesignNavigationButton(wxWindow* parent, const wxString& caption, const char* asset,
                             bool primary = true)
        : Button(parent, caption, "", wxBORDER_NONE), m_asset(asset), m_primary(primary)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        apply_ai_theme(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            apply_ai_theme(true);
            event.Skip();
        });
    }
    void set_selected(bool selected) {
        if (m_selected == selected) return;
        m_selected = selected;
        Refresh(false);
    }
    void apply_ai_theme(bool fonts) override {
        SetBackgroundColour(wxColour(33, 33, 35));
        if (fonts) {
            RedesignTheme::style_text(this, RedesignTheme::primary_text_colour(), 9);
            m_icon = redesign_resource_bitmap(this, m_asset, wxSize(24, 24));
        }
        Refresh(false);
    }
private:
    void paint() {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
        const auto size = GetClientSize();
        if (m_selected && m_primary) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(RedesignTheme::accent_colour()));
            dc.DrawRectangle(0, 0, FromDIP(3), size.y);
        }
        if (m_icon.IsOk()) dc.DrawBitmap(m_icon, (size.x - m_icon.GetLogicalWidth()) / 2,
            m_primary ? FromDIP(12) : FromDIP(4), true);
        dc.SetFont(GetFont());
        dc.SetTextForeground(m_selected ? RedesignTheme::primary_text_colour() : RedesignTheme::secondary_text_colour());
        const auto label = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END, std::max(1, size.x - FromDIP(8)));
        const auto extent = dc.GetTextExtent(label);
        dc.DrawText(label, (size.x - extent.x) / 2, m_primary ? FromDIP(40) : FromDIP(31));
        if (HasFocus()) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(RedesignTheme::accent_colour(), 1, wxPENSTYLE_SHORT_DASH));
            dc.DrawRoundedRectangle(GetClientRect().Deflate(FromDIP(5)), FromDIP(4));
        }
    }
    const char* m_asset;
    wxBitmap m_icon;
    bool m_primary;
    bool m_selected {false};
};

// Reuse Orca's real dropdown and selection events; only its closed presentation
// is replaced by the accepted 56-DIP picker with the original leading artwork.
class RedesignChoice final : public ComboBox, public AIThemeOwner {
public:
    RedesignChoice(wxWindow* parent, const wxArrayString& choices, const char* asset = nullptr)
        : ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
            parent->FromDIP(wxSize(1, 56)), 0, nullptr, wxCB_READONLY), m_asset(asset)
    {
        for (const auto& choice : choices) Append(choice);
        SetSelection(0);
        SetName("input_field");
        apply_ai_theme(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(RedesignTheme::panel_colour())); dc.Clear();
            const auto size = GetClientSize();
            dc.SetPen(HasFocus() ? wxPen(RedesignTheme::accent_colour()) : *wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(RedesignTheme::control_colour()));
            dc.DrawRoundedRectangle(GetClientRect(), FromDIP(12));
            const int leading = m_icon.IsOk() ? FromDIP(62) : FromDIP(16);
            if (m_icon.IsOk()) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxColour(60, 60, 62)));
                dc.DrawRoundedRectangle(FromDIP(6), FromDIP(6), FromDIP(44), FromDIP(44), FromDIP(10));
                dc.DrawBitmap(m_icon, FromDIP(16), (size.y - m_icon.GetLogicalHeight()) / 2, true);
            }
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled() ? RedesignTheme::primary_text_colour() : RedesignTheme::secondary_text_colour());
            const wxString value = GetLabel().empty() ? _L("请选择") : GetLabel();
            const auto caption = wxControl::Ellipsize(value, dc, wxELLIPSIZE_END,
                std::max(1, size.x - leading - FromDIP(38)));
            dc.DrawText(caption, leading, (size.y - dc.GetTextExtent(caption).y) / 2);
            dc.DrawBitmap(m_arrow.bmp(), size.x - FromDIP(20) - m_arrow.GetBmpWidth(),
                (size.y - m_arrow.GetBmpHeight()) / 2, true);
        });
    }
    void SetSelection(int value) override {
        ComboBox::SetSelection(value);
        SetMinSize(FromDIP(wxSize(1, 56)));
    }
    void Rescale() override { ComboBox::Rescale(); apply_ai_theme(true); }
    void apply_ai_theme(bool fonts) override {
        SetMinSize(FromDIP(wxSize(1, 56)));
        SetBackgroundColor(RedesignTheme::control_colour());
        SetBorderColor(RedesignTheme::control_colour());
        SetTextColor(RedesignTheme::primary_text_colour());
        SetLabelColor(RedesignTheme::primary_text_colour());
        GetDropDown().SetTextColor(RedesignTheme::primary_text_colour());
        GetDropDown().SetBackgroundColour(RedesignTheme::panel_colour());
        GetDropDown().SetCornerRadius(FromDIP(12));
        GetDropDown().SetBorderColor(RedesignTheme::panel_colour());
        GetDropDown().SetSelectorBorderColor(RedesignTheme::panel_colour());
        GetDropDown().SetSelectorBackgroundColor(wxColour(64, 61, 44));
        if (fonts) {
            RedesignTheme::style_text(this, RedesignTheme::primary_text_colour(), 11);
            GetDropDown().SetFont(GetFont());
            GetDropDown().Rescale();
            if (m_asset) m_icon = redesign_resource_bitmap(this, m_asset, wxSize(24, 24));
            m_arrow = ScalableBitmap(this, "figma-ux/beauty-mode-arrow", 13);
        }
        Refresh(false);
    }
private:
    const char* m_asset;
    wxBitmap m_icon;
    ScalableBitmap m_arrow;
};

} // namespace Slic3r::GUI
