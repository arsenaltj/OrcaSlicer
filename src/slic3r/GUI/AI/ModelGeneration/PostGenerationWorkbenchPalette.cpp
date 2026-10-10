#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/sizer.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::synchronize_workbench_project_palette(bool activate_preview)
{
    if (m_shutdown || !m_workbench_project_colors) return;
    const auto palette = m_palette_provider.printable_palette();
    const auto channels = m_project_channels_provider ? m_project_channels_provider() : palette.physical_channels;
    const bool changed = palette.project_colors != m_workbench_project_palette.project_colors ||
        channels.size() != m_workbench_project_channels.size() ||
        !std::equal(channels.begin(), channels.end(), m_workbench_project_channels.begin(),
            [](const auto& left, const auto& right) { return left.slot == right.slot && left.display_color == right.display_color; }) ||
        palette.valid_slots != m_workbench_project_palette.valid_slots ||
        palette.material_fingerprint != m_workbench_project_palette.material_fingerprint ||
        palette.process_fingerprint != m_workbench_project_palette.process_fingerprint;
    if (!changed && !activate_preview) return;
    m_workbench_project_palette = palette;
    m_workbench_project_channels = channels;
    if (m_model_preview_ready) m_model_preview->synchronize_project_colors(activate_preview);
    auto* contents = m_workbench_project_colors->GetSizer();
    contents->Clear(true);
    const bool enabled = workbench_snapshot().can_edit_project_colors;
    for (const auto& channel : channels) {
        const wxColour color(wxString::FromUTF8(channel.display_color));
        if (!color.IsOk()) continue;
        auto* swatch = new WorkbenchButton(m_workbench_project_colors,
            wxString::Format("%u", unsigned(channel.slot + 1)));
        swatch->SetMinSize(FromDIP(wxSize(40, 32)));
        swatch->SetCornerRadius(FromDIP(4));
        swatch->SetBackgroundColor(StateColor(color));
        const double luminance = 0.2126 * color.Red() + 0.7152 * color.Green() + 0.0722 * color.Blue();
        swatch->SetTextColor(StateColor(luminance > 140 ? wxColour(22, 22, 25) : wxColour(245, 245, 245)));
        swatch->SetName(wxString::Format("project_filament_color_%u", unsigned(channel.slot + 1)));
        swatch->SetToolTip(wxString::Format(_L("工程耗材 %u · "), unsigned(channel.slot + 1)) +
            wxString::FromUTF8(channel.display_color) + "\n" +
            _L("修改此槽位颜色会影响已有对象，可单独撤销；取消模型导入仍保留改色。"));
        swatch->Enable(enabled);
        const wxWeakRef<ModelGenerationPanel> weak(this);
        swatch->Bind(wxEVT_BUTTON, [weak, slot = channel.slot](wxCommandEvent&) {
            // The picker runs a nested event loop; palette refresh can replace this swatch.
            if (!weak || weak->m_shutdown) return;
            weak->CallAfter([weak, slot] {
                if (weak && !weak->m_shutdown && weak->m_project_color_edit && weak->workbench_snapshot().can_edit_project_colors)
                    weak->m_project_color_edit(slot);
            });
        });
        contents->Add(swatch);
    }
    m_workbench_project_colors->InvalidateBestSize();
    m_workbench_project_colors->Layout();
    m_workbench_settings_scroll->Layout();
    m_workbench_settings_scroll->FitInside();
    refresh_post_generation_workbench();
    publish_workbench_state();
}

} // namespace Slic3r::GUI
