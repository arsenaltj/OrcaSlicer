#include "ModelGenerationFeatureHost.hpp"
#include "ModelWorkspaceFeatureHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "WorkbenchImportEligibility.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"

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
#include <wx/sizer.h>
#ifdef __WXMSW__
#include <commdlg.h>
#endif

#include <utility>
#include <optional>

namespace Slic3r::GUI {

struct ModelGenerationFeatureHost::Impl
{
    Impl(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import, RetryServiceFn retry_service)
        : workspace(std::make_unique<OrcaWorkspaceAdapter>(plater, navigate_after_import))
        , local_import_navigation(navigate_after_import)
    {
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: creating model generation panel";
        workbench = new ModelGenerationPanel(parent, *workspace, *workspace, ModelGenerationPanel::Role::WorkbenchAssets);
        configure_local_import(workbench);
        workbench->initialize_workbench_assets();
        workbench->Hide();
        model_generation = new ModelGenerationPanel(parent, *workspace, *workspace, ModelGenerationPanel::Role::SmartCreation);
        configure_local_import(model_generation);
        model_generation->set_asset_replace_guard([this] { return workbench->library_asset_switch_allowed(); });
        model_generation->set_asset_open_handler([this](WorkbenchOpenRequest request) { return open_asset(std::move(request)); });
        capabilities = std::make_unique<ModelWorkspaceFeatureHost>(ModelGenerationPresentation::generated_models_root(),
            plater, navigate_after_import, ModelLibraryApplication::Ports {
                [this] { return workbench && workbench->library_asset_switch_allowed(); },
                [this, parent](ModelLibraryCommand command, const ModelLibraryEntry& entry) {
                    if (command == ModelLibraryCommand::MatchColors) {
                        AI::GeneratedModelArtifact artifact; artifact.local_path = entry.model_path;
                        artifact.job_id = entry.job_id; artifact.generation_palette = entry.palette;
                        return capabilities->open_colors(parent, artifact);
                    }
                    const auto format = AI::model_artifact_format(entry.model_path);
                    const bool editable_import = command == ModelLibraryCommand::Import &&
                        (format == "glb" || format == "obj");
                    bool opened = false;
                    if (editable_import) {
                        WorkbenchOpenRequest request; request.entry = entry;
                        request.origin = WorkbenchOpenRequest::Origin::Library;
                        opened = open_asset(std::move(request));
                        import_on_history_commit = opened;
                    } else opened = command == ModelLibraryCommand::OpenImage
                        ? model_generation->request_library_action(command, entry)
                        : workbench->request_library_action(command, entry);
                    if (opened && history_navigation && command == ModelLibraryCommand::OpenImage)
                        history_navigation(true);
                    if (opened && (command == ModelLibraryCommand::Remove || command == ModelLibraryCommand::Import)) capabilities->refresh_assets();
                    return opened;
                }, [this] { workbench->request_library_local_import(); }});
        workbench->set_project_color_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(workbench);
            if (!workbench->workbench_snapshot().can_edit_project_colors) {
                wxMessageBox(_L("模型正在处理或存在未接受候选，暂时不能配置打印机耗材。"),
                    _L("打印机耗材"), wxOK | wxICON_WARNING, owner);
                return;
            }
            if (show_orca_filament_selection(owner, *wxGetApp().plater(), slot, false,
                workbench->project_filament_color_sources()))
                workbench->synchronize_workbench_project_palette(true);
        }, [this] { return workspace->project_filament_channels(); });
        workbench->set_project_filament_edit_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(workbench);
            if (!workbench->workbench_snapshot().can_edit_project_colors) return;
            if (show_orca_filament_selection(owner, *wxGetApp().plater(), slot, false,
                workbench->project_filament_color_sources()))
                workbench->synchronize_workbench_project_palette(true);
        });
        workbench->set_project_filament_delete_handler([this](size_t slot) {
            auto* owner = wxGetTopLevelParent(workbench);
            auto* plater = wxGetApp().plater();
            if (!plater || !workbench->workbench_snapshot().can_edit_project_colors) return;
            workbench->set_project_color_sync_suspended(true);
            delete_orca_printer_filament(owner, *plater, slot);
            workbench->set_project_color_sync_suspended(false);
            // The handler is invoked by a swatch button.  Rebuilding the
            // palette synchronously destroys that button while Windows is
            // still dispatching its click message; the next deletion then
            // re-enters wxWindow::MSWWindowProc with a stale window.  Defer
            // the refresh until the current event has fully unwound.
            wxWeakRef<ModelGenerationPanel> weak(workbench);
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
        workbench->set_prepare_navigation_handler(std::move(navigate_after_import));
        workbench->set_color_matching_handler([this, parent](const AI::GeneratedModelArtifact& artifact) {
            capabilities->open_colors(parent, artifact);
        });
        model_generation->Hide();
        BOOST_LOG_TRIVIAL(info) << "AI model generation startup: model generation panel created";
    }

    void configure_local_import(ModelGenerationPanel* panel)
    {
        panel->set_local_import_executor([this](const AI::ModelImportRequest& request,
            std::shared_ptr<WorkbenchImportSession> session, WorkbenchImportProgress progress,
            WorkbenchImportCompletion completion, std::function<bool()> current) {
            auto finished = [completion = std::move(completion), current, navigate = local_import_navigation](const AI::ModelImportResult& result) {
                if (result.imported() && current && current() && navigate) navigate();
                if (completion) completion(result);
            };
            return !shutdown_requested && workspace->import_workbench_artifact_async(request,
                std::move(session), std::move(progress), std::move(finished), std::move(current));
        });
    }

    void shutdown()
    {
        if (shutdown_requested)
            return;
        shutdown_requested = true;
        preparation_lifetime.reset();
        if (capabilities) capabilities->shutdown();
        if (import_session) import_session->invalidate();
        for (auto* panel : {model_generation, workbench, legacy_creation}) {
            if (!panel) continue;
            panel->set_ui_state_listener({});
            panel->set_workbench_listener({});
            panel->set_workbench_results_handler({});
            panel->set_service_retry_handler({});
            panel->set_prepare_navigation_handler({});
            panel->set_color_matching_handler({});
            panel->set_workbench_import_handler({});
            panel->set_local_import_executor({});
            panel->set_project_color_handler({});
            panel->set_project_filament_edit_handler({});
            panel->set_project_filament_delete_handler({});
            panel->set_asset_open_handler({});
            panel->shutdown();
        }
    }

    bool open_asset(WorkbenchOpenRequest request)
    {
        if (shutdown_requested || !workbench || !workbench->library_asset_switch_allowed()) return false;
        if (request.origin == WorkbenchOpenRequest::Origin::Smart ||
            request.origin == WorkbenchOpenRequest::Origin::Legacy) {
            const auto* creator = request.origin == WorkbenchOpenRequest::Origin::Smart ? model_generation : legacy_creation;
            if (!creator || !request.matches_creator(creator->m_displayed_model_job_id, creator->m_model_generation_session)) return false;
        }
        const auto context = request.return_context;
        if (!workbench->open_workbench_asset(std::move(request))) return false;
        if (!context.empty()) {
            previous_return_context = return_context;
            return_context = context;
        }
        return true;
    }

    void maybe_import(const PostGenerationWorkbenchState& state)
    {
        if (pending_import_asset.empty() || shutdown_requested) return;
        if (workbench->m_history_open_generation != pending_import_generation ||
            state.check.status == WorkbenchCheckStatus::Failed) {
            pending_import_asset.clear(); return;
        }
        if (!state.workbench_presented || state.modal_blocking || !state.actions.can_import) return;
        const auto asset = state.asset_id;
        const auto revision = state.revision;
        pending_import_asset.clear();
        wxWeakRef<ModelGenerationPanel> weak(workbench);
        workbench->CallAfter([weak, asset, revision] {
            if (!weak || weak->m_shutdown) return;
            const auto latest = weak->workbench_snapshot();
            if (latest.asset_id == asset && latest.revision == revision && latest.workbench_presented &&
                !latest.modal_blocking && latest.actions.can_import)
                weak->request_workbench_color_matching();
        });
    }

    ModelGenerationPanel* legacy_panel()
    {
        if (shutdown_requested) return nullptr;
        if (!legacy_creation) {
            legacy_creation = new ModelGenerationPanel(model_generation->GetParent(), *workspace, *workspace,
                ModelGenerationPanel::Role::AdvancedCreation);
            configure_local_import(legacy_creation);
            legacy_creation->set_asset_replace_guard([this] { return workbench->library_asset_switch_allowed(); });
            legacy_creation->set_asset_open_handler([this](WorkbenchOpenRequest request) { return open_asset(std::move(request)); });
            legacy_creation->initialize_for_shell_host();
            legacy_creation->set_service_availability(service_available, generation_available, service_message);
            legacy_creation->set_ui_state_listener([this](const ModelGenerationUIState& state) {
                if (advanced_state_listener) advanced_state_listener(state);
            });
            legacy_creation->Hide();
        }
        return legacy_creation;
    }

    ModelGenerationUIState latest_state;
    ModelGenerationUIStateListener state_listener;

    std::unique_ptr<OrcaWorkspaceAdapter> workspace;
    NavigateAfterImportFn local_import_navigation;
    std::unique_ptr<ModelWorkspaceFeatureHost> capabilities;
    std::function<void(bool)> history_navigation;
    ModelGenerationPanel* model_generation { nullptr };
    ModelGenerationPanel* workbench { nullptr };
    ModelGenerationPanel* legacy_creation { nullptr };
    ModelGenerationUIStateListener advanced_state_listener;
    std::string return_context;
    std::optional<std::string> previous_return_context;
    bool import_on_history_commit {false};
    std::string pending_import_asset;
    std::uint64_t pending_import_generation {0};
    bool service_available {false}, generation_available {false};
    std::string service_message;
    bool shutdown_requested { false };
    std::shared_ptr<WorkbenchImportSession> import_session;
    std::shared_ptr<const char> preparation_lifetime {std::make_shared<const char>(0)};
};

ModelGenerationFeatureHost::ModelGenerationFeatureHost(wxWindow* parent, Plater* plater,
                                                       NavigateAfterImportFn navigate_after_import,
                                                       RetryServiceFn retry_service)
    : m_impl(std::make_unique<Impl>(parent, plater, std::move(navigate_after_import), std::move(retry_service)))
{}

bool ModelGenerationFeatureHost::can_replace_asset() const
{ return m_impl->workbench && m_impl->workbench->library_asset_switch_allowed(); }

ModelGenerationFeatureHost::~ModelGenerationFeatureHost()
{
    shutdown();
}

void ModelGenerationFeatureHost::set_workbench_import_handler(std::function<void(const AI::ModelImportRequest&)> handler)
{
    m_impl->workbench->set_workbench_import_handler(std::move(handler));
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
    const auto admitted = current_workbench_import_source(request, [this] { return workbench_snapshot(); });
    if (!admitted) return false;
    const auto source = *admitted;
    wxWeakRef<ModelGenerationPanel> panel(m_impl->workbench);
    auto current = [panel, source] {
        if (!panel) return false;
        const auto latest = panel->workbench_snapshot();
        return latest.revision == source.revision && latest.asset_id == source.asset_id &&
            latest.model_path == source.model_path && latest.candidate_path.empty() && !latest.dirty &&
            latest.check.model_sha256 == source.check.model_sha256;
    };
    m_impl->import_session = session;
    panel->set_workbench_import_running(true);
    const auto alive = std::weak_ptr<const char>(m_impl->preparation_lifetime);
    WorkbenchImportCompletion finished = [this, panel, session, alive, completion = std::move(completion)](
            const AI::ModelImportResult& result) {
        if (alive.expired() || m_impl->import_session != session) return;
        m_impl->import_session.reset();
        if (panel) panel->set_workbench_import_running(false);
        if (session->valid() && completion) completion(result);
    };
    bool started = false;
    try {
        // Shell has already painted the original import card. File/hash,
        // saved appearance and exact native topology preparation run behind it.
        started = panel->prepare_workbench_import_request_async(request, session,
            [this, alive, session, progress = std::move(progress), finished, current = std::move(current)](
                    AI::ModelImportRequest prepared, std::string error) mutable {
                if (alive.expired()) return;
                AI::ModelImportResult rejected;
                if (session->cancelled()) {
                    rejected.outcome = AI::ModelImportOutcome::Cancelled;
                    finished(rejected); return;
                }
                if (!error.empty()) {
                    // Keep the existing preparation-failure presentation. The
                    // Shell only closes its loading card for this outcome.
                    wxMessageBox(wxString::FromUTF8(error), _L("配色确认"), wxOK | wxICON_ERROR,
                        m_impl->workbench);
                    rejected.outcome = AI::ModelImportOutcome::Cancelled;
                    finished(rejected); return;
                }
                if (!current()) {
                    rejected.error = error.empty()
                        ? "模型或工程状态已变化，或正在执行其他处理任务。请完成当前操作后重新导入。" : std::move(error);
                    session->advance(WorkbenchImportPhase::Failed);
                    finished(rejected); return;
                }
                try {
                    if (m_impl->workspace->import_workbench_artifact_async(prepared, session,
                            std::move(progress), finished, std::move(current))) return;
                    rejected.error = "模型或工程状态已变化，或正在执行其他处理任务。请完成当前操作后重新导入。";
                } catch (const std::exception& failure) { rejected.error = failure.what(); }
                session->advance(WorkbenchImportPhase::Failed);
                finished(rejected);
            });
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

void ModelGenerationFeatureHost::get_provider_catalog(
    const std::string& role, const std::string& provider,
    AIModelGenerationClient::CatalogFn on_complete,
    AIModelGenerationClient::ErrorFn on_error)
{
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->get_provider_catalog(role, provider,
                                                       std::move(on_complete), std::move(on_error));
    else if (on_error)
        on_error("Model generation panel is unavailable.");
}

bool ModelGenerationFeatureHost::request_generate_smart()
{ return can_replace_asset() && m_impl->model_generation && m_impl->model_generation->request_generate_smart(); }

void ModelGenerationFeatureHost::get_advanced_provider_catalog(
    const std::string& role, const std::string& provider,
    AIModelGenerationClient::CatalogFn on_complete,
    AIModelGenerationClient::ErrorFn on_error)
{
    // Each creator owns its catalog cache and request generation as well as
    // its jobs, so querying advanced providers cannot invalidate smart queries.
    if (auto* creator = m_impl->legacy_panel())
        creator->get_provider_catalog(role, provider, std::move(on_complete), std::move(on_error));
    else if (on_error)
        on_error("Advanced generation adapter is unavailable.");
}

ModelGenerationSubmissionResult ModelGenerationFeatureHost::request_generate_design()
{
    return can_replace_asset() && m_impl->model_generation ? m_impl->model_generation->request_generate_design() : ModelGenerationSubmissionResult::Rejected;
}

ModelGenerationSubmissionResult ModelGenerationFeatureHost::request_generate_model()
{
    return can_replace_asset() && m_impl->model_generation ? m_impl->model_generation->request_generate_model() : ModelGenerationSubmissionResult::Rejected;
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
    if (m_impl->model_generation) m_impl->model_generation->request_refresh_history();
    return m_impl->workbench->request_refresh_history();
}

bool ModelGenerationFeatureHost::request_open_image_history(const std::string& job_id)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_open_image_history(job_id);
}

bool ModelGenerationFeatureHost::request_open_history(const std::string& job_id)
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_open_history(job_id);
}

void ModelGenerationFeatureHost::set_smart_creation_presentation(bool smart)
{ if (m_impl->model_generation) m_impl->model_generation->set_smart_creation_presentation(smart); }

PostGenerationWorkbenchState ModelGenerationFeatureHost::workbench_snapshot() const
{
    return m_impl->workbench->workbench_snapshot();
}

void ModelGenerationFeatureHost::set_workbench_listener(PostGenerationWorkbenchListener listener)
{
    m_impl->workbench->set_workbench_listener([this, listener = std::move(listener)](const auto& state) {
        if (m_impl->shutdown_requested) return;
        if (m_impl->model_generation) m_impl->model_generation->refresh_controls();
        if (listener) listener(state);
        m_impl->maybe_import(state);
    });
}

void ModelGenerationFeatureHost::set_workbench_results_handler(std::function<void()> handler)
{
    m_impl->workbench->set_workbench_results_handler(std::move(handler));
}

void ModelGenerationFeatureHost::set_workbench_return_to_design_handler(std::function<void()> handler)
{
    m_impl->workbench->set_workbench_return_to_design_handler(std::move(handler));
}

bool ModelGenerationFeatureHost::request_open_workbench() { return m_impl->workbench->request_open_workbench(); }
void ModelGenerationFeatureHost::set_workbench_presented(bool presented)
{
    if (!presented) m_impl->pending_import_asset.clear();
    m_impl->workbench->set_workbench_presented(presented);
}

void ModelGenerationFeatureHost::present_workbench_restore_frame()
{
    if (m_impl->workbench) m_impl->workbench->present_workbench_restore_frame();
}

void ModelGenerationFeatureHost::prepare_workbench_presentation(std::function<void()> ready)
{
    m_impl->workbench->prepare_workbench_presentation(std::move(ready));
}

void ModelGenerationFeatureHost::cancel_workbench_presentation()
{
    m_impl->workbench->cancel_workbench_presentation();
}
void ModelGenerationFeatureHost::raise_workbench_modal() { m_impl->workbench->raise_workbench_modal(); }
void ModelGenerationFeatureHost::mount_workbench(wxWindow* parent) { m_impl->workbench->mount_workbench(parent); }
wxWindow* ModelGenerationFeatureHost::workbench_import_viewport() const { return m_impl->workbench->workbench_import_viewport(); }
wxWindow* ModelGenerationFeatureHost::workbench_import_history() const { return m_impl->workbench->workbench_import_history(); }
void ModelGenerationFeatureHost::mount_workbench_import_history(wxWindow* parent) { m_impl->workbench->mount_workbench_import_history(parent); }
void ModelGenerationFeatureHost::restore_workbench_import_history() { m_impl->workbench->restore_workbench_import_history(); }
void ModelGenerationFeatureHost::mount_workbench_history_sidebar(wxWindow* parent) { m_impl->workbench->mount_workbench_history_sidebar(parent); }
void ModelGenerationFeatureHost::restore_workbench_history_sidebar() { m_impl->workbench->restore_workbench_history_sidebar(); }
void ModelGenerationFeatureHost::set_workbench_import_feedback(bool active) { m_impl->workbench->set_workbench_import_feedback(active); }
void ModelGenerationFeatureHost::mount_assets(wxWindow* parent) { m_impl->capabilities->mount_assets(parent); }
void ModelGenerationFeatureHost::unmount_assets() { m_impl->capabilities->unmount_assets(); }
void ModelGenerationFeatureHost::set_history_navigation_handler(std::function<void(bool)> handler)
{ m_impl->history_navigation = handler; m_impl->model_generation->set_history_navigation_handler(handler);
  m_impl->workbench->set_history_navigation_handler(std::move(handler)); }
void ModelGenerationFeatureHost::set_history_model_open_handler(ModelHistoryOpenHandler handler)
{ m_impl->workbench->set_history_model_open_handler(std::move(handler)); }
void ModelGenerationFeatureHost::finish_history_model_open(bool accepted, std::function<void()> complete)
{
    if (!accepted) {
        if (m_impl->previous_return_context) m_impl->return_context = *m_impl->previous_return_context;
        m_impl->import_on_history_commit = false;
        m_impl->pending_import_asset.clear();
    }
    m_impl->previous_return_context.reset();
    m_impl->workbench->finish_history_model_open(accepted, std::move(complete));
    if (accepted && m_impl->import_on_history_commit) {
        m_impl->import_on_history_commit = false;
        const auto state = m_impl->workbench->workbench_snapshot();
        m_impl->pending_import_asset = state.asset_id;
        m_impl->pending_import_generation = m_impl->workbench->m_history_open_generation;
        m_impl->maybe_import(state);
    }
}
void ModelGenerationFeatureHost::unmount_workbench() { m_impl->workbench->unmount_workbench(); }
void ModelGenerationFeatureHost::set_color_matching_handler(std::function<void(const AI::GeneratedModelArtifact&)> handler)
{
    m_impl->workbench->set_color_matching_handler(std::move(handler));
}
bool ModelGenerationFeatureHost::request_enter_beauty() { return m_impl->workbench->request_enter_beauty(); }
bool ModelGenerationFeatureHost::request_return_overview() { return m_impl->workbench->request_return_overview(); }
bool ModelGenerationFeatureHost::request_workbench_color_matching() { return m_impl->workbench->request_workbench_color_matching(); }
bool ModelGenerationFeatureHost::request_enable_portrait(bool enabled) { return m_impl->workbench->request_enable_portrait(enabled); }

void ModelGenerationFeatureHost::set_service_availability(bool available, const std::string& message)
{
    set_service_availability(available, available, message);
}

void ModelGenerationFeatureHost::set_service_availability(bool available, bool generation_available, const std::string& message)
{
    m_impl->service_available = available;
    m_impl->generation_available = generation_available;
    m_impl->service_message = message;
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->set_service_availability(available, generation_available, message);
    m_impl->workbench->set_service_availability(available, false, message);
    if (m_impl->legacy_creation) m_impl->legacy_creation->set_service_availability(available, generation_available, message);
}

void ModelGenerationFeatureHost::shutdown()
{
    m_impl->shutdown();
}

bool ModelGenerationFeatureHost::request_semantic_mode(SemanticMode mode) {
    return !m_impl->shutdown_requested && m_impl->workbench->request_semantic_mode(mode);
}
bool ModelGenerationFeatureHost::request_portrait_optimization() {
    wxString reason;
    return !m_impl->shutdown_requested && m_impl->workbench->request_portrait_optimization(reason);
}
void ModelGenerationFeatureHost::cancel_portrait_optimization() {
    if (!m_impl->shutdown_requested) m_impl->workbench->cancel_portrait_optimization();
}

wxWindow* ModelGenerationFeatureHost::legacy_creation_panel() { return m_impl->legacy_panel(); }
void ModelGenerationFeatureHost::unmount_legacy_creation()
{
    if (!m_impl->legacy_creation) return;
    auto* panel = m_impl->legacy_creation;
    if (auto* sizer = panel->GetContainingSizer()) sizer->Detach(panel);
    panel->Hide(); panel->Reparent(m_impl->model_generation->GetParent());
}
void ModelGenerationFeatureHost::set_advanced_state_listener(ModelGenerationUIStateListener listener)
{ m_impl->advanced_state_listener = std::move(listener); }
ModelGenerationUIState ModelGenerationFeatureHost::advanced_snapshot() const
{ return m_impl->legacy_creation ? m_impl->legacy_creation->ui_state_snapshot() : ModelGenerationUIState{}; }
bool ModelGenerationFeatureHost::request_open_legacy_image_history(const std::string& job_id)
{ auto* panel = m_impl->legacy_panel(); return panel && panel->request_open_image_history(job_id); }
bool ModelGenerationFeatureHost::request_open_workbench_artifact(WorkbenchOpenRequest request)
{
    if (!request.identifies_asset() || request.origin != WorkbenchOpenRequest::Origin::Advanced ||
        request.sha256.size() != 64 || !request.generation) return false;
    return m_impl->open_asset(std::move(request));
}
bool ModelGenerationFeatureHost::request_open_generated_workbench(bool advanced, const ModelGenerationUIState& displayed)
{
    auto* creator = advanced ? m_impl->legacy_panel() : m_impl->model_generation;
    if (!creator || !displayed.model_ready || !creator->ui_state_snapshot().model_ready) return false;
    auto request = creator->generated_workbench_request(advanced
        ? WorkbenchOpenRequest::Origin::Legacy : WorkbenchOpenRequest::Origin::Smart);
    if (!request.matches_creator(displayed.model_asset_id, displayed.model_generation_session)) return false;
    return m_impl->open_asset(std::move(request));
}
bool ModelGenerationFeatureHost::workbench_returns_to_advanced() const
{ return m_impl->return_context == "advanced"; }

ModelWorkspaceFeatureHost& ModelGenerationFeatureHost::workspace_capabilities() { return *m_impl->capabilities; }

} // namespace Slic3r::GUI
