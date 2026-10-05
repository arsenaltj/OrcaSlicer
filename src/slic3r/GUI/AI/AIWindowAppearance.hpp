#pragma once

#include "slic3r/GUI/GUI_App.hpp"
#include <wx/window.h>

namespace Slic3r::GUI {

// A migrated subtree owns its controls. The legacy walk must not repaint it
// before or after its local theme (in particular, never repaint content colors).
class AIThemeOwner {
public:
    virtual ~AIThemeOwner() = default;
    virtual void apply_ai_theme(bool update_fonts) = 0;
};

// Only for AI feature subtrees. Palette swatches are content, not theme tokens.
inline void refresh_ai_appearance(wxWindow* window, bool update_fonts = true)
{
    if (window == nullptr || window->GetName() == "ai_content_color")
        return;
    if (auto* owner = dynamic_cast<AIThemeOwner*>(window)) {
        owner->apply_ai_theme(update_fonts);
        return;
    }
    window->SetBackgroundColour(wxGetApp().get_window_default_clr());
    window->SetForegroundColour(wxGetApp().get_label_clr_default());
    if (update_fonts)
        window->SetFont(window->GetFont().GetWeight() == wxFONTWEIGHT_BOLD ?
                        wxGetApp().bold_font() : wxGetApp().normal_font());
    for (auto* child : window->GetChildren())
        refresh_ai_appearance(child, update_fonts);
    window->Refresh(false);
}

} // namespace Slic3r::GUI
