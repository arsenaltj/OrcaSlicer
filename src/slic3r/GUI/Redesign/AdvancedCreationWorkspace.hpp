#pragma once
#include <wx/panel.h>
#include <functional>
#include <memory>
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
                              std::function<void(const AdvancedCreation::ModelJobEvent&)> model_job_event);
    ~AdvancedCreationWorkspace() override;
    void render();
    void set_presented(bool visible);
    void set_service_available(bool available);
    bool navigation_allowed();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
