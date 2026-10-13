#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchFilamentRecipes.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintPaletteSnapshot.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <cctype>

namespace Slic3r::GUI {

namespace {

std::optional<AI::ModelMatchedColors> capture_applied_filament_match(
    const PresetBundle& bundle, const ModelPreview3D& preview,
    const std::string& source_sha256)
{
    const auto state = PrinterFilamentConfig::capture(bundle);
    if (source_sha256.empty() || preview.geometry_id().empty() || state.slots.empty()) return {};
    const auto trial = preview.color_trial_state();
    if (trial.source != 1 || trial.project_slot_identity.empty() ||
        trial.project_color_slots.size() != trial.colors.size() || trial.colors.empty()) return {};

    AI::ModelMatchedColors matched;
    matched.source_sha256 = source_sha256;
    matched.geometry_id = preview.geometry_id();
    const auto native_palette = OrcaPrintPaletteSnapshot::capture(bundle);
    matched.material_fingerprint = native_palette.material_fingerprint;
    matched.process_fingerprint = native_palette.process_fingerprint;
    matched.mixed_recipes = native_palette.mixed_recipes;
    for (const auto& slot : state.slots) {
        if (!slot.exists || !slot.configured || !AI::is_rgb_hex_color(slot.color)) continue;
        matched.palette.push_back({slot.slot, slot.color, slot.material, slot.compatible});
    }
    if (!AI::valid_native_mixed_palette(matched.palette, matched.mixed_recipes)) return {};

    std::vector<PreviewPalette::Color> logical_colors;
    std::vector<size_t> logical_slots;
    if (trial.semantic_palette.size() == trial.project_semantic_slots.size() && !trial.semantic_palette.empty()) {
        logical_colors.insert(logical_colors.end(), trial.semantic_palette.begin(), trial.semantic_palette.end());
        logical_slots.insert(logical_slots.end(), trial.project_semantic_slots.begin(), trial.project_semantic_slots.end());
    }
    logical_colors.insert(logical_colors.end(), trial.colors.begin(), trial.colors.end());
    logical_slots.insert(logical_slots.end(), trial.project_color_slots.begin(), trial.project_color_slots.end());
    if (logical_colors.empty() || logical_colors.size() != logical_slots.size()) return {};

    const auto face_colors = preview.printer_filament_export_faces();
    matched.face_slots.assign(preview.triangle_count(), size_t(-1));
    for (const auto& item : face_colors) {
        if (item.first >= matched.face_slots.size()) return {};
        const size_t index = PrinterFilamentPreview::nearest(item.second, logical_colors);
        if (index >= logical_slots.size() || !AI::native_palette_has_slot(matched.palette, matched.mixed_recipes, logical_slots[index]))
            return {};
        matched.face_slots[item.first] = logical_slots[index];
    }
    if (std::any_of(matched.face_slots.begin(), matched.face_slots.end(), [](size_t slot) { return slot == size_t(-1); }) ||
        !matched.valid()) return {};
    return matched;
}

}

void ModelGenerationPanel::apply_workbench_printer_filaments()
{
    if (m_shutdown || m_apply_filaments_saving || !m_model_preview || !workbench_snapshot().can_apply_printer_filaments) return;
    m_apply_filaments_match.reset();
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return;
    auto before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    before->project_config = std::make_shared<DynamicPrintConfig>(bundle->project_config);
    before->project_presets = bundle->filament_presets;
    auto staged = std::make_shared<PresetBundle>(*bundle);
    std::vector<size_t> bindings;
    std::vector<PrinterFilamentSlotRemap::Change> changes;
    std::string error;
    const auto state = PrinterFilamentConfig::capture(*bundle);
    const auto trial = m_model_preview->color_trial_state();
    if (m_applied_filaments_saved_model == m_displayed_model_path &&
        m_applied_filaments_saved_signature == state.signature && trial.source == 1 &&
        !m_beauty_manual_color_dirty && (!m_beauty_controls || !m_beauty_controls->has_changes())) {
        m_workbench_operation_notice = "已应用并保存，可直接导入";
        refresh_controls(); return;
    }
    if (state.physical_capacity < 32) {
        std::vector<std::string> targets;
        // The semantic/portrait palette is an alternate rendering of the
        // same logical card entries.  Stage one target per logical color and
        // let apply_bound reuse those slot identities for both palettes.
        // Appending both palettes here doubled a six-color portrait card to
        // twelve targets and consequently created surplus mix slots.
        const auto& source_colors = trial.colors.empty() ? trial.semantic_palette : trial.colors;
        for (const auto& c : source_colors)
            targets.push_back(color_decompose_rgb_to_hex({static_cast<unsigned char>(std::lround(c[0]*255)),
                static_cast<unsigned char>(std::lround(c[1]*255)), static_cast<unsigned char>(std::lround(c[2]*255))}));
        auto preferred = trial.project_slot_identity == state.identity ? trial.project_color_slots : std::vector<size_t>{};
        if (!WorkbenchFilamentRecipes::stage_targets(*staged, targets, bindings, error, preferred)) {
            m_workbench_operation_notice = error;
            refresh_controls(); publish_workbench_state(); return;
        }
        // A previous card may have left obsolete virtual recipes at the end
        // of the native list. Keep the staged project bounded to the current
        // logical card and carry the removals through the normal remap
        // transaction so model/painting references stay undoable.
        for (size_t at = bundle->filament_presets.size(); at < staged->filament_presets.size(); ++at)
            changes.push_back({{at}, -1, -1});
        if (!PrinterFilamentConfig::trim_unused_mixed_slots(*staged, bindings, targets.size(), changes, error)) {
            m_workbench_operation_notice = error;
            refresh_controls(); publish_workbench_state(); return;
        }
    }
    m_model_preview->set_project_configuration_override(staged);
    if (!m_model_preview->apply_project_filament_palette(bindings.empty() ? nullptr : &bindings)) {
        m_model_preview->set_project_configuration_override({});
        restore_beauty_candidate(*before);
        m_workbench_operation_notice = "耗材匹配失败，原模型和工程配置保持不变";
        refresh_controls(); return;
    }
    m_apply_filaments_match = capture_applied_filament_match(
        *staged, *m_model_preview, AI::model_artifact_sha256(m_displayed_model_path));
    if (!m_apply_filaments_match) {
        m_model_preview->set_project_configuration_override({});
        restore_beauty_candidate(*before);
        m_workbench_operation_notice = "耗材应用没有生成完整面级匹配，原模型和工程配置保持不变";
        refresh_controls(); return;
    }
    m_apply_filaments_before = before;
    m_apply_filaments_staged = staged;
    m_apply_filaments_changes = std::move(changes);
    m_apply_filaments_source_identity = state.signature;
    m_apply_filaments_saving = true;
    m_beauty_session_source = before;
    m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
    m_beauty_session_file_base = m_beauty_candidate_files.size();
    m_beauty_manual_color_dirty = true;
    if (m_beauty_controls) m_beauty_controls->set_dirty(true);
    m_workbench_operation_notice = "正在应用并自动保存……";
    if (!request_save_and_return()) finish_printer_filament_application(false);
    refresh_controls();
    publish_workbench_state();
}

bool ModelGenerationPanel::commit_printer_filament_application(std::string& error)
{
    if (!m_apply_filaments_saving) return true;
    auto* bundle = wxGetApp().preset_bundle;
    auto* plater = wxGetApp().plater();
    if (!bundle || !plater || !m_apply_filaments_staged ||
        PrinterFilamentConfig::capture(*bundle).signature != m_apply_filaments_source_identity) {
        error = "保存期间打印机或耗材已变化，本次应用未提交"; return false;
    }
    return plater->apply_printer_filament_config(*m_apply_filaments_staged, m_apply_filaments_changes, error);
}

void ModelGenerationPanel::finish_printer_filament_application(bool success)
{
    if (!m_apply_filaments_saving) return;
    m_apply_filaments_saving = false;
    m_save_and_return = false;
    m_model_preview->set_project_configuration_override({});
    if (!success && m_apply_filaments_before) {
        // The live project was never modified before final publication. Restore
        // the model draft, without overwriting an external configuration change.
        auto before = *m_apply_filaments_before;
        before.project_config.reset(); before.project_presets.clear();
        restore_beauty_candidate(before);
        if (m_beauty_transactions) m_beauty_transactions->truncate_to(m_beauty_session_undo_base);
        clear_unaccepted_beauty_candidates(m_beauty_session_file_base);
        m_beauty_session_source.reset();
        m_workbench_operation_notice = "应用未保存，原模型和工程配置保留。 " + m_workbench_operation_notice;
    } else if (success) {
        m_applied_filaments_saved_model = m_displayed_model_path;
        m_applied_filaments_saved_signature = PrinterFilamentConfig::capture(*wxGetApp().preset_bundle).signature;
        m_workbench_operation_notice = "已应用并保存，可直接导入";
        m_finishing_status->SetLabel(_L("已应用并保存，可直接导入。"));
        m_status->SetLabel(m_finishing_status->GetLabel());
    }
    m_apply_filaments_before.reset(); m_apply_filaments_staged.reset();
    m_apply_filaments_changes.clear();
    m_apply_filaments_source_identity.clear();
    m_apply_filaments_match.reset();
    synchronize_workbench_project_palette(false);
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
    const int columns = m_workbench_project_colors->GetClientSize().x >= FromDIP(340) ? 2 : 1;
    auto* grid = new wxFlexGridSizer(columns, FromDIP(5), FromDIP(7));
    grid->AddGrowableCol(0);
    if (columns == 2) grid->AddGrowableCol(1);
    contents->Add(grid, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    auto make_row = [&](size_t id, const std::string& hex, const wxString& text, const wxString& tooltip) {
        auto* row = new wxPanel(m_workbench_project_colors);
        row->SetBackgroundColour(m_workbench_project_colors->GetBackgroundColour());
        row->SetSizer(new wxBoxSizer(wxHORIZONTAL));
        auto* swatch = new WorkbenchButton(row, wxString::Format("%u", unsigned(id + 1)));
        swatch->SetMinSize(FromDIP(wxSize(34, 34))); swatch->SetCornerRadius(FromDIP(4));
        wxColour color(wxString::FromUTF8(hex));
        if (!color.IsOk()) color = wxColour(65,65,70);
        swatch->SetBackgroundColor(StateColor(color));
        const double luminance = 0.2126*color.Red()+0.7152*color.Green()+0.0722*color.Blue();
        swatch->SetTextColor(StateColor(luminance>140?wxColour(22,22,25):wxColour(245,245,245)));
        swatch->SetName(wxString::Format("project_filament_color_%u",unsigned(id+1)));
        swatch->SetToolTip(tooltip);
        auto* label = new wxStaticText(row, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        label->SetForegroundColour(wxColour(190,190,196)); label->SetMinSize(wxSize(1,-1));
        label->SetToolTip(tooltip);
        row->GetSizer()->Add(swatch,0,wxRIGHT|wxALIGN_CENTER_VERTICAL,FromDIP(6));
        row->GetSizer()->Add(label,1,wxALIGN_CENTER_VERTICAL);
        return std::make_pair(row, swatch);
    };
    for (const auto& slot : state.slots) {
        const wxString summary = slot.configured ? wxString::FromUTF8(slot.preset) : _L("待配置真实耗材");
        const wxString tooltip = summary + "\n" + wxString::FromUTF8(slot.material + " · " + slot.color +
            (slot.reason.empty() ? " · 已配置" : " · " + slot.reason));
        auto [row, swatch] = make_row(slot.slot, slot.color, summary, tooltip);
        const wxWeakRef<ModelGenerationPanel> weak(this);
        auto edit = [weak,id=slot.slot] {
            if (weak && !weak->m_shutdown && weak->m_project_color_edit && weak->workbench_snapshot().can_edit_project_colors)
                weak->m_project_color_edit(id);
        };
        swatch->Bind(wxEVT_BUTTON,[weak, edit, id=slot.slot, removable=slot.removable, swatch](wxCommandEvent&){
            if (!weak || weak->m_shutdown) return;
            wxMenu menu;
            const int first = wxWindow::NewControlId(2);
            menu.Append(first, _L("配置耗材"));
            menu.Append(first + 1, _L("删除并替换引用")); menu.Enable(first + 1, removable);
            const int chosen = swatch->GetPopupMenuSelectionFromUser(menu);
            if (chosen == first) edit();
            else if (chosen == first + 1 && weak->m_project_filament_delete && weak->workbench_snapshot().can_edit_project_colors)
                weak->m_project_filament_delete(id);
        });
        row->Enable(snapshot.can_edit_project_colors);
        grid->Add(row,0,wxEXPAND);
    }
    if (!state.mixed.empty()) {
        auto* title = new wxStaticText(m_workbench_project_colors, wxID_ANY, _L("叠色配方"));
        title->SetForegroundColour(wxColour(190,190,196));
        contents->Add(title, 0, wxTOP | wxBOTTOM, FromDIP(5));
        for (const auto& mix : state.mixed) {
            const auto text = wxString::FromUTF8(mix.recipe);
            auto [row, swatch] = make_row(mix.slot, mix.color, text,
                text + "\n" + _L("预估叠色；实物色准需打印验证") + "\n" + wxString::FromUTF8(mix.reason));
            swatch->SetToolTip(text + "\n" + _L("预估叠色；实物色准需打印验证"));
            const wxWeakRef<ModelGenerationPanel> weak(this);
            swatch->Bind(wxEVT_BUTTON, [weak, id=mix.slot, swatch](wxCommandEvent&) {
                if (!weak || weak->m_shutdown) return;
                wxMenu menu;
                const int remove = wxWindow::NewControlId();
                menu.Append(remove, _L("删除叠色配方"));
                const int chosen = swatch->GetPopupMenuSelectionFromUser(menu);
                if (chosen == remove && weak->m_project_filament_delete && weak->workbench_snapshot().can_edit_project_colors)
                    weak->m_project_filament_delete(id);
            });
            if (mix.valid && wxGetApp().preset_bundle) {
                const auto& config = wxGetApp().preset_bundle->project_config;
                const auto ids = parse_mixed_components(config.opt_string("filament_mixed_components", mix.slot));
                const auto ratios = parse_mixed_ratios(config.opt_string("filament_mixed_sublayer_ratios", mix.slot), ids.size());
                auto* old_label = row->GetSizer()->GetItem(size_t(1))->GetWindow();
                row->GetSizer()->Detach(old_label); old_label->Destroy();
                auto* components = new wxPanel(row);
                components->SetBackgroundColour(row->GetBackgroundColour());
                components->SetMinSize(wxSize(1, -1));
                auto* wrap = new wxWrapSizer(wxHORIZONTAL);
                components->SetSizer(wrap);
                for (size_t i = 0; i < ids.size(); ++i) {
                    if (i) {
                        auto* plus = new wxStaticText(components, wxID_ANY, "+");
                        plus->SetForegroundColour(wxColour(190,190,196));
                        wrap->Add(plus, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                    }
                    const auto found = std::find_if(state.slots.begin(), state.slots.end(),
                        [&](const auto& slot) { return slot.slot + 1 == ids[i]; });
                    if (found == state.slots.end()) continue;
                    auto* chip = new WorkbenchButton(components, wxString::Format("%u", ids[i]));
                    chip->SetMinSize(FromDIP(wxSize(24,24))); chip->SetCornerRadius(FromDIP(4));
                    wxColour base(wxString::FromUTF8(found->color));
                    if (!base.IsOk()) base = wxColour(65,65,70);
                    chip->SetBackgroundColor(StateColor(base));
                    chip->SetTextColor(StateColor(0.2126*base.Red()+0.7152*base.Green()+0.0722*base.Blue()>140
                        ? wxColour(22,22,25) : wxColour(245,245,245)));
                    chip->SetToolTip(wxString::FromUTF8(found->preset + " · " + found->color));
                    chip->Enable(snapshot.can_edit_project_colors);
                    chip->Bind(wxEVT_BUTTON, [weak, id=found->slot](wxCommandEvent&) {
                        if (weak && !weak->m_shutdown && weak->m_project_color_edit && weak->workbench_snapshot().can_edit_project_colors)
                            weak->m_project_color_edit(id);
                    });
                    wrap->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
                    auto* ratio = new wxStaticText(components, wxID_ANY,
                        wxString::Format("%d%%", int(std::lround(ratios[i]*100))));
                    ratio->SetForegroundColour(wxColour(190,190,196));
                    wrap->Add(ratio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
                }
                row->GetSizer()->Add(components, 1, wxEXPAND);
            }
            contents->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        }
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
