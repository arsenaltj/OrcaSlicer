bool Plater::is_ai_assistant_shown() const
{
    return p->ai_assistant_panel != nullptr && p->m_aui_mgr.GetPane(p->ai_assistant_panel).IsShown();
}

void Plater::enable_ai_assistant()
{
    if (p->ai_assistant_panel != nullptr)
        return;

    p->ai_assistant_panel = new AIAssistantPanel(this, this);
    p->m_aui_mgr.AddPane(p->ai_assistant_panel, wxAuiPaneInfo()
                                                     .Name("ai_assistant")
                                                     .Caption(_L("AI Assistant"))
                                                     .Right()
                                                     .CloseButton(true)
                                                     .TopDockable(false)
                                                     .BottomDockable(false)
                                                     .BestSize(wxSize(32 * wxGetApp().em_unit(), 70 * wxGetApp().em_unit()))
                                                     .Hide());
    p->m_aui_mgr.Update();
}

void Plater::show_ai_assistant(bool show)
{
    if (p->ai_assistant_panel == nullptr)
        return;
    auto& pane = p->m_aui_mgr.GetPane(p->ai_assistant_panel);
    if (!pane.IsOk())
        return;
    pane.Show(show);
    p->m_aui_mgr.Update();
}

bool Plater::is_smart_slicing_shown() const
{
    return p->smart_slicing_host != nullptr && p->smart_slicing_host->is_shown();
}

#include "AI/PlaterSmartSlicingWorkflow.ipp"

void Plater::show_smart_slicing(bool show)
{
    if (p->smart_slicing_host != nullptr)
        p->smart_slicing_host->show(show);
}
