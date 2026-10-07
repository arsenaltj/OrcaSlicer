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

void Plater::enable_smart_slicing()
{
    if (p->smart_slicing_host != nullptr)
        return;

    p->smart_slicing_host = std::make_unique<SmartSlicingFeatureHost>(
        *this, p->m_aui_mgr, *p->sidebar,
        [this] {
            if (printer_technology() != ptFFF ||
                p->process_completed_with_error == p->partplate_list.get_curr_plate_index())
                return false;
            PartPlate* plate = p->partplate_list.get_curr_plate();
            if (plate == nullptr || !plate->has_printable_instances())
                return false;
            const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
            Print& print = p->partplate_list.get_current_fff_print();
            Model::setExtruderParams(config, wxGetApp().preset_bundle->filament_presets.size());
            Model::setPrintSpeedTable(config, print.config());
            p->m_slice_all = false;
            plate->update_slice_result_valid_state(false);
            reslice();
            return p->m_is_slicing;
        });
}

void Plater::show_smart_slicing(bool show)
{
    if (p->smart_slicing_host != nullptr)
        p->smart_slicing_host->show(show);
}
