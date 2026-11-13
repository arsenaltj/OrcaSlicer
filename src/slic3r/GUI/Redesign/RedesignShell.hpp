#pragma once

#include "ImageDesignDraft.hpp"
#include "NativeWorkspaceTheme.hpp"
#include "RedesignModelRoute.hpp"
#include "RedesignPageCoordinator.hpp"
#include "../Presentation/UiRuntime.hpp"
#include "../Presentation/WindowMountLease.hpp"
#include "RedesignPreviewPresentation.hpp"
#include "RedesignPresentationDeadline.hpp"
#include "RedesignWorkspaceNavigation.hpp"
#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/ModelHistoryOpen.hpp"
#include "../AI/ModelGeneration/PortraitWorkbenchEntry.hpp"
#include "../AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "../AI/ModelGeneration/WorkbenchImportSession.hpp"
#include "../AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "../AI/Text/AITextClient.hpp"
#include "../AI/Text/AITextPolishState.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"

#include <boost/filesystem/path.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>
#include <optional>
#include <memory>
#include <vector>

#include <wx/image.h>

#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/eventfilter.h>

class wxBoxSizer;
class wxActivityIndicator;
class wxDialog;
class wxFrame;
class wxPanel;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r::GUI {

class WorkbenchLabel;

enum class AIServiceStatus;
class ModelGenerationFeatureHost;
class AdvancedCreationFeatureHost;
class AdvancedCreationWorkspace;
class AITextFeatureHost;
class RoundedActionButton;
class UploadThumbnail;
class ImagePreview;
class ModelPreview3D;
class Plater;
class SmartSlicingFeatureHost;
class PrinterWorkspace;
class ImageHistorySidebar;
class AssetsWorkspace;
class RedesignSlicingTools;
class SlicingCanvasOverlay;
class WorkbenchPaletteChoice;
class WorkbenchSlider;
class AccountWorkspace;
class PlatformAccountService;
class LibraryWorkspaceService;

class RedesignShell final : public wxPanel
{
public:
    using Page = RedesignWorkspacePage;

    explicit RedesignShell(wxWindow* parent, ModelGenerationFeatureHost* model_generation_host = nullptr,
                           Plater* plater = nullptr, AITextFeatureHost* text_feature_host = nullptr);
    ~RedesignShell() override;
    void disconnect_model_generation_host();

    bool navigate_to(Page page);
    bool navigate_to_tab(const wxString& id);
    wxString active_tab_id() const;
    void set_service_status(AIServiceStatus status);
    bool owns_model_workflow() const;
    bool native_workspace_visible() const;
    bool native_slicing_visible() const { return native_workspace_visible() && m_native_slicing; }
    void start_slicing_from_workspace();
    void refresh_workflow_layout();
    void request_viewport_frame();
    void present_restore_frame();
    void show_printer_media();
    void refresh_printer_state();

private:
    enum class ImageState { Empty, Loading, Ready, Failed };
    enum class SecondaryAction { None, Stop, RetryService, RestoreLatest, Restart, ReturnToDesign };
    enum class ModelPageAction { None, RetryService, RetryModel, RestoreLatest, BackToDesign, ReloadPreview, Import };

    void build_image_workspace();
    void switch_creation_mode(bool advanced);
    std::unique_ptr<AdvancedCreationFeatureHost> m_advanced_host;
    AdvancedCreationWorkspace* m_advanced_workspace {nullptr};
    void update_navigation();
    wxPanel* m_assets_page {nullptr};
    AssetsWorkspace* m_assets_workspace {nullptr};
    wxPanel* build_model_workspace();
    wxPanel* create_placeholder_page(const wxString& title, const wxString& body);
    void connect_model_generation_host();
    void apply_advanced_model_state(const ModelGenerationUIState& state);
    void on_advanced_model_job(const AdvancedCreation::ModelJobEvent& event);
    void queue_advanced_model_route();
    AdvancedModelRouteIntent m_advanced_route_intent;
    std::uint64_t m_creation_navigation_epoch {0};
    bool m_advanced_route_refresh_pending {false};
    const ModelGenerationUIState& displayed_model_state() const;
    void mount_smart_model_history();
    void restore_smart_model_history();
    void sync_smart_model_history();
    void queue_creation_history_refresh();
    Presentation::RefreshScheduler m_creation_history_refresh;
    void apply_model_generation_state(const ModelGenerationUIState& state);
    void restore_image_input(const ModelGenerationUIInput& input);
    ModelGenerationUIInput current_generation_input() const;
    void update_generation_style_controls();
    void layout_image_settings();
    ModelGenerationUIOptions current_generation_options() const;
    bool synchronize_generation_input();
    bool synchronize_generation_options();
    bool generation_input_editable() const;
    void refresh_prompt_count();
    void refresh_text_capability(bool sidecar_compatible);
    void request_prompt_polish();
    void finish_prompt_polish(std::uint64_t sequence, const std::string& prompt,
                              std::uint64_t image_generation, bool has_reference_image,
                              const std::string& job_id,
                              std::uint64_t model_session, AITextClient::PolishResult result);
    void fail_prompt_polish(std::uint64_t sequence, const std::string& prompt,
                            std::uint64_t image_generation, bool has_reference_image,
                            const std::string& job_id,
                            std::uint64_t model_session, AITextClient::Error error);
    bool prompt_polish_request_matches(const std::string& prompt,
                                       std::uint64_t image_generation, bool has_reference_image,
                                       const std::string& job_id, std::uint64_t model_session) const;
    void discard_stale_prompt_polish(std::uint64_t sequence, bool clear_original);
    void cancel_prompt_polish(bool clear_original);
    void restore_original_prompt();
    void refresh_prompt_polish_controls();
    void apply_generation_options(const ModelGenerationUIOptions& options);
    void on_generation_option_changed();
    void refresh_provider_catalog();
    void apply_provider_catalog(const AIModelGenerationClient::ProviderCatalog& catalog,
                                const std::string& error = {});
    void refresh_provider_choice(bool geometry_mode);
    bool request_open_history(const std::string& job_id);
    void request_generate_design();
    void request_generate_model();
    void request_primary_action();
    void request_secondary_action();
    void request_model_page_action();
    bool can_return_to_image_design() const;
    bool return_to_image_design();
    void update_model_page(const ModelGenerationUIState& state);
    void apply_model_page_snapshot(const ModelGenerationUIState& state);
    void layout_model_details();
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
    using ModelView = RedesignModelView;
    void build_model_workflow();
    bool open_model_workbench();
    bool begin_workbench_presentation(bool history_loading = false);
    bool continue_workbench_presentation();
    void layout_workbench_transition_cover();
    void finish_workbench_presentation(bool ready, bool report_failure = true, std::string error = {});
    void restore_workbench_source_page();
    void sync_page_settings_visibility(Page page);
    wxRect workbench_transition_bounds() const;
    void update_history_loading(ModelHistoryOpenEvent::Stage stage);
    void layout_history_loading();
    void finish_history_loading();
    void return_model_overview();
    bool show_model_view(ModelView view);
    void apply_workbench_state(const PostGenerationWorkbenchState& state);
    void apply_slicing_state(const SmartSlicingWorkbenchState& state);
    wxPanel* create_slicing_recommendation_source(wxWindow* parent);
    void set_slicing_recommendation_source_selection(int selection);
    void on_slicing_recommendation_source_changed(int selection);
    void confirm_workbench_import(const AI::ModelImportRequest& request);
    void select_slicing_tab(bool native);
    void start_workbench_slice();
    void update_import_loading(WorkbenchImportPhase phase, const std::string& message = {});
    void show_import_loading();
    void stage_import_target_view();
    void rollback_import_target_view();
    void commit_import_target_view();
    void retry_import_target_view();
    void finish_import_loading(bool retain_view_retry = false);
    void layout_import_loading();
    void layout_import_transition_cover();
    void restore_import_source_page();
    void check_import_first_frame();
    void open_print_preparation();
    bool update_print_preview(bool active, bool layers, bool support);
    void return_to_slice_preview();
    bool navigate_to_workbench();
    void refresh_navigation();
    bool prepare_preview_transition();
    void check_preview_first_frame(RedesignPageTransition::Token token);
    void finish_preview_transition(RedesignPageTransition::Token token, bool ready);
    void layout_preview_transition_cover();
    bool can_save_print_project() const;
    void refresh_project_save_actions();
    void save_print_project();
    void update_slicing_tools();
    void change_slicing_palette();
    void show_slicing_palette_details();
    void layout_slicing_overlays();
    void layout_slicing_camera_loading();
    void refresh_native_theme();
    bool slicing_mode_switch_allowed() const;
    bool block_workbench_navigation(bool leaves_workbench = true);
    bool navigate_to_impl(Page page, bool allow_locked_image_view);
    wxWindow* page_route_root(Page page, ModelView view) const;
    wxWindow* active_page_focus_root() const;
    void configure_page_lifecycle();
    void commit_active_page(Presentation::Dirty flags);
    void apply_slicing_snapshot();
    void present_active_page(bool flush_children = false);
    void remember_active_page_focus();
    void restore_active_page_focus();
    // User navigation to the image page may inspect the current design while
    // a model submission still owns the route. This path keeps the route lock
    // and exposes the image workspace as read-only.
    bool navigate_to_image_view();

    wxBoxSizer* m_sizer { nullptr };
    wxPanel* m_content_host { nullptr };
    wxPanel* m_image_page { nullptr };
    wxPanel* m_model_page { nullptr };
    wxPanel* m_workbench_page { nullptr };
    wxPanel* m_slicing_page { nullptr };
    wxPanel* m_slicing_settings { nullptr };
    wxWindow* m_slice_return_overview {nullptr};
    wxWindow* m_native_return_ai {nullptr};
    wxWindow* m_native_return_overview {nullptr};
    wxWindow* m_native_prepare {nullptr};
    wxWindow* m_native_preview {nullptr};
    NativeWorkspaceTheme m_native_theme;
    wxPanel* m_native_plater_host { nullptr };
    bool m_print_canvas_mounted {false}, m_print_canvas_layers {false}, m_print_canvas_support {true};
    std::function<bool()> m_print_preview_revision_current;
    std::string m_print_preview_gcode;
    wxPanel* m_ai_slicing_controls { nullptr };
    wxWindow* m_ai_slicing_tab { nullptr };
    wxWindow* m_native_slicing_tab { nullptr };
    wxPanel* m_slice_recommendation_source { nullptr };
    wxWindow* m_slice_start { nullptr };
    wxWindow* m_slice_analyze { nullptr };
    wxWindow* m_slice_cancel { nullptr };
    wxWindow* m_slice_keep_mesh { nullptr };
    wxWindow* m_slice_export { nullptr };
    wxWindow* m_slice_save { nullptr };
    wxWindow* m_slice_print { nullptr };
    std::array<wxWindow*, 3> m_slice_goals { nullptr, nullptr, nullptr };
    wxPanel* m_slice_parameter_panel {nullptr};
    // The compact slicing summary keeps every detail row in the left sidebar.
    std::vector<wxStaticText*> m_slice_parameter_labels;
    std::vector<wxStaticText*> m_slice_parameter_values;
    wxStaticText* m_slice_status { nullptr };
    wxStaticText* m_slice_check_status { nullptr };
    wxStaticText* m_slice_model_stats { nullptr };
    wxBoxSizer* m_slice_palette { nullptr };
    wxPanel* m_slice_palette_host {nullptr};
    WorkbenchPaletteChoice* m_slice_color_mode {nullptr};
    WorkbenchPaletteChoice* m_slice_color_source {nullptr};
    WorkbenchSlider* m_slice_color_count {nullptr};
    wxStaticText* m_slice_color_count_label {nullptr};
    wxWindow* m_slice_palette_all {nullptr};
    wxWindow* m_slice_check {nullptr};
    std::unique_ptr<RedesignSlicingTools> m_slice_tools;
    SlicingCanvasOverlay* m_slice_stats_overlay {nullptr};
    SlicingCanvasOverlay* m_slice_progress_overlay {nullptr};
    wxPanel* m_slice_camera_loading {nullptr};
    int m_slice_committed_count {6};
    int m_slice_progress_percent {0};
    wxString m_slice_progress_stage;
    Presentation::ProgressGate m_slice_progress_gate;
    int m_slice_progress_phase {0};
    bool m_slice_camera_fitted {false};
    bool m_slice_camera_loading_pending {false};
    bool m_slice_camera_waiting_for_frame {false};
    size_t m_slice_camera_frame_baseline {0};
    std::vector<std::string> m_slice_displayed_palette;
    wxWindow* m_return_slice { nullptr };
    Plater* m_plater { nullptr };
    Presentation::WindowMountLease m_plater_mount, m_print_plater_mount;
    SmartSlicingFeatureHost* m_slicing_host { nullptr };
    PostGenerationWorkbenchState m_workbench_state;
    SmartSlicingWorkbenchState m_slicing_state;
    Presentation::RefreshState m_slicing_dirty;
    bool m_native_slicing {false};
    bool m_preview_toolpath_outside {false};
    PreviewAutoFitState m_preview_auto_fit;
    SliceCompletionPresentation m_slice_completion;
    bool m_slice_completion_retry_pending {false};
    RedesignPreviewFrameGate m_preview_frame_gate;
    std::optional<RedesignPageTransition::Token> m_preview_transition;
    wxFrame* m_preview_transition_cover {nullptr};
    wxTimer m_preview_transition_timer;
    std::optional<RedesignPageTransition::Token> m_workbench_transition;
    uint64_t m_history_open_request {0};
    std::string m_history_open_asset;
    wxDialog* m_history_loading {nullptr};
    wxActivityIndicator* m_history_activity {nullptr};
    WorkbenchLabel *m_history_stage {nullptr}, *m_history_elapsed {nullptr};
    wxWindow* m_history_cancel {nullptr};
    std::chrono::steady_clock::time_point m_history_started;
    bool m_history_rollback_pending {false}, m_workbench_history_loading {false};
    bool m_workbench_source_reparented {false};
    wxFrame* m_workbench_transition_cover {nullptr};
    wxWindow* m_workbench_transition_source {nullptr};
    Presentation::WindowMountLease m_workbench_source_mount, m_import_source_mount;
    std::unique_ptr<wxEventFilter> m_workbench_transition_input_filter;
    wxSize m_workbench_prepared_size;
    wxTimer m_workbench_transition_timer;
    RedesignPresentationDeadline m_workbench_presentation_deadline;
    bool m_import_in_progress {false};
    bool m_import_awaiting_frame {false};
    bool m_import_target_staged {false};
    bool m_import_target_presenting {false};
    bool m_import_waiting_post_fit_frame {false};
    bool m_import_project_committed {false};
    bool m_import_view_retry_available {false};
    size_t m_import_frame_baseline {0};
    wxSize m_import_fitted_viewport;
    std::shared_ptr<WorkbenchImportSession> m_import_session;
    wxDialog* m_import_loading {nullptr};
    wxFrame* m_import_transition_cover {nullptr};
    wxActivityIndicator* m_import_activity {nullptr};
    WorkbenchLabel* m_import_stage {nullptr};
    WorkbenchLabel* m_import_elapsed {nullptr};
    wxWindow* m_import_cancel {nullptr};
    wxTimer m_import_timer;
    wxTimer m_slice_camera_retry_timer;
    int m_slice_camera_retry_attempts {0};
    std::chrono::steady_clock::time_point m_import_started;
    std::chrono::steady_clock::time_point m_import_view_started;
    boost::filesystem::path m_import_committed_model_path;
    RedesignPageCoordinator m_page_coordinator;
    Presentation::RefreshScheduler m_surface_refresh;
    bool m_presentation_stopping {false};
    Presentation::TaskScope m_result_preview_scope;
    Presentation::TaskScope m_design_image_scope;
    std::optional<RedesignPageTransition::Token> m_import_transition;
    bool m_saved_sidebar_collapsed {false};
    std::optional<bool> m_saved_collapse_toolbar;
    std::string m_pending_workbench_job;
    PrinterWorkspace* m_print_page { nullptr };
    wxPanel* m_image_settings_panel { nullptr };
    wxScrolledWindow* m_image_settings_scroll { nullptr };
    wxPanel* m_image_settings_content { nullptr };
    wxPanel* m_upload_surface { nullptr };
    wxStaticText* m_upload_icon { nullptr };
    wxWindow* m_generate_button { nullptr };
    wxWindow* m_secondary_action_button { nullptr };
    wxStaticText* m_creation_lock_hint { nullptr };
    ImageHistorySidebar* m_image_history { nullptr };
    wxPanel* m_smart_model_history_host { nullptr };
    wxStaticText* m_sidecar_status { nullptr };
    wxStaticText* m_provider_label { nullptr };
    wxPanel* m_style_choice { nullptr };
    wxPanel* m_stylized_styles_panel { nullptr };
    std::array<wxWindow*, 7> m_stylized_style_buttons {};
    wxPanel* m_custom_style_panel { nullptr };
    wxTextCtrl* m_custom_style { nullptr };
    int m_last_stylized_style { 1 };
    wxPanel* m_provider_choice { nullptr };
    wxPanel* m_face_limit_choice { nullptr };
    bool m_smart_creation {true};
    int m_style_filter {0};
    wxPanel* m_smart_styles {nullptr};
    wxStaticText* m_style_selection_label {nullptr};
    std::array<wxWindow*, 4> m_style_filters {};
    std::array<wxWindow*, 9> m_style_cards {};
    std::string m_selected_style_id { "realistic" };
    wxStaticText* m_upload_hint { nullptr };
    wxStaticText* m_upload_status { nullptr };
    wxStaticText* m_upload_link { nullptr };
    wxStaticText* m_upload_filename { nullptr };
    UploadThumbnail* m_upload_thumbnail { nullptr };
    wxTextCtrl* m_prompt { nullptr };
    wxStaticText* m_prompt_count { nullptr };
    wxStaticText* m_prompt_limit_warning { nullptr };
    RoundedActionButton* m_prompt_polish_button { nullptr };
    wxStaticText* m_prompt_polish_status { nullptr };
    wxStaticText* m_restore_prompt { nullptr };
    wxPanel* m_guide_panel { nullptr };
    wxPanel* m_preview_host { nullptr };
    wxPanel* m_source_preview_card { nullptr };
    wxPanel* m_result_preview_card { nullptr };
    ImagePreview* m_preview { nullptr };
    ImagePreview* m_result_preview { nullptr };
    ImagePreview* m_model_stage_visual { nullptr };
    wxSize m_model_result_layout_size;
    wxString m_model_result_summary;
    ModelPreview3D* m_model_preview_3d { nullptr };
    wxPanel* m_model_visual_host { nullptr };
    wxPanel* m_model_details_column { nullptr };
    wxPanel* m_model_view_controls { nullptr };
    wxStaticText* m_model_stage_title { nullptr };
    wxStaticText* m_model_stage_summary { nullptr };
    wxStaticText* m_model_preview_details { nullptr };
    wxStaticText* m_model_task_status { nullptr };
    wxWindow* m_model_action_button { nullptr };
    wxWindow* m_model_back_to_design_button { nullptr };
    wxWindow* m_model_stop_button { nullptr };
    std::array<wxWindow*, 3> m_nav_buttons {};
    std::array<wxPanel*, 5> m_pages {};
    std::array<wxWeakRef<wxWindow>, 5> m_page_focus;
    std::shared_ptr<PlatformAccountService> m_platform_account;
    std::shared_ptr<LibraryWorkspaceService> m_asset_service;
    bool m_account_return_assets {false};
    AccountWorkspace* m_account_workspace {nullptr};
    RoundedActionButton* m_account_nav {nullptr};
    bool m_account_started {false};
    bool m_account_startup {true};
    bool m_account_was_exchanging {false};
    std::array<wxWeakRef<wxWindow>, 4> m_model_view_focus;
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
    ModelGenerationFeatureHost* m_model_generation_host { nullptr };
    AITextFeatureHost* m_text_feature_host { nullptr };
    std::shared_ptr<AITextClient::RequestHandle> m_text_availability_request;
    std::shared_ptr<AITextClient::RequestHandle> m_prompt_polish_request;
    AITextPolishState m_prompt_polish_state;
    std::uint64_t m_text_availability_sequence { 0 };
    bool m_prompt_polish_available { false };
    std::string m_prompt_polish_unavailable_reason;
    ModelGenerationUIState m_model_generation_state;
    ModelGenerationUIState m_advanced_model_state;
    ImageDesignDraft m_image_design_draft;
    std::optional<ModelGenerationUIInput> m_saved_creation_input;
    std::optional<ModelGenerationUIOptions> m_saved_creation_options;
    ModelGenerationUIOptions m_generation_options;
    AIModelGenerationClient::ProviderCatalog m_provider_catalog;
    std::uint64_t m_provider_catalog_sequence { 0 };
    std::string m_provider_catalog_role;
    std::string m_provider_catalog_provider;
    bool m_generation_geometry_mode { true };
    int m_last_service_status { 0 };
    Page m_last_workbench_page { Page::Model };
    // Keep the semantic workspace request, not only the visual host. Several
    // legacy requests (Prepare, Preview, Monitor and Multi-device) currently
    // share the Print host while their business meaning still differs.
    wxString m_active_tab_id;
    ImageState m_image_state { ImageState::Empty };
    ImageState m_design_image_state { ImageState::Empty };
    SecondaryAction m_secondary_action { SecondaryAction::None };
    ModelPageAction m_model_page_action { ModelPageAction::None };
    // A model submission owns the Shell route until the user explicitly
    // chooses "返回图像设计". The session and route identity reject stale
    // callbacks that could otherwise repaint the old image page.
    bool m_model_route_locked { false };
    bool m_advanced_model_route { false };
    std::uint64_t m_model_route_session { 0 };
    std::string m_model_route_id;
    bool m_image_page_view_only { false };
    bool m_input_sync_ok { false };
    bool m_option_sync_ok { false };
    bool m_submit_in_progress { false };
    bool m_applying_model_generation_state { false };
    bool m_model_preview_loading { false };
    bool m_model_preview_failed { false };
    std::string m_model_preview_error;
};

}
