#pragma once
#include "ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Redesign/RedesignFlowGuide.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r::GUI::ModelGenerationInputStyle {
class Welcome final : public wxPanel, public AIThemeOwner {
public:
    explicit Welcome(wxWindow* parent) : wxPanel(parent) {
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        SetMinSize(wxSize(1, 1));
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->AddStretchSpacer();
        m_title = new wxStaticText(this, wxID_ANY, _L("上传图片创建你的专属模型吧！"),
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL | wxST_ELLIPSIZE_END);
        m_title->SetMinSize(wxSize(1, -1));
        RedesignTheme::style_text(m_title, text, 28);
        root->Add(m_title, 0, wxEXPAND | wxBOTTOM, FromDIP(76));
        m_guide = new RedesignArtwork::FlowGuideCanvas(this);
        m_guide->SetMinSize(wxSize(1, FromDIP(259)));
        root->Add(m_guide, 0, wxEXPAND);
        root->AddStretchSpacer();
        SetSizer(root);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            const bool compact = GetClientSize().x < FromDIP(900) || GetClientSize().y < FromDIP(540);
            RedesignTheme::style_text(m_title, text, compact ? 18 : 28);
            m_title->SetLabel(compact ? _L("图片 → 设计图 → 3D 模型") :
                _L("上传图片创建你的专属模型吧！"));
            GetSizer()->GetItem(m_title)->SetBorder(FromDIP(compact ? 24 : 76));
            m_guide->SetMinSize(wxSize(1, FromDIP(compact ? 180 : 259)));
            m_guide->Refresh(false);
            event.Skip();
        });
    }
    void apply_ai_theme(bool) override {
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        m_title->SetForegroundColour(text);
        Refresh(false);
    }
private:
    wxStaticText* m_title;
    wxWindow* m_guide;
};
} // namespace Slic3r::GUI::ModelGenerationInputStyle
