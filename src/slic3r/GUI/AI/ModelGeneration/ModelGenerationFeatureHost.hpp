#pragma once

#include <functional>
#include <memory>
#include <string>
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "ModelGenerationPresentation.hpp"

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
    void navigate(ModelGenerationPresentation::WorkspaceAction action);
    ModelGenerationPresentation::WorkspaceView workspace_view() const;
    bool has_model() const;
    void set_workspace_changed_handler(std::function<void()> handler);
    void set_service_availability(bool available, const std::string& message);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
