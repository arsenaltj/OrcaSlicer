#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationInputStyle.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::mount_assets(wxWindow* parent)
{
    initialize_for_shell_host();
    if (!parent || !m_library_scroller) return;
    auto* surface = m_library_scroller->GetParent();
    if (surface->GetParent() == parent) return;
    m_assets_home_parent = surface->GetParent();
    if (auto* book = dynamic_cast<wxNotebook*>(m_assets_home_parent)) {
        const int index = book->FindPage(surface);
        if (index != wxNOT_FOUND) book->RemovePage(index);
    }
    if (auto* sizer = surface->GetContainingSizer()) sizer->Detach(surface);
    surface->Reparent(parent);
    parent->GetSizer()->Add(surface, 1, wxEXPAND);
    surface->Show();
    m_library_appearance_handler = [](wxWindow* card) {
        ModelGenerationInputStyle::apply(card, true);
    };
    ModelGenerationInputStyle::apply(surface, true);
    parent->Layout();
}

void ModelGenerationPanel::unmount_assets()
{
    if (!m_assets_home_parent || !m_library_scroller) return;
    auto* surface = m_library_scroller->GetParent();
    if (auto* sizer = surface->GetContainingSizer()) sizer->Detach(surface);
    surface->Reparent(m_assets_home_parent);
    if (auto* book = dynamic_cast<wxNotebook*>(m_assets_home_parent))
        book->AddPage(surface, _L("历史资产"), false);
    surface->Hide();
    m_assets_home_parent = nullptr;
    m_library_appearance_handler = {};
}

} // namespace Slic3r::GUI
