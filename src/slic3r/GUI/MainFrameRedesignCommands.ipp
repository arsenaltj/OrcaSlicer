    // During migration, legacy commands must not operate hidden Plater or
    // notebook controls after the redesign shell becomes the visible owner.
    bool route_legacy_command_to_redesign(const char *command,
                                          const wxString &tab_id = wxString());
    void bind_workspace_shortcuts();
    void init_ai_feature_host();
    void start_workflow_services();
