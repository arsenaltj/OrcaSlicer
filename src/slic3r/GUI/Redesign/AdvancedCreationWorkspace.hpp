#pragma once
#include <wx/panel.h>
#include <functional>
#include <memory>
#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/WorkbenchOpenRequest.hpp"
#include <optional>

namespace Slic3r::GUI {
class AdvancedCreationFeatureHost;
class AdvancedCreationWorkspace final : public wxPanel {
public:
    AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
                              std::function<void()> show_smart, std::function<bool()> can_replace_asset,
                              std::function<void()> open_workbench, std::function<void()> completed);
    ~AdvancedCreationWorkspace() override;
    void render();
    void set_service_available(bool available);
    bool navigation_allowed();
    void shutdown();
    void apply_model_state(const ModelGenerationUIState& state, bool new_generation);
    void set_presented(bool active);
    std::optional<WorkbenchOpenRequest> displayed_artifact() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
