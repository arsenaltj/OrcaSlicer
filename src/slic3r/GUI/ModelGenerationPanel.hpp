#pragma once

#include "AIModelGenerationClient.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <functional>
#include <string>

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

private:
    void build_page();
    wxWindow* build_workflow_panel(wxWindow* parent);
    wxWindow* build_preview_panel(wxWindow* parent);
    wxWindow* build_library_placeholder(wxWindow* parent);

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
    wxStaticBitmap* m_preview { nullptr };
    wxStaticText*   m_preview_message { nullptr };
    wxStaticText*   m_result_summary { nullptr };
    wxButton*       m_generate { nullptr };
    wxButton*       m_stop { nullptr };
    wxGauge*        m_progress { nullptr };
    wxStaticText*   m_status { nullptr };
    wxButton*       m_import { nullptr };
    wxButton*       m_discard { nullptr };
    wxTimer         m_poll_timer;

    boost::filesystem::path m_selected_image_path;
    boost::filesystem::path m_preview_path;
    boost::filesystem::path m_artifact_path;
    std::string m_job_id;
    std::string m_artifact_format;
    uint64_t m_sequence { 0 };
    bool m_busy { false };
    bool m_awaiting_confirmation { false };
    bool m_ready { false };
    bool m_shutdown { false };
};

} // namespace Slic3r::GUI
