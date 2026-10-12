#pragma once

#include "RedesignTheme.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/SpinInput.hpp"
#include "../Widgets/Button.hpp"
#include "../Widgets/SideButton.hpp"
#include <wx/weakref.h>
#include <algorithm>
#include <functional>
#include <vector>

namespace Slic3r::GUI {

// Native controls are shared with ordinary Orca. Save their actual appearance,
// including owner-drawn state colors, once; dynamic controls join on refresh.
class NativeWorkspaceTheme final {
public:
    ~NativeWorkspaceTheme() { restore(); }

    void apply(wxWindow* window, const std::vector<wxWindow*>& swatches,
               wxColour surface = RedesignTheme::panel_colour())
    {
        if (!window || std::find(swatches.begin(), swatches.end(), window) != swatches.end()) return;
        if (std::none_of(m_saved.begin(), m_saved.end(), [window](const auto& saved) { return saved.window == window; }))
            capture(window);
        const wxColour text(230, 230, 233);
        window->SetName("ai_content_color");
        window->SetBackgroundColour(surface);
        window->SetForegroundColour(text);
        if (auto* box = dynamic_cast<StaticBox*>(window)) {
            if (dynamic_cast<TextInput*>(window) || dynamic_cast<SpinInput*>(window)) surface = wxColour(70, 70, 73);
            box->SetBackgroundColor(surface);
            box->SetBackgroundColor2(StateColor());
            box->SetBorderColor(wxColour(82, 82, 87));
            box->SetCornerRadius(window->FromDIP(6));
        }
        if (auto* input = dynamic_cast<TextInput*>(window)) {
            const StateColor content(std::pair<wxColour, int>(wxColour(126,126,130), StateColor::Disabled),
                                     std::pair<wxColour, int>(text, StateColor::Normal));
            input->SetTextColor(content);
            input->SetLabelColor(content);
            if (auto* combo = dynamic_cast<ComboBox*>(input)) {
                auto& drop = combo->GetDropDown();
                drop.SetBackgroundColour(surface);
                drop.SetTextColor(text);
                drop.SetSelectorBackgroundColor(wxColour(85, 85, 90));
            }
        } else if (auto* input = dynamic_cast<SpinInput*>(window)) {
            input->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour(126,126,130), StateColor::Disabled),
                                          std::pair<wxColour, int>(text, StateColor::Normal)));
            input->SetLabelColor(wxColour(190, 190, 196));
        } else if (auto* button = dynamic_cast<Button*>(window)) {
            button->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour(126,126,130), StateColor::Disabled),
                                           std::pair<wxColour, int>(text, StateColor::Normal)));
        } else if (auto* button = dynamic_cast<SideButton*>(window)) {
            const StateColor background(std::pair<wxColour, int>(wxColour(55,55,58), StateColor::Disabled),
                std::pair<wxColour, int>(wxColour(255,210,78), StateColor::Hovered),
                std::pair<wxColour, int>(RedesignTheme::accent_colour(), StateColor::Normal));
            button->SetBackgroundColor(background); button->SetBorderColor(background);
            button->SetForegroundColor(StateColor(std::pair<wxColour, int>(wxColour(126,126,130), StateColor::Disabled),
                std::pair<wxColour, int>(wxColour(22,22,25), StateColor::Normal)));
            button->SetBottomColour(surface); button->SetCornerRadius(window->FromDIP(6));
        }
        // Owned dialogs/popups manage their own theme and physical swatches.
        // A sidebar refresh must not overwrite the active dialog's controls.
        for (wxWindow* child : window->GetChildren())
            if (!child->IsTopLevel()) apply(child, swatches, surface);
        window->Refresh(false);
    }

    void restore()
    {
        for (auto& saved : m_saved) if (saved.window) {
            saved.window->SetName(saved.name);
            saved.window->SetBackgroundColour(saved.background);
            saved.window->SetForegroundColour(saved.foreground);
            for (auto& restore_color : saved.colors) restore_color();
            saved.window->Refresh(false);
        }
        m_saved.clear();
    }

private:
    struct Saved {
        wxWeakRef<wxWindow> window;
        wxString name;
        wxColour background, foreground;
        std::vector<std::function<void()>> colors;
    };
    std::vector<Saved> m_saved;

    void capture(wxWindow* window)
    {
        Saved saved {window, window->GetName(), window->GetBackgroundColour(), window->GetForegroundColour(), {}};
        if (auto* box = dynamic_cast<StaticBox*>(window)) {
            saved.colors.emplace_back([box, background = box->GetBackgroundColor(), gradient = box->GetBackgroundColor2(),
                border = box->GetBorderColor(), radius = box->GetCornerRadiusValue()] {
                box->SetBackgroundColor(background); box->SetBackgroundColor2(gradient);
                box->SetBorderColor(border); box->SetCornerRadius(radius);
            });
        }
        if (auto* input = dynamic_cast<TextInput*>(window)) {
            saved.colors.emplace_back([input, text = input->GetTextColor(), label = input->GetLabelColor()] {
                input->SetTextColor(text); input->SetLabelColor(label);
            });
            if (auto* combo = dynamic_cast<ComboBox*>(input)) {
                auto* drop = &combo->GetDropDown();
                saved.colors.emplace_back([drop, background = drop->GetBackgroundColour(), text = drop->GetTextColor(),
                    selection = drop->GetSelectorBackgroundColor()] {
                    drop->SetBackgroundColour(background); drop->SetTextColor(text); drop->SetSelectorBackgroundColor(selection);
                });
            }
        } else if (auto* input = dynamic_cast<SpinInput*>(window)) {
            saved.colors.emplace_back([input, text = input->GetTextColor(), label = input->GetLabelColor()] {
                input->SetTextColor(text); input->SetLabelColor(label);
            });
        } else if (auto* button = dynamic_cast<Button*>(window)) {
            saved.colors.emplace_back([button, text = button->GetTextColor()] { button->SetTextColor(text); });
        } else if (auto* button = dynamic_cast<SideButton*>(window)) {
            saved.colors.emplace_back([button, text = button->GetForegroundColor(), background = button->GetBackgroundColor(),
                border = button->GetBorderColor(), bottom = button->GetBottomColour(), radius = button->GetCornerRadiusValue()] {
                button->SetForegroundColor(text); button->SetBackgroundColor(background); button->SetBorderColor(border);
                button->SetBottomColour(bottom); button->SetCornerRadius(radius);
            });
        }
        m_saved.push_back(std::move(saved));
    }
};

} // namespace Slic3r::GUI
