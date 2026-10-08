#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "WorkbenchModelInspection.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::ensure_workbench_check()
{
    if (m_shutdown || m_workbench_import_running || !m_model_preview_ready || m_workbench_check_running || m_finishing_running ||
        m_busy || m_model_preview->semantic_processing() || m_model_preview->selection_busy() ||
        !m_finishing_candidate.empty() || (m_beauty_controls && m_beauty_controls->has_changes())) return;
    if (m_workbench_check_path == m_displayed_model_path.string() && m_workbench_check_revision == m_sequence &&
        m_workbench_check_result.status != WorkbenchCheckStatus::NotRun) return;
    request_check_workbench();
}

bool ModelGenerationPanel::request_check_workbench()
{
    if (m_shutdown || !m_model_preview_ready || m_workbench_check_running || m_busy || m_finishing_running ||
        !post_generation_ui_state().can_edit || !m_finishing_candidate.empty() ||
        (m_beauty_controls && m_beauty_controls->has_changes())) return false;
    if (m_beauty_transactions && m_beauty_transactions->processing()) return false;
    if (m_workbench_check_worker.joinable()) m_workbench_check_worker.join();
    const auto source = m_displayed_model_path;
    const auto revision = m_sequence;
    const auto asset = m_displayed_model_job_id;
    const auto id = "finish-" + ModelGenerationPresentation::new_request_id();
    const auto destination = source.parent_path() / (id + ".obj");
    const auto partition = m_beauty_controls ? m_beauty_controls->capture_partition() : nullptr;
    const bool bound_edits = !m_model_preview->face_color_overrides().empty() || m_model_preview->leaf_editing() ||
        m_model_preview->semantic_result_active() || bool(m_model_preview->semantic_region_evidence()) ||
        bool(m_model_preview->secondary_region_evidence()) || bool(m_model_preview->portrait_shape_details()) ||
        m_model_preview->selected_face_count() != 0 || m_model_preview->protected_face_count() != 0 ||
        (partition && bool(partition->puzzle));
    const bool repair_allowed = workbench_safe_repair_allowed(AI::model_artifact_format(source), bound_edits, false, false);
    const auto canceled = m_workbench_check_cancel = std::make_shared<std::atomic<bool>>(false);
    const auto attempts = m_workbench_repair_attempts;
    const auto previous_result = m_workbench_check_result;
    m_workbench_check_path = source.string();
    m_workbench_check_revision = revision;
    m_workbench_check_result = {};
    m_workbench_check_result.status = WorkbenchCheckStatus::Running;
    m_workbench_check_result.phase = WorkbenchCheckPhase::Inspecting;
    m_workbench_check_result.summary = "正在检查模型……";
    m_workbench_check_running = true;
    update_finishing_selection();
    refresh_model_finishing();
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {
        m_workbench_check_worker = std::thread([weak, source, destination, revision, asset, id, canceled,
            attempts, previous_result, repair_allowed, bound_edits] {
            auto result = inspect_workbench_model(source, [canceled] { return canceled->load(); });
            const auto source_hash = result.model_sha256;
            const std::string key = asset + "|" + source_hash;
            AI::ModelFinishingResult repair;
            AI::ModelFinishingOptions options;
            options.smooth_surface = false;
            options.repair_mesh = true;
            const bool attempted = result.status != WorkbenchCheckStatus::Failed && !source_hash.empty() &&
                repair_allowed && !attempts.count(key) && !canceled->load();
            auto progress = [weak, source, revision, canceled](WorkbenchCheckPhase phase, const std::string& text) {
                wxGetApp().CallAfter([weak, source, revision, canceled, phase, text] {
                    if (!weak || weak->m_shutdown || canceled->load() || revision != weak->m_sequence ||
                        source != weak->m_displayed_model_path || weak->m_workbench_check_cancel != canceled) return;
                    weak->m_workbench_check_result.phase = phase;
                    weak->m_workbench_check_result.summary = text;
                    weak->refresh_post_generation_workbench();
                });
            };
            if (attempted) {
                progress(WorkbenchCheckPhase::Repairing, "正在执行安全修复；原始模型保留……");
                repair = AI::finish_model_artifact(source, destination, options, [canceled] { return canceled->load(); });
                if (repair.success && repair.changed() && !canceled->load()) {
                    progress(WorkbenchCheckPhase::Rechecking, "正在复检安全修复后的模型……");
                    result = inspect_workbench_model(destination, [canceled] { return canceled->load(); });
                    result.removed_faces = repair.removed_degenerate_faces + repair.removed_duplicate_faces;
                    result.reversed_faces = repair.reversed_faces;
                    result.repair_reason = "已清理 " + std::to_string(result.removed_faces) + " 个面，校正 " +
                        std::to_string(result.reversed_faces) + " 个面。";
                } else if (!repair.success && !canceled->load()) {
                    result.repair_reason = "安全修复失败，原模型保留：" + repair.error;
                    if (result.status == WorkbenchCheckStatus::Normal) result.status = WorkbenchCheckStatus::Attention;
                }
            } else if (result.status != WorkbenchCheckStatus::Failed) {
                result.repair_reason = AI::model_artifact_format(source) == "glb"
                    ? result.status == WorkbenchCheckStatus::Normal ? "" : "GLB 暂不支持保留贴图的网格修复；当前拓扑问题仍保留。"
                    : bound_edits ? "当前模型含选区、语义、保护或局部配色标注；无法安全重映射面身份，未自动修改。"
                    : "此版本已自动尝试安全修复；撤销后不会重新应用。";
                if (source_hash == previous_result.model_sha256 && !previous_result.repair_reason.empty()) {
                    result.repair_reason = previous_result.repair_reason;
                    result.removed_faces = previous_result.removed_faces;
                    result.reversed_faces = previous_result.reversed_faces;
                    if (result.status == WorkbenchCheckStatus::Normal && previous_result.status == WorkbenchCheckStatus::NotRun)
                        result.status = WorkbenchCheckStatus::Attention;
                }
            }
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string error;
            if (repair.success && repair.changed() && result.status != WorkbenchCheckStatus::Failed && !canceled->load()) {
                try { ModelPreview3D::prepare_model(destination, *prepared, error, {}, {}, true,
                    [canceled] { return canceled->load(); }); }
                catch (const std::exception& e) { error = e.what(); }
            }
            if (canceled->load()) { boost::system::error_code ec; boost::filesystem::remove(destination, ec); }
            wxGetApp().CallAfter([weak, source, destination, revision, asset, id, canceled, attempted, key, source_hash,
                result = std::move(result), repair, options, prepared, error] () mutable {
                auto cleanup = [&destination] { boost::system::error_code ec; boost::filesystem::remove(destination, ec); };
                if (!weak || weak->m_shutdown || revision != weak->m_sequence || source != weak->m_displayed_model_path ||
                    weak->m_workbench_check_cancel != canceled) { cleanup(); return; }
                auto* self = weak.get();
                if (self->m_workbench_check_worker.joinable()) self->m_workbench_check_worker.join();
                self->m_workbench_check_running = false;
                result.phase = WorkbenchCheckPhase::Idle;
                if (canceled->load()) {
                    cleanup();
                    result.status = WorkbenchCheckStatus::Failed;
                    result.summary = "已取消检查与修复，当前模型保持不变。";
                    result.repair_reason.clear();
                } else if (attempted) self->m_workbench_repair_attempts.insert(key);
                self->m_workbench_check_result = result;
                if (!canceled->load() && !source_hash.empty() && AI::model_artifact_sha256(source) != source_hash) {
                    cleanup();
                    self->m_workbench_check_result = {};
                    self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                    self->m_workbench_check_result.summary = "模型文件在检查期间已变化，已丢弃结果，请重新加载。";
                    self->refresh_controls();
                    return;
                }
                if (!canceled->load() && repair.success && repair.changed() && error.empty() &&
                    result.status != WorkbenchCheckStatus::Failed) {
                    auto before = std::make_shared<BeautyCandidateSnapshot>(self->capture_beauty_candidate());
                    self->m_beauty_session_source = before;
                    self->m_beauty_session_undo_base = self->m_beauty_transactions ? self->m_beauty_transactions->undo_count() : 0;
                    self->m_beauty_session_file_base = self->m_beauty_candidate_files.size();
                    self->m_finishing_source = source;
                    self->m_finishing_source_context = self->capture_model_context();
                    self->m_finishing_candidate = destination;
                    self->m_finishing_id = id;
                    self->m_finishing_options = options;
                    self->m_finishing_result = repair;
                    size_t triangles = 0, colors = 0;
                    Vec3d dimensions;
                    std::string load_error;
                    const auto view = self->m_model_preview->view_state();
                    if (!self->m_model_preview->load_prepared_model(std::move(*prepared), {}, triangles, dimensions, colors, load_error)) {
                        self->m_finishing_candidate.clear();
                        self->m_beauty_session_source.reset();
                        cleanup();
                        self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                        self->m_workbench_check_result.summary = "安全修复版本加载失败，原模型保留：" + load_error;
                    } else {
                        self->m_model_preview->restore_view(view);
                        self->m_model_preview->restore_color_trial_without_recognition(before->color_trial);
                        self->m_beauty_candidate_files.push_back(destination);
                        if (self->m_beauty_transactions) self->m_beauty_transactions->mark_preview_ready();
                        self->m_workbench_repair_attempts.insert(id + "|" + repair.output_sha256);
                        self->m_workbench_auto_repair = true;
                        self->accept_model_finishing();
                        if (!self->m_finishing_running && !self->m_finishing_candidate.empty()) {
                            const auto reason = self->m_finishing_status->GetLabel();
                            self->m_workbench_auto_repair = false;
                            self->discard_model_finishing();
                            self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                            self->m_workbench_check_result.summary = reason.ToUTF8().data();
                        }
                    }
                } else {
                    cleanup();
                    if (!error.empty()) {
                        self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                        self->m_workbench_check_result.summary = "安全修复预览失败，原模型保留：" + error;
                    }
                }
                self->refresh_controls();
                self->update_finishing_selection();
                self->publish_workbench_state();
            });
        });
    } catch (const std::exception& error) {
        m_workbench_check_running = false;
        m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
        m_workbench_check_result.phase = WorkbenchCheckPhase::Idle;
        m_workbench_check_result.summary = error.what();
        refresh_controls();
        return false;
    }
    return true;
}

} // namespace Slic3r::GUI
