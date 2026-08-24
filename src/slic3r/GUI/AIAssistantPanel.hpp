#ifndef slic3r_GUI_AIAssistantPanel_hpp_
#define slic3r_GUI_AIAssistantPanel_hpp_

#include "AIAssistantConfig.hpp"
#include "AISidecarClient.hpp"

#include <wx/panel.h>

#include <vector>

class wxButton;
class wxCheckListBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {

class Plater;

class AIAssistantPanel : public wxPanel
{
public:
    explicit AIAssistantPanel(wxWindow* parent, Plater* plater);
    ~AIAssistantPanel() override;

private:
    void on_ask(wxCommandEvent& event);
    void on_cancel(wxCommandEvent& event);
    void on_apply(wxCommandEvent& event);
    void on_discard(wxCommandEvent& event);

    void set_busy(bool busy);
    void set_status(const wxString& status);
    void clear_proposal();
    void show_validation_result(const AISidecarClient::Response& response, AIAssistantConfig::ValidationResult result);
    void apply_selected_changes();
    void refresh_button_state();

    Plater* m_plater { nullptr };
    AISidecarClient m_client;

    wxTextCtrl*     m_prompt { nullptr };
    wxButton*       m_ask { nullptr };
    wxButton*       m_cancel { nullptr };
    wxStaticText*   m_status { nullptr };
    wxTextCtrl*     m_assistant_text { nullptr };
    wxCheckListBox* m_accepted { nullptr };
    wxTextCtrl*     m_rejected { nullptr };
    wxButton*       m_apply { nullptr };
    wxButton*       m_discard { nullptr };

    std::vector<AIAssistantConfig::ValidatedChange> m_changes;
    bool m_busy { false };
};

} // namespace Slic3r::GUI

#endif // slic3r_GUI_AIAssistantPanel_hpp_
