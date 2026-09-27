#pragma once

#include "ModelGenerationHost.hpp"

#include <functional>
#include <memory>
#include <string>

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
    ModelGenerationUIState snapshot() const;
    void set_state_listener(ModelGenerationUIStateListener listener);
    bool synchronize_input(const ModelGenerationUIInput& input);
    bool synchronize_options(const ModelGenerationUIOptions& options);
    bool request_generate_design();
    bool request_generate_model();
    bool request_stop();
    bool request_retry_service();
    bool request_restore_latest();
    bool request_restart();
    bool request_import();
    bool request_refresh_history();
    bool request_open_history(const std::string& job_id);
    void set_service_availability(bool available, const std::string& message);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
