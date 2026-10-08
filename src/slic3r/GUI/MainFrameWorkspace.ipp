// Native workspace and redesigned surface composition.

namespace {

bool ui_redesign_drag_trace_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("ORCASLICER_UI_REDESIGN_DRAG_TRACE");
        return value != nullptr && (*value == '1' || *value == 'y' || *value == 'Y' ||
                                    *value == 't' || *value == 'T');
    }();
    return enabled;
}

#ifdef __WXMSW__
const char* ui_redesign_win32_message_name(WXUINT message)
{
    switch (message) {
    case WM_NCLBUTTONDOWN: return "WM_NCLBUTTONDOWN";
    case WM_ENTERSIZEMOVE: return "WM_ENTERSIZEMOVE";
    case WM_EXITSIZEMOVE: return "WM_EXITSIZEMOVE";
    case WM_WINDOWPOSCHANGING: return "WM_WINDOWPOSCHANGING";
    case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
    case WM_NCHITTEST: return "WM_NCHITTEST";
    default: return nullptr;
    }
}
#endif

}

// Native workspace and redesigned surface composition.

void MainFrame::start_workflow_services()
{
    if (wxGetApp().is_editor() && m_ai_feature_host) m_ai_feature_host->start();
}

#ifdef __WXMSW__
WXLRESULT MainFrame::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    HWND hWnd = GetHandle();
    /* When we have a custom titlebar in the window, we don't need the non-client area of a normal window
     * to be painted. In order to achieve this, we handle the "WM_NCCALCSIZE" which is responsible for the
     * size of non-client area of a window and set the return value to 0. Also we have to tell the
     * application to not paint this area on activate and deactivation events so we also handle
     * "WM_NCACTIVATE" message. */
    switch (nMsg) {
    case WM_NCACTIVATE: {
        /* Returning 0 from this message disable the window from receiving activate events which is not
        desirable. However When a visual style is not active (?) for this window, "lParam" is a handle to an
        optional update region for the nonclient area of the window. If this parameter is set to -1,
        DefWindowProc does not repaint the nonclient area to reflect the state change. */
        lParam = -1;
        break;
    }
    /* To remove the standard window frame, you must handle the WM_NCCALCSIZE message, specifically when
    its wParam value is TRUE and the return value is 0 */
    case WM_NCCALCSIZE:
        if (wParam) {
            WINDOWPLACEMENT wPos;
            // GetWindowPlacement fail if this member is not set correctly.
            wPos.length = sizeof(wPos);
            GetWindowPlacement(hWnd, &wPos);
            NCCALCSIZE_PARAMS *sz = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
            RECT borderThickness;
            SetRectEmpty(&borderThickness);
            // Use & ~WS_CAPTION to get only the border thickness, not the caption height.
            // wxWidgets 3.3 adds WS_CAPTION when wxMINIMIZE_BOX/wxMAXIMIZE_BOX/wxCLOSE_BOX is set,
            // but we use a custom titlebar so we must exclude the caption from NC area calculations.
            AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, NULL);
            borderThickness.left *= -1;
            borderThickness.top *= -1;
            if (wPos.showCmd != SW_SHOWMAXIMIZED) {
                // Add 1 pixel to the top border to make the window resizable from the top border
                sz->rgrc[0].top += 1;
            } else {
                // When maximized, Windows extends the window beyond the screen by the border thickness.
                // Strip the full border overshoot so the client area matches the work area.
                sz->rgrc[0].top += borderThickness.top;
            }
            sz->rgrc[0].left += borderThickness.left;
            sz->rgrc[0].right -= borderThickness.right;
            sz->rgrc[0].bottom -= borderThickness.bottom;
            return 0;
        }
        break;

    case WM_NCLBUTTONDOWN:
    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
        if (ui_redesign_drag_trace_enabled()) {
            BOOST_LOG_TRIVIAL(info) << "[UiRedesignDrag][MainFrame] "
                                     << ui_redesign_win32_message_name(nMsg)
                                     << " wParam=" << static_cast<unsigned long long>(wParam)
                                     << " lParam=" << static_cast<long long>(lParam);
        }
        break;

    case WM_NCHITTEST: {
        const auto begin = std::chrono::steady_clock::now();
        const int screen_x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int screen_y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        WXLRESULT result = 0;
        if (IsMaximized()) {
            result = HTCAPTION;
        } else if (m_topbar != nullptr) {
            // Use the coordinates supplied by Windows. Calling wxGetMousePosition()
            // here can synchronously query the desktop input state while Windows is
            // beginning a move, which makes this path a suspect for drag latency.
            const wxPoint mouse_pos(screen_x, screen_y);
            if (m_topbar->GetScreenRect().GetBottom() >= mouse_pos.y) {
                RECT borderThickness;
                SetRectEmpty(&borderThickness);
                AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, NULL);
                borderThickness.left *= -1;
                borderThickness.top *= -1;
                const wxPoint client_pos = this->ScreenToClient(mouse_pos);
                const bool on_top_border = client_pos.y <= borderThickness.top;
                if (client_pos.x <= borderThickness.left)
                    result = on_top_border ? HTTOPLEFT : HTLEFT;
                else if (client_pos.x >= GetClientSize().x - borderThickness.right)
                    result = on_top_border ? HTTOPRIGHT : HTRIGHT;
                else
                    result = on_top_border ? HTTOP : HTCAPTION;
            }
        }
        if (ui_redesign_drag_trace_enabled()) {
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin).count();
            BOOST_LOG_TRIVIAL(info) << "[UiRedesignDrag][MainFrame] WM_NCHITTEST"
                                     << " screen=" << screen_x << "," << screen_y
                                     << " result=" << static_cast<long long>(result)
                                     << " elapsed_us=" << elapsed_us;
        }
        if (result != 0)
            return result;
        break;
    }

    case WM_GETMINMAXINFO: {
        auto mmi = (MINMAXINFO*) lParam;
        HandleGetMinMaxInfo(mmi);
        AdjustWorkingAreaForAutoHide(hWnd, mmi);
        return 0;
    }
    }
    return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}

#endif

void MainFrame::update_layout()
{
    auto restore_to_creation = [this]() {
        auto clean_sizer = [](wxSizer* sizer) {
            while (!sizer->GetChildren().IsEmpty()) {
                sizer->Detach(0);
            }
        };

        // On Linux m_plater needs to be removed from m_tabpanel before to reparent it
        int plater_page_id = m_tabpanel->FindPage(m_plater);
        if (plater_page_id != wxNOT_FOUND)
            m_tabpanel->RemovePage(plater_page_id);

        if (m_plater->GetParent() != this)
            m_plater->Reparent(this);

        if (m_tabpanel->GetParent() != this)
            m_tabpanel->Reparent(this);

        plater_page_id = (m_plater_page != nullptr) ? m_tabpanel->FindPage(m_plater_page) : wxNOT_FOUND;
        if (plater_page_id != wxNOT_FOUND) {
            m_tabpanel->DeletePage(plater_page_id);
            m_plater_page = nullptr;
        }

        clean_sizer(m_main_sizer);
        clean_sizer(m_settings_dialog.GetSizer());

        if (m_settings_dialog.IsShown())
            m_settings_dialog.Close();

        m_tabpanel->Hide();
        m_plater->Hide();

        Layout();
    };

    // The redesign shell owns the visible workspace once selected. Keep the
    // legacy notebook and Plater alive as migration hosts, but do not expose
    // them or allow a layout refresh to switch back to them.
    if (m_redesign_shell_requested && wxGetApp().is_editor()) {
        // Detach any notebook page before the shell reparents the native Plater.
        if (m_redesign_shell == nullptr || m_tabpanel->FindPage(m_plater) != wxNOT_FOUND)
            restore_to_creation();
        if (m_ai_feature_host != nullptr)
            m_ai_feature_host->initialize_model_generation_for_shell();
        if (m_redesign_shell == nullptr) {
            m_redesign_shell = new RedesignShell(
                this, m_ai_feature_host != nullptr ? m_ai_feature_host->model_generation_host() : nullptr, m_plater);
            if (m_ai_feature_host != nullptr)
                m_ai_feature_host->set_service_status_handler([this](AIServiceStatus status) {
                    if (m_redesign_shell != nullptr)
                        m_redesign_shell->set_service_status(status);
                });
        }
        m_redesign_shell_active = true;
        m_redesign_shell->Show();
        m_tabpanel->Hide();
        m_plater->Hide();
        m_main_sizer->Add(m_redesign_shell, 1, wxEXPAND);
        m_redesign_shell->refresh_workflow_layout();
        Layout();
        return;
    }

    //BBS GUI refactor: remove unused layout new/dlg
    //ESettingsLayout layout = wxGetApp().is_gcode_viewer() ? ESettingsLayout::GCodeViewer : ESettingsLayout::Old;
    ESettingsLayout layout =  ESettingsLayout::Old;

    if (m_layout == layout)
        return;

    wxBusyCursor busy;

    Freeze();

    // Remove old settings
    if (m_layout != ESettingsLayout::Unknown)
        restore_to_creation();

    ESettingsLayout old_layout = m_layout;
    m_layout = layout;

    // From the very beginning the Print settings should be selected
    //m_last_selected_tab = m_layout == ESettingsLayout::Dlg ? TAB_ID_HOME : TAB_ID_PREPARE;
    m_last_selected_tab = TAB_ID_PREPARE;

    // Set new settings
    switch (m_layout)
    {
    case ESettingsLayout::Old:
    {
        m_plater->Reparent(m_tabpanel);
        // Keep the optional generation page between Home and the two Plater views.
        // Name-based placement preserves the order when plugins add or remove pages.
        int anchor_idx = m_tabpanel->FindPageByName(TAB_ID_GENERATE_3D);
        if (anchor_idx == wxNOT_FOUND)
            anchor_idx = m_tabpanel->FindPageByName(TAB_ID_HOME);
        const size_t prepare_pos = (anchor_idx == wxNOT_FOUND) ? 0 : static_cast<size_t>(anchor_idx) + 1;
        m_tabpanel->InsertPage(prepare_pos, TAB_ID_PREPARE, m_plater, _L("Prepare"), "tab_3d_active");
        m_tabpanel->InsertPage(prepare_pos + 1, TAB_ID_PREVIEW, m_plater, _L("Preview"), "tab_preview_active");
        m_main_sizer->Add(m_tabpanel, 1, wxEXPAND | wxTOP, 0);

        m_tabpanel->Bind(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, [this](wxCommandEvent& evt)
        {
            // jump to 3deditor under preview_only mode
            if (evt.GetId() == m_tabpanel->FindPageByName(TAB_ID_PREPARE)) {
                Sidebar& sidebar = GUI::wxGetApp().sidebar();
                if (sidebar.need_auto_sync_after_connect_printer()) {
                    sidebar.set_need_auto_sync_after_connect_printer(false);
                    sidebar.sync_extruder_list();
                }

                m_plater->update(true);

                if (!preview_only_hint())
                    return;
            }
            evt.Skip();
        });

        m_plater->Show();
        m_tabpanel->Show();

        break;
    }
    case ESettingsLayout::GCodeViewer:
    {
        m_main_sizer->Add(m_plater, 1, wxEXPAND);
        //BBS: add bed exclude area
        m_plater->set_bed_shape({{0.0, 0.0}, {200.0, 0.0}, {200.0, 200.0}, {0.0, 200.0}}, {}, {}, 0.0, {}, {}, {}, {}, true);
        m_plater->get_collapse_toolbar().set_enabled(false);
        m_plater->enable_sidebar(false);
        m_plater->Show();
        break;
    }
    default:
        break;
    }

    //BBS GUI refactor: remove unused layout new/dlg
//#ifdef __APPLE__
//    // Using SetMinSize() on Mac messes up the window position in some cases
//    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
//    // So, if we haven't possibility to set MinSize() for the MainFrame,
//    // set the MinSize() as a half of regular  for the m_plater and m_tabpanel, when settings layout is in slNew mode
//    // Otherwise, MainFrame will be maximized by height
//    if (m_layout == ESettingsLayout::New) {
//        wxSize size = wxGetApp().get_min_size();
//        size.SetHeight(int(0.5 * size.GetHeight()));
//        m_plater->SetMinSize(size);
//        m_tabpanel->SetMinSize(size);
//    }
//#endif

#ifdef __APPLE__
    m_plater->sidebar().change_top_border_for_mode_sizer(m_layout != ESettingsLayout::Old);
#endif

    Layout();
    Thaw();
}

bool MainFrame::is_active_and_shown_tab(wxPanel* panel)
{
    if (m_redesign_shell_active)
        return (panel == m_redesign_shell && m_redesign_shell->IsShown()) ||
            ((panel == m_plater || panel == m_param_panel) && m_redesign_shell->native_workspace_visible());
    if (panel == m_param_panel)
        panel = m_plater;
    else
        return m_param_dialog->IsShown();

    if (m_tabpanel->GetCurrentPage() != panel)
        return false;
    return true;
}

void MainFrame::show_redesign_shell(bool show)
{
    if (!show && m_redesign_shell_active) {
        BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] refusing request to return from the redesign shell to the legacy workspace";
        return;
    }

    m_redesign_shell_requested = show;
    if (m_loaded)
        update_layout();
}

wxString MainFrame::selected_tab_id() const
{
    if (m_redesign_shell_active && m_redesign_shell != nullptr)
        return m_redesign_shell->active_tab_id();
    if (m_tabpanel != nullptr)
        return m_tabpanel->GetSelectedPageName();
    return wxString();
}

void MainFrame::focus_workspace_navigation()
{
    if (m_redesign_shell_active && m_redesign_shell != nullptr) {
        m_redesign_shell->SetFocus();
        return;
    }
    if (m_tabpanel != nullptr)
        m_tabpanel->SetFocus();
}

void MainFrame::set_workspace_enabled(bool enabled)
{
    if (m_redesign_shell_active && m_redesign_shell != nullptr)
        m_redesign_shell->Enable(enabled);
    else if (m_tabpanel != nullptr)
        m_tabpanel->Enable(enabled);
}

void MainFrame::jump_to_monitor(std::string dev_id)
{
    if (m_redesign_shell_active) {
        if (!dev_id.empty())
            select_device(dev_id);
        select_tab(TAB_ID_MONITOR);
        return;
    }
    if (!m_monitor)
        return;
    m_tabpanel->SelectPageByName(TAB_ID_MONITOR);
    if (!dev_id.empty())
        m_monitor->select_machine(dev_id);
}

void MainFrame::jump_to_monitor_hms()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (m_monitor) m_monitor->jump_to_HMS();
}

void MainFrame::jump_to_monitor_upgrade()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (m_monitor) m_monitor->jump_to_Upgrade();
}

void MainFrame::jump_to_monitor_live_view()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (m_monitor) m_monitor->jump_to_LiveView();
}

void MainFrame::jump_to_monitor_rack()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (m_monitor) m_monitor->jump_to_Rack();
}

void MainFrame::jump_to_monitor_playback(const std::string& dev_id)
{
    jump_to_monitor(dev_id);
    if (!m_redesign_shell_active && m_monitor)
        m_monitor->jump_to_LiveView();
}

void MainFrame::select_monitor_status(const std::string& dev_id)
{
    select_device(dev_id);
    select_tab(TAB_ID_MONITOR);
    if (!m_redesign_shell_active && m_monitor != nullptr)
        m_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PT_STATUS);
}

void MainFrame::jump_to_monitor_media()
{
    if (m_redesign_shell_active) {
        if (m_redesign_shell != nullptr) m_redesign_shell->show_printer_media();
        return;
    }
    jump_to_monitor();
    if (m_monitor) m_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PT_MEDIA);
}

void MainFrame::notify_hms_read(const wxString& error_code)
{
    if (m_redesign_shell_active || m_monitor == nullptr)
        return;
    wxCommandEvent event(EVT_ALREADY_READ_HMS);
    event.SetString(error_code);
    wxPostEvent(m_monitor, event);
}

void MainFrame::refresh_device_surface()
{
    if (m_redesign_shell_active) {
        if (m_redesign_shell != nullptr) m_redesign_shell->refresh_printer_state();
        return;
    }
    if (m_monitor != nullptr) {
        m_monitor->update_network_version_footer();
        m_monitor->set_default();
    }
}

void MainFrame::select_device(const std::string& dev_id)
{
    if (dev_id.empty())
        return;
    if (m_redesign_shell_active) {
        if (auto* manager = wxGetApp().getDeviceManager())
            manager->set_selected_machine(dev_id);
        return;
    }
    if (m_monitor != nullptr)
        m_monitor->select_machine(dev_id);
    else if (auto* manager = wxGetApp().getDeviceManager())
        manager->set_selected_machine(dev_id);
}

void MainFrame::update_monitor_error(MachineObject* obj)
{
    if (m_redesign_shell_active || m_monitor == nullptr)
        return;
    StatusPanel* status_panel = m_monitor->get_status_panel();
    if (status_panel != nullptr) {
        status_panel->obj = obj;
        status_panel->update_error_message();
    }
}

void MainFrame::layout_device_surface()
{
    if (m_redesign_shell_active) {
        if (m_redesign_shell != nullptr) m_redesign_shell->Layout();
        return;
    }
    if (m_monitor != nullptr)
        m_monitor->Layout();
}

void MainFrame::notify_calibration_job_finished(int tab_index, const wxString& payload)
{
    select_tab(TAB_ID_CALIBRATION);
    if (m_redesign_shell_active || m_calibration == nullptr)
        return;
    auto* page = static_cast<CalibrationWizard*>(m_calibration->get_tabpanel()->GetPage(tab_index));
    if (page == nullptr)
        return;
    wxCommandEvent event(EVT_CALIBRATION_JOB_FINISHED);
    event.SetString(payload);
    event.SetEventObject(page);
    wxPostEvent(page, event);
}

void MainFrame::update_print_error_info(int code, const std::string& message, const std::string& extra)
{
    if (m_redesign_shell_active || m_calibration == nullptr)
        return;
    m_calibration->update_print_error_info(code, message, extra);
}

void MainFrame::init_tabpanel() {
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
        Bind(EVT_LOAD_URL, [this](wxCommandEvent &evt) {
            wxString url = evt.GetString();
            select_tab(TAB_ID_HOME);
            m_webview->load_url(url);
        });
        m_tabpanel->AddPage(TAB_ID_HOME, m_webview, "", "tab_home_active");
        m_param_panel = new ParamsPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    }

    m_plater = new Plater(this, this);
    m_plater->SetBackgroundColour(*wxWHITE);
    m_plater->Hide();

    wxGetApp().plater_ = m_plater;

    m_ai_feature_host = std::make_unique<AIDesktopFeatureHost>(m_tabpanel, m_plater, [this] {
        m_plater->exit_gizmo();
        m_plater->update(true, true);
        select_tab(TAB_ID_PREPARE);
        // AI imports and color matching return to the native prepare page.
        // Keep the post-generation preparation controls visible so the user
        // can add a detached base immediately, without discovering the
        // Smart Slicing pane through the View menu first.
        if (m_redesign_shell == nullptr || !m_redesign_shell->owns_model_workflow())
            m_plater->show_smart_slicing(true);
    }, [this] { register_ai_assistant(); });
    m_tabpanel->AddPage(TAB_ID_GENERATE_3D, m_ai_feature_host->model_generation_panel(), _L("3D 生成"),
                        "tab_generate_3d_active");

    create_preset_tabs();

        //BBS add pages
    m_monitor = new MonitorPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_monitor->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_MONITOR, m_monitor, _L("Device"), "tab_monitor_active");

    m_printer_view = new PrinterWebView(m_tabpanel);
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
    m_project->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_PROJECT, m_project, _L("Project"), "tab_auxiliary_active");

    m_calibration = new CalibrationPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_calibration->SetBackgroundColour(*wxWHITE);
    m_tabpanel->AddPage(TAB_ID_CALIBRATION, m_calibration, _L("Calibration"), "tab_calibration_active");

    // Plugin pages are appended after the built-in tabs; their ids are namespaced
    // (plugin.<plugin_key>.<name>) so they can't collide with the built-in TAB_ID_* constants.
    m_plugin_pages.initialize(m_tabpanel);

    if (m_plater) {
        // load initial config
        auto full_config = wxGetApp().preset_bundle->full_config();
        m_plater->on_config_change(full_config);

        // Show a correct number of filament fields.
        // nozzle_diameter is undefined when SLA printer is selected
        // BBS
        if (full_config.has("filament_colour")) {
            m_plater->on_filament_count_change(full_config.option<ConfigOptionStrings>("filament_colour")->values.size());
        }
    }
}

void MainFrame::register_ai_assistant()
{
    if (m_plater != nullptr && m_view_menu != nullptr && !m_ai_assistant_registered) {
        m_plater->enable_ai_assistant();
        append_menu_check_item(
            m_view_menu, wxID_ANY, _L("高级参数助手"), _L("打开高级参数问答工具。"),
            [this](wxCommandEvent&) { m_plater->show_ai_assistant(!m_plater->is_ai_assistant_shown()); }, this,
            [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return m_plater->is_ai_assistant_shown(); }, this);
        m_ai_assistant_registered = true;
    }
}

bool MainFrame::is_prepare_or_preview_tab() const
{
    const wxString tab = selected_tab_id();
    if (m_redesign_shell_active && !m_redesign_shell->native_workspace_visible())
        return false;
    return tab == TAB_ID_PREPARE || tab == TAB_ID_PREVIEW;
}

bool MainFrame::route_legacy_command_to_redesign(const char *command, const wxString &tab_id)
{
    if (!m_redesign_shell_active)
        return false;
    if (m_redesign_shell && m_redesign_shell->owns_model_workflow() &&
        (std::strcmp(command, "Ctrl+O") == 0 || std::strcmp(command, "Open Project") == 0 ||
         std::strcmp(command, "Ctrl+N") == 0 || std::strcmp(command, "New Project") == 0 ||
         std::strcmp(command, "Ctrl+S") == 0 || std::strcmp(command, "Ctrl+Shift+S") == 0 ||
         std::strcmp(command, "Ctrl+I") == 0 || std::strcmp(command, "Import model") == 0 ||
         std::strcmp(command, "Import Zip Archive") == 0)) {
        if (!m_plater || m_plater->is_background_process_slicing()) return true;
        return !m_redesign_shell->navigate_to_tab(TAB_ID_PREPARE);
    }
    if (is_prepare_or_preview_tab() && tab_id != TAB_ID_MONITOR &&
        std::strcmp(command, "Ctrl+Shift+G") != 0 && std::strcmp(command, "Print button") != 0 &&
        std::strcmp(command, "Ctrl+P") != 0)
        return false;

    BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] routed legacy command to redesign shell: " << command;
    select_tab(tab_id.empty() ? TAB_ID_PROJECT : tab_id);
    return true;
}

void MainFrame::select_tab(wxPanel* panel)
{
    if (!panel)
        return;

    if (m_redesign_shell_active) {
        // Legacy callers still pass panel pointers. Resolve them to semantic
        // routes without looking them up in the hidden legacy notebook.
        wxString route;
        if (panel == m_param_panel || panel == m_plater)
            route = selected_tab_id() == TAB_ID_PREVIEW ? TAB_ID_PREVIEW : TAB_ID_PREPARE;
        else if (panel == m_monitor || panel == m_printer_view)
            route = TAB_ID_MONITOR;
        else if (panel == m_multi_machine)
            route = TAB_ID_MULTI_DEVICE;
        else if (panel == m_calibration)
            route = TAB_ID_CALIBRATION;
        else if (panel == m_project)
            route = TAB_ID_PROJECT;

        if (!route.empty())
            select_tab(route);
        else
            BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] ignored legacy panel route while redesign shell is active";
        return;
    }

    if (panel == m_param_panel) {
        panel = m_plater;
    } else if (dynamic_cast<ParamsPanel*>(panel)) {
        wxGetApp().params_dialog()->Popup();
        return;
    }
    // Not panel->GetName(): Prepare and Preview share the single m_plater window, so the
    // window has no one correct name. The slot -> id lookup is the only correct resolution.
    int page_idx = m_tabpanel->FindPage(panel);
    wxString page_name = (page_idx == wxNOT_FOUND) ? wxString() : m_tabpanel->GetPageName(static_cast<size_t>(page_idx));
    if (page_name == TAB_ID_PREPARE && m_tabpanel->GetSelectedPageName() == TAB_ID_PREVIEW)
        return;
    //BBS GUI refactor: remove unused layout new/dlg
    /*if (page_idx != wxNOT_FOUND && m_layout == ESettingsLayout::Dlg)
        page_idx++;*/
    select_tab(page_name);
}

void MainFrame::jump_to_multipage()
{
    if (m_redesign_shell_active) {
        select_tab(TAB_ID_MULTI_DEVICE);
        return;
    }
    if(!m_multi_machine)
        return;
    m_tabpanel->SelectPageByName(TAB_ID_MULTI_DEVICE);
    ((MultiMachinePage*)m_multi_machine)->jump_to_send_page();
}

void MainFrame::select_tab(const wxString& id/* = wxString()*/)
{
    if (m_redesign_shell_active && m_redesign_shell != nullptr) {
        const wxString requested = id.empty() ? m_redesign_shell->active_tab_id() : id;
        if (!m_redesign_shell->navigate_to_tab(requested))
            BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] failed to navigate to tab " << requested;
        return;
    }

    //bool tabpanel_was_hidden = false;

    // Controls on page are created on active page of active tab now.
    // We should select/activate tab before its showing to avoid an UI-flickering
    auto select = [this, id](bool was_hidden) {
        // when id is empty, it means we should show the last selected tab
        //BBS GUI refactor: remove unused layout new/dlg
        //size_t new_selection = tab == (size_t)(-1) ? m_last_selected_tab : (m_layout == ESettingsLayout::Dlg && tab != 0) ? tab - 1 : tab;
        wxString new_selection = id.empty() ? m_last_selected_tab : id;

        if (m_tabpanel->GetSelectedPageName() != new_selection)
            m_tabpanel->SelectPageByName(new_selection);
#ifdef _MSW_DARK_MODE
        /*if (wxGetApp().tabs_as_menu()) {
            if (Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPage(new_selection)))
                update_marker_for_tabs_menu((m_layout == ESettingsLayout::Old ? m_menubar : m_settings_dialog.menubar()), cur_tab->title(), m_layout == ESettingsLayout::Old);
            else if (tab == 0 && m_layout == ESettingsLayout::Old)
                m_plater->get_current_canvas3D()->render();
        }*/
#endif
        // Intentionally `id`, not `new_selection`: the fallback-to-last-tab path must not
        // trigger this render even when the last selected tab was Prepare.
        if (id == TAB_ID_PREPARE && m_layout == ESettingsLayout::Old)
            m_plater->canvas3D()->render();
        else if (was_hidden) {
            Tab* cur_tab = dynamic_cast<Tab*>(m_tabpanel->GetPageByName(new_selection));
            if (cur_tab)
                cur_tab->OnActivate();
        }
    };

    select(false);
}

bool MainFrame::get_enable_slice_status()
{
    bool enable = true;

    bool on_slicing = m_plater->is_background_process_slicing();
    if (on_slicing || m_plater->is_empty_project()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": on slicing or empty project, return false directly!";
        return false;
    }
    else if  (m_plater->only_gcode_mode() || m_plater->using_exported_file()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": in gcode/exported 3mf mode, return false directly!");
        return false;
    }

    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();

    if (m_slice_select == eSliceAll)
    {
        /*if (part_plate_list.is_all_slice_results_valid())
        {
            enable = false;
        }
        else if (!part_plate_list.is_all_plates_ready_for_slice())
        {
            enable = false;
        }*/
        //always enable slice_all button
        enable = true;
    }
    else if (m_slice_select == eSlicePlate)
    {
        if (current_plate->is_slice_result_valid())
        {
            enable = false;
        }
        else if (!current_plate->can_slice() || !current_plate->has_printable_instances())
        {
            enable = false;
        }
    }

    // A mixed filament whose components were deleted, or whose components disagree in type,
    // cannot be resolved at slicing time. Block the slice until the user fixes it.
    if (enable && m_plater->sidebar().has_broken_mixed_filament())
        enable = false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": m_slice_select %1%, enable= %2% ")%m_slice_select %enable;
    return enable;
}

bool MainFrame::get_enable_print_status()
{
    bool enable = true;

    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();
    bool is_all_plates = wxGetApp().plater()->get_preview_canvas3D()->is_all_plates_selected();
    if (m_print_select == ePrintAll)
    {
        if (!part_plate_list.is_all_slice_results_ready_for_print())
        {
            enable = false;
        }
    }
    else if (m_print_select == ePrintPlate)
    {
        if (!current_plate->is_slice_result_ready_for_print())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eExportGcode)
    {
        if (!current_plate->is_slice_result_valid())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eSendGcode)
    {
        if (!current_plate->is_slice_result_valid())
            enable = false;
        if (!can_send_gcode())
            enable = false;
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eUploadGcode)
    {
        if (!current_plate->is_slice_result_valid())
            enable = false;
        if (!can_send_gcode())
            enable = false;
        enable = enable && !is_all_plates;
    }
    else if (m_print_select == eExportSlicedFile)
    {
        if (!current_plate->is_slice_result_ready_for_export())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
	}
	else if (m_print_select == eSendToPrinter)
	{
		if (!current_plate->is_slice_result_ready_for_print())
		{
			enable = false;
		}
        enable = enable && !is_all_plates;
	}
    else if (m_print_select == eSendToPrinterAll)
    {
        if (!part_plate_list.is_all_slice_results_ready_for_print())
        {
            enable = false;
        }
    }
    else if (m_print_select == eExportAllSlicedFile)
    {
        if (!part_plate_list.is_all_slice_result_ready_for_export())
        {
            enable = false;
        }
    }
    else if (m_print_select == ePrintMultiMachine)
    {
        if (!current_plate->is_slice_result_ready_for_print())
        {
            enable = false;
        }
        enable = enable && !is_all_plates;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": m_print_select %1%, enable= %2% ")%m_print_select %enable;

    return enable;
}

void MainFrame::update_slice_print_status(SlicePrintEventType event, bool can_slice, bool can_print)
{
    bool enable_print = true, enable_slice = true;

    if (!can_slice)
    {
        if (m_slice_select == eSlicePlate)
            enable_slice = false;
    }
    if (!can_print)
        enable_print = false;


    //process print logic
    if (enable_print)
    {
        enable_print = get_enable_print_status();
    }

    //process slice logic
    if (enable_slice)
    {
        enable_slice = get_enable_slice_status();
    }

    bool old_slice_status = m_slice_btn->IsEnabled();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" m_slice_select %1%: can_slice= %2%, can_print %3%, enable_slice %4%, enable_print %5% ")%m_slice_select % can_slice %can_print %enable_slice %enable_print;
    m_print_btn->Enable(enable_print);
    m_slice_btn->Enable(enable_slice);
    m_slice_enable = enable_slice;
    m_print_enable = enable_print;

    if (!old_slice_status && enable_slice)
        m_plater->reset_check_status();

    if (wxGetApp().mainframe)
        wxGetApp().plater()->update_title_dirty_status();
}

void MainFrame::on_dpi_changed(const wxRect& suggested_rect)
{
    wxGetApp().update_fonts(this);
    this->SetFont(this->normal_font());
    if (m_ai_feature_host) refresh_ai_appearance(m_ai_feature_host->model_generation_panel());

#ifdef _MSW_DARK_MODE
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif

#ifndef __APPLE__
    // BBS
    m_topbar->Rescale();
#endif

    m_tabpanel->Rescale();

    update_side_button_style();

    m_slice_btn->Rescale();
    m_print_btn->Rescale();
    m_slice_option_btn->Rescale();
    m_print_option_btn->Rescale();

    // update Plater
    wxGetApp().plater()->msw_rescale();

    // update Tabs
    //BBS GUI refactor: remove unused layout new/dlg
    //if (m_layout != ESettingsLayout::Dlg) // Do not update tabs if the Settings are in the separated dialog
    m_param_panel->msw_rescale();
    m_project->msw_rescale();
    if(m_monitor)
        m_monitor->msw_rescale();
    if(m_multi_machine)
        m_multi_machine->msw_rescale();
    if(m_calibration)
        m_calibration->msw_rescale();

    // BBS
#if 0
    for (size_t id = 0; id < m_menubar->GetMenuCount(); id++)
        msw_rescale_menu(m_menubar->GetMenu(id));
#endif

    // Workarounds for correct Window rendering after rescale

    /* Even if Window is maximized during moving,
     * first of all we should imitate Window resizing. So:
     * 1. cancel maximization, if it was set
     * 2. imitate resizing
     * 3. set maximization, if it was set
     */
    const bool is_maximized = this->IsMaximized();
    if (is_maximized)
        this->Maximize(false);

    /* To correct window rendering (especially redraw of a status bar)
     * we should imitate window resizing.
     */
    const wxSize& sz = this->GetSize();
    this->SetSize(sz.x + 1, sz.y + 1);
    this->SetSize(sz);

    this->Maximize(is_maximized);

    fit_tab_labels(); // ORCA
}

void MainFrame::on_sys_color_changed()
{
    wxBusyCursor wait;

    // update label colors in respect to the system mode
    wxGetApp().init_label_colours();
    if (m_ai_feature_host) refresh_ai_appearance(m_ai_feature_host->model_generation_panel());

#ifndef __WINDOWS__
    wxGetApp().force_colors_update();
    wxGetApp().update_ui_from_settings();
#endif //__APPLE__

#ifdef __WXMSW__
    wxGetApp().UpdateDarkUI(m_tabpanel);
 //   m_statusbar->update_dark_ui();
#ifdef _MSW_DARK_MODE
    // update common mode sizer
    if (!wxGetApp().tabs_as_menu())
        dynamic_cast<Notebook*>(m_tabpanel)->Rescale();
#endif
#endif

    diff_dialog.on_sys_color_changed();

    // BBS
    m_tabpanel->Rescale();
    m_param_panel->msw_rescale();

    // update Plater
    wxGetApp().plater()->sys_color_changed();
    if(m_monitor)
        m_monitor->on_sys_color_changed();
    if(m_calibration)
        m_calibration->on_sys_color_changed();
    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->sys_color_changed();
    for (auto tab : wxGetApp().model_tabs_list)
        tab->sys_color_changed();
    wxGetApp().plate_tab->sys_color_changed();

    MenuFactory::sys_color_changed(m_menubar);

    WebView::RecreateAll();

    this->Refresh();
}

// On macOS, we use system menu bar, which handles the key accelerators automatically and breaks key handling in normal typing
// See https://github.com/OrcaSlicer/OrcaSlicer/issues/8152
// So we disable some of the accelerators on macOS, by replacing the accelerator seperator to a hyphen.
#ifdef __APPLE__
static const wxString sep = " - ";
#else
static const wxString sep = "\t";
#endif

static wxMenu* generate_help_menu()
{
    wxMenu* helpMenu = new wxMenu();

    // shortcut key
    append_menu_item(helpMenu, wxID_ANY, _L("Keyboard Shortcuts") + sep + "&?", _L("Show the list of keyboard shortcuts"),
        [](wxCommandEvent&) { wxGetApp().keyboard_shortcuts(); });
    // Show Beginner's Tutorial
    append_menu_item(helpMenu, wxID_ANY, _L("Setup Wizard"), _L("Setup Wizard"), [](wxCommandEvent &) {wxGetApp().ShowUserGuide();});

    helpMenu->AppendSeparator();

    // Open Config Folder
    append_menu_item(helpMenu, wxID_ANY, _L("Show Configuration Folder"), _L("Show Configuration Folder"),
        [](wxCommandEvent&) { Slic3r::GUI::desktop_open_datadir_folder(); });

    helpMenu->AppendSeparator();

    // Troubleshoot center
    append_menu_item(helpMenu, wxID_ANY, _L("Troubleshoot Center"), "",
        [](wxCommandEvent&) { wxGetApp().troubleshoot(); });

    append_menu_item(helpMenu, wxID_ANY, _L("Open Network Test"), _L("Open Network Test"), [](wxCommandEvent&) {
            NetworkTestDialog dlg(wxGetApp().mainframe);
            dlg.ShowModal();
        });

    helpMenu->AppendSeparator();

    append_menu_item(helpMenu, wxID_ANY, _L("Show Tip of the Day"), _L("Show Tip of the Day"), [](wxCommandEvent&) {
        wxGetApp().plater()->get_dailytips()->open();
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        });

    // Report a bug
    //append_menu_item(helpMenu, wxID_ANY, _L("Report Bug(TODO)"), _L("Report a bug of OrcaSlicer"),
    //    [](wxCommandEvent&) {
    //        //TODO
    //    });
    // Check New Version
    append_menu_item(helpMenu, wxID_ANY, _L("Check for Updates"), _L("Check for Updates"),
        [](wxCommandEvent&) {
            wxGetApp().check_new_version_sf(true, 1);
        }, "", nullptr, []() {
            return true;
        });

    // About
#ifndef __APPLE__
    wxString about_title = wxString::Format(_L("&About %s"), SLIC3R_APP_FULL_NAME);
    append_menu_item(helpMenu, wxID_ANY, about_title, about_title,
            [](wxCommandEvent&) { Slic3r::GUI::about(); });
#endif

    return helpMenu;
}


static void add_common_publish_menu_items(wxMenu* publish_menu, MainFrame* mainFrame)
{
#ifndef __WINDOWS__
    append_menu_item(publish_menu, wxID_ANY, _L("Upload Models"), _L("Upload Models"),
        [](wxCommandEvent&) {
            if (!wxGetApp().getAgent()) {
                BOOST_LOG_TRIVIAL(info) << "publish: no agent";
                return;
            }

            json j;
            NetworkAgent* agent = GUI::wxGetApp().getAgent();

            //if (GUI::wxGetApp().plater()->model().objects.empty()) return;
            wxGetApp().open_publish_page_dialog();
        });

    append_menu_item(publish_menu, wxID_ANY, _L("Download Models"), _L("Download Models"),
        [](wxCommandEvent&) {
            if (!wxGetApp().getAgent()) {
                BOOST_LOG_TRIVIAL(info) << "publish: no agent";
                return;
}

            //if (GUI::wxGetApp().plater()->model().objects.empty()) return;
            wxGetApp().open_mall_page_dialog();
        });
#endif
}

static void add_common_view_menu_items(wxMenu* view_menu, MainFrame* mainFrame, std::function<bool(void)> can_change_view)
{
    // The camera control accelerators are captured by GLCanvas3D::on_char().
    append_menu_item(view_menu, wxID_ANY, _L("Default View") + "\t" + ctrl + "0", _L("Default View"), [mainFrame](wxCommandEvent&) {
        mainFrame->select_view("plate");
        mainFrame->plater()->get_current_canvas3D()->zoom_to_bed();
        },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //view_menu->AppendSeparator();
    //TRN To be shown in the main menu View->Top
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Top", "Camera View") + "\t" + ctrl + "1", _L("Top View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("top"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    //TRN To be shown in the main menu View->Bottom
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Bottom", "Camera View") + "\t" + ctrl + "2", _L("Bottom View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("bottom"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Front", "Camera View") + "\t" + ctrl + "3", _L("Front View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("front"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Rear", "Camera View") + "\t" + ctrl + "4", _L("Rear View"), [mainFrame](wxCommandEvent&) { mainFrame->select_view("rear"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Left", "Camera View") + "\t" + ctrl + "5", _L("Left View"),[mainFrame](wxCommandEvent &) {mainFrame->select_view("left"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
    append_menu_item(view_menu, wxID_ANY, _L_CONTEXT("Right", "Camera View") + "\t" + ctrl + "6", _L("Right View"),[mainFrame](wxCommandEvent &) { mainFrame->select_view("right"); },
        "", nullptr, [can_change_view]() { return can_change_view(); }, mainFrame);
}

void MainFrame::init_menubar_as_editor()
{
#ifdef __APPLE__
    m_menubar = new wxMenuBar();
#endif

    // File menu
    wxMenu* fileMenu = new wxMenu;
    {
#ifdef __APPLE__
        // New Window
        append_menu_item(fileMenu, wxID_ANY, _L("New Window"), _L("Start a new window"),
                         [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
                         [this] { return m_plater != nullptr && wxGetApp().app_config->get("app", "single_instance") == "false"; }, this);
#endif
        // New Project
        append_menu_item(fileMenu, wxID_ANY, _L("New Project") + "\t" + ctrl + "N", _L("Start a new project"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("New Project", TAB_ID_PROJECT)) return; if (m_plater) m_plater->new_project(); }, "", nullptr,
            [this](){return can_start_new_project(); }, this);
        // Open Project

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Open Project") + dots + "\t" + ctrl + "O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Open Project", TAB_ID_PROJECT)) return; if (m_plater) m_plater->load_project(); }, "menu_open", nullptr,
            [this](){return can_open_project(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Open Project") + dots + "\t" + ctrl + "O", _L("Open a project file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->load_project(); }, "", nullptr,
            [this](){return can_open_project(); }, this);
#endif

        // Recent Project
        wxMenu* recent_projects_menu = new wxMenu();
        wxMenuItem* recent_projects_submenu = append_submenu(fileMenu, recent_projects_menu, wxID_ANY, _L("Recent files"), "");
        m_recent_projects.UseMenu(recent_projects_menu);
        Bind(wxEVT_MENU, [this](wxCommandEvent& evt) {
            size_t file_id = evt.GetId() - wxID_FILE1;
            wxString filename = m_recent_projects.GetHistoryFile(file_id);
                open_recent_project(file_id, filename);
            }, wxID_FILE1, wxID_FILE1 + 49); // [5050, 5100)

        std::vector<std::string> recent_projects = wxGetApp().app_config->get_recent_projects();
        std::reverse(recent_projects.begin(), recent_projects.end());
        for (const std::string& project : recent_projects)
        {
            m_recent_projects.AddFileToHistory(from_u8(project));
        }
        m_recent_projects.LoadThumbnails();

        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_open_project() && (m_recent_projects.GetCount() > 0)); }, recent_projects_submenu->GetId());

        // BBS: close save project
#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project") + "\t" + ctrl + "S", _L("Save current project to file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(); }, "menu_save", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project") + "\t" + ctrl + "S", _L("Save current project to file"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(); }, "", nullptr,
            [this](){return m_plater != nullptr && can_save(); }, this);
#endif

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project as") + dots + "\t" + ctrl + shift + "S", _L("Save current project as"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(true); }, "menu_save", nullptr,
            [this](){return m_plater != nullptr && can_save_as(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Save Project as") + dots + "\t" + ctrl + shift + "S", _L("Save current project as"),
            [this](wxCommandEvent&) { if (m_plater) m_plater->save_project(true); }, "", nullptr,
            [this](){return m_plater != nullptr && can_save_as(); }, this);
#endif


        fileMenu->AppendSeparator();

        // BBS
        wxMenu *import_menu = new wxMenu();
#ifndef __APPLE__
        append_menu_item(import_menu, wxID_ANY, _L("Import 3MF/STL/STEP/SVG/OBJ/AMF") + dots + "\t" + ctrl + "I", _L("Load a model"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Import model", TAB_ID_PROJECT)) return; if (m_plater) {
            m_plater->add_file();
        } }, "menu_import", nullptr,
            [this](){return can_add_models(); }, this);
#else
        append_menu_item(import_menu, wxID_ANY, _L("Import 3MF/STL/STEP/SVG/OBJ/AMF") + dots + "\t" + ctrl + "I", _L("Load a model"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Import model", TAB_ID_PROJECT)) return; if (m_plater) { m_plater->add_model(); } }, "", nullptr,
            [this](){return can_add_models(); }, this);
#endif
        append_menu_item(import_menu, wxID_ANY, _L("Import Zip Archive") + dots, _L("Load models contained within a zip archive"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Import Zip Archive", TAB_ID_PROJECT)) return; if (m_plater) m_plater->import_zip_archive(); }, "menu_import", nullptr,
            [this]() { return can_add_models(); });
        append_menu_item(import_menu, wxID_ANY, _L("Import Configs") + dots /*+ "\t" + ctrl + "I"*/, _L("Load configs"),
            [this](wxCommandEvent&) { load_config_file(); }, "menu_import", nullptr,
            [this](){return true; }, this);

        append_submenu(fileMenu, import_menu, wxID_ANY, _L("Import"), "");


        wxMenu* export_menu = new wxMenu();
        // BBS export as STL
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as one STL") + dots, _L("Export all objects as one STL"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export STL", TAB_ID_PROJECT)) return; if (m_plater) m_plater->export_stl(); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as STLs") + dots, _L("Export all objects as STLs"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export STLs", TAB_ID_PROJECT)) return; if (m_plater) m_plater->export_stl(false, false, true); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as one DRC") + dots, _L("Export all objects as one DRC"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export DRC", TAB_ID_PROJECT)) return; if (m_plater) m_plater->export_stl(false, false, false, FT_DRC); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export all objects as DRCs") + dots, _L("Export all objects as DRCs"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export DRCs", TAB_ID_PROJECT)) return; if (m_plater) m_plater->export_stl(false, false, true, FT_DRC); }, "menu_export_stl", nullptr,
            [this](){return can_export_model(); }, this);
        append_menu_item(export_menu, wxID_ANY, _L("Export Generic 3MF") + dots/* + "\t" + ctrl + "G"*/, _L("Export 3MF file without using some 3mf-extensions"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export Generic 3MF", TAB_ID_PROJECT)) return; if (m_plater) m_plater->export_core_3mf(); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_model(); }, this);
        // BBS export .gcode.3mf
        append_menu_item(export_menu, wxID_ANY, _L("Export plate sliced file") + dots + "\t" + ctrl + "G", _L("Export current sliced file"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export sliced file", TAB_ID_PREVIEW)) return; if (m_plater) wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE)); }, "menu_export_sliced_file", nullptr,
            [this](){return can_export_gcode(); }, this);

        append_menu_item(export_menu, wxID_ANY, _L("Export all plate sliced file") + dots/* + "\t" + ctrl + "G"*/, _L("Export all plate sliced file"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export all sliced files", TAB_ID_PREVIEW)) return; if (m_plater) wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE)); }, "menu_export_sliced_file", nullptr,
            [this]() {return can_export_all_gcode(); }, this);

        append_menu_item(export_menu, wxID_ANY, _L("Export G-code") + dots/* + "\t" + ctrl + "G"*/, _L("Export current plate as G-code"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export G-code", TAB_ID_PREVIEW)) return; if (m_plater) m_plater->export_gcode(false); }, "menu_export_gcode", nullptr,
            [this]() {return can_export_gcode(); }, this);

        append_menu_item(export_menu, wxID_ANY, _L("Export toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Export toolpaths", TAB_ID_PREVIEW)) return; if (m_plater != nullptr) m_plater->export_toolpaths_to_obj(); }, "menu_export_toolpaths", nullptr,
            [this]() {return can_export_toolpaths(); }, this);

        append_menu_item(
            export_menu, wxID_ANY, _L("Export Preset Bundle") + dots /* + "\t" + ctrl + "E"*/, _L("Export current configuration to files"),
            [this](wxCommandEvent &) { export_config(); },
            "menu_export_config", nullptr,
            []() { return true; }, this);

        append_submenu(fileMenu, export_menu, wxID_ANY, _L("Export"), "");

        fileMenu->AppendSeparator();

#ifndef __APPLE__
        append_menu_item(fileMenu, wxID_EXIT, _L("Quit"), wxString::Format(_L("Quit")),
            [this](wxCommandEvent&) { Close(false); }, "menu_exit", nullptr);
#else
        append_menu_item(fileMenu, wxID_EXIT, _L("Quit"), wxString::Format(_L("Quit")),
            [this](wxCommandEvent&) { Close(false); }, "", nullptr);
#endif
    }

    // Edit menu
    wxMenu* editMenu = nullptr;
    if (m_plater != nullptr && !m_redesign_shell_requested)
    {
        editMenu = new wxMenu();

    auto handle_key_event = [](wxKeyEvent& evt) {
        if (wxGetApp().imgui()->update_key_data(evt)) {
            wxGetApp().plater()->get_current_canvas3D()->render();
            return true;
        }
        return false;
    };
#ifndef __APPLE__
        // BBS undo
        append_menu_item(editMenu, wxID_ANY, _L("Undo") + "\t" + ctrl + "Z",
            _L("Undo"), [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Undo", TAB_ID_PROJECT)) return; m_plater->undo(); },
            "menu_undo", nullptr, [this](){return m_plater->can_undo(); }, this);
        // BBS redo
        append_menu_item(editMenu, wxID_ANY, _L("Redo") + "\t" + ctrl + "Y",
            _L("Redo"), [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Redo", TAB_ID_PROJECT)) return; m_plater->redo(); },
            "menu_redo", nullptr, [this](){return m_plater->can_redo(); }, this);
        editMenu->AppendSeparator();
        // BBS Cut TODO
        append_menu_item(editMenu, wxID_ANY, _L("Cut") + "\t" + ctrl + "X",
            _L("Cut selection to clipboard"), [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Cut", TAB_ID_PROJECT)) return; m_plater->cut_selection_to_clipboard(); },
            "menu_cut", nullptr, [this]() {return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Copy
        append_menu_item(editMenu, wxID_ANY, _L("Copy") + "\t" + ctrl + "C",
            _L("Copy selection to clipboard"), [this](wxCommandEvent&) { m_plater->copy_selection_to_clipboard(); },
            "menu_copy", nullptr, [this](){return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Paste
        append_menu_item(editMenu, wxID_ANY, _L("Paste") + "\t" + ctrl + "V",
            _L("Paste clipboard"), [this](wxCommandEvent&) { m_plater->paste_from_clipboard(); },
            "menu_paste", nullptr, [this](){return m_plater->can_paste_from_clipboard(); }, this);
        // BBS Delete selected
        append_menu_item(editMenu, wxID_ANY, _L("Delete Selected") + "\t" + _L_CONTEXT("Del", "Keyboard Shortcut"),
            _L("Deletes the current selection"),[this](wxCommandEvent&) { m_plater->remove_selected(); },
            "menu_remove", nullptr, [this](){return can_delete(); }, this);
        //BBS: delete all
        append_menu_item(editMenu, wxID_ANY, _L("Delete All") + "\t" + ctrl + "D",
            _L("Deletes all objects"),[this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Delete All", TAB_ID_PROJECT)) return; m_plater->delete_all_objects_from_model(); },
            "menu_remove", nullptr, [this](){return can_delete_all(); }, this);
        editMenu->AppendSeparator();
        // BBS Clone Selected
        append_menu_item(editMenu, wxID_ANY, _L("Clone Selected") /*+ "\t" + ctrl + "M"*/,
            _L("Clone copies of selections"),[this](wxCommandEvent&) {
                if (route_legacy_command_to_redesign("Clone Selected", TAB_ID_PROJECT)) return;
                 m_plater->clone_selection();
            },
            "menu_remove", nullptr, [this](){return can_clone(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Duplicate Current Plate"),
            _L("Duplicate the current plate"),[this](wxCommandEvent&) {
                if (route_legacy_command_to_redesign("Duplicate Current Plate", TAB_ID_PROJECT)) return;
                 m_plater->duplicate_plate();
            },
            "menu_remove", nullptr, [this](){return true;}, this);
        editMenu->AppendSeparator();
#else
        // BBS undo
        append_menu_item(editMenu, wxID_ANY, _L("Undo") + sep + ctrl_t + "Z",
            _L("Undo"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'Z';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->undo(); },
            "", nullptr, [this](){return m_plater->can_undo(); }, this);
        // BBS redo
        append_menu_item(editMenu, wxID_ANY, _L("Redo") + sep + ctrl_t + "Y",
            _L("Redo"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'Y';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->redo(); },
            "", nullptr, [this](){return m_plater->can_redo(); }, this);
        editMenu->AppendSeparator();
        // BBS Cut TODO
        append_menu_item(editMenu, wxID_ANY, _L("Cut") + sep + ctrl_t + "X",
            _L("Cut selection to clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'X';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->cut_selection_to_clipboard(); },
            "", nullptr, [this]() {return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Copy
        append_menu_item(editMenu, wxID_ANY, _L("Copy") + sep + ctrl_t + "C",
            _L("Copy selection to clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'C';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->copy_selection_to_clipboard(); },
            "", nullptr, [this](){return m_plater->can_copy_to_clipboard(); }, this);
        // BBS Paste
        append_menu_item(editMenu, wxID_ANY, _L("Paste") + sep + ctrl_t + "V",
            _L("Paste clipboard"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'V';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->paste_from_clipboard(); },
            "", nullptr, [this](){return m_plater->can_paste_from_clipboard(); }, this);
#if 0
        // BBS Delete selected
        append_menu_item(editMenu, wxID_ANY, _L("Delete Selected") + "\t" + _L_CONTEXT("Backspace", "Keyboard Shortcut"),
            _L("Deletes the current selection"),[this](wxCommandEvent&) {
                m_plater->remove_selected();
            },
            "", nullptr, [this](){return can_delete(); }, this);
#endif
        //BBS: delete all
        append_menu_item(editMenu, wxID_ANY, _L("Delete All") + "\t" + ctrl + "D",
            _L("Deletes all objects"),[this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'D';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->delete_all_objects_from_model(); },
            "", nullptr, [this](){return can_delete_all(); }, this);
        editMenu->AppendSeparator();
        // BBS Clone Selected
        append_menu_item(editMenu, wxID_ANY, _L("Clone Selected") + "\t" + ctrl + "K",
            _L("Clone copies of selections"),[this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'M';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->clone_selection();
            },
            "", nullptr, [this](){return can_clone(); }, this);
        editMenu->AppendSeparator();
        append_menu_item(editMenu, wxID_ANY, _L("Duplicate Current Plate"),
            _L("Duplicate the current plate"),[this, handle_key_event](wxCommandEvent&) {
                m_plater->duplicate_plate();
            },
            "", nullptr, [this](){return true;}, this);
        editMenu->AppendSeparator();

#endif

        // BBS Select All
        append_menu_item(editMenu, wxID_ANY, _L("Select All") + sep + ctrl_t + "A",
            _L("Selects all objects"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.SetControlDown(true);
                e.m_keyCode = 'A';
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->select_all(); },
            "", nullptr, [this](){return can_select(); }, this);
        // BBS Deslect All
        append_menu_item(editMenu, wxID_ANY, _L("Deselect All") + sep + _L_CONTEXT("Esc", "Keyboard Shortcut"),
            _L("Deselects all objects"), [this, handle_key_event](wxCommandEvent&) {
                wxKeyEvent e;
                e.SetEventType(wxEVT_KEY_DOWN);
                e.m_keyCode = WXK_ESCAPE;
                if (handle_key_event(e)) {
                    return;
                }
                m_plater->deselect_all(); },
            "", nullptr, [this](){return can_deselect(); }, this);
        //editMenu->AppendSeparator();
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Model Mesh(TODO)"),
        //    _L("Display triangles of models."), [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_model_mesh", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_model_mesh").compare("true") == 0; }, this);
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Model Shadow(TODO)"), _L("Display shadow of objects."),
        //    [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_model_shadow", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_model_shadow").compare("true") == 0; }, this);
        //editMenu->AppendSeparator();
        //append_menu_check_item(editMenu, wxID_ANY, _L("Show Printable Box(TODO)"), _L("Display printable box."),
        //    [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set_bool("show_printable_box", evt.GetInt() == 1);
        //    }, nullptr, [this]() {return can_select(); }, [this]() { return wxGetApp().app_config->get("show_printable_box").compare("true") == 0; }, this);
    }

    // BBS

    //publish menu

    /*if (m_plater) {
        publishMenu = new wxMenu();
        add_common_publish_menu_items(publishMenu, this);
        publishMenu->AppendSeparator();
    }*/

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater) {
        viewMenu = new wxMenu();
        m_view_menu = viewMenu;
        add_common_view_menu_items(viewMenu, this, std::bind(&MainFrame::can_change_view, this));
        viewMenu->AppendSeparator();

        m_plater->enable_smart_slicing();
        append_menu_check_item(
            viewMenu, wxID_ANY, _L("智能切片"), _L("检查当前打印板、比较方案并应用切片。"),
            [this](wxCommandEvent&) { m_plater->show_smart_slicing(!m_plater->is_smart_slicing_shown()); }, this,
            [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return m_plater->is_smart_slicing_shown(); }, this);
        viewMenu->AppendSeparator();

        //BBS perspective view
        wxWindowID camera_id_base = wxWindow::NewControlId(int(wxID_CAMERA_COUNT));
        auto perspective_item = append_menu_radio_item(viewMenu, wxID_CAMERA_PERSPECTIVE + camera_id_base, _L("Use Perspective View"), _L("Use Perspective View"),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("use_perspective_camera", true);
                wxGetApp().update_ui_from_settings();
            }, nullptr);
        //BBS orthogonal view
        auto orthogonal_item = append_menu_radio_item(viewMenu, wxID_CAMERA_ORTHOGONAL + camera_id_base, _L("Use Orthogonal View"), _L("Use Orthogonal View"),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("use_perspective_camera", false);
                wxGetApp().update_ui_from_settings();
            }, nullptr);
        this->Bind(wxEVT_UPDATE_UI, [viewMenu, camera_id_base](wxUpdateUIEvent& evt) {
                if (wxGetApp().app_config->get("use_perspective_camera").compare("true") == 0)
                    viewMenu->Check(wxID_CAMERA_PERSPECTIVE + camera_id_base, true);
                else
                    viewMenu->Check(wxID_CAMERA_ORTHOGONAL + camera_id_base, true);
            }, perspective_item->GetId());
        append_menu_check_item(viewMenu, wxID_ANY, _L("Auto Perspective"), _L("Automatically switch between orthographic and perspective when changing from top/bottom/side views."),
            [this](wxCommandEvent&) {
                wxGetApp().app_config->set_bool("auto_perspective", !wxGetApp().app_config->get_bool("auto_perspective"));
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return wxGetApp().app_config->get_bool("auto_perspective"); }, this);

        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &G-code Window") + sep + "C", _L("Show G-code window in Preview scene."),
            [this](wxCommandEvent &) {
                wxGetApp().toggle_show_gcode_window();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return is_prepare_or_preview_tab() && selected_tab_id() == TAB_ID_PREVIEW; },
            [this]() { return wxGetApp().show_gcode_window(); }, this);

        append_menu_check_item(
            viewMenu, wxID_ANY, _L("Show 3D Navigator"), _L("Show 3D navigator in Prepare and Preview scene."),
            [this](wxCommandEvent&) {
                wxGetApp().toggle_show_3d_navigator();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return wxGetApp().show_3d_navigator(); }, this);

        append_menu_check_item(viewMenu, wxID_ANY, _L("Show Gridlines"), _L("Show Gridlines on plate"),
            [this](wxCommandEvent&) {
                wxGetApp().toggle_show_plate_gridlines();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            }, this,
            [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return wxGetApp().show_plate_gridlines(); }, this);

        append_menu_item(
            viewMenu, wxID_ANY, _L("Reset Window Layout"), _L("Reset to default window layout"),
            [this](wxCommandEvent&) { m_plater->reset_window_layout(); }, "", this,
            [this]() {
                return is_prepare_or_preview_tab() && m_plater->is_sidebar_enabled();
            },
            this);

        viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Labels") + "\t" + ctrl + "E", _L("Show object labels in 3D scene."),
            [this](wxCommandEvent&) { m_plater->show_view3D_labels(!m_plater->are_view3D_labels_shown()); m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT)); }, this,
            [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->are_view3D_labels_shown(); }, this);

        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Overhang"), _L("Show object overhang highlight in 3D scene."),
            [this](wxCommandEvent &) {
                m_plater->show_view3D_overhang(!m_plater->is_view3D_overhang_shown());
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return m_plater->is_view3D_shown(); }, [this]() { return m_plater->is_view3D_overhang_shown(); }, this);

        append_menu_check_item(
            viewMenu, wxID_ANY, _L("Show Selected Outline (beta)"), _L("Show outline around selected object in 3D scene."),
            [this](wxCommandEvent&) {
                wxGetApp().toggle_show_outline();
                m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
            },
            this, [this]() { return is_prepare_or_preview_tab() && selected_tab_id() == TAB_ID_PREPARE; },
            [this]() { return wxGetApp().show_outline(); }, this);

        /*viewMenu->AppendSeparator();
        append_menu_check_item(viewMenu, wxID_ANY, _L("Show &Wireframe") + "\t" + ctrl + shift + _L("Enter"), _L("Show wireframes in 3D scene."),
            [this](wxCommandEvent&) { m_plater->toggle_show_wireframe(); m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT)); }, this,
            [this]() { return m_plater->is_wireframe_enabled(); }, [this]() { return m_plater->is_show_wireframe(); }, this);*/

        //viewMenu->AppendSeparator();
        ////BBS orthogonal view
        //append_menu_check_item(viewMenu, wxID_ANY, _L("Show Edges(TODO)"), _L("Show Edges."),
        //    [this](wxCommandEvent& evt) {
        //        wxGetApp().app_config->set("show_build_edges", evt.GetInt() == 1 ? "true" : "false");
        //    }, nullptr, [this]() {return can_select(); }, [this]() {
        //        std::string show_build_edges = wxGetApp().app_config->get("show_build_edges");
        //        return show_build_edges.compare("true") == 0;
        //    }, this);
    }

    wxWindowID config_id_base = wxWindow::NewControlId(int(ConfigMenuCnt));
    //TODO remove
    //auto config_wizard_name = _(ConfigWizard::name(true) + "(Debug)");
    //const auto config_wizard_tooltip = from_u8((boost::format(_utf8(L("Run %s"))) % config_wizard_name).str());
    //auto config_item = new wxMenuItem(m_topbar->GetTopMenu(), ConfigMenuWizard + config_id_base, config_wizard_name, config_wizard_tooltip);
#ifdef __APPLE__
    wxWindowID bambu_studio_id_base = wxWindow::NewControlId(int(2));
    wxMenu* parent_menu = m_menubar->OSXGetAppleMenu();
    //auto preference_item = new wxMenuItem(parent_menu, OrcaSlicerMenuPreferences + bambu_studio_id_base, _L("Preferences") + "\t" + ctrl + ",", "");
#else
    wxMenu* parent_menu = m_topbar->GetTopMenu();
    auto preference_item = new wxMenuItem(parent_menu, ConfigMenuPreferences + config_id_base, _L("Preferences") + "\t" + ctrl + "P", "");

#endif
    const auto config_wizard_name = _(ConfigWizard::name(true));
    const auto config_wizard_tooltip = from_u8((boost::format(_utf8(L("Open %s"))) % config_wizard_name).str());
    append_menu_item(
        parent_menu, wxID_ANY, config_wizard_name, config_wizard_tooltip,
        [](wxCommandEvent &) { wxGetApp().run_wizard(ConfigWizard::RR_USER); },
        "", nullptr, []() { return true; }, this);

    //auto printer_item = new wxMenuItem(parent_menu, ConfigMenuPrinter + config_id_base, _L("Printer"), "");
    //auto language_item = new wxMenuItem(parent_menu, ConfigMenuLanguage + config_id_base, _L("Switch Language"), "");
//    parent_menu->Bind(wxEVT_MENU, [this, config_id_base](wxEvent& event) {
//        switch (event.GetId() - config_id_base) {
//        //case ConfigMenuLanguage:
//        //{
//        //    /* Before change application language, let's check unsaved changes on 3D-Scene
//        //     * and draw user's attention to the application restarting after a language change
//        //     */
//        //    {
//        //        // the dialog needs to be destroyed before the call to switch_language()
//        //        // or sometimes the application crashes into wxDialogBase() destructor
//        //        // so we put it into an inner scope
//        //        wxString title = _L("Language selection");
//        //        wxMessageDialog dialog(nullptr,
//        //            _L("Switching the language requires application restart.\n") + "\n\n" +
//        //            _L("Do you want to continue?"),
//        //            title,
//        //            wxICON_QUESTION | wxOK | wxCANCEL);
//        //        if (dialog.ShowModal() == wxID_CANCEL)
//        //            return;
//        //    }
//
//        //    wxGetApp().switch_language();
//        //    break;
//        //}
//        //case ConfigMenuWizard:
//        //{
//        //    wxGetApp().run_wizard(ConfigWizard::RR_USER);
//        //    break;
//        //}
//        case ConfigMenuPrinter:
//        {
//            wxGetApp().params_dialog()->Popup();
//            wxGetApp().get_tab(Preset::TYPE_PRINTER)->restore_last_select_item();
//            break;
//        }
//        case ConfigMenuPreferences:
//        {
//            CallAfter([this] {
//                PreferencesDialog dlg(this);
//                dlg.ShowModal();
//#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//                if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
//#else
//                if (dlg.seq_top_layer_only_changed())
//#endif // ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//                    plater()->refresh_print();
//#if ENABLE_CUSTOMIZABLE_FILES_ASSOCIATION_ON_WIN
//#ifdef _WIN32
//                /*
//                if (wxGetApp().app_config()->get("associate_3mf") == "true")
//                    wxGetApp().associate_3mf_files();
//                if (wxGetApp().app_config()->get("associate_stl") == "true")
//                    wxGetApp().associate_stl_files();
//                /*if (wxGetApp().app_config()->get("associate_step") == "true")
//                    wxGetApp().associate_step_files();*/
//#endif // _WIN32
//#endif
//            });
//            break;
//        }
//        default:
//            break;
//        }
//    });

#ifdef __APPLE__
    wxString about_title = wxString::Format(_L("&About %s"), SLIC3R_APP_FULL_NAME);
    //auto about_item = new wxMenuItem(parent_menu, OrcaSlicerMenuAbout + bambu_studio_id_base, about_title, "");
        //parent_menu->Bind(wxEVT_MENU, [this, bambu_studio_id_base](wxEvent& event) {
        //    switch (event.GetId() - bambu_studio_id_base) {
        //        case OrcaSlicerMenuAbout:
        //            Slic3r::GUI::about();
        //            break;
        //        case OrcaSlicerMenuPreferences:
        //            CallAfter([this] {
        //                PreferencesDialog dlg(this);
        //                dlg.ShowModal();
        //#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
        //                if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
        //#else
        //                if (dlg.seq_top_layer_only_changed())
        //#endif // ENABLE_GCODE_LINES_ID_IN_H_SLIDER
        //                    plater()->refresh_print();
        //            });
        //            break;
        //        default:
        //            break;
        //    }
        //});
    //parent_menu->Insert(0, about_item);
    append_menu_item(
        parent_menu, wxID_ANY, _L(about_title), "",
        [this](wxCommandEvent &) { Slic3r::GUI::about();},
        "", nullptr, []() { return true; }, this, 0);
    append_menu_item(
        parent_menu, wxID_ANY, _L("Preferences") + "\t" + ctrl + ",", "",
        [this](wxCommandEvent &) {
            wxGetApp().open_preferences();
        },
        "", nullptr, []() { return true; }, this, 1);
    //parent_menu->Insert(1, preference_item);
#endif
    // Help menu
    auto helpMenu = generate_help_menu();

#ifndef __APPLE__
    m_topbar->SetFileMenu(fileMenu);
    if (editMenu)
        m_topbar->AddDropDownSubMenu(editMenu, _L_CONTEXT("Edit", "Menu"));
    if (viewMenu)
        m_topbar->AddDropDownSubMenu(viewMenu, _L("View"));
    //BBS add Preference

    append_menu_item(
        m_topbar->GetTopMenu(), wxID_ANY, _L("Preferences") + "\t" + ctrl + "P", "",
        [this](wxCommandEvent &) {
            // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
            wxGetApp().open_preferences();
        },
        "", nullptr, []() { return true; }, this);

    auto top_menu = m_topbar->GetTopMenu();
    top_menu->AppendSeparator();

        append_menu_item(
        top_menu, wxID_ANY, _L("Preset Bundle") + "\t", "",
        [this](wxCommandEvent &) {
            // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
            wxGetApp().open_presetbundledialog();
            plater()->get_current_canvas3D()->force_set_focus();
        },
        "", nullptr, []() { return true; }, this);

    append_menu_item(
        top_menu, wxID_ANY, _L("Sync Presets"), _L("Pull and apply the latest presets from OrcaCloud"),
        [this](wxCommandEvent&) {
            if (!wxGetApp().is_user_login()) {
                MessageDialog info_dlg(this, _L("You must be logged in to sync presets from cloud."),
                    _L("Sync Presets"), wxOK | wxICON_INFORMATION);
                info_dlg.ShowModal();
                return;
            }
            if (m_plater)
                m_plater->get_notification_manager()->push_notification(
                    into_u8(_L("Syncing presets from cloud\u2026")));
            wxGetApp().restart_sync_user_preset();
        }, "", nullptr,
        [this]() {
            return wxGetApp().is_user_login() && !wxGetApp().app_config->get_stealth_mode();
        }, this);

    top_menu->AppendSeparator();
    append_menu_item(
        top_menu, wxID_ANY, _L("Plugins") + "\t", "",
        [this](wxCommandEvent &) {
            wxGetApp().open_plugins_dialog();
        },
        "", nullptr, []() { return true; }, this);

    //m_topbar->AddDropDownMenuItem(preference_item);
    //m_topbar->AddDropDownMenuItem(printer_item);
    //m_topbar->AddDropDownMenuItem(language_item);
    //m_topbar->AddDropDownMenuItem(config_item);
    top_menu->AppendSeparator();
    m_topbar->AddDropDownSubMenu(helpMenu, _L("Help"));

    // SoftFever calibrations

    // Temperature
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Temperature"), _L("Temperature Calibration"),
        [this](wxCommandEvent&) {
            if (!m_temp_calib_dlg)
                m_temp_calib_dlg = new Temp_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_temp_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Max Volumetric Speed
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Max flowrate"), _L("Max flowrate"),
        [this](wxCommandEvent&) {
            if (!m_vol_test_dlg)
                m_vol_test_dlg = new MaxVolumetricSpeed_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vol_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Pressure Advance
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Pressure advance"), _L("Pressure advance"),
        [this](wxCommandEvent&) {
            if (!m_pa_calib_dlg)
                m_pa_calib_dlg = new PA_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_pa_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Flow rate (Wizard Dialog)
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Flow ratio"), _L("Flow Rate Calibration"),
        [this](wxCommandEvent&) {
            if (!m_plater) return;
            if (!m_flow_rate_calib_dlg)
                m_flow_rate_calib_dlg = new FlowRateCalibrationDialog((wxWindow*)this, wxID_ANY, m_plater);
            m_flow_rate_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Retraction
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Retraction"), _L("Retraction"),
        [this](wxCommandEvent&) {
            if (!m_retraction_calib_dlg)
                m_retraction_calib_dlg = new Retraction_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_retraction_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Cornering
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Cornering"), _L("Cornering calibration"),
        [this](wxCommandEvent&) {
            auto dlg = new Cornering_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Input Shaping (with submenu)
    auto input_shaping_menu = new wxMenu();
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Frequency"), _L("Input Shaping Frequency"),
        [this](wxCommandEvent&) {
            auto dlg = new Input_Shaping_Freq_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Damping/zeta factor"), _L("Input Shaping Damping/zeta factor"),
        [this](wxCommandEvent&) {
            auto dlg = new Input_Shaping_Damp_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    m_topbar->GetCalibMenu()->AppendSubMenu(input_shaping_menu, _L("Input Shaping"));

    // VFA
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("VFA"), _L("VFA"),
        [this](wxCommandEvent&) {
            if (!m_vfa_test_dlg)
                m_vfa_test_dlg = new VFA_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vfa_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // help
    append_menu_item(m_topbar->GetCalibMenu(), wxID_ANY, _L("Calibration Guide"), _L("Calibration Guide"), [this](wxCommandEvent &)
                     { wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/calibration_guide", wxBROWSER_NEW_WINDOW); }, "", nullptr, [this]()
                     {return m_plater->is_view3D_shown();; }, this);

#else
    // On Mac, the Apple menu ignores non-standard custom items, so add Preset Bundle to the File menu
    fileMenu->AppendSeparator();
    append_menu_item(
        fileMenu, wxID_ANY, _L("Preset Bundle"), "",
        [this](wxCommandEvent&) {
            wxGetApp().open_presetbundledialog();
            plater()->get_current_canvas3D()->force_set_focus();
        },
        "", nullptr, []() { return true; }, this);

    append_menu_item(
        fileMenu, wxID_ANY, _L("Sync Presets"), _L("Pull and apply the latest presets from OrcaCloud"),
        [this](wxCommandEvent&) {
            if (!wxGetApp().is_user_login()) {
                MessageDialog info_dlg(this, _L("You must be logged in to sync presets from cloud."),
                    _L("Sync Presets"), wxOK | wxICON_INFORMATION);
                info_dlg.ShowModal();
                return;
            }
            if (m_plater)
                m_plater->get_notification_manager()->push_notification(
                    into_u8(_L("Syncing presets from cloud\u2026")));
            wxGetApp().restart_sync_user_preset();
        }, "", nullptr,
        [this]() {
            return wxGetApp().is_user_login() && !wxGetApp().app_config->get_stealth_mode();
        }, this);

    fileMenu->AppendSeparator();
    append_menu_item(
        fileMenu, wxID_ANY, _L("Plugins"), "", [this](wxCommandEvent&) { wxGetApp().open_plugins_dialog(); }, "", nullptr,
        []() { return true; }, this);

    fileMenu->AppendSeparator();

    m_menubar->Append(fileMenu, wxString::Format("&%s", _L("File")));
    if (editMenu)
        m_menubar->Append(editMenu, wxString::Format("&%s", _L_CONTEXT("Edit", "Menu")));
    if (viewMenu)
        m_menubar->Append(viewMenu, wxString::Format("&%s", _L("View")));
    /*if (publishMenu)
        m_menubar->Append(publishMenu, wxString::Format("&%s", _L("3D Models")));*/

    // SoftFever calibrations
    auto calib_menu = new wxMenu();

    // Temperature
    append_menu_item(calib_menu, wxID_ANY, _L("Temperature"), _L("Temperature"),
        [this](wxCommandEvent&) {
            if (!m_temp_calib_dlg)
                m_temp_calib_dlg = new Temp_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_temp_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Max Volumetric Speed
    append_menu_item(calib_menu, wxID_ANY, _L("Max flowrate"), _L("Max flowrate"),
        [this](wxCommandEvent&) {
            if (!m_vol_test_dlg)
                m_vol_test_dlg = new MaxVolumetricSpeed_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vol_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Pressure Advance
    append_menu_item(calib_menu, wxID_ANY, _L("Pressure advance"), _L("Pressure advance"),
        [this](wxCommandEvent&) {
            if (!m_pa_calib_dlg)
                m_pa_calib_dlg = new PA_Calibration_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_pa_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Flowrate (with submenu)
    // ORCA: Flow rate (Wizard Dialog)
    append_menu_item(calib_menu, wxID_ANY, _L("Flow ratio"), _L("Flow Rate Calibration"),
        [this](wxCommandEvent&) {
            if (!m_plater) return;
            if (!m_flow_rate_calib_dlg)
                m_flow_rate_calib_dlg = new FlowRateCalibrationDialog((wxWindow*)this, wxID_ANY, m_plater);
            m_flow_rate_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Retraction
    append_menu_item(calib_menu, wxID_ANY, _L("Retraction"), _L("Retraction"),
        [this](wxCommandEvent&) {
            if (!m_retraction_calib_dlg)
                m_retraction_calib_dlg = new Retraction_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_retraction_calib_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Cornering
    append_menu_item(calib_menu, wxID_ANY, _L("Cornering"), _L("Cornering calibration"),
        [this](wxCommandEvent&) {
            auto dlg = new Cornering_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    // Input Shaping (with submenu)
    auto input_shaping_menu = new wxMenu();
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Frequency"), _L("Input Shaping Frequency"),
        [this](wxCommandEvent&) {
            auto dlg = new Input_Shaping_Freq_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    append_menu_item(
        input_shaping_menu, wxID_ANY, _L("Input Shaping Damping/zeta factor"), _L("Input Shaping Damping/zeta factor"),
        [this](wxCommandEvent&) {
            auto dlg = new Input_Shaping_Damp_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            dlg->ShowModal();
            dlg->Destroy();
        },
        "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    calib_menu->AppendSubMenu(input_shaping_menu, _L("Input Shaping"));

    // VFA
    append_menu_item(calib_menu, wxID_ANY, _L("VFA"), _L("VFA"),
        [this](wxCommandEvent&) {
            if (!m_vfa_test_dlg)
                m_vfa_test_dlg = new VFA_Test_Dlg((wxWindow*)this, wxID_ANY, m_plater);
            m_vfa_test_dlg->ShowModal();
        }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);
    // help
    append_menu_item(calib_menu, wxID_ANY, _L("Calibration Guide"), _L("Calibration Guide"),
        [this](wxCommandEvent&) { wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/calibration_guide", wxBROWSER_NEW_WINDOW); }, "", nullptr,
        [this]() {return m_plater->is_view3D_shown();; }, this);

    m_menubar->Append(calib_menu,wxString::Format("&%s", _L("Calibration")));
    if (helpMenu)
        m_menubar->Append(helpMenu, wxString::Format("&%s", _L("Help")));
    SetMenuBar(m_menubar);

#endif

#ifdef _MSW_DARK_MODE
    if (wxGetApp().tabs_as_menu())
        m_menubar->EnableTop(6, false);
#endif

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            Close();
        }, wxID_EXIT);
    }
#endif // __APPLE__
}

void MainFrame::bind_workspace_shortcuts()
{
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
#ifdef __APPLE__
        if (evt.CmdDown() && (evt.GetKeyCode() == 'H')) {
            //call parent_menu hide behavior
            return;}
        if (evt.CmdDown() && (!evt.ShiftDown()) && (evt.GetKeyCode() == 'M')) {
            this->Iconize();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'Q') { wxPostEvent(this, wxCloseEvent(wxEVT_CLOSE_WINDOW)); return;}
        if (evt.CmdDown() && evt.RawControlDown() && evt.GetKeyCode() == 'F') {
            EnableFullScreenView(true);
            if (IsFullScreen()) {
                ShowFullScreen(false);
            } else {
                ShowFullScreen(true);
            }
            return;}
#endif
        if (evt.CmdDown() && evt.GetKeyCode() == 'R') {
            if (m_redesign_shell_active && m_redesign_shell && m_redesign_shell->native_workspace_visible()) {
                m_redesign_shell->start_slicing_from_workspace();
                return;
            }
            if (m_slice_enable) {
                if (m_redesign_shell_active) {
                    // Preview is currently a migration host. Keep the shortcut in
                    // the new shell instead of invoking the hidden legacy Plater.
                    BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] Ctrl+R ignored legacy slice event while redesign shell is active";
                    select_tab(TAB_ID_PREVIEW);
                } else {
                    wxGetApp().plater()->update(true, true);
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                    m_tabpanel->SelectPageByName(TAB_ID_PREVIEW);
                }
            }
            return;
        }
        if (evt.CmdDown() && evt.ShiftDown() && evt.GetKeyCode() == 'G') {
            if (route_legacy_command_to_redesign("Ctrl+Shift+G", TAB_ID_PREVIEW))
                return;
            m_plater->apply_background_progress();
            m_print_enable = get_enable_print_status();
            m_print_btn->Enable(m_print_enable);
            if (m_print_enable) {
                if (wxGetApp().preset_bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents"))
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE));
                else
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_GCODE));
            }
            evt.Skip();
            return;
        }
        else if (evt.CmdDown() && evt.GetKeyCode() == 'G') {
            if (route_legacy_command_to_redesign("Ctrl+G", TAB_ID_PREVIEW))
                return;
            if (can_export_gcode())
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE));
            evt.Skip();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'J') {
            if (route_legacy_command_to_redesign("Ctrl+J", TAB_ID_MONITOR))
                return;
            m_printhost_queue_dlg->Show();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'N') {
            if (route_legacy_command_to_redesign("Ctrl+N", TAB_ID_PROJECT))
                return;
            m_plater->new_project();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'O') {
            if (route_legacy_command_to_redesign("Ctrl+O", TAB_ID_PROJECT))
                return;
            m_plater->load_project();
            return;
        }
        if (evt.CmdDown() && evt.ShiftDown() && evt.GetKeyCode() == 'S') {
            if (route_legacy_command_to_redesign("Ctrl+Shift+S", TAB_ID_PROJECT))
                return;
            if (can_save_as()) m_plater->save_project(true);
            return;
        }
        else if (evt.CmdDown() && evt.GetKeyCode() == 'S') {
            if (route_legacy_command_to_redesign("Ctrl+S", TAB_ID_PROJECT))
                return;
            if (can_save()) m_plater->save_project();
            return;
        }
        if (evt.CmdDown() && evt.GetKeyCode() == 'F') {
            if (route_legacy_command_to_redesign("Ctrl+F", TAB_ID_PROJECT))
                return;
            if (m_plater && is_prepare_or_preview_tab()) {
                m_plater->sidebar().can_search();
            }
        }
#ifdef __APPLE__
        if (evt.CmdDown() && evt.GetKeyCode() == ',')
#else
        if (evt.CmdDown() && evt.GetKeyCode() == 'P')
#endif
        {
            if (route_legacy_command_to_redesign("Ctrl+P"))
                return;
            // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
            wxGetApp().open_preferences();
            plater()->get_current_canvas3D()->force_set_focus();
            return;
        }

        if (evt.CmdDown() && evt.GetKeyCode() == 'I' && !evt.ShiftDown()) {
            if (route_legacy_command_to_redesign("Ctrl+I", TAB_ID_PROJECT))
                return;
            if (!can_add_models()) return;
            if (m_plater) { m_plater->add_file(); }
            return;
        }
        evt.Skip();
    });

}


bool MainFrame::can_start_new_project() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
/*return m_plater && (!m_plater->get_project_filename(".3mf").IsEmpty() ||
                        GetTitle().StartsWith('*')||
                        wxGetApp().has_current_preset_changes() ||
                        !m_plater->model().objects.empty());*/
    return (m_plater && !m_plater->is_background_process_slicing());
}

bool MainFrame::can_open_project() const
{
    if (m_redesign_shell_active && !is_prepare_or_preview_tab() &&
        !(m_redesign_shell && m_redesign_shell->owns_model_workflow()))
        return false;
    return m_plater && !m_plater->is_background_process_slicing();
}

bool  MainFrame::can_add_models() const
{
    if (m_redesign_shell_active && !is_prepare_or_preview_tab() &&
        !(m_redesign_shell && m_redesign_shell->owns_model_workflow()))
        return false;
    return m_plater && !m_plater->is_background_process_slicing() && !m_plater->only_gcode_mode() && !m_plater->using_exported_file();
}

bool MainFrame::can_save() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
return (m_plater != nullptr) &&
        !m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(false) &&
        m_plater->is_project_dirty() && !m_plater->using_exported_file() && !m_plater->only_gcode_mode();
}

bool MainFrame::can_save_as() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
return (m_plater != nullptr) &&
        !m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(false) && !m_plater->using_exported_file() && !m_plater->only_gcode_mode();
}

void MainFrame::save_project()
{
    save_project_as(m_plater->get_project_filename(".3mf"));
}

bool MainFrame::save_project_as(const wxString& filename)
{
    bool ret = (m_plater != nullptr) ? m_plater->export_3mf(into_path(filename)) : false;
    if (ret) {
//        wxGetApp().update_saved_preset_from_current_preset();
        m_plater->reset_project_dirty_after_save();
    }
    return ret;
}

bool MainFrame::can_upload() const
{
    return true;
}

bool MainFrame::can_export_model() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
return (m_plater != nullptr) && !m_plater->model().objects.empty();
}

bool MainFrame::can_export_toolpaths() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
return (m_plater != nullptr) && (m_plater->printer_technology() == ptFFF) && m_plater->is_preview_shown() && m_plater->is_preview_loaded() && m_plater->has_toolpaths_to_export();
}

bool MainFrame::can_export_supports() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
if ((m_plater == nullptr) || (m_plater->printer_technology() != ptSLA) || m_plater->model().objects.empty())
        return false;

    bool can_export = false;
    const PrintObjects& objects = m_plater->sla_print().objects();
    for (const SLAPrintObject* object : objects)
    {
        if (object->has_mesh(slaposPad) || object->has_mesh(slaposSupportTree))
        {
            can_export = true;
            break;
        }
    }
    return can_export;
}

bool MainFrame::can_export_gcode() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters
    PartPlateList &part_plate_list = m_plater->get_partplate_list();
    PartPlate *current_plate = part_plate_list.get_curr_plate();
    if (!current_plate->is_slice_result_ready_for_print())
        return false;

    return true;
}

bool MainFrame::can_export_all_gcode() const
{
        if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters
    PartPlateList& part_plate_list = m_plater->get_partplate_list();
    return part_plate_list.is_all_slice_results_ready_for_print();
}

bool MainFrame::can_print_3mf() const
{
        if (m_redesign_shell_active)
        return false;
if (m_plater && !m_plater->model().objects.empty()) {
        //
    }
    return true;
}

bool MainFrame::can_send_gcode() const
{
        if (m_redesign_shell_active)
        return false;
if (m_plater && !m_plater->model().objects.empty())
    {
        auto cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;

        const auto *print_host_opt = cfg.option<ConfigOptionString>("print_host");
        if (! print_host_opt) return false;
        else return !print_host_opt->value.empty();
    }
    return true;
}

/*bool MainFrame::can_export_gcode_sd() const
{
    if (m_plater == nullptr)
        return false;

    if (m_plater->model().objects.empty())
        return false;

    if (m_plater->is_export_gcode_scheduled())
        return false;

    // TODO:: add other filters

    return wxGetApp().removable_drive_manager()->status().has_removable_drives;
}

bool MainFrame::can_eject() const
{
	return wxGetApp().removable_drive_manager()->status().has_eject;
}*/

bool MainFrame::can_slice() const
{
    if (m_redesign_shell_active && !is_prepare_or_preview_tab())
        return false;
#ifdef SUPPORT_BACKGROUND_PROCESSING
    bool bg_proc = wxGetApp().app_config->get("background_processing") == "1";
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() && !bg_proc : false;
#else
    return (m_plater != nullptr) ? !m_plater->model().objects.empty() : false;
#endif
}

bool MainFrame::can_change_view() const
{
    if (m_redesign_shell_active)
        return is_prepare_or_preview_tab();
    switch (m_layout)
    {
    default:                   { return false; }
    //BBS GUI refactor: remove unused layout new/dlg
    case ESettingsLayout::Old: {
        int page_id = m_tabpanel->GetSelection();
        return page_id != wxNOT_FOUND && dynamic_cast<const Slic3r::GUI::Plater*>(m_tabpanel->GetPage((size_t)page_id)) != nullptr;
    }
    case ESettingsLayout::GCodeViewer: { return true; }
    }
}

bool MainFrame::can_clone() const {
    return can_select() && !m_plater->is_selection_empty();
}

bool MainFrame::can_select() const
{
    return is_prepare_or_preview_tab() && (m_plater != nullptr) && (selected_tab_id() == TAB_ID_PREPARE) && !m_plater->model().objects.empty();
}

bool MainFrame::can_deselect() const
{
    return is_prepare_or_preview_tab() && (m_plater != nullptr) && (selected_tab_id() == TAB_ID_PREPARE) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete() const
{
    return is_prepare_or_preview_tab() && (m_plater != nullptr) && (selected_tab_id() == TAB_ID_PREPARE) && !m_plater->is_selection_empty();
}

bool MainFrame::can_delete_all() const
{
    return is_prepare_or_preview_tab() && (m_plater != nullptr) && (selected_tab_id() == TAB_ID_PREPARE) && !m_plater->model().objects.empty();
}

bool MainFrame::can_reslice() const
{
    return is_prepare_or_preview_tab() && (m_plater != nullptr) && !m_plater->model().objects.empty();
}
