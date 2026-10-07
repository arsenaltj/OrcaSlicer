#pragma once
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r::GUI {

// The accepted shell dialog presents the current owner's confirmation text.
// Input/version validation and the single-request submission remain in PR20.
class ModelGenerationConfirmation final : public RedesignMessageDialog, public AIThemeOwner {
public:
    ModelGenerationConfirmation(wxWindow* parent, const wxString& message,
                                const wxString& title, const wxString& action)
        : RedesignMessageDialog(parent, message, title, wxYES_NO | wxNO_DEFAULT, true, true),
          m_release_timer(this) {
        set_action_label(wxID_YES, action);
        set_action_label(wxID_NO, _L("取消"));
        enable_action(wxID_YES, false);
        SetAffirmativeId(wxID_NO);
        SetEscapeId(wxID_NO);
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (!m_armed && (event.GetKeyCode() == WXK_RETURN ||
                             event.GetKeyCode() == WXK_NUMPAD_ENTER || event.GetKeyCode() == WXK_SPACE))
                return;
            event.Skip();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (wxGetKeyState(WXK_RETURN) || wxGetKeyState(WXK_NUMPAD_ENTER) || wxGetKeyState(WXK_SPACE))
                return;
            m_armed = true;
            m_release_timer.Stop();
            enable_action(wxID_YES, true);
        }, m_release_timer.GetId());
    }
    int ShowModal() override {
        m_release_timer.Start(25);
        const int result = RedesignMessageDialog::ShowModal();
        m_release_timer.Stop();
        return m_armed ? result : wxID_NO;
    }
    // RedesignMessageDialog owns its palette, rounded frame and leaf controls.
    void apply_ai_theme(bool) override { Refresh(false); }
private:
    wxTimer m_release_timer;
    bool m_armed {false};
};
} // namespace Slic3r::GUI
