// Included once by Plater.cpp after Sidebar::priv is complete.
void Sidebar::build_ai_workflow_panel(wxSizer* scrolled_sizer)
{
    m_ai_workflow_panel = new wxPanel(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_ai_workflow_panel->SetBackgroundColour(p->scrolled->GetBackgroundColour());
    auto* ai_workflow_sizer = new wxBoxSizer(wxVERTICAL);
    auto* ai_workflow_title = new wxStaticText(m_ai_workflow_panel, wxID_ANY, _L("最近模型导入"));
    wxFont ai_title_font = ai_workflow_title->GetFont();
    ai_title_font.SetWeight(wxFONTWEIGHT_BOLD);
    ai_workflow_title->SetFont(ai_title_font);
    ai_workflow_sizer->Add(ai_workflow_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_ai_workflow_summary = new wxStaticText(m_ai_workflow_panel, wxID_ANY, _L("等待开始"));
    m_ai_workflow_summary->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    ai_workflow_sizer->Add(m_ai_workflow_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    const std::array<wxString, AIWorkflowStepCount> ai_step_names {
        _L("模型导入"), _L("网格检查/修复"), _L("颜色处理"),
        _L("自动摆放"), _L("切片"), _L("G-code")
    };
    for (size_t index = 0; index < ai_step_names.size(); ++index) {
        m_ai_workflow_steps[index] = new wxStaticText(
            m_ai_workflow_panel, wxID_ANY,
            wxString::Format("%llu. ", static_cast<unsigned long long>(index + 1)) + ai_step_names[index] + _L("  等待"));
        m_ai_workflow_steps[index]->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        ai_workflow_sizer->Add(m_ai_workflow_steps[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }
    ai_workflow_sizer->AddSpacer(FromDIP(10));
    m_ai_workflow_panel->SetSizer(ai_workflow_sizer);
    scrolled_sizer->Add(m_ai_workflow_panel, 0, wxEXPAND);
    m_ai_workflow_panel->Hide();
}
