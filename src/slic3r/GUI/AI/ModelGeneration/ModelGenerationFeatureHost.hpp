#pragma once

#include "ModelGenerationHost.hpp"
#include "PostGenerationWorkbenchState.hpp"
#include "WorkbenchImportSession.hpp"

#include <functional>
#include <memory>
#include <string>
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"

class wxWindow;

namespace Slic3r::GUI {

class ModelGenerationPanel;
class Plater;

class ModelGenerationFeatureHost final
{
public:
    using NavigateAfterImportFn = std::function<void()>;
    using RetryServiceFn = std::function<void()>;

    ModelGenerationFeatureHost(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import,
                               RetryServiceFn retry_service);
    ~ModelGenerationFeatureHost();

    ModelGenerationFeatureHost(const ModelGenerationFeatureHost&) = delete;
    ModelGenerationFeatureHost& operator=(const ModelGenerationFeatureHost&) = delete;

    wxWindow* panel() const;
    void initialize_for_shell();
    void mount_assets(wxWindow* parent);
    void unmount_assets();
    void set_history_navigation_handler(std::function<void(bool)> handler);
    ModelGenerationUIState snapshot() const;
    void set_state_listener(ModelGenerationUIStateListener listener);
    bool synchronize_input(const ModelGenerationUIInput& input);
    bool synchronize_options(const ModelGenerationUIOptions& options);
    bool request_generate_design();
    bool request_generate_model();
    bool request_retry_model();
    bool request_stop();
    bool request_retry_service();
    bool request_restore_latest();
    bool request_restart();
    bool request_import();
    bool request_refresh_history();
    bool request_open_history(const std::string& job_id);
    bool request_open_image_history(const std::string& job_id);
    PostGenerationWorkbenchState workbench_snapshot() const;
    void set_workbench_listener(PostGenerationWorkbenchListener listener);
    void set_workbench_results_handler(std::function<void()> handler);
    void mount_workbench(wxWindow* parent);
    void unmount_workbench();
    void set_color_matching_handler(std::function<void(const AI::GeneratedModelArtifact&)> handler);
    void set_workbench_import_handler(std::function<void(const AI::ModelImportRequest&)> handler);
    AI::ModelImportResult import_workbench_model(const AI::ModelImportRequest& request);
    bool import_workbench_model_async(const AI::ModelImportRequest& request,
        std::shared_ptr<WorkbenchImportSession> session, WorkbenchImportProgress progress,
        WorkbenchImportCompletion completion);
    bool request_open_workbench();
    bool request_enter_beauty();
    bool request_return_overview();
    bool request_workbench_color_matching();
    bool request_enable_portrait(bool enabled);
    void set_service_availability(bool available, const std::string& message);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
