#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "WorkbenchModelInspection.hpp"
#include "ModelGenerationPresentation.hpp"
#include "PortraitWorkbenchEntry.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <ctime>

namespace Slic3r::GUI {
namespace PP = PortraitPrint;
namespace {
boost::filesystem::path flow_root() { return ModelGenerationPresentation::generated_models_root() / "projects" / "portrait-print"; }
std::string u8(const boost::filesystem::path& path) { return wxString(path.wstring()).ToStdString(wxConvUTF8); }
boost::filesystem::path path8(const std::string& path) { return boost::filesystem::path(wxString::FromUTF8(path).ToStdWstring()); }
boost::filesystem::path saved_repair_model(const PP::State& flow)
{
    auto candidate = path8(flow.model_path);
    const auto root = ModelGenerationPresentation::generated_models_root();
    // Existing color publications already retain their source-model chain.
    // Follow that verified chain rather than repairing or generating again.
    for (size_t depth = 0; depth < 64; ++depth) {
        const auto hash = AI::model_artifact_sha256(candidate);
        if (hash.empty()) break;
        if (hash == flow.repair_result_hash) return candidate;
        const auto metadata = AI::beauty_metadata_path(candidate, root);
        if (!boost::filesystem::is_regular_file(metadata) ||
            boost::filesystem::file_size(metadata) > 128ULL * 1024 * 1024) break;
        boost::filesystem::ifstream input(metadata);
        const auto doc = nlohmann::json::parse(input);
        boost::filesystem::path parent;
        if (!doc.contains("source_model") || !doc.contains("source_sha256") ||
            !doc["source_model"].is_string() || !doc["source_sha256"].is_string() ||
            !original_appearance_detail::safe_library_file(root, path8(doc["source_model"].get<std::string>()), parent) ||
            AI::model_artifact_sha256(parent) != doc["source_sha256"].get<std::string>()) break;
        candidate = parent;
    }
    return {};
}
std::string portrait_project_revision(Plater* plater) {
    if (!plater) return {};
    const auto revision = OrcaSmartSlicingAdapter(plater).current_revision();
    if (!revision.valid()) return {};
    return std::to_string(revision.model_revision) + ":" + std::to_string(revision.config_revision) + ":" +
        std::to_string(revision.plate_revision) + ":" + revision.fingerprint;
}
void atomic_json(const boost::filesystem::path& path, const nlohmann::json& doc) {
    boost::filesystem::create_directories(path.parent_path());
    const auto temp = path.parent_path() / boost::filesystem::unique_path("flow-%%%%-%%%%.tmp");
    try {
        boost::filesystem::ofstream stream(temp, std::ios::binary);
        stream << doc.dump(); stream.close();
        if (!stream) throw std::runtime_error("流程记录无法写入");
        if (auto error = Slic3r::rename_file(u8(temp), u8(path))) throw std::runtime_error(error.message());
    } catch (...) { boost::system::error_code ignored; boost::filesystem::remove(temp, ignored); throw; }
}
struct RunningGuard { bool& value; explicit RunningGuard(bool& v) : value(v) { value = true; } ~RunningGuard() { value = false; } };
}

bool ModelGenerationPanel::save_portrait_print()
{
    if (m_portrait_print.id.empty()) return false;
    try {
        atomic_json(flow_root() / (m_portrait_print.id + ".json"), PP::encode(m_portrait_print));
        atomic_json(flow_root() / "current.json", PP::encode(m_portrait_print));
        return true;
    } catch (const std::exception&) {
        m_portrait_print.pause("无法保存一键流程记录，已停止自动推进；现有模型和任务保留。", true);
        if (m_status) m_status->SetLabel(wxString::FromUTF8(m_portrait_print.reason));
        publish_ui_state();
        return false;
    }
}

void ModelGenerationPanel::restore_portrait_print()
{
    try {
        const auto path = flow_root() / "current.json";
        if (!boost::filesystem::is_regular_file(path) || boost::filesystem::file_size(path) > 1024 * 1024) return;
        boost::filesystem::ifstream input(path, std::ios::binary);
        PP::State restored;
        if (PP::decode(nlohmann::json::parse(input), restored)) {
            m_portrait_print = std::move(restored);
            if (m_portrait_print.from_workbench() && m_portrait_print.active()) {
                wxWeakRef<ModelGenerationPanel> weak(this);
                CallAfter([weak] {
                    if (weak && !weak->m_shutdown && weak->m_portrait_print.restored) {
                        weak->restore_portrait_model();
                        weak->publish_ui_state();
                    }
                });
            }
        }
    } catch (const std::exception&) { /* Invalid records never authorize a request. */ }
}

void ModelGenerationPanel::pause_portrait_print(const std::string& reason, bool hard)
{
    if (!m_portrait_print.active()) return;
    m_portrait_print.pause(reason, hard);
    save_portrait_print();
    m_workbench_operation_notice = reason;
    m_workbench_printer_filament_notice = reason;
    m_workbench_printer_filament_notice_error = true;
    if (m_status) m_status->SetLabel(wxString::FromUTF8(reason));
    refresh_post_generation_workbench();
    publish_ui_state();
}

// Local workbench processing has no generation inputs and never calls a provider.
nlohmann::json ModelGenerationPanel::portrait_inputs_snapshot() const
{
    return {{"geometry_id", m_model_preview ? m_model_preview->geometry_id() : std::string{}},
        {"appearance_id", m_model_preview ? m_model_preview->semantic_source_content_id() : std::string{}},
        {"model_path", u8(m_displayed_model_path)}};
}

bool ModelGenerationPanel::validate_portrait_inputs()
{
    if (!m_portrait_print.from_workbench()) return false;
    const auto stage = m_portrait_print.stage;
    const bool repairing = stage == PP::Stage::Check && m_workbench_auto_repair &&
        !m_finishing_candidate.empty() && m_finishing_result.source_sha256 == m_portrait_print.model_hash &&
        m_model_preview && m_model_preview->showing_model(m_finishing_candidate);
    const bool exporting = stage == PP::Stage::SaveColor && m_portrait_export_started &&
        !m_finishing_candidate.empty() && m_finishing_result.source_sha256 == m_portrait_print.model_hash &&
        m_model_preview && m_model_preview->showing_model(m_finishing_candidate);
    if (!repairing && !exporting && PP::model_processing(stage) &&
        (!m_model_preview || m_displayed_model_path != path8(m_portrait_print.model_path) ||
         !m_model_preview->showing_model(m_displayed_model_path) ||
         m_model_preview->geometry_id() != m_portrait_print.geometry_id ||
         m_model_preview->semantic_source_content_id() != m_portrait_print.appearance_id ||
         AI::model_artifact_sha256(m_displayed_model_path) != m_portrait_print.model_hash)) {
        pause_portrait_print("工作台模型或几何身份已变化，已保留当前模型并暂停。"); return false;
    }
    return true;
}

PP::Submission ModelGenerationPanel::request_portrait_print_from_model(std::function<bool()> create_empty_project)
{
    if (m_portrait_advancing || m_portrait_print.running()) return PP::Submission::Pending;
    if (!m_page_initialized || m_busy || m_finishing_running || !m_model_preview_ready || !m_model_preview ||
        current_style() != "realistic" || !create_empty_project ||
        !m_model_preview->showing_model(m_displayed_model_path) || !m_finishing_candidate.empty())
        return PP::Submission::Rejected;
    const auto model_hash = AI::model_artifact_sha256(m_displayed_model_path);
    if (PP::workbench_entry_is_duplicate(m_portrait_print, u8(m_displayed_model_path), model_hash))
        return PP::Submission::Pending; // Explicit Continue owns a paused run.
    const auto palette = m_model_preview->color_trial_state();
    if (!PP::palette_allows_automatic_workbench(palette.source, palette.suite_id,
            !palette.color_card_target_palette.empty()))
        return PP::Submission::Rejected; // Other suites/cards keep their manual workflow and project.
    RunningGuard guard(m_portrait_advancing);
    PP::State flow;
    flow.entry = "completed_model";
    flow.id = ModelGenerationPresentation::new_request_id(); flow.stage = PP::Stage::Project;
    flow.original_model_path = flow.model_path = u8(m_displayed_model_path);
    flow.original_model_hash = flow.model_hash = model_hash;
    flow.geometry_id = m_model_preview->geometry_id();
    flow.appearance_id = m_model_preview->semantic_source_content_id();
    flow.edit_revision = m_model_preview->leaf_edit_revision();
    // The model's final generation palette wins over the currently visible
    // generation page. Finishing versions retain this value in their record.
    flow.final_count = original_appearance_detail::ai_source_color_count(
        m_displayed_model_path, ModelGenerationPresentation::generated_models_root());
    if (flow.final_count) flow.requested_count = flow.final_count;
    if (!flow.final_count) {
        const auto basis = m_model_preview->automatic_color_basis();
        if (basis.valid() && basis.geometry_id == flow.geometry_id && basis.requested_count >= 4 && basis.requested_count <= 6)
            flow.final_count = flow.requested_count = int(basis.requested_count);
    }
    flow.frozen = portrait_inputs_snapshot();
    flow.suite_revision = honor_people_suite().revision;
    if (flow.model_hash.empty() || flow.geometry_id.empty() || flow.appearance_id.empty() ||
        !m_model_preview->original_comparison_available()) return PP::Submission::Rejected;
    flow.authorized = true; // Local processing only; no generation authorization.
    flow.authorization_id = flow.id;
    m_portrait_print = std::move(flow);
    m_portrait_verified_check = {};
    if (!save_portrait_print()) return PP::Submission::Rejected;
    auto* plater = wxGetApp().plater();
    try {
        if (!plater) throw std::runtime_error("工程宿主不可用");
        if (!plater->model().objects.empty() || plater->is_project_dirty()) {
            const auto backup = flow_root() / (m_portrait_print.id + "-recovery.3mf");
            if (!plater->export_3mf_copy(backup) || !boost::filesystem::is_regular_file(backup) ||
                boost::filesystem::file_size(backup) == 0) throw std::runtime_error("工程恢复副本保存失败");
            m_portrait_print.recovery_project = u8(backup);
            if (!save_portrait_print()) return PP::Submission::Rejected;
        }
    } catch (const std::exception& e) {
        pause_portrait_print("备份失败，原工程已保留：" + std::string(e.what()));
        return PP::Submission::Rejected;
    }
    if (!create_empty_project()) {
        pause_portrait_print("空工程未创建，当前工程与模型保留。"); return PP::Submission::Cancelled;
    }
    m_portrait_print.workspace_id = std::to_string(plater->model().id().id);
    m_portrait_semantics_requested = m_portrait_export_started = false;
    m_portrait_color_snapshot = {};
    if (!advance_portrait_print(m_portrait_print.id, PP::Stage::Project, PP::Stage::Check)) return PP::Submission::Rejected;
    return PP::Submission::Pending;
}

bool ModelGenerationPanel::resume_portrait_print()
{
    if (!m_portrait_print.active() || m_portrait_print.failed || m_portrait_advancing ||
        !m_portrait_print.paused || m_busy || m_finishing_running) return false;
    if (!m_portrait_print.from_workbench()) {
        m_workbench_operation_notice = "旧流程与成果可查看；请打开已完成模型并点击“打开工作台”，不会恢复前段自动生成。";
        publish_workbench_state(); return false;
    }
    RunningGuard guard(m_portrait_advancing);
    auto* plater = wxGetApp().plater();
    if (!plater) return false;
    if (m_portrait_print.stage == PP::Stage::Project) {
        // Backup may have failed. Retry the same local intent, never a provider.
        try {
            if (!plater->model().objects.empty() || plater->is_project_dirty()) {
                auto backup = flow_root() / boost::filesystem::unique_path(m_portrait_print.id + "-recovery-%%%%.3mf");
                if (!plater->export_3mf_copy(backup) || !boost::filesystem::is_regular_file(backup) ||
                    boost::filesystem::file_size(backup) == 0) throw std::runtime_error("工程恢复副本保存失败");
                m_portrait_print.recovery_project = u8(backup);
                if (!save_portrait_print()) return false;
            }
            if (plater->new_project(true, true, {}, false) == wxID_CANCEL) return false;
        } catch (const std::exception& e) { pause_portrait_print(e.what()); return false; }
        m_portrait_print.stage = PP::Stage::Check;
    }
    if (m_portrait_print.record_version < 4) {
        // Keep every historical asset, but old "repaired at import" evidence
        // cannot certify the new repair-before-partition contract.
        m_portrait_print.model_path = m_portrait_print.original_model_path;
        m_portrait_print.model_hash = m_portrait_print.original_model_hash;
        m_portrait_print.repair_checked = m_portrait_print.repair_saved = false;
        m_portrait_print.partition_completed = m_portrait_print.colored_saved = false;
        m_portrait_print.native_repair_required = true;
        m_portrait_print.basis_signature.clear();m_portrait_print.region_binding_digest.clear();
        m_portrait_print.boundary_algorithm_version = 0;
        m_portrait_print.final_palette.clear();
        m_portrait_print.final_count = original_appearance_detail::ai_source_color_count(
            path8(m_portrait_print.original_model_path), ModelGenerationPresentation::generated_models_root());
        if (m_portrait_print.final_count) m_portrait_print.requested_count = m_portrait_print.final_count;
        m_portrait_verified_check = {};
        m_portrait_print.record_version = 4;
        m_portrait_print.stage = PP::Stage::Check;
        // Keep the saved source identities while restoring the original model.
        // A load failure must still leave a readable, paused recovery record.
    }
    if (m_portrait_print.partition_needs_rebuild(PortraitColorBlocks::portrait_postprocess_algorithm_version)) {
        if (!plater->model().objects.empty()) {
            pause_portrait_print("原自动分区已失效，请先保存当前工程并创建空工程，再继续已保存修复成果。"); return false;
        }
        try {
            const auto repaired = saved_repair_model(m_portrait_print);
            if (repaired.empty() || !m_portrait_print.rebuild_partition(
                    PortraitColorBlocks::portrait_postprocess_algorithm_version, u8(repaired),
                    AI::model_artifact_sha256(repaired))) {
                pause_portrait_print("未能核验已保存修复模型，旧配色成果保留，请打开修复版本后继续。"); return false;
            }
            m_portrait_semantics_requested = m_portrait_export_started = false;
            m_portrait_color_snapshot = {};
            m_portrait_verified_check = {};
            if (!save_portrait_print()) return false;
        } catch (const std::exception& error) {
            pause_portrait_print("修复成果恢复失败：" + std::string(error.what())); return false;
        }
    }
    if (m_portrait_print.restored) {
        if (!plater->model().objects.empty()) {
            pause_portrait_print("请先保存当前工程并创建空工程，再手动继续已保存人像。"); return false;
        }
        // Recover a publication committed just before a crash without recoloring.
        const auto asset = m_portrait_print.color_asset_id();
        const auto metadata_path = ModelGenerationPresentation::library_metadata_path(asset);
        if (m_portrait_print.repair_checked && !m_portrait_print.colored_saved && boost::filesystem::is_regular_file(metadata_path)) {
            try {
                boost::filesystem::ifstream in(metadata_path);
                const auto doc = nlohmann::json::parse(in);
                boost::filesystem::path candidate;
                if (!original_appearance_detail::safe_library_file(ModelGenerationPresentation::generated_models_root(),
                    boost::filesystem::path(doc.at("model_path").get<std::string>()), candidate))
                    throw std::runtime_error("配色成果路径无效");
                if (doc.at("portrait_print_id") != m_portrait_print.id ||
                    doc.at("source_sha256") != m_portrait_print.repair_result_hash ||
                    AI::model_artifact_sha256(candidate) != doc.at("model_sha256").get<std::string>()) return false;
                m_portrait_print.model_path = u8(candidate); m_portrait_print.model_hash = doc.at("model_sha256");
                m_portrait_print.colored_saved = true;
            } catch (const std::exception&) { pause_portrait_print("已保存配色版本身份不匹配，未重复配色。"); return false; }
        }
        m_portrait_print.stage = m_portrait_print.colored_saved ? PP::Stage::Base : PP::Stage::Check;
        m_portrait_print.slice_started = false; m_portrait_print.native_repair_completed = false;
        m_portrait_print.project_revision.clear(); m_portrait_print.slice_revision.clear(); m_portrait_print.gcode_path.clear();
    }
    if ((PP::before_import(m_portrait_print.stage) || m_portrait_print.stage == PP::Stage::Import) && !plater->model().objects.empty()) {
        if (m_portrait_print.stage != PP::Stage::Import || !m_portrait_print.native_repair_completed ||
            plater->model().objects.size() != 1 || portrait_project_revision(plater) != m_portrait_print.project_revision) {
            pause_portrait_print("当前工程已变化，请先保存并创建空工程，再继续。"); return false;
        }
    } else if (plater->model().objects.empty()) m_portrait_print.workspace_id = std::to_string(plater->model().id().id);
    if (!restore_portrait_model()) return false;
    m_portrait_print.authorized = true; m_portrait_print.paused = false; m_portrait_print.reason.clear(); m_portrait_print.restored = false;
    m_portrait_semantics_requested = m_portrait_export_started = false;
    if (!save_portrait_print()) return false;
    queue_portrait_print(); publish_ui_state(); return true;
}

bool ModelGenerationPanel::restore_portrait_model()
{
    const auto path = path8(m_portrait_print.model_path);
    if (AI::model_artifact_sha256(path) != m_portrait_print.model_hash) {
        pause_portrait_print("保存的模型身份已变化，成果保留，未继续。", true); return false;
    }
    if (!m_model_preview->showing_model(path) || m_displayed_model_path != path) {
        size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error;
        ModelPreview3D::PreparedModel prepared;
        if (!ModelPreview3D::prepare_model(path, prepared, error, {},
                AI::beauty_metadata_path(path, ModelGenerationPresentation::generated_models_root())) ||
            !m_model_preview->load_prepared_model(std::move(prepared), {}, triangles, dimensions, colors, error)) {
            pause_portrait_print("保存的模型无法恢复：" + error); return false;
        }
        reset_beauty_asset();
        select_local_finishing_version(path, m_portrait_print.color_asset_id());
        m_model_preview_ready = true;
    }
    if (m_portrait_print.geometry_id.empty() && m_portrait_print.appearance_id.empty()) {
        m_portrait_print.geometry_id = m_model_preview->geometry_id();
        m_portrait_print.appearance_id = m_model_preview->semantic_source_content_id();
        m_portrait_print.edit_revision = m_model_preview->leaf_edit_revision();
    }
    if (m_model_preview->geometry_id() != m_portrait_print.geometry_id ||
        m_model_preview->semantic_source_content_id() != m_portrait_print.appearance_id) {
        pause_portrait_print("保存的面域身份无法恢复，未继续。", true); return false;
    }
    if (m_portrait_print.colored_saved) {
        const auto state = m_model_preview->color_trial_state();
        if (state.suite_id != "honor.people.six" || state.suite_revision != m_portrait_print.suite_revision ||
            !state.base_binding || !state.base_binding->valid() ||
            (m_portrait_print.semantic_applied && !m_model_preview->honor_suite_v2_appearance_active())) {
            pause_portrait_print("保存的人物区域或底座绑定无法恢复，未导入。"); return false;
        }
        std::string error;
        if (!m_model_preview->set_round_base_preview(true, error)) { pause_portrait_print(error); return false; }
        m_portrait_color_snapshot = m_model_preview->color_trial_metadata();
        m_portrait_color_edit_revision = m_model_preview->leaf_edit_revision();
    }
    return true;
}

bool ModelGenerationPanel::record_portrait_import(const std::string& id, const AI::ModelImportResult& result)
{
    if (!m_portrait_print.current(id, PP::Stage::Import) || !result.imported() ||
        !m_portrait_print.repair_checked || m_portrait_print.native_repair_required ||
        !m_portrait_print.colored_saved || m_portrait_print.repair_result_hash.empty()) return false;
    m_portrait_print.native_repair_completed = true;
    m_portrait_print.native_geometry_id = result.native_geometry_id.empty() ? m_portrait_print.geometry_id : result.native_geometry_id;
    m_portrait_print.project_revision = portrait_project_revision(wxGetApp().plater());
    if (m_portrait_print.project_revision.empty()) return false;
    if (!save_portrait_print()) return false;
    // The shell advances from the feature host's published snapshot. Publish
    // the committed repair before releasing the import/view transition.
    publish_ui_state();
    return true;
}

void ModelGenerationPanel::queue_portrait_print()
{
    if (m_shutdown || !m_portrait_print.running() || m_portrait_advance_queued) return;
    m_portrait_advance_queued = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    CallAfter([weak] {
        if (!weak) return;
        weak->m_portrait_advance_queued = false;
        if (!weak->m_shutdown) weak->run_portrait_print();
    });
}

bool ModelGenerationPanel::advance_portrait_print(const std::string& id, PP::Stage expected,
    PP::Stage next, const std::string& revision, const std::string& gcode)
{
    if (!validate_portrait_inputs()) return false;
    bool advanced = false;
    if (expected == PP::Stage::Slice && next == PP::Stage::PrintReady)
        advanced = m_portrait_print.slice_complete(id, revision, true, true, gcode);
    else if (expected == PP::Stage::Slice && next == expected)
        advanced = m_portrait_print.bind_slice_input(id, revision);
    else advanced = m_portrait_print.advance(id, expected, next);
    if (!advanced) return false;
    if (next == PP::Stage::Recommend) m_portrait_print.project_revision = revision;
    if (next == PP::Stage::Slice) m_portrait_print.slice_revision = revision;
    if (next == PP::Stage::PrintReady) m_portrait_print.preview_result_id = revision + ":" + gcode;
    if (!save_portrait_print()) return false;
    publish_ui_state(); queue_portrait_print(); return true;
}

void ModelGenerationPanel::add_portrait_notice(const std::string& id, const std::string& notice)
{
    if (m_portrait_print.id != id || !m_portrait_print.active() || notice.empty()) return;
    if (std::find(m_portrait_print.notices.begin(), m_portrait_print.notices.end(), notice) != m_portrait_print.notices.end()) return;
    m_portrait_print.notices.push_back(notice);
    save_portrait_print(); publish_ui_state();
}

// v1/v2 job callbacks may still arrive; they never advance the workbench flow.
bool ModelGenerationPanel::observe_portrait_status(const AIModelGenerationClient::JobStatus&, uint64_t) { return true; }
void ModelGenerationPanel::portrait_repair_published(const std::string& source, const std::string& hash,
    const boost::filesystem::path& candidate)
{
    if (!m_portrait_print.from_workbench() || !m_model_preview ||
        m_displayed_model_path != candidate || !m_model_preview->showing_model(candidate) ||
        AI::model_artifact_sha256(candidate) != hash ||
        !m_portrait_print.accept_repair(source, hash, true, workbench_topology_safe(m_workbench_check_result))) return;
    m_portrait_print.model_path = u8(candidate);
    m_portrait_print.geometry_id = m_model_preview->geometry_id();
    m_portrait_print.appearance_id = m_model_preview->semantic_source_content_id();
    m_portrait_print.edit_revision = m_model_preview->leaf_edit_revision();
    m_portrait_verified_check = m_workbench_check_result;
    save_portrait_print(); queue_portrait_print();
}

void ModelGenerationPanel::run_portrait_print()
{
    if (!m_portrait_print.running() || !m_portrait_print.from_workbench() || m_portrait_advancing || !m_page_initialized) return;
    RunningGuard guard(m_portrait_advancing);
    if (!validate_portrait_inputs()) return;
    const auto id = m_portrait_print.id;
    const auto stage = m_portrait_print.stage;
    if (PP::protects_saved_color(stage) && !m_portrait_color_snapshot.empty() &&
        (m_model_preview->color_trial_metadata() != m_portrait_color_snapshot ||
         m_model_preview->leaf_edit_revision() != m_portrait_color_edit_revision)) {
        pause_portrait_print("人物配色、底座绑定或局部编辑已变化，当前编辑保留，请保存后重新核对。"); return;
    }
    if (PP::before_import(stage)) {
        auto* plater = wxGetApp().plater();
        if (!plater || !plater->model().objects.empty() || std::to_string(plater->model().id().id) != m_portrait_print.workspace_id) {
            pause_portrait_print("空工程已变化，本次自动流程暂停。"); return;
        }
    }
    if (m_busy || m_preview_loading || m_preview_download_in_flight || m_finishing_running || m_workbench_check_running) return;
    if (stage == PP::Stage::Check) {
        if (!m_finishing_workbench || !m_workbench_presented) return;
        m_workbench_operation_notice = "正在自动检查并修复模型，保存修复成果后建立 AI 颜色分区。";
        if (m_workbench_check_path != m_displayed_model_path.string() ||
            m_workbench_check_revision != m_sequence || m_workbench_check_result.status == WorkbenchCheckStatus::NotRun) {
            if (!request_check_workbench(true)) pause_portrait_print("模型自动检查未能启动，当前模型保留。");
            return;
        }
        if (!workbench_topology_safe(m_workbench_check_result) ||
            m_workbench_check_result.candidate_status == RepairCandidateStatus::Failed || !m_finishing_candidate.empty()) {
            pause_portrait_print(m_workbench_check_result.repair_reason.empty() ?
                "模型修复尚未安全保存，请处理工作台提示后继续。" : m_workbench_check_result.repair_reason); return;
        }
        if (m_portrait_print.repair_checked) {
            if (m_workbench_check_result.model_sha256 != m_portrait_print.repair_result_hash) {
                pause_portrait_print("修复成果与复检身份不匹配，未重新分区。"); return;
            }
            m_portrait_verified_check = m_workbench_check_result;
            advance_portrait_print(id, stage, PP::Stage::Partition); return;
        }
        if (m_portrait_print.accept_repair(m_portrait_print.model_hash, m_portrait_print.model_hash, true, true)) {
            m_portrait_verified_check = m_workbench_check_result;
            advance_portrait_print(id, stage, PP::Stage::Partition);
        }
        return;
    }
    if (stage == PP::Stage::Partition) {
        if (m_portrait_print.final_count < 4 || m_portrait_print.final_count > 6) {
            const auto basis = m_model_preview->automatic_color_basis();
            if (!basis.valid() || basis.requested_count < 4 || basis.requested_count > 6) {
                pause_portrait_print("模型历史缺少 AI 建议色数，请在工作台选择 4/5/6 色后继续。"); return;
            }
            m_portrait_print.final_count = int(basis.requested_count);
        }
        if (!m_model_preview->activate_current_ai_palette(m_portrait_print.final_count)) {
            pause_portrait_print("无法建立修复模型的 AI 源色分区，原始外观保留。"); return;
        }
        const auto basis = m_model_preview->automatic_color_basis();
        if (!basis.valid() || basis.geometry_id != m_portrait_print.geometry_id) {
            pause_portrait_print("AI 源色基准与修复后模型不匹配。"); return;
        }
        m_portrait_print.basis_signature = basis.basis_signature;
        m_portrait_print.boundary_algorithm_version = PortraitColorBlocks::portrait_postprocess_algorithm_version;
        m_portrait_print.partition_completed = true;
        m_portrait_print.edit_revision = m_model_preview->leaf_edit_revision();
        m_workbench_operation_notice = "AI 建议色基准已就绪，正在按原有连通区域绑定人物六色槽位。";
        advance_portrait_print(id, stage, PP::Stage::Color); return;
    }
    if (stage == PP::Stage::Color) {
        if (m_portrait_semantics_requested) return;
        if (m_model_preview->leaf_edit_revision() != m_portrait_print.edit_revision) {
            pause_portrait_print("模型编辑修订已变化，当前编辑保留，未启动配色。"); return;
        }
        m_portrait_semantics_requested = true;
        if (!m_model_preview->request_honor_people_suite()) pause_portrait_print("人物六色区域配色未能启动，请检查模型原始外观。");
        return; // The same suite completion callback serves manual and automatic application.
    }
    if (stage == PP::Stage::SaveColor) { save_portrait_colored_model(); return; }
    if (stage == PP::Stage::Base) {
        if (!m_portrait_print.colored_saved) { pause_portrait_print("人物配色成果尚未保存，未导入耗材。"); return; }
        // On restart, inspect the saved carrier asynchronously. This is a
        // read-only check, never another topology repair of painted geometry.
        if (m_workbench_check_path != m_displayed_model_path.string() || m_workbench_check_revision != m_sequence ||
            m_workbench_check_result.status == WorkbenchCheckStatus::NotRun) {
            if (!request_check_workbench(true)) pause_portrait_print("已保存配色模型的身份检查未能启动。");
            return;
        }
        if (!workbench_topology_safe(m_workbench_check_result)) {
            pause_portrait_print("已保存配色模型未通过检查：" + m_workbench_check_result.summary); return;
        }
        std::string error;
        if (!m_model_preview->set_round_base_preview(true, error)) { pause_portrait_print(error); return; }
        advance_portrait_print(id, stage, PP::Stage::Filaments); return;
    }
    if (stage == PP::Stage::Filaments) {
        auto before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        if (wxGetApp().preset_bundle) {
            before->project_config = std::make_shared<DynamicPrintConfig>(wxGetApp().preset_bundle->project_config);
            before->project_presets = wxGetApp().preset_bundle->filament_presets;
        }
        if (!apply_model_palette_to_filaments()) {
            pause_portrait_print("已配色成果已保存。请切换兼容六实体槽机型后点击“继续一键流程”。\n" + m_workbench_printer_filament_notice); return;
        }
        record_beauty_candidate(BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor, before);
        m_portrait_color_snapshot = m_model_preview->color_trial_metadata();
        m_portrait_color_edit_revision = m_model_preview->leaf_edit_revision();
        advance_portrait_print(id, stage, PP::Stage::Import);
    }
}

void ModelGenerationPanel::save_portrait_colored_model()
{
    if (!m_portrait_print.current(m_portrait_print.id, PP::Stage::SaveColor) || m_finishing_running || m_busy) return;
    if (!m_finishing_candidate.empty()) {
        if (!m_finishing_result.success) {
            pause_portrait_print("涂色候选写出失败，成果保留：" + m_finishing_result.error); return;
        }
        if (m_finishing_result.source_sha256 != m_portrait_print.model_hash) {
            pause_portrait_print("涂色候选的源模型身份不匹配，成果保留。"); return;
        }
        if (m_model_preview->printer_filament_export_faces().empty() &&
            m_model_preview->import_subface_color_overrides(true).empty()) {
            pause_portrait_print("涂色候选没有可校验的面或子面颜色，成果保留。"); return;
        }
        accept_model_finishing(); return;
    }
    if (m_portrait_export_started) { pause_portrait_print("涂色导出未完成，当前编辑保留，请查看工作台错误后继续。"); return; }
    m_portrait_export_started = true;
    export_semantic_candidate();
}

void ModelGenerationPanel::portrait_color_published(const boost::filesystem::path& candidate)
{
    const auto id = m_portrait_print.id;
    if (!m_portrait_print.current(id, PP::Stage::SaveColor)) return;
    if (m_displayed_model_path != candidate || !m_model_preview->showing_model(candidate) ||
        m_finishing_result.output_sha256.empty() || AI::model_artifact_sha256(candidate) != m_finishing_result.output_sha256 ||
        m_model_preview->geometry_id() != m_portrait_print.geometry_id ||
        (m_model_preview->printer_filament_export_faces().empty() && m_model_preview->import_subface_color_overrides(true).empty())) {
        pause_portrait_print("已保存版本的面域或配色身份核验失败，未继续导入。"); return;
    }
    m_portrait_print.model_path = u8(candidate); m_portrait_print.model_hash = m_finishing_result.output_sha256;
    try {
        boost::filesystem::ifstream input(AI::beauty_metadata_path(candidate, ModelGenerationPresentation::generated_models_root()));
        const auto metadata = nlohmann::json::parse(input);
        if (metadata.at("model_sha256") != m_portrait_print.model_hash ||
            metadata.at("portrait_print_id") != id)
            throw std::runtime_error("配色成果与流程身份不匹配");
        if (m_portrait_print.semantic_applied) {
            const auto& layer = metadata.at("portrait_applied_color_layer");
            const auto& reference = layer.at("reference");
            if (layer.at("schema") != "orca.portrait-applied-color-layer/v1" ||
                reference != metadata.at("portrait_color_blocks") ||
                reference.at("geometry_id") != m_portrait_print.geometry_id ||
                layer.at("painted_faces").get<size_t>() + layer.at("painted_subfaces").get<size_t>() == 0)
                throw std::runtime_error("实际面或子面颜色层未保存");
            m_portrait_print.region_binding_digest = reference.at("content_id").get<std::string>();
        } else m_portrait_print.region_binding_digest = AI::beauty_leaf_digest(
            m_portrait_print.model_hash + m_model_preview->color_trial_metadata().dump());
    } catch (const std::exception& e) {
        pause_portrait_print("配色保存核验失败，成果保留：" + std::string(e.what())); return;
    }
    m_portrait_print.colored_saved = true;
    const auto state = m_model_preview->color_trial_state();
    m_portrait_print.base_binding = state.base_binding ? state.base_binding->encode() : nlohmann::json();
    m_portrait_color_snapshot = m_model_preview->color_trial_metadata();
    m_portrait_color_edit_revision = m_model_preview->leaf_edit_revision();
    // A color-only publication inherits the verified repair. Never repair the
    // newly painted topology again at import.
    if (!workbench_topology_safe(m_portrait_verified_check) ||
        m_portrait_verified_check.model_sha256 != m_portrait_print.repair_result_hash) {
        pause_portrait_print("修复检查身份未保留，已保存配色成果保留，请手动继续复检。"); return;
    }
    m_workbench_check_result = m_portrait_verified_check;
    m_workbench_check_result.model_sha256 = m_portrait_print.model_hash;
    m_workbench_check_path = candidate.string(); m_workbench_check_revision = m_sequence;
    advance_portrait_print(id, PP::Stage::SaveColor, PP::Stage::Base);
}
} // namespace Slic3r::GUI
