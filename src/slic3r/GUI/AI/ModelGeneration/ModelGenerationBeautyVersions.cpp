#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
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
bool ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path)
{
    const auto view = m_model_preview->view_state();
    const auto color_state = m_model_preview->color_trial_state();
    size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero(); std::string error;
    const bool session_source = m_finishing_workbench && m_beauty_session_source &&
        path == m_beauty_session_source->source;
    const auto explicit_overrides = path == m_finishing_candidate ? m_finishing_candidate_face_overrides
        : session_source ? m_beauty_session_source->face_overrides : ModelPreview3D::FaceColorOverrides {};
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error, explicit_overrides)) {
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
    } else if (session_source && (!m_beauty_session_source->semantic_faces.empty() ||
                                  !m_beauty_session_source->semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(m_beauty_session_source->color_trial);
        m_model_preview->set_saved_semantic_result(
            m_beauty_session_source->semantic_faces, m_beauty_session_source->semantic_subfaces);
    } else if (m_finishing_workbench || m_model_preview->semantic_result_active())
        m_model_preview->restore_color_trial_without_recognition(color_state);
    else m_model_preview->restore_color_trial(color_state);
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
    ++m_sequence;
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

void ModelGenerationPanel::accept_model_finishing()
{
    if (m_busy || m_finishing_running || m_finishing_candidate.empty()) return;
    if (m_finishing_workbench && m_beauty_transactions &&
        !m_beauty_transactions->begin(BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate)) {
        m_finishing_status->SetLabel(_L("当前仍有 Beauty 处理正在进行，请先完成或取消。"));
        return;
    }
    if (!show_finishing_version(m_finishing_candidate)) {
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "candidate load failed");
        m_save_and_return = false;
        return;
    }
    const auto root = generated_models_root();
    const auto before = m_beauty_session_source;
    const auto source_context = m_finishing_source_context;
    auto after = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    const auto source = m_finishing_source;
    const auto candidate = m_finishing_candidate;
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
            {"max_displacement_source_units", m_finishing_result.max_displacement}}}
    };
    if (m_finishing_options.recolor_selected) {
        metadata["recolor_target_palette"] = m_finishing_color_palette;
        metadata["preserves_unselected_face_colors"] = true;
    }
    metadata["face_color_intent"] = m_model_preview->face_color_metadata();
    const auto leaf_edits=m_model_preview->leaf_edit_metadata();
    if (!leaf_edits.is_null() && !leaf_edits.empty()) metadata["beauty_leaf_edit"]=leaf_edits;
    metadata["color_trial"] = m_model_preview->color_trial_metadata();
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
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    m_beauty_publication_committed = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    const auto committed = m_beauty_publication_committed;
    const auto cache_root = boost::filesystem::path(Slic3r::data_dir()) / "cache";
    const auto revision = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_finishing_running = m_busy = true;
    m_finishing_status->SetLabel(_L("正在保存版本和编辑记录，可取消……"));
    refresh_controls();
    try {
        m_finishing_worker = std::thread([weak, revision, canceled, committed, metadata_path,
            source, candidate, before, after, source_context, cache_root, region_cache_unsaved,
            metadata = std::move(metadata), history_index = std::move(history_index)]() mutable {
            std::string error;
            try {
                if (after->shape_details && after->shape_details->compatible(after->geometry_id, after->result.faces_after)) {
                    auto shapes = PortraitShapeCache::save(*after->shape_details, cache_root);
                    shapes["model_sha256"] = after->model_sha256;
                    metadata["portrait_shape_reference"] = std::move(shapes);
                    metadata["portrait_shapes_unlocked"] = after->shapes_unlocked;
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
                const auto encoded = document.dump();
                AI::publish_beauty_version_record(metadata_path, candidate, source, metadata,
                    &encoded, [canceled] { return canceled->load(); }, &history_index);
                committed->store(true);
            } catch (const std::exception& e) { error = e.what(); }
            wxGetApp().CallAfter([weak, revision, canceled, committed, candidate, before, after,
                source_context, region_cache_unsaved, error] {
                if (!weak || weak->m_shutdown || revision != weak->m_sequence ||
                    weak->m_beauty_publication_committed != committed) return;
                auto* self = weak.get();
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = self->m_busy = false;
                if (!committed->load()) {
                    self->m_save_and_return = false;
                    if (self->m_beauty_transactions)
                        self->m_beauty_transactions->finish(false, false, error);
                    self->m_finishing_status->SetLabel(canceled->load()
                        ? _L("已取消保存，候选和草稿保留，可再次保存。")
                        : _L("版本保存失败，候选和草稿保留：") + from_u8(error));
                    self->refresh_controls();
                    return;
                }
                self->m_finishing_undo_path = self->m_finishing_source;
                self->m_finishing_restore_context = source_context;
                self->m_finishing_accepted_path = candidate;
                self->m_beauty_accepted_files.push_back(candidate);
                self->select_local_finishing_version(candidate, self->m_finishing_id);
                const auto accepted_context = self->capture_model_context();
                self->m_finishing_candidate.clear();
                after->source = candidate;
                after->candidate.clear();
                after->options = {};
                if (self->m_finishing_workbench) {
                    self->m_model_preview->set_selection_preview_suppressed(true);
                    self->m_finishing_source.clear();
                    self->m_finishing_undo_path.clear();
                    self->m_finishing_redo_path.clear();
                    self->m_finishing_source_context = {};
                    self->m_finishing_restore_context = {};
                    self->m_finishing_restore_selection = {};
                    self->m_beauty_session_source.reset();
                }
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
                            if (self->m_beauty_controls) self->m_beauty_controls->mark_saved();
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
                self->m_workbench_check_result = {};
                self->m_workbench_history_filter = self->m_workbench_history_page = 0;
                if (self->m_workbench_history_search) self->m_workbench_history_search->ChangeValue(wxEmptyString);
                self->load_library_entries(self->m_finishing_workbench);
                self->refresh_controls();
                self->update_finishing_selection();
                self->finish_workbench_save();
            });
        });
    } catch (const std::exception& error) {
        m_finishing_running = m_busy = m_save_and_return = false;
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, error.what());
        m_finishing_status->SetLabel(_L("暂时无法启动保存，候选保留：") + from_u8(error.what()));
        refresh_controls();
    }
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
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
    m_finishing_candidate.clear();
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
    if (m_busy || m_finishing_undo_path.empty()) return;
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
}

void ModelGenerationPanel::redo_model_finishing()
{
    if (m_busy || m_finishing_redo_path.empty() || m_displayed_model_path != m_finishing_redo_source) return;
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
}

void ModelGenerationPanel::stop_model_finishing()
{
    if (m_finishing_canceled) m_finishing_canceled->store(true);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_running = false;
    // Publication may complete while the close request waits for the worker.
    if (m_beauty_publication_committed && m_beauty_publication_committed->load() && !m_finishing_candidate.empty()) {
        m_finishing_accepted_path = m_finishing_candidate;
        m_beauty_accepted_files.push_back(m_finishing_candidate);
    }
    clear_unaccepted_beauty_candidates();
    if (!m_finishing_candidate.empty() && m_finishing_candidate != m_finishing_accepted_path) {
        boost::system::error_code ignored; boost::filesystem::remove(m_finishing_candidate, ignored);
    }
    m_finishing_candidate.clear();
}
} // namespace Slic3r::GUI
