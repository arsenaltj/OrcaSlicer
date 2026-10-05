#pragma once

#include "ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <algorithm>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r::GUI {

// Shared only by the two paid generation confirmations. The existing owner
// still validates the confirmed input and submits exactly one request.
class ModelGenerationConfirmation final : public MsgDialog, public AIThemeOwner
{
public:
    ModelGenerationConfirmation(wxWindow* parent, const wxString& message,
                                const wxString& title, const wxString& action)
        : MsgDialog(parent, title, wxEmptyString, wxYES_NO), m_release_timer(this)
    {
        logo->Hide();
        // No message-box brand illustration in the design. Keep the existing
        // dialog/button mechanics, with readable native text in the content slot.
        btn_sizer->GetItem(size_t(0))->SetBorder(FromDIP(20));
        auto* heading = new Label(this, Label::Head_20, title);
        content_sizer->Add(heading, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
        auto* body_view = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        body_view->SetScrollRate(0, FromDIP(12));
        auto* body = new Label(body_view, wxGetApp().normal_font(), message,
            LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(400, -1)));
        auto* body_sizer = new wxBoxSizer(wxVERTICAL);
        body_sizer->Add(body, 0, wxEXPAND);
        body_view->SetSizer(body_sizer);
        content_sizer->Add(body_view, 1, wxEXPAND | wxBOTTOM, FromDIP(16));
        SetButtonLabel(wxID_YES, action);
        SetButtonLabel(wxID_NO, _L("取消"), true);
        SetAffirmativeId(wxID_NO);
        SetEscapeId(wxID_NO);
        for (const int id : {wxID_YES, wxID_NO}) {
            auto* button = get_button(id);
            button->SetName(id == wxID_YES ? "input_primary" : "input_quiet");
            button->SetMinSize(FromDIP(wxSize(200, 48)));
            button->SetPaddingSize(FromDIP(wxSize(12, 10)));
        }
        get_button(wxID_YES)->Enable(false);
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
            get_button(wxID_YES)->Enable();
            apply_ai_theme(false);
        }, m_release_timer.GetId());
        SetMaxSize(wxDefaultSize);
        SetMinSize(FromDIP(wxSize(480, 249)));
        apply_ai_theme(true);
        body->Wrap(FromDIP(400));
        body_view->SetMinSize(wxSize(FromDIP(400), std::min(body->GetBestSize().y, FromDIP(240))));
        Fit();
        // Long filenames and fee/stop text scroll without pushing actions out
        // of the active main window. The title and default Cancel stay fixed.
        body_view->SetMinSize(wxSize(FromDIP(400), FromDIP(48)));
        const int available_height = wxGetTopLevelParent(parent)->GetClientSize().y - FromDIP(32);
        SetSize(wxSize(GetSize().x, std::max(FromDIP(249), std::min(GetSize().y, available_height))));
        Layout();
        body_view->FitInside();
        CentreOnParent();
    }

    int ShowModal() override
    {
        m_release_timer.Start(25);
        const int result = MsgDialog::ShowModal();
        m_release_timer.Stop();
        return m_armed ? result : wxID_NO;
    }

    void apply_ai_theme(bool fonts) override
    {
        ModelGenerationInputStyle::apply(this, fonts);
    }

private:
    wxTimer m_release_timer;
    bool m_armed {false};
};

} // namespace Slic3r::GUI
