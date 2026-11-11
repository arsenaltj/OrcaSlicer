#pragma once
#include <wx/panel.h>
#include <functional>
#include <memory>

namespace Slic3r::GUI {
class AdvancedCreationFeatureHost;
struct ModelGenerationUIState;
class Plater;
class AdvancedCreationWorkspace final : public wxPanel {
public:
    AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
                              std::function<void()> show_smart, Plater* plater,
                              std::function<void()> after_import,
                              std::function<void(const ModelGenerationUIState&)> model_state_changed,
                              std::function<void(const ModelGenerationUIState&)> show_model_route);
    ~AdvancedCreationWorkspace() override;
    void render();
    void set_service_available(bool available);
    bool navigation_allowed();
    bool request_open_model_workbench();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
