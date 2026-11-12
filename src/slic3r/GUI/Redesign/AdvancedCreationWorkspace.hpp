#pragma once
#include <wx/panel.h>
#include <functional>
#include <memory>

namespace Slic3r::GUI {
class AdvancedCreationFeatureHost;
class AdvancedCreationWorkspace final : public wxPanel {
public:
    AdvancedCreationWorkspace(wxWindow* parent, AdvancedCreationFeatureHost& host,
                              std::function<void()> show_smart, std::function<bool()> can_replace_asset);
    ~AdvancedCreationWorkspace() override;
    void render();
    void set_service_available(bool available);
    bool navigation_allowed();
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
