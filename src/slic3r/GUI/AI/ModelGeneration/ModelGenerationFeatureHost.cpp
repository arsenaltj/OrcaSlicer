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
        model_generation->m_library_removals = workbench->m_library_removals;
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
                    } else if (command == ModelLibraryCommand::OpenImage) {
                        opened = !creation_context.locked() && model_generation->request_library_action(command, entry);
                    } else opened = workbench->request_library_action(command, entry);
                    if (opened && history_navigation && command == ModelLibraryCommand::OpenImage)
                        history_navigation(true);
                    if (opened && command == ModelLibraryCommand::Import) capabilities->refresh_assets();
                    return opened;
                }, [this] { workbench->request_library_local_import(); }});
        capabilities->capabilities().library->set_removal_state(workbench->m_library_removals);
        configure_library_publication(workbench);
        configure_library_publication(model_generation);
        workbench->m_library_removal_listener = [this](const ModelLibraryRemovalState::Event& event) {
            if (shutdown_requested) return;
            for (auto* panel : {model_generation}) {
                if (!panel) continue;
                panel->apply_library_removal_projection();
                if (event.phase == ModelLibraryRemovalState::Phase::Committed) panel->m_library_removed_thumbnails.clear();
                if (event.phase == ModelLibraryRemovalState::Phase::Finished || event.phase == ModelLibraryRemovalState::Phase::Failed)
                    panel->load_library_entries(true, true);
            }
            capabilities->apply_library_removals();
            if (event.phase == ModelLibraryRemovalState::Phase::Finished || event.phase == ModelLibraryRemovalState::Phase::Failed)
                capabilities->refresh_assets();
        };
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
            latest_state = state;
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

    void configure_library_publication(ModelGenerationPanel* panel)
    {
        panel->m_library_application = capabilities->capabilities().library;
        panel->m_library_commit_listener = [this](ModelLibraryCommit event) { publish_library_commit(std::move(event)); };
    }

    void publish_library_commit(ModelLibraryCommit event)
    {
        if (shutdown_requested) return;
        if (event.entry.job_id.empty()) {
            for (auto* panel : {workbench, model_generation}) if (panel) panel->load_library_entries(true, true);
            capabilities->refresh_assets();
            return;
        }
        const auto committed = capabilities->capabilities().library->commit(std::move(event));
        if (!committed) return;
        for (auto* panel : {workbench, model_generation})
            if (panel) panel->apply_library_commit(*committed);
        capabilities->apply_library_commit();
    }

    void configure_local_import(ModelGenerationPanel* panel)
    {
        panel->set_local_import_executor([this](const AI::ModelImportRequest& request,
            std::shared_ptr<WorkbenchImportSession> session, WorkbenchImportProgress progress,
            WorkbenchImportCompletion completion, std::function<bool()> current) {
            auto navigation_current = capture_import_navigation ? capture_import_navigation() : std::function<bool()>{};
            auto finished = [completion = std::move(completion), current, navigate = local_import_navigation,
                             navigation_current = std::move(navigation_current)](const AI::ModelImportResult& result) {
                if (result.imported() && current && current() && navigate &&
                    (!navigation_current || navigation_current())) navigate();
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
        for (auto* panel : {model_generation, workbench}) {
            if (!panel) continue;
            panel->set_ui_state_listener({});
            panel->set_workbench_listener({});
            panel->set_service_retry_handler({});
            panel->set_prepare_navigation_handler({});
            panel->set_color_matching_handler({});
            panel->set_workbench_import_handler({});
            panel->set_local_import_executor({});
            panel->set_project_color_handler({});
            panel->set_project_filament_edit_handler({});
            panel->set_project_filament_delete_handler({});
            panel->set_asset_open_handler({});
            panel->m_library_removal_listener = {};
            panel->m_library_commit_listener = {};
            panel->shutdown();
        }
    }

    bool open_asset(WorkbenchOpenRequest request)
    {
        if (shutdown_requested || !workbench || !workbench->library_asset_switch_allowed()) return false;
        if (request.origin == WorkbenchOpenRequest::Origin::Legacy) return false;
        if (request.origin == WorkbenchOpenRequest::Origin::Smart) {
            if (!model_generation || !request.matches_creator(model_generation->m_displayed_model_job_id,
                                                              model_generation->m_model_generation_session)) return false;
        }
        const auto context = request.return_context;
        auto creation_request = request;
        // History carries a return page, but does not represent the explicit
        // creator-to-workbench handoff that acquires the editing lock.
        if (request.origin == WorkbenchOpenRequest::Origin::History ||
            request.origin == WorkbenchOpenRequest::Origin::Library)
            creation_request.return_context.clear();
        if (!workbench->open_workbench_asset(std::move(request))) return false;
        creation_context.stage(creation_request);
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

    ModelGenerationUIState latest_state;
    ModelGenerationUIStateListener state_listener;

    std::unique_ptr<OrcaWorkspaceAdapter> workspace;
    NavigateAfterImportFn local_import_navigation;
    std::unique_ptr<ModelWorkspaceFeatureHost> capabilities;
    std::function<void(bool)> history_navigation;
    ModelGenerationPanel* model_generation { nullptr };
    ModelGenerationPanel* workbench { nullptr };
    std::string return_context;
    CreationWorkbenchContext creation_context;
    std::function<void()> creation_workflow_listener;
    std::function<std::function<bool()>()> capture_import_navigation;
    std::optional<std::string> previous_return_context;
    bool import_on_history_commit {false};
    std::string pending_import_asset;
    std::uint64_t pending_import_generation {0};
    bool shutdown_requested { false };
    std::shared_ptr<WorkbenchImportSession> import_session;
    std::shared_ptr<const char> preparation_lifetime {std::make_shared<const char>(0)};
};

ModelGenerationFeatureHost::ModelGenerationFeatureHost(wxWindow* parent, Plater* plater,
                                                       NavigateAfterImportFn navigate_after_import,
                                                       RetryServiceFn retry_service)
    : m_impl(std::make_unique<Impl>(parent, plater, std::move(navigate_after_import), std::move(retry_service)))
{}

bool ModelGenerationFeatureHost::settings_printer_switch_blocked() const
{
    return m_impl->workbench && m_impl->workbench->settings_printer_switch_blocked();
}

void ModelGenerationFeatureHost::settings_printer_switch_applied()
{
    if (m_impl->workbench)
        m_impl->workbench->synchronize_workbench_project_palette(false, true);
}

bool ModelGenerationFeatureHost::can_replace_asset() const
{ return m_impl->workbench && m_impl->workbench->library_asset_switch_allowed(); }

ModelGenerationFeatureHost::~ModelGenerationFeatureHost()
{
    shutdown();
}

void ModelGenerationFeatureHost::set_workbench_import_handler(WorkbenchImportHandler handler)
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

bool ModelGenerationFeatureHost::synchronize_input(const ModelGenerationUIInput& input, GenerationRejected rejected)
{
    if (creation_editing_locked()) return m_impl->latest_state.input == input;
    return m_impl->model_generation != nullptr && m_impl->model_generation->synchronize_ui_input(input, std::move(rejected));
}

bool ModelGenerationFeatureHost::synchronize_options(const ModelGenerationUIOptions& options, GenerationRejected rejected)
{
    if (creation_editing_locked()) return m_impl->latest_state.options == options;
    return m_impl->model_generation != nullptr && m_impl->model_generation->synchronize_ui_options(options, std::move(rejected));
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

bool ModelGenerationFeatureHost::request_generate_smart(GenerationRejected rejected)
{
    if (creation_editing_locked() || !can_replace_asset() || !m_impl->model_generation) {
        if (rejected) rejected(creation_editing_locked() ? "creation_read_only" : "workbench_guard");
        return false;
    }
    return m_impl->model_generation->request_generate_smart(std::move(rejected));
}

ModelGenerationSubmissionResult ModelGenerationFeatureHost::request_generate_design()
{
    return !creation_editing_locked() && can_replace_asset() && m_impl->model_generation ? m_impl->model_generation->request_generate_design() : ModelGenerationSubmissionResult::Rejected;
}

ModelGenerationSubmissionResult ModelGenerationFeatureHost::request_generate_model()
{
    return !creation_editing_locked() && can_replace_asset() && m_impl->model_generation ? m_impl->model_generation->request_generate_model() : ModelGenerationSubmissionResult::Rejected;
}

bool ModelGenerationFeatureHost::request_retry_model()
{
    if (creation_editing_locked()) return false;
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
    if (creation_editing_locked()) return false;
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_restore_latest();
}

void ModelGenerationFeatureHost::set_smart_creation_presented(bool presented)
{
    if (m_impl->model_generation) m_impl->model_generation->set_smart_creation_presented(presented);
}

bool ModelGenerationFeatureHost::request_restart()
{
    if (creation_editing_locked()) return false;
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_restart();
}

bool ModelGenerationFeatureHost::request_import()
{
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_import();
}

void ModelGenerationFeatureHost::publish_library_commit(ModelLibraryCommit event)
{
    m_impl->publish_library_commit(std::move(event));
}

bool ModelGenerationFeatureHost::request_refresh_history()
{
    m_impl->capabilities->refresh_assets();
    if (m_impl->model_generation) m_impl->model_generation->request_refresh_history();
    return m_impl->workbench->request_refresh_history();
}

bool ModelGenerationFeatureHost::request_open_image_history(const std::string& job_id)
{
    if (creation_editing_locked()) return false;
    return m_impl->model_generation != nullptr && m_impl->model_generation->request_open_image_history(job_id);
}

bool ModelGenerationFeatureHost::request_open_history(const std::string& job_id)
{
    if (creation_editing_locked()) return false;
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

void ModelGenerationFeatureHost::set_workbench_return_to_design_handler(std::function<void()> handler)
{
    m_impl->workbench->set_workbench_return_to_design_handler(std::move(handler));
}

bool ModelGenerationFeatureHost::request_open_workbench() { return m_impl->workbench->request_open_workbench(); }
void ModelGenerationFeatureHost::present_existing_workbench()
{
    auto* panel = m_impl->workbench;
    if (!panel || m_impl->shutdown_requested) return;
    panel->initialize_for_shell_host();
    if (!panel->m_finishing_workbench) panel->set_finishing_workbench(true);
}
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
    m_impl->creation_context.finish(accepted);
    if (!accepted) {
        if (m_impl->previous_return_context) m_impl->return_context = *m_impl->previous_return_context;
        m_impl->import_on_history_commit = false;
        m_impl->pending_import_asset.clear();
    }
    m_impl->previous_return_context.reset();
    m_impl->workbench->finish_history_model_open(accepted, std::move(complete));
    if (m_impl->creation_workflow_listener) m_impl->creation_workflow_listener();
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
    if (m_impl->model_generation != nullptr)
        m_impl->model_generation->set_service_availability(available, generation_available, message);
    m_impl->workbench->set_service_availability(available, false, message);
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

bool ModelGenerationFeatureHost::request_open_workbench_artifact(WorkbenchOpenRequest request)
{
    if (!request.identifies_asset() ||
        (request.origin != WorkbenchOpenRequest::Origin::Advanced && request.origin != WorkbenchOpenRequest::Origin::Smart) ||
        request.sha256.size() != 64 || !request.generation) return false;
    return m_impl->open_asset(std::move(request));
}
std::optional<WorkbenchOpenRequest> ModelGenerationFeatureHost::snapshot_generated_artifact() const
{
    if (!m_impl->model_generation || !m_impl->model_generation->ui_state_snapshot().model_ready)
        return std::nullopt;
    auto request = m_impl->model_generation->generated_workbench_request(WorkbenchOpenRequest::Origin::Smart);
    if (!request.identifies_asset() || request.sha256.size() != 64) return std::nullopt;
    return request;
}
bool ModelGenerationFeatureHost::workbench_returns_to_advanced() const
{ return m_impl->creation_context.anchor() ? m_impl->creation_context.anchor()->advanced : m_impl->return_context == "advanced"; }

bool ModelGenerationFeatureHost::creation_editing_locked() const
{ return m_impl->creation_context.locked(); }

bool ModelGenerationFeatureHost::commit_current_creation(const WorkbenchOpenRequest& request)
{
    const auto current = workbench_snapshot();
    if (request.origin == WorkbenchOpenRequest::Origin::History ||
        request.origin == WorkbenchOpenRequest::Origin::Library || !request.identifies_asset() ||
        current.asset_id != request.entry.job_id || current.model_path.empty()) return false;
    m_impl->creation_context.stage(request);
    m_impl->creation_context.finish(true);
    if (m_impl->creation_workflow_listener) m_impl->creation_workflow_listener();
    return true;
}

void ModelGenerationFeatureHost::restore_creation_editing()
{
    m_impl->creation_context.restore_editing();
    if (m_impl->creation_workflow_listener) m_impl->creation_workflow_listener();
}

void ModelGenerationFeatureHost::set_creation_workflow_listener(std::function<void()> listener)
{ m_impl->creation_workflow_listener = std::move(listener); }

void ModelGenerationFeatureHost::set_import_navigation_guard(std::function<std::function<bool()>()> capture)
{ m_impl->capture_import_navigation = std::move(capture); }

ModelWorkspaceFeatureHost& ModelGenerationFeatureHost::workspace_capabilities() { return *m_impl->capabilities; }

} // namespace Slic3r::GUI
