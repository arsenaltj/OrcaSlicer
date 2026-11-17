#pragma once
#include <wx/panel.h>
#include <functional>
#include <memory>
#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/WorkbenchOpenRequest.hpp"
#include <optional>
#include "../AI/ModelGeneration/AdvancedCreationTypes.hpp"

namespace Slic3r::GUI {
class AdvancedCreationFeatureHost;
struct ModelGenerationUIState;
class ModelGenerationFeatureHost;
class AdvancedCreationWorkspace final : public wxPanel {
public:
    AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
                              std::function<void()> show_smart, ModelGenerationFeatureHost& shared_models,
                              std::function<void(const ModelGenerationUIState&)> model_state_changed,
                              std::function<void(const AdvancedCreation::ModelJobEvent&)> model_job_event,
                              std::function<void()> open_workbench, std::function<void()> completed);
    ~AdvancedCreationWorkspace() override;
    void render();
    void set_presented(bool visible);
    void set_service_available(bool available);
    bool navigation_allowed();
    void set_creation_return_handler(std::function<void()> action, std::function<bool()> allowed);
    void shutdown();
    void apply_model_state(const ModelGenerationUIState& state, bool new_generation);
    std::optional<WorkbenchOpenRequest> displayed_artifact() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
