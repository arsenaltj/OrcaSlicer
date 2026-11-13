#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyRestorePreparation.hpp"
#include "PortraitColorLayerPublication.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchModelInspection.hpp"
#include "NativeWorkbenchPublication.hpp"
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

}
void ModelGenerationPanel::show_finishing_version_async(const boost::filesystem::path& path,
    std::function<void(bool)> complete, std::function<bool()> may_install)
{
    BeautyCandidateSnapshot target;
    target.source = path;
    const bool session_source = m_finishing_workbench && m_beauty_session->source() &&
        path == m_beauty_session->source()->source;
    if (path == m_finishing_candidate) {
        target.model_sha256 = m_finishing_result.output_sha256;
        target.face_overrides = m_finishing_candidate_face_overrides;
        target.original_appearance = m_finishing_candidate_original_appearance;
        target.original_appearance_error = m_finishing_candidate_original_appearance_error;
    } else if (session_source) target = *m_beauty_session->source();
    wxWeakRef<ModelGenerationPanel> weak(this);
    prepare_beauty_restoration(std::move(target), [weak, path, complete = std::move(complete), may_install = std::move(may_install)](auto prepared) {
        if (!weak || weak->m_shutdown) return;
        const bool ok = (!may_install || may_install()) && weak->show_finishing_version_prepared(path, prepared);
        if (complete) complete(ok);
    });
}

bool ModelGenerationPanel::show_finishing_version_prepared(const boost::filesystem::path& path,
    const std::shared_ptr<BeautyRestorePreparation>& preparation)
{
    Presentation::assert_gui_thread();
    if (!preparation || !preparation->error.empty() || preparation->model.path != path) {
        if (preparation) m_finishing_status->SetLabel(_L("预览加载失败，原件仍保留：") + from_u8(preparation->error));
        return false;
    }
    const auto view = m_model_preview->view_state();
    const auto color_state = m_model_preview->color_trial_state();
    const auto block_state=m_model_preview->portrait_color_blocks();
    const auto printer_restore=m_model_preview->printer_restore_snapshot();
    size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero(); std::string error;
    const bool session_source = m_finishing_workbench && m_beauty_session->source() &&
        path == m_beauty_session->source()->source;
    invalidate_printer_filament_trial_for_model_change();
    if (!m_model_preview->load_prepared_model(std::move(preparation->model), {}, triangles, dimensions, colors, error)) {
        m_finishing_status->SetLabel(_L("预览加载失败，原件仍保留：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    if (path == m_finishing_candidate)
        m_model_preview->restore_semantic_region_evidence(m_finishing_candidate_region_evidence, m_finishing_candidate_region_error);
    else if (session_source)
        m_model_preview->restore_semantic_region_evidence(m_beauty_session->source()->region_evidence, m_beauty_session->source()->region_evidence_error);
    if (path == m_finishing_candidate)
        m_model_preview->restore_secondary_region_evidence(m_finishing_candidate_secondary_evidence, m_finishing_candidate_secondary_error);
    else if (session_source)
        m_model_preview->restore_secondary_region_evidence(m_beauty_session->source()->secondary_evidence, m_beauty_session->source()->secondary_evidence_error);
    if (path == m_finishing_candidate &&
        (!m_finishing_candidate_semantic_faces.empty() || !m_finishing_candidate_semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(color_state);
        m_model_preview->set_saved_semantic_result(
            m_finishing_candidate_semantic_faces, m_finishing_candidate_semantic_subfaces);
        m_model_preview->synchronize_project_bound_semantics(color_state, m_model_preview->color_trial_state());
    } else if (session_source && (!m_beauty_session->source()->semantic_faces.empty() ||
                                  !m_beauty_session->source()->semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(m_beauty_session->source()->color_trial);
        m_model_preview->set_saved_semantic_result(
            m_beauty_session->source()->semantic_faces, m_beauty_session->source()->semantic_subfaces);
        m_model_preview->synchronize_project_bound_semantics(
            m_beauty_session->source()->color_trial, m_model_preview->color_trial_state());
    } else if (m_finishing_workbench || m_model_preview->semantic_result_active())
        m_model_preview->restore_color_trial_without_recognition(color_state);
    else m_model_preview->restore_color_trial(color_state);
    if(path==m_finishing_candidate) {
        m_model_preview->restore_portrait_color_blocks(block_state);
        m_model_preview->restore_printer_restore_snapshot(printer_restore);
    } else if(session_source) {
        m_model_preview->restore_portrait_color_blocks(m_beauty_session->source()->color_blocks);
        m_model_preview->restore_printer_restore_snapshot(m_beauty_session->source()->printer_restore);
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

void ModelGenerationPanel::accept_model_finishing(std::optional<AI::ModelMatchedColors> prepared_match)
{
    auto report = [this](const wxString& message) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        auto finish = [weak, message](bool) {
            if (!weak || weak->m_shutdown) return;
            weak->m_workbench_operation_notice = into_u8(message);
            weak->m_finishing_status->SetLabel(message);
            weak->m_status->SetLabel(message);
            weak->refresh_controls();
            weak->publish_workbench_state();
        };
        if (m_workbench_auto_repair && !m_busy && !m_finishing_running && !m_beauty_restore_running && !m_workbench_check_running) {
            m_workbench_auto_repair = false;
            m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
            discard_model_finishing_async([weak, message, finish](bool restored) {
                if (!weak || weak->m_shutdown) return;
                weak->fail_workbench_check(into_u8(message));
                finish(restored);
            });
        } else finish(true);
    };
    if (m_busy || m_finishing_running || m_beauty_restore_running || m_workbench_check_running) {
        report(_L("正在处理模型，请等待完成后接受候选。")); return;
    }
    if (m_finishing_candidate.empty()) {
        report(_L("没有可接受的候选，请先检查或生成候选。")); return;
    }
    const bool topology_candidate = m_workbench_check_result.candidate_status == RepairCandidateStatus::Ready &&
        m_finishing_options.repair_mesh && !m_finishing_options.smooth_surface;
    if (!topology_candidate && m_finishing_workbench && m_workbench_editing && !beauty_color_match_current()) {
        m_save_and_return = false;
        report(_L("当前模型配色尚未完整绑定到耗材；候选保留，请恢复兼容配置后再保留效果。"));
        return;
    }
    if (topology_candidate && !workbench_repair_publication_allowed(
            m_workbench_check_result, m_finishing_result.repair_geometry_verified)) {
        report(_L("修复未通过拓扑、几何或配色复检，原模型保留；可重新检查。")); return;
    }
    // A repair already has a loaded candidate and a captured colour trial.
    // Baking that trial first would replace the repair candidate and recursively
    // route Accept through Save. Identity-bound edits were confirmed above.
    if (!topology_candidate && m_finishing_workbench && m_beauty_manual_color_dirty) {
        if (!request_save_and_return()) report(_L("颜色草稿暂时无法保存，请检查耗材匹配状态。"));
        return;
    }
    if (!m_model_preview->showing_model(m_finishing_candidate)) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        show_finishing_version_async(m_finishing_candidate, [weak](bool restored) {
            if (!weak || weak->m_shutdown) return;
            if (restored) weak->accept_model_finishing();
            else {
                weak->m_save_and_return = false;
                weak->m_finishing_status->SetLabel(_L("候选预览未加载，候选保留；请重新预览后接受。"));
                weak->refresh_controls();
            }
        });
        return;
    }
    if (m_beauty_transactions &&
        !m_beauty_transactions->begin(BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate)) {
        report(_L("当前仍有模型处理正在进行，请先完成或取消。")); return;
    }
    m_workbench_operation_notice = "正在接受候选并保存，可取消……";
    const auto root = generated_models_root();
    const auto before = m_beauty_session->source();
    const auto source_context = m_finishing_source_context;
    auto after = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    const auto verified_check = m_workbench_check_result;
    std::optional<WorkbenchNativeTopologyCertificate> native_proof;
    if (after->native_portrait_partition && after->native_portrait_partition->valid() && after->color_blocks) {
        WorkbenchNativeTopologyCertificate proof;
        proof.partition_metadata = m_native_workbench_partition_commit;
        proof.native_reference = m_native_workbench_partition_reference;
        if (!WorkbenchArtifacts::valid_native_partition_reference(proof.native_reference)) {
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "native reference unavailable");
            report(_L("原生分区引用无法验证，候选保留；请重新打开已保存版本。")); return;
        }
        proof.partition_metadata_path = proof.native_reference.at("metadata_path").get<std::string>();
        proof.candidate_sha256 = after->model_sha256;
        proof.carrier_sha256 = m_finishing_result.source_sha256;
        proof.geometry_id = after->geometry_id;
        proof.appearance_id = m_model_preview->semantic_source_content_id();
        proof.partition_digest = after->native_portrait_partition->partition_digest;
        proof.topology_hash = proof.native_reference.at("topology_hash").get<std::string>();
        proof.face_count = m_finishing_result.faces_after;
        native_proof = std::move(proof);
    }
    const bool filament_application = m_apply_filaments_saving;
    if (filament_application && m_apply_filaments_staged) {
        after->project_config = std::make_shared<DynamicPrintConfig>(m_apply_filaments_staged->project_config);
        after->project_presets = m_apply_filaments_staged->filament_presets;
    }
    const auto source = m_finishing_source;
    const auto candidate = m_finishing_candidate;
    const auto semantic_appearance_id = m_model_preview->semantic_source_content_id();
    const auto saved_appearance_id = semantic_appearance_id.empty() ? after->model_sha256 : semantic_appearance_id;
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
    const int ai_count = original_appearance_detail::ai_source_color_count(source, root);
    if (ai_count >= 4 && ai_count <= 6)
        metadata["ai_source_color_count"] = ai_count;
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
        if (!prepared_match) {
            // No publication state or candidate ownership has changed yet.
            // Re-enter the same acceptance transaction after the strict match.
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "filament rematch pending");
            wxWeakRef<ModelGenerationPanel> weak(this);
            const bool accepted = request_workbench_filament_match([weak](BeautyColorGateResult match) {
                if (!weak || weak->m_shutdown) return;
                if (match.matched) weak->accept_model_finishing(std::move(match.matched));
                else {
                    weak->m_save_and_return = false;
                    weak->m_finishing_status->SetLabel(_L("处理后的面对应关系已变化，且无法重新完整匹配当前耗材；候选保留但不能接受。"));
                    weak->m_workbench_operation_notice = into_u8(weak->m_finishing_status->GetLabel());
                    weak->refresh_controls(); weak->publish_workbench_state();
                }
            });
            if (!accepted) report(_L("处理后的面对应关系已变化，且无法重新完整匹配当前耗材；候选保留但不能接受。"));
            return;
        }
        saved_match = std::move(prepared_match);
        if (!saved_match) {
            m_save_and_return = false;
            if (m_beauty_transactions)
                m_beauty_transactions->finish(false, false, "topology candidate lost exact filament coverage");
            report(_L("处理后的面对应关系已变化，且无法重新完整匹配当前耗材；候选保留但不能接受。"));
            return;
        }
    }
    if (saved_match) {
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
    } else if (!topology_candidate && m_finishing_workbench && m_workbench_editing) {
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
    for (const char* key : {"reference_image_path", "ai_image_path"})
        if (metadata.contains(key)) history_index[key] = metadata.at(key);
    Presentation::background_tasks().retire(m_finishing_worker);
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    m_beauty_publication_committed = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    const auto committed = m_beauty_publication_committed;
    const auto cache_root = boost::filesystem::path(Slic3r::data_dir()) / "cache";
    const auto revision = m_sequence;
    const bool resume_automatic_palette_after_accept = topology_candidate &&
        m_workbench_auto_palette_revision == revision &&
        m_workbench_auto_palette_path == source &&
        workbench_ai_palette_should_resume_after_accept(
            m_workbench_auto_palette_pending, verified_check.source,
            workbench_topology_safe(verified_check), verified_check.candidate_status);
    const auto session_revision = m_beauty_session->revision();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_finishing_running = m_busy = true;
    m_finishing_status->SetLabel(_L("正在保存版本和编辑记录，可取消……"));
    refresh_controls();
    try {
        m_finishing_worker = std::thread([weak, revision, session_revision, canceled, committed, metadata_path,
            source, source_hash, candidate, before, after, source_context, cache_root, region_cache_unsaved,
            topology_candidate, verified_check, filament_application, root, native_proof, saved_appearance_id,
            resume_automatic_palette_after_accept,
            topology_publication_artifacts = std::move(topology_publication_artifacts),
            metadata = std::move(metadata), history_index = std::move(history_index)]() mutable {
            std::string error;
            auto final_check = verified_check;
            try {
                final_check = inspect_workbench_publication(candidate, source,
                    topology_candidate ? &verified_check : nullptr,
                    [canceled] { return canceled->load(); },
                    native_proof ? &*native_proof : nullptr);

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
                if (native_proof) {
                    prepare_native_workbench_publication(metadata, metadata_path, candidate, *after->color_blocks, *native_proof);
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
                metadata["beauty_workbench"] = std::move(document);
                if (native_proof) {
                    certify_native_workbench_publication(metadata, metadata_path, final_check);
                } else {
                    final_check.purpose = WorkbenchCheckPurpose::SavedModelReadOnly;
                    auto saved_check = final_check;
                    if (topology_candidate) {
                        saved_check.before_faces = final_check.after_faces;
                        saved_check.before_vertices = final_check.after_vertices;
                    }
                    auto certificate = WorkbenchArtifacts::encode_workbench_check_certificate(saved_check,
                        WorkbenchCheckPurpose::SavedModelReadOnly, after->geometry_id, saved_appearance_id,
                        boost::nowide::narrow(metadata_path.wstring()), metadata);
                    if (!WorkbenchArtifacts::valid_workbench_check_certificate(certificate,
                            WorkbenchCheckPurpose::SavedModelReadOnly, after->model_sha256,
                            after->geometry_id, saved_appearance_id, boost::nowide::narrow(metadata_path.wstring()), metadata))
                        throw std::runtime_error("Saved model check certificate is invalid.");
                    metadata["workbench_check_certificate"] = std::move(certificate);
                }
                AI::publish_beauty_version_record(metadata_path, candidate, source, metadata,
                    nullptr, [canceled] { return canceled->load(); }, &history_index);
                committed->store(true);
            } catch (const std::exception& e) {
                error = e.what();
                if (topology_candidate) {
                    boost::system::error_code ignored;
                    for (const auto& artifact : topology_publication_artifacts)
                        boost::filesystem::remove(artifact, ignored);
                }
            }
            wxGetApp().CallAfter([weak, revision, session_revision, canceled, committed, candidate, before, after,
                source, source_hash, source_context, region_cache_unsaved, topology_candidate, filament_application, native_proof,
                resume_automatic_palette_after_accept, final_check, metadata_path, error]() mutable {
                if (!weak || weak->m_shutdown || revision != weak->m_sequence ||
                    !weak->m_beauty_session->is_current(session_revision) ||
                    weak->m_beauty_publication_committed != committed) return;
                auto* self = weak.get();
                Presentation::background_tasks().retire(self->m_finishing_worker);
                self->m_finishing_running = self->m_busy = false;
                if (!committed->load()) {
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
                        self->discard_model_finishing_async([weak, save_canceled, hash, final_check, source, error](bool restored) {
                            if (!weak || weak->m_shutdown) return;
                            auto* self = weak.get();
                            if (!restored) BOOST_LOG_TRIVIAL(error) << "Repair publication rollback failed; draft retained";
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
                            self->m_workbench_operation_notice = into_u8(self->m_finishing_status->GetLabel());
                            self->finish_printer_filament_application(false);
                            self->refresh_controls();
                        });
                        return;
                    } else self->m_finishing_status->SetLabel(canceled->load()
                        ? _L("已取消保存，候选和草稿保留，可再次保存。")
                        : _L("版本保存失败，候选和草稿保留：") + from_u8(error));
                    self->m_workbench_operation_notice = into_u8(self->m_finishing_status->GetLabel());
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
                self->m_finishing_undo_path = self->m_finishing_source;
                self->m_finishing_restore_context = source_context;
                self->m_finishing_accepted_path = candidate;
                self->select_local_finishing_version(candidate, self->m_finishing_id);
                self->publish_native_workbench_context();
                if (resume_automatic_palette_after_accept) {
                    self->m_workbench_auto_palette_pending = true;
                    self->m_workbench_auto_palette_revision = self->m_sequence;
                    self->m_workbench_auto_palette_path = candidate;
                }
                const auto accepted_context = self->capture_model_context();
                self->m_finishing_candidate.clear();
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
                    self->m_beauty_compare_draft.reset();
                }
                if (self->m_beauty_controls) self->m_beauty_controls->mark_saved();
                BeautyWorkbenchTransactionController::Entry history;
                if (before && self->m_finishing_workbench) {
                    const auto restore = [weak](const auto& snapshot, const auto& context, std::function<void(bool)> complete) {
                        if (!weak || weak->m_shutdown) { complete(false); return; }
                        weak->restore_beauty_candidate_async(*snapshot, [weak, snapshot, context, complete](bool restored) {
                        if (!weak || !restored) { complete(false); return; }
                        auto* self = weak.get();
                        if (context) context();
                        ++self->m_sequence;
                        self->publish_native_workbench_context();
                        self->m_finishing_source.clear();
                        self->m_finishing_undo_path.clear();
                        self->m_finishing_redo_path.clear();
                        self->m_finishing_source_context = {};
                        self->m_beauty_session->clear_draft();
                        self->m_beauty_manual_color_dirty = snapshot->manual_color_dirty;
                        if (self->m_beauty_controls) self->m_beauty_controls->set_dirty(snapshot->dirty);
                        self->m_workbench_check_result = {};
                        self->refresh_controls();
                        complete(true);
                        });
                    };
                    history = {BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate,
                        "accepted Beauty version", {}, {}, {}, {}, {}, {},
                        [before, source_context, restore](auto complete) { restore(before, source_context, std::move(complete)); },
                        [after, accepted_context, restore](auto complete) { restore(after, accepted_context, std::move(complete)); }};
                }
                self->m_beauty_session->complete_accept(session_revision, candidate,
                    after->original_appearance.valid() ? after->original_appearance.carrier_model : boost::filesystem::path {},
                    std::move(history));
                self->m_finishing_status->SetLabel(_L("新版本已保存到模型库，可继续编辑、撤销或导入。"));
                if (region_cache_unsaved) self->m_finishing_status->SetLabel(self->m_finishing_status->GetLabel() +
                    _L("区域缓存未保存，重新打开后需要重新识别。"));
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
                {
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
                }
                self->m_workbench_operation_notice = topology_candidate ? "修复已接受并保存" : "新版本已保存";
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
            });
        });
    } catch (const std::exception& error) {
        m_finishing_running = m_busy = m_save_and_return = false;
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, error.what());
        if (topology_candidate) {
            const auto hash = m_finishing_result.source_sha256;
            m_workbench_auto_repair = false;
            m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
            const std::string message = error.what();
            wxWeakRef<ModelGenerationPanel> weak(this);
            discard_model_finishing_async([weak, hash, source, message](bool restored) {
                if (!weak || weak->m_shutdown) return;
                auto* self = weak.get();
                if (!restored) BOOST_LOG_TRIVIAL(error) << "Repair start rollback failed; draft retained";
                self->m_workbench_check_result = {};
                self->m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
                self->m_workbench_check_result.phase = WorkbenchCheckPhase::Idle;
                self->m_workbench_check_result.model_sha256 = hash;
                self->m_workbench_check_result.source_sha256 = hash;
                self->m_workbench_check_result.candidate_status = RepairCandidateStatus::Failed;
                self->m_workbench_check_result.summary = "无法启动修复版本保存，候选和原始外观载体已回滚。";
                self->m_workbench_check_result.repair_reason = self->m_workbench_check_result.summary + " " + message;
                self->m_workbench_check_path = source.string();
                self->m_workbench_check_revision = self->m_sequence;
                self->m_finishing_status->SetLabel(from_u8(self->m_workbench_check_result.repair_reason));
                self->m_workbench_operation_notice = into_u8(self->m_finishing_status->GetLabel());
                self->finish_printer_filament_application(false);
                self->refresh_controls();
            });
            return;
        } else {
            m_finishing_status->SetLabel(_L("暂时无法启动保存，候选保留：") + from_u8(error.what()));
        }
        m_workbench_operation_notice = into_u8(m_finishing_status->GetLabel());
        finish_printer_filament_application(false);
        refresh_controls();
    }
}

void ModelGenerationPanel::discard_model_finishing_async(std::function<void(bool)> complete)
{
    if (m_busy || m_beauty_restore_running || m_workbench_check_running || (m_finishing_candidate.empty() &&
        !beauty_session_has_changes())) { if (complete) complete(false); return; }
    const auto discarded = m_finishing_candidate;
    wxWeakRef<ModelGenerationPanel> weak(this);
    auto finished = [weak, complete = std::move(complete)](bool restored) {
        if (!weak || weak->m_shutdown) return;
        if (!restored) { if (complete) complete(false); return; }
        auto* self = weak.get();
        self->m_beauty_compare_draft.reset();
        self->m_finishing_candidate.clear();
        self->m_finishing_candidate_original_appearance = {};
        self->m_finishing_candidate_original_appearance_error.clear();
        if (self->m_workbench_check_result.candidate_status == RepairCandidateStatus::Ready) {
            self->m_workbench_check_result = {};
            self->ensure_workbench_check();
        }
        self->m_model_preview->set_selection_preview_suppressed(false);
        self->m_finishing_status->SetLabel(_L("已放弃预览，恢复处理前模型。"));
        self->m_status->SetLabel(self->m_finishing_status->GetLabel());
        self->m_model_preview_message->SetLabel(_L("当前显示：处理前模型。"));
        self->refresh_controls();
        self->update_finishing_selection();
        if (self->m_finishing_restore_selection)
            self->m_finishing_restore_selection();
        if (complete) complete(true);
    };
    if (m_beauty_session->source()) {
        m_beauty_session->discard_async(m_finishing_accepted_path, beauty_session_ports(), std::move(finished));
    } else {
        show_finishing_version_async(m_finishing_source, [weak, discarded, finished](bool restored) {
            if (!weak) return;
            if (!restored) { finished(false); return; }
            weak->clear_unaccepted_beauty_candidates();
            boost::system::error_code ignored;
            boost::filesystem::remove(discarded, ignored);
            finished(true);
        });
    }
}

void ModelGenerationPanel::undo_model_finishing()
{
    if (m_busy || m_beauty_restore_running || m_workbench_check_running || m_finishing_undo_path.empty()) return;
    const auto redo_colors = m_model_preview->color_trial_state();
    wxWeakRef<ModelGenerationPanel> weak(this);
    show_finishing_version_async(m_finishing_undo_path, [weak, redo_colors](bool restored) {
        if (!weak || weak->m_shutdown || !restored) return;
        auto* self = weak.get();
        self->m_finishing_redo_preview = [weak, redo_colors] { if (weak) weak->m_model_preview->restore_color_trial(redo_colors); };
        self->m_finishing_redo_path = self->m_finishing_accepted_path;
        self->m_finishing_redo_id = self->m_displayed_model_job_id;
        self->m_finishing_redo_source = self->m_finishing_undo_path;
        ++self->m_sequence;
        if (self->m_finishing_restore_context) self->m_finishing_restore_context();
        if (self->m_finishing_restore_selection) self->m_finishing_restore_selection();
        self->refresh_model_quality_card();
        self->m_finishing_undo_path.clear(); self->m_finishing_source.clear();
        self->m_finishing_status->SetLabel(_L("已返回上个版本。处理后的版本仍在模型库中，可随时重新选用。"));
        self->m_status->SetLabel(_L("已返回上个版本，可继续导入。"));
        self->m_model_preview_message->SetLabel(_L("当前显示：上个版本。"));
        self->refresh_controls();
        self->ensure_workbench_check();
    });
}

void ModelGenerationPanel::redo_model_finishing()
{
    if (m_busy || m_beauty_restore_running || m_workbench_check_running || m_finishing_redo_path.empty() || m_displayed_model_path != m_finishing_redo_source) return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    show_finishing_version_async(m_finishing_redo_path, [weak](bool restored) {
        if (!weak || weak->m_shutdown || !restored) return;
        auto* self = weak.get();
        self->m_finishing_undo_path = self->m_finishing_redo_source;
        self->m_finishing_accepted_path = self->m_finishing_redo_path;
        self->select_local_finishing_version(self->m_finishing_redo_path, self->m_finishing_redo_id);
        if (self->m_finishing_workbench) self->m_model_preview->set_selection_preview_suppressed(true);
        if (self->m_finishing_redo_preview) self->m_finishing_redo_preview();
        self->m_finishing_redo_path.clear();
        self->m_finishing_status->SetLabel(_L("已重做修整，恢复已保存版本。"));
        self->m_status->SetLabel(self->m_finishing_status->GetLabel());
        self->m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
        self->refresh_controls(); self->update_finishing_selection();
        self->ensure_workbench_check();
    });
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

        m_beauty_session->preserve_publication(m_finishing_candidate,
            m_finishing_candidate_original_appearance.valid()
                ? m_finishing_candidate_original_appearance.carrier_model : boost::filesystem::path {});

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
