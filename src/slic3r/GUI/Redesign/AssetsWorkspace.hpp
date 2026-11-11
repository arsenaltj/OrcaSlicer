#pragma once
#include <wx/panel.h>
#include <memory>
#include <nlohmann/json.hpp>
class wxWebView;
namespace Slic3r::GUI {
class LibraryWorkspaceService;
class ModelLibraryApplication;
class AssetsWorkspace final : public wxPanel {
public:
    explicit AssetsWorkspace(wxWindow*);
    ~AssetsWorkspace() override;
    void attach(std::shared_ptr<LibraryWorkspaceService>, std::shared_ptr<ModelLibraryApplication>);
    void set_project_available(bool);
private:
    void load();
    void send(const nlohmann::json&);
    std::shared_ptr<LibraryWorkspaceService> m_service;
    std::shared_ptr<ModelLibraryApplication> m_local;
    wxWebView* m_web {nullptr};
    wxWindow* m_fallback {nullptr};
    std::string m_nonce;
    bool m_loaded {false}, m_project {false};
};
}
