#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/sizer.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::apply_workbench_printer_filaments()
{
    if (m_shutdown || !m_model_preview || !workbench_snapshot().can_apply_printer_filaments) return;
    auto before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    if (!m_model_preview->apply_project_filament_palette()) {
        restore_beauty_candidate(*before);
        return;
    }
    m_beauty_manual_color_dirty = true;
    if (m_beauty_controls) m_beauty_controls->set_dirty(true);
    record_beauty_candidate(BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor, before);
    refresh_controls();
    update_finishing_selection();
    publish_workbench_state();
}

void ModelGenerationPanel::synchronize_workbench_project_palette(bool activate_preview)
{
    if (m_shutdown || !m_workbench_project_colors) return;
    const auto snapshot = workbench_snapshot();
    const auto& state = snapshot.printer_filaments;
    const bool changed = state.signature != m_workbench_filament_signature;
    if (m_model_preview_ready) m_model_preview->synchronize_project_colors(activate_preview);
    if (!changed && !activate_preview) return;
    m_workbench_filament_signature = state.signature;
    m_workbench_project_palette = snapshot.project_palette;
    m_workbench_project_channels = snapshot.project_channels;
    auto* contents = m_workbench_project_colors->GetSizer();
    contents->Clear(true);
    for (const auto& slot : state.slots) {
        auto* row = new wxPanel(m_workbench_project_colors);
        row->SetBackgroundColour(m_workbench_project_colors->GetBackgroundColour());
        row->SetSizer(new wxBoxSizer(wxHORIZONTAL));
        auto* swatch = new WorkbenchButton(row, wxString::Format("%u", unsigned(slot.slot + 1)));
        swatch->SetMinSize(FromDIP(wxSize(38, 40))); swatch->SetCornerRadius(FromDIP(4));
        wxColour color(wxString::FromUTF8(slot.color));
        if (!color.IsOk()) color = wxColour(65,65,70);
        swatch->SetBackgroundColor(StateColor(color));
        const double luminance = 0.2126*color.Red()+0.7152*color.Green()+0.0722*color.Blue();
        swatch->SetTextColor(StateColor(luminance>140?wxColour(22,22,25):wxColour(245,245,245)));
        swatch->SetName(wxString::Format("project_filament_color_%u",unsigned(slot.slot+1)));
        const wxString summary = slot.configured ? wxString::FromUTF8(slot.preset)+"\n"+
            wxString::FromUTF8(slot.material)+" · "+wxString::FromUTF8(slot.color)+(slot.compatible?_L(" · 已配置"):_L(" · 不兼容")) : _L("待配置真实耗材及颜色");
        swatch->SetToolTip(summary+"\n"+_L("点击配置真实耗材；取消不改变工程"));
        auto* label = new wxStaticText(row, wxID_ANY, summary);
        label->SetForegroundColour(wxColour(190,190,196)); label->SetMinSize(wxSize(1,-1));
        label->Wrap(FromDIP(155));
        row->GetSizer()->Add(swatch,0,wxRIGHT|wxALIGN_CENTER_VERTICAL,FromDIP(7));
        row->GetSizer()->Add(label,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(6));
        auto* remove = new WorkbenchButton(row,_L("删除"));
        remove->SetMinSize(FromDIP(wxSize(45,32))); remove->Enable(slot.removable);
        remove->SetToolTip(slot.removable?_L("先选择兼容的替代耗材，再删除此槽位"):_L("基础喷嘴槽位不能删除"));
        row->GetSizer()->Add(remove,0,wxALIGN_CENTER_VERTICAL);
        const wxWeakRef<ModelGenerationPanel> weak(this);
        auto edit = [weak,id=slot.slot] {
            if (!weak || weak->m_shutdown) return;
            weak->CallAfter([weak,id] {
                if (weak && !weak->m_shutdown && weak->m_project_color_edit && weak->workbench_snapshot().can_edit_project_colors)
                    weak->m_project_color_edit(id);
            });
        };
        swatch->Bind(wxEVT_BUTTON,[edit](wxCommandEvent&){edit();});
        remove->Bind(wxEVT_BUTTON,[weak,id=slot.slot](wxCommandEvent&){
            if(!weak || weak->m_shutdown)return;
            weak->CallAfter([weak,id]{
                if(weak && !weak->m_shutdown && weak->m_project_filament_delete && weak->workbench_snapshot().can_edit_project_colors)
                    weak->m_project_filament_delete(id);
            });
        });
        row->Enable(snapshot.can_edit_project_colors);
        contents->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(6));
    }
    // Keep the primary action within reach even with the full 32-slot table.
    // Every row stays accessible through the list's own scrollbar.
    const int list_height = std::clamp(contents->CalcMin().y, FromDIP(46), FromDIP(216));
    m_workbench_project_colors->SetMinSize(wxSize(1, list_height));
    m_workbench_project_colors->SetMaxSize(wxSize(-1, list_height));
    m_workbench_project_colors->InvalidateBestSize();
    m_workbench_project_colors->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_workbench_project_colors)) scroll->FitInside();
    m_workbench_settings_scroll->Layout();
    m_workbench_settings_scroll->FitInside();
    refresh_post_generation_workbench();
    publish_workbench_state();
}

} // namespace Slic3r::GUI
