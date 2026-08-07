#include "ModelGenerationPanel.hpp"

#include "AISidecarClient.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

#include <algorithm>
#include <regex>
#include <utility>

namespace Slic3r::GUI {
namespace {

constexpr int POLL_TIMER_ID = wxID_HIGHEST + 913;
constexpr size_t MAX_IMAGE_SIZE = 20 * 1024 * 1024;

std::string new_request_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

bool is_supported_image(const boost::filesystem::path& path)
{
    if (!boost::filesystem::is_regular_file(path))
        return false;
    const auto size = boost::filesystem::file_size(path);
    if (size == 0 || size > MAX_IMAGE_SIZE)
        return false;
    boost::filesystem::ifstream stream(path, std::ios::binary);
    unsigned char magic[8] {};
    stream.read(reinterpret_cast<char*>(magic), sizeof(magic));
    const auto count = stream.gcount();
    const bool png = count >= 8 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G' &&
                     magic[4] == 0x0d && magic[5] == 0x0a && magic[6] == 0x1a && magic[7] == 0x0a;
    const bool jpeg = count >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff;
    return png || jpeg;
}

boost::filesystem::path temp_path(const std::string& job_id, const std::string& extension)
{
    return boost::filesystem::temp_directory_path() / ("orcaslicer-ai-" + job_id + "." + extension);
}

wxStaticText* section_label(wxWindow* parent, const wxString& text)
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(wxColour(40, 55, 58));
    return label;
}

} // namespace

ModelGenerationPanel::ModelGenerationPanel(wxWindow* parent, Plater* plater, ImportSucceededFn on_import_succeeded)
    : wxPanel(parent)
    , m_plater(plater)
    , m_on_import_succeeded(std::move(on_import_succeeded))
    , m_client(AISidecarClient::default_endpoint())
    , m_poll_timer(this, POLL_TIMER_ID)
{
    SetBackgroundColour(*wxWHITE);
    build_page();
    Bind(wxEVT_TIMER, &ModelGenerationPanel::on_poll, this, POLL_TIMER_ID);
    m_status->SetLabel(_L("Checking the local 3D generation service..."));
    m_result_summary->SetLabel(_L("3D generation is unavailable until the local service is ready."));
    refresh_controls();
}

ModelGenerationPanel::~ModelGenerationPanel()
{
    shutdown();
}

void ModelGenerationPanel::set_service_availability(bool available, const std::string& message)
{
    if (m_shutdown)
        return;
    m_service_available = available;
    if (available && !m_busy) {
        m_status->SetLabel(_L("Local 3D generation service is ready."));
        m_result_summary->SetLabel(_L("Describe an object to begin 3D generation."));
        update_workflow();
    } else if (!m_busy) {
        m_status->SetLabel(message.empty() ? _L("Configure and start the local AI service to enable 3D generation.") : wxString::FromUTF8(message));
        m_result_summary->SetLabel(_L("3D generation is unavailable."));
    }
    refresh_controls();
}

void ModelGenerationPanel::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    ++m_sequence;
    m_poll_timer.Stop();
    m_client.cancel_current();
    cleanup_files();
}

void ModelGenerationPanel::build_page()
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxPanel(this);
    header->SetBackgroundColour(wxColour(246, 249, 249));
    auto* header_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(header, wxID_ANY, _L("3D Generate"));
    wxFont title_font = title->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 5);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    title->SetForegroundColour(wxColour(31, 55, 59));
    header_sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(24));
    header_sizer->Add(new wxStaticText(header, wxID_ANY, _L("Turn a description or reference image into a model you can review and import onto the current plate.")),
                      0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(24));
    header->SetSizer(header_sizer);
    root->Add(header, 0, wxEXPAND);

    auto* content = new wxBoxSizer(wxHORIZONTAL);
    content->Add(build_workflow_panel(this), 0, wxEXPAND | wxALL, FromDIP(18));

    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(build_preview_panel(this), 3, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(18));
    right->Add(build_model_library(this), 1, wxEXPAND | wxRIGHT | wxBOTTOM, FromDIP(18));
    content->Add(right, 1, wxEXPAND);
    root->Add(content, 1, wxEXPAND);
    SetSizer(root);
}

wxWindow* ModelGenerationPanel::build_workflow_panel(wxWindow* parent)
{
    auto* scroll = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(420), -1), wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetMinSize(wxSize(FromDIP(390), -1));
    scroll->SetBackgroundColour(wxColour(250, 251, 251));
    scroll->SetScrollRate(0, FromDIP(12));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_workflow_steps = new wxStaticText(scroll, wxID_ANY, _L("Input  →  Prepare  →  Review  →  Generate  →  Import"));
    m_workflow_steps->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_workflow_steps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    m_workflow_phase = new wxStaticText(scroll, wxID_ANY, _L("Checking local service"));
    wxFont workflow_font = m_workflow_phase->GetFont();
    workflow_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_workflow_phase->SetFont(workflow_font);
    sizer->Add(m_workflow_phase, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));

    sizer->Add(section_label(scroll, _L("1. Input")), 0, wxEXPAND | wxALL, FromDIP(16));
    wxArrayString modes;
    modes.Add(_L("Text to 3D"));
    modes.Add(_L("Image + Text to 3D"));
    m_mode = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, modes);
    m_mode->SetSelection(0);
    sizer->Add(m_mode, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("Describe the printable object"));
    sizer->Add(m_prompt_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(100)), wxTE_MULTILINE);
    sizer->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    auto* image_row = new wxBoxSizer(wxHORIZONTAL);
    m_choose_image = new wxButton(scroll, wxID_ANY, _L("Choose image..."));
    m_selected_image = new wxStaticText(scroll, wxID_ANY, _L("No image selected"));
    image_row->Add(m_choose_image, 0, wxRIGHT, FromDIP(8));
    image_row->Add(m_selected_image, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(image_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_upload_notice = new wxStaticText(scroll, wxID_ANY, _L("Only the selected PNG/JPEG and your instruction are sent for AI image preparation. Project meshes, G-code, credentials, and local paths are not sent."));
    m_upload_notice->Wrap(FromDIP(360));
    m_upload_notice->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_upload_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    sizer->Add(section_label(scroll, _L("2. AI preprocessing")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(16));
    m_preprocess = new wxButton(scroll, wxID_ANY, _L("Preprocess"));
    sizer->Add(m_preprocess, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_prepared_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("Prepared 3D prompt"));
    sizer->Add(m_prepared_prompt_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_prepared_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(95)), wxTE_MULTILINE);
    sizer->Add(m_prepared_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    sizer->Add(section_label(scroll, _L("3. Generate and import")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(16));
    auto* generation_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_generate = new wxButton(scroll, wxID_ANY, _L("Generate 3D model"));
    m_stop = new wxButton(scroll, wxID_ANY, _L("Stop"));
    generation_buttons->Add(m_generate, 0, wxRIGHT, FromDIP(8));
    generation_buttons->Add(m_stop, 0);
    sizer->Add(generation_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_generation_progress = new wxGauge(scroll, wxID_ANY, 100);
    m_generation_progress->SetValue(0);
    m_status = new wxStaticText(scroll, wxID_ANY, _L("Idle"));
    m_status->Wrap(FromDIP(360));
    sizer->Add(m_generation_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    auto* result_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_import = new wxButton(scroll, wxID_ANY, _L("Import to current plate"));
    m_discard = new wxButton(scroll, wxID_ANY, _L("Discard"));
    result_buttons->Add(m_import, 0, wxRIGHT, FromDIP(8));
    result_buttons->Add(m_discard, 0);
    sizer->Add(result_buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    auto* format_note = new wxStaticText(scroll, wxID_ANY, _L("3MF is preferred. STL fallback contains geometry only and does not preserve texture or color."));
    format_note->Wrap(FromDIP(360));
    format_note->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(format_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    scroll->SetSizer(sizer);
    scroll->FitInside();

    m_mode->Bind(wxEVT_CHOICE, &ModelGenerationPanel::on_mode_changed, this);
    m_prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    m_choose_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_choose_image, this);
    m_preprocess->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_preprocess, this);
    m_generate->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_generate, this);
    m_stop->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_stop, this);
    m_import->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_import, this);
    m_discard->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_discard, this);
    return scroll;
}

wxWindow* ModelGenerationPanel::build_preview_panel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(section_label(panel, _L("Preview and result")), 0, wxEXPAND | wxALL, FromDIP(18));

    m_preview_area = new wxPanel(panel);
    m_preview_area->SetBackgroundColour(wxColour(241, 244, 245));
    m_preview_area->SetMinSize(wxSize(FromDIP(480), FromDIP(330)));
    auto* preview_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview = new wxStaticBitmap(m_preview_area, wxID_ANY, wxNullBitmap);
    m_preview_message = new wxStaticText(m_preview_area, wxID_ANY, _L("Start with a description or reference image."));
    m_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    preview_sizer->AddStretchSpacer();
    preview_sizer->Add(m_preview, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(20));
    preview_sizer->Add(m_preview_message, 0, wxALIGN_CENTER | wxALL, FromDIP(14));
    preview_sizer->AddStretchSpacer();
    m_preview_area->SetSizer(preview_sizer);
    m_preview_area->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        rescale_preview_to_fit();
        event.Skip();
    });
    sizer->Add(m_preview_area, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_result_summary = new wxStaticText(panel, wxID_ANY, _L("No generated model yet."));
    m_result_summary->Wrap(FromDIP(520));
    sizer->Add(m_result_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    panel->SetSizer(sizer);
    return panel;
}

wxWindow* ModelGenerationPanel::build_model_library(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(section_label(panel, _L("Model Library")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    auto* session = new wxStaticText(panel, wxID_ANY, _L("This session"));
    session->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(session, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_empty = new wxStaticText(panel, wxID_ANY, _L("Models you import in this session appear here. No copies are saved."));
    m_library_empty->SetForegroundColour(wxColour(110, 122, 125));
    sizer->Add(m_library_empty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_scroller = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 180)), wxVSCROLL);
    m_library_scroller->SetScrollRate(0, FromDIP(8));
    m_library_sizer = new wxBoxSizer(wxVERTICAL);
    m_library_scroller->SetSizer(m_library_sizer);
    sizer->Add(m_library_scroller, 1, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(sizer);
    refresh_library();
    return panel;
}

void ModelGenerationPanel::on_mode_changed(wxCommandEvent&)
{
    reset(true);
    m_prompt->Clear();
    m_selected_image_path.clear();
    m_selected_image->SetLabel(_L("No image selected"));
    refresh_controls();
}

void ModelGenerationPanel::on_choose_image(wxCommandEvent&)
{
    wxFileDialog dialog(this, _L("Choose a reference image"), wxEmptyString, wxEmptyString,
                        _L("PNG and JPEG images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    boost::filesystem::path path(dialog.GetPath().ToStdWstring());
    if (!is_supported_image(path)) {
        MessageDialog error(this, _L("Choose a non-empty PNG or JPEG image no larger than 20 MB."), wxEmptyString, wxOK | wxICON_ERROR);
        error.ShowModal();
        return;
    }
    m_selected_image_path = std::move(path);
    const size_t bytes = boost::filesystem::file_size(m_selected_image_path);
    m_selected_image->SetLabel(wxString::FromUTF8(m_selected_image_path.filename().string()) +
                               wxString::Format(" (%llu KB)", static_cast<unsigned long long>((bytes + 1023) / 1024)));
    refresh_controls();
}

void ModelGenerationPanel::on_preprocess(wxCommandEvent&)
{
    const std::string prompt = m_prompt->GetValue().ToUTF8().data();
    if (prompt.empty()) {
        MessageDialog dlg(this, _L("Describe the object before preprocessing."), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    const bool image_mode = m_mode->GetSelection() == 1;
    if (image_mode) {
        static const std::regex absolute_path(R"(^\s*(?:[A-Za-z]:[\\/]|/).*)");
        if (std::regex_match(prompt, absolute_path)) {
            MessageDialog dlg(this, _L("Describe how AI should redraw the selected image. Do not paste a local file path into the instruction."), wxEmptyString, wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
            return;
        }
        if (m_selected_image_path.empty()) {
            MessageDialog dlg(this, _L("Choose an image before preprocessing."), wxEmptyString, wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
            return;
        }
        wxString message;
        message << _L("Upload this image for AI preparation?\n\n")
                << wxString::FromUTF8(m_selected_image_path.filename().string()) << "\n"
                << _L("Only this image and the instruction are sent.");
        MessageDialog confirm(this, message, _L("Confirm image upload"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
    }

    reset(true);
    m_busy = true;
    const uint64_t sequence = ++m_sequence;
    m_generation_progress->SetValue(15);
    update_workflow();
    m_status->SetLabel(_L("Preparing AI input..."));
    m_result_summary->SetLabel(_L("Preparing a 3D-generation input."));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak)
                weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak)
                weak->handle_error(error, sequence);
        });
    };
    if (image_mode)
        m_client.preprocess_image(new_request_id(), prompt, m_selected_image_path, std::move(success), std::move(failure));
    else
        m_client.preprocess_text(new_request_id(), prompt, std::move(success), std::move(failure));
}

void ModelGenerationPanel::on_generate(wxCommandEvent&)
{
    if (!m_awaiting_confirmation || m_job_id.empty())
        return;
    MessageDialog confirm(this, _L("Generate the reviewed 3D input? This may consume API credits."), _L("Confirm 3D generation"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    m_busy = true;
    m_awaiting_confirmation = false;
    const uint64_t sequence = m_sequence;
    m_status->SetLabel(_L("Submitting 3D generation request..."));
    refresh_controls();
    const std::string prepared = m_mode->GetSelection() == 0 ? m_prepared_prompt->GetValue().ToUTF8().data() : std::string();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.generate(m_job_id, prepared,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_stop(wxCommandEvent&)
{
    if (m_job_id.empty())
        return;
    m_poll_timer.Stop();
    m_client.cancel_current();
    m_status->SetLabel(_L("Stopping locally. A submitted generation task may continue remotely."));
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.stop(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_import(wxCommandEvent&) { download_and_import(); }
void ModelGenerationPanel::on_discard(wxCommandEvent&) { reset(true); }
void ModelGenerationPanel::on_poll(wxTimerEvent&) { schedule_poll(); }

void ModelGenerationPanel::handle_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_poll_timer.Stop();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = false;
    m_artifact_format.clear();
    m_generation_progress->SetValue(0);
    update_workflow();
    wxString message = wxString::FromUTF8(error);
    if (message.Contains("not reachable") || message.Contains("Couldn't connect") || message.Contains("Failed to connect") || message.Contains("Connection refused"))
        message = _L("The local AI service is not reachable. Check its configuration and try again.");
    m_status->SetLabel(message);
    m_result_summary->SetLabel(_L("Generation stopped before a model was ready."));
    refresh_controls();
}

void ModelGenerationPanel::handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_job_id = status.id;
    m_status->SetLabel(wxString::FromUTF8(status.message));
    m_busy = status.state == "preprocessing" || status.state == "queued" || status.state == "running" || status.state == "stopping";
    m_awaiting_confirmation = status.state == "awaiting_confirmation";
    m_ready = status.state == "ready" && status.artifact_ready;
    m_artifact_format = status.artifact_format;
    m_artifact_color_encoding = status.artifact_color_encoding;
    if (!status.prepared_prompt.empty())
        m_prepared_prompt->SetValue(wxString::FromUTF8(status.prepared_prompt));
    if (status.preview_ready && m_preview_path.empty())
        download_preview(sequence);
    if (m_ready) {
        wxString summary;
        summary << _L("Model ready for import") << " · " << wxString::FromUTF8(m_artifact_format);
        if (status.artifact_size > 0)
            summary << wxString::Format(" · %.1f MB", double(status.artifact_size) / (1024.0 * 1024.0));
        m_result_summary->SetLabel(summary);
    } else if (m_awaiting_confirmation) {
        m_result_summary->SetLabel(_L("Review the prepared input before starting 3D generation."));
    } else {
        m_result_summary->SetLabel(wxString::FromUTF8(status.message));
    }
    int workflow_progress = std::clamp(status.progress, 0, 80);
    if (status.state == "awaiting_confirmation")
        workflow_progress = 15;
    else if (status.state == "ready")
        workflow_progress = 94;
    m_generation_progress->SetValue(workflow_progress);
    update_workflow(&status);
    if (m_busy)
        m_poll_timer.StartOnce(1500);
    refresh_controls();
}

void ModelGenerationPanel::schedule_poll()
{
    if (m_shutdown || m_job_id.empty() || !m_busy)
        return;
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::download_preview(uint64_t sequence)
{
    m_preview_path = temp_path(m_job_id, "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_preview(m_job_id, m_preview_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                wxImage image(path.wstring());
                if (!image.IsOk()) {
                    weak->m_preview_path.clear();
                    return;
                }
                weak->m_preview_image = image;
                weak->m_preview_message->SetLabel(_L("Prepared reference image — review it before generating the model."));
                weak->rescale_preview_to_fit();
                weak->Layout();
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak && sequence == weak->m_sequence) {
                    weak->m_preview_path.clear();
                    weak->m_preview_message->SetLabel(wxString::FromUTF8(error));
                }
            });
        });
}

void ModelGenerationPanel::download_and_import()
{
    if (!m_ready || m_job_id.empty())
        return;
    if (m_artifact_format == "obj" && m_artifact_color_encoding != "vertex_colors") {
        m_status->SetLabel(_L("Generated OBJ does not contain supported vertex colors."));
        return;
    }
    if (m_artifact_format != "obj" && m_artifact_format != "3mf" && m_artifact_format != "stl") {
        m_status->SetLabel(_L("Unsupported generated model format."));
        return;
    }
    m_artifact_path = temp_path(m_job_id, m_artifact_format);
    m_busy = true;
    m_generation_progress->SetValue(94);
    const uint64_t sequence = m_sequence;
    m_status->SetLabel(_L("Downloading generated model from the local sidecar..."));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_busy = false;
                MessageDialog confirm(weak.get(), _L("Import this generated model onto the current plate?\n\nThe import creates an undoable action."), _L("Import generated model"), wxYES_NO | wxICON_QUESTION);
                if (confirm.ShowModal() != wxID_YES) {
                    weak->cleanup_files();
                    weak->refresh_controls();
                    return;
                }
                const size_t before = weak->m_plater->model().objects.size();
                weak->m_generation_progress->SetValue(97);
                weak->m_status->SetLabel(_L("Importing generated model..."));
                weak->m_plater->add_model(false, path.string());
                if (weak->m_plater->model().objects.size() <= before) {
                    weak->m_status->SetLabel(_L("The generated model could not be imported."));
                    weak->cleanup_files();
                    weak->refresh_controls();
                    return;
                }
                const size_t artifact_size = boost::filesystem::file_size(path);
                weak->add_library_entry(artifact_size);
                const std::string job_id = weak->m_job_id;
                weak->cleanup_files();
                weak->m_client.remove(job_id, [] {}, [](std::string) {});
                weak->m_poll_timer.Stop();
                weak->m_job_id.clear();
                weak->m_artifact_format.clear();
                weak->m_busy = false;
                weak->m_awaiting_confirmation = false;
                weak->m_ready = false;
                weak->m_generation_progress->SetValue(100);
                weak->update_workflow();
                weak->m_prepared_prompt->Clear();
                weak->m_status->SetLabel(_L("Generated model imported."));
                weak->m_result_summary->SetLabel(_L("The model was added to the current plate."));
                weak->refresh_controls();
                if (weak->m_on_import_succeeded)
                    weak->m_on_import_succeeded();
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak) return;
                weak->cleanup_files();
                weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::refresh_controls()
{
    if (m_shutdown)
        return;
    const bool image_mode = m_mode->GetSelection() == 1;
    m_prompt_label->SetLabel(image_mode ? _L("Describe how AI should redraw the selected image") : _L("Describe the printable object"));
    m_choose_image->Show(image_mode);
    m_selected_image->Show(image_mode);
    m_upload_notice->Show(image_mode);
    m_prepared_prompt_label->Show(!image_mode);
    m_prepared_prompt->Show(!image_mode);
    const bool valid_input = !m_prompt->GetValue().empty() && (!image_mode || !m_selected_image_path.empty());
    m_mode->Enable(!m_busy);
    m_prompt->Enable(!m_busy);
    m_choose_image->Enable(!m_busy);
    m_preprocess->Enable(m_service_available && !m_busy && valid_input);
    m_prepared_prompt->Enable(m_service_available && !m_busy && m_awaiting_confirmation && !image_mode);
    m_generate->Enable(m_service_available && !m_busy && m_awaiting_confirmation);
    m_stop->Enable(m_service_available && m_busy && !m_job_id.empty());
    m_import->Enable(m_service_available && !m_busy && m_ready);
    m_discard->Enable(m_service_available && !m_busy && !m_job_id.empty());
    Layout();
}

void ModelGenerationPanel::reset(bool remove_remote)
{
    m_poll_timer.Stop();
    m_client.cancel_current();
    const std::string old_job = m_job_id;
    ++m_sequence;
    cleanup_files();
    m_job_id.clear();
    m_artifact_format.clear();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = false;
    m_generation_progress->SetValue(0);
    update_workflow();
    m_status->SetLabel(_L("Idle"));
    m_prepared_prompt->Clear();
    set_preview_empty(_L("Start with a description or reference image."));
    m_result_summary->SetLabel(_L("No generated model yet."));
    if (remove_remote && !old_job.empty())
        m_client.remove(old_job, [] {}, [](std::string) {});
    refresh_controls();
}

void ModelGenerationPanel::cleanup_files()
{
    boost::system::error_code ec;
    if (!m_preview_path.empty())
        boost::filesystem::remove(m_preview_path, ec);
    if (!m_artifact_path.empty())
        boost::filesystem::remove(m_artifact_path, ec);
    m_preview_path.clear();
    m_artifact_path.clear();
}

void ModelGenerationPanel::add_library_entry(size_t artifact_size)
{
    GeneratedModelEntry entry;
    entry.title = _L("Generated ") + wxString::FromUTF8(m_artifact_format.empty() ? "model" : m_artifact_format);
    entry.details = _L("Imported this session") + " · " + wxString::FromUTF8(m_mode->GetSelection() == 1 ? "Image + text" : "Text") +
                    wxString::Format(" · %.1f MB", double(artifact_size) / (1024.0 * 1024.0));
    if (m_preview_image.IsOk()) {
        wxImage thumbnail = m_preview_image;
        thumbnail.Rescale(FromDIP(96), FromDIP(96), wxIMAGE_QUALITY_HIGH);
        entry.preview = wxBitmap(thumbnail);
    }
    m_library_entries.push_back(std::move(entry));
    if (m_library_entries.size() > 12)
        m_library_entries.erase(m_library_entries.begin());
    refresh_library();
}

void ModelGenerationPanel::refresh_library()
{
    if (m_library_sizer == nullptr || m_library_scroller == nullptr)
        return;
    m_library_sizer->Clear(true);
    m_library_empty->Show(m_library_entries.empty());
    for (const GeneratedModelEntry& entry : m_library_entries) {
        auto* card = new wxPanel(m_library_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        if (entry.preview.IsOk()) {
            auto* bitmap = new wxStaticBitmap(card, wxID_ANY, entry.preview);
            row->Add(bitmap, 0, wxALL, FromDIP(8));
        } else {
            auto* format = new wxStaticText(card, wxID_ANY, entry.title);
            format->SetForegroundColour(wxColour(31, 122, 116));
            row->Add(format, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
        }
        auto* text = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(card, wxID_ANY, entry.title);
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        text->Add(title, 0, wxBOTTOM, FromDIP(3));
        auto* details = new wxStaticText(card, wxID_ANY, entry.details);
        details->SetForegroundColour(wxColour(91, 104, 107));
        text->Add(details, 0);
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetSizer(row);
        m_library_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_library_scroller->FitInside();
    m_library_scroller->Layout();
}

void ModelGenerationPanel::rescale_preview_to_fit()
{
    if (!m_preview_image.IsOk() || m_preview_area == nullptr)
        return;
    const wxSize available = m_preview_area->GetClientSize();
    const int max_width = std::max(1, available.GetWidth() - FromDIP(40));
    const int max_height = std::max(1, available.GetHeight() - FromDIP(72));
    const double scale = std::min(double(max_width) / m_preview_image.GetWidth(), double(max_height) / m_preview_image.GetHeight());
    const int width = std::max(1, int(m_preview_image.GetWidth() * std::min(1.0, scale)));
    const int height = std::max(1, int(m_preview_image.GetHeight() * std::min(1.0, scale)));
    wxImage image = m_preview_image;
    if (width != image.GetWidth() || height != image.GetHeight())
        image.Rescale(width, height, wxIMAGE_QUALITY_HIGH);
    m_preview->SetBitmap(wxBitmap(image));
    m_preview_area->Layout();
}

void ModelGenerationPanel::update_workflow(const AIModelGenerationClient::JobStatus* status)
{
    wxString phase = _L("Input");
    wxString steps = _L("Input  →  Prepare  →  Review  →  Generate  →  Import");
    if (status != nullptr) {
        if (status->state == "preprocessing")
            phase = _L("Prepare input");
        else if (status->state == "awaiting_confirmation")
            phase = _L("Review prepared input");
        else if (status->phase == "generating")
            phase = _L("Generate model");
        else if (status->phase == "converting" || status->phase == "downloading_artifact")
            phase = _L("Finalize model");
        else if (status->state == "ready")
            phase = _L("Import model");
        else if (status->state == "stopping")
            phase = _L("Stopping generation");
    }
    m_workflow_phase->SetLabel(phase);
    m_workflow_steps->SetLabel(steps);
}

void ModelGenerationPanel::set_preview_empty(const wxString& message)
{
    m_preview_image = wxImage();
    m_preview->SetBitmap(wxNullBitmap);
    m_preview_message->SetLabel(message);
}

} // namespace Slic3r::GUI
