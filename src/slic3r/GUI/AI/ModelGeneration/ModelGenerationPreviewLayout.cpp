#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/AI/ColorMatching/LocalPrintColorPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/simplebook.h>
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
                close_workbench_color_matching(false);
                if (m_prepare_navigation) m_prepare_navigation();
            },
            [this] { close_workbench_color_matching(); });
        refresh_ai_appearance(m_workbench_color_matching);
        m_preview_book->AddPage(m_workbench_color_matching, _L("颜色匹配"), false);
    }
    if (!m_workbench_color_matching->open_artifact(artifact)) return;
    m_preview_book->SetSelection(m_preview_book->FindPage(m_workbench_color_matching));
    m_preview_book->GetParent()->Layout();
}

void ModelGenerationPanel::close_workbench_color_matching(bool restore_entry)
{
    if (!m_preview_book || !m_workbench_color_matching) return;
    if (m_preview_book->GetSelection() != m_preview_book->FindPage(m_workbench_color_matching)) return;
    m_preview_book->SetSelection(0);
    m_preview_book->GetParent()->Layout();
    const auto entry = m_color_matching_entry;
    const auto sequence = m_color_matching_entry_sequence;
    m_color_matching_entry = nullptr;
    if (!restore_entry) return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    // Restore keyboard navigation only after the original page is visible.
    // A new route/model or destroyed/disabled entry must not steal focus.
    CallAfter([weak, entry, sequence]() {
        if (!weak || weak->m_shutdown || weak->m_busy || sequence != weak->m_sequence ||
            !weak->m_preview_book || weak->m_preview_book->GetSelection() != 0 ||
            !entry || !weak->IsDescendant(entry.get()) ||
            !entry->IsShownOnScreen() || !entry->IsEnabled()) return;
        entry->SetFocus();
    });
}

void ModelGenerationPanel::show_model_comparison()
{
    // An async restore may finish after the user has explicitly opened Image.
    // Make the model available without stealing that navigation choice.
    const bool preserve_image = m_preserve_image_on_model_ready &&
        m_workspace_view == ModelGenerationPresentation::WorkspaceView::Image;
    m_preserve_image_on_model_ready = false;
    if (preserve_image) {
        refresh_comparison_layout(false);
        return;
    }
    m_result_view = m_model_preview_ready ? ModelGenerationPresentation::WorkspaceView::Model : ModelGenerationPresentation::WorkspaceView::Image;
    if (m_workspace_view != ModelGenerationPresentation::WorkspaceView::Library) m_workspace_view = m_result_view;
    if (!m_expand_images) return;
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
    // Compact overview gives the real GL canvas the available height. Image
    // comparison remains one navigation action away, with its zoom preserved.
    const bool show_images = !m_finishing_workbench && (!show_model || !stacked);
    const bool visibility_changed = card->IsShown() != show_model || m_preview_area->IsShown() != show_images;
    const bool layout_changed = row->GetOrientation() != orientation;
    card->Show(show_model);
    m_preview_area->Show(show_images);
    m_model_preview_message->Show(show_model && show_images);
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
