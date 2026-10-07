// Included once by MainFrame.cpp inside Slic3r::GUI; desktop AI composition.
template<typename Trace>
static std::unique_ptr<AIDesktopFeatureHost> create_ai_feature_host(
    Notebook* tabs, Plater* plater, std::function<void()> select_prepare,
    std::function<void()> register_assistant, Trace&& trace_stage)
{
    auto host = std::make_unique<AIDesktopFeatureHost>(tabs, plater,
        std::move(select_prepare), std::move(register_assistant));
    trace_stage();
    tabs->AddPage(TAB_ID_GENERATE_3D, host->model_generation_panel(), _L("3D 生成"),
                        "tab_generate_3d_active");
    return host;
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
