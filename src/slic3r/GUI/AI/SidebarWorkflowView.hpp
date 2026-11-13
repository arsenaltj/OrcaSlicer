#pragma once

#include "AIDesktopFeatureHost.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <array>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/settings.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

// The retained native import status view owns only its controls and display
// state. Import/slice decisions remain with their existing sessions.
class SidebarWorkflowView final : public wxPanel {
public:
    SidebarWorkflowView(wxWindow* parent, wxWindow* sidebar)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_sidebar(sidebar)
    {
        SetBackgroundColour(parent->GetBackgroundColour());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, _L("最近模型导入"));
        auto font = title->GetFont(); font.SetWeight(wxFONTWEIGHT_BOLD); title->SetFont(font);
        sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        m_summary = new wxStaticText(this, wxID_ANY, _L("等待开始"));
        m_summary->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        sizer->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        const auto names = step_names();
        for (size_t i = 0; i < names.size(); ++i) {
            m_steps[i] = new wxStaticText(this, wxID_ANY, prefix(i) + names[i] + _L("  等待"));
            m_steps[i]->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
            sizer->Add(m_steps[i], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }
        sizer->AddSpacer(FromDIP(10));
        SetSizer(sizer);
        Hide();
    }

    void start(const wxString& summary)
    {
        m_active = false;
        m_summary->SetLabel(summary);
        m_summary->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        for (size_t i = 0; i < m_steps.size(); ++i) update(i, AIWorkflowStatus::Waiting, {});
        m_active = true;
        Show(); Layout(); layout_owner();
    }
    void update(size_t index, AIWorkflowStatus status, const wxString& detail)
    {
        if (index >= m_steps.size()) return;
        wxString state; wxColour colour;
        describe_ai_workflow_status(status, state, colour);
        auto label = prefix(index) + step_names()[index] + "  " + state;
        if (!detail.empty()) label += _L(" · ") + detail;
        m_steps[index]->SetLabel(label);
        m_steps[index]->SetForegroundColour(colour);
        m_steps[index]->Wrap(FromDIP(340));
        layout_owner();
    }
    void finish(bool success, const wxString& summary, bool cancelled)
    {
        if (!m_active) return;
        m_summary->SetLabel(summary);
        m_summary->SetForegroundColour(cancelled ? wxColour(100,100,100) : success ? wxColour(46,125,50) : wxColour(179,38,30));
        m_summary->Wrap(FromDIP(340));
        layout_owner();
        if (cancelled) for (auto* step : m_steps) step->SetLabel(wxEmptyString);
        m_active = false;
    }
    bool active() const { return m_active; }

private:
    static std::array<wxString, 6> step_names()
    {
        return {_L("模型导入"), _L("网格检查/修复"), _L("颜色处理"), _L("自动摆放"), _L("切片"), _L("G-code")};
    }
    static wxString prefix(size_t i) { return wxString::Format("%llu. ", static_cast<unsigned long long>(i + 1)); }
    void layout_owner() { GetParent()->Layout(); if (m_sidebar) m_sidebar->Layout(); }
    wxWeakRef<wxWindow> m_sidebar;
    wxStaticText* m_summary{};
    std::array<wxStaticText*, 6> m_steps{};
    bool m_active{false};
};

} // namespace Slic3r::GUI
