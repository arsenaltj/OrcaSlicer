#include "ModelGenerationFeatureHost.hpp"

#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/ModelGenerationPanel.hpp"

#include <boost/log/trivial.hpp>
#include <wx/colour.h>
#include <wx/colordlg.h>
#include <wx/msgdlg.h>
#include "slic3r/GUI/I18N.hpp"
#include <wx/window.h>
#ifdef __WXMSW__
#include <commdlg.h>
#endif

#include <utility>

namespace Slic3r::GUI {

bool ModelGenerationUIInput::operator==(const ModelGenerationUIInput& other) const
{
    return image_path == other.image_path && prompt == other.prompt && style == other.style &&
           custom_style == other.custom_style;
}

bool ModelGenerationUIOptions::operator==(const ModelGenerationUIOptions& other) const
{
    return provider == other.provider && face_limit == other.face_limit &&
           geometry_quality == other.geometry_quality && texture_quality == other.texture_quality &&
           output_format == other.output_format;
}

bool ModelGenerationUIHistoryEntry::operator==(const ModelGenerationUIHistoryEntry& other) const
{
    return job_id == other.job_id && title == other.title && details == other.details &&
           design_only == other.design_only && thumbnail_width == other.thumbnail_width &&
           thumbnail_height == other.thumbnail_height && thumbnail_rgb == other.thumbnail_rgb &&
           thumbnail_alpha == other.thumbnail_alpha;
}

bool ModelGenerationUIState::same_content(const ModelGenerationUIState& other) const
{
    return stage == other.stage && input == other.input && options == other.options &&
           service_available == other.service_available &&
           service_availability_known == other.service_availability_known && busy == other.busy &&
           can_generate_design == other.can_generate_design && can_generate_model == other.can_generate_model &&
           can_stop == other.can_stop && can_retry_service == other.can_retry_service &&
           can_retry_model == other.can_retry_model &&
           can_restore_latest == other.can_restore_latest && can_import == other.can_import &&
           can_restart == other.can_restart && design_ready == other.design_ready && model_ready == other.model_ready &&
           model_generation_context == other.model_generation_context &&
           model_generation_session == other.model_generation_session && inputs_match_job == other.inputs_match_job &&
           progress == other.progress && job_id == other.job_id && model_asset_id == other.model_asset_id &&
           job_state == other.job_state &&
           design_elapsed_seconds == other.design_elapsed_seconds &&
           design_estimated_seconds == other.design_estimated_seconds &&
           job_phase == other.job_phase && provider_error_code == other.provider_error_code &&
           provider_error_category == other.provider_error_category && provider_name == other.provider_name &&
           provider_task_id == other.provider_task_id &&
           provider_conversion_task_id == other.provider_conversion_task_id &&
           provider_error_retryable == other.provider_error_retryable &&
           provider_error_ambiguous == other.provider_error_ambiguous &&
           status_text == other.status_text && summary_text == other.summary_text &&
           workflow_phase == other.workflow_phase && workflow_guidance == other.workflow_guidance &&
           cost_summary == other.cost_summary && original_image_path == other.original_image_path &&
           design_image_path == other.design_image_path && model_path == other.model_path &&
           history_entries == other.history_entries && history_loading == other.history_loading &&
           history_error == other.history_error;
}

struct ModelGenerationFeatureHost::Impl
{
    Impl(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import, RetryServiceFn retry_service)
        : workspace(std::make_unique<OrcaWorkspaceAdapter>(plater, navigate_after_import))
    {
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: creating model generation panel";
        model_generation = new ModelGenerationPanel(parent, *workspace, *workspace);
        model_generation->set_project_color_handler([this](size_t slot) {
            const auto state = model_generation->workbench_snapshot();
            auto* owner = wxGetTopLevelParent(model_generation);
            if (!state.can_edit_project_colors) {
                wxMessageBox(_L("模型正在处理或存在未接受候选，暂时不能修改工程耗材颜色。"),
                    _L("工程耗材颜色"), wxOK | wxICON_WARNING, owner);
                return;
            }
            const auto& channels = state.project_channels;
            const auto selected = std::find_if(channels.begin(), channels.end(),
                [slot](const auto& channel) { return channel.slot == slot; });
            if (selected == channels.end()) {
                wxMessageBox(_L("工程耗材槽位已变化，请重新选择颜色。"),
                    _L("工程耗材颜色"), wxOK | wxICON_WARNING, owner);
                return;
            }
            const auto current = workspace->capture_import_guard();
            wxColourData data;
            data.SetColour(wxColour(wxString::FromUTF8(selected->display_color)));
            data.SetChooseFull(true);
            // The feature panel is hidden when its workbench is mounted in the shell.
            wxColourDialog picker(owner, &data);
            picker.SetTitle(wxString::Format(_L("工程耗材 %u 的颜色"), unsigned(slot + 1)));
            if (picker.ShowModal() != wxID_OK) {
#ifdef __WXMSW__
                const auto diagnostic = ::CommDlgExtendedError();
                if (diagnostic != 0) {
                    BOOST_LOG_TRIVIAL(error) << "Project filament color dialog failed: " << diagnostic;
                    wxMessageBox(wxString::Format(_L("无法打开选色窗口，Windows 错误：%lu。"),
                        static_cast<unsigned long>(diagnostic)), _L("工程耗材颜色"), wxOK | wxICON_ERROR, owner);
                }
#endif
                return;
            }
            std::string error;
            const auto latest = model_generation->workbench_snapshot();
            if (!latest.can_edit_project_colors || latest.revision != state.revision || latest.asset_id != state.asset_id ||
                !workspace->set_project_filament_color(slot,
                    picker.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), current, error)) {
                if (error.empty()) error = "模型或工程状态已变化，请重新选择颜色。";
                wxMessageBox(wxString::FromUTF8(error), _L("工程耗材颜色"), wxOK | wxICON_ERROR, owner);
                return;
            }
            model_generation->synchronize_workbench_project_palette(true);
        }, [this] { return workspace->project_filament_channels(); });
        model_generation->set_ui_state_listener([this](const ModelGenerationUIState& state) {
            latest_state = state;
            if (state_listener)
                state_listener(latest_state);
        });
        model_generation->set_service_retry_handler(std::move(retry_service));
        model_generation->set_prepare_navigation_handler(std::move(navigate_after_import));
        model_generation->set_color_matching_handler([this, plater](const AI::GeneratedModelArtifact& artifact) {
            model_generation->show_workbench_color_matching(artifact, plater);
        });
        model_generation->Hide();
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: model generation panel created";
    }

    void shutdown()
    {
        if (shutdown_requested)
            return;
        shutdown_requested = true;
        if (import_session) import_session->invalidate();
        if (model_generation != nullptr) {
            model_generation->set_ui_state_listener({});
            model_generation->set_workbench_listener({});
            model_generation->set_workbench_results_handler({});
            model_generation->set_service_retry_handler({});
            model_generation->set_prepare_navigation_handler({});
            model_generation->set_color_matching_handler({});
            model_generation->set_workbench_import_handler({});
            model_generation->set_project_color_handler({});
            model_generation->shutdown();
        }
    }

    ModelGenerationUIState latest_state;
    ModelGenerationUIStateListener state_listener;

    std::unique_ptr<OrcaWorkspaceAdapter> workspace;
    ModelGenerationPanel* model_generation { nullptr };
    bool shutdown_requested { false };
    std::shared_ptr<WorkbenchImportSession> import_session;
};

ModelGenerationFeatureHost::ModelGenerationFeatureHost(wxWindow* parent, Plater* plater,
                                                       NavigateAfterImportFn navigate_after_import,
                                                       RetryServiceFn retry_service)
    : m_impl(std::make_unique<Impl>(parent, plater, std::move(navigate_after_import), std::move(retry_service)))
{}

ModelGenerationFeatureHost::~ModelGenerationFeatureHost()
{
    shutdown();
}

void ModelGenerationFeatureHost::set_workbench_import_handler(std::function<void(const AI::ModelImportRequest&)> handler)
{
    m_impl->model_generation->set_workbench_import_handler(std::move(handler));
}

AI::ModelImportResult ModelGenerationFeatureHost::import_workbench_model(const AI::ModelImportRequest& request)
{
    return m_impl->workspace->import_workbench_artifact(request);
}

bool ModelGenerationFeatureHost::import_workbench_model_async(const AI::ModelImportRequest& request,
    std::shared_ptr<WorkbenchImportSession> session, WorkbenchImportProgress progress,
    WorkbenchImportCompletion completion)
{
    if (m_impl->shutdown_requested || m_impl->import_session) return false;
    const auto source = workbench_snapshot();
    if (!source.can_import_for_slicing || source.model_path != request.artifact.local_path) return false;
    wxWeakRef<ModelGenerationPanel> panel(m_impl->model_generation);
    auto current = [panel, source] {
        if (!panel) return false;
        const auto latest = panel->workbench_snapshot();
        return latest.revision == source.revision && latest.asset_id == source.asset_id &&
            latest.model_path == source.model_path && latest.candidate_path.empty() && !latest.dirty;
    };
    m_impl->import_session = session;
    panel->set_workbench_import_running(true);
    bool started = false;
    try {
        started = m_impl->workspace->import_workbench_artifact_async(request, session, std::move(progress),
            [this, panel, session, completion = std::move(completion)](const auto& result) {
                if (!session->valid()) return;
                m_impl->import_session.reset();
                if (panel) panel->set_workbench_import_running(false);
                if (completion) completion(result);
            }, std::move(current));
    } catch (...) {
        m_impl->import_session.reset();
        if (panel) panel->set_workbench_import_running(false);
        throw;
    }
    if (!started) {
        m_impl->import_session.reset();
        if (panel) panel->set_workbench_import_running(false);
    }
    return started;
}

wxWindow* ModelGenerationFeatureHost::panel() const
{
    return m_impl->model_generation;
}

void ModelGenerationFeatureHost::initialize_for_shell()
{
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->initialize_for_shell_host();
}

ModelGenerationUIState ModelGenerationFeatureHost::snapshot() const
{
    return m_impl->latest_state;
}

void ModelGenerationFeatureHost::set_state_listener(ModelGenerationUIStateListener listener)
{
    m_impl->state_listener = std::move(listener);
    if (m_impl->state_listener)
        m_impl->state_listener(m_impl->latest_state);
}

bool ModelGenerationFeatureHost::synchronize_input(const ModelGenerationUIInput& input)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->synchronize_ui_input(input);
}

bool ModelGenerationFeatureHost::synchronize_options(const ModelGenerationUIOptions& options)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->synchronize_ui_options(options);
}

bool ModelGenerationFeatureHost::request_generate_design()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_generate_design();
}

bool ModelGenerationFeatureHost::request_generate_model()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_generate_model();
}

bool ModelGenerationFeatureHost::request_retry_model()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_retry_model();
}

bool ModelGenerationFeatureHost::request_stop()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_stop();
}

bool ModelGenerationFeatureHost::request_retry_service()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_retry_service();
}

bool ModelGenerationFeatureHost::request_restore_latest()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_restore_latest();
}

bool ModelGenerationFeatureHost::request_restart()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_restart();
}

bool ModelGenerationFeatureHost::request_import()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_import();
}

bool ModelGenerationFeatureHost::request_refresh_history()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_refresh_history();
}

bool ModelGenerationFeatureHost::request_open_image_history(const std::string& job_id)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_open_image_history(job_id);
}

bool ModelGenerationFeatureHost::request_open_history(const std::string& job_id)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_open_history(job_id);
}

PostGenerationWorkbenchState ModelGenerationFeatureHost::workbench_snapshot() const
{
    return m_impl->model_generation->workbench_snapshot();
}

void ModelGenerationFeatureHost::set_workbench_listener(PostGenerationWorkbenchListener listener)
{
    m_impl->model_generation->set_workbench_listener(std::move(listener));
}

void ModelGenerationFeatureHost::set_workbench_results_handler(std::function<void()> handler)
{
    m_impl->model_generation->set_workbench_results_handler(std::move(handler));
}

void ModelGenerationFeatureHost::set_workbench_return_to_design_handler(std::function<void()> handler)
{
    m_impl->model_generation->set_workbench_return_to_design_handler(std::move(handler));
}

bool ModelGenerationFeatureHost::request_open_workbench() { return m_impl->model_generation->request_open_workbench(); }
void ModelGenerationFeatureHost::mount_workbench(wxWindow* parent) { m_impl->model_generation->mount_workbench(parent); }
void ModelGenerationFeatureHost::mount_assets(wxWindow* parent) { m_impl->model_generation->mount_assets(parent); }
void ModelGenerationFeatureHost::unmount_assets() { m_impl->model_generation->unmount_assets(); }
void ModelGenerationFeatureHost::set_history_navigation_handler(std::function<void(bool)> handler)
{ m_impl->model_generation->set_history_navigation_handler(std::move(handler)); }
void ModelGenerationFeatureHost::unmount_workbench() { m_impl->model_generation->unmount_workbench(); }
void ModelGenerationFeatureHost::set_color_matching_handler(std::function<void(const AI::GeneratedModelArtifact&)> handler)
{
    m_impl->model_generation->set_color_matching_handler(std::move(handler));
}
bool ModelGenerationFeatureHost::request_enter_beauty() { return m_impl->model_generation->request_enter_beauty(); }
bool ModelGenerationFeatureHost::request_return_overview() { return m_impl->model_generation->request_return_overview(); }
bool ModelGenerationFeatureHost::request_workbench_color_matching() { return m_impl->model_generation->request_workbench_color_matching(); }
bool ModelGenerationFeatureHost::request_enable_portrait(bool enabled) { return m_impl->model_generation->request_enable_portrait(enabled); }

void ModelGenerationFeatureHost::set_service_availability(bool available, const std::string& message)
{
    set_service_availability(available, available, message);
}

void ModelGenerationFeatureHost::set_service_availability(bool available, bool generation_available, const std::string& message)
{
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->set_service_availability(available, generation_available, message);
}

void ModelGenerationFeatureHost::shutdown()
{
    m_impl->shutdown();
}

bool ModelGenerationFeatureHost::request_semantic_mode(SemanticMode mode) {
    return !m_impl->shutdown_requested && m_impl->model_generation->request_semantic_mode(mode);
}
bool ModelGenerationFeatureHost::request_portrait_optimization() {
    wxString reason;
    return !m_impl->shutdown_requested && m_impl->model_generation->request_portrait_optimization(reason);
}
void ModelGenerationFeatureHost::cancel_portrait_optimization() {
    if (!m_impl->shutdown_requested) m_impl->model_generation->cancel_portrait_optimization();
}

} // namespace Slic3r::GUI
