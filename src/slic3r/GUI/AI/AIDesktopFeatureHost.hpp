#pragma once

#include <functional>
#include <memory>
#include "ModelGeneration/ModelGenerationPresentation.hpp"

class wxWindow;
class wxString;
class wxColour;

namespace Slic3r::GUI {

class Plater;

enum class AIWorkflowStatus
{
    Waiting,
    Running,
    Success,
    Warning,
    Failed
};

// Shared desktop presentation for import, preparation and slicing progress.
void describe_ai_workflow_status(AIWorkflowStatus status, wxString& label, wxColour& colour);

class AIDesktopFeatureHost final
{
public:
    using NavigateAfterImportFn = std::function<void()>;
    using SmartSlicingAvailableFn = std::function<void()>;

    AIDesktopFeatureHost(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import,
                         SmartSlicingAvailableFn smart_slicing_available);
    ~AIDesktopFeatureHost();

    AIDesktopFeatureHost(const AIDesktopFeatureHost&) = delete;
    AIDesktopFeatureHost& operator=(const AIDesktopFeatureHost&) = delete;

    wxWindow* model_generation_panel() const;
    void navigate_generation(ModelGenerationPresentation::WorkspaceAction action);
    ModelGenerationPresentation::WorkspaceView generation_view() const;
    bool has_generation_model() const;
    void set_workspace_changed_handler(std::function<void()> handler);
    void start();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
