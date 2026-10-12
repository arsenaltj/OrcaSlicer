#include "ModelWorkspaceFeatureHost.hpp"
#include "ModelLibraryPanel.hpp"
#include <wx/sizer.h>

namespace Slic3r::GUI {
ModelWorkspaceFeatureHost::ModelWorkspaceFeatureHost(boost::filesystem::path root, Plater* workspace,
    std::function<void()> navigate, ModelLibraryApplication::Ports ports)
    : m_root(std::move(root)), m_ports(std::move(ports)), m_capabilities(m_root), m_colors(workspace, std::move(navigate), m_capabilities.colors) {}
ModelWorkspaceFeatureHost::~ModelWorkspaceFeatureHost() { shutdown(); }
void ModelWorkspaceFeatureHost::mount_assets(wxWindow* parent)
{
    if (m_stopped || !parent) return;
    if (!parent->GetSizer()) parent->SetSizer(new wxBoxSizer(wxVERTICAL));
    if (!m_library) {
        auto ports = m_ports;
        const auto execute = ports.execute;
        ports.execute = [this, execute](ModelLibraryCommand command, const ModelLibraryEntry& entry) {
            if (m_stopped) return false;
            if (execute) return execute(command, entry);
            if (!m_library || (command != ModelLibraryCommand::OpenModel && command != ModelLibraryCommand::MatchColors)) return false;
            AI::GeneratedModelArtifact artifact; artifact.local_path = entry.model_path; artifact.job_id = entry.job_id;
            artifact.generation_palette = entry.palette;
            return open_colors(m_library->GetParent(), artifact);
        };
        m_capabilities.library->set_ports(std::move(ports));
        m_library = new ModelLibraryPanel(parent, m_capabilities.library);
        parent->GetSizer()->Add(m_library.get(), 1, wxEXPAND);
    } else if (m_library->GetParent() != parent) {
        if (auto* sizer = m_library->GetContainingSizer()) sizer->Detach(m_library.get());
        m_library->Reparent(parent); parent->GetSizer()->Add(m_library.get(), 1, wxEXPAND);
    }
    m_library->Show(); parent->Layout();
}
void ModelWorkspaceFeatureHost::unmount_assets() { if (m_library) m_library->Hide(); }
void ModelWorkspaceFeatureHost::refresh_assets() { if (!m_stopped && m_library) m_library->refresh_assets(); }
bool ModelWorkspaceFeatureHost::open_colors(wxWindow* parent, const AI::GeneratedModelArtifact& artifact)
{ return !m_stopped && m_colors.open(parent, artifact); }
void ModelWorkspaceFeatureHost::shutdown()
{
    if (m_stopped) return; m_stopped = true;
    m_capabilities.library->set_ports({}); m_ports = {};
    if (m_library) m_library->shutdown();
    m_colors.shutdown();
}
}
