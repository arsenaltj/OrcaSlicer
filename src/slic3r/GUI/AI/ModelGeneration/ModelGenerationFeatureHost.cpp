#include "ModelGenerationFeatureHost.hpp"
#include "ModelWorkspaceFeatureHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "WorkbenchImportEligibility.hpp"

#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/AI/Orca/OrcaFilamentSelection.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/log/trivial.hpp>
#include <wx/colour.h>
#include <wx/colordlg.h>
#include <wx/msgdlg.h>
#include <wx/weakref.h>
#include "slic3r/GUI/I18N.hpp"
#include <wx/window.h>
#ifdef __WXMSW__
#include <commdlg.h>
#endif

#include <utility>

namespace Slic3r::GUI {

struct ModelGenerationFeatureHost::Impl
{
    Impl(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import, RetryServiceFn retry_service)
        : workspace(std::make_unique<OrcaWorkspaceAdapter>(plater, navigate_after_import))
    {
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: creating model generation panel";
        model_generation = new ModelGenerationPanel(parent, *workspace, *workspace);
        capabilities = std::make_unique<ModelWorkspaceFeatureHost>(ModelGenerationPresentation::generated_models_root(),
            plater, navigate_after_import, ModelLibraryApplication::Ports {
                [this] { return model_generation && model_generation->library_asset_switch_allowed(); },
                [this, parent](ModelLibraryCommand command, const ModelLibraryEntry& entry) {
                    if (command == ModelLibraryCommand::MatchColors) {
                        AI::GeneratedModelArtifact artifact; artifact.local_path = entry.model_path;
                        artifact.job_id = entry.job_id; artifact.generation_palette = entry.palette;
                        return capabilities->open_colors(parent, artifact);
                    }
                    const bool opened = model_generation->request_library_action(command, entry);
                    if (opened && history_navigation && (command == ModelLibraryCommand::OpenModel || command == ModelLibraryCommand::OpenImage))
                        history_navigation(command == ModelLibraryCommand::OpenImage);
                    if (opened && (command == ModelLibraryCommand::Remove || command == ModelLibraryCommand::Import)) capabilities->refresh_assets();
                    return opened;
                }, [this] { model_generation->request_library_local_import(); }});
        model_generation->set_project_color_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(model_generation);
            if (!model_generation->workbench_snapshot().can_edit_project_colors) {
                wxMessageBox(_L("模型正在处理或存在未接受候选，暂时不能配置打印机耗材。"),
                    _L("打印机耗材"), wxOK | wxICON_WARNING, owner);
                return;
            }
            show_orca_filament_selection(owner, *wxGetApp().plater(), slot, false,
                model_generation->project_filament_color_sources());
            model_generation->synchronize_workbench_project_palette(true);
        }, [this] { return workspace->project_filament_channels(); });
        model_generation->set_project_filament_edit_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(model_generation);
            if (!model_generation->workbench_snapshot().can_edit_project_colors) return;
            show_orca_filament_selection(owner, *wxGetApp().plater(), slot, false,
                model_generation->project_filament_color_sources());
            model_generation->synchronize_workbench_project_palette(true);
        });
        model_generation->set_project_filament_delete_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(model_generation);
            auto* plater = wxGetApp().plater();
            if (!plater || !model_generation->workbench_snapshot().can_edit_project_colors) return;
            model_generation->set_project_color_sync_suspended(true);
            delete_orca_printer_filament(owner, *plater, slot);
            model_generation->set_project_color_sync_suspended(false);
            // The handler is invoked by a swatch button.  Rebuilding the
            // palette synchronously destroys that button while Windows is
            // still dispatching its click message; the next deletion then
            // re-enters wxWindow::MSWWindowProc with a stale window.  Defer
            // the refresh until the current event has fully unwound.
            wxWeakRef<ModelGenerationPanel> weak(model_generation);
            wxGetApp().CallAfter([weak] {
                if (weak) weak->synchronize_workbench_project_palette(true);
            });
        });
        model_generation->set_ui_state_listener([this](const ModelGenerationUIState& state) {
            const bool assets_changed = state.model_asset_id != latest_state.model_asset_id ||
                (latest_state.busy && !state.busy && !state.job_id.empty());
            latest_state = state;
            if (assets_changed) capabilities->refresh_assets();
            if (state_listener)
                state_listener(latest_state);
        });
        model_generation->set_service_retry_handler(std::move(retry_service));
        model_generation->set_prepare_navigation_handler(std::move(navigate_after_import));
        model_generation->set_color_matching_handler([this, parent](const AI::GeneratedModelArtifact& artifact) {
            capabilities->open_colors(parent, artifact);
        });
        model_generation->Hide();
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: model generation panel created";
    }

    void shutdown()
    {
        if (shutdown_requested)
            return;
        shutdown_requested = true;
        if (capabilities) capabilities->shutdown();
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
            model_generation->set_project_filament_edit_handler({});
            model_generation->set_project_filament_delete_handler({});
            model_generation->shutdown();
        }
    }

    ModelGenerationUIState latest_state;
    ModelGenerationUIStateListener state_listener;

    std::unique_ptr<OrcaWorkspaceAdapter> workspace;
    std::unique_ptr<ModelWorkspaceFeatureHost> capabilities;
    std::function<void(bool)> history_navigation;
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
    if (!workbench_import_request_allowed(source, request)) return false;
    wxWeakRef<ModelGenerationPanel> panel(m_impl->model_generation);
    auto current = [panel, source] {
        if (!panel) return false;
        const auto latest = panel->workbench_snapshot();
        return latest.revision == source.revision && latest.asset_id == source.asset_id &&
            latest.model_path == source.model_path && latest.candidate_path.empty() && !latest.dirty &&
            latest.check.model_sha256 == source.check.model_sha256;
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
    m_impl->capabilities->refresh_assets();
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
wxWindow* ModelGenerationFeatureHost::workbench_import_viewport() const { return m_impl->model_generation->workbench_import_viewport(); }
wxWindow* ModelGenerationFeatureHost::workbench_import_history() const { return m_impl->model_generation->workbench_import_history(); }
void ModelGenerationFeatureHost::mount_workbench_import_history(wxWindow* parent) { m_impl->model_generation->mount_workbench_import_history(parent); }
void ModelGenerationFeatureHost::restore_workbench_import_history() { m_impl->model_generation->restore_workbench_import_history(); }
void ModelGenerationFeatureHost::set_workbench_import_feedback(bool active) { m_impl->model_generation->set_workbench_import_feedback(active); }
void ModelGenerationFeatureHost::mount_assets(wxWindow* parent) { m_impl->capabilities->mount_assets(parent); }
void ModelGenerationFeatureHost::unmount_assets() { m_impl->capabilities->unmount_assets(); }
void ModelGenerationFeatureHost::set_history_navigation_handler(std::function<void(bool)> handler)
{ m_impl->history_navigation = handler; m_impl->model_generation->set_history_navigation_handler(std::move(handler)); }
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

ModelWorkspaceFeatureHost& ModelGenerationFeatureHost::workspace_capabilities() { return *m_impl->capabilities; }

} // namespace Slic3r::GUI
