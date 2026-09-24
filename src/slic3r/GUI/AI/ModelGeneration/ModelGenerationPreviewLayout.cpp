#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ColorMatching/LocalPrintColorPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/tglbtn.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::show_workbench_color_matching(const AI::GeneratedModelArtifact& artifact, Plater* plater)
{
    if (m_shutdown || !m_preview_book || !plater) return;
    if (!m_workbench_color_matching) {
        m_workbench_color_matching = new LocalPrintColorPanel(
            m_preview_book, plater,
            [this] {
                close_workbench_color_matching();
                if (m_prepare_navigation) m_prepare_navigation();
            },
            [this] { close_workbench_color_matching(); });
        m_preview_book->AddPage(m_workbench_color_matching, _L("颜色匹配"), false);
    }
    if (!m_workbench_color_matching->open_artifact(artifact)) return;
    m_preview_book->SetSelection(m_preview_book->FindPage(m_workbench_color_matching));
    m_preview_book->GetParent()->Layout();
}

void ModelGenerationPanel::close_workbench_color_matching()
{
    if (!m_preview_book || !m_workbench_color_matching) return;
    if (m_preview_book->GetSelection() == m_preview_book->FindPage(m_workbench_color_matching))
        m_preview_book->SetSelection(0);
}

void ModelGenerationPanel::show_model_comparison()
{
    m_expand_images->SetValue(false);
    m_expand_images->SetLabel(_L("展开图片"));
    refresh_comparison_layout(true);
}

void ModelGenerationPanel::refresh_comparison_layout(bool reset_scroll)
{
    if (m_workbench_color_matching && m_workbench_color_matching->IsShown()) return;
    if (m_updating_comparison_layout || !m_model_page || !m_model_preview || !m_expand_images)
        return;
    auto* row = dynamic_cast<wxBoxSizer*>(m_comparison_panel->GetSizer());
    if (!row) return;
    m_updating_comparison_layout = true;
    wxWindow* card = m_model_preview->GetParent();
    const bool show_model = m_finishing_workbench || (m_model_preview_ready && !m_expand_images->GetValue());
    const bool stacked = !m_finishing_workbench && m_model_page->GetClientSize().x < FromDIP(920);
    const int orientation = stacked ? wxVERTICAL : wxHORIZONTAL;
    const bool visibility_changed = card->IsShown() != show_model;
    const bool layout_changed = row->GetOrientation() != orientation;
    card->Show(show_model);
    m_expand_images->Enable(m_model_preview_ready && !m_finishing_workbench);
    if (layout_changed) {
        row->SetOrientation(orientation);
        row->GetItem(card)->SetFlag(wxEXPAND | (stacked ? wxBOTTOM : wxRIGHT));
    }
    // wxScrolledWindow lays out against its virtual size. Reset that extent to
    // the viewport before fitting the children, otherwise a previously tall
    // comparison keeps stretching both the canvas and the image below view.
    if (visibility_changed || layout_changed || reset_scroll ||
        m_model_page->GetVirtualSize() != m_model_page->GetClientSize()) {
        m_model_page->SetVirtualSize(m_model_page->GetClientSize());
        m_comparison_panel->Layout();
        m_model_page->Layout();
        m_model_page->FitInside();
        if (visibility_changed || reset_scroll) m_model_page->Scroll(0, 0);
        update_preview_view(reset_scroll || visibility_changed);
    }
    if (reset_scroll && show_model) {
        // Record the actual minimum-size chain for diagnosing clipped previews.
        // No model content, paths or provider configuration enter this log.
        auto record_size = [](const char* name, wxWindow* window) {
            const auto size = window->GetClientSize();
            const auto minimum = window->GetMinSize();
            const auto best = window->GetBestSize();
            const auto children = window->GetSizer() ? window->GetSizer()->CalcMin() : wxSize(-1, -1);
            BOOST_LOG_TRIVIAL(info) << "AI comparison layout: " << name
                << " client=" << size.x << 'x' << size.y
                << " minimum=" << minimum.x << 'x' << minimum.y
                << " best=" << best.x << 'x' << best.y
                << " children_min=" << children.x << 'x' << children.y;
        };
        record_size("page", m_model_page);
        record_size("comparison", m_comparison_panel);
        record_size("model_card", card);
        record_size("model_preview", m_model_preview);
        record_size("image", m_preview_area);
    }
    m_updating_comparison_layout = false;
}

} // namespace Slic3r::GUI
