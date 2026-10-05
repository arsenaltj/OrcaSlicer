#pragma once
#include <wx/panel.h>
#include <wx/timer.h>
#include <vector>
#include "AIWindowAppearance.hpp"
class Notebook;
class Button;
class wxStaticText;
class wxStaticBitmap;
class wxScrolledWindow;
namespace Slic3r::GUI {
class AIDesktopFeatureHost;
// Main-window navigation only. Generation state and native pages keep their owners.
class DesktopWorkspaceNavigation final : public wxPanel, public AIThemeOwner {
public:
    DesktopWorkspaceNavigation(wxWindow* parent, Notebook* book, AIDesktopFeatureHost& host);
    void refresh();
    void apply_ai_theme(bool fonts) override;
private:
    void navigate_workspace_page(const wxString& id);
    void show_workspace_pages();
    void update_compact_layout();
    void build_print_center();
    void refresh_print_center();
    Notebook* m_tabpanel;
    AIDesktopFeatureHost* m_ai_feature_host;
    wxWindow* m_body {nullptr};
    wxWindow* m_print_center {nullptr};
    wxStaticText* m_print_status {nullptr};
    wxStaticText* m_print_preset {nullptr};
    wxStaticText* m_print_connection {nullptr};
    wxStaticText* m_print_job {nullptr};
    wxStaticText* m_print_action_hint {nullptr};
    wxWindow* m_print_sidebar {nullptr};
    wxScrolledWindow* m_print_details {nullptr};
    wxStaticBitmap* m_print_preview {nullptr};
    Button* m_print_review {nullptr};
    wxTimer m_print_refresh;
    const void* m_print_preview_result {nullptr};
    unsigned int m_print_preview_result_id {0};
    std::vector<Button*> m_workspace_buttons;
    bool m_compact {false};
};
}
