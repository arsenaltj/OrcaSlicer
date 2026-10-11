#pragma once
#include "ModelLibraryApplication.hpp"
#include "ModelWorkspaceCapabilities.hpp"
#include "slic3r/GUI/AI/ColorMatching/ColorMatchingFeatureHost.hpp"
#include <wx/weakref.h>

class wxWindow;
namespace Slic3r::GUI {
class ModelLibraryPanel;
// The minimal GUI entry point: library + color matching, with optional design /
// beauty / import action ports. It never constructs ModelGenerationPanel.
class ModelWorkspaceFeatureHost final {
public:
    ModelWorkspaceFeatureHost(boost::filesystem::path root, Plater* workspace, std::function<void()> navigate,
                              ModelLibraryApplication::Ports ports = {});
    ~ModelWorkspaceFeatureHost();
    void mount_assets(wxWindow* parent);
    void unmount_assets();
    void refresh_assets();
    bool open_colors(wxWindow* parent, const AI::GeneratedModelArtifact& artifact);
    std::shared_ptr<ColorMatchingSession> color_session() const { return m_colors.session(); }
    ModelWorkspaceCapabilities& capabilities() { return m_capabilities; }
    void shutdown();
private:
    boost::filesystem::path m_root;
    ModelLibraryApplication::Ports m_ports;
    ModelWorkspaceCapabilities m_capabilities;
    ColorMatchingFeatureHost m_colors;
    wxWeakRef<ModelLibraryPanel> m_library;
    bool m_stopped {false};
};
}
