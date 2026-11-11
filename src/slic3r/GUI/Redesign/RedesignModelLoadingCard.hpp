#pragma once

#include "HonorBrandMark.hpp"
#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../I18N.hpp"
#include <wx/sizer.h>
#include <wx/activityindicator.h>

namespace Slic3r::GUI {

class WorkbenchActivityIndicator final : public wxActivityIndicator
{
public:
    WorkbenchActivityIndicator(wxWindow* parent, const wxSize& size)
        : wxActivityIndicator(parent, wxID_ANY, wxDefaultPosition, size),
          m_background(workbench_owned_surface(parent))
    {
        SetBackgroundStyle(wxBG_STYLE_ERASE);
        SetBackgroundColour(m_background);
#ifdef __WXMSW__
        SetDoubleBuffered(true);
#endif
        Bind(wxEVT_ERASE_BACKGROUND, [this](wxEraseEvent& event) {
            if (auto* dc = event.GetDC()) {
                dc->SetBackground(wxBrush(m_background));
                dc->Clear();
            }
        });
    }

    bool HasTransparentBackground() override { return false; }

private:
    wxColour m_background;
};

// Both operations use the existing import card surface and metrics, but keep
// their own controls, timers and cancellation owners.
struct RedesignModelLoadingCard {
    wxDialog* window;
    wxActivityIndicator* activity;
    WorkbenchLabel *stage, *elapsed;
    wxWindow* cancel;
};

inline RedesignModelLoadingCard create_model_loading_card(wxWindow* owner,
    const wxString& label, const std::function<WorkbenchButton*(wxWindow*)>& make_cancel)
{
    auto* card = new WorkbenchFloatingCard(owner);
    auto* frame = new WorkbenchPaintedPanel(card, wxColour(45, 45, 49), 8, wxColour(72, 72, 78));
    frame->SetName("ai_content_color");
    frame->SetMinSize(owner->FromDIP(wxSize(410, 184)));
#ifdef __WXMSW__
    frame->SetDoubleBuffered(true);
#endif
    auto* loading = new wxBoxSizer(wxVERTICAL);
    frame->SetSizer(loading);
    loading->Add(new HonorBrandMark(frame), 0, wxLEFT | wxTOP, owner->FromDIP(16));
    auto* activity = new WorkbenchActivityIndicator(frame, owner->FromDIP(wxSize(36, 36)));
    activity->SetForegroundColour(RedesignTheme::accent_colour());
    activity->SetBackgroundColour(wxColour(45, 45, 49));
    loading->Add(activity, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, owner->FromDIP(16));
    auto* stage = new WorkbenchLabel(frame, label, wxALIGN_CENTER);
    stage->SetMinSize(owner->FromDIP(wxSize(370, 24)));
    RedesignTheme::style_text(stage, RedesignTheme::primary_text_colour(), 12);
    loading->Add(stage, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, owner->FromDIP(16));
    auto* elapsed = new WorkbenchLabel(frame, wxEmptyString, wxALIGN_CENTER);
    elapsed->SetMinSize(owner->FromDIP(wxSize(370, 20)));
    RedesignTheme::style_text(elapsed, RedesignTheme::secondary_text_colour(), 10);
    loading->Add(elapsed, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, owner->FromDIP(8));
    auto* cancel = make_cancel(frame);
    cancel->use_parent_surface();
    cancel->SetMinSize(owner->FromDIP(wxSize(160, 36)));
    loading->Add(cancel, 0, wxALIGN_CENTER | wxBOTTOM, owner->FromDIP(16));
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(frame, 1, wxEXPAND);
    card->SetSizerAndFit(root);
    card->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& event) {
        if (event.GetKeyCode() != WXK_ESCAPE) event.Skip();
    });
    card->Hide();
    return {card, activity, stage, elapsed, cancel};
}

} // namespace Slic3r::GUI
