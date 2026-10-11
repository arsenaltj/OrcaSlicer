#include "ColorMatchingFeatureHost.hpp"
#include "LocalPrintColorPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/dialog.h>
#include <wx/sizer.h>

namespace Slic3r::GUI {
ColorMatchingFeatureHost::ColorMatchingFeatureHost(Plater* workspace, std::function<void()> navigate, std::shared_ptr<ColorMatchingSession> session)
    : m_workspace(workspace), m_navigate(std::move(navigate)), m_session(session ? std::move(session) : std::make_shared<ColorMatchingSession>()) {}
ColorMatchingFeatureHost::~ColorMatchingFeatureHost() { shutdown(); }
bool ColorMatchingFeatureHost::open(wxWindow* parent, const AI::GeneratedModelArtifact& artifact)
{
    if (m_stopped || !m_workspace || !parent) return false;
    if (!m_dialog) {
        auto* dialog = new wxDialog(wxGetTopLevelParent(parent), wxID_ANY, _L("颜色匹配"),
            wxDefaultPosition, parent->FromDIP(wxSize(1080, 760)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        dialog->SetName("color_matching_capability"); dialog->SetSizer(new wxBoxSizer(wxVERTICAL));
        auto* view = new LocalPrintColorPanel(dialog, m_workspace,
            [dialog, navigate = m_navigate] { dialog->Hide(); if (navigate) navigate(); },
            [dialog] { dialog->Hide(); }, m_session);
        dialog->GetSizer()->Add(view, 1, wxEXPAND);
        // Closing preserves the session/draft so reopening the same source can
        // retain history. The feature owner explicitly shuts it down at exit.
        dialog->Bind(wxEVT_CLOSE_WINDOW, [dialog](wxCloseEvent&) { dialog->Hide(); });
        m_dialog = dialog; m_view = view;
    }
    if (!m_view || !m_view->open_artifact(artifact)) return false;
    m_dialog->Show(); m_dialog->Raise(); return true;
}
void ColorMatchingFeatureHost::shutdown()
{
    if (m_stopped) return; m_stopped = true;
    m_session->shutdown();
    if (m_view) m_view->shutdown();
    if (m_dialog) m_dialog->Destroy();
    m_view = nullptr; m_dialog = nullptr;
}
}
