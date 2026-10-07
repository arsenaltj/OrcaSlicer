#pragma once
#include <wx/panel.h>
#include <vector>
#include "AIWindowAppearance.hpp"
class Notebook;
class Button;
namespace Slic3r::GUI {
class AIDesktopFeatureHost;
class PrinterWorkspace;
// Main-window navigation only. Generation state and native pages keep their owners.
class DesktopWorkspaceNavigation final : public wxPanel, public AIThemeOwner {
public:
    DesktopWorkspaceNavigation(wxWindow* parent, Notebook* book, AIDesktopFeatureHost& host);
    void refresh();
    bool route_printer_page_request(const wxString& id);
    void apply_ai_theme(bool fonts) override;
private:
    void navigate_workspace_page(const wxString& id);
    void show_workspace_pages();
    void update_compact_layout();
    void build_print_center();
    Notebook* m_tabpanel;
    AIDesktopFeatureHost* m_ai_feature_host;
    wxWindow* m_body {nullptr};
    wxWindow* m_print_center {nullptr};
    PrinterWorkspace* m_printer_workspace {nullptr};
    std::vector<Button*> m_workspace_buttons;
    bool m_compact {false};
};
}
