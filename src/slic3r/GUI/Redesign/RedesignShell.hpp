#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "../AI/ModelGeneration/WorkbenchImportSession.hpp"
#include "../AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"

#include <boost/filesystem/path.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>

#include <wx/image.h>

#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class wxPanel;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r::GUI {

enum class AIServiceStatus;
class ModelGenerationFeatureHost;
class UploadThumbnail;
class ImagePreview;
class ModelPreview3D;
class Plater;
class SmartSlicingFeatureHost;
class PrinterWorkspace;
class ImageHistorySidebar;

class RedesignShell final : public wxPanel
{
public:
    enum class Page { Assets, Image, Model, Print };

    explicit RedesignShell(wxWindow* parent, ModelGenerationFeatureHost* model_generation_host = nullptr,
                           Plater* plater = nullptr);
    ~RedesignShell() override;
    void disconnect_model_generation_host();

    bool navigate_to(Page page);
    bool navigate_to_tab(const wxString& id);
    wxString active_tab_id() const;
    void set_service_status(AIServiceStatus status);
    bool owns_model_workflow() const;
    bool native_workspace_visible() const;
    void start_slicing_from_workspace();
    void refresh_workflow_layout();
    void show_printer_media();
    void refresh_printer_state();

private:
    enum class ImageState { Empty, Loading, Ready, Failed };
    enum class SecondaryAction { None, Stop, RetryService, RestoreLatest, Restart };
    enum class ModelPageAction { None, RetryService, RetryModel, RestoreLatest, BackToDesign, ReloadPreview, Import };

    void build_image_workspace();
    wxPanel* m_assets_page {nullptr};
    wxPanel* build_model_workspace();
    wxPanel* create_placeholder_page(const wxString& title, const wxString& body);
    void connect_model_generation_host();
    void apply_model_generation_state(const ModelGenerationUIState& state);
    ModelGenerationUIInput current_generation_input() const;
    void update_generation_style_controls();
    void layout_image_settings();
    ModelGenerationUIOptions current_generation_options() const;
    bool synchronize_generation_input();
    bool synchronize_generation_options();
    bool generation_input_editable() const;
    void apply_generation_options(const ModelGenerationUIOptions& options);
    void on_generation_option_changed();
    bool request_open_history(const std::string& job_id);
    void request_generate_design();
    void request_generate_model();
    void request_primary_action();
    void request_secondary_action();
    void request_model_page_action();
    void update_model_page(const ModelGenerationUIState& state);
    void ensure_model_preview(const ModelGenerationUIState& state);
    void clear_model_preview();
    void ensure_design_image(const std::string& path);
    void clear_design_image();
    void choose_image();
    void accept_image(const wxString& path);
    void clear_image();
    void update_image_state();
    void update_preview_bitmap();
    void bind_upload_click(wxWindow* window);
    enum class ModelView { Result, Workbench, Slicing, Preview };
    void build_model_workflow();
    bool open_model_workbench();
    void show_model_view(ModelView view);
    void apply_workbench_state(const PostGenerationWorkbenchState& state);
    void apply_slicing_state(const SmartSlicingWorkbenchState& state);
    void confirm_workbench_import(const AI::ModelImportRequest& request);
    void select_slicing_tab(bool native);
    void start_workbench_slice();
    void update_import_loading(WorkbenchImportPhase phase, const std::string& message = {});
    void finish_import_loading();
    void check_import_first_frame();
    void open_print_preparation();

    wxBoxSizer* m_sizer { nullptr };
    wxPanel* m_content_host { nullptr };
    wxPanel* m_image_page { nullptr };
    wxPanel* m_model_page { nullptr };
    wxPanel* m_workbench_page { nullptr };
    wxPanel* m_slicing_page { nullptr };
    wxPanel* m_slicing_settings { nullptr };
    wxPanel* m_native_slice_commands { nullptr };
    wxPanel* m_native_plater_host { nullptr };
    wxPanel* m_ai_slicing_controls { nullptr };
    wxWindow* m_ai_slicing_tab { nullptr };
    wxWindow* m_native_slicing_tab { nullptr };
    wxWindow* m_slice_start { nullptr };
    wxWindow* m_slice_analyze { nullptr };
    wxWindow* m_slice_cancel { nullptr };
    wxWindow* m_slice_keep_mesh { nullptr };
    wxWindow* m_slice_export { nullptr };
    wxWindow* m_slice_print { nullptr };
    std::array<wxWindow*, 3> m_slice_goals { nullptr, nullptr, nullptr };
    wxStaticText* m_slice_details { nullptr };
    wxStaticText* m_slice_status { nullptr };
    wxStaticText* m_slice_check_status { nullptr };
    wxStaticText* m_slice_model_stats { nullptr };
    wxBoxSizer* m_slice_palette { nullptr };
    std::vector<std::string> m_slice_displayed_palette;
    wxStaticText* m_native_slice_status { nullptr };
    wxWindow* m_native_slice_start { nullptr };
    wxWindow* m_native_slice_export { nullptr };
    wxWindow* m_native_slice_print { nullptr };
    wxWindow* m_return_slice { nullptr };
    Plater* m_plater { nullptr };
    wxWindow* m_plater_original_parent { nullptr };
    SmartSlicingFeatureHost* m_slicing_host { nullptr };
    PostGenerationWorkbenchState m_workbench_state;
    SmartSlicingWorkbenchState m_slicing_state;
    ModelView m_model_view {ModelView::Result};
    bool m_native_slicing {false};
    bool m_import_in_progress {false};
    bool m_import_switching_view {false};
    bool m_import_awaiting_frame {false};
    size_t m_import_frame_baseline {0};
    std::shared_ptr<WorkbenchImportSession> m_import_session;
    wxPanel* m_import_loading {nullptr};
    wxStaticText* m_import_stage {nullptr};
    wxStaticText* m_import_elapsed {nullptr};
    wxWindow* m_import_cancel {nullptr};
    wxTimer m_import_timer;
    std::chrono::steady_clock::time_point m_import_started;
    std::chrono::steady_clock::time_point m_import_view_started;
    bool m_saved_sidebar_collapsed {false};
    std::string m_pending_workbench_job;
    PrinterWorkspace* m_print_page { nullptr };
    wxPanel* m_image_settings_panel { nullptr };
    wxScrolledWindow* m_image_settings_scroll { nullptr };
    wxPanel* m_image_settings_content { nullptr };
    wxPanel* m_upload_surface { nullptr };
    wxStaticText* m_upload_icon { nullptr };
    wxWindow* m_generate_button { nullptr };
    wxWindow* m_secondary_action_button { nullptr };
    ImageHistorySidebar* m_image_history { nullptr };
    wxStaticText* m_sidecar_status { nullptr };
    wxStaticText* m_provider_label { nullptr };
    wxPanel* m_style_choice { nullptr };
    wxPanel* m_stylized_styles_panel { nullptr };
    std::array<wxWindow*, 7> m_stylized_style_buttons {};
    wxPanel* m_custom_style_panel { nullptr };
    wxTextCtrl* m_custom_style { nullptr };
    int m_last_stylized_style { 1 };
    wxPanel* m_provider_choice { nullptr };
    std::string m_selected_style_id { "sculpture" };
    wxStaticText* m_upload_hint { nullptr };
    wxStaticText* m_upload_status { nullptr };
    wxStaticText* m_upload_filename { nullptr };
    UploadThumbnail* m_upload_thumbnail { nullptr };
    wxTextCtrl* m_prompt { nullptr };
    wxPanel* m_guide_panel { nullptr };
    wxPanel* m_preview_host { nullptr };
    wxPanel* m_source_preview_card { nullptr };
    wxPanel* m_result_preview_card { nullptr };
    ImagePreview* m_preview { nullptr };
    ImagePreview* m_result_preview { nullptr };
    ImagePreview* m_model_stage_visual { nullptr };
    ModelPreview3D* m_model_preview_3d { nullptr };
    wxPanel* m_model_visual_host { nullptr };
    wxPanel* m_model_view_controls { nullptr };
    wxStaticText* m_model_stage_title { nullptr };
    wxStaticText* m_model_stage_status { nullptr };
    wxStaticText* m_model_stage_summary { nullptr };
    wxStaticText* m_model_preview_details { nullptr };
    wxStaticText* m_model_progress_label { nullptr };
    wxWindow* m_model_progress { nullptr };
    wxWindow* m_model_action_button { nullptr };
    wxWindow* m_model_stop_button { nullptr };
    std::array<wxPanel*, 4> m_nav_markers { nullptr, nullptr, nullptr, nullptr };
    std::array<wxStaticText*, 4> m_nav_labels { nullptr, nullptr, nullptr, nullptr };
    std::array<wxPanel*, 4> m_pages { nullptr, nullptr, nullptr, nullptr };
    wxImage m_selected_image;
    wxImage m_design_image;
    boost::filesystem::path m_selected_image_path;
    boost::filesystem::path m_design_image_path;
    boost::filesystem::path m_loaded_model_path;
    boost::filesystem::path m_loading_model_path;
    wxSize m_last_preview_bounds;
    wxTimer m_preview_resize_timer;
    std::uint64_t m_image_request_generation { 0 };
    std::uint64_t m_design_image_request_generation { 0 };
    std::uint64_t m_model_preview_request_generation { 0 };
    std::thread m_model_preview_worker;
    ModelGenerationFeatureHost* m_model_generation_host { nullptr };
    ModelGenerationUIState m_model_generation_state;
    ModelGenerationUIOptions m_generation_options;
    Page m_active_page { Page::Image };
    // Keep the semantic workspace request, not only the visual host. Several
    // legacy requests (Prepare, Preview, Monitor and Multi-device) currently
    // share the Print host while their business meaning still differs.
    wxString m_active_tab_id;
    ImageState m_image_state { ImageState::Empty };
    ImageState m_design_image_state { ImageState::Empty };
    SecondaryAction m_secondary_action { SecondaryAction::None };
    ModelPageAction m_model_page_action { ModelPageAction::None };
    // A model submission owns the Shell route until the user explicitly
    // chooses "返回 2D 设计". The session and job identity reject stale
    // callbacks that could otherwise repaint the old image page.
    bool m_model_route_locked { false };
    std::uint64_t m_model_route_session { 0 };
    std::string m_model_route_job_id;
    bool m_input_sync_ok { false };
    bool m_option_sync_ok { false };
    bool m_submit_in_progress { false };
    bool m_applying_model_generation_state { false };
    bool m_model_preview_loading { false };
    bool m_model_preview_failed { false };
    std::string m_model_preview_error;
};

}
