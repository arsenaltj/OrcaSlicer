#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
constexpr int beauty_finishing_tools[] = {4, 1, 3, 5, 1};
// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
void wrap_workbench_text(wxStaticText* label, int width, bool preserve_lines = false)
{
    wxString source = label->GetLabel(), line, result;
    source.Replace("\r", "");
    if (!preserve_lines) source.Replace("\n", "");
    for (wxUniChar character : source) {
        if (character == '\n') {
            result += line + "\n"; line.clear();
            continue;
        }
        wxString next = line; next += character;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            result += line + "\n"; line.clear();
        }
        line += character;
    }
    const wxString wrapped = result + line;
    label->SetLabel(wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    label->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + label->FromDIP(2)));
}

void apply_workbench_theme(wxWindow* window, bool enabled,
                           const wxColour& enabled_surface = wxColour(32, 32, 35))
{
    if (!window) return;
    const wxColour surface = enabled ? enabled_surface : *wxWHITE;
    const wxColour text = enabled ? wxColour(238, 240, 244) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    window->SetBackgroundColour(surface);
    window->SetForegroundColour(text);
    for (wxWindow* child : window->GetChildren())
        apply_workbench_theme(child, enabled, enabled_surface);
    if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
}

void apply_workbench_parameter_theme(wxWindow* parameters)
{
    apply_workbench_theme(parameters, true, wxColour(40, 40, 43));
}

void repaint_workbench_surface(wxWindow* window)
{
    if (!window || !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh();
    window->Update();
    for (wxWindow* child : window->GetChildren())
        repaint_workbench_surface(child);
}
}
void ModelGenerationPanel::preview_model_finishing()
{
    if (m_busy || m_shutdown || !m_model_preview_ready) return;
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，完成后即可预览；可按 Esc 取消选区计算。")); return;
    }
    if (m_model_preview->leaf_editing()) {
        if (m_finishing_tool->GetSelection()!=4) {
            m_finishing_status->SetLabel(_L("子面草稿保留原几何；当前可选择颜色，几何处理需要先解除锁定并返回原面。")); return;
        }
        const auto palette=local_recolor_palette();
        if (m_region_color_index<0 || size_t(m_region_color_index)>=palette.size()) return;
        const wxColour target(from_u8(palette[m_region_color_index]));
        if (!target.IsOk()) return;
        const auto before=m_model_preview->leaf_edit_metadata();
        if (!m_model_preview->paint_selected_leaves({target.Red()/255.f,target.Green()/255.f,target.Blue()/255.f,1.f})) return;
        const auto after=m_model_preview->leaf_edit_metadata();
        if (m_beauty_controls && m_beauty_controls->on_record)
            m_beauty_controls->on_record("leaf color draft",[this,before]{m_model_preview->restore_leaf_edits(before);},
                [this,after]{m_model_preview->restore_leaf_edits(after);});
        if (m_beauty_controls) m_beauty_controls->set_dirty(true);
        m_model_preview->set_selection_preview_suppressed(true);
        m_finishing_status->SetLabel(_L("子面颜色草稿已更新，原面和未选子面保持不变。"));
        refresh_model_finishing(); return;
    }
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，完成后即可预览；可按 Esc 取消选区计算。")); return;
    }
    const bool cleanup = m_finishing_tool->GetSelection() == 5;
    const bool recolor = m_finishing_tool->GetSelection() == 4;
    const bool local = m_finishing_tool->GetSelection() == 1 || cleanup || recolor;
    // Beauty edits always operate on the currently visible candidate and its
    // current selection. The legacy path keeps its saved selection snapshot
    // while a candidate is pending.
    auto selected_faces = local ? ((m_finishing_workbench || m_finishing_candidate.empty())
        ? m_model_preview->selected_face_indices() : m_finishing_options.selected_faces) : std::vector<size_t>{};
    if (local && selected_faces.empty()) {
        m_finishing_status->SetLabel(cleanup ? _L("请先圈住杂点及周围主色，涂抹保护花纹等细节；未选区域不会改变。")
            : recolor ? _L("请先圈选要换色的区域；涂抹保护可排除需要保留的细节。")
            : _L("请先在模型上选择要柔化的区域；未选区域不会改变。"));
        wrap_workbench_text(m_finishing_status,
            std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
        m_finishing_panel->Layout();
        return;
    }
    const auto source = m_finishing_workbench && !m_finishing_candidate.empty()
        ? m_finishing_candidate : m_displayed_model_path;
    if (!is_nonempty_model(source)) {
        m_finishing_status->SetLabel(_L("模型文件已不存在，请从模型库重新加载。")); return;
    }
    auto before = m_finishing_workbench
        ? std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate()) : nullptr;
    if (before && m_finishing_candidate.empty() && !m_beauty_session_source) {
        m_beauty_session_source = before;
        m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
        m_beauty_session_file_base = m_beauty_candidate_files.size();
    }
    if (m_finishing_candidate.empty() || m_finishing_workbench) {
        m_finishing_selection_state = m_model_preview->selection_state();
        m_finishing_restore_selection = [this, selection_state = m_finishing_selection_state] {
            m_model_preview->restore_selection_state(selection_state);
        };
    }
    const int tool = m_finishing_tool->GetSelection();
    const bool geometry_smoothing = m_finishing_workbench && tool == 0;
    // Workbench operations are exclusive; hidden legacy checkboxes may still
    // contain the values from the result page.
    const bool smooth = m_finishing_workbench ? (tool == 0 || tool == 1) : m_finishing_smooth->GetValue();
    const bool repair = m_finishing_workbench ? tool == 3 : m_finishing_repair->GetValue();
    AI::ModelFinishingOptions options {!cleanup && !recolor && smooth, !local && repair, m_finishing_strength->GetValue() / 100.0};
    if (m_finishing_workbench && m_workbench_smoothing_iterations && m_workbench_preserve_hard_edges) {
        options.smoothing_iterations = geometry_smoothing ? m_workbench_smoothing_iterations->GetValue() : 0;
        options.preserve_hard_edges = !geometry_smoothing || m_workbench_preserve_hard_edges->GetValue();
    }
    options.selected_faces = selected_faces;
    if (m_finishing_workbench && m_model_preview->active_shape_locks()) {
        auto constrained = m_finishing_selection_state;
        m_model_preview->constrain_shape_selection(constrained);
        options.selected_faces.clear();
        for (size_t face = 0; face < constrained.selected.size(); ++face)
            if (constrained.selected[face]) options.selected_faces.push_back(face);
        selected_faces = options.selected_faces;
    }
    options.clean_color_spots = cleanup;
    options.recolor_selected = recolor;
    if (AI::model_artifact_format(source) == "glb" && (options.repair_mesh || cleanup)) {
        m_finishing_status->SetLabel(cleanup
            ? _L("GLB 的保真保存暂不支持清理杂点。可圈选后统一这块颜色，并保留原版用于对照。")
            : _L("为保留 GLB 原始贴图，请在这里选择表面柔化。需要修复网格时，可先导入准备页，再使用修复功能。"));
        wrap_workbench_text(m_finishing_status,
            std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
        m_finishing_panel->Layout();
        return;
    }
    if (recolor) {
        m_finishing_color_palette = local_recolor_palette();
        if (m_region_color_index < 0 || size_t(m_region_color_index) >= m_finishing_color_palette.size()) {
            m_finishing_status->SetLabel(_L("请先选择一个可用耗材颜色。")); return;
        }
        const wxColour target(from_u8(m_finishing_color_palette[m_region_color_index]));
        if (!target.IsOk()) return;
        options.target_color = {target.Red()/255.0f, target.Green()/255.0f, target.Blue()/255.0f, 1.0f};
        // Embedded-texture GLBs use the Beauty appearance path so local color
        // edits do not collapse the source texture into vertex colors. The
        // existing face overrides are still composed by ModelPreview3D after
        // the Beauty artifact is loaded.
        if (AI::model_artifact_format(source) == "glb") {
            const auto editor = m_model_preview->beauty_editor();
            if (!editor) {
                m_finishing_status->SetLabel(_L("正在准备局部 Beauty 编辑数据，请稍后重试。"));
                return;
            }
            auto surface = AI::BeautySurface::build(editor->mesh(), editor->vertex_colors());
            AI::BeautyDocument document;
            document.geometry_id = m_model_preview->geometry_id();
            document.face_count = editor->mesh().indices.size();
            document.face_patch = surface->face_patch;
            options.beauty_appearance = true;
            options.recolor_selected = false;
            options.smooth_surface = false;
            options.repair_mesh = false;
            options.beauty_surface = std::move(surface);
            options.beauty_document = document.encode();
            options.appearance.face_weights.assign(document.face_count, 0.f);
            options.appearance.face_target_colors.resize(document.face_count);
            for (size_t face : options.selected_faces)
                if (face < options.appearance.face_weights.size() &&
                    (face >= m_finishing_selection_state.protected_faces.size() ||
                     !m_finishing_selection_state.protected_faces[face])) {
                    options.appearance.face_weights[face] = 1.f;
                    options.appearance.face_target_colors[face] = {
                        options.target_color[0], options.target_color[1], options.target_color[2]};
                }
            options.beauty_protected_faces = m_finishing_selection_state.protected_faces;
        }
    }
    if (cleanup) options.cleanup_palette = m_finishing_candidate.empty()
        ? m_model_preview->color_trial_mapping().mapping_colors : m_finishing_options.cleanup_palette;
    options.selected_faces = std::move(selected_faces);
    if (m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()) {
        if (AI::model_artifact_format(source) != "glb" || !m_beauty_controls->ready() ||
            m_beauty_controls->displacement_mm() == 0.) {
            m_finishing_status->SetLabel(_L("局部拉伸需要 GLB、有效选区和非零推拉距离。"));
            return;
        }
        m_beauty_controls->prepare_options(options, m_finishing_selection_state);
        options.beauty_appearance = false;
        options.beauty_deform = true;
        options.beauty_displacement_mm = m_beauty_controls->displacement_mm();
        options.beauty_falloff_mm = m_beauty_controls->falloff_mm();
    }
    if (!options.smooth_surface && !options.repair_mesh && !options.clean_color_spots && !recolor && !options.beauty_deform) {
        m_finishing_status->SetLabel(_L("请至少选择表面美化或网格修复。")); return;
    }
    if (!m_finishing_candidate.empty() && !m_finishing_workbench) {
        if (!m_finishing_before && !show_finishing_version(m_finishing_source)) return;
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    const auto color_state = m_model_preview->color_trial_state();
    const auto automatic_semantic_faces = m_finishing_workbench && m_model_preview->semantic_result_active()
        ? m_model_preview->import_face_color_overrides(true) : ModelPreview3D::FaceColorOverrides {};
    const auto automatic_semantic_subfaces = m_finishing_workbench && m_model_preview->semantic_result_active()
        ? m_model_preview->import_subface_color_overrides(true) : ModelPreview3D::SubfaceColorOverrides {};
    auto face_overrides = options.repair_mesh ? ModelPreview3D::FaceColorOverrides {} : m_model_preview->face_color_overrides();
    if (cleanup && !face_overrides.empty()) {
        std::unordered_set<size_t> locked;
        for (const auto& entry : face_overrides) locked.insert(entry.first);
        auto& faces = options.selected_faces;
        faces.erase(std::remove_if(faces.begin(), faces.end(), [&](size_t face) { return locked.count(face) != 0; }), faces.end());
        if (faces.empty()) {
            m_finishing_status->SetLabel(_L("选区均为已指定颜色的表面，清理杂点会保留这些颜色。可用“统一这块颜色”重新指定。"));
            refresh_controls(); return;
        }
    }
    if (recolor) {
        std::map<size_t, std::array<float,3>> colors(face_overrides.begin(), face_overrides.end());
        for (size_t face : options.selected_faces) {
            colors[face] = {options.target_color[0], options.target_color[1], options.target_color[2]};
        }
        face_overrides.assign(colors.begin(), colors.end());
    }
    const bool intent_changed = face_overrides != m_model_preview->face_color_overrides();
    if (m_finishing_workbench && m_model_preview->semantic_result_active()) {
        const auto current_provenance = m_model_preview->semantic_color_metadata();
        if (!current_provenance.empty()) m_finishing_candidate_semantic_provenance = current_provenance;
        m_finishing_candidate_semantic_faces = AI::SemanticColoring::compose(
            automatic_semantic_faces, face_overrides, !automatic_semantic_faces.empty());
        m_finishing_candidate_semantic_subfaces = AI::SemanticColoring::compose_subfaces(
            automatic_semantic_subfaces, face_overrides, !automatic_semantic_subfaces.empty());
    } else {
        m_finishing_candidate_semantic_faces.clear();
        m_finishing_candidate_semantic_subfaces.clear();
        m_finishing_candidate_semantic_provenance = nlohmann::json::object();
    }
    const auto kind = options.beauty_deform ? BeautyWorkbenchTransactionController::OperationKind::SurfaceSoften
            : recolor ? BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor
            : cleanup ? BeautyWorkbenchTransactionController::OperationKind::SpotCleanup
            : options.repair_mesh ? BeautyWorkbenchTransactionController::OperationKind::MeshRepair
            : BeautyWorkbenchTransactionController::OperationKind::SurfaceSoften;
    if (m_finishing_workbench && m_beauty_transactions) {
        if (!m_beauty_transactions->begin(kind)) {
            m_finishing_status->SetLabel(_L("当前仍有 Beauty 处理正在进行，请先完成或取消。"));
            refresh_controls();
            return;
        }
    }
    m_finishing_candidate_face_overrides = face_overrides;
    m_finishing_options = options;
    m_finishing_redo_path.clear();
    if (m_finishing_source.empty() || !m_finishing_workbench) {
    m_finishing_source = source;
    m_finishing_source_context = capture_model_context();
    }
    m_finishing_id = "finish-" + new_request_id();
    const auto destination = source.parent_path() / temp_path(m_finishing_id, AI::model_artifact_format(source)).filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    m_beauty_publication_committed.reset();
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true; m_busy = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_finishing_cancel->Enable();
    m_finishing_status->SetLabel(recolor ? _L("正在生成局部颜色预览，可取消；选区之外保持原样……") : cleanup ? _L("正在清理选区内的小杂色块，可取消……") : _L("正在本地处理三维表面，原始模型保持不变……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    try {
      m_finishing_worker = std::thread([weak, source, destination, options, canceled, sequence, color_state, intent_changed, before, kind, recolor, cleanup, face_overrides = std::move(face_overrides)] {
        const auto result = AI::finish_model_artifact(source, destination, options, [canceled] { return canceled->load(); });
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        std::string preview_error;
        if (result.success && (result.changed() || intent_changed) && !canceled->load()) {
            try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, face_overrides, {}, true,
                [canceled] { return canceled->load(); }); }
            catch (const std::exception& e) { preview_error = e.what(); }
        }
        wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled, prepared, preview_error, color_state, intent_changed, before, kind, recolor, cleanup] {
            if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                return;
            }
            auto* self = weak.get();
            if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
            self->m_finishing_running = false; self->m_busy = false;
            self->m_finishing_result = result;
            if (result.canceled || canceled->load()) {
                self->m_save_and_return = false;
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "cancelled");
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(_L("已取消，原始模型保持不变。"));
            }
            else if (!result.success) {
                self->m_save_and_return = false;
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, result.error);
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(_L("处理未完成，原件已保留：") + from_u8(result.error));
            }
            else if (!result.changed() && !intent_changed) {
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "no change");
                boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(recolor
                    ? _L("选区已经是这个颜色，无需重复保存。可以选择其他颜色或继续编辑范围。")
                    : cleanup
                    ? _L("未找到可合并的小杂色块。可扩大选区包含周围主色，或用“统一这块颜色”处理连续色带。")
                    : _L("当前设置没有改变模型；可扩大选区或调整强度，边界与锐边保持保护。"));
            } else {
                const auto view = self->m_model_preview->view_state();
                size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error = preview_error;
                if (!error.empty() || !self->m_model_preview->load_prepared_model(
                    std::move(*prepared), {}, triangles, dimensions, colors, error)) {
                    self->m_save_and_return = false;
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, error);
                    boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                    if (before) self->restore_beauty_candidate(*before);
                    self->m_finishing_status->SetLabel(_L("预览未完成，原件已保留：") + from_u8(error));
                    self->refresh_controls(); return;
                }
                self->m_model_preview->restore_view(view);
                if (before) self->m_model_preview->restore_semantic_region_evidence(before->region_evidence, before->region_evidence_error);
                if (before) self->m_model_preview->restore_portrait_shapes(before->shape_details, before->shapes_unlocked);
                self->m_finishing_candidate_region_evidence = self->m_model_preview->semantic_region_evidence();
                self->m_finishing_candidate_region_error = self->m_model_preview->semantic_region_evidence_error();
                if (before) self->m_model_preview->transfer_secondary_region_evidence(
                    before->secondary_evidence, before->model_sha256);
                self->m_finishing_candidate_secondary_evidence = self->m_model_preview->secondary_region_evidence();
                self->m_finishing_candidate_secondary_error = self->m_model_preview->secondary_region_evidence_error();
                if (self->m_finishing_workbench &&
                    (!self->m_finishing_candidate_semantic_faces.empty() ||
                     !self->m_finishing_candidate_semantic_subfaces.empty())) {
                    self->m_model_preview->restore_color_trial_without_recognition(color_state);
                    if (!self->m_model_preview->set_saved_semantic_result(
                        self->m_finishing_candidate_semantic_faces,
                        self->m_finishing_candidate_semantic_subfaces)) {
                        self->m_save_and_return = false;
                        boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                        if (before) self->restore_beauty_candidate(*before);
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "semantic preview unavailable");
                        self->m_finishing_status->SetLabel(_L("语义预览无法加载，已保留处理前版本。"));
                        self->refresh_controls(); return;
                    }
                } else if (self->m_finishing_workbench)
                    self->m_model_preview->restore_color_trial_without_recognition(color_state);
                else self->m_model_preview->restore_color_trial(color_state);
                // Match the before-view summary before exposing comparison:
                // a stale loading row changes the viewport height on first compare.
                self->m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                    static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
                    dimensions.x(), dimensions.y(), dimensions.z()));
                self->m_finishing_candidate = destination; self->m_finishing_before = false;
                if (self->m_finishing_workbench)
                    self->m_model_preview->set_selection_preview_suppressed(true);
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(true, true);
                if (self->m_finishing_workbench) self->refresh_workbench_history();
                if (before) {
                    if (before->geometry_id == self->m_model_preview->geometry_id() &&
                        before->selection.selected.size() == self->m_model_preview->triangle_count())
                        self->m_model_preview->restore_selection_state(before->selection);
                    self->record_beauty_candidate(kind, before);
                }
                self->m_finishing_compare->SetLabel(_L("查看处理前"));
                self->m_model_preview_message->SetLabel(_L("处理后 · 尚未接受；可旋转模型并查看处理前对比。"));
                self->m_finishing_status->SetLabel(recolor && self->m_finishing_options.beauty_appearance
                    ? _L("局部颜色候选已生成。选区外与造型保持不变；对比后接受，或放弃预览继续调整范围。")
                    : recolor ? wxString::Format(
                    _L("已统一 %llu 个面的颜色。选区外与造型保持不变；对比后接受，或放弃预览继续调整范围。"),
                    static_cast<unsigned long long>(result.recolored_faces)) : cleanup ? wxString::Format(
                    _L("已清理 %llu 处小杂色块，调整 %llu 个顶点颜色。造型不变；请对比细节后接受新版本。"),
                    static_cast<unsigned long long>(result.cleaned_color_regions),
                    static_cast<unsigned long long>(result.recolored_vertices)) : self->m_finishing_options.beauty_deform ? wxString::Format(
                    _L("已拉伸 %llu 个顶点。请对比处理前后的局部造型，接受或放弃候选版本。"),
                    static_cast<unsigned long long>(result.moved_vertices)) : wxString::Format(
                    _L("已柔化 %llu 个顶点，清理 %llu 个面，校正 %llu 个面。开放边 %llu 条，非流形边 %llu 条（仅作提醒）。"),
                    static_cast<unsigned long long>(result.moved_vertices),
                    static_cast<unsigned long long>(result.removed_degenerate_faces + result.removed_duplicate_faces),
                    static_cast<unsigned long long>(result.reversed_faces),
                    static_cast<unsigned long long>(result.boundary_edges),
                    static_cast<unsigned long long>(result.nonmanifold_edges)));
            }
            self->m_status->SetLabel(self->m_finishing_status->GetLabel());
            self->refresh_controls();
            self->update_finishing_selection();
            if (self->m_finishing_candidate.empty() && self->m_finishing_restore_selection)
                self->m_finishing_restore_selection();
        });
      });
    } catch (const std::exception&) {
        m_finishing_running = false; m_busy = false;
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "worker start failed");
        m_finishing_status->SetLabel(_L("暂时无法启动处理，请稍后重试。原始模型保持不变。"));
        refresh_controls();
    }
}
} // namespace Slic3r::GUI
