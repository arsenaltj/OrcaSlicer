void MainFrame::mount_native_workspace_tools(wxWindow* host)
{
    if (!m_native_side_tools || !m_slice_btn || !m_print_btn) return;
    auto* current = m_slice_btn->GetParent()->GetParent();
    if (host == current) return;
    if (host) m_native_side_tools_parent = current;
    else host = m_native_side_tools_parent;
    if (!host || !current->GetSizer() || !host->GetSizer()) return;
    current->GetSizer()->Detach(m_native_side_tools);
    m_slice_btn->GetParent()->Reparent(host);
    m_print_btn->GetParent()->Reparent(host);
    host->GetSizer()->Add(m_native_side_tools, 0, wxALIGN_CENTER_VERTICAL);
    host->Layout();
    current->Layout();
}

void MainFrame::slice_native_workspace()
{
    if (!m_slice_btn || !is_prepare_or_preview_tab()) return;
    wxCommandEvent event(wxEVT_BUTTON, m_slice_btn->GetId());
    event.SetEventObject(m_slice_btn);
    m_slice_btn->GetEventHandler()->ProcessEvent(event);
}

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

void MainFrame::init_ai_feature_host()
{
    if (!wxGetApp().is_editor())
        return;
    m_ai_feature_host = std::make_unique<AIDesktopFeatureHost>(m_tabpanel, m_plater, [this] {
        m_plater->exit_gizmo();
        m_plater->update(true, true);
        select_tab(TAB_ID_PREPARE);
        if (m_redesign_shell == nullptr || !m_redesign_shell->owns_model_workflow())
            m_plater->show_smart_slicing(true);
    }, [this] { register_ai_assistant(); });
    m_tabpanel->AddPage(TAB_ID_GENERATE_3D, m_ai_feature_host->model_generation_panel(), _L("3D 生成"),
                        "tab_generate_3d_active");
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
                this, m_ai_feature_host != nullptr ? m_ai_feature_host->model_generation_host() : nullptr,
                m_plater, m_ai_feature_host != nullptr ? m_ai_feature_host->text_feature_host() : nullptr);
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
        int anchor_idx = m_tabpanel->FindPageByName(TAB_ID_GENERATE_3D);
        if (anchor_idx == wxNOT_FOUND)
            anchor_idx = m_tabpanel->FindPageByName(TAB_ID_HOME);
        size_t prepare_pos = (anchor_idx == wxNOT_FOUND) ? 0 : static_cast<size_t>(anchor_idx) + 1;
#ifdef SLIC3R_CAD
        if (m_design_page != nullptr) {
            m_design_page->Reparent(m_tabpanel);
            m_tabpanel->InsertPage(prepare_pos++, TAB_ID_DESIGN, m_design_page, _L("Design"), "tab_design_active");
        }
#endif
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

void MainFrame::jump_to_monitor_hms()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (MonitorPanel* monitor = MonitorPanel::ensure()) monitor->jump_to_HMS();
}

void MainFrame::jump_to_monitor_upgrade()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (MonitorPanel* monitor = MonitorPanel::ensure()) monitor->jump_to_Upgrade();
}

void MainFrame::jump_to_monitor_live_view()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (MonitorPanel* monitor = MonitorPanel::ensure()) monitor->jump_to_LiveView();
}

void MainFrame::jump_to_monitor_rack()
{
    if (m_redesign_shell_active) { select_tab(TAB_ID_MONITOR); return; }
    jump_to_monitor();
    if (MonitorPanel* monitor = MonitorPanel::ensure()) monitor->jump_to_Rack();
}

void MainFrame::jump_to_monitor_playback(const std::string& dev_id)
{
    jump_to_monitor(dev_id);
    if (!m_redesign_shell_active)
        if (MonitorPanel* monitor = MonitorPanel::ensure()) monitor->jump_to_LiveView();
}

void MainFrame::select_monitor_status(const std::string& dev_id)
{
    select_device(dev_id);
    select_tab(TAB_ID_MONITOR);
    if (!m_redesign_shell_active)
        if (MonitorPanel* monitor = MonitorPanel::ensure())
            monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PT_STATUS);
}

void MainFrame::jump_to_monitor_media()
{
    if (m_redesign_shell_active) {
        if (m_redesign_shell != nullptr) m_redesign_shell->show_printer_media();
        return;
    }
    jump_to_monitor();
    if (MonitorPanel* monitor = MonitorPanel::ensure())
        monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PT_MEDIA);
}

void MainFrame::notify_hms_read(const wxString& error_code)
{
    MonitorPanel* monitor = MonitorPanel::if_built();
    if (m_redesign_shell_active || monitor == nullptr)
        return;
    wxCommandEvent event(EVT_ALREADY_READ_HMS);
    event.SetString(error_code);
    wxPostEvent(monitor, event);
}

void MainFrame::refresh_device_surface()
{
    if (m_redesign_shell_active) {
        if (m_redesign_shell != nullptr) m_redesign_shell->refresh_printer_state();
        return;
    }
    if (MonitorPanel* monitor = MonitorPanel::if_built()) {
        monitor->update_network_version_footer();
        monitor->set_default();
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
    if (MonitorPanel* monitor = MonitorPanel::if_built())
        monitor->select_machine(dev_id);
    else if (auto* manager = wxGetApp().getDeviceManager())
        manager->set_selected_machine(dev_id);
}

void MainFrame::update_monitor_error(MachineObject* obj)
{
    MonitorPanel* monitor = MonitorPanel::if_built();
    if (m_redesign_shell_active || monitor == nullptr)
        return;
    StatusPanel* status_panel = monitor->get_status_panel();
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
    if (MonitorPanel* monitor = MonitorPanel::if_built())
        monitor->Layout();
}

void MainFrame::notify_calibration_job_finished(int tab_index, const wxString& payload)
{
    select_tab(TAB_ID_CALIBRATION);
    if (m_redesign_shell_active)
        return;
    CalibrationPanel* calibration = CalibrationPanel::ensure();
    if (calibration == nullptr)
        return;
    auto* page = static_cast<CalibrationWizard*>(calibration->get_tabpanel()->GetPage(tab_index));
    if (page == nullptr)
        return;
    wxCommandEvent event(EVT_CALIBRATION_JOB_FINISHED);
    event.SetString(payload);
    event.SetEventObject(page);
    wxPostEvent(page, event);
}

void MainFrame::update_print_error_info(int code, const std::string& message, const std::string& extra)
{
    CalibrationPanel* calibration = CalibrationPanel::if_built();
    if (m_redesign_shell_active || calibration == nullptr)
        return;
    calibration->update_print_error_info(code, message, extra);
}

void MainFrame::register_ai_assistant()
{
    if (m_plater != nullptr && m_view_menu != nullptr) {
        m_plater->enable_ai_assistant();
        append_menu_check_item(
            m_view_menu, wxID_ANY, _L("高级参数助手"), _L("打开高级参数问答工具。"),
            [this](wxCommandEvent&) { m_plater->show_ai_assistant(!m_plater->is_ai_assistant_shown()); }, this,
            [this]() { return is_prepare_or_preview_tab(); },
            [this]() { return m_plater->is_ai_assistant_shown(); }, this);
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
    if (m_redesign_shell && m_redesign_shell->native_slicing_visible() && tab_id != TAB_ID_MONITOR)
        return false;
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
        else if (panel == m_monitor_page || panel == m_printer_view_page ||
                 panel == MonitorPanel::if_built() || panel == PrinterWebView::if_built())
            route = TAB_ID_MONITOR;
        else if (panel == m_multi_machine_page || panel == MultiMachinePage::if_built())
            route = TAB_ID_MULTI_DEVICE;
        else if (panel == m_calibration_page || panel == CalibrationPanel::if_built())
            route = TAB_ID_CALIBRATION;
        else if (panel == m_project_page || panel == ProjectPanel::if_built())
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

MainFrame::~MainFrame()
{
    // Member-owned hosts are destroyed before wxWidgets destroys child windows.
    if (m_redesign_shell != nullptr)
        m_redesign_shell->disconnect_model_generation_host();
}
