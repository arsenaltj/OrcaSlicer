#pragma once
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include <wx/panel.h>
#include <functional>
#include <memory>

namespace Slic3r::GUI {
class Plater;
// Offline color tools embedded in the existing finishing workbench.
class LocalPrintColorPanel final : public wxPanel {
public:
    LocalPrintColorPanel(wxWindow* parent, Plater* plater, std::function<void()> prepare_navigation,
                        std::function<void()> back_to_workbench = {});
    ~LocalPrintColorPanel() override;
    bool open_artifact(const AI::GeneratedModelArtifact& artifact);
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
