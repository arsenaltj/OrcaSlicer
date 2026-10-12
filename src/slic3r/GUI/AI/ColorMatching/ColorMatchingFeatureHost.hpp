#pragma once
#include "ColorMatchingSession.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include <wx/weakref.h>
#include <functional>

class wxWindow;
class wxDialog;
namespace Slic3r::GUI {
class Plater;
class LocalPrintColorPanel;
// Native UI adapter for color matching. It can be embedded or opened by any
// frontend and never asks the model-generation panel to host its session.
class ColorMatchingFeatureHost final {
public:
    ColorMatchingFeatureHost(Plater* workspace, std::function<void()> navigate,
                             std::shared_ptr<ColorMatchingSession> session = {});
    ~ColorMatchingFeatureHost();
    bool open(wxWindow* parent, const AI::GeneratedModelArtifact& artifact);
    void shutdown();
    std::shared_ptr<ColorMatchingSession> session() const { return m_session; }
private:
    Plater* m_workspace;
    std::function<void()> m_navigate;
    std::shared_ptr<ColorMatchingSession> m_session;
    wxWeakRef<wxDialog> m_dialog;
    wxWeakRef<LocalPrintColorPanel> m_view;
    bool m_stopped {false};
};
}
