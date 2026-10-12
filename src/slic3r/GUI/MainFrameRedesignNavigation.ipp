    bool is_redesign_shell_active() const { return m_redesign_shell_active; }
    void show_redesign_shell(bool show);
    wxString selected_tab_id() const;
    // Focus the active navigation surface without exposing the legacy notebook.
    void focus_workspace_navigation();
    void set_workspace_enabled(bool enabled);

    void        mount_native_workspace_tools(wxWindow* host);
    void        slice_native_workspace();
