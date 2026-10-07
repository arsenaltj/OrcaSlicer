void MainFrame::init_tabpanel() {
    wxString startup_trace_value;
    const bool startup_trace = wxGetEnv("ORCASLICER_STARTUP_TRACE", &startup_trace_value) && startup_trace_value == "1";
    auto stage_started = std::chrono::steady_clock::now();
    auto trace_stage = [&](const char* stage) {
        if (!startup_trace)
            return;
        const auto now = std::chrono::steady_clock::now();
        BOOST_LOG_TRIVIAL(info) << "Startup detail: " << stage << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count();
        stage_started = now;
    };
    // wxNB_NOPAGETHEME: Disable Windows Vista theme for the Notebook background. The theme performance is terrible on
    // Windows 10 with multiple high resolution displays connected.
    // BBS
    wxBoxSizer *side_tools = create_side_tools();
    m_tabpanel = new Notebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, side_tools,
                              wxNB_TOP | wxTAB_TRAVERSAL | wxNB_NOPAGETHEME);
    m_tabpanel->SetBackgroundColour(*wxWHITE);

#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    m_tabpanel->SetFont(Slic3r::GUI::wxGetApp().normal_font());
#endif
    m_tabpanel->Hide();
    m_settings_dialog.set_tabpanel(m_tabpanel);

#ifdef __WXMSW__
    m_tabpanel->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#else
    m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& e) {
#endif
        //BBS
        wxWindow* panel = m_tabpanel->GetCurrentPage();
        //wxString page_text = m_tabpanel->GetPageText(sel);
        m_last_selected_tab = m_tabpanel->GetSelectedPageName();
        if (m_workspace_navigation) m_workspace_navigation->refresh();
        if (panel == m_plater) {
            if (m_last_selected_tab == TAB_ID_PREPARE) {
                wxPostEvent(m_plater, SimpleEvent(EVT_GLVIEWTOOLBAR_3D));
                m_param_panel->OnActivate();
            }
            else if (m_last_selected_tab == TAB_ID_PREVIEW) {
                m_plater->reset_check_status();
                if (!m_plater->check_ams_status(m_slice_select == eSliceAll))
                    return;
                wxPostEvent(m_plater, SimpleEvent(EVT_GLVIEWTOOLBAR_PREVIEW));
                m_param_panel->OnActivate();
            }
            fit_tab_labels(); // ORCA on switching prepare / preview
        }
        //else if (panel == m_param_panel)
        //    m_param_panel->OnActivate();
        else if (panel == m_monitor) {
            //monitor
        }
#ifndef __APPLE__
        if (m_last_selected_tab == TAB_ID_PREPARE) {
            m_topbar->EnableUndoRedoItems();
        }
        else {
            m_topbar->DisableUndoRedoItems();
        }
#endif

        if (panel)
            panel->SetFocus();
    });

    if (wxGetApp().is_editor()) {
        m_webview         = new WebViewPanel(m_tabpanel);
        trace_stage("home_webview");
        Bind(EVT_LOAD_URL, [this](wxCommandEvent &evt) {
            wxString url = evt.GetString();
            select_tab(TAB_ID_HOME);
            m_webview->load_url(url);
        });
        m_tabpanel->AddPage(TAB_ID_HOME, m_webview, "", "tab_home_active");
        m_param_panel = new ParamsPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
        trace_stage("params_panel");
    }

    m_plater = new Plater(this, this);
    trace_stage("plater");
    m_plater->SetBackgroundColour(*wxWHITE);
    m_plater->Hide();

    wxGetApp().plater_ = m_plater;

    m_ai_feature_host = create_ai_feature_host(m_tabpanel, m_plater,
        [this] { select_tab(TAB_ID_PREPARE); }, [this] { register_ai_assistant(); },
        [&] { trace_stage("ai_feature_host"); });

    create_preset_tabs();
    trace_stage("preset_tabs");

        //BBS add pages
    m_monitor = new MonitorPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    trace_stage("monitor_panel");
    m_monitor->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_MONITOR, m_monitor, _L("Device"), "tab_monitor_active");

    m_printer_view = new PrinterWebView(m_tabpanel);
    trace_stage("printer_webview");
    Bind(EVT_LOAD_PRINTER_URL, [this](LoadPrinterViewEvent &evt) {
        wxString url = evt.GetString();
        wxString key = evt.GetAPIkey();
        //select_tab(MainFrame::tpMonitor);
        m_printer_view->load_url(url, key);
    });
    m_printer_view->Hide();

    if (wxGetApp().is_enable_multi_machine()) {
        m_multi_machine = new MultiMachinePage(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
        m_multi_machine->SetBackgroundColour(*wxWHITE);
        // TODO: change the bitmap
        m_tabpanel->AddPage(TAB_ID_MULTI_DEVICE, m_multi_machine, _L("Multi-device"), "tab_multi_active");
    }

    m_project = new ProjectPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    trace_stage("project_panel");
    m_project->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_PROJECT, m_project, _L("Project"), "tab_auxiliary_active");

    m_calibration = new CalibrationPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_calibration->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_CALIBRATION, m_calibration, _L("Calibration"), "tab_calibration_active");
    trace_stage("calibration_panel");

    // Plugin pages are appended after the built-in tabs; their ids are namespaced
    // (plugin.<plugin_key>.<name>) so they can't collide with the built-in TAB_ID_* constants.
    m_plugin_pages.initialize(m_tabpanel);
    trace_stage("plugin_pages");

    if (m_plater) {
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        trace_stage("initial_full_config");
        m_plater->on_config_change(full_config);
        trace_stage("initial_config_change");

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        // BBS
        if (full_config.has("filament_colour")) {
            m_plater->on_filament_count_change(full_config.option<ConfigOptionStrings>("filament_colour")->values.size());
        }
        trace_stage("initial_filament_count");
    }
    trace_stage("remaining_tabs_and_initial_config");
}
