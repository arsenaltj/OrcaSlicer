    std::unique_ptr<AIDesktopFeatureHost> m_ai_feature_host;
    void bind_workspace_window_events();
#ifdef __WXMSW__
    bool MSWPrintChild(WXHDC dc, wxWindow* child) override;
#endif
    RedesignShell*        m_redesign_shell { nullptr };
    bool                  m_redesign_shell_requested { true };
    bool                  m_redesign_shell_active { false };

    wxBoxSizer* m_native_side_tools {nullptr};
    wxWindow* m_native_side_tools_parent {nullptr};
