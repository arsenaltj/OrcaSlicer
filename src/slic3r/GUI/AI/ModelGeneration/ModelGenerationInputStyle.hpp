#pragma once

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <array>

namespace Slic3r::GUI::ModelGenerationInputStyle {

inline const wxColour background(49, 49, 54);
inline const wxColour panel(34, 34, 37);
inline const wxColour field(19, 19, 22);
inline const wxColour text(226, 226, 228);
inline const wxColour secondary(158, 158, 163);
inline const wxColour yellow(254, 212, 69);
inline const wxColour selected(74, 66, 37);
inline const wxColour disabled_action(111, 96, 48);
inline const wxColour action_text(20, 20, 22);
inline const wxColour success(117, 211, 168);
inline const wxColour warning(255, 166, 145);
inline constexpr int corner_radius = 12;
inline constexpr const char* font_face = "HONOR SANS Design";
enum class Role { Panel, Field, QuietAction, Secondary, PrimaryAction, Accent, Navigation, SelectedNavigation, Success, Warning };

inline void apply_control(wxWindow* window, Role role, bool update_fonts = false, bool force_colors = false)
{
    const bool primary = role == Role::PrimaryAction;
    const bool quiet = role == Role::QuietAction;
    const wxColour quiet_fill(78, 78, 81);
    const wxColour bg = primary ? (window->IsThisEnabled() ? yellow : disabled_action) :
        quiet ? quiet_fill : role == Role::Field ? field : role == Role::SelectedNavigation ? selected : panel;
    const wxColour fg = primary ? action_text : (role == Role::SelectedNavigation || role == Role::Accent) ? yellow :
        role == Role::Success ? success : role == Role::Warning ? warning :
        (role == Role::Secondary || role == Role::Navigation) ? secondary : text;
    auto* button = dynamic_cast<Button*>(window);
    const bool navigation = role == Role::Navigation || role == Role::SelectedNavigation;
    const bool painted_button = button && (primary || quiet || role == Role::Field || navigation);
    // StaticBox paints its rounded fill over the native window background.
    // Keep that outer canvas at the surface color, so corners remain visible.
    const wxColour canvas = painted_button ? panel : bg;
    const bool changed = window->GetBackgroundColour() != canvas || window->GetForegroundColour() != fg;
    if (force_colors || window->GetBackgroundColour() != canvas) window->SetBackgroundColour(canvas);
    if (force_colors || window->GetForegroundColour() != fg) window->SetForegroundColour(fg);
    if (update_fonts) {
        wxFont font = window->GetFont(); font.SetFaceName(font_face);
        if (window->GetFont() != font) window->SetFont(font);
    }
    if (painted_button && (changed || update_fonts || force_colors)) {
        const wxColour normal = primary ? yellow : quiet ? quiet_fill : navigation ? bg : field;
        const StateColor fill(
            std::make_pair(primary ? disabled_action : field, int(StateColor::Disabled)),
            std::make_pair(primary ? yellow.ChangeLightness(90) : selected, int(StateColor::Pressed)),
            std::make_pair(primary ? yellow.ChangeLightness(108) : background, int(StateColor::Hovered)),
            std::make_pair(normal, int(StateColor::Normal)));
        button->SetBackgroundColor(fill);
        button->SetBorderColor(StateColor(std::make_pair(yellow, int(StateColor::Focused)),
            std::make_pair(normal, int(StateColor::Normal))));
        button->SetTextColor(fg);
        button->SetCornerRadius(navigation ? 0 : window->FromDIP(corner_radius));
    }
}

// Input and result surfaces share roles, while native import pages and content
// colors retain their own owners. Never recolor material swatches as UI chrome.
inline void apply(wxWindow* window, bool update_fonts = true)
{
    if (!window || window->GetName() == "ai_content_color") return;
    if (window->GetName() == "ai_native_theme") {
        refresh_ai_appearance(window, update_fonts);
        return;
    }
    const auto name = window->GetName();
    const Role role = name == "input_primary" ? Role::PrimaryAction :
        name == "input_quiet" ? Role::QuietAction :
        name == "ai_status_success" ? Role::Success :
        name == "ai_status_warning" ? Role::Warning :
        name == "input_secondary" ? Role::Secondary :
        name == "input_accent" ? Role::Accent :
        name == "input_navigation_selected" ? Role::SelectedNavigation :
        name == "input_navigation" ? Role::Navigation :
        (name == "input_field" || dynamic_cast<wxTextCtrl*>(window)) ? Role::Field : Role::Panel;
    apply_control(window, role, update_fonts, true);
    if (auto* combo = dynamic_cast<ComboBox*>(window)) {
        combo->SetBackgroundColor(field);
        combo->SetBorderColor(field);
        combo->SetTextColor(text);
        combo->SetLabelColor(text);
        combo->SetCornerRadius(window->FromDIP(10));
        combo->GetDropDown().SetTextColor(text);
        combo->GetDropDown().SetBackgroundColour(panel);
        combo->GetDropDown().SetSelectorBackgroundColor(selected);
        return; // Composite control internals own their painting.
    }
    for (auto* child : window->GetChildren()) {
        if (auto* owner = dynamic_cast<AIThemeOwner*>(child)) owner->apply_ai_theme(update_fonts);
        else apply(child, update_fonts);
    }
}

class Surface final : public wxPanel, public AIThemeOwner {
public:
    explicit Surface(wxWindow* parent) : wxPanel(parent) { SetBackgroundColour(background); }
    void apply_ai_theme(bool fonts) override {
        SetBackgroundColour(background);
        for (auto* child : GetChildren()) refresh_ai_appearance(child, fonts);
    }
};

class RoundedPanel final : public wxPanel, public AIThemeOwner {
public:
    void apply_ai_theme(bool fonts) override { apply(this, fonts); Refresh(false); }
    explicit RoundedPanel(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background)); dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(panel));
            dc.DrawRoundedRectangle(GetClientRect(), FromDIP(12));
        });
    }
};

} // namespace Slic3r::GUI::ModelGenerationInputStyle
