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

void ModelGenerationPanel::clear_pending_workbench_ai_palette()
{
    m_workbench_auto_palette_pending = false;
    m_workbench_auto_palette_revision = 0;
    m_workbench_auto_palette_path.clear();
}

bool ModelGenerationPanel::apply_pending_workbench_ai_palette(const boost::filesystem::path& source,
                                                               std::uint64_t revision)
{
    if (!m_workbench_auto_palette_pending || revision != m_workbench_auto_palette_revision ||
        source != m_workbench_auto_palette_path || revision != m_sequence || source != m_displayed_model_path ||
        !m_model_preview_ready || m_model_preview->restored_persisted_color_trial()) {
        clear_pending_workbench_ai_palette();
        return false;
    }
    clear_pending_workbench_ai_palette();
    if (m_model_preview->activate_current_ai_palette()) return true;
    m_workbench_operation_notice = "模型检查已完成，但当前模型无法应用 AI配色。";
    return false;
}

void ModelGenerationPanel::ensure_workbench_check()
{
    if (m_shutdown || m_workbench_import_running ||
        !workbench_automatic_check_should_start(m_finishing_workbench, m_model_preview_ready) ||
        m_workbench_check_running || m_finishing_running ||
        m_busy || m_model_preview->semantic_processing() || m_model_preview->selection_busy() ||
        !m_finishing_candidate.empty() || (m_beauty_controls && m_beauty_controls->has_changes())) return;
    if (m_workbench_check_path == m_displayed_model_path.string() && m_workbench_check_revision == m_sequence &&
        m_workbench_check_result.status != WorkbenchCheckStatus::NotRun) return;
    request_check_workbench(true);
}

bool ModelGenerationPanel::request_check_workbench(bool automatic)
{
    if (m_shutdown || (automatic && !m_finishing_workbench) || !m_model_preview_ready ||
        m_workbench_check_running || m_busy || m_finishing_running ||
        !post_generation_ui_state().can_edit || !m_finishing_candidate.empty() ||
        (m_beauty_controls && m_beauty_controls->has_changes())) return false;
    if (m_beauty_transactions && m_beauty_transactions->processing()) return false;
    if (m_workbench_check_worker.joinable()) m_workbench_check_worker.join();
    const auto source = m_displayed_model_path;
    const auto revision = m_sequence;
    const auto asset = m_displayed_model_job_id;
    const auto id = "finish-" + ModelGenerationPresentation::new_request_id();
    // Candidates are deliberately written beside the source and never replace
    // it.  GLB repairs are exported as a colour-carrying OBJ because topology
    // changes cannot be represented by the texture-preserving GLB editor.
    const auto destination = source.parent_path() / ("orcaslicer-ai-" + id + ".obj");
    auto appearance_carrier = destination;
    appearance_carrier.replace_extension(".source-appearance.obj");
    const auto appearance_root = ModelGenerationPresentation::generated_models_root();
    const auto source_appearance = m_model_preview->original_appearance_reference();
    const auto source_appearance_error = m_model_preview->original_appearance_error();
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
    m_workbench_auto_repair = false;
    if (automatic && !m_model_preview->restored_persisted_color_trial()) {
        m_workbench_auto_palette_pending = true;
        m_workbench_auto_palette_revision = revision;
        m_workbench_auto_palette_path = source;
    } else {
        clear_pending_workbench_ai_palette();
    }
    m_workbench_check_path = source.string();
    m_workbench_check_revision = revision;
    m_workbench_check_source = automatic ? WorkbenchCheckSource::Automatic : WorkbenchCheckSource::Manual;
    m_workbench_force_import_ack_hash.clear();
    m_workbench_force_import_ack_revision = 0;
    m_workbench_check_result = {};
    m_workbench_check_result.source = m_workbench_check_source;
    m_workbench_check_result.status = WorkbenchCheckStatus::Running;
    m_workbench_check_result.phase = WorkbenchCheckPhase::Inspecting;
    m_workbench_check_result.summary = "正在检查模型……";
    m_workbench_check_running = true;
    m_workbench_check_card_visible = automatic;
    update_finishing_selection();
    refresh_model_finishing();
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {
        m_workbench_check_worker = std::thread([weak, source, destination, revision, asset, id, canceled,
            attempts, previous_result, repair_allowed, bound_edits, automatic, appearance_carrier,
            appearance_root, source_appearance, source_appearance_error,
            check_source = m_workbench_check_source] {
            auto result = inspect_workbench_model(source, [canceled] { return canceled->load(); });
            result.source = check_source;
            const auto source_check = result;
            const auto source_hash = result.model_sha256;
            const std::string key = asset + "|" + source_hash;
            AI::ModelFinishingResult repair;
            AI::ModelFinishingOptions options;
            options.smooth_surface = false;
            options.repair_mesh = true;
            const bool attempted = result.status != WorkbenchCheckStatus::Failed && !source_hash.empty() &&
                !workbench_topology_safe(result) && repair_allowed && !attempts.count(key) && !canceled->load();
            auto progress = [weak, source, revision, canceled](WorkbenchCheckPhase phase, const std::string& text) {
                wxGetApp().CallAfter([weak, source, revision, canceled, phase, text] {
                    if (!weak || weak->m_shutdown || canceled->load() || revision != weak->m_sequence ||
                        source != weak->m_displayed_model_path || weak->m_workbench_check_cancel != canceled) return;
                    weak->m_workbench_check_result.phase = phase;
                    weak->m_workbench_check_result.summary = text;
                    if (phase == WorkbenchCheckPhase::Repairing || phase == WorkbenchCheckPhase::Rechecking)
                        weak->m_workbench_check_card_visible = true;
                    weak->refresh_post_generation_workbench();
                });
            };
            result.source_sha256 = source_hash;
            result.candidate_status = attempted ? RepairCandidateStatus::Generating : RepairCandidateStatus::None;
            if (attempted) {
                progress(WorkbenchCheckPhase::Repairing, "正在执行安全修复；原始模型保留……");
                repair = AI::finish_model_artifact(source, destination, options, [canceled] { return canceled->load(); });
                if (repair.success && repair.changed() && !canceled->load()) {
                    progress(WorkbenchCheckPhase::Rechecking, "正在复检安全修复后的模型……");
                    const auto before_faces = result.before_faces, before_vertices = result.before_vertices;
                    result = inspect_workbench_model(destination, [canceled] { return canceled->load(); });
                    result.source = check_source;
                    const auto after_faces = result.before_faces, after_vertices = result.before_vertices;
                    const auto candidate_hash = result.model_sha256;
                    result.source_sha256 = source_hash;
                    // The import gate always hashes the currently displayed
                    // original while a candidate is pending or rejected.
                    result.model_sha256 = source_hash;
                    result.before_faces = before_faces;
                    result.before_vertices = before_vertices;
                    result.after_faces = after_faces;
                    result.after_vertices = after_vertices;
                    result.candidate_status = RepairCandidateStatus::Ready;
                    result.candidate_path = destination.string();
                    result.candidate_sha256 = candidate_hash;
                    result.appearance_mapped = !bound_edits;
                    result.appearance_confidence = bound_edits ? 0.0 : 1.0;
                    result.appearance_reason = bound_edits
                        ? "候选几何改变了面身份；局部涂色、语义和保护编辑需要重新确认。"
                        : "顶点颜色与模型几何保持可验证映射。";
                    const bool candidate_ok = workbench_topology_safe(result);
                    if (!candidate_ok) {
                        const auto candidate_summary = result.summary;
                        // Rejected candidate statistics must not certify the
                        // original whose digest the import gate is checking.
                        result = source_check;
                        result.source_sha256 = source_hash;
                        result.after_faces = after_faces;
                        result.after_vertices = after_vertices;
                        result.candidate_status = RepairCandidateStatus::Failed;
                        result.candidate_path.clear();
                        result.candidate_sha256.clear();
                        result.repair_reason = "修复候选复检未通过，原模型保留。候选检查：" + candidate_summary;
                    }
                    result.removed_faces = repair.removed_degenerate_faces + repair.removed_duplicate_faces;
                    result.reversed_faces = repair.reversed_faces;
                    const std::string changes = "已清理 " + std::to_string(result.removed_faces) + " 个面，校正 " +
                        std::to_string(result.reversed_faces) + " 个面。";
                    if (candidate_ok) result.repair_reason = changes;
                    else result.repair_reason += " " + changes;
                    result.repair_reason += " 修复前 " + std::to_string(before_faces) + " 面/" +
                        std::to_string(before_vertices) + " 点，修复后 " + std::to_string(after_faces) +
                        " 面/" + std::to_string(after_vertices) + " 点。";
                } else if (!repair.success && !canceled->load()) {
                    result.candidate_status = RepairCandidateStatus::Failed;
                    result.candidate_path.clear();
                    result.repair_reason = "安全修复失败，原模型保留：" + repair.error;
                    if (result.status == WorkbenchCheckStatus::Normal) result.status = WorkbenchCheckStatus::Attention;
                } else if (!canceled->load()) {
                    result.candidate_status = RepairCandidateStatus::Failed;
                    result.candidate_path.clear();
                    result.repair_reason = "修复未产生可验证的拓扑变化，原模型保留。";
                }
            } else if (result.status != WorkbenchCheckStatus::Failed) {
                result.repair_reason = AI::model_artifact_format(source) == "glb"
                    ? result.status == WorkbenchCheckStatus::Normal ? "" : "GLB 修复候选将以保留顶点颜色的 OBJ 预览生成，原始 GLB 保留。"
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
            OriginalAppearanceReference candidate_appearance = source_appearance;
            if (repair.success && repair.changed() && result.candidate_status == RepairCandidateStatus::Ready && !canceled->load()) {
                if (source_appearance.valid() && !prepare_original_appearance_for_candidate(
                        source, destination, repair.source_sha256, source_appearance, appearance_carrier,
                        appearance_root, candidate_appearance, error,
                        [canceled] { return canceled->load(); })) {
                    result.appearance_mapped = false;
                    result.appearance_confidence = 0.0;
                    result.appearance_reason = error;
                } else try {
                    const std::optional<OriginalAppearanceReference> appearance_override =
                        candidate_appearance.valid() || !source_appearance_error.empty()
                            ? std::optional<OriginalAppearanceReference>(candidate_appearance) : std::nullopt;
                    ModelPreview3D::prepare_model(destination, *prepared, error, {}, {}, true,
                        [canceled] { return canceled->load(); }, nullptr, appearance_override);
                    if (!source_appearance_error.empty() && !prepared->has_source_appearance)
                        prepared->original_appearance_error = source_appearance_error;
                    if (source_appearance.valid()) {
                        result.appearance_mapped = prepared->has_source_appearance && !bound_edits;
                        result.appearance_confidence = result.appearance_mapped ? 1.0 : 0.0;
                        if (!result.appearance_mapped && result.appearance_reason.empty())
                            result.appearance_reason = prepared->original_appearance_error;
                    }
                    if (automatic && !prepared->has_source_appearance) {
                        result.appearance_mapped = false;
                        result.appearance_confidence = 0.0;
                        result.appearance_reason = prepared->original_appearance_error.empty()
                            ? "原始配色证据不可用，自动流程未保存修复候选。"
                            : prepared->original_appearance_error;
                    }
                }
                catch (const std::exception& e) { error = e.what(); }
            }
            if (canceled->load()) {
                boost::system::error_code ec;
                boost::filesystem::remove(destination, ec);
                boost::filesystem::remove(appearance_carrier, ec);
            }
            wxGetApp().CallAfter([weak, source, destination, revision, asset, id, canceled, attempted, key, source_hash,
                automatic, check_source, appearance_carrier,
                result = std::move(result), repair, options, prepared, error] () mutable {
                auto cleanup = [&destination, &appearance_carrier] {
                    boost::system::error_code ec;
                    boost::filesystem::remove(destination, ec);
                    boost::filesystem::remove(appearance_carrier, ec);
                };
                if (!weak || weak->m_shutdown || revision != weak->m_sequence || source != weak->m_displayed_model_path ||
                    weak->m_workbench_check_cancel != canceled) { cleanup(); return; }
                auto* self = weak.get();
                if (self->m_workbench_check_worker.joinable()) self->m_workbench_check_worker.join();
                self->m_workbench_check_running = false;
                result.phase = WorkbenchCheckPhase::Idle;
                result.source = check_source;
                if (canceled->load()) {
                    cleanup();
                    result.status = WorkbenchCheckStatus::Failed;
                    result.candidate_status = RepairCandidateStatus::Failed;
                    result.candidate_path.clear();
                    result.summary = "已取消检查与修复，当前模型保持不变。";
                    result.repair_reason.clear();
                    result.force_import_risk = false;
                } else if (attempted) self->m_workbench_repair_attempts.insert(key);
                result.force_import_risk = result.status == WorkbenchCheckStatus::Attention &&
                    result.boundary_edges > 0 && result.nonmanifold_edges == 0 &&
                    result.nonmanifold_vertices == 0 && result.degenerate_faces == 0 &&
                    result.duplicate_faces == 0 && result.inconsistent_edges == 0 &&
                    result.self_intersections == 0;
                self->m_workbench_check_result = result;
                if (!canceled->load() && !source_hash.empty() && AI::model_artifact_sha256(source) != source_hash) {
                    cleanup();
                    self->clear_pending_workbench_ai_palette();
                    self->m_workbench_check_result = {};
                    self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                    self->m_workbench_check_result.source = check_source;
                    self->m_workbench_check_result.summary = "模型文件在检查期间已变化，已丢弃结果，请重新加载。";
                    self->m_workbench_check_result.force_import_risk = false;
                    self->m_status->SetLabel(wxString::FromUTF8(self->m_workbench_check_result.summary));
                    self->refresh_controls();
                    return;
                }
                if (!canceled->load() && automatic && repair.success && repair.changed() && error.empty() &&
                    result.candidate_status == RepairCandidateStatus::Ready &&
                    (!workbench_topology_safe(result) || !result.appearance_mapped || result.appearance_confidence < 0.99)) {
                    // Automatic checks may only publish a fully verified,
                    // appearance-preserving repair. Keep the source visible
                    // and discard anything that would need human review.
                    cleanup();
                    result.status = WorkbenchCheckStatus::Failed;
                    result.candidate_status = RepairCandidateStatus::Failed;
                    result.candidate_path.clear();
                    result.candidate_sha256.clear();
                    result.summary = "自动修复无法安全接受，原模型保留；请手动检查后决定。";
                    result.repair_reason = result.appearance_reason.empty()
                        ? "修复候选需要人工确认，自动流程未加载候选。" : result.appearance_reason;
                    result.force_import_risk = false;
                    self->m_workbench_check_result = result;
                } else if (!canceled->load() && repair.success && repair.changed() && error.empty() &&
                    result.candidate_status == RepairCandidateStatus::Ready &&
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
                    self->invalidate_printer_filament_trial_for_model_change();
                    if (!self->m_model_preview->load_prepared_model(std::move(*prepared), {}, triangles, dimensions, colors, load_error)) {
                        self->m_finishing_candidate.clear();
                        self->m_finishing_candidate_original_appearance = {};
                        self->m_finishing_candidate_original_appearance_error.clear();
                        self->m_beauty_session_source.reset();
                        cleanup();
                        self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                        self->m_workbench_check_result.summary = "安全修复版本加载失败，原模型保留：" + load_error;
                    } else {
                        self->m_model_preview->restore_view(view);
                        self->m_model_preview->restore_color_trial_without_recognition(before->color_trial);
                        self->m_finishing_candidate_original_appearance =
                            self->m_model_preview->original_appearance_reference();
                        self->m_finishing_candidate_original_appearance_error =
                            self->m_model_preview->original_appearance_error();
                        self->m_beauty_candidate_files.push_back(destination);
                        if (boost::filesystem::is_regular_file(appearance_carrier))
                            self->m_beauty_candidate_files.push_back(appearance_carrier);
                        if (self->m_beauty_transactions) self->m_beauty_transactions->mark_preview_ready();
                        self->m_workbench_repair_attempts.insert(id + "|" + repair.output_sha256);
                        // A fully rechecked topology candidate with no bound
                        // appearance edits is safe to publish immediately.
                        // Reuse the normal AcceptCandidate transaction so the
                        // original file, history, undo and rollback paths stay
                        // identical to an explicit user acceptance.
                        self->m_workbench_auto_repair = automatic &&
                            workbench_topology_safe(self->m_workbench_check_result) &&
                            self->m_workbench_check_result.appearance_mapped &&
                            self->m_workbench_check_result.appearance_confidence >= 0.99 &&
                            (!self->m_workbench_auto_palette_pending ||
                             self->m_model_preview->original_comparison_available());
                        self->m_workbench_check_result.candidate_status = RepairCandidateStatus::Ready;
                        self->m_workbench_check_result.candidate_path = destination.string();
                        self->m_workbench_check_result.candidate_sha256 = repair.output_sha256;
                        if (self->m_workbench_auto_repair) {
                            if (workbench_ai_palette_should_apply(self->m_workbench_auto_palette_pending,
                                    self->m_workbench_check_result.source,
                                    workbench_topology_safe(self->m_workbench_check_result),
                                    self->m_workbench_check_result.candidate_status, true))
                                self->apply_pending_workbench_ai_palette(source, revision);
                            else
                                self->clear_pending_workbench_ai_palette();
                            self->m_workbench_check_result.summary += "\n修复复检通过，正在自动接受并保存。";
                            self->m_status->SetLabel(wxString::FromUTF8(self->m_workbench_check_result.summary));
                            self->refresh_controls();
                            self->accept_model_finishing();
                        } else {
                            // Manual repair candidates are reviewed in the
                            // Beauty inspector. Its fixed action dock owns
                            // accept/discard; the canvas no longer has a
                            // global candidate bar.
                            self->m_workbench_editing = true;
                            self->m_workbench_check_result.summary += "\n修复候选已生成，请预览后接受或保留原模型。";
                        }
                    }
                } else {
                    cleanup();
                    if (!error.empty()) {
                        self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                        self->m_workbench_check_result.summary = "安全修复预览失败，原模型保留：" + error;
                    }
                }
                if (automatic && !self->m_workbench_auto_repair) {
                    if (workbench_ai_palette_should_apply(self->m_workbench_auto_palette_pending,
                            self->m_workbench_check_result.source,
                            workbench_topology_safe(self->m_workbench_check_result),
                            self->m_workbench_check_result.candidate_status, false))
                        self->apply_pending_workbench_ai_palette(source, revision);
                    else
                        self->clear_pending_workbench_ai_palette();
                }
                self->m_status->SetLabel(wxString::FromUTF8(self->m_workbench_check_result.summary));
                self->refresh_controls();
                self->update_finishing_selection();
                self->publish_workbench_state();
            });
        });
    } catch (const std::exception& error) {
        clear_pending_workbench_ai_palette();
        m_workbench_check_running = false;
        m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
        m_workbench_check_result.source = automatic ? WorkbenchCheckSource::Automatic : WorkbenchCheckSource::Manual;
        m_workbench_check_result.force_import_risk = false;
        m_workbench_check_result.phase = WorkbenchCheckPhase::Idle;
        m_workbench_check_result.summary = error.what();
        m_status->SetLabel(wxString::FromUTF8(m_workbench_check_result.summary));
        refresh_controls();
        return false;
    }
    return true;
}

} // namespace Slic3r::GUI
