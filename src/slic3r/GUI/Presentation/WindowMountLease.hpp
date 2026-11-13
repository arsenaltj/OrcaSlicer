#pragma once
#include "UiRuntime.hpp"
#include <algorithm>
#include <map>
#include <vector>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/window.h>

namespace Slic3r::GUI::Presentation {

// Stable retained layouts own their sizers. The original parent is checked
// before touching its sizer; a window is restored only by the current token.
class WindowMountLease {
public:
    using Token = MountGeneration::Token;
    WindowMountLease() = default;
    WindowMountLease(const WindowMountLease&) = delete;
    WindowMountLease& operator=(const WindowMountLease&) = delete;
    ~WindowMountLease() { if (m_token) forget(*m_token); }
    std::optional<Token> mount(wxWindow* window, wxWindow* parent, int proportion = 1,
                               int flags = wxEXPAND, int border = 0) {
        assert_gui_thread();
        if (!window || !parent || !parent->GetSizer()) return std::nullopt;
        if (m_token) return m_window.get() == window && window->GetParent() == parent ? m_token : std::nullopt;
        m_window = window; m_window_id = window; m_parent = window->GetParent(); m_target = parent; m_focus = wxWindow::FindFocus();
        m_sizer = window->GetContainingSizer(); m_shown = window->IsShown();
        if (m_sizer) {
            const auto& children = m_sizer->GetChildren(); m_position = 0;
            for (auto* item : children) {
                if (item->IsWindow() && item->GetWindow() == window) {
                    m_proportion = item->GetProportion(); m_flags = item->GetFlag(); m_border = item->GetBorder(); break;
                }
                ++m_position;
            }
            m_sizer->Detach(window);
        }
        if (!window->Reparent(parent)) {
            if (m_sizer) m_sizer->Insert(m_position, window, m_proportion, m_flags, m_border);
            m_window = nullptr; m_parent = nullptr; m_sizer = nullptr; return std::nullopt;
        }
        parent->GetSizer()->Add(window, proportion, flags, border);
        m_token = m_generation.acquire();
        owners()[window].push_back(this);
        return m_token;
    }
    bool restore(Token token) {
        assert_gui_thread();
        if (!m_token || !m_generation.current(token)) return false;
        auto* window = m_window.get(); auto* parent = m_parent.get();
        if (!window || !parent) { forget(token); return false; }
        const auto owner = owners().find(window);
        if (owner == owners().end() || owner->second.empty() || owner->second.back() != this ||
            window->GetParent() != m_target.get()) return false;
        // A retained parent may have rebuilt its layout. Never dereference its
        // old sizer, but still return the child before its temporary host dies.
        auto* restore_sizer = m_sizer && contains(parent->GetSizer(), m_sizer)
            ? m_sizer : m_sizer ? parent->GetSizer() : nullptr;
        auto* current_parent = window->GetParent();
        auto* current_sizer = window->GetContainingSizer();
        std::size_t position = 0;
        int proportion = 0, flags = 0, border = 0;
        if (current_sizer) {
            for (auto* item : current_sizer->GetChildren()) {
                if (item->IsWindow() && item->GetWindow() == window) {
                    proportion = item->GetProportion(); flags = item->GetFlag(); border = item->GetBorder(); break;
                }
                ++position;
            }
        }
        if (current_sizer) current_sizer->Detach(window);
        if (!window->Reparent(parent)) {
            if (current_sizer && current_parent == window->GetParent())
                current_sizer->Insert(position, window, proportion, flags, border);
            return false;
        }
        if (restore_sizer) restore_sizer->Insert(std::min(m_position, size_t(restore_sizer->GetChildren().GetCount())),
            window, m_proportion, m_flags, m_border);
        window->Show(m_shown);
        auto* focus = m_focus.get();
        if (focus && focus->IsShownOnScreen() && focus->IsEnabled() &&
            (focus == window || window->IsDescendant(focus))) focus->SetFocus();
        forget(token); return true;
    }
    bool restore() { return m_token && restore(*m_token); }
    bool active() const { return m_token.has_value(); }
private:
    static std::map<wxWindow*, std::vector<const WindowMountLease*>>& owners() {
        static std::map<wxWindow*, std::vector<const WindowMountLease*>> value;
        return value;
    }
    static bool contains(wxSizer* root, wxSizer* target) {
        if (!root) return false;
        if (root == target) return true;
        for (auto* item : root->GetChildren())
            if (item->IsSizer() && contains(item->GetSizer(), target)) return true;
        return false;
    }
    void forget(Token token) {
        if (auto it = owners().find(m_window_id); it != owners().end()) {
            auto& stack = it->second;
            stack.erase(std::remove(stack.begin(), stack.end(), this), stack.end());
            if (stack.empty()) owners().erase(it);
        }
        m_generation.release(token); m_token.reset(); m_window = nullptr; m_parent = nullptr; m_focus = nullptr; m_sizer = nullptr;
        m_target = nullptr; m_window_id = nullptr;
    }
    MountGeneration m_generation;
    std::optional<Token> m_token;
    wxWeakRef<wxWindow> m_window, m_parent, m_target, m_focus;
    wxWindow* m_window_id {nullptr};
    wxSizer* m_sizer {nullptr};
    std::size_t m_position {0};
    int m_proportion {0}, m_flags {0}, m_border {0};
    bool m_shown {false};
};
} // namespace Slic3r::GUI::Presentation
