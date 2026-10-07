// Included once by MainFrame.cpp inside Slic3r::GUI; desktop AI composition.
template<typename Trace>
void MainFrame::initialize_ai_features(Trace&& trace_stage)
{
    m_ai_feature_host = std::make_unique<AIDesktopFeatureHost>(m_tabpanel, m_plater, [this] {
        m_plater->exit_gizmo();
        m_plater->update(true, true);
        select_tab(TAB_ID_PREPARE);
        // AI imports and color matching return to the native prepare page.
        // Keep the post-generation preparation controls visible so the user
        // can add a detached base immediately, without discovering the
        // Smart Slicing pane through the View menu first.
        m_plater->show_smart_slicing(true);
    }, [this] { register_ai_assistant(); });
    trace_stage();
    m_tabpanel->AddPage(TAB_ID_GENERATE_3D, m_ai_feature_host->model_generation_panel(), _L("3D 生成"),
                        "tab_generate_3d_active");
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
