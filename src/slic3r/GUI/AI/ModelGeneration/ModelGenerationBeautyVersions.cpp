#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "PortraitColorLayerPublication.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchModelInspection.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
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
#include <optional>
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
boost::filesystem::path portrait_metadata_path(const std::string& path)
{
    return boost::filesystem::path(wxString::FromUTF8(path).ToStdWstring());
}
std::string portrait_path_utf8(const boost::filesystem::path& path)
{
    return into_u8(wxString(path.wstring()));
}
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

nlohmann::json encode_portrait_post_color_check(
    const WorkbenchCheckResult& check,
    const nlohmann::json& partition_reference)
{
    if (!workbench_topology_safe(check) ||
        !PortraitPrint::valid_native_partition_reference(partition_reference) ||
        check.model_sha256 != partition_reference.at("carrier_sha256"))
        throw std::runtime_error("着色后只读拓扑检查未通过。");
    return {{"schema", "orca.portrait-post-color-check/v1"}, {"status", "normal"},
        {"model_sha256", check.model_sha256},
        {"geometry_id", partition_reference.at("geometry_id")},
        {"partition_digest", partition_reference.at("partition_digest")},
        {"topology_hash", partition_reference.at("topology_hash")},
        {"faces", check.before_faces}, {"vertices", check.before_vertices},
        {"boundary_edges", check.boundary_edges}, {"boundary_loops", check.boundary_loops},
        {"boundary_loop_sizes", check.boundary_loop_sizes},
        {"nonmanifold_edges", check.nonmanifold_edges},
        {"nonmanifold_vertices", check.nonmanifold_vertices},
        {"degenerate_faces", check.degenerate_faces},
        {"duplicate_faces", check.duplicate_faces},
        {"inconsistent_edges", check.inconsistent_edges},
        {"isolated_components", check.isolated_components}, {"small_faces", check.small_faces},
        {"self_intersections_checked", check.self_intersections_checked},
        {"self_intersections", check.self_intersections}, {"issue_faces", check.issue_faces}};
}
}
bool ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path)
{
    const auto view = m_model_preview->view_state();
    const auto color_state = m_model_preview->color_trial_state();
    const auto block_state=m_model_preview->portrait_color_blocks();
    const auto printer_restore=m_model_preview->printer_restore_snapshot();
    size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero(); std::string error;
    const bool session_source = m_finishing_workbench && m_beauty_session_source &&
        path == m_beauty_session_source->source;
    const auto explicit_overrides = path == m_finishing_candidate ? m_finishing_candidate_face_overrides
        : session_source ? m_beauty_session_source->face_overrides : ModelPreview3D::FaceColorOverrides {};
    std::optional<OriginalAppearanceReference> appearance_override;
    if (path == m_finishing_candidate &&
        (m_finishing_candidate_original_appearance.valid() || !m_finishing_candidate_original_appearance_error.empty()))
        appearance_override = m_finishing_candidate_original_appearance;
    else if (session_source && (m_beauty_session_source->original_appearance.valid() ||
                               !m_beauty_session_source->original_appearance_error.empty()))
        appearance_override = m_beauty_session_source->original_appearance;
    invalidate_printer_filament_trial_for_model_change();
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error, explicit_overrides, {},
            appearance_override)) {
        m_finishing_status->SetLabel(_L("预览加载失败，原件仍保留：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    if (path == m_finishing_candidate)
        m_model_preview->restore_semantic_region_evidence(m_finishing_candidate_region_evidence, m_finishing_candidate_region_error);
    else if (session_source)
        m_model_preview->restore_semantic_region_evidence(m_beauty_session_source->region_evidence, m_beauty_session_source->region_evidence_error);
    if (path == m_finishing_candidate)
        m_model_preview->restore_secondary_region_evidence(m_finishing_candidate_secondary_evidence, m_finishing_candidate_secondary_error);
    else if (session_source)
        m_model_preview->restore_secondary_region_evidence(m_beauty_session_source->secondary_evidence, m_beauty_session_source->secondary_evidence_error);
    if (path == m_finishing_candidate &&
        (!m_finishing_candidate_semantic_faces.empty() || !m_finishing_candidate_semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(color_state);
        m_model_preview->set_saved_semantic_result(
            m_finishing_candidate_semantic_faces, m_finishing_candidate_semantic_subfaces);
        m_model_preview->synchronize_project_bound_semantics(color_state, m_model_preview->color_trial_state());
    } else if (session_source && (!m_beauty_session_source->semantic_faces.empty() ||
                                  !m_beauty_session_source->semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(m_beauty_session_source->color_trial);
        m_model_preview->set_saved_semantic_result(
            m_beauty_session_source->semantic_faces, m_beauty_session_source->semantic_subfaces);
        m_model_preview->synchronize_project_bound_semantics(
            m_beauty_session_source->color_trial, m_model_preview->color_trial_state());
    } else if (m_finishing_workbench || m_model_preview->semantic_result_active())
        m_model_preview->restore_color_trial_without_recognition(color_state);
    else m_model_preview->restore_color_trial(color_state);
    if(path==m_finishing_candidate) {
        m_model_preview->restore_portrait_color_blocks(block_state);
        m_model_preview->restore_printer_restore_snapshot(printer_restore);
    } else if(session_source) {
        m_model_preview->restore_portrait_color_blocks(m_beauty_session_source->color_blocks);
        m_model_preview->restore_printer_restore_snapshot(m_beauty_session_source->printer_restore);
    }
    m_model_preview->set_color_controls_visible(!m_finishing_workbench || m_finishing_tool->GetSelection() == 2);
    m_model_preview->set_selection_preview_suppressed(
        m_finishing_workbench && !m_finishing_candidate.empty() && path == m_finishing_candidate);
    m_model_preview_ready = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
        dimensions.x(), dimensions.y(), dimensions.z()));
    return true;
}

void ModelGenerationPanel::select_local_finishing_version(const boost::filesystem::path& path, const std::string& id)
{
    if (m_native_workbench_partition_running)
        cancel_native_workbench_partition();
    m_native_workbench_context = {};
    m_native_workbench_partition_reference = nullptr;
    m_native_workbench_partition_report = nullptr;
    m_native_workbench_partition_certificate = nullptr;
    m_native_workbench_partition_commit = nullptr;
    ++m_sequence;
    ++m_model_generation_session;
    m_poll_timer.Stop();
    m_job_id.clear(); m_job_palette.clear(); m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_artifact_path = m_displayed_model_path = path;
    m_displayed_model_job_id = id;
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_artifact_format = AI::model_artifact_format(path); m_artifact_color_encoding = "vertex_colors";
    m_ready = true; m_library_model_loaded = true; m_artifact_download_started = false;
    m_awaiting_confirmation = false; m_awaiting_palette_confirmation = false;
    m_last_imported_model_path.clear();
    clear_model_quality();
    m_visual_quality = {};
    m_model_refinement = {};
}

bool ModelGenerationPanel::portrait_color_save_snapshot_current() const
{
    return m_portrait_save_draft && m_portrait_print.from_workbench() &&
        m_portrait_print.stage == PortraitPrint::Stage::SaveColor &&
        m_portrait_save_flow_id == m_portrait_print.id &&
        m_portrait_save_sequence == m_sequence &&
        m_portrait_save_draft->model_sha256 == m_portrait_print.model_hash &&
        m_portrait_save_draft->geometry_id == m_portrait_print.geometry_id;
}

void ModelGenerationPanel::rollback_portrait_color_save(const std::string& diagnostic, bool restore_draft)
{
    BOOST_LOG_TRIVIAL(error) << "portrait save rollback: " << diagnostic;
    if (!portrait_color_save_snapshot_current()) {
        BOOST_LOG_TRIVIAL(error) << "ignored stale portrait save rollback";
        return;
    }
    // A user edit arriving during export/publication belongs to the current
    // draft. Never replace it with the older save snapshot or truncate its undo.
    restore_draft = restore_draft && m_model_preview &&
        m_model_preview->leaf_edit_revision() == m_portrait_save_edit_revision;
    const auto files = m_portrait_save_files;
    const bool was_editing = m_portrait_save_was_editing;
    m_finishing_running = m_busy = false;
    cancel_beauty_color_gate();
    if (m_beauty_transactions) {
        m_beauty_transactions->finish(true);
        if (restore_draft) m_beauty_transactions->truncate_to(m_portrait_save_undo_base);
    }
    if (restore_draft && !restore_beauty_candidate(*m_portrait_save_draft))
        BOOST_LOG_TRIVIAL(error) << "portrait save draft restoration failed";
    m_finishing_candidate.clear();
    m_portrait_color_candidate_identity = {};
    m_portrait_export_started = m_portrait_semantics_requested = false;
    m_save_and_return = false;
    m_workbench_operation_notice.clear();
    m_finishing_status->SetLabel(wxString());
    if (m_status) m_status->SetLabel(wxString());
    m_portrait_print.rollback_color_save();
    m_portrait_color_snapshot = m_model_preview->color_trial_metadata();
    m_portrait_color_edit_revision = m_model_preview->leaf_edit_revision();
    m_portrait_print.edit_revision = m_portrait_color_edit_revision;
    // Automatic portrait coloring precedes filament application and may never
    // have acquired a Beauty match. Keep the existing overview entry available;
    // entering Beauty again must run its existing real compatibility check.
    if (!beauty_color_match_current()) {
        m_workbench_editing = false;
        m_beauty_matched_colors.reset();
        m_beauty_color_gate_state = BeautyColorGateState::Unchecked;
        m_beauty_color_gate_message.clear();
        m_beauty_color_gate_configuration_signature.clear();
        m_beauty_color_gate_cache.reset();
        m_beauty_color_gate_cache_key.clear();
    } else m_workbench_editing = was_editing;
    if (m_portrait_save_task && m_portrait_task == m_portrait_save_task) {
        m_portrait_task->finish(PortraitOutcome::Cancelled);
        m_portrait_task.reset();
        if (m_portrait_preview_draft == m_portrait_save_draft) {
            m_portrait_preview_draft.reset();
            m_portrait_draft_before.reset();
        }
        m_portrait_entered = m_portrait_preview_drawn = m_portrait_cancel_requested = false;
        m_auto_portrait_active = m_portrait_reoptimization_pending = false;
    }
    m_portrait_save_task.reset();
    m_portrait_save_draft.reset();
    m_portrait_save_flow_id.clear();
    m_portrait_save_files.clear();
    for (const auto& file : files) {
        if (file == m_finishing_source || file == m_displayed_model_path ||
            (m_model_preview && m_model_preview->showing_model(file))) continue;
        boost::system::error_code ignored;
        boost::filesystem::remove(file, ignored);
        m_beauty_candidate_files.erase(std::remove(m_beauty_candidate_files.begin(),
            m_beauty_candidate_files.end(), file), m_beauty_candidate_files.end());
    }
    // A failed rollback write is diagnostic only; the native partition survives.
    save_portrait_print(true);
    refresh_controls();
    publish_ui_state();
}

void ModelGenerationPanel::accept_model_finishing()
{
    auto report = [this](const wxString& message) {
        if (m_portrait_save_draft && m_portrait_print.stage == PortraitPrint::Stage::SaveColor) {
            rollback_portrait_color_save(into_u8(message)); return;
        }
        m_workbench_operation_notice = into_u8(message);
        m_finishing_status->SetLabel(message);
        m_status->SetLabel(message);
        refresh_controls();
        publish_workbench_state();
        if ((portrait_owns_display_context() || portrait_owns_save_candidate()) &&
            m_portrait_print.running() &&
            (m_portrait_print.stage == PortraitPrint::Stage::Check ||
            m_portrait_print.stage == PortraitPrint::Stage::SaveColor)) pause_portrait_print(into_u8(message));
    };
    if (m_busy || m_finishing_running || m_workbench_check_running) {
        report(_L("正在处理模型，请等待完成后接受候选。")); return;
    }
    if (m_finishing_candidate.empty()) {
        report(_L("没有可接受的候选，请先检查或生成候选。")); return;
    }
    const bool portrait_save = portrait_owns_save_candidate() && m_portrait_print.from_workbench() &&
        m_portrait_print.current(m_portrait_print.id, PortraitPrint::Stage::SaveColor) &&
        m_finishing_id == m_portrait_print.color_asset_id();
    const bool portrait_repair = portrait_owns_display_context() &&
        m_portrait_print.record_version < 5 &&
        m_portrait_print.current(m_portrait_print.id, PortraitPrint::Stage::Check) &&
        m_workbench_auto_repair;
    if (!portrait_save && !portrait_repair && m_finishing_workbench && m_workbench_editing && !beauty_color_match_current()) {
        m_save_and_return = false;
        report(_L("当前模型配色尚未完整绑定到耗材；候选保留，请恢复兼容配置后再保留效果。"));
        return;
    }
    const bool topology_candidate = m_workbench_check_result.candidate_status == RepairCandidateStatus::Ready &&
        m_finishing_options.repair_mesh && !m_finishing_options.smooth_surface;
    if (topology_candidate && !workbench_topology_safe(m_workbench_check_result)) {
        report(_L("修复候选未通过拓扑复检，请保留原模型并重新检查。")); return;
    }
    if (topology_candidate && !m_workbench_check_result.appearance_mapped) {
        if (portrait_repair) { pause_portrait_print("修复后的原始外观映射无法核验，未接受候选。"); return; }
        const auto answer = wxMessageBox(
            _L("修复改变了面身份，当前局部涂色、语义区域或人像保护无法安全映射。\n接受修复将清除这些无法验证的编辑；选择“否”保留原模型。"),
            _L("确认接受修复候选"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            wxGetTopLevelParent(m_workbench_shell));
        if (answer != wxYES) { report(_L("尚未接受修复，原模型和候选保留。")); return; }
    }
    // A repair already has a loaded candidate and a captured colour trial.
    // Baking that trial first would replace the repair candidate and recursively
    // route Accept through Save. Identity-bound edits were confirmed above.
    if (!topology_candidate && m_finishing_workbench && m_beauty_manual_color_dirty) {
        if (!request_save_and_return()) report(_L("颜色草稿暂时无法保存，请检查耗材匹配状态。"));
        return;
    }
    if (m_beauty_transactions &&
        !m_beauty_transactions->begin(BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate)) {
        report(_L("当前仍有模型处理正在进行，请先完成或取消。")); return;
    }
    if (!m_model_preview->showing_model(m_finishing_candidate) && !show_finishing_version(m_finishing_candidate)) {
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "candidate load failed");
        m_save_and_return = false;
        report(_L("候选预览未加载，候选保留；请重新预览后接受。")); return;
    }
    m_workbench_operation_notice = "正在接受候选并保存，可取消……";
    const auto root = generated_models_root();
    const auto before = portrait_save ? m_portrait_save_draft : m_beauty_session_source;
    const auto source_context = m_finishing_source_context;
    auto after = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    const auto verified_check = m_workbench_check_result;
    const bool filament_application = m_apply_filaments_saving;
    if (filament_application && m_apply_filaments_staged) {
        after->project_config = std::make_shared<DynamicPrintConfig>(m_apply_filaments_staged->project_config);
        after->project_presets = m_apply_filaments_staged->filament_presets;
    }
    const auto source = m_finishing_source;
    const auto candidate = m_finishing_candidate;
    std::vector<boost::filesystem::path> topology_publication_artifacts;
    if (topology_candidate) {
        topology_publication_artifacts.push_back(candidate);
        auto expected_carrier = candidate;
        expected_carrier.replace_extension(".source-appearance.obj");
        boost::system::error_code equivalent_error;
        if (after->original_appearance.valid() && boost::filesystem::is_regular_file(expected_carrier) &&
            boost::filesystem::equivalent(expected_carrier,
                after->original_appearance.carrier_model, equivalent_error) && !equivalent_error)
            topology_publication_artifacts.push_back(expected_carrier);
    }
    const auto source_hash = before && before->source == source
        ? before->model_sha256 : m_finishing_result.source_sha256;
    const auto metadata_path = library_metadata_path(m_finishing_id);
    nlohmann::json metadata {
        {"schema_version", 4}, {"history_index_required", true}, {"job_id", m_finishing_id},
        {"model_path", m_finishing_candidate.lexically_relative(root).generic_string()},
        {"source", "local_finishing"}, {"prompt", "3D 美颜与修复"},
        {"source_model", m_finishing_source.lexically_relative(root).generic_string()},
        {"source_sha256", source_hash}, {"model_sha256", m_finishing_result.output_sha256},
        {"palette", nlohmann::json::array()}, {"palette_roles", nlohmann::json::object()},
        {"use_printable_colors", false}, {"generated_at", std::time(nullptr)},
        {"triangle_count", m_finishing_result.faces_after},
        {"dimensions", m_finishing_result.dimensions},
        {"finishing", {{"smooth_surface", m_finishing_options.smooth_surface}, {"repair_mesh", m_finishing_options.repair_mesh},
            {"recolor_selected", m_finishing_options.recolor_selected}, {"target_color", m_finishing_options.target_color},
            {"recolored_faces", m_finishing_result.recolored_faces},
            {"clean_color_spots", m_finishing_options.clean_color_spots}, {"cleanup_palette", m_finishing_options.cleanup_palette},
            {"cleaned_color_regions", m_finishing_result.cleaned_color_regions}, {"recolored_vertices", m_finishing_result.recolored_vertices},
            {"selected_faces", m_finishing_options.selected_faces},
            {"strength", m_finishing_options.strength}, {"moved_vertices", m_finishing_result.moved_vertices},
            {"protected_vertices", m_finishing_result.protected_vertices},
            {"removed_degenerate_faces", m_finishing_result.removed_degenerate_faces},
            {"removed_duplicate_faces", m_finishing_result.removed_duplicate_faces},
            {"reversed_faces", m_finishing_result.reversed_faces},
            {"boundary_edges", m_finishing_result.boundary_edges}, {"nonmanifold_edges", m_finishing_result.nonmanifold_edges},
            {"max_displacement_source_units", m_finishing_result.max_displacement},
            {"repair_geometry_verified", m_finishing_result.repair_geometry_verified},
            {"repair_sampled_deviation", m_finishing_result.repair_sampled_deviation},
            {"repair_area_change_ratio", m_finishing_result.repair_area_change_ratio},
            {"repair_strategy", m_finishing_result.repair_strategy}}}
    };
    if (m_finishing_options.recolor_selected) {
        metadata["recolor_target_palette"] = m_finishing_color_palette;
        metadata["preserves_unselected_face_colors"] = true;
    }
    const int ai_count = portrait_save || portrait_repair ? m_portrait_print.model_color_count :
        original_appearance_detail::ai_source_color_count(source, root);
    if (ai_count >= 4 && ai_count <= 6)
        metadata["ai_source_color_count"] = ai_count;
    if (portrait_repair) {
        metadata["portrait_repair_flow_id"] = m_portrait_print.id;
        metadata["portrait_repair_phase"] = "before_source_partition";
    }
    std::optional<WorkbenchNativeTopologyCertificate> portrait_certificate;
    if (portrait_save) {
        const auto exported_faces = m_model_preview->printer_filament_export_faces();
        const auto exported_subfaces = m_model_preview->import_subface_color_overrides(true);
        const auto native_partition = m_model_preview->native_portrait_partition();
        const auto color_blocks = m_model_preview->portrait_color_blocks();
        if (exported_faces.size() != m_finishing_result.faces_after || !exported_subfaces.empty() ||
            m_finishing_result.source_sha256 != m_portrait_print.model_hash ||
            m_finishing_result.output_sha256 != m_portrait_print.model_hash ||
            m_model_preview->geometry_id() != m_portrait_print.geometry_id ||
            !m_portrait_print.partition_committed ||
            !PortraitPrint::valid_native_partition_reference(
                m_portrait_print.native_portrait_partition, m_portrait_print.basis_signature,
                m_portrait_print.partition_topology_hash) ||
            !native_partition || !native_partition->valid() ||
            native_partition->partition_digest != m_portrait_print.basis_signature ||
            !color_blocks || color_blocks->algorithm_version !=
                PortraitColorBlocks::portrait_postprocess_algorithm_version ||
            !color_blocks->native_partition_identity ||
            color_blocks->native_partition_identity->partition_digest !=
                native_partition->partition_digest) {
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "portrait paint identity mismatch");
            rollback_portrait_color_save("portrait paint identity mismatch"); return;
        }
        metadata["portrait_print_id"] = m_portrait_print.id;
        metadata["prompt"] = m_portrait_print.semantic_applied ? "人物六色 · 区域语义配色" : "人物六色 · 近色回退";
        metadata["portrait_preparation"] = {{"schema", "orca.portrait-preparation/v2"},
            {"round_base", true}, {"height_mm", 3.0}, {"overlap_mm", 0.2},
            {"partition_digest", m_portrait_print.basis_signature},
            {"partition_topology_hash", m_portrait_print.partition_topology_hash},
            {"boundary_algorithm_version", m_portrait_print.boundary_algorithm_version}};
        try {
            auto partition_reference = m_portrait_print.native_portrait_partition;
            const auto commit_metadata_path =
                portrait_metadata_path(partition_reference.at("metadata_path").get<std::string>());
            boost::filesystem::ifstream commit_stream(commit_metadata_path, std::ios::binary);
            const auto commit = nlohmann::json::parse(commit_stream);
            const auto& certificate = commit.at("native_portrait_topology_certificate");
            const bool flow_requested_partition =
                commit.value("portrait_print_id", std::string{}).empty() &&
                commit.value("requesting_portrait_print_id", std::string{}) == m_portrait_print.id;
            if (commit.at("schema") != "orca.native-workbench-partition-commit/v1" ||
                commit.at("transaction_status") != "committed" ||
                (!flow_requested_partition &&
                    commit.value("portrait_print_id", std::string{}) != m_portrait_print.id) ||
                AI::beauty_leaf_digest(certificate.dump()) !=
                    partition_reference.at("topology_hash") ||
                certificate.at("carrier_sha256") != m_finishing_result.output_sha256 ||
                certificate.at("geometry_id") != m_model_preview->geometry_id() ||
                certificate.at("partition_digest") != m_portrait_print.basis_signature)
                throw std::runtime_error("原生分区拓扑证书与配色载体不匹配。");
            const auto& owner = m_portrait_color_candidate_identity;
            WorkbenchNativeTopologyCertificate proof;
            proof.partition_metadata = commit;
            proof.partition_metadata_path = portrait_path_utf8(commit_metadata_path);
            proof.native_reference = m_portrait_print.native_portrait_partition;
            proof.candidate_sha256 = owner.candidate_sha256;
            proof.carrier_sha256 = owner.source_sha256;
            proof.geometry_id = owner.geometry_id;
            proof.appearance_id = owner.appearance_id;
            proof.partition_digest = owner.partition_digest;
            proof.topology_hash = m_portrait_print.partition_topology_hash;
            proof.face_count = m_finishing_result.faces_after;
            portrait_certificate = std::move(proof);
            partition_reference["carrier_path"] = portrait_path_utf8(candidate);
            partition_reference["carrier_sha256"] = m_finishing_result.output_sha256;
            partition_reference["metadata_path"] = portrait_path_utf8(metadata_path);
            const auto encoded_blocks = PortraitColorBlocks::encode(*after->color_blocks);
            const auto region_semantics = encoded_blocks.at("region_semantics");
            const auto native_color_reduction = encoded_blocks.at("native_color_reduction");
            const auto final_slot_binding = encoded_blocks.at("final_slot_binding");
            const size_t region_count = region_semantics.at("regions").size();
            if (!PortraitPrint::valid_native_partition_reference(partition_reference,
                    m_portrait_print.basis_signature, m_portrait_print.partition_topology_hash) ||
                !PortraitPrint::valid_region_semantics(
                    region_semantics, m_portrait_print.basis_signature) ||
                !PortraitPrint::valid_native_color_reduction(native_color_reduction,
                    m_portrait_print.basis_signature, region_count) ||
                !PortraitPrint::valid_final_slot_binding(final_slot_binding,
                    m_portrait_print.basis_signature,
                    native_color_reduction.at("mapping_digest").get<std::string>(),
                    region_count, size_t(m_portrait_print.model_color_count)))
                throw std::runtime_error("原生分区、区域语义、减色映射或整区槽位无法发布。");
            metadata["native_portrait_partition"] = std::move(partition_reference);
            metadata["native_portrait_topology_certificate"] = certificate;
            metadata["region_semantics"] = region_semantics;
            metadata["native_color_reduction"] = native_color_reduction;
            metadata["final_slot_binding"] = final_slot_binding;
            metadata["partition_build_report"] = m_portrait_print.partition_build_report;
        } catch (const std::exception& failure) {
            if (m_beauty_transactions)
                m_beauty_transactions->finish(false, false, failure.what());
            rollback_portrait_color_save(failure.what());
            return;
        }
    }
    metadata["face_color_intent"] = m_model_preview->face_color_metadata();
    if (m_finishing_candidate_baked_appearance.is_object() &&
        m_finishing_candidate_baked_appearance.value("model_sha256",std::string())==m_finishing_result.output_sha256)
        metadata["baked_portrait_appearance"]=m_finishing_candidate_baked_appearance;
    const auto leaf_edits=m_model_preview->leaf_edit_metadata();
    if (!leaf_edits.is_null() && !leaf_edits.empty()) metadata["beauty_leaf_edit"]=leaf_edits;
    metadata["color_trial"] = m_model_preview->color_trial_metadata();
    const auto printer_restore = m_model_preview->printer_filament_restore_metadata();
    if (!printer_restore.empty()) metadata["printer_filament_restore"] = printer_restore;
    if (filament_application && m_apply_filaments_staged) {
        metadata["printer_filament_configuration"]["presets"] = m_apply_filaments_staged->filament_presets;
        for (const auto& key : m_apply_filaments_staged->project_config.keys())
            metadata["printer_filament_configuration"]["project"][key] = m_apply_filaments_staged->project_config.option(key)->serialize();
    }
    std::optional<AI::ModelMatchedColors> saved_match = filament_application
        ? m_apply_filaments_match : after->matched_colors;
    const bool requires_filament_rematch = beauty_candidate_requires_filament_rematch(
        topology_candidate, m_finishing_result.faces_before, m_finishing_result.faces_after,
        m_finishing_result.removed_degenerate_faces, m_finishing_result.removed_duplicate_faces);
    const bool stable_face_mapping = saved_match &&
        saved_match->face_slots.size() == m_finishing_result.faces_after &&
        !requires_filament_rematch;
    if (saved_match && !stable_face_mapping) {
        auto* bundle = wxGetApp().preset_bundle;
        const auto rematched = bundle ? m_model_preview->printer_filament_match_result(
            *bundle, m_finishing_result.output_sha256) : ModelPreview3D::PrinterFilamentMatchResult {};
        saved_match = rematched.matched;
        if (!saved_match) {
            m_save_and_return = false;
            if (m_beauty_transactions)
                m_beauty_transactions->finish(false, false, "topology candidate lost exact filament coverage");
            report(_L("处理后的面对应关系已变化，且无法重新完整匹配当前耗材；候选保留但不能接受。"));
            return;
        }
    }
    if (saved_match) {
        const auto portrait_trial = portrait_save
            ? m_model_preview->color_trial_state() : ModelPreviewColorControls::State {};
        const bool portrait_model_slots_only = !portrait_save ||
            (portrait_trial.project_color_slots.size() == size_t(m_portrait_print.model_color_count) &&
             std::all_of(saved_match->face_slots.begin(), saved_match->face_slots.end(),
                [&](size_t slot) {
                    return std::find(portrait_trial.project_color_slots.begin(),
                        portrait_trial.project_color_slots.end(), slot) !=
                        portrait_trial.project_color_slots.end();
                }));
        if (portrait_save && (!saved_match->subface_slots.empty() ||
            saved_match->face_slots.size() != m_finishing_result.faces_after ||
            !portrait_model_slots_only)) {
            m_save_and_return = false;
            if (m_beauty_transactions)
                m_beauty_transactions->finish(false, false, "portrait v9 requires face-only slot coverage");
            report(_L("人物 v9 配色必须逐面完整绑定且不能包含子面槽位；候选已保留。"));
            return;
        }
        // Appearance-only edits and smoothing retain face correspondence. Bind
        // the same slot snapshot to the newly published file identity. A
        // topology-changing candidate reached this point only after rematch.
        saved_match->source_sha256 = m_finishing_result.output_sha256;
        saved_match->geometry_id = m_model_preview->geometry_id();
        if (!saved_match->valid()) {
            m_save_and_return = false;
            if (m_beauty_transactions)
                m_beauty_transactions->finish(false, false, "invalid printer-filament match snapshot");
            report(_L("耗材匹配快照无效，候选保留但未保存。"));
            return;
        }
        after->matched_colors = saved_match;
        after->matched_configuration_signature = filament_application && m_apply_filaments_staged
            ? PrinterFilamentConfig::capture(*m_apply_filaments_staged).signature
            : m_beauty_color_gate_configuration_signature;
        metadata["applied_printer_filament_match"] = AI::encode_model_matched_colors(*saved_match);
    } else if (!portrait_save && m_finishing_workbench && m_workbench_editing) {
        m_save_and_return = false;
        if (m_beauty_transactions)
            m_beauty_transactions->finish(false, false, "missing printer-filament match snapshot");
        report(_L("当前美颜版本没有完整耗材匹配，候选保留但不能接受。"));
        return;
    }
    const auto provenance = m_model_preview->semantic_color_metadata();
    metadata["semantic_color_state"] = provenance.empty()
        ? m_finishing_candidate_semantic_provenance : provenance;
    const auto semantic_result = m_model_preview->semantic_result_metadata();
    if (!semantic_result.empty()) metadata["semantic_result"] = semantic_result;
    const auto region_reference = m_model_preview->semantic_region_evidence_metadata();
    const bool region_cache_unsaved = m_model_preview->semantic_regions_ready() && region_reference.empty();
    if (!region_reference.empty()) metadata["semantic_region_evidence"] = region_reference;
    const auto secondary_region_reference = m_model_preview->secondary_region_evidence_metadata();
    if (!secondary_region_reference.empty()) metadata["secondary_region_evidence"] = secondary_region_reference;
    if (!m_finishing_options.repair_mesh && m_finishing_selection_state.selected.size() == m_finishing_result.faces_after)
        metadata["local_selection"] = AI::SurfaceSelectionPersistence::encode(m_finishing_selection_state,
            m_finishing_result.faces_after, m_model_preview->geometry_id());
    else if (m_model_preview->leaf_editing()) metadata["local_selection"]=m_model_preview->selection_metadata();
    if (!m_reference_image_path.empty() && path_is_inside(root, m_reference_image_path))
        metadata["reference_image_path"] = m_reference_image_path.lexically_relative(root).generic_string();
    if (!m_raw_preview_path.empty() && path_is_inside(root, m_raw_preview_path))
        metadata["ai_image_path"] = m_raw_preview_path.lexically_relative(root).generic_string();
    nlohmann::json history_index = {
        {"schema", "orca.local-finishing-history/v1"},
        {"source", "local_finishing"},
        {"job_id", m_finishing_id},
        {"model_path", metadata.at("model_path")},
        {"model_sha256", m_finishing_result.output_sha256},
        {"generated_at", metadata.at("generated_at")},
        {"triangle_count", m_finishing_result.faces_after},
        {"prompt", metadata.at("prompt")},
        {"use_printable_colors", false}
    };
    const auto publication_id = boost::filesystem::unique_path("portrait-publication-%%%%-%%%%-%%%%").string();
    if (portrait_save) {
        metadata["portrait_publication_id"] = publication_id;
        history_index["portrait_publication_id"] = publication_id;
    }
    for (const char* key : {"reference_image_path", "ai_image_path"})
        if (metadata.contains(key)) history_index[key] = metadata.at(key);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    m_beauty_publication_committed = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    const auto committed = m_beauty_publication_committed;
    const auto portrait_save_flow_id = portrait_save ? m_portrait_save_flow_id : std::string{};
    const auto portrait_save_snapshot = portrait_save ? m_portrait_save_draft : nullptr;
    std::vector<std::pair<boost::filesystem::path, std::string>> portrait_owned_files;
    if (portrait_save) for (const auto& file : m_portrait_save_files) {
        // Ownership and an expected hash are both required. Reused files never
        // enter this list, even when their bytes equal the current candidate.
        const auto hash = file == candidate ? after->model_sha256 :
            file == after->original_appearance.carrier_model
                ? after->original_appearance.carrier_sha256 : std::string{};
        if (!hash.empty()) portrait_owned_files.emplace_back(file, hash);
    }
    const auto cache_root = boost::filesystem::path(Slic3r::data_dir()) / "cache";
    const auto revision = m_sequence;
    const bool resume_automatic_palette_after_accept = topology_candidate &&
        m_workbench_auto_palette_revision == revision &&
        m_workbench_auto_palette_path == source &&
        workbench_ai_palette_should_resume_after_accept(
            m_workbench_auto_palette_pending, verified_check.source,
            workbench_topology_safe(verified_check), verified_check.candidate_status);
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_finishing_running = m_busy = true;
    m_finishing_status->SetLabel(_L("正在保存版本和编辑记录，可取消……"));
    refresh_controls();
    try {
        m_finishing_worker = std::thread([weak, revision, canceled, committed, metadata_path,
            source, source_hash, candidate, before, after, source_context, cache_root, region_cache_unsaved,
            topology_candidate, verified_check, filament_application, portrait_save, root,
            publication_id, portrait_save_flow_id, portrait_save_snapshot,
            portrait_owned_files = std::move(portrait_owned_files),
            portrait_certificate = std::move(portrait_certificate),
            resume_automatic_palette_after_accept,
            topology_publication_artifacts = std::move(topology_publication_artifacts),
            metadata = std::move(metadata), history_index = std::move(history_index)]() mutable {
            std::string error;
            auto final_check = verified_check;
            try {
                if (portrait_save && !portrait_certificate)
                    throw std::runtime_error("Owned portrait publication certificate is missing.");
                if (topology_candidate || filament_application || portrait_save)
                    final_check = inspect_workbench_publication(candidate, source,
                        topology_candidate ? &verified_check : nullptr,
                        [canceled] { return canceled->load(); },
                        portrait_save ? &*portrait_certificate : nullptr);

                if (after->shape_details && after->shape_details->compatible(after->geometry_id, after->result.faces_after)) {
                    auto shapes = PortraitShapeCache::save(*after->shape_details, cache_root);
                    shapes["model_sha256"] = after->model_sha256;
                    metadata["portrait_shape_reference"] = std::move(shapes);
                    metadata["portrait_shapes_unlocked"] = after->shapes_unlocked;
                }
                if (after->color_blocks) {
                    if (after->color_trial.suite_id == "honor.people.six") {
                        const auto layer = PortraitColorBlocks::publish_verified_color_layer(
                            *after->color_blocks, cache_root / "portrait-color-blocks");
                        if (portrait_save && (layer.painted_faces != after->color_blocks->face_count ||
                            layer.painted_subfaces != 0))
                            throw std::runtime_error("人物 v9 配色层不是完整逐面颜色层。");
                        metadata["portrait_color_blocks"] = layer.reference;
                        metadata["portrait_applied_color_layer"] = layer.encode();
                    } else metadata["portrait_color_blocks"] = PortraitColorBlocks::save(
                        *after->color_blocks, cache_root / "portrait-color-blocks");
                }
                if (after->original_appearance.valid()) {
                    TriangleMesh saved_mesh;
                    ObjInfo saved_appearance;
                    std::string appearance_error;
                    if (!AI::load_model_artifact(candidate, saved_mesh, saved_appearance, appearance_error,
                            [canceled] { return canceled->load(); }) || saved_mesh.empty())
                        throw std::runtime_error(appearance_error.empty()
                            ? "无法校验原始配色证据。" : appearance_error);
                    ResolvedOriginalAppearance resolved;
                    if (!resolve_original_appearance_reference(after->original_appearance, saved_mesh.its,
                            root, resolved, appearance_error, [canceled] { return canceled->load(); }))
                        throw std::runtime_error(appearance_error.empty()
                            ? "原始配色证据校验失败。" : appearance_error);
                    metadata["original_appearance"] = resolved.reference.encode(root);
                }
                nlohmann::json document;
                const auto& options = after->options;
                if (options.beauty_appearance || options.beauty_deform || options.beauty_puzzle) {
                    document = BeautyWorkbenchControls::accepted_document(options, after->geometry_id);
                    if (after->partition && after->partition->puzzle)
                        document["puzzle"] = after->partition->puzzle->encode();
                } else {
                    AI::BeautyDocument beauty;
                    beauty.geometry_id = after->geometry_id;
                    beauty.face_count = after->result.faces_after;
                    beauty.face_patch.assign(beauty.face_count, 0);
                    document = beauty.encode();
                }
                document["model_sha256"] = after->model_sha256;
                if (portrait_save) metadata["beauty_workbench"] = document;
                if (portrait_save) {
                    const auto& reference = metadata.at("native_portrait_partition");
                    const auto encoded_blocks = PortraitColorBlocks::encode(*after->color_blocks);
                    if (encoded_blocks.at("region_semantics") != metadata.at("region_semantics") ||
                        encoded_blocks.at("native_color_reduction") !=
                            metadata.at("native_color_reduction") ||
                        encoded_blocks.at("final_slot_binding") !=
                            metadata.at("final_slot_binding"))
                        throw std::runtime_error("区域语义、减色映射或整区槽位在发布期间发生变化。");
                    metadata["post_color_check"] =
                        encode_portrait_post_color_check(final_check, reference);
                    if (!PortraitPrint::valid_post_color_check(
                            metadata.at("post_color_check"), reference))
                        throw std::runtime_error("着色后拓扑证据无效。");
                    metadata["workbench_check_certificate"] =
                        PortraitPrint::encode_workbench_check_certificate(final_check,
                            WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                            reference.at("geometry_id").get<std::string>(),
                            reference.at("appearance_id").get<std::string>(),
                            portrait_path_utf8(metadata_path), metadata,
                            reference.at("partition_digest").get<std::string>(),
                            reference.at("topology_hash").get<std::string>());
                    if (!PortraitPrint::valid_workbench_check_certificate(
                            metadata.at("workbench_check_certificate"),
                            WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                            final_check.model_sha256,
                            reference.at("geometry_id").get<std::string>(),
                            reference.at("appearance_id").get<std::string>(),
                            portrait_path_utf8(metadata_path), metadata,
                            reference.at("partition_digest").get<std::string>(),
                            reference.at("topology_hash").get<std::string>()))
                        throw std::runtime_error("着色后检查证书无效。");
                }
                const auto encoded = document.dump();
                AI::publish_beauty_version_record(metadata_path, candidate, source, metadata,
                    portrait_save ? nullptr : &encoded, [canceled] { return canceled->load(); }, &history_index);
                committed->store(true);
            } catch (const std::exception& e) {
                error = e.what();
                if (topology_candidate) {
                    boost::system::error_code ignored;
                    for (const auto& artifact : topology_publication_artifacts)
                        boost::filesystem::remove(artifact, ignored);
                }
            }
            wxGetApp().CallAfter([weak, revision, canceled, committed, candidate, before, after,
                source, source_hash, source_context, region_cache_unsaved, topology_candidate, filament_application,
                portrait_save, resume_automatic_palette_after_accept,
                final_check, metadata_path, publication_id, error, portrait_save_flow_id,
                portrait_save_snapshot, portrait_owned_files = std::move(portrait_owned_files)]() mutable {
                const auto cleanup_stale_portrait = [&] {
                    if (!portrait_save) return;
                    try {
                        bool foreign_record = false;
                        const auto retract_owned_record = [&](const boost::filesystem::path& path) {
                            if (!boost::filesystem::exists(path)) return;
                            if (boost::filesystem::is_symlink(path)) {
                                foreign_record = true; return;
                            }
                            nlohmann::json record;
                            { boost::filesystem::ifstream input(path); input >> record; }
                            if (record.value("portrait_publication_id", std::string{}) != publication_id) {
                                foreign_record = true; return;
                            }
                            boost::filesystem::remove(path);
                        };
                        auto index = metadata_path; index.replace_extension(".history.json");
                        retract_owned_record(index);
                        retract_owned_record(metadata_path);
                        // A conflicting older record may reference a reused path.
                        // Preserve its files rather than breaking existing history.
                        if (foreign_record) return;
                        committed->store(false);
                        for (const auto& owned : portrait_owned_files) {
                            const auto& file = owned.first;
                            if (file == source || (weak && (file == weak->m_displayed_model_path ||
                                file == weak->m_artifact_path || file == weak->m_finishing_source ||
                                (weak->m_model_preview && weak->m_model_preview->showing_model(file))))) continue;
                            if (boost::filesystem::is_symlink(file) ||
                                AI::model_artifact_sha256(file) != owned.second) continue;
                            boost::system::error_code ignored;
                            boost::filesystem::remove(file, ignored);
                            if (weak) weak->m_beauty_candidate_files.erase(std::remove(
                                weak->m_beauty_candidate_files.begin(), weak->m_beauty_candidate_files.end(), file),
                                weak->m_beauty_candidate_files.end());
                        }
                    } catch (const std::exception& failure) {
                        BOOST_LOG_TRIVIAL(error) << "stale portrait publication cleanup: " << failure.what();
                    }
                };
                if (!weak || weak->m_shutdown || revision != weak->m_sequence ||
                    weak->m_beauty_publication_committed != committed ||
                    (portrait_save && (!weak->portrait_color_save_snapshot_current() ||
                        weak->m_portrait_save_flow_id != portrait_save_flow_id ||
                        weak->m_portrait_save_draft != portrait_save_snapshot))) {
                    cleanup_stale_portrait();
                    return;
                }
                auto* self = weak.get();
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = self->m_busy = false;
                if (!committed->load()) {
                    if (portrait_save) {
                        self->rollback_portrait_color_save(error);
                        return;
                    }
                    self->m_save_and_return = false;
                    if (self->m_beauty_transactions)
                        self->m_beauty_transactions->finish(false, false, error);
                    if (topology_candidate) {
                        const bool save_canceled = canceled->load();
                        const auto hash = self->m_finishing_result.source_sha256;
                        self->m_workbench_auto_repair = false;
                        // A topology candidate and its immutable appearance
                        // carrier are one publication. Never leave either as a
                        // retryable draft after metadata publication failed.
                        self->m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
                        self->discard_model_finishing();
                        self->m_workbench_check_result = {};
                        self->m_workbench_check_result.source = final_check.source;
                        self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                        self->m_workbench_check_result.phase = WorkbenchCheckPhase::Idle;
                        self->m_workbench_check_result.model_sha256 = hash;
                        self->m_workbench_check_result.source_sha256 = hash;
                        self->m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
                        self->m_workbench_check_result.summary = save_canceled
                            ? "已取消保存，修复候选和原始外观载体已回滚。"
                            : "修复版本保存失败，候选和原始外观载体已回滚。";
                        self->m_workbench_check_result.repair_reason = self->m_workbench_check_result.summary;
                        if (!error.empty()) self->m_workbench_check_result.repair_reason += " " + error;
                        self->m_workbench_check_path = source.string();
                        self->m_workbench_check_revision = self->m_sequence;
                        self->m_finishing_status->SetLabel(from_u8(self->m_workbench_check_result.repair_reason));
                    } else self->m_finishing_status->SetLabel(canceled->load()
                        ? _L("已取消保存，候选和草稿保留，可再次保存。")
                        : _L("版本保存失败，候选和草稿保留：") + from_u8(error));
                    self->m_workbench_operation_notice = into_u8(self->m_finishing_status->GetLabel());
                    if ((self->portrait_owns_display_context() || self->portrait_owns_save_candidate()) &&
                        self->m_portrait_print.running() &&
                        (self->m_portrait_print.stage == PortraitPrint::Stage::Check ||
                        self->m_portrait_print.stage == PortraitPrint::Stage::SaveColor))
                        self->pause_portrait_print(self->m_workbench_operation_notice);
                    self->finish_printer_filament_application(false);
                    self->refresh_controls();
                    return;
                }
                std::string commit_error;
                if (filament_application && !self->commit_printer_filament_application(commit_error)) {
                    // These records were created by this publication only. The
                    // original model and all pre-existing history stay intact.
                    boost::system::error_code ignored;
                    boost::filesystem::remove(metadata_path, ignored);
                    auto index = metadata_path; index.replace_extension(".history.json");
                    boost::filesystem::remove(index, ignored);
                    self->m_workbench_operation_notice = commit_error;
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, commit_error);
                    self->finish_printer_filament_application(false);
                    self->refresh_controls(); return;
                }
                if (portrait_save && !self->portrait_color_published(candidate, final_check)) {
                    // The candidate and source stay intact, while the final
                    // history markers are rolled back so no half-accepted
                    // portrait version can appear after a ledger failure.
                    boost::system::error_code ignored;
                    boost::filesystem::remove(metadata_path, ignored);
                    auto index = metadata_path; index.replace_extension(".history.json");
                    boost::filesystem::remove(index, ignored);
                    committed->store(false);
                    if (self->m_beauty_transactions)
                        self->m_beauty_transactions->finish(false, false,
                            "portrait flow commit failed");
                    self->m_save_and_return = false;
                    self->rollback_portrait_color_save("portrait flow commit failed");
                    return;
                }
                self->m_finishing_undo_path = self->m_finishing_source;
                self->m_finishing_restore_context = source_context;
                self->m_finishing_accepted_path = candidate;
                self->m_beauty_accepted_files.push_back(candidate);
                if (after->original_appearance.valid() &&
                    std::find(self->m_beauty_candidate_files.begin(), self->m_beauty_candidate_files.end(),
                              after->original_appearance.carrier_model) != self->m_beauty_candidate_files.end())
                    self->m_beauty_accepted_files.push_back(after->original_appearance.carrier_model);
                self->select_local_finishing_version(candidate, self->m_finishing_id);
                if (resume_automatic_palette_after_accept) {
                    self->m_workbench_auto_palette_pending = true;
                    self->m_workbench_auto_palette_revision = self->m_sequence;
                    self->m_workbench_auto_palette_path = candidate;
                }
                const auto accepted_context = self->capture_model_context();
                self->m_finishing_candidate.clear();
                self->m_portrait_color_candidate_identity = {};
                self->m_portrait_save_draft.reset();
                self->m_portrait_save_task.reset();
                self->m_portrait_save_flow_id.clear();
                self->m_portrait_save_files.clear();
                self->m_finishing_candidate_original_appearance = {};
                self->m_finishing_candidate_original_appearance_error.clear();
                after->source = candidate;
                after->candidate.clear();
                after->options = {};
                after->manual_color_dirty = false;
                after->dirty = false;
                if (after->matched_colors) {
                    self->m_beauty_matched_colors = after->matched_colors;
                    self->m_beauty_color_gate_configuration_signature =
                        after->matched_configuration_signature;
                    self->m_beauty_color_gate_cache.reset();
                    self->m_beauty_color_gate_cache_key.clear();
                    self->m_beauty_color_gate_expected_key.clear();
                    self->m_beauty_color_gate_state = BeautyColorGateState::Compatible;
                    self->m_beauty_color_gate_message =
                        "模型颜色已逐面绑定到当前耗材，可安全编辑。";
                }
                if (self->m_finishing_workbench) {
                    self->m_model_preview->set_selection_preview_suppressed(true);
                    self->m_finishing_source.clear();
                    self->m_finishing_undo_path.clear();
                    self->m_finishing_redo_path.clear();
                    self->m_finishing_source_context = {};
                    self->m_finishing_restore_context = {};
                    self->m_finishing_restore_selection = {};
                    self->m_beauty_session_source.reset();
                    self->m_beauty_compare_draft.reset();
                }
                self->m_beauty_manual_color_dirty = false;
                if (self->m_beauty_controls) self->m_beauty_controls->mark_saved();
                if (self->m_beauty_transactions) {
                    self->m_beauty_transactions->finish(true, false);
                    if (before && self->m_finishing_workbench) {
                        self->m_beauty_transactions->truncate_to(self->m_beauty_session_undo_base);
                        const auto restore = [self](const auto& snapshot, const auto& context) {
                            if (!self->restore_beauty_candidate(*snapshot)) return false;
                            if (context) context();
                            ++self->m_sequence;
                            self->m_finishing_source.clear();
                            self->m_finishing_undo_path.clear();
                            self->m_finishing_redo_path.clear();
                            self->m_finishing_source_context = {};
                            self->m_beauty_session_source.reset();
                            self->m_beauty_manual_color_dirty = snapshot->manual_color_dirty;
                            if (self->m_beauty_controls) self->m_beauty_controls->set_dirty(snapshot->dirty);
                            self->m_workbench_check_result = {};
                            self->refresh_controls();
                            return true;
                        };
                        self->m_beauty_transactions->record({
                            BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate,
                            "accepted Beauty version", {}, {}, {}, {},
                            [before, source_context, restore] { return restore(before, source_context); },
                            [after, accepted_context, restore] { return restore(after, accepted_context); }});
                    }
                    self->m_beauty_session_undo_base = self->m_beauty_transactions->undo_count();
                }
                self->m_finishing_status->SetLabel(_L("新版本已保存到模型库，可继续编辑、撤销或导入。"));
                if (region_cache_unsaved) self->m_finishing_status->SetLabel(self->m_finishing_status->GetLabel() +
                    _L("区域缓存未保存，重新打开后需要重新识别。"));
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
                if (topology_candidate || filament_application || portrait_save) {
                    if (topology_candidate) {
                        // The candidate is already the accepted model at this
                        // point. Do not leave Ready in the workbench state or
                        // the import gate will keep waiting for a button that
                        // no longer exists.
                        final_check.candidate_status = RepairCandidateStatus::Accepted;
                        final_check.candidate_path.clear();
                    }
                    self->m_workbench_check_result = final_check;
                    self->m_workbench_check_path = candidate.string();
                    self->m_workbench_check_revision = self->m_sequence;
                } else self->m_workbench_check_result = {};
                self->m_workbench_operation_notice = topology_candidate ? "修复已接受并保存" : "新版本已保存";
                if (topology_candidate)
                    self->portrait_repair_published(source_hash, final_check.model_sha256, candidate);
                self->finish_printer_filament_application(true);
                self->m_workbench_history_filter = self->m_workbench_history_page = 0;
                if (self->m_workbench_history_search) self->m_workbench_history_search->ChangeValue(wxEmptyString);
                self->cancel_library_loading();
                self->load_library_entries(self->m_finishing_workbench);
                self->refresh_controls();
                self->update_finishing_selection();
                self->finish_workbench_save();
                self->m_workbench_auto_repair = false;
                if (resume_automatic_palette_after_accept)
                    self->apply_pending_workbench_ai_palette(candidate, self->m_sequence);
                self->ensure_workbench_check();
                self->queue_portrait_print();
            });
        });
    } catch (const std::exception& error) {
        m_finishing_running = m_busy = m_save_and_return = false;
        if (portrait_save) { rollback_portrait_color_save(error.what()); return; }
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, error.what());
        if (topology_candidate) {
            const auto hash = m_finishing_result.source_sha256;
            m_workbench_auto_repair = false;
            m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
            discard_model_finishing();
            m_workbench_check_result = {};
            m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
            m_workbench_check_result.phase = WorkbenchCheckPhase::Idle;
            m_workbench_check_result.model_sha256 = hash;
            m_workbench_check_result.source_sha256 = hash;
            m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
            m_workbench_check_result.summary = "无法启动修复版本保存，候选和原始外观载体已回滚。";
            m_workbench_check_result.repair_reason = m_workbench_check_result.summary + " " + error.what();
            m_workbench_check_path = source.string();
            m_workbench_check_revision = m_sequence;
            m_finishing_status->SetLabel(from_u8(m_workbench_check_result.repair_reason));
        } else {
            m_finishing_status->SetLabel(_L("暂时无法启动保存，候选保留：") + from_u8(error.what()));
        }
        m_workbench_operation_notice = into_u8(m_finishing_status->GetLabel());
        if ((portrait_owns_display_context() || portrait_owns_save_candidate()) &&
            m_portrait_print.running() &&
            (m_portrait_print.stage == PortraitPrint::Stage::Check ||
            m_portrait_print.stage == PortraitPrint::Stage::SaveColor)) pause_portrait_print(m_workbench_operation_notice);
        finish_printer_filament_application(false);
        refresh_controls();
    }
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_workbench_check_running || (m_finishing_candidate.empty() &&
        !beauty_session_has_changes())) return;
    const auto discarded = m_finishing_candidate;
    const bool beauty = bool(m_beauty_session_source);
    if (m_beauty_session_source) {
        if (!restore_beauty_candidate(*m_beauty_session_source)) return;
    } else if (!show_finishing_version(m_finishing_source)) return;
    if (m_beauty_transactions) m_beauty_transactions->truncate_to(m_beauty_session_undo_base);
    clear_unaccepted_beauty_candidates(m_beauty_session_file_base);
    if (!beauty) {
        boost::system::error_code ignored;
        boost::filesystem::remove(discarded, ignored);
    }
    m_beauty_session_source.reset();
    m_beauty_compare_draft.reset();
    m_finishing_candidate.clear();
    m_portrait_color_candidate_identity = {};
    m_finishing_candidate_original_appearance = {};
    m_finishing_candidate_original_appearance_error.clear();
    if (m_workbench_check_result.candidate_status == RepairCandidateStatus::Ready) {
        m_workbench_check_result = {};
        ensure_workbench_check();
    }
    m_model_preview->set_selection_preview_suppressed(false);
    m_finishing_status->SetLabel(_L("已放弃预览，恢复处理前模型。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：处理前模型。"));
    refresh_controls();
    update_finishing_selection();
    if (m_finishing_restore_selection)
        m_finishing_restore_selection();
}

void ModelGenerationPanel::undo_model_finishing()
{
    if (m_busy || m_workbench_check_running || m_finishing_undo_path.empty()) return;
    const auto redo_colors = m_model_preview->color_trial_state();
    if (!show_finishing_version(m_finishing_undo_path)) return;
    m_finishing_redo_preview = [this, redo_colors] { m_model_preview->restore_color_trial(redo_colors); };
    m_finishing_redo_path = m_finishing_accepted_path;
    m_finishing_redo_id = m_displayed_model_job_id;
    m_finishing_redo_source = m_finishing_undo_path;
    ++m_sequence;
    if (m_finishing_restore_context) m_finishing_restore_context();
    if (m_finishing_restore_selection) m_finishing_restore_selection();
    refresh_model_quality_card();
    m_finishing_undo_path.clear(); m_finishing_source.clear();
    m_finishing_status->SetLabel(_L("已返回上个版本。处理后的版本仍在模型库中，可随时重新选用。"));
    m_status->SetLabel(_L("已返回上个版本，可继续导入。"));
    m_model_preview_message->SetLabel(_L("当前显示：上个版本。"));
    refresh_controls();
    ensure_workbench_check();
}

void ModelGenerationPanel::redo_model_finishing()
{
    if (m_busy || m_workbench_check_running || m_finishing_redo_path.empty() || m_displayed_model_path != m_finishing_redo_source) return;
    if (!show_finishing_version(m_finishing_redo_path)) return;
    m_finishing_undo_path = m_finishing_redo_source;
    m_finishing_accepted_path = m_finishing_redo_path;
    select_local_finishing_version(m_finishing_redo_path, m_finishing_redo_id);
    if (m_finishing_workbench) m_model_preview->set_selection_preview_suppressed(true);
    if (m_finishing_redo_preview) m_finishing_redo_preview();
    m_finishing_redo_path.clear();
    m_finishing_status->SetLabel(_L("已重做修整，恢复已保存版本。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
    refresh_controls(); update_finishing_selection();
    ensure_workbench_check();
}

void ModelGenerationPanel::stop_model_finishing()

{

    if (m_finishing_canceled) m_finishing_canceled->store(true);

    if (m_finishing_worker.joinable()) m_finishing_worker.join();

    m_residual_review.reset();

    if(m_model_preview)m_model_preview->clear_portrait_residual_preview();

    if(m_beauty_controls)m_beauty_controls->set_residual_review(false);

    m_finishing_running = false;

    // Publication may complete while the close request waits for the worker.

    if (m_beauty_publication_committed && m_beauty_publication_committed->load() && !m_finishing_candidate.empty()) {

        m_finishing_accepted_path = m_finishing_candidate;

        m_beauty_accepted_files.push_back(m_finishing_candidate);

        if (m_finishing_candidate_original_appearance.valid() &&
            std::find(m_beauty_candidate_files.begin(), m_beauty_candidate_files.end(),
                      m_finishing_candidate_original_appearance.carrier_model) != m_beauty_candidate_files.end())
            m_beauty_accepted_files.push_back(m_finishing_candidate_original_appearance.carrier_model);

    }

    clear_unaccepted_beauty_candidates();

    if (!m_finishing_candidate.empty() && m_finishing_candidate != m_finishing_accepted_path) {

        boost::system::error_code ignored; boost::filesystem::remove(m_finishing_candidate, ignored);

    }

    m_finishing_candidate.clear();
    m_finishing_candidate_original_appearance = {};
    m_finishing_candidate_original_appearance_error.clear();

}

} // namespace Slic3r::GUI
