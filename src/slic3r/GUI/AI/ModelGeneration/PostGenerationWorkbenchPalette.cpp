#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintPaletteSnapshot.hpp"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>
#include <wx/wrapsizer.h>
#include <cctype>

namespace Slic3r::GUI {

namespace {

enum class NativeFilamentControl { Color, Preset, Actions };

ModelPreview3D::PrinterFilamentMatchResult capture_applied_filament_match(
    const PresetBundle& bundle, const ModelPreview3D& preview,
    const std::string& source_sha256)
{
    ModelPreview3D::PrinterFilamentMatchResult result;
    const auto state = PrinterFilamentConfig::capture(bundle);
    if (source_sha256.empty() || preview.geometry_id().empty()) {
        result.error = "当前模型身份无效，请重新载入模型。";
        return result;
    }
    if (!state.ready) {
        result.error = state.reason.empty() ? "请先配置当前打印机耗材。" : state.reason;
        return result;
    }

    AI::ModelMatchedColors matched;
    matched.source_sha256 = source_sha256;
    matched.geometry_id = preview.geometry_id();
    const auto native_palette = OrcaPrintPaletteSnapshot::capture(bundle);
    matched.material_fingerprint = native_palette.material_fingerprint;
    matched.process_fingerprint = native_palette.process_fingerprint;
    matched.mixed_recipes = native_palette.mixed_recipes;
    matched.palette = native_palette.physical_channels;
    if (!AI::valid_native_mixed_palette(matched.palette, matched.mixed_recipes)) {
        result.error = "当前打印机耗材或叠色配方无效，请先完成配置。";
        return result;
    }

    std::vector<PrinterFilamentPreview::NativeColorSlot> candidates;
    auto append = [&candidates](size_t slot, const std::string& color, bool mixed) {
        const auto key = PrinterFilamentPreview::rgb8(color);
        if (key) candidates.push_back({slot, PrinterFilamentPreview::rgb8_color(*key), mixed});
    };
    std::vector<AI::PhysicalFilamentChannel> physical = matched.palette;
    std::sort(physical.begin(), physical.end(), [](const auto& first, const auto& second) {
        return first.slot < second.slot;
    });
    for (const auto& slot : physical)
        if (slot.compatible) append(slot.slot, slot.display_color, false);
    auto recipes = matched.mixed_recipes;
    std::sort(recipes.begin(), recipes.end(), [](const auto& first, const auto& second) {
        return first.existing_virtual_slot.value_or(size_t(-1)) < second.existing_virtual_slot.value_or(size_t(-1));
    });
    for (const auto& recipe : recipes)
        if (recipe.existing_virtual_slot) append(*recipe.existing_virtual_slot, recipe.target_color, true);
    if (candidates.empty()) {
        result.error = "当前没有可用于模型配色的兼容耗材槽。";
        return result;
    }

    const auto surface = preview.workbench_surface_palette();
    if (surface.faces.size() != preview.triangle_count()) {
        result.error = "无法读取当前模型的完整表面配色。";
        return result;
    }
    std::vector<PreviewPalette::Color> face_colors, subface_colors;
    std::vector<std::optional<size_t>> preferred_faces, preferred_subfaces;
    face_colors.reserve(surface.faces.size());
    preferred_faces.reserve(surface.faces.size());
    for (const auto& face : surface.faces) {
        face_colors.push_back(face.color);
        preferred_faces.push_back(face.preferred_slot);
    }
    subface_colors.reserve(surface.subfaces.size());
    preferred_subfaces.reserve(surface.subfaces.size());
    for (const auto& subface : surface.subfaces) {
        subface_colors.push_back(subface.color);
        preferred_subfaces.push_back(subface.preferred_slot);
    }
    const auto faces = PrinterFilamentPreview::bind_exact_colors(face_colors, candidates, preferred_faces);
    const auto subfaces = PrinterFilamentPreview::bind_exact_colors(subface_colors, candidates, preferred_subfaces);
    result.missing_colors = faces.missing_colors;
    result.missing_colors.insert(result.missing_colors.end(), subfaces.missing_colors.begin(), subfaces.missing_colors.end());
    std::sort(result.missing_colors.begin(), result.missing_colors.end());
    result.missing_colors.erase(std::unique(result.missing_colors.begin(), result.missing_colors.end()), result.missing_colors.end());
    if (!faces.complete(face_colors.size()) || !subfaces.complete(subface_colors.size())) return result;

    matched.face_slots = faces.slots;
    for (size_t index = 0; index < surface.subfaces.size(); ++index) {
        const auto& child = surface.subfaces[index];
        matched.subface_slots.push_back({child.face_id, child.depth, child.path, subfaces.slots[index]});
    }
    if (!matched.valid()) {
        result.error = "当前耗材映射没有生成完整的面级和子面级绑定。";
        return result;
    }
    result.matched = std::move(matched);
    return result;
}

std::string workbench_filament_layout(const WorkbenchPrinterFilaments& state, const PresetBundle* bundle)
{
    auto layout = nlohmann::json::parse(state.layout, nullptr, false);
    if (layout.is_discarded() || !layout.is_object()) layout = nlohmann::json::object();
    layout["visible_mixed"] = nlohmann::json::array();
    for (const auto& mix : state.mixed) {
        nlohmann::json entry = {mix.slot, mix.valid};
        if (bundle && mix.slot < bundle->filament_presets.size())
            entry.push_back(parse_mixed_components(
                bundle->project_config.opt_string("filament_mixed_components", mix.slot)));
        layout["visible_mixed"].push_back(std::move(entry));
    }
    return layout.dump();
}

}

std::optional<AI::ModelMatchedColors> ModelPreview3D::printer_filament_match(
    const PresetBundle& bundle, const std::string& source_sha256) const
{
    return capture_applied_filament_match(bundle, *this, source_sha256).matched;
}

ModelPreview3D::PrinterFilamentMatchResult ModelPreview3D::printer_filament_match_result(
    const PresetBundle& bundle, const std::string& source_sha256) const
{
    return capture_applied_filament_match(bundle, *this, source_sha256);
}

void ModelGenerationPanel::toggle_printer_filament_trial()
{
    if (m_shutdown || m_apply_filaments_saving || !m_model_preview || !wxGetApp().plater()) return;
    if (m_printer_filament_trial_active) {
        exit_printer_filament_trial();
        return;
    }
    auto report = [this](std::string message, bool error) {
        m_workbench_printer_filament_notice = message;
        m_workbench_printer_filament_notice_error = error;
        m_workbench_operation_notice = std::move(message);
        refresh_controls();
        publish_workbench_state();
    };
    const auto snapshot = workbench_snapshot();
    if (!snapshot.can_edit_project_colors || m_finishing_candidate.size()) {
        report("模型处理中或存在未接受候选，请先完成当前操作。", true);
        return;
    }
    const auto entry = capture_beauty_candidate();
    const auto card = m_model_preview->model_color_pack_for_filament_import();
    if (!card) {
        report("当前没有可导入的模型配色，请先生成自动建议或完成试色。", true);
        return;
    }
    auto* bundle = wxGetApp().preset_bundle;
    auto* plater = wxGetApp().plater();
    if (!bundle || !plater) {
        report("当前没有可用的打印机耗材配置。", true);
        return;
    }
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;
    if (!PrinterFilamentConfig::stage_workbench_color_pack_import(*bundle, *card, staged, error)) {
        report(error.empty() ? "色卡无法生成可用的实体耗材与叠色配方，工程保持原状。" : error, true);
        return;
    }

    auto rollback_entry = [&] {
        const bool restored = restore_beauty_candidate(entry);
        if (!restored)
            BOOST_LOG_TRIVIAL(error) << "AI model-color import failed to restore its entry preview state";
        return restored;
    };
    // A previously persisted printer source may still be visible after a
    // history restore. Restore the captured model appearance only after all
    // read-only staging has succeeded, so early failures cannot change it.
    if (m_model_preview->color_trial_state().source == 1 &&
        !m_model_preview->restore_printer_filament_input_source()) {
        const bool restored = rollback_entry();
        report(restored ? "无法恢复进入试色前的模型外观，请重新载入模型后再试。" :
            "无法恢复进入试色前的模型外观，且点击前状态恢复失败；请重新载入模型。", true);
        return;
    }

    set_project_color_sync_suspended(true);
    m_model_preview->set_project_configuration_override(staged.configuration);
    auto rollback_preview = [&] {
        m_model_preview->set_project_configuration_override({});
        set_project_color_sync_suspended(false);
        rollback_entry();
    };
    if (!m_model_preview->apply_project_filament_palette(&staged.bindings)) {
        rollback_preview();
        report("色卡暂存成功，但模型配色无法绑定到耗材；工程和模型预览均保持原状。", true);
        return;
    }
    std::string preview_error;
    if (!m_model_preview->activate_printer_filament_preview(&preview_error)) {
        rollback_preview();
        BOOST_LOG_TRIVIAL(error) << "AI model-color import preview activation failed: "
            << preview_error;
        report("色卡暂存成功，但模型试色无法激活；工程和模型预览均保持原状。", true);
        return;
    }
    if (PrinterFilamentConfig::capture(*bundle).signature != staged.source_signature) {
        rollback_preview();
        report("提交前打印机或耗材配置已变化，请重新导入色卡。", true);
        return;
    }
    if (!plater->apply_printer_filament_config(*staged.configuration, staged.changes, error)) {
        rollback_preview();
        report(error.empty() ? "色卡提交失败，工程和模型预览均保持原状。" :
            "色卡提交失败，工程和模型预览均保持原状：" + error, true);
        return;
    }
    m_model_preview->set_project_configuration_override({});
    set_project_color_sync_suspended(false);
    m_printer_filament_trial_active = true;
    m_printer_filament_trial_geometry = m_model_preview->geometry_id();
    m_printer_filament_trial_signature = PrinterFilamentConfig::capture(*bundle).signature;
    const std::string success = "已导入 " + std::to_string(card->colors.size()) + " 色：" +
        std::to_string(staged.physical_count) + " 个实体槽 + " +
        std::to_string(staged.mixed_count) + " 个叠色槽，已进入试色";
    m_workbench_printer_filament_notice = success;
    m_workbench_printer_filament_notice_error = false;
    m_workbench_operation_notice = success;
    synchronize_workbench_project_palette(true);
    refresh_controls();
    publish_workbench_state();
}

void ModelGenerationPanel::exit_printer_filament_trial(const wxString& reason)
{
    if (!m_printer_filament_trial_active &&
        (!m_model_preview || m_model_preview->color_trial_state().source != 1)) return;
    // Clear the flag before restoring the source: the color-control callback
    // is synchronous and would otherwise re-enter this method.
    m_printer_filament_trial_active = false;
    const bool restored = m_model_preview && m_model_preview->exit_printer_filament_trial();
    m_printer_filament_trial_geometry.clear();
    m_printer_filament_trial_signature.clear();
    m_workbench_operation_notice = !reason.empty() ? std::string(reason.ToUTF8().data()) :
        (restored ? "已退出打印机耗材试色，模型已恢复进入前外观。" : "耗材试色已失效，请重新载入模型。 ");
    m_workbench_printer_filament_notice = m_workbench_operation_notice;
    m_workbench_printer_filament_notice_error = !restored;
    refresh_controls();
    publish_workbench_state();
}

void ModelGenerationPanel::invalidate_printer_filament_trial_for_model_change()
{
    if (!m_printer_filament_trial_active) return;
    // Clear the panel-owned identity first. Restoring the captured source can
    // synchronously notify the color controls, and that callback must not
    // re-enter the user-facing exit path while the preview is being replaced.
    m_printer_filament_trial_active = false;
    if (m_model_preview) m_model_preview->exit_printer_filament_trial();
    m_printer_filament_trial_geometry.clear();
    m_printer_filament_trial_signature.clear();
}

void ModelGenerationPanel::apply_workbench_printer_filaments()
{
    if (m_shutdown || m_apply_filaments_saving || !m_model_preview || !workbench_snapshot().can_apply_printer_filaments) return;
    auto report = [this](std::string message, bool error) {
        m_workbench_operation_notice = message;
        m_workbench_printer_filament_notice = std::move(message);
        m_workbench_printer_filament_notice_error = error;
        refresh_controls();
        publish_workbench_state();
    };
    m_apply_filaments_match.reset();
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) {
        report("当前打印机耗材配置不可用", true);
        return;
    }
    const auto state = PrinterFilamentConfig::capture(*bundle);
    if (!state.ready) {
        report(state.reason.empty() ? "请先配置当前打印机耗材" : state.reason, true);
        return;
    }
    auto trial = m_model_preview->color_trial_state();
    if (m_applied_filaments_saved_model == m_displayed_model_path &&
        m_applied_filaments_saved_signature == state.signature && trial.source == 1 &&
        trial.enabled && m_model_preview->project_matching_valid() &&
        !m_beauty_manual_color_dirty && (!m_beauty_controls || !m_beauty_controls->has_changes())) {
        report("已应用并保存，可直接导入", false);
        return;
    }
    const bool trial_was_active = m_printer_filament_trial_active;
    const std::string trial_geometry = m_printer_filament_trial_geometry;
    const std::string trial_signature = m_printer_filament_trial_signature;
    auto before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    before->project_config = std::make_shared<DynamicPrintConfig>(bundle->project_config);
    before->project_presets = bundle->filament_presets;
    auto rollback_model = [&] {
        auto rollback = *before;
        rollback.project_config.reset();
        rollback.project_presets.clear();
        const bool restored = restore_beauty_candidate(rollback);
        if (restored && trial_was_active) {
            m_printer_filament_trial_active = true;
            m_printer_filament_trial_geometry = trial_geometry;
            m_printer_filament_trial_signature = trial_signature;
        }
        return restored;
    };
    // Applying printer filaments is a read-only match against the live native
    // palette. Never stage color-card targets here: doing so changes the
    // printer slot count and invents recipes from the card.
    auto staged = std::make_shared<PresetBundle>(*bundle);
    std::vector<PrinterFilamentSlotRemap::Change> changes;
    if (trial.source == 1 && !m_model_preview->restore_printer_filament_input_source()) {
        const bool restored = rollback_model();
        report(restored ? "无法恢复应用前的模型颜色，请重新加载模型后再试" :
            "无法恢复应用前的模型颜色，且点击前状态恢复失败；请重新加载模型", true);
        return;
    }
    m_model_preview->set_project_configuration_override(staged);
    if (!m_model_preview->apply_project_filament_basis()) {
        m_model_preview->set_project_configuration_override({});
        const bool restored = rollback_model();
        report(restored ? "耗材匹配失败，原模型和工程配置保持不变" :
            "耗材匹配失败，且点击前模型状态恢复失败；请重新加载模型", true);
        return;
    }
    std::string preview_error;
    if (!m_model_preview->activate_printer_filament_preview(&preview_error)) {
        m_model_preview->set_project_configuration_override({});
        const bool restored = rollback_model();
        BOOST_LOG_TRIVIAL(error) << "AI printer-to-model preview activation failed: "
            << preview_error;
        report(restored ? "耗材已匹配，但预览刷新失败，原模型和工程配置保持不变" :
            "耗材已匹配，但预览与点击前模型状态均恢复失败；请重新加载模型", true);
        return;
    }
    if (m_beauty_controls) m_beauty_controls->synchronize_preview_options();
    auto captured_match = capture_applied_filament_match(
        *staged, *m_model_preview, AI::model_artifact_sha256(m_displayed_model_path));
    m_apply_filaments_match = std::move(captured_match.matched);
    if (!m_apply_filaments_match) {
        m_model_preview->set_project_configuration_override({});
        const bool restored = rollback_model();
        report(!restored ? "耗材应用未生成完整绑定，且点击前模型状态恢复失败；请重新加载模型" :
            captured_match.error.empty()
            ? "耗材应用没有生成完整面级匹配，原模型和工程配置保持不变"
            : captured_match.error + " 原模型和工程配置保持不变", true);
        return;
    }
    m_apply_filaments_before = before;
    m_apply_filaments_staged = staged;
    m_apply_filaments_changes = std::move(changes);
    m_apply_filaments_source_identity = state.signature;
    m_apply_filaments_before_trial_active = trial_was_active;
    m_apply_filaments_saving = true;
    m_beauty_session_source = before;
    m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
    m_beauty_session_file_base = m_beauty_candidate_files.size();
    m_beauty_manual_color_dirty = true;
    if (m_beauty_controls) m_beauty_controls->set_dirty(true);
    m_workbench_operation_notice = "正在应用并自动保存……";
    m_workbench_printer_filament_notice = m_workbench_operation_notice;
    m_workbench_printer_filament_notice_error = false;
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
        const bool restored = restore_beauty_candidate(before);
        if (restored && m_apply_filaments_before_trial_active) {
            m_printer_filament_trial_active = true;
            m_printer_filament_trial_geometry = before.geometry_id;
            m_printer_filament_trial_signature = m_apply_filaments_source_identity;
        }
        if (m_beauty_transactions) m_beauty_transactions->truncate_to(m_beauty_session_undo_base);
        clear_unaccepted_beauty_candidates(m_beauty_session_file_base);
        m_beauty_session_source.reset();
        m_workbench_operation_notice = "应用未保存，原模型和工程配置保留。 " + m_workbench_operation_notice;
        m_workbench_printer_filament_notice = m_workbench_operation_notice;
        m_workbench_printer_filament_notice_error = true;
    } else if (success) {
        // A printer-to-model application publishes a saved model version, not
        // a temporary filament trial. If it started while a trial was active,
        // retire only the panel flag while preserving the source snapshot that
        // lets the opposite action recover the original model palette later.
        m_printer_filament_trial_active = false;
        m_printer_filament_trial_geometry.clear();
        m_printer_filament_trial_signature.clear();
        m_applied_filaments_saved_model = m_displayed_model_path;
        m_applied_filaments_saved_signature = PrinterFilamentConfig::capture(*wxGetApp().preset_bundle).signature;
        m_workbench_operation_notice = "已应用并保存，可直接导入";
        m_workbench_printer_filament_notice = m_workbench_operation_notice;
        m_workbench_printer_filament_notice_error = false;
        m_finishing_status->SetLabel(_L("已应用并保存，可直接导入。"));
        m_status->SetLabel(m_finishing_status->GetLabel());
    }
    m_apply_filaments_before.reset(); m_apply_filaments_staged.reset();
    m_apply_filaments_changes.clear();
    m_apply_filaments_source_identity.clear();
    m_apply_filaments_match.reset();
    m_apply_filaments_before_trial_active = false;
    synchronize_workbench_project_palette(false);
}

void ModelGenerationPanel::synchronize_workbench_project_palette(bool activate_preview)
{
    if (m_shutdown || !m_workbench_project_colors) return;
    if (m_printer_filament_trial_active && m_model_preview &&
        !m_printer_filament_trial_geometry.empty() &&
        m_model_preview->geometry_id() != m_printer_filament_trial_geometry) {
        exit_printer_filament_trial(_L("模型已更换，打印机耗材试色已退出。"));
        return;
    }
    const auto snapshot = workbench_snapshot();
    const auto& state = snapshot.printer_filaments;
    const bool changed = state.signature != m_workbench_filament_signature;
    if (m_model_preview_ready) m_model_preview->synchronize_project_colors(activate_preview);
    if (m_printer_filament_trial_active && m_model_preview) {
        const auto trial = m_model_preview->color_trial_state();
        if (trial.source != 1 || !m_model_preview->project_matching_valid()) {
            exit_printer_filament_trial(_L("打印机耗材槽位已变化，试色映射已失效。"));
            return;
        }
        m_printer_filament_trial_signature = state.signature;
    }
    if (!changed) return;
    m_workbench_filament_signature = state.signature;
    m_workbench_project_palette = snapshot.project_palette;
    m_workbench_project_channels = snapshot.project_channels;
    const auto* bundle = wxGetApp().preset_bundle;
    const std::string layout = workbench_filament_layout(state, bundle);
    const bool rebuild = layout != m_workbench_filament_layout ||
        m_workbench_filament_rows.size() != state.slots.size() + state.mixed.size();
    wxWindowUpdateLocker updates(m_workbench_project_colors);

    auto native_filament_control = [this](size_t id, NativeFilamentControl control, wxWindow* anchor = nullptr) {
        auto *plater = wxGetApp().plater();
        if (!plater) return false;
        for (auto *combo : plater->sidebar().combos_filament()) {
            if (!combo || combo->get_filament_idx() != int(id)) continue;
            if (control == NativeFilamentControl::Color) {
                if (!combo->clr_picker) return false;
                combo->show_color_picker(anchor);
                return true;
            }
            if (control == NativeFilamentControl::Preset)
                return combo->show_dropdown_at(anchor);
            auto *button = combo->edit_btn;
            if (!button) return false;
            // The native sidebar handler owns the menu's slot identity.  Use
            // that handler unchanged, but temporarily place its hidden button
            // below the visible workbench row so wxPopupMenu gets the right
            // screen anchor instead of the hidden sidebar origin.
            const wxPoint previous = button->GetPosition();
            if (anchor && button->GetParent()) {
                const wxPoint screen = anchor->ClientToScreen(
                    wxPoint(0, anchor->GetSize().GetHeight() + FromDIP(2)));
                button->SetPosition(button->GetParent()->ScreenToClient(screen));
            }
            wxCommandEvent event(wxEVT_BUTTON, button->GetId());
            event.SetEventObject(button);
            const bool handled = button->GetEventHandler()->ProcessEvent(event);
            if (anchor && button->GetParent()) button->SetPosition(previous);
            return handled;
        }
        return false;
    };
    if (rebuild) {
        auto* contents = m_workbench_project_colors->GetSizer();
        contents->Clear(true);
        m_workbench_filament_rows.clear();
        m_workbench_mixed_filament_title = nullptr;
        // Keep one native-style filament row per line. The workbench is a
        // narrow settings rail, so every row keeps the native control scale.
        auto* grid = new wxFlexGridSizer(1, FromDIP(5), FromDIP(7));
        grid->AddGrowableCol(0);
        contents->Add(grid, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        auto make_row = [&](size_t id, bool mixed) {
            const int row_height = FromDIP(PrinterFilamentConfig::workbench_row_height_dip);
            WorkbenchFilamentRowView view;
            view.slot = id;
            view.mixed = mixed;
            view.color = "\x01";
            view.panel = new wxPanel(m_workbench_project_colors);
            view.panel->SetBackgroundColour(m_workbench_project_colors->GetBackgroundColour());
            view.panel->SetMinSize(wxSize(-1, row_height));
            view.panel->SetMaxSize(wxSize(-1, row_height));
            view.panel->SetSizer(new wxBoxSizer(wxHORIZONTAL));
            view.swatch = new WorkbenchButton(view.panel, wxString::Format("%u", unsigned(id + 1)));
            view.swatch->SetMinSize(wxSize(row_height, row_height));
            view.swatch->SetMaxSize(wxSize(row_height, row_height));
            view.swatch->SetCornerRadius(FromDIP(4));
            view.swatch->SetName(wxString::Format("project_filament_color_%u", unsigned(id + 1)));
            view.preset = new WorkbenchButton(view.panel, wxEmptyString);
            view.preset->SetMinSize(wxSize(1, row_height));
            view.preset->SetMaxSize(wxSize(-1, row_height));
            view.preset->SetCornerRadius(FromDIP(7));
            view.preset->SetBorderWidth(0);
            view.preset->SetBackgroundColor(StateColor(
                std::pair<wxColour, int>(wxColour(94, 94, 98), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour(80, 80, 84), StateColor::Normal),
                std::pair<wxColour, int>(wxColour(58, 58, 63), StateColor::Disabled)));
            view.preset->SetTextColor(StateColor(
                std::pair<wxColour, int>(wxColour(236, 237, 240), StateColor::Normal),
                std::pair<wxColour, int>(wxColour(145, 145, 151), StateColor::Disabled)));
            view.actions = new WorkbenchButton(view.panel, _L("..."));
            view.actions->SetMinSize(wxSize(row_height, row_height));
            view.actions->SetMaxSize(wxSize(row_height, row_height));
            view.actions->SetCornerRadius(row_height / 2);
            view.actions->SetBorderWidth(0);
            view.actions->SetBackgroundColor(StateColor(
                std::pair<wxColour, int>(wxColour(86, 86, 91), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour(55, 55, 60), StateColor::Normal),
                std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Disabled)));
            view.actions->SetTextColor(StateColor(wxColour(190, 190, 196)));
            view.actions->SetToolTip(_L("更多耗材操作"));
            view.panel->GetSizer()->Add(view.swatch, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
            view.panel->GetSizer()->Add(view.preset, 1, wxALIGN_CENTER_VERTICAL);
            view.panel->GetSizer()->Add(view.actions, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
            return view;
        };
        for (const auto& slot : state.slots) {
            auto view = make_row(slot.slot, false);
            const wxWeakRef<ModelGenerationPanel> weak(this);
            auto edit = [weak, id=slot.slot] {
                if (weak && !weak->m_shutdown && weak->m_project_color_edit &&
                    weak->workbench_snapshot().can_edit_project_colors) weak->m_project_color_edit(id);
            };
            view.swatch->Bind(wxEVT_BUTTON,
                [weak, edit, native_filament_control, id=slot.slot, removable=slot.removable, swatch=view.swatch](wxCommandEvent&) {
                    if (!weak || weak->m_shutdown) return;
                    if (native_filament_control(id, NativeFilamentControl::Color, swatch)) {
                        wxWeakRef<ModelGenerationPanel> refresh(weak);
                        wxGetApp().CallAfter([refresh] {
                            if (refresh) refresh->synchronize_workbench_project_palette(true);
                        });
                        return;
                    }
                    wxMenu menu;
                    const int first = wxWindow::NewControlId(2);
                    menu.Append(first, _L("配置耗材"));
                    menu.Append(first + 1, _L("删除并替换引用"));
                    menu.Enable(first + 1, removable);
                    const int chosen = swatch->GetPopupMenuSelectionFromUser(menu);
                    if (chosen == first) edit();
                    else if (chosen == first + 1 && weak->m_project_filament_delete &&
                        weak->workbench_snapshot().can_edit_project_colors) weak->m_project_filament_delete(id);
                });
            view.preset->Bind(wxEVT_BUTTON,
                [weak, native_filament_control, id=slot.slot, preset=view.preset](wxCommandEvent&) {
                    if (!weak || weak->m_shutdown || !weak->workbench_snapshot().can_edit_project_colors) return;
                    native_filament_control(id, NativeFilamentControl::Preset, preset);
                    wxWeakRef<ModelGenerationPanel> refresh(weak);
                    wxGetApp().CallAfter([refresh] {
                        if (refresh) refresh->synchronize_workbench_project_palette(true);
                    });
                });
            view.actions->Bind(wxEVT_BUTTON,
                [weak, native_filament_control, id=slot.slot, actions=view.actions](wxCommandEvent&) {
                    if (!weak || weak->m_shutdown || !wxGetApp().plater() ||
                        !weak->workbench_snapshot().can_edit_project_colors) return;
                    native_filament_control(id, NativeFilamentControl::Actions, actions);
                    wxWeakRef<ModelGenerationPanel> refresh(weak);
                    wxGetApp().CallAfter([refresh] {
                        if (refresh) refresh->synchronize_workbench_project_palette(true);
                    });
                });
            grid->Add(view.panel, 0, wxEXPAND);
            m_workbench_filament_rows.push_back(std::move(view));
        }
        if (!state.mixed.empty()) {
            m_workbench_mixed_filament_title = new wxStaticText(
                m_workbench_project_colors, wxID_ANY, _L("叠色配方"));
            m_workbench_mixed_filament_title->SetForegroundColour(wxColour(190, 190, 196));
            contents->Add(m_workbench_mixed_filament_title, 0, wxTOP | wxBOTTOM, FromDIP(5));
            for (const auto& mix : state.mixed) {
                auto view = make_row(mix.slot, true);
                const wxWeakRef<ModelGenerationPanel> weak(this);
                view.swatch->Bind(wxEVT_BUTTON, [weak, id=mix.slot](wxCommandEvent&) {
                    auto* plater = wxGetApp().plater();
                    if (!weak || weak->m_shutdown || !plater ||
                        !weak->workbench_snapshot().can_edit_project_colors) return;
                    const auto panel = PrinterFilamentConfig::mixed_panel_index(
                        plater->mixed_filament_config_indices(), id);
                    if (!panel) return;
                    plater->sidebar().edit_mixed_filament(*panel);
                    wxWeakRef<ModelGenerationPanel> refresh(weak);
                    wxGetApp().CallAfter([refresh] {
                        if (refresh) refresh->synchronize_workbench_project_palette(true);
                    });
                });
                view.actions->Hide();
                if (mix.valid && bundle) {
                    const auto& config = bundle->project_config;
                    const auto ids = parse_mixed_components(
                        config.opt_string("filament_mixed_components", mix.slot));
                    view.components = new wxPanel(view.panel);
                    view.components->SetBackgroundColour(view.panel->GetBackgroundColour());
                    const int row_height = FromDIP(PrinterFilamentConfig::workbench_row_height_dip);
                    view.components->SetMinSize(wxSize(1, row_height));
                    view.components->SetMaxSize(wxSize(-1, row_height));
                    auto* wrap = new wxBoxSizer(wxHORIZONTAL);
                    view.components->SetSizer(wrap);
                    for (size_t i = 0; i < ids.size(); ++i) {
                        if (i) {
                            auto* plus = new wxStaticText(view.components, wxID_ANY, "+");
                            plus->SetForegroundColour(wxColour(190, 190, 196));
                            wrap->Add(plus, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
                        }
                        auto* chip = new WorkbenchButton(view.components, wxString::Format("%u", ids[i]));
                        chip->SetMinSize(FromDIP(wxSize(22, 22)));
                        chip->SetMaxSize(FromDIP(wxSize(22, 22)));
                        chip->SetCornerRadius(FromDIP(4));
                        const size_t component_slot = ids[i] ? size_t(ids[i] - 1) : size_t(-1);
                        chip->Bind(wxEVT_BUTTON, [weak, id=component_slot](wxCommandEvent&) {
                            if (id != size_t(-1) && weak && !weak->m_shutdown && weak->m_project_color_edit &&
                                weak->workbench_snapshot().can_edit_project_colors) weak->m_project_color_edit(id);
                        });
                        wrap->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
                        auto* ratio = new wxStaticText(view.components, wxID_ANY, wxEmptyString);
                        ratio->SetMinSize(FromDIP(wxSize(30, -1)));
                        ratio->SetForegroundColour(wxColour(190, 190, 196));
                        wrap->Add(ratio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
                        view.component_slots.push_back(component_slot);
                        view.component_swatches.push_back(chip);
                        view.component_ratios.push_back(ratio);
                        view.component_colors.emplace_back("\x01");
                        view.component_tooltips.emplace_back();
                        view.component_percentages.push_back(-1);
                    }
                    view.panel->GetSizer()->Replace(view.preset, view.components, true);
                    view.preset->Hide();
                }
                contents->Add(view.panel, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
                m_workbench_filament_rows.push_back(std::move(view));
            }
        }
        m_workbench_filament_layout = layout;
    }

    auto find_row = [&](size_t slot, bool mixed) -> WorkbenchFilamentRowView* {
        const auto found = std::find_if(m_workbench_filament_rows.begin(), m_workbench_filament_rows.end(),
            [slot, mixed](const auto& row) { return row.slot == slot && row.mixed == mixed; });
        return found == m_workbench_filament_rows.end() ? nullptr : &*found;
    };
    auto update_swatch = [&](WorkbenchButton* swatch, std::string& cached, const std::string& value) {
        if (!swatch || cached == value) return;
        wxColour color(wxString::FromUTF8(value));
        if (!color.IsOk()) color = wxColour(65, 65, 70);
        swatch->SetBackgroundColor(StateColor(color));
        const double luminance = 0.2126 * color.Red() + 0.7152 * color.Green() + 0.0722 * color.Blue();
        swatch->SetTextColor(StateColor(luminance > 140 ? wxColour(22, 22, 25) : wxColour(245, 245, 245)));
        cached = value;
    };
    auto update_text = [](WorkbenchButton* control, wxString& cached, const wxString& value) {
        if (control && cached != value) {
            control->SetLabel(value);
            cached = value;
        }
    };
    auto update_tooltip = [](wxWindow* control, wxString& cached, const wxString& value) {
        if (control && cached != value) {
            control->SetToolTip(value);
            cached = value;
        }
    };
    auto update_enabled = [&](WorkbenchFilamentRowView& row) {
        if (row.panel->IsEnabled() != snapshot.can_edit_project_colors)
            row.panel->Enable(snapshot.can_edit_project_colors);
        row.enabled = snapshot.can_edit_project_colors;
    };
    for (const auto& slot : state.slots) {
        auto* row = find_row(slot.slot, false);
        if (!row) continue;
        const wxString summary = slot.configured ? wxString::FromUTF8(slot.preset) : _L("待配置真实耗材");
        const wxString tooltip = summary + "\n" + wxString::FromUTF8(slot.material + " · " + slot.color +
            (slot.reason.empty() ? " · 已配置" : " · " + slot.reason));
        update_swatch(row->swatch, row->color, slot.color);
        update_text(row->preset, row->label, summary);
        if (row->tooltip != tooltip) {
            row->swatch->SetToolTip(tooltip);
            row->preset->SetToolTip(tooltip);
            row->tooltip = tooltip;
        }
        update_enabled(*row);
    }
    for (const auto& mix : state.mixed) {
        auto* row = find_row(mix.slot, true);
        if (!row) continue;
        const wxString text = wxString::FromUTF8(mix.recipe);
        const wxString tooltip = text + "\n" + _L("预估叠色；实物色准需打印验证") +
            "\n" + wxString::FromUTF8(mix.reason);
        update_swatch(row->swatch, row->color, mix.color);
        update_text(row->preset, row->label, text);
        if (row->tooltip != tooltip) {
            row->swatch->SetToolTip(text + "\n" + _L("预估叠色；实物色准需打印验证"));
            row->preset->SetToolTip(tooltip);
            row->tooltip = tooltip;
        }
        if (row->components && bundle) {
            const auto ratios = parse_mixed_ratios(
                bundle->project_config.opt_string("filament_mixed_sublayer_ratios", mix.slot),
                row->component_slots.size());
            for (size_t i = 0; i < row->component_slots.size(); ++i) {
                const auto found = std::find_if(state.slots.begin(), state.slots.end(),
                    [&](const auto& slot) { return slot.slot == row->component_slots[i]; });
                if (found != state.slots.end()) {
                    update_swatch(row->component_swatches[i], row->component_colors[i], found->color);
                    const wxString component_tooltip = wxString::FromUTF8(found->preset + " · " + found->color);
                    update_tooltip(row->component_swatches[i], row->component_tooltips[i], component_tooltip);
                }
                const int percentage = i < ratios.size() ? int(std::lround(ratios[i] * 100)) : 0;
                if (row->component_percentages[i] != percentage) {
                    row->component_ratios[i]->SetLabel(wxString::Format("%d%%", percentage));
                    row->component_percentages[i] = percentage;
                }
            }
        }
        update_enabled(*row);
    }
    if (rebuild) {
        const int natural_height = m_workbench_project_colors->GetSizer()->CalcMin().y;
        m_workbench_project_colors->SetMinSize(wxSize(1, natural_height));
        m_workbench_project_colors->SetMaxSize(wxDefaultSize);
        m_workbench_project_colors->InvalidateBestSize();
        m_workbench_project_colors->Layout();
        m_workbench_settings_scroll->Layout();
        m_workbench_settings_scroll->FitInside();
    }
    refresh_post_generation_workbench();
    publish_workbench_state();
}

} // namespace Slic3r::GUI
