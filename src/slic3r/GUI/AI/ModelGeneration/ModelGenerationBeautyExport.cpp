#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/BeautyCellRemap.hpp"
#include "slic3r/GUI/AI/Model/BakedPortraitAppearance.hpp"
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
void ModelGenerationPanel::export_semantic_candidate()
{
    const auto portrait_task = m_portrait_task && m_portrait_task->snapshot().running() ? m_portrait_task : nullptr;
    if (portrait_task) portrait_task->report({PortraitStage::Saving, "校验颜色与连续裁切，准备保存候选",0,0});
    if (!m_model_preview || (!m_model_preview->semantic_regions_ready() && !m_model_preview->leaf_editing()) ||
        !is_nonempty_model(m_finishing_candidate.empty() ? m_displayed_model_path : m_finishing_candidate)) {
        if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic candidate source unavailable");
        if (m_finishing_status) m_finishing_status->SetLabel(_L("人像区域结果没有可用的模型源，当前版本保持不变。"));
        finish_portrait_optimization(PortraitOutcome::Failed, _L("人像区域结果没有可用的模型源，当前版本保持不变。"));
        refresh_model_finishing();
        return;
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    const auto source = m_finishing_candidate.empty() ? m_displayed_model_path : m_finishing_candidate;
    const auto region_evidence = m_model_preview->semantic_region_evidence();
    const auto secondary_evidence = m_model_preview->secondary_region_evidence();
    const auto shape_details = m_model_preview->portrait_shape_details();
    const bool shapes_unlocked = m_model_preview->portrait_shapes_unlocked();
    const auto leaf_edits=m_model_preview->leaf_edit_metadata();
    const auto leaf_revision=m_model_preview->leaf_edit_revision();
    const auto partition_snapshot=m_beauty_controls ? m_beauty_controls->capture_partition() : nullptr;
    const auto draft_snapshot=std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    auto before = m_beauty_reoptimization_before ? std::move(m_beauty_reoptimization_before)
        : std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    if (m_finishing_candidate.empty() && !m_beauty_session_source) {
        m_beauty_session_source = before;
        m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
        m_beauty_session_file_base = m_beauty_candidate_files.size();
    }
    const size_t faces = m_model_preview->triangle_count();
    const auto overrides = m_model_preview->import_face_color_overrides(true);
    const auto subfaces = m_model_preview->import_subface_color_overrides(true);
    const auto cell_colors=m_model_preview->import_cell_color_overrides();
    const auto baked_semantic=m_model_preview->semantic_result_metadata();
    const auto color_state = m_model_preview->color_trial_state();
    // Automatic semantic colors are baked into the candidate appearance. Keep
    // only explicit manual face locks as preview overrides so a later explicit
    // re-optimization can still replace automatic colors without overriding
    // the user's manual edits.
    const auto manual_overrides = m_model_preview->face_color_overrides();
    if (faces == 0 || (overrides.empty() && subfaces.empty() && cell_colors.empty())) {
        restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic result has no face colors");
        m_finishing_status->SetLabel(_L("人像区域没有产生可靠的面级颜色，当前模型保持不变。"));
        finish_portrait_optimization(PortraitOutcome::Failed, _L("人像区域没有产生可靠的面级颜色，当前模型保持不变。"));
        refresh_model_finishing();
        return;
    }
    AI::ModelFinishingOptions options;
    options.beauty_appearance = true;
    options.smooth_surface = false;
    options.repair_mesh = false;
    options.recolor_selected = false;
    options.selected_faces.reserve(overrides.size());
    options.appearance.face_weights.assign(faces, 0.f);
    options.appearance.face_target_colors.resize(faces, {0.f, 0.f, 0.f});
    for (const auto& item : overrides) {
        if (item.first >= faces) continue;
        options.selected_faces.push_back(item.first);
        options.appearance.face_weights[item.first] = 1.f;
        options.appearance.face_target_colors[item.first] = item.second;
    }
    if (!subfaces.empty()) {
        AI::appearance_subface_colors(options.appearance,subfaces,m_model_preview->geometry_id());
        for (const auto& leaf:options.appearance.leaves) if (leaf.weight>0) options.selected_faces.push_back(leaf.key.source_face_id);
        std::sort(options.selected_faces.begin(),options.selected_faces.end());
        options.selected_faces.erase(std::unique(options.selected_faces.begin(),options.selected_faces.end()),options.selected_faces.end());
    }
    if(shape_details && shape_details->surface_partition) {
        for(const auto& face:shape_details->surface_partition->at("faces"))
            for(const auto& cell:face.at("cells")) if(cell_colors.count(cell.at("id").get<std::string>()))
                options.selected_faces.push_back(face.at("source_face_id").get<size_t>());
        std::sort(options.selected_faces.begin(),options.selected_faces.end());
        options.selected_faces.erase(std::unique(options.selected_faces.begin(),options.selected_faces.end()),options.selected_faces.end());
    }
    if (options.selected_faces.empty()) {
        restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic face colors are out of range");
        m_finishing_status->SetLabel(_L("人像区域颜色与当前模型面数不一致，已保留原模型。"));
        finish_portrait_optimization(PortraitOutcome::Failed, _L("人像区域颜色与当前模型面数不一致，已保留原模型。"));
        refresh_model_finishing();
        return;
    }
    AI::BeautyDocument document;
    document.geometry_id = m_model_preview->geometry_id();
    document.face_count = faces;
    document.face_patch.assign(faces, 0);
    options.beauty_document = document.encode();
    m_finishing_options = options;
    m_finishing_candidate_face_overrides = manual_overrides;
    m_finishing_candidate_semantic_faces = overrides;
    m_finishing_candidate_semantic_subfaces = subfaces;
    m_finishing_candidate_semantic_provenance = m_model_preview->semantic_color_metadata();
    // Keep the original stable source for the candidate lifecycle. When a
    // candidate is re-optimized, its predecessor remains temporary and must
    // not become the new invalidation anchor.
    if (m_finishing_source.empty()) {
        m_finishing_source_context = capture_model_context();
        m_finishing_source = source;
    }
    m_finishing_id = "finish-semantic-" + new_request_id();
    m_finishing_candidate_baked_appearance = nullptr;
    const auto destination = temp_path(m_finishing_id, "glb");
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    m_beauty_publication_committed.reset();
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true;
    m_busy = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_finishing_status->SetLabel(_L("正在把人像语义结果写入新的 GLB 版本，可旋转查看或取消……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    const auto cache_root=boost::filesystem::path(Slic3r::data_dir())/"cache";
    try {
        m_finishing_worker = std::thread([weak, source, destination, options, canceled, sequence, manual_overrides, color_state, before, region_evidence, secondary_evidence, shape_details, shapes_unlocked,leaf_edits,leaf_revision,partition_snapshot,portrait_task,cell_colors,cache_root,draft_snapshot,baked_semantic] {
            auto saved_shapes=shape_details;
            auto saved_edits=leaf_edits;
            auto output_options=options;
            AI::ModelFinishingResult result;
            try {
                if(shape_details && shape_details->surface_partition) {
                    if(portrait_task) portrait_task->report({PortraitStage::Saving,"校验草稿与裁切网格",0,0});
                    auto rebuilt=AI::SurfacePartition::rebuild_tessellation(*shape_details->surface_partition,
                        [canceled]{return canceled->load();},[portrait_task](size_t done,size_t total){
                            if(portrait_task) portrait_task->report({PortraitStage::Saving,"重建裁切网格与校验接缝",done,total});
                        });
                    if(rebuilt!=*shape_details->surface_partition) {
                        auto copy=std::make_shared<PortraitShapeDetails>(*shape_details);
                        copy->surface_partition=std::make_shared<const nlohmann::json>(std::move(rebuilt));
                        auto locks=shape_details->contour_locks->document;
                        const auto hash=AI::beauty_leaf_digest(copy->surface_partition->dump());
                        locks["partition_ref"]={{"schema","orca.surface-partition-reference/v1"},
                            {"path","surface-partitions/"+hash+".json"},{"sha256",hash}};
                        copy->contour_locks=std::make_shared<const AI::BeautySurfaceShapeLock>(AI::BeautySurfaceShapeLock::decode(
                            locks,*copy->surface_partition,AI::BeautySurfaceShapeLock::identity(*copy->surface_partition),hash));
                        saved_edits=AI::remap_cell_edits(leaf_edits,*shape_details->surface_partition,*copy->surface_partition,
                            shape_details->boundary_fingerprint(),copy->boundary_fingerprint());
                        if(canceled->load()) throw std::runtime_error("Contour mesh rebuild cancelled.");
                        PortraitShapeCache::save(*copy,cache_root);
                        saved_shapes=std::move(copy);
                    }
                    AI::appearance_cell_colors(output_options.appearance,*saved_shapes->surface_partition,cell_colors,
                        saved_shapes->locks.geometry_id);
                }
                if(portrait_task) portrait_task->report({PortraitStage::Saving,"裁切校验完成，正在烘焙 GLB",0,0});
                result=AI::finish_model_artifact(source,destination,output_options,[canceled]{return canceled->load();});
            } catch(const std::exception& e) { result.error=e.what();result.canceled=canceled->load(); }
            if (portrait_task && result.success) portrait_task->report({PortraitStage::Preview,"候选已写出，解析预览模型",0,0});
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string preview_error;
            if (result.success && !canceled->load()) {
                try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, manual_overrides, {}, true,
                    [canceled] { return canceled->load(); }); }
                catch (const std::exception& e) { preview_error = e.what(); }
            }
            const auto baked=AI::baked_portrait_appearance(result.output_sha256,baked_semantic,cell_colors,
                saved_shapes && saved_shapes->surface_partition ? saved_shapes->surface_partition->at("partition_sha256").get<std::string>() : std::string());
            wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled, prepared, preview_error, color_state, before, region_evidence, secondary_evidence, shape_details=saved_shapes, shapes_unlocked,leaf_edits=saved_edits,leaf_revision,partition_snapshot,portrait_task,draft_snapshot,output_options,baked] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    return;
                }
                auto* self = weak.get();
                if (portrait_task && self->m_portrait_task != portrait_task) return;
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = false;
                self->m_busy = false;
                if (self->m_model_preview->leaf_edit_revision()!=leaf_revision) {
                    self->m_save_and_return = false;
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination,ignored); }
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false,false,"leaf draft changed during export");
                    self->m_finishing_status->SetLabel(_L("草稿在保存期间已更新，已丢弃过期输出；请重新保存。"));
                    self->m_portrait_draft_before = before;
                    self->finish_portrait_optimization(PortraitOutcome::DraftOnly, self->m_finishing_status->GetLabel());
                    self->preserve_portrait_draft();
                    self->refresh_controls(); return;
                }
                self->m_finishing_result = result;
                self->m_finishing_options=output_options;
                if (result.canceled || canceled->load()) {
                    self->m_save_and_return = false;
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    self->restore_beauty_candidate(*draft_snapshot);
                    self->m_portrait_draft_before=before;
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "cancelled");
                    self->m_finishing_status->SetLabel(_L("已取消语义 GLB 写出，当前模型保持不变。"));
                    self->finish_portrait_optimization(PortraitOutcome::Cancelled, self->m_finishing_status->GetLabel());
                    self->preserve_portrait_draft();
                } else if (!result.success || !preview_error.empty()) {
                    self->m_save_and_return = false;
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    const bool keep_contour_draft=shape_details && shape_details->surface_partition;
                    if(!keep_contour_draft) self->restore_beauty_candidate(*before);
                    else {
                        self->m_portrait_draft_before = before;
                        if (self->m_beauty_controls) self->m_beauty_controls->set_dirty(true);
                    }
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false,
                        preview_error.empty() ? result.error : preview_error);
                    self->m_finishing_status->SetLabel((keep_contour_draft ?
                        _L("裁切草稿已保留，但无法安全烘焙为 GLB：") : _L("语义 GLB 写出失败，当前模型保持不变：")) +
                        from_u8(preview_error.empty() ? result.error : preview_error));
                    self->finish_portrait_optimization(keep_contour_draft ? PortraitOutcome::DraftOnly : PortraitOutcome::Failed,
                        self->m_finishing_status->GetLabel());
                    if (keep_contour_draft) self->preserve_portrait_draft();
                } else {
                    size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error;
                    if (!self->m_model_preview->load_prepared_model(std::move(*prepared), {}, triangles,
                            dimensions, colors, error)) {
                        self->m_save_and_return = false;
                        boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                        self->restore_beauty_candidate(*draft_snapshot);
                        self->m_portrait_draft_before=before;
                        self->preserve_portrait_draft();
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, error);
                        self->m_finishing_status->SetLabel(_L("语义 GLB 预览加载失败，当前模型保持不变：") + from_u8(error));
                        self->finish_portrait_optimization(PortraitOutcome::DraftOnly,self->m_finishing_status->GetLabel());
                    } else {
                        self->m_model_preview->restore_semantic_region_evidence(region_evidence);
                        self->m_model_preview->restore_portrait_shapes(shape_details, shapes_unlocked);
                        const bool edits_restored=self->m_model_preview->restore_leaf_edits(leaf_edits);
                        if (self->m_beauty_controls) self->m_beauty_controls->restore_partition_snapshot(partition_snapshot);
                        self->m_finishing_candidate_region_evidence = self->m_model_preview->semantic_region_evidence();
                        self->m_finishing_candidate_region_error = self->m_model_preview->semantic_region_evidence_error();
                        self->m_model_preview->transfer_secondary_region_evidence(
                            secondary_evidence, before->model_sha256);
                        self->m_finishing_candidate_secondary_evidence = self->m_model_preview->secondary_region_evidence();
                        self->m_finishing_candidate_secondary_error = self->m_model_preview->secondary_region_evidence_error();
                        self->m_model_preview->restore_color_trial_without_recognition(color_state);
                        if (!edits_restored || !self->m_model_preview->set_saved_semantic_result(
                                self->m_finishing_candidate_semantic_faces,
                                self->m_finishing_candidate_semantic_subfaces)) {
                            self->m_save_and_return = false;
                            boost::system::error_code ignored;
                            boost::filesystem::remove(destination, ignored);
                            self->restore_beauty_candidate(*draft_snapshot);
                            self->m_portrait_draft_before=before;
                            self->preserve_portrait_draft();
                            if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false,
                                "semantic result preview unavailable");
                            self->m_finishing_status->SetLabel(_L("语义候选预览失败，已保留处理前版本。"));
                            self->finish_portrait_optimization(PortraitOutcome::DraftOnly,self->m_finishing_status->GetLabel());
                            self->refresh_controls();
                            return;
                        }
                        self->m_model_preview->synchronize_project_bound_semantics(
                            color_state, self->m_model_preview->color_trial_state());
                        self->m_finishing_candidate = destination;
                        self->m_finishing_candidate_baked_appearance = baked;
                        self->m_finishing_before = false;
                        self->m_model_preview->set_selection_preview_suppressed(true);
                        self->m_finishing_compare->SetLabel(_L("查看处理前"));
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(true, true);
                        // Cell masks above already refer to the rebuilt mesh.
                        // Only a root-only draft uses the separate face mask.
                        if ((leaf_edits.is_null() || leaf_edits.empty()) &&
                            draft_snapshot->geometry_id == self->m_model_preview->geometry_id() &&
                            draft_snapshot->selection.selected.size() == self->m_model_preview->triangle_count())
                            self->m_model_preview->restore_selection_state(draft_snapshot->selection);
                        self->record_beauty_candidate(
                            BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization, before);
                        if (self->m_beauty_controls && self->m_model_preview->secondary_regions_ready())
                            self->m_beauty_controls->request_secondary_partition(destination);
                        self->m_finishing_status->SetLabel(self->m_model_preview->active_shape_locks()
                            ? _L("GLB 候选已就绪，形状锁定细节已保留；可对比、继续编辑或接受。")
                            : self->m_model_preview->secondary_regions_ready()
                                ? _L("人像区域和二级细节已就绪；正在自动划区，可对比、继续编辑或接受。")
                                : _L("人像区域已写入新的 GLB 候选版本；二级细节不可用，一级分区仍可编辑。"));
                        self->m_model_preview_message->SetLabel(_L("语义优化后 · 尚未接受"));
                        if (portrait_task) {
                            self->m_portrait_preview_draft=draft_snapshot;
                            self->m_portrait_draft_before=before;
                        } else self->clear_portrait_draft();
                        self->portrait_preview_ready();
                    }
                }
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->refresh_controls();
                self->update_finishing_selection();
            });
        });
    } catch (const std::exception& e) {
        m_finishing_running = false;
        m_busy = false;
        restore_beauty_candidate(*draft_snapshot);
        m_portrait_draft_before=before;
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, e.what());
        m_finishing_status->SetLabel(_L("无法启动语义 GLB 写出，当前模型保持不变：") + from_u8(e.what()));
        finish_portrait_optimization(PortraitOutcome::DraftOnly, m_finishing_status->GetLabel());
        preserve_portrait_draft();
        refresh_controls();
    }
}
} // namespace Slic3r::GUI
