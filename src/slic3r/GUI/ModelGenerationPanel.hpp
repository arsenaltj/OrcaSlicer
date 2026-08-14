#pragma once

#include "AIModelGenerationClient.hpp"
#include "slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp"

#include <boost/filesystem/path.hpp>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxColourPickerCtrl;
class wxGauge;
class wxGridSizer;
class wxNotebook;
class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {

class ModelPreview3D;

class ModelGenerationPanel : public wxPanel
{
public:
    ModelGenerationPanel(wxWindow* parent, AI::IModelArtifactConsumer& artifact_consumer,
                         AI::IPrintablePaletteProvider& palette_provider);
    ~ModelGenerationPanel() override;

    void shutdown();
    void set_service_availability(bool available, const std::string& message = {});

private:
    void build_page();
    wxWindow* build_workflow_panel(wxWindow* parent);
    wxWindow* build_preview_panel(wxWindow* parent);
    wxWindow* build_model_library(wxWindow* parent);

    void on_choose_image(wxCommandEvent& event);
    void on_clear_image(wxCommandEvent& event);
    void on_printable_colors_toggled(wxCommandEvent& event);
    void on_palette_source_changed(wxCommandEvent& event);
    void on_add_custom_color(wxCommandEvent& event);
    void on_preprocess(wxCommandEvent& event);
    void on_generate(wxCommandEvent& event);
    void on_stop(wxCommandEvent& event);
    void on_import(wxCommandEvent& event);
    void on_discard(wxCommandEvent& event);
    void on_poll(wxTimerEvent& event);

    void handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence);
    void handle_error(const std::string& error, uint64_t sequence);
    void schedule_poll();
    void restore_latest_job();
    void restore_job(AIModelGenerationClient::JobStatus status, uint64_t sequence);
    void download_restored_input(uint64_t sequence);
    void refresh_controls();
    void refresh_palette();
    std::vector<size_t> valid_project_slots() const;
    std::vector<size_t> compatible_project_slots() const;
    std::vector<std::string> project_palette() const;
    std::vector<std::string> current_palette() const;
    bool use_printable_colors() const;
    std::string current_style() const;
    wxString current_style_label() const;
    int current_face_limit() const;
    bool has_image_input() const;
    bool job_uses_image() const;
    bool job_inputs_match() const;
    void remove_custom_color(const std::string& color);
    void reset(bool remove_remote);
    void download_preview(uint64_t sequence);
    void download_model_preview(uint64_t sequence);
    void download_and_import();
    void import_local_artifact(const boost::filesystem::path& path, uint64_t sequence);
    void cleanup_files();
    void set_preview_empty(const wxString& message);
    void show_selected_image_preview();
    void update_preview_view(bool center = false);
    void set_preview_zoom(double zoom);
    void update_progress(int value, int step, const wxString& phase);
    void update_workflow(const AIModelGenerationClient::JobStatus* status = nullptr);
    void load_library_entries();
    void save_library_entry(size_t artifact_size, size_t triangle_count, double width, double depth,
                            double height, size_t color_count);
    void load_library_entry(const boost::filesystem::path& model_path,
                            const std::vector<std::string>& palette, bool use_printable_colors,
                            const wxString& title);
    void refresh_library();

    struct GeneratedModelEntry
    {
        wxString title;
        wxString details;
        boost::filesystem::path model_path;
        boost::filesystem::path preview_path;
        std::vector<std::string> palette;
        std::string job_id;
        std::time_t generated_at { 0 };
        bool use_printable_colors { false };
    };

    AI::IModelArtifactConsumer&    m_artifact_consumer;
    AI::IPrintablePaletteProvider& m_palette_provider;
    AIModelGenerationClient m_client;

    wxStaticText*   m_prompt_label { nullptr };
    wxTextCtrl*     m_prompt { nullptr };
    wxChoice*       m_style { nullptr };
    wxChoice*       m_quality { nullptr };
    wxButton*       m_choose_image { nullptr };
    wxButton*       m_clear_image { nullptr };
    wxStaticText*   m_selected_image { nullptr };
    wxStaticText*   m_upload_notice { nullptr };
    wxPanel*        m_palette_panel { nullptr };
    wxPanel*        m_custom_color_panel { nullptr };
    wxGridSizer*    m_palette_sizer { nullptr };
    wxCheckBox*     m_use_printable_colors { nullptr };
    wxChoice*       m_palette_source { nullptr };
    wxChoice*       m_import_color_mode { nullptr };
    wxCheckBox*     m_auto_slice_after_import { nullptr };
    wxColourPickerCtrl* m_custom_color { nullptr };
    wxButton*       m_add_custom_color { nullptr };
    wxStaticText*   m_palette_summary { nullptr };
    wxStaticText*   m_preprocess_section { nullptr };
    wxButton*       m_preprocess { nullptr };
    wxStaticText*   m_prepared_prompt_label { nullptr };
    wxTextCtrl*     m_prepared_prompt { nullptr };
    wxStaticText*   m_workflow_steps { nullptr };
    wxStaticText*   m_workflow_phase { nullptr };
    wxStaticText*   m_preview_kind { nullptr };
    wxNotebook*     m_preview_book { nullptr };
    wxButton*       m_zoom_out { nullptr };
    wxButton*       m_zoom_fit { nullptr };
    wxButton*       m_zoom_in { nullptr };
    wxStaticText*   m_preview_zoom { nullptr };
    wxStaticText*   m_preview_message { nullptr };
    wxStaticText*   m_result_summary { nullptr };
    ModelPreview3D* m_model_preview { nullptr };
    wxStaticText*   m_model_preview_message { nullptr };
    wxStaticText*   m_model_stats { nullptr };
    wxButton*       m_reset_model_view { nullptr };
    wxButton*       m_generate { nullptr };
    wxButton*       m_stop { nullptr };
    wxStaticText*   m_progress_percent { nullptr };
    wxGauge*        m_generation_progress { nullptr };
    wxStaticText*   m_status { nullptr };
    wxButton*       m_import { nullptr };
    wxButton*       m_discard { nullptr };
    wxScrolledWindow* m_preview_area { nullptr };
    wxScrolledWindow* m_library_scroller { nullptr };
    wxBoxSizer*     m_library_sizer { nullptr };
    wxStaticText*   m_library_empty { nullptr };
    wxTimer         m_poll_timer;

    boost::filesystem::path m_selected_image_path;
    boost::filesystem::path m_job_image_path;
    boost::filesystem::path m_preview_path;
    boost::filesystem::path m_artifact_path;
    boost::filesystem::path m_displayed_model_path;
    wxImage m_reference_image;
    wxImage m_style_preview_image;
    wxBitmap m_reference_bitmap;
    wxBitmap m_style_preview_bitmap;
    wxRect m_reference_preview_pane;
    wxRect m_style_preview_pane;
    wxString m_style_preview_placeholder;
    std::vector<GeneratedModelEntry> m_library_entries;
    std::vector<std::string> m_palette;
    std::vector<std::string> m_custom_palette;
    std::vector<std::string> m_job_palette;
    std::vector<std::string> m_displayed_model_palette;
    wxString m_job_prompt;
    std::string m_job_style;
    int m_job_face_limit { 300000 };
    std::string m_job_id;
    std::string m_artifact_format;
    std::string m_artifact_color_encoding;
    uint64_t m_sequence { 0 };
    bool m_busy { false };
    bool m_awaiting_confirmation { false };
    bool m_ready { false };
    bool m_artifact_download_started { false };
    bool m_model_preview_ready { false };
    bool m_library_model_loaded { false };
    bool m_service_available { false };
    bool m_restore_checked { false };
    bool m_restoring_input { false };
    bool m_shutdown { false };
    bool m_updating_preview { false };
    bool m_style_preview_ready { false };
    bool m_palette_is_custom { false };
    bool m_job_use_printable_colors { false };
    double m_preview_zoom_factor { 1.0 };
};

} // namespace Slic3r::GUI
