#pragma once

#include "AIModelGenerationClient.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class wxButton;
class wxChoice;
class wxGauge;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {

class Plater;

class ModelGenerationPanel : public wxPanel
{
public:
    using ImportSucceededFn = std::function<void()>;

    ModelGenerationPanel(wxWindow* parent, Plater* plater, ImportSucceededFn on_import_succeeded);
    ~ModelGenerationPanel() override;

    void shutdown();
    void set_service_availability(bool available, const std::string& message = {});

private:
    void build_page();
    wxWindow* build_workflow_panel(wxWindow* parent);
    wxWindow* build_preview_panel(wxWindow* parent);
    wxWindow* build_model_library(wxWindow* parent);

    void on_mode_changed(wxCommandEvent& event);
    void on_choose_image(wxCommandEvent& event);
    void on_preprocess(wxCommandEvent& event);
    void on_generate(wxCommandEvent& event);
    void on_stop(wxCommandEvent& event);
    void on_import(wxCommandEvent& event);
    void on_discard(wxCommandEvent& event);
    void on_poll(wxTimerEvent& event);

    void handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence);
    void handle_error(const std::string& error, uint64_t sequence);
    void schedule_poll();
    void refresh_controls();
    void reset(bool remove_remote);
    void download_preview(uint64_t sequence);
    void download_and_import();
    void cleanup_files();
    void set_preview_empty(const wxString& message);
    void rescale_preview_to_fit();
    void update_workflow(const AIModelGenerationClient::JobStatus* status = nullptr);
    void add_library_entry(size_t artifact_size);
    void refresh_library();

    struct GeneratedModelEntry
    {
        wxString title;
        wxString details;
        wxBitmap preview;
    };

    Plater* m_plater { nullptr };
    ImportSucceededFn m_on_import_succeeded;
    AIModelGenerationClient m_client;

    wxChoice*       m_mode { nullptr };
    wxStaticText*   m_prompt_label { nullptr };
    wxTextCtrl*     m_prompt { nullptr };
    wxButton*       m_choose_image { nullptr };
    wxStaticText*   m_selected_image { nullptr };
    wxStaticText*   m_upload_notice { nullptr };
    wxButton*       m_preprocess { nullptr };
    wxStaticText*   m_prepared_prompt_label { nullptr };
    wxTextCtrl*     m_prepared_prompt { nullptr };
    wxStaticText*   m_workflow_steps { nullptr };
    wxStaticText*   m_workflow_phase { nullptr };
    wxStaticBitmap* m_preview { nullptr };
    wxStaticText*   m_preview_message { nullptr };
    wxStaticText*   m_result_summary { nullptr };
    wxButton*       m_generate { nullptr };
    wxButton*       m_stop { nullptr };
    wxGauge*        m_generation_progress { nullptr };
    wxStaticText*   m_status { nullptr };
    wxButton*       m_import { nullptr };
    wxButton*       m_discard { nullptr };
    wxPanel*        m_preview_area { nullptr };
    wxScrolledWindow* m_library_scroller { nullptr };
    wxBoxSizer*     m_library_sizer { nullptr };
    wxStaticText*   m_library_empty { nullptr };
    wxTimer         m_poll_timer;

    boost::filesystem::path m_selected_image_path;
    boost::filesystem::path m_preview_path;
    boost::filesystem::path m_artifact_path;
    wxImage m_preview_image;
    std::vector<GeneratedModelEntry> m_library_entries;
    std::string m_job_id;
    std::string m_artifact_format;
    uint64_t m_sequence { 0 };
    bool m_busy { false };
    bool m_awaiting_confirmation { false };
    bool m_ready { false };
    bool m_service_available { false };
    bool m_shutdown { false };
};

} // namespace Slic3r::GUI
