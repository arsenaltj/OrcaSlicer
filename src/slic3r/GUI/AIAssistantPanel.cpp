#include "AIAssistantPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

#include <map>
#include <utility>

namespace Slic3r::GUI {
namespace {

wxString change_label(const AIAssistantConfig::ValidatedChange& change)
{
    wxString label;
    label << wxString::FromUTF8(change.scope.c_str())
          << " / " << wxString::FromUTF8(change.key.c_str())
          << ": " << wxString::FromUTF8(change.old_value.c_str())
          << " -> " << wxString::FromUTF8(change.new_value.c_str());
    if (!change.reason.empty())
        label << "  " << wxString::FromUTF8(change.reason.c_str());
    return label;
}

std::string new_request_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

} // namespace

AIAssistantPanel::AIAssistantPanel(wxWindow* parent, Plater* plater)
    : wxPanel(parent)
    , m_plater(plater)
    , m_client(AISidecarClient::default_endpoint())
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("AI Assistant")), 0, wxEXPAND | wxALL, 8);

    m_prompt = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(80)), wxTE_MULTILINE | wxTE_PROCESS_ENTER);
    sizer->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_ask = new wxButton(this, wxID_ANY, _L("Ask"));
    m_cancel = new wxButton(this, wxID_ANY, _L("Cancel"));
    buttons->Add(m_ask, 0, wxRIGHT, 6);
    buttons->Add(m_cancel, 0);
    sizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    m_status = new wxStaticText(this, wxID_ANY, _L("Idle"));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Assistant message")), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    m_assistant_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(70)), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(m_assistant_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Accepted changes")), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    m_accepted = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(130)));
    sizer->Add(m_accepted, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Rejected changes")), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
    m_rejected = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(80)), wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(m_rejected, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto* apply_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_apply = new wxButton(this, wxID_ANY, _L("Apply selected"));
    m_discard = new wxButton(this, wxID_ANY, _L("Discard"));
    apply_buttons->Add(m_apply, 0, wxRIGHT, 6);
    apply_buttons->Add(m_discard, 0);
    sizer->Add(apply_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    SetSizer(sizer);

    m_ask->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_ask, this);
    m_cancel->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_cancel, this);
    m_apply->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_apply, this);
    m_discard->Bind(wxEVT_BUTTON, &AIAssistantPanel::on_discard, this);
    m_accepted->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent&) { refresh_button_state(); });

    clear_proposal();
    set_busy(false);
}

AIAssistantPanel::~AIAssistantPanel()
{
    m_client.cancel_current();
}

void AIAssistantPanel::on_ask(wxCommandEvent&)
{
    const std::string prompt = m_prompt->GetValue().ToUTF8().data();
    if (prompt.empty()) {
        MessageDialog dlg(this, _L("Please enter what you want to improve before asking the assistant."), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    clear_proposal();
    set_busy(true);
    set_status(_L("Contacting AI sidecar..."));
    auto request = AIAssistantConfig::build_context(*m_plater, prompt, new_request_id());
    wxWeakRef<AIAssistantPanel> weak(this);
    m_client.propose_config_changes(request,
        [weak](AISidecarClient::Response response) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, response = std::move(response)]() mutable {
                if (!weak) return;
                weak->set_status(_L("Validating proposal..."));
                weak->show_validation_result(response, AIAssistantConfig::validate_proposal(response.proposal));
                weak->set_busy(false);
            });
        },
        [weak](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, error = std::move(error)]() {
                if (!weak) return;
                weak->set_busy(false);
                weak->set_status(wxString::FromUTF8(error));
            });
        });
}

void AIAssistantPanel::on_cancel(wxCommandEvent&)
{
    m_client.cancel_current();
    set_busy(false);
    set_status(_L("Cancelled"));
}

void AIAssistantPanel::on_apply(wxCommandEvent&) { apply_selected_changes(); }
void AIAssistantPanel::on_discard(wxCommandEvent&) { clear_proposal(); set_status(_L("Discarded")); }

void AIAssistantPanel::set_busy(bool busy)
{
    m_busy = busy;
    m_ask->Enable(!busy);
    m_cancel->Enable(busy);
    m_prompt->Enable(!busy);
    refresh_button_state();
}

void AIAssistantPanel::set_status(const wxString& status) { m_status->SetLabel(status); }

void AIAssistantPanel::clear_proposal()
{
    m_changes.clear();
    m_assistant_text->Clear();
    m_accepted->Clear();
    m_rejected->Clear();
    refresh_button_state();
}

void AIAssistantPanel::show_validation_result(const AISidecarClient::Response& response, AIAssistantConfig::ValidationResult result)
{
    m_changes = std::move(result.accepted);
    m_assistant_text->SetValue(wxString::FromUTF8(response.assistant_text));
    m_accepted->Clear();
    for (const auto& change : m_changes) {
        const unsigned int index = m_accepted->Append(change_label(change));
        m_accepted->Check(index, true);
    }
    wxString rejected;
    for (const std::string& item : result.rejected)
        rejected += wxString::FromUTF8(item) + "\n";
    m_rejected->SetValue(rejected);
    if (!m_changes.empty())
        set_status(_L("Review the proposed changes before applying."));
    else if (!result.rejected.empty())
        set_status(_L("No valid changes were proposed."));
    else
        set_status(_L("The assistant did not propose any changes."));
    refresh_button_state();
}

void AIAssistantPanel::apply_selected_changes()
{
    std::map<Preset::Type, std::vector<AIAssistantConfig::ValidatedChange>> grouped_changes;
    for (unsigned int i = 0; i < m_changes.size(); ++i)
        if (m_accepted->IsChecked(i))
            grouped_changes[m_changes[i].preset_type].push_back(m_changes[i]);
    if (grouped_changes.empty()) {
        MessageDialog dlg(this, _L("Please select at least one change to apply."), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    for (const auto& [type, changes] : grouped_changes) {
        Tab* tab = wxGetApp().get_tab(type);
        if (tab == nullptr || tab->get_config() == nullptr)
            continue;
        DynamicPrintConfig patch(*tab->get_config());
        patch.apply(AIAssistantConfig::build_patch_config(changes), true);
        tab->load_config(patch);
    }
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->reslice();
    clear_proposal();
    set_status(_L("Applied selected changes."));
}

void AIAssistantPanel::refresh_button_state()
{
    bool has_checked_change = false;
    for (unsigned int i = 0; i < m_changes.size(); ++i)
        if (m_accepted->IsChecked(i)) {
            has_checked_change = true;
            break;
        }
    m_apply->Enable(!m_busy && has_checked_change);
    m_discard->Enable(!m_busy && (!m_changes.empty() || !m_rejected->GetValue().empty() || !m_assistant_text->GetValue().empty()));
}

} // namespace Slic3r::GUI
