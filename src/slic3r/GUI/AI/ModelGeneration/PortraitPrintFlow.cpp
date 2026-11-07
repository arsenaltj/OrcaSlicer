#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "WorkbenchModelInspection.hpp"
#include "ModelOriginalAppearance.hpp"
#include "ModelGenerationPresentation.hpp"
#include "PortraitNativePartition.hpp"
#include "PortraitNativePartitionCache.hpp"
#include "PortraitNativeCheckpoint.hpp"
#include "PortraitWorkbenchEntry.hpp"
#include "PortraitSaveLedger.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
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
#include <chrono>
#include <ctime>

namespace Slic3r::GUI {
namespace PP = PortraitPrint;
namespace {
boost::filesystem::path flow_root() { return ModelGenerationPresentation::generated_models_root() / "projects" / "portrait-print"; }
std::string u8(const boost::filesystem::path& path) { return wxString(path.wstring()).ToStdString(wxConvUTF8); }
boost::filesystem::path path8(const std::string& path) { return boost::filesystem::path(wxString::FromUTF8(path).ToStdWstring()); }
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

struct NativeWorkbenchActivationRollback {
    BeautyCandidateSnapshot beauty;
    boost::filesystem::path displayed_model_path;
    std::string displayed_model_job_id;
    nlohmann::json partition_reference;
    nlohmann::json partition_report;
    nlohmann::json partition_certificate;
    nlohmann::json partition_commit;
    WorkbenchCheckResult workbench_check;
    std::string workbench_check_path;
    std::uint64_t workbench_check_revision {0};
    PortraitPrint::State portrait_print;
    WorkbenchCheckResult portrait_verified_check;
};

const char* portrait_source_check_status(WorkbenchCheckStatus status)
{
    switch (status) {
    case WorkbenchCheckStatus::NotRun: return "not_run";
    case WorkbenchCheckStatus::Running: return "running";
    case WorkbenchCheckStatus::Normal: return "normal";
    case WorkbenchCheckStatus::Attention: return "attention";
    case WorkbenchCheckStatus::Invalid: return "invalid";
    case WorkbenchCheckStatus::Failed: return "failed";
    }
    return "unknown";
}

nlohmann::json portrait_source_check_report(const WorkbenchCheckResult& check)
{
    return {{"schema", "orca.portrait-source-check/v1"},
        {"status", portrait_source_check_status(check.status)},
        {"model_sha256", check.model_sha256}, {"summary", check.summary},
        {"faces", check.before_faces}, {"vertices", check.before_vertices},
        {"boundary_edges", check.boundary_edges}, {"boundary_loops", check.boundary_loops},
        {"boundary_loop_sizes", check.boundary_loop_sizes},
        {"nonmanifold_edges", check.nonmanifold_edges},
        {"nonmanifold_vertices", check.nonmanifold_vertices},
        {"degenerate_faces", check.degenerate_faces}, {"duplicate_faces", check.duplicate_faces},
        {"inconsistent_edges", check.inconsistent_edges},
        {"isolated_components", check.isolated_components}, {"small_faces", check.small_faces},
        {"self_intersections_checked", check.self_intersections_checked},
        {"self_intersections", check.self_intersections}, {"issue_faces", check.issue_faces}};
}

nlohmann::json portrait_topology_report(const StrictMeshTopologyReport& report)
{
    return {{"open_edges", report.open_edges}, {"nonmanifold_edges", report.nonmanifold_edges},
        {"nonmanifold_vertices", report.nonmanifold_vertices},
        {"degenerate_faces", report.degenerate_faces}, {"duplicate_faces", report.duplicate_faces},
        {"inconsistent_edges", report.inconsistent_edges},
        {"self_intersections_checked", report.self_intersections_checked},
        {"self_intersects", report.self_intersects}};
}

WorkbenchCheckResult portrait_partition_check_result(
    const NativePortrait::NativePortraitPartition& partition,
    const NativePortrait::NativePortraitPartitionBuildReport& build,
    const std::string& carrier_hash)
{
    WorkbenchCheckResult result;
    const auto& topology = build.strict_repair.output_topology;
    result.status = WorkbenchCheckStatus::Normal;
    result.source = WorkbenchCheckSource::Automatic;
    result.purpose = WorkbenchCheckPurpose::PortraitPostColorReadOnly;
    result.model_sha256 = carrier_hash;
    result.source_sha256 = carrier_hash;
    result.summary = "Orca 原生分区严格修复与完整拓扑复检已通过。";
    result.boundary_edges = topology.open_edges;
    result.nonmanifold_edges = topology.nonmanifold_edges;
    result.nonmanifold_vertices = topology.nonmanifold_vertices;
    result.degenerate_faces = topology.degenerate_faces;
    result.duplicate_faces = topology.duplicate_faces;
    result.inconsistent_edges = topology.inconsistent_edges;
    result.self_intersections_checked = topology.self_intersections_checked;
    result.self_intersections = topology.self_intersects ? 1 : 0;
    result.before_faces = partition.painted.indices.size();
    result.before_vertices = partition.painted.vertices.size();
    result.candidate_status = RepairCandidateStatus::None;
    result.appearance_mapped = true;
    result.appearance_confidence = 1.0;
    result.appearance_reason = "原始外观引用与分区载体分别保存。";
    return result;
}

uint64_t portrait_elapsed_ms(const std::chrono::steady_clock::time_point& started)
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}

void log_native_portrait_partition_report(
    const NativePortrait::NativePortraitPartitionBuildReport& report,
    const char* outcome)
{
    BOOST_LOG_TRIVIAL(info) << "Native portrait partition report [" << outcome << "]: "
        << NativePortrait::encode_native_portrait_partition_build_report(report).dump();
}

bool validate_native_partition_checkpoint(const PP::State& flow, nlohmann::json& document,
                                          std::string& error)
{
    error.clear();
    try {
        std::string metadata_name = flow.partition_pending ? flow.pending_partition_metadata_path :
            flow.native_portrait_partition.value("metadata_path", std::string {});
        if (metadata_name.empty()) throw std::runtime_error("原生分区提交标记缺失");
        const auto metadata_path = path8(metadata_name);
        boost::system::error_code filesystem_error;
        const auto bytes = boost::filesystem::file_size(metadata_path, filesystem_error);
        if (filesystem_error || bytes == 0 || bytes > 128ULL * 1024 * 1024 ||
            boost::filesystem::is_symlink(metadata_path, filesystem_error) || filesystem_error)
            throw std::runtime_error("原生分区提交元数据缺失或不可读");
        boost::filesystem::ifstream input(metadata_path, std::ios::binary);
        document = nlohmann::json::parse(input);
        PP::State recovered = flow;
        if (!recovered.adopt_partition_commit(document))
            throw std::runtime_error("原生分区提交元数据与流程身份不匹配");

        const auto& reference = document.at("native_portrait_partition");
        if (!PP::same_model_path(metadata_name, reference.at("metadata_path").get<std::string>()))
            throw std::runtime_error("原生分区提交路径不匹配");
        const auto verified_file = [&](const char* path_key, const char* hash_key) {
            const auto file = path8(reference.at(path_key).get<std::string>());
            boost::system::error_code file_error;
            return boost::filesystem::is_regular_file(file, file_error) && !file_error &&
                !boost::filesystem::is_symlink(file, file_error) && !file_error &&
                AI::model_artifact_sha256(file) == reference.at(hash_key).get<std::string>();
        };
        if (!verified_file("carrier_path", "carrier_sha256") ||
            !verified_file("appearance_path", "appearance_sha256") ||
            !verified_file("cache_path", "cache_sha256"))
            throw std::runtime_error("原生分区载体、外观或缓存已缺失或变化");

        const auto& certificate = document.at("native_portrait_topology_certificate");
        if (!certificate.is_object() ||
            AI::beauty_leaf_digest(certificate.dump()) != reference.at("topology_hash") ||
            certificate.at("geometry_id") != reference.at("geometry_id") ||
            certificate.at("carrier_sha256") != reference.at("carrier_sha256") ||
            certificate.at("partition_digest") != reference.at("partition_digest") ||
            certificate.at("coordinate_units") != reference.at("coordinate_units") ||
            certificate.at("source_bounds_mm") != reference.at("source_bounds_mm") ||
            certificate.at("carrier_bounds_mm") != reference.at("carrier_bounds_mm"))
            throw std::runtime_error("原生分区拓扑证书无效");
        const auto& report = document.at("partition_build_report");
        if (report.value("schema", std::string {}) !=
                "orca.native-workbench-partition-build-report/v1" ||
            !report.value("success", false) || !report.contains("stages"))
            throw std::runtime_error("原生分区阶段报告不完整");

        const auto& cache_reference = reference.at("cache_reference");
        if (cache_reference.at("content_id") != reference.at("cache_sha256") ||
            cache_reference.at("partition_digest") != reference.at("partition_digest") ||
            cache_reference.at("geometry_id") != reference.at("geometry_id"))
            throw std::runtime_error("原生分区缓存引用无效");
        NativePortrait::NativePortraitPartitionIdentity identity;
        TriangleMesh exact_native_mesh;
        const auto cache_root = boost::filesystem::path(Slic3r::data_dir()) /
            "cache" / "native-workbench-partitions";
        if (!NativePortrait::load_native_portrait_partition_cache(
                cache_reference, cache_root, identity, error, &exact_native_mesh))
            throw std::runtime_error(error);
        if (certificate.at("native_mesh_sha256") != cache_reference.at("native_mesh_sha256") ||
            identity.partition_digest != reference.at("partition_digest") ||
            identity.geometry_id != reference.at("geometry_id"))
            throw std::runtime_error("原生分区缓存身份不匹配");
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        document = nullptr;
        return false;
    }
}

bool read_portrait_check_metadata(const boost::filesystem::path& path,
                                  nlohmann::json& document)
{
    try {
        boost::system::error_code error;
        const auto bytes = boost::filesystem::file_size(path, error);
        if (error || bytes == 0 || bytes > 128ULL * 1024 * 1024 ||
            boost::filesystem::is_symlink(path, error) || error) return false;
        boost::filesystem::ifstream input(path, std::ios::binary);
        document = nlohmann::json::parse(input);
        return document.is_object();
    } catch (const std::exception&) {
        document = nullptr;
        return false;
    }
}


bool same_check_evidence(const WorkbenchCheckResult& left,
                         const WorkbenchCheckResult& right)
{
    return left.status == right.status && left.purpose == right.purpose &&
        left.model_sha256 == right.model_sha256 && left.before_faces == right.before_faces &&
        left.before_vertices == right.before_vertices && left.boundary_edges == right.boundary_edges &&
        left.boundary_loops == right.boundary_loops &&
        left.boundary_loop_sizes == right.boundary_loop_sizes &&
        left.nonmanifold_edges == right.nonmanifold_edges &&
        left.nonmanifold_vertices == right.nonmanifold_vertices &&
        left.degenerate_faces == right.degenerate_faces &&
        left.duplicate_faces == right.duplicate_faces &&
        left.inconsistent_edges == right.inconsistent_edges &&
        left.isolated_components == right.isolated_components &&
        left.small_faces == right.small_faces &&
        left.self_intersections_checked == right.self_intersections_checked &&
        left.self_intersections == right.self_intersections;
}
}

bool ModelGenerationPanel::save_portrait_print(bool quiet)
{
    if (m_portrait_print.id.empty()) return false;
    try {
        PP::commit_save_ledger({flow_root() / (m_portrait_print.id + ".json"), flow_root() / "current.json"},
            PP::encode(m_portrait_print), atomic_json);
        return true;
    } catch (const std::exception& failure) {
        if (quiet) { BOOST_LOG_TRIVIAL(error) << "portrait ledger: " << failure.what(); return false; }
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
            // Restart restores only a dormant ledger. It must not replace the
            // model the user chose to display; explicit continuation is
            // offered only after that exact model context is opened again.
            if (m_portrait_print.from_workbench() && m_portrait_print.active())
                publish_ui_state();
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
    const auto asset_id = m_displayed_model_job_id.empty()
        ? PP::portrait_source_asset_id_from_path(u8(m_displayed_model_path))
        : m_displayed_model_job_id;
    return {{"geometry_id", m_model_preview ? m_model_preview->geometry_id() : std::string{}},
        {"appearance_id", m_model_preview ? m_model_preview->semantic_source_content_id() : std::string{}},
        {"model_path", u8(m_displayed_model_path)}, {"asset_id", asset_id}};
}

bool ModelGenerationPanel::validate_portrait_inputs()
{
    if (!m_portrait_print.from_workbench()) return false;
    const auto stage = m_portrait_print.stage;
    const bool repairing = stage == PP::Stage::Check && m_workbench_auto_repair &&
        !m_finishing_candidate.empty() && m_finishing_result.source_sha256 == m_portrait_print.model_hash &&
        m_model_preview && m_model_preview->showing_model(m_finishing_candidate);
    const bool exporting = stage == PP::Stage::SaveColor && m_portrait_export_started &&
        portrait_owns_save_candidate();
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

bool ModelGenerationPanel::portrait_owns_display_context() const
{
    return m_model_preview && m_model_preview_ready &&
        m_model_preview->showing_model(m_displayed_model_path) &&
        m_model_preview->model_path() == m_displayed_model_path &&
        m_portrait_print.owns_display_context(u8(m_displayed_model_path),
            m_model_preview->model_sha256(), m_model_preview->geometry_id(),
            m_model_preview->semantic_source_content_id());
}

bool ModelGenerationPanel::portrait_owns_save_candidate() const
{
    if (!m_model_preview || !m_model_preview_ready || m_finishing_candidate.empty() ||
        !m_model_preview->showing_model(m_finishing_candidate) ||
        m_model_preview->model_path() != m_finishing_candidate)
        return false;
    return m_portrait_color_candidate_identity.owns(m_portrait_print, m_finishing_id,
        u8(m_finishing_candidate), m_finishing_result.source_sha256,
        AI::model_artifact_sha256(m_finishing_candidate), m_model_preview->geometry_id(),
        m_model_preview->semantic_source_content_id());
}

bool ModelGenerationPanel::prepare_for_model_asset_switch()
{
    if (!m_portrait_print.active() || !portrait_owns_display_context()) return true;

    const auto stage = m_portrait_print.stage;
    const bool checking = (stage == PP::Stage::Check || stage == PP::Stage::Base) &&
        m_workbench_check_running;
    const bool partitioning = stage == PP::Stage::Partition && m_native_workbench_partition_running;
    const bool recognizing = stage == PP::Stage::Color && m_model_preview->semantic_processing();
    const bool exporting = stage == PP::Stage::SaveColor && m_portrait_export_started &&
        m_finishing_running;
    if (!checking && !partitioning && !recognizing && !exporting &&
        !m_portrait_print.running()) return true;

    if (!m_portrait_print.pause_for_history_switch(exporting)) return false;
    const bool saved = save_portrait_print();

    // Every worker callback captures this revision. Invalidate callbacks
    // before waiting so a queued completion cannot adopt results afterward.
    ++m_sequence;
    m_portrait_advance_queued = false;

    if (checking) {
        if (m_workbench_check_cancel) m_workbench_check_cancel->store(true);
        if (m_workbench_check_worker.joinable()) m_workbench_check_worker.join();
        m_workbench_check_cancel.reset();
        m_workbench_check_running = false;
        m_workbench_check_result = {};
        m_workbench_check_path.clear();
        m_workbench_check_revision = 0;
        clear_pending_workbench_ai_palette();
    }
    if (partitioning) cancel_native_workbench_partition();
    if (recognizing) {
        m_model_preview->cancel_semantic_request_and_wait(false);
        m_honor_suite_before.reset();
        m_workbench_honor_semantic_active = false;
        m_workbench_honor_semantic_progress = 0;
        m_workbench_honor_semantic_context.clear();
        m_workbench_honor_semantic_geometry.clear();
        m_workbench_honor_semantic_message.clear();
    }
    if (exporting) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_finishing_worker.joinable()) m_finishing_worker.join();
        m_finishing_canceled.reset();
        m_finishing_running = false;
        m_busy = false;
        m_finishing_source.clear();
        m_portrait_color_snapshot = {};
        m_portrait_color_edit_revision = 0;
    }
    m_portrait_semantics_requested = false;
    m_portrait_export_started = false;
    BOOST_LOG_TRIVIAL(info) << "Portrait flow paused for history switch: id="
        << m_portrait_print.id << ", stage=" << int(stage);
    return saved;
}

PP::Submission ModelGenerationPanel::request_portrait_print_from_model(std::function<bool()> create_empty_project)
{
    if (m_portrait_advancing) return PP::Submission::Pending;
    if (!m_page_initialized || m_busy || m_finishing_running || !m_model_preview_ready || !m_model_preview ||
        current_style() != "realistic" || !create_empty_project ||
        !m_model_preview->showing_model(m_displayed_model_path) || !m_finishing_candidate.empty())
        return PP::Submission::Rejected;
    const auto displayed_path = u8(m_displayed_model_path);
    const auto model_hash = AI::model_artifact_sha256(m_displayed_model_path);
    const auto geometry = m_model_preview->geometry_id();
    const auto appearance = m_model_preview->semantic_source_content_id();
    if (m_portrait_print.active() && !m_portrait_print.from_workbench()) {
        const auto retired_id = m_portrait_print.id;
        const auto retired_stage = m_portrait_print.stage;
        if (!PP::retire_legacy_generation_for_workbench(m_portrait_print) ||
            !save_portrait_print())
            return PP::Submission::Rejected;
        // The durable per-flow record retains every task and file identity.
        // Only the in-memory current pointer is detached; the new workbench
        // intent replaces current.json after it has been saved below.
        m_portrait_print = {};
        m_portrait_verified_check = {};
        m_portrait_semantics_requested = false;
        m_portrait_export_started = false;
        m_portrait_color_snapshot = {};
        m_portrait_color_edit_revision = 0;
        m_portrait_color_candidate_identity = {};
        BOOST_LOG_TRIVIAL(info) << "Retired legacy generation flow before current workbench entry: id="
            << retired_id << ", stage=" << int(retired_stage)
            << ", model=" << displayed_path;
    }

    auto ownership = PP::classify_workbench_entry(m_portrait_print, displayed_path,
        model_hash, geometry, appearance, m_displayed_model_job_id);
    if (ownership == PP::WorkbenchEntryOwnership::Alias) {
        if (!m_portrait_print.adopt_source_alias(displayed_path, model_hash, geometry,
                appearance, m_displayed_model_job_id) || !save_portrait_print())
            return PP::Submission::Rejected;
        BOOST_LOG_TRIVIAL(info) << "Portrait flow rebound to canonical history asset before explicit continue: id="
            << m_portrait_print.id << ", asset=" << m_displayed_model_job_id;
        ownership = PP::WorkbenchEntryOwnership::Current;
    }
    if (ownership == PP::WorkbenchEntryOwnership::Current && m_portrait_print.running())
        return PP::Submission::Pending;
    if (ownership == PP::WorkbenchEntryOwnership::Foreign) {
        const auto retired_id = m_portrait_print.id;
        const auto retired_stage = m_portrait_print.stage;
        if (m_portrait_print.running()) {
            if (!m_portrait_print.pause_for_history_switch(false) || !save_portrait_print())
                return PP::Submission::Rejected;
        }
        // The per-flow record remains the recovery owner. Only the in-memory
        // current pointer is released so this exact displayed model can start a
        // new flow; current.json is replaced when the new intent is committed.
        m_portrait_print = {};
        m_portrait_verified_check = {};
        m_portrait_semantics_requested = false;
        m_portrait_export_started = false;
        m_portrait_color_snapshot = {};
        m_portrait_color_edit_revision = 0;
        m_portrait_color_candidate_identity = {};
        BOOST_LOG_TRIVIAL(info) << "Archived foreign portrait flow before current workbench entry: id="
            << retired_id << ", stage=" << int(retired_stage)
            << ", model=" << displayed_path;
    }
    if (PP::workbench_entry_is_duplicate(m_portrait_print, displayed_path, model_hash))
        return PP::Submission::Pending; // Explicit Continue owns a paused run.
    const auto palette = m_model_preview->color_trial_state();
    if (!PP::palette_allows_automatic_workbench(palette.source, palette.suite_id,
            !palette.color_card_target_palette.empty()))
        return PP::Submission::Rejected; // Other suites/cards keep their manual workflow and project.

    // A generic workbench check may have started before the shell identified
    // this model as the automatic portrait entry. Retire its token and wait for
    // its worker before publishing portrait ownership. Any already queued
    // callback sees the token mismatch and can only clean up its temporary
    // files. The portrait Check stage then performs a fresh read-only source
    // inspection; strict repair remains exclusively in Partition.
    const auto superseded_check = m_workbench_check_cancel;
    if (superseded_check) superseded_check->store(true);
    if (m_workbench_check_worker.joinable()) m_workbench_check_worker.join();
    if (m_workbench_check_cancel == superseded_check) m_workbench_check_cancel.reset();
    m_workbench_check_running = false;
    m_workbench_auto_repair = false;
    m_workbench_check_result = {};
    m_workbench_check_path.clear();
    m_workbench_check_revision = 0;
    m_workbench_check_card_visible = false;
    m_workbench_force_import_ack_hash.clear();
    m_workbench_force_import_ack_revision = 0;
    clear_pending_workbench_ai_palette();

    RunningGuard guard(m_portrait_advancing);
    PP::State flow;
    flow.entry = "completed_model";
    flow.id = ModelGenerationPresentation::new_request_id(); flow.stage = PP::Stage::Project;
    flow.original_model_path = flow.model_path = displayed_path;
    flow.original_model_hash = flow.model_hash = model_hash;
    flow.geometry_id = m_model_preview->geometry_id();
    flow.appearance_id = m_model_preview->semantic_source_content_id();
    flow.edit_revision = m_model_preview->leaf_edit_revision();
    flow.model_color_count = int(NativePortrait::portrait_model_color_count);
    flow.physical_slot_count = int(honor_people_suite().physical_slots.size());
    flow.support_slot_count = flow.physical_slot_count - flow.model_color_count;
    flow.requested_count = flow.final_count = flow.model_color_count;
    flow.frozen = portrait_inputs_snapshot();
    flow.suite_revision = honor_people_suite().revision;
    if (flow.model_hash.empty() || flow.geometry_id.empty() || flow.appearance_id.empty() ||
        !m_model_preview->original_evidence_available()) return PP::Submission::Rejected;
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
    if (!portrait_owns_display_context()) return false;
    RunningGuard guard(m_portrait_advancing);
    auto* plater = wxGetApp().plater();
    if (!plater) return false;
    if (m_portrait_print.restored && m_portrait_print.record_version >= 5 &&
        (m_portrait_print.partition_pending || m_portrait_print.partition_committed)) {
        try {
            const auto restored = NativePortrait::restore_native_portrait_partition_archive(
                plater->model().get_auxiliary_file_temp_path(),
                boost::filesystem::path(Slic3r::data_dir()) / "cache" / "native-workbench-partitions");
            if (restored != 0)
                BOOST_LOG_TRIVIAL(info) << "Restored " << restored
                    << " native portrait partition cache entries before explicit flow continuation.";
        } catch (const std::exception& failure) {
            // A damaged optional archive is handled by the ordinary checkpoint
            // validation below; it never blocks unrelated project loading.
            BOOST_LOG_TRIVIAL(warning) << "Native portrait partition cache restore unavailable: "
                << failure.what();
        }
    }
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
    bool checkpoint_recovered = false;
    bool checkpoint_invalid = false;
    std::string checkpoint_error;
    if (m_portrait_print.restored && m_portrait_print.record_version >= 5 &&
        !m_portrait_print.colored_saved &&
        (m_portrait_print.partition_pending || m_portrait_print.partition_committed)) {
        nlohmann::json committed_document;
        if (validate_native_partition_checkpoint(
                m_portrait_print, committed_document, checkpoint_error) &&
            m_portrait_print.adopt_partition_commit(committed_document)) {
            checkpoint_recovered = true;
            if (!save_portrait_print()) return false;
        } else {
            checkpoint_invalid = true;
            BOOST_LOG_TRIVIAL(warning) << "Native portrait checkpoint rejected: " << checkpoint_error;
        }
    }
    const bool incomplete_v5 = m_portrait_print.record_version >= 5 &&
        m_portrait_print.restored && !m_portrait_print.colored_saved &&
        PP::workbench_order(m_portrait_print.stage) >= PP::workbench_order(PP::Stage::Partition) &&
        !checkpoint_recovered;
    const bool rebuild_from_original = !checkpoint_recovered &&
        (m_portrait_print.record_version < 5 || incomplete_v5 || checkpoint_invalid ||
         m_portrait_print.partition_needs_rebuild(
             PortraitColorBlocks::portrait_postprocess_algorithm_version));
    if (rebuild_from_original) {
        if (!plater->model().objects.empty()) {
            pause_portrait_print("旧分区或中断记录需要从原始模型重建；请先保存当前工程并创建空工程。");
            return false;
        }
        const auto original = path8(m_portrait_print.original_model_path);
        const auto original_hash = AI::model_artifact_sha256(original);
        if (original.empty() || original_hash != m_portrait_print.original_model_hash ||
            !m_portrait_print.reset_partition_to_original(
                PortraitColorBlocks::portrait_postprocess_algorithm_version,
                m_portrait_print.original_model_path, original_hash)) {
            pause_portrait_print("无法从不可变原始模型重建 v11 分区；旧文件、候选和元数据均已保留。");
            return false;
        }
        m_portrait_print.final_palette.clear();
        m_portrait_print.model_color_count = int(NativePortrait::portrait_model_color_count);
        m_portrait_print.physical_slot_count = int(honor_people_suite().physical_slots.size());
        m_portrait_print.support_slot_count =
            m_portrait_print.physical_slot_count - m_portrait_print.model_color_count;
        m_portrait_print.requested_count = m_portrait_print.final_count =
            m_portrait_print.model_color_count;
        if (checkpoint_invalid)
            m_portrait_print.notices.push_back(
                "原生分区引用不完整或已损坏，旧成果保留，本次从原始模型重建。");
        m_portrait_semantics_requested = m_portrait_export_started = false;
        m_portrait_color_snapshot = {};
        m_portrait_verified_check = {};
        if (!save_portrait_print()) return false;
    }
    if (m_portrait_print.restored) {
        if (!plater->model().objects.empty()) {
            pause_portrait_print("请先保存当前工程并创建空工程，再手动继续已保存人像。"); return false;
        }
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
    if (!restore_portrait_workbench_check_certificate()) {
        m_workbench_check_result = {};
        m_workbench_check_path.clear();
        m_workbench_check_revision = 0;
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

bool ModelGenerationPanel::restore_portrait_workbench_check_certificate()
{
    if (!m_model_preview || !m_model_preview_ready || m_displayed_model_path.empty() ||
        !m_model_preview->showing_model(m_displayed_model_path) ||
        m_model_preview->model_path() != m_displayed_model_path) return false;
    const auto model_hash = AI::model_artifact_sha256(m_displayed_model_path);
    const auto geometry_id = m_model_preview->geometry_id();
    const auto appearance_id = m_model_preview->semantic_source_content_id();
    if (!PP::valid_sha256(model_hash) || model_hash != m_model_preview->model_sha256() ||
        geometry_id.empty() || appearance_id.empty()) return false;

    const auto publish = [&](const nlohmann::json& certificate) {
        WorkbenchCheckResult restored;
        if (!PP::decode_workbench_check_certificate(certificate, restored)) return false;
        m_workbench_check_result = std::move(restored);
        m_workbench_check_path = m_displayed_model_path.string();
        m_workbench_check_revision = m_sequence;
        m_workbench_check_card_visible = false;
        return true;
    };

    if (model_hash == m_portrait_print.original_model_hash &&
        m_portrait_print.source_check.is_object() &&
        m_portrait_print.source_check.contains("workbench_check_certificate")) {
        const auto& certificate = m_portrait_print.source_check.at("workbench_check_certificate");
        if (PP::valid_workbench_check_certificate(certificate,
                WorkbenchCheckPurpose::PortraitSourceRisk, model_hash, geometry_id, appearance_id,
                {}, m_portrait_print.source_check) && publish(certificate)) return true;
    }

    const auto metadata_path = AI::beauty_metadata_path(
        m_displayed_model_path, ModelGenerationPresentation::generated_models_root());
    nlohmann::json metadata;
    if (!read_portrait_check_metadata(metadata_path, metadata)) return false;
    const bool require_post_color = metadata.contains("post_color_check");
    nlohmann::json reference;
    const bool exact_flow = !m_portrait_print.id.empty() &&
        m_portrait_print.model_hash == model_hash &&
        PP::same_model_path(m_portrait_print.model_path, u8(m_displayed_model_path)) &&
        m_portrait_print.geometry_id == geometry_id && m_portrait_print.appearance_id == appearance_id;
    const std::string flow_id = exact_flow ? m_portrait_print.id : std::string{};
    if (!portrait_partition_metadata_matches(metadata, metadata_path, m_displayed_model_path,
            model_hash, geometry_id, appearance_id, require_post_color, flow_id, reference)) return false;

    nlohmann::json embedded = metadata.value("workbench_check_certificate", nlohmann::json{});
    const auto certificate_valid = [&](const nlohmann::json& certificate) {
        if (!PP::valid_workbench_check_certificate(certificate,
                WorkbenchCheckPurpose::PortraitPostColorReadOnly, model_hash, geometry_id,
                appearance_id, u8(metadata_path), metadata,
                reference.at("partition_digest").get<std::string>(),
                reference.at("topology_hash").get<std::string>())) return false;
        WorkbenchCheckResult restored;
        if (!PP::decode_workbench_check_certificate(certificate, restored) ||
            !workbench_topology_safe(restored) ||
            metadata.at("triangle_count").get<size_t>() != restored.before_faces) return false;
        if (require_post_color) {
            const auto& check = metadata.at("post_color_check");
            if (check.at("faces").get<size_t>() != restored.before_faces ||
                check.at("vertices").get<size_t>() != restored.before_vertices ||
                check.at("boundary_edges").get<size_t>() != restored.boundary_edges ||
                check.at("nonmanifold_edges").get<size_t>() != restored.nonmanifold_edges ||
                check.at("nonmanifold_vertices").get<size_t>() != restored.nonmanifold_vertices ||
                check.at("degenerate_faces").get<size_t>() != restored.degenerate_faces ||
                check.at("duplicate_faces").get<size_t>() != restored.duplicate_faces ||
                check.at("inconsistent_edges").get<size_t>() != restored.inconsistent_edges ||
                check.at("self_intersections_checked").get<bool>() !=
                    restored.self_intersections_checked ||
                check.at("self_intersections").get<size_t>() != restored.self_intersections)
                return false;
        }
        return true;
    };

    const bool embedded_valid = embedded.is_object() && certificate_valid(embedded);
    if (embedded_valid && exact_flow && m_portrait_print.workbench_check_certificate.is_object() &&
        !m_portrait_print.workbench_check_certificate.empty() &&
        m_portrait_print.workbench_check_certificate != embedded) return false;
    const nlohmann::json certificate = embedded_valid ? embedded :
        exact_flow ? m_portrait_print.workbench_check_certificate : nlohmann::json{};
    if (!certificate_valid(certificate) || !publish(certificate)) return false;
    if (exact_flow) m_portrait_print.workbench_check_certificate = certificate;
    return true;
}

bool ModelGenerationPanel::persist_portrait_workbench_check_certificate()
{
    if (!portrait_owns_display_context() || !m_portrait_print.colored_saved ||
        !workbench_topology_safe(m_workbench_check_result) ||
        m_workbench_check_result.model_sha256 != m_portrait_print.model_hash) return false;
    const auto metadata_path = AI::beauty_metadata_path(
        m_displayed_model_path, ModelGenerationPresentation::generated_models_root());
    nlohmann::json metadata;
    if (!read_portrait_check_metadata(metadata_path, metadata)) return false;
    nlohmann::json reference;
    if (!portrait_partition_metadata_matches(metadata, metadata_path, m_displayed_model_path,
            m_portrait_print.model_hash, m_portrait_print.geometry_id,
            m_portrait_print.appearance_id, true, m_portrait_print.id, reference)) return false;

    auto check = m_workbench_check_result;
    check.purpose = WorkbenchCheckPurpose::PortraitPostColorReadOnly;
    nlohmann::json certificate = metadata.value(
        "workbench_check_certificate", nlohmann::json{});
    const auto valid = [&](const nlohmann::json& value) {
        WorkbenchCheckResult restored;
        return PP::valid_workbench_check_certificate(value,
                   WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                   m_portrait_print.model_hash, m_portrait_print.geometry_id,
                   m_portrait_print.appearance_id, u8(metadata_path), metadata,
                   m_portrait_print.basis_signature, m_portrait_print.partition_topology_hash) &&
            PP::decode_workbench_check_certificate(value, restored) &&
            same_check_evidence(restored, check);
    };
    if (!valid(certificate)) {
        certificate = PP::encode_workbench_check_certificate(check,
            WorkbenchCheckPurpose::PortraitPostColorReadOnly,
            m_portrait_print.geometry_id, m_portrait_print.appearance_id,
            u8(metadata_path), metadata, m_portrait_print.basis_signature,
            m_portrait_print.partition_topology_hash);
        if (!valid(certificate)) return false;
    }
    m_portrait_print.workbench_check_certificate = std::move(certificate);
    return save_portrait_print();
}

bool ModelGenerationPanel::record_portrait_import(const std::string& id, const AI::ModelImportResult& result)
{
    WorkbenchCheckResult certified_check;
    const bool certificate_matches = PP::decode_workbench_check_certificate(
            m_portrait_print.workbench_check_certificate, certified_check) &&
        same_check_evidence(certified_check, m_workbench_check_result);
    if (!portrait_owns_display_context() ||
        !m_portrait_print.current(id, PP::Stage::Import) || !result.imported() ||
        !m_portrait_print.partition_committed || !m_portrait_print.colored_saved ||
        !PP::valid_native_partition_reference(m_portrait_print.native_portrait_partition,
            m_portrait_print.basis_signature, m_portrait_print.partition_topology_hash) ||
        !PP::valid_region_semantics(
            m_portrait_print.region_semantics, m_portrait_print.basis_signature) ||
        !PP::valid_native_color_reduction(m_portrait_print.native_color_reduction,
            m_portrait_print.basis_signature,
            m_portrait_print.region_semantics.at("regions").size()) ||
        !PP::valid_final_slot_binding(m_portrait_print.final_slot_binding,
            m_portrait_print.basis_signature,
            m_portrait_print.native_color_reduction.at("mapping_digest").get<std::string>(),
            m_portrait_print.region_semantics.at("regions").size(),
            size_t(m_portrait_print.model_color_count)) ||
        !PP::valid_post_color_check(
            m_portrait_print.post_color_check, m_portrait_print.native_portrait_partition) ||
        !workbench_topology_safe(m_workbench_check_result) ||
        m_workbench_check_result.model_sha256 != m_portrait_print.model_hash ||
        !certificate_matches ||
        result.native_repair_completed || result.subface_colors_applied ||
        result.subface_color_count != 0) return false;
    // This compatibility flag means the native preparation certificate has
    // reached the project. No repair is allowed during or after import.
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
    const bool owns_processing_context = portrait_owns_display_context() ||
        (m_portrait_print.stage == PP::Stage::SaveColor && portrait_owns_save_candidate());
    if (m_shutdown || !m_portrait_print.running() || m_portrait_advance_queued ||
        (m_portrait_print.from_workbench() && !owns_processing_context)) return;
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

bool ModelGenerationPanel::native_workbench_context_matches(
    const NativeWorkbenchPartitionContext& context) const
{
    return context.valid() && context == m_native_workbench_context && m_model_preview &&
        m_model_preview_ready && m_model_preview->showing_model(context.model_path) &&
        m_displayed_model_path == context.model_path && m_sequence == context.generation &&
        AI::model_artifact_sha256(context.model_path) == context.source_sha256 &&
        m_model_preview->geometry_id() == context.geometry_id &&
        m_model_preview->semantic_source_content_id() == context.appearance_id;
}

bool ModelGenerationPanel::restore_native_workbench_partition_checkpoint()
{
    m_native_workbench_partition_reference = nullptr;
    m_native_workbench_partition_report = nullptr;
    m_native_workbench_partition_certificate = nullptr;
    m_native_workbench_partition_commit = nullptr;
    if (!native_workbench_context_matches(m_native_workbench_context)) return false;
    const auto partition = m_model_preview->native_portrait_partition();
    const auto& preview_reference = m_model_preview->native_workbench_partition_reference();
    if (!partition || !partition->valid() ||
        !PP::valid_native_partition_reference(preview_reference, partition->partition_digest) ||
        preview_reference.at("carrier_sha256") != m_native_workbench_context.source_sha256 ||
        preview_reference.at("geometry_id") != m_native_workbench_context.geometry_id ||
        preview_reference.at("appearance_id") != m_native_workbench_context.appearance_id)
        return false;

    const auto metadata_path = path8(preview_reference.at("metadata_path").get<std::string>());
    nlohmann::json metadata;
    nlohmann::json verified_reference;
    if (!read_portrait_check_metadata(metadata_path, metadata) ||
        !restorable_native_partition_metadata_matches(metadata, metadata_path,
            m_native_workbench_context.model_path, m_native_workbench_context.source_sha256,
            m_native_workbench_context.geometry_id, m_native_workbench_context.appearance_id,
            verified_reference) || verified_reference != preview_reference)
        return false;
    const auto report = metadata.value("partition_build_report", nlohmann::json{});
    const auto certificate = metadata.value("workbench_check_certificate", nlohmann::json{});
    if (report.value("schema", std::string{}) !=
            "orca.native-workbench-partition-build-report/v1" ||
        !report.value("success", false) ||
        !PP::valid_workbench_check_certificate(certificate,
            WorkbenchCheckPurpose::PortraitPostColorReadOnly,
            m_native_workbench_context.source_sha256,
            m_native_workbench_context.geometry_id,
            m_native_workbench_context.appearance_id, u8(metadata_path), metadata,
            partition->partition_digest,
            preview_reference.at("topology_hash").get<std::string>()))
        return false;

    m_native_workbench_partition_reference = preview_reference;
    m_native_workbench_partition_report = report;
    m_native_workbench_partition_certificate = certificate;
    m_native_workbench_partition_commit = std::move(metadata);
    return true;
}

void ModelGenerationPanel::publish_native_workbench_context()
{
    NativeWorkbenchPartitionContext next;
    if (!m_shutdown && m_model_preview_ready && m_model_preview &&
        m_model_preview->showing_model(m_displayed_model_path)) {
        next.model_path = m_displayed_model_path;
        next.source_sha256 = AI::model_artifact_sha256(m_displayed_model_path);
        next.geometry_id = m_model_preview->geometry_id();
        next.appearance_id = m_model_preview->semantic_source_content_id();
        next.generation = m_sequence;
    }
    if (next != m_native_workbench_context) {
        if (m_native_workbench_partition_running) cancel_native_workbench_partition();
        m_native_workbench_context = std::move(next);
        m_native_workbench_partition_reference = nullptr;
        m_native_workbench_partition_report = nullptr;
        m_native_workbench_partition_certificate = nullptr;
        m_native_workbench_partition_commit = nullptr;
    }
    if (m_native_workbench_context.valid())
        restore_native_workbench_partition_checkpoint();
}

bool ModelGenerationPanel::adopt_native_workbench_partition_for_portrait()
{
    if (!m_portrait_print.current(m_portrait_print.id, PP::Stage::Partition) ||
        !native_workbench_context_matches(m_native_workbench_context) ||
        !PP::valid_native_partition_reference(m_native_workbench_partition_reference) ||
        !m_native_workbench_partition_report.is_object()) return false;
    const auto partition = m_model_preview->native_portrait_partition();
    if (!partition || !partition->valid() ||
        partition->partition_digest !=
            m_native_workbench_partition_reference.at("partition_digest").get<std::string>())
        return false;
    const auto metadata_path =
        m_native_workbench_partition_reference.at("metadata_path").get<std::string>();
    if (!m_portrait_print.partition_committed) {
        if (!m_portrait_print.partition_pending &&
            !m_portrait_print.begin_partition(metadata_path)) return false;
        if (!m_portrait_print.commit_partition(m_native_workbench_partition_reference,
                m_native_workbench_partition_reference.at("topology_hash").get<std::string>(),
                partition->partition_digest, m_native_workbench_partition_report,
                m_native_workbench_partition_certificate)) return false;
    }
    if (!m_portrait_print.activate_partition(u8(m_native_workbench_context.model_path),
            m_native_workbench_context.source_sha256,
            m_native_workbench_context.geometry_id,
            m_native_workbench_context.appearance_id)) return false;
    m_portrait_print.edit_revision = m_model_preview->leaf_edit_revision();
    m_portrait_print.boundary_algorithm_version = NativePortrait::algorithm_version;
    WorkbenchCheckResult certified;
    if (PP::decode_workbench_check_certificate(
            m_native_workbench_partition_certificate, certified)) {
        m_workbench_check_result = std::move(certified);
        m_workbench_check_path = m_native_workbench_context.model_path.string();
        m_workbench_check_revision = m_sequence;
    }
    m_workbench_operation_notice =
        "Orca 原生原子色区已恢复，正在只读识别区域语义并绑定五个模型槽位。";
    return advance_portrait_print(m_portrait_print.id,
        PP::Stage::Partition, PP::Stage::Color);
}

bool ModelGenerationPanel::bind_native_workbench_partition_commit_to_portrait()
{
    if (!m_portrait_print.current(m_portrait_print.id, PP::Stage::Partition) ||
        !PP::valid_native_partition_reference(m_native_workbench_partition_reference) ||
        !m_native_workbench_partition_commit.is_object() ||
        !m_portrait_print.source_check.is_object()) return false;
    const auto current_id = m_native_workbench_partition_commit.value(
        "portrait_print_id", std::string{});
    const auto requester_id = m_native_workbench_partition_commit.value(
        "requesting_portrait_print_id", std::string{});
    if (current_id == m_portrait_print.id && requester_id == m_portrait_print.id)
        return true;

    WorkbenchCheckResult check;
    if (!PP::decode_workbench_check_certificate(
            m_native_workbench_partition_certificate, check)) return false;
    auto metadata = m_native_workbench_partition_commit;
    if (!PP::bind_native_workbench_partition_commit_identity(
            metadata, m_portrait_print.id, m_portrait_print.source_check)) return false;
    try {
        const auto metadata_path = boost::filesystem::path(
            m_native_workbench_partition_reference.at("metadata_path").get<std::string>());
        const auto partition_digest = m_native_workbench_partition_reference.at(
            "partition_digest").get<std::string>();
        const auto topology_hash = m_native_workbench_partition_reference.at(
            "topology_hash").get<std::string>();
        metadata["workbench_check_certificate"] = PP::encode_workbench_check_certificate(
            check, WorkbenchCheckPurpose::PortraitPostColorReadOnly,
            m_native_workbench_partition_reference.at("geometry_id").get<std::string>(),
            m_native_workbench_partition_reference.at("appearance_id").get<std::string>(),
            u8(metadata_path), metadata, partition_digest, topology_hash);
        const auto published = NativePortrait::replace_native_portrait_partition_commit(
            metadata_path,
            boost::filesystem::path(metadata.at("model_path").get<std::string>()),
            boost::filesystem::path(metadata.at("source_model").get<std::string>()),
            metadata);
        if (published.value("portrait_print_id", std::string{}) != m_portrait_print.id ||
            published.value("requesting_portrait_print_id", std::string{}) != m_portrait_print.id)
            return false;
        m_native_workbench_partition_commit = published;
        m_native_workbench_partition_certificate =
            published.at("workbench_check_certificate");
        return true;
    } catch (const std::exception& failure) {
        BOOST_LOG_TRIVIAL(error) << "Failed to bind native partition commit to portrait flow: "
                                 << failure.what();
        return false;
    }
}

void ModelGenerationPanel::cancel_native_workbench_partition()
{
    const bool was_running = m_native_workbench_partition_running;
    if (m_native_workbench_partition_cancel) m_native_workbench_partition_cancel->store(true);
    if (m_native_workbench_partition_worker.joinable()) m_native_workbench_partition_worker.join();
    m_native_workbench_partition_cancel.reset();
    m_native_workbench_partition_running = false;
    m_native_workbench_partition_consumers = 0;
    m_native_workbench_partition_context = {};
    m_honor_suite_partition_pending = false;
    m_honor_suite_partition_resume_queued = false;
    if (was_running) m_busy = false;
}

void ModelGenerationPanel::queue_honor_suite_after_native_partition()
{
    if (!m_honor_suite_partition_pending || m_honor_suite_partition_resume_queued) return;
    const auto partition = m_model_preview ? m_model_preview->native_portrait_partition() : nullptr;
    const std::string persisted_digest = m_native_workbench_partition_reference.is_object()
        ? m_native_workbench_partition_reference.value("partition_digest", std::string{})
        : std::string{};
    const PP::HonorSuiteContinuationIdentity identity {
        u8(m_native_workbench_context.model_path),
        m_native_workbench_context.source_sha256,
        m_native_workbench_context.geometry_id,
        m_native_workbench_context.appearance_id,
        m_native_workbench_context.generation,
        partition && partition->valid() ? partition->partition_digest : std::string{}
    };
    if (!identity.valid() || identity.partition_digest != persisted_digest ||
        !PP::valid_native_partition_reference(
            m_native_workbench_partition_reference, identity.partition_digest)) {
        m_honor_suite_partition_pending = false;
        m_honor_suite_partition_resume_queued = false;
        return;
    }
    const auto context = m_native_workbench_context;
    m_honor_suite_partition_resume_queued = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    wxGetApp().CallAfter([weak, context, identity] {
        if (!weak) return;
        auto* self = weak.get();
        self->m_honor_suite_partition_resume_queued = false;
        if (!self->m_honor_suite_partition_pending) return;
        const auto current_partition = self->m_model_preview
            ? self->m_model_preview->native_portrait_partition() : nullptr;
        const std::string persisted_digest =
            self->m_native_workbench_partition_reference.is_object()
                ? self->m_native_workbench_partition_reference.value(
                    "partition_digest", std::string{})
                : std::string{};
        const bool exact_owner = !self->m_shutdown &&
            self->native_workbench_context_matches(context) && current_partition &&
            current_partition->valid() &&
            identity.matches(u8(self->m_native_workbench_context.model_path),
                self->m_native_workbench_context.source_sha256,
                self->m_native_workbench_context.geometry_id,
                self->m_native_workbench_context.appearance_id,
                self->m_native_workbench_context.generation,
                current_partition->partition_digest, persisted_digest) &&
            PP::valid_native_partition_reference(
                self->m_native_workbench_partition_reference, identity.partition_digest);
        if (!exact_owner) {
            self->m_honor_suite_partition_pending = false;
            self->m_honor_suite_partition_resume_queued = false;
            return;
        }
        self->m_honor_suite_partition_pending = false;
        if (!self->m_model_preview->request_honor_people_suite()) {
            self->m_workbench_operation_notice =
                "Orca 原生原子分区已建立，但人物六色未能继续，已保留原配色。";
            self->refresh_post_generation_workbench();
            self->publish_workbench_state();
        }
    });
}

bool ModelGenerationPanel::ensure_native_workbench_partition(
    NativeWorkbenchPartitionConsumers consumers)
{
    const auto has_consumer = [consumers](NativeWorkbenchPartitionConsumer consumer) {
        return PP::native_workbench_partition_requested(consumers, consumer);
    };
    const bool automatic_requested = has_consumer(
        NativeWorkbenchPartitionConsumer::AutomaticPalette);
    const bool portrait_requested = has_consumer(
        NativeWorkbenchPartitionConsumer::PortraitFlow);
    const bool honor_suite_requested = has_consumer(
        NativeWorkbenchPartitionConsumer::HonorSuite);
    if (consumers == 0) return false;
    if (m_shutdown || !m_model_preview || !m_model_preview_ready ||
        !m_model_preview->showing_model(m_displayed_model_path)) return false;
    if (!native_workbench_context_matches(m_native_workbench_context))
        publish_native_workbench_context();
    if (!native_workbench_context_matches(m_native_workbench_context)) return false;
    if (portrait_requested &&
        !m_portrait_print.current(m_portrait_print.id, PP::Stage::Partition)) return false;
    if (m_model_preview->has_native_portrait_partition_conflict() ||
        (m_beauty_controls && m_beauty_controls->has_changes())) return false;

    auto activation_before = std::make_shared<NativeWorkbenchActivationRollback>();
    activation_before->beauty = capture_beauty_candidate();
    activation_before->displayed_model_path = m_displayed_model_path;
    activation_before->displayed_model_job_id = m_displayed_model_job_id;
    activation_before->partition_reference = m_native_workbench_partition_reference;
    activation_before->partition_report = m_native_workbench_partition_report;
    activation_before->partition_certificate = m_native_workbench_partition_certificate;
    activation_before->partition_commit = m_native_workbench_partition_commit;
    activation_before->workbench_check = m_workbench_check_result;
    activation_before->workbench_check_path = m_workbench_check_path;
    activation_before->workbench_check_revision = m_workbench_check_revision;
    activation_before->portrait_print = m_portrait_print;
    activation_before->portrait_verified_check = m_portrait_verified_check;

    if (restore_native_workbench_partition_checkpoint()) {
        m_native_workbench_partition_consumers |= consumers;
        bool completed = !portrait_requested ||
            bind_native_workbench_partition_commit_to_portrait();
        if (completed && automatic_requested)
            completed = m_model_preview->activate_current_ai_palette(
                int(NativePortrait::default_model_color_count));
        if (completed && portrait_requested)
            completed = adopt_native_workbench_partition_for_portrait();
        if (!completed) {
            const bool preview_restored = restore_beauty_candidate(activation_before->beauty);
            m_displayed_model_path = activation_before->displayed_model_path;
            m_displayed_model_job_id = activation_before->displayed_model_job_id;
            m_native_workbench_partition_reference = activation_before->partition_reference;
            m_native_workbench_partition_report = activation_before->partition_report;
            m_native_workbench_partition_certificate = activation_before->partition_certificate;
            m_native_workbench_partition_commit = activation_before->partition_commit;
            m_workbench_check_result = activation_before->workbench_check;
            m_workbench_check_path = activation_before->workbench_check_path;
            m_workbench_check_revision = activation_before->workbench_check_revision;
            if (portrait_requested) {
                m_portrait_print = activation_before->portrait_print;
                m_portrait_verified_check = activation_before->portrait_verified_check;
                if (!save_portrait_print())
                    BOOST_LOG_TRIVIAL(error) << "Failed to persist portrait flow rollback after cached partition activation.";
            }
            if (!preview_restored)
                BOOST_LOG_TRIVIAL(error) << "Failed to restore workbench preview after cached partition activation rollback.";
        }
        if (automatic_requested) {
            clear_pending_workbench_ai_palette();
            if (!completed)
                m_workbench_operation_notice =
                    "原生原子分区已恢复，但当前模型无法应用 6 色 AI配色。";
        }
        if (honor_suite_requested && completed)
            queue_honor_suite_after_native_partition();
        m_native_workbench_partition_consumers &= std::uint8_t(~consumers);
        return completed;
    }
    if (m_native_workbench_partition_running) {
        if (m_native_workbench_partition_context != m_native_workbench_context) return false;
        m_native_workbench_partition_consumers |= consumers;
        return true;
    }
    if (m_native_workbench_partition_worker.joinable()) m_native_workbench_partition_worker.join();

    const auto source_appearance = m_model_preview->original_appearance_reference();
    const auto source_appearance_error = m_model_preview->original_appearance_error();
    const auto source = source_appearance.valid()
        ? source_appearance.origin_model : m_displayed_model_path;
    const auto source_hash = source_appearance.valid()
        ? source_appearance.origin_sha256 : m_native_workbench_context.source_sha256;
    const auto context = m_native_workbench_context;
    const auto id = portrait_requested
        ? m_portrait_print.id : std::string{};
    const auto edit_revision = m_model_preview->leaf_edit_revision();
    const auto asset_id = "native-workbench-partition-v12-" +
        ModelGenerationPresentation::new_request_id();
    const auto destination = ModelGenerationPresentation::temp_path(asset_id, "obj");
    const auto metadata_path = ModelGenerationPresentation::library_metadata_path(asset_id);
    auto appearance_carrier = destination;
    appearance_carrier.replace_extension(".source-appearance.obj");
    const auto appearance_root = ModelGenerationPresentation::generated_models_root();
    const auto cache_root = boost::filesystem::path(Slic3r::data_dir()) /
        "cache" / "native-workbench-partitions";
    const bool share_exact_vertices = !m_model_preview->retains_surface_attributes();
    if (portrait_requested &&
        (!m_portrait_print.begin_partition(u8(metadata_path)) || !save_portrait_print())) return false;
    const auto canceled = m_native_workbench_partition_cancel = std::make_shared<std::atomic<bool>>(false);
    const auto committed = std::make_shared<std::atomic<bool>>(false);
    auto report = std::make_shared<NativePortrait::NativePortraitPartitionBuildReport>();
    auto reference = std::make_shared<nlohmann::json>();
    auto workbench_certificate = std::make_shared<nlohmann::json>();
    auto commit_document = std::make_shared<nlohmann::json>();
    const auto report_started = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());
    m_native_workbench_partition_running = true;
    m_native_workbench_partition_consumers = consumers;
    m_native_workbench_partition_context = context;
    m_busy = true;
    m_workbench_check_card_visible = true;
    m_workbench_operation_notice =
        "正在通过 Orca 原生纹理链路严格修复并建立最多 32 色固定原子区域。";
    refresh_controls();
    refresh_post_generation_workbench();
    publish_workbench_state();

    wxWeakRef<ModelGenerationPanel> weak(this);
    try {
        m_native_workbench_partition_worker = std::thread([weak, source, source_hash, source_appearance,
            source_appearance_error, source_check = portrait_requested
                ? m_portrait_print.source_check : nlohmann::json{}, id, asset_id,
            context, edit_revision, destination, metadata_path, appearance_carrier,
            appearance_root, cache_root, share_exact_vertices, canceled, committed, report, reference,
            workbench_certificate, commit_document, report_started, activation_before] {
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            auto partition = std::make_shared<NativePortrait::NativePortraitPartition>();
            std::string output_hash;
            std::string error;
            std::string topology_hash;
            nlohmann::json topology_certificate;
            std::optional<NativePortrait::BuildStage> active_stage;
            std::chrono::steady_clock::time_point active_started;
            const auto stopped = [canceled] { return canceled->load(); };
            const auto begin_stage = [&](NativePortrait::BuildStage stage, std::string detail) {
                active_stage = stage;
                active_started = std::chrono::steady_clock::now();
                auto& value = report->stage(stage);
                value.status = NativePortrait::BuildStageStatus::NotRun;
                value.detail = std::move(detail);
            };
            const auto finish_stage = [&](NativePortrait::BuildStageStatus status) {
                if (!active_stage) return;
                auto& value = report->stage(*active_stage);
                value.duration_ms += portrait_elapsed_ms(active_started);
                value.status = status;
                active_stage.reset();
            };
            const auto cleanup = [&] {
                if (committed->load()) return;
                boost::system::error_code ignored;
                boost::filesystem::remove(destination, ignored);
                boost::filesystem::remove(appearance_carrier, ignored);
                boost::filesystem::remove(metadata_path, ignored);
            };
            try {
                begin_stage(NativePortrait::BuildStage::SourceCheck,
                    "Verify recorded source identity and topology risks before partitioning.");
                if (!source_appearance.valid())
                    throw std::runtime_error(source_appearance_error.empty()
                        ? "原始外观证据缺失，未建立 v11 分区。" : source_appearance_error);
                if (AI::model_artifact_sha256(source) != source_hash)
                    throw std::runtime_error("原始模型身份已变化，未建立 v11 分区。");
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);
                begin_stage(NativePortrait::BuildStage::Decode, "Decode immutable texture and material source.");
                DecodedWorkbenchModel decoded;
                if (!decode_workbench_model(source, decoded, error, stopped) || !decoded.texture)
                    throw std::runtime_error(error.empty()
                        ? "原始模型缺少可解码的纹理、材质或面颜色。" : error);
                if (!normalize_decoded_workbench_model_to_millimeters(decoded, error))
                    throw std::runtime_error(error.empty()
                        ? "原始模型无法转换到毫米坐标。" : error);
                const auto source_bounds_mm = NativePortrait::native_workbench_bounds(*decoded.texture);
                if (!source_bounds_mm.valid())
                    throw std::runtime_error("原始模型毫米包围盒无效，未建立分区。");
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);
                *partition = NativePortrait::build_native_workbench_partition(
                    *decoded.texture,
                    NativePortrait::native_mesh_repair_callback(&report->strict_repair), stopped, report.get());
                if (!partition->valid() || !report->strict_repair.success ||
                    !report->strict_repair.output_topology.safe())
                    throw std::runtime_error("严格修复或原生分区证书无效，未发布分区。");
                const auto carrier_bounds_mm = NativePortrait::native_workbench_bounds(*partition);
                if (!NativePortrait::native_workbench_bounds_compatible(
                        source_bounds_mm, carrier_bounds_mm, error))
                    throw std::runtime_error(error);

                begin_stage(NativePortrait::BuildStage::CarrierPublish,
                    "Publish native color-boundary carrier.");
                if (!NativePortrait::write_native_portrait_carrier(
                        destination, *partition, source_bounds_mm, output_hash, error))
                    throw std::runtime_error(error.empty() ? "原生人像颜色载体写出失败。" : error);
                if (AI::model_artifact_sha256(destination) != output_hash)
                    throw std::runtime_error("原生人像颜色载体校验失败。");
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);
                if (stopped()) throw std::runtime_error("原生人像分区已取消。");

                begin_stage(NativePortrait::BuildStage::AppearancePublish,
                    "Publish immutable original-appearance carrier.");
                OriginalAppearanceReference inherited;
                // The immutable GLB remains the texture/material source for
                // partitioning. Geometry mapping must start from the verified
                // appearance carrier, whose topology matches the evidence; a
                // previous repair may legitimately make it differ from the
                // origin GLB.
                if (!prepare_original_appearance_for_candidate(
                        source_appearance.carrier_model, destination,
                        source_appearance.carrier_sha256,
                        source_appearance, appearance_carrier, appearance_root, inherited, error,
                        stopped, true))
                    throw std::runtime_error(error.empty() ? "原始外观无法映射到原生分区。" : error);
                const std::optional<OriginalAppearanceReference> appearance_override {inherited};
                if (!ModelPreview3D::prepare_model(destination, *prepared, error, {}, {}, false,
                        stopped, nullptr, appearance_override))
                    throw std::runtime_error(error.empty() ? "原生分区预览准备失败。" : error);
                if (!prepared->semantic_source || prepared->geometry_id != partition->geometry_id)
                    throw std::runtime_error("原生分区载体与原始外观身份不匹配。");
                if (!NativePortrait::native_workbench_bounds_compatible(
                        source_bounds_mm,
                        NativePortrait::native_workbench_bounds(prepared->mesh), error))
                    throw std::runtime_error(error.empty()
                        ? "原生分区预览尺寸与原模型不一致。" : error);
                if (AI::model_artifact_sha256(appearance_carrier) != inherited.carrier_sha256)
                    throw std::runtime_error("原始外观载体校验失败。");
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);

                begin_stage(NativePortrait::BuildStage::CachePublish,
                    "Publish content-addressed native partition identity.");
                const auto cache_reference = NativePortrait::save_native_portrait_partition_cache(
                    *partition, cache_root);
                const auto cache_sha256 = cache_reference.at("content_id").get<std::string>();
                const auto cache_path = cache_root / (cache_sha256 + ".json");
                if (AI::model_artifact_sha256(cache_path) != cache_sha256)
                    throw std::runtime_error("原生人像分区缓存校验失败。");
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);

                const auto appearance_id = prepared->semantic_source->content_id;
                if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(appearance_id))
                    throw std::runtime_error("原始外观身份无效。");
                topology_certificate = {{"schema", "orca.native-workbench-topology-certificate/v1"},
                    {"geometry_id", partition->geometry_id}, {"carrier_sha256", output_hash},
                    {"partition_digest", partition->partition_digest},
                    {"native_mesh_sha256", cache_reference.at("native_mesh_sha256")},
                    {"coordinate_units", "mm"},
                    {"source_bounds_mm", NativePortrait::encode_native_workbench_bounds(source_bounds_mm)},
                    {"carrier_bounds_mm", NativePortrait::encode_native_workbench_bounds(carrier_bounds_mm)},
                    {"topology", portrait_topology_report(report->strict_repair.output_topology)},
                    {"sampled_deviation_ratio", report->strict_repair.sampled_deviation_ratio},
                    {"area_change_ratio", report->strict_repair.area_change_ratio}};
                topology_hash = AI::beauty_leaf_digest(topology_certificate.dump());
                if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(topology_hash))
                    throw std::runtime_error("原生人像拓扑证书无法生成。");
                *reference = {{"schema", "orca.native-workbench-partition-reference/v2"},
                    {"algorithm", NativePortrait::algorithm_version},
                    {"partition_digest", partition->partition_digest}, {"topology_hash", topology_hash},
                    {"coordinate_units", "mm"},
                    {"source_bounds_mm", NativePortrait::encode_native_workbench_bounds(source_bounds_mm)},
                    {"carrier_bounds_mm", NativePortrait::encode_native_workbench_bounds(carrier_bounds_mm)},
                    {"source_path", u8(source)}, {"source_sha256", source_hash},
                    {"carrier_path", u8(destination)}, {"carrier_sha256", output_hash},
                    {"appearance_path", u8(appearance_carrier)},
                    {"appearance_sha256", inherited.carrier_sha256},
                    {"cache_path", u8(cache_path)}, {"cache_sha256", cache_sha256},
                    {"metadata_path", u8(metadata_path)}, {"geometry_id", partition->geometry_id},
                    {"appearance_id", appearance_id}, {"cache_reference", cache_reference}};
                if (!PP::valid_native_partition_reference(*reference,
                        partition->partition_digest, topology_hash))
                    throw std::runtime_error("原生人像分区引用无效。");
                prepared->native_portrait_partition = partition;
                prepared->native_workbench_partition_reference = *reference;
                prepared->prepare_render_geometry(stopped, share_exact_vertices);
                if (stopped()) throw std::runtime_error("原生人像分区已取消。");
                if (AI::model_artifact_sha256(source) != source_hash ||
                    AI::model_artifact_sha256(destination) != output_hash)
                    throw std::runtime_error("原生分区发布期间模型身份发生变化。");

                begin_stage(NativePortrait::BuildStage::MetadataPublish,
                    "Atomically publish native partition commit metadata.");
                const auto metadata_document = [&](const char* transaction_status,
                                                     const NativePortrait::NativePortraitPartitionBuildReport& build_report) {
                    nlohmann::json document {{"schema", "orca.native-workbench-partition-commit/v1"},
                        {"schema_version", 6}, {"transaction_status", transaction_status},
                        {"job_id", asset_id}, {"portrait_print_id", id},
                        {"requesting_portrait_print_id", id},
                        {"source", "native_workbench_partition"},
                        {"source_model", u8(source)}, {"source_sha256", source_hash},
                        {"model_path", u8(destination)}, {"model_sha256", output_hash},
                        {"generated_at", std::time(nullptr)},
                        {"triangle_count", partition->painted.indices.size()},
                        {"native_portrait_partition", *reference},
                        {"native_portrait_topology_certificate", topology_certificate},
                        {"partition_build_report",
                            NativePortrait::encode_native_portrait_partition_build_report(build_report)},
                        {"source_check", source_check},
                        {"original_appearance", inherited.encode(appearance_root)}};
                    const auto check = portrait_partition_check_result(*partition, build_report, output_hash);
                    document["workbench_check_certificate"] =
                        PP::encode_workbench_check_certificate(check,
                            WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                            partition->geometry_id, appearance_id, u8(metadata_path), document,
                            partition->partition_digest, topology_hash);
                    return document;
                };
                // Measure and validate a same-directory staging write. The
                // authoritative path is still absent, so only the later atomic
                // publication can become the commit marker.
                const auto staging_path = metadata_path.parent_path() /
                    boost::filesystem::unique_path("native-partition-%%%%-%%%%.tmp");
                NativePortrait::write_validate_remove_native_portrait_partition_staging(
                    staging_path, metadata_document("staging", *report));

                // The commit record carries a precommit timing snapshot. The
                // exact atomic publication and read-back duration is finalized
                // in the flow checkpoint and log only after this call returns.
                auto commit_report = *report;
                auto& commit_stage = commit_report.stage(NativePortrait::BuildStage::MetadataPublish);
                commit_stage.status = NativePortrait::BuildStageStatus::Succeeded;
                commit_stage.duration_ms += portrait_elapsed_ms(active_started);
                commit_stage.detail =
                    "Commit bytes staged and validated before the atomic marker was published.";
                commit_report.success = true;
                commit_report.error.clear();
                commit_report.total_ms = portrait_elapsed_ms(*report_started);
                const auto metadata = metadata_document("committed", commit_report);
                *commit_document = metadata;
                *workbench_certificate = metadata.at("workbench_check_certificate");
                NativePortrait::publish_native_portrait_partition_commit(
                    metadata_path, destination, source, metadata);
                finish_stage(NativePortrait::BuildStageStatus::Succeeded);
                auto& metadata_stage = report->stage(NativePortrait::BuildStage::MetadataPublish);
                metadata_stage.detail =
                    "Atomic commit marker published and read back before preview activation.";
                report->success = true;
                report->error.clear();
                report->total_ms = portrait_elapsed_ms(*report_started);
                committed->store(true);
            } catch (const std::exception& failure) {
                finish_stage(NativePortrait::BuildStageStatus::Failed);
                if (!committed->load()) {
                    auto& metadata = report->stage(NativePortrait::BuildStage::MetadataPublish);
                    if (metadata.status == NativePortrait::BuildStageStatus::Succeeded) {
                        metadata.status = NativePortrait::BuildStageStatus::Failed;
                        metadata.detail = "Final commit publication or read-back validation failed.";
                    }
                }
                error = failure.what();
                report->success = false;
                report->error = error;
                report->total_ms = portrait_elapsed_ms(*report_started);
                cleanup();
            }
            wxGetApp().CallAfter([weak, context, edit_revision,
                destination, metadata_path, appearance_carrier, canceled, committed, prepared,
                partition, report, reference, workbench_certificate, commit_document,
                output_hash, report_started, activation_before,
                error = std::move(error)]() mutable {
                const auto cleanup = [&] {
                    if (committed->load()) return;
                    boost::system::error_code ignored;
                    boost::filesystem::remove(destination, ignored);
                    boost::filesystem::remove(appearance_carrier, ignored);
                    boost::filesystem::remove(metadata_path, ignored);
                };
                if (!weak) { cleanup(); return; }
                auto* self = weak.get();
                if (self->m_native_workbench_partition_worker.joinable()) self->m_native_workbench_partition_worker.join();
                if (self->m_native_workbench_partition_cancel != canceled) { cleanup(); return; }
                const auto consumers = self->m_native_workbench_partition_consumers;
                const bool automatic_requested =
                    (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::AutomaticPalette)) != 0;
                const bool portrait_requested =
                    (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::PortraitFlow)) != 0;
                const bool honor_suite_requested =
                    (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::HonorSuite)) != 0;
                self->m_native_workbench_partition_running = false;
                self->m_native_workbench_partition_cancel.reset();
                self->m_workbench_check_card_visible = false;
                self->m_busy = false;
                if (self->m_shutdown || canceled->load() ||
                    !self->native_workbench_context_matches(context) ||
                    self->m_model_preview->leaf_edit_revision() != edit_revision) {
                    report->total_ms = portrait_elapsed_ms(*report_started);
                    log_native_portrait_partition_report(*report, "discarded");
                    cleanup();
                    self->m_native_workbench_partition_consumers = 0;
                    self->m_native_workbench_partition_context = {};
                    if (honor_suite_requested) {
                        self->m_honor_suite_partition_pending = false;
                        self->m_honor_suite_partition_resume_queued = false;
                    }
                    self->refresh_controls();
                    self->refresh_post_generation_workbench();
                    self->publish_workbench_state();
                    return;
                }
                if (!error.empty() || !partition->valid()) {
                    cleanup();
                    report->total_ms = portrait_elapsed_ms(*report_started);
                    log_native_portrait_partition_report(*report, "failed");
                    if (portrait_requested && self->m_portrait_print.partition_pending) {
                        self->m_portrait_print.fail_partition(
                            NativePortrait::encode_native_portrait_partition_build_report(*report));
                        self->save_portrait_print();
                    }
                    const auto failure = "Orca 原生工作台分区未完成，原文件和旧成果均已保留：" +
                        (error.empty() ? std::string("分区身份无效") : error);
                    if (portrait_requested && self->m_portrait_print.active())
                        self->pause_portrait_print(failure);
                    else {
                        self->m_workbench_operation_notice = failure;
                        if (automatic_requested) self->clear_pending_workbench_ai_palette();
                        self->publish_workbench_state();
                    }
                    if (honor_suite_requested) {
                        self->m_honor_suite_partition_pending = false;
                        self->m_honor_suite_partition_resume_queued = false;
                    }
                    self->m_native_workbench_partition_consumers = 0;
                    self->m_native_workbench_partition_context = {};
                    self->refresh_controls();
                    self->refresh_post_generation_workbench();
                    self->publish_workbench_state();
                    return;
                }
                const auto view = self->m_model_preview->view_state();
                size_t triangles = 0, colors = 0;
                Vec3d dimensions;
                std::string load_error;
                const auto preview_started = std::chrono::steady_clock::now();
                self->invalidate_printer_filament_trial_for_model_change();
                if (!self->m_model_preview->load_prepared_model(
                        std::move(*prepared), {}, triangles, dimensions, colors, load_error)) {
                    auto& preview = report->stage(NativePortrait::BuildStage::PreviewSwitch);
                    preview.status = NativePortrait::BuildStageStatus::Failed;
                    preview.duration_ms += portrait_elapsed_ms(preview_started);
                    preview.detail = load_error;
                    report->success = false;
                    report->error = load_error;
                    report->total_ms = portrait_elapsed_ms(*report_started);
                    log_native_portrait_partition_report(*report, "preview-failed");
                    const auto failure =
                        "Orca 原生工作台分区预览未加载，原文件和旧成果均已保留：" + load_error;
                    if (portrait_requested && self->m_portrait_print.active()) {
                        self->m_portrait_print.partition_build_report =
                            NativePortrait::encode_native_portrait_partition_build_report(*report);
                        self->save_portrait_print();
                        self->pause_portrait_print(failure);
                    } else {
                        self->m_workbench_operation_notice = failure;
                        if (automatic_requested) self->clear_pending_workbench_ai_palette();
                        self->publish_workbench_state();
                    }
                    if (honor_suite_requested) {
                        self->m_honor_suite_partition_pending = false;
                        self->m_honor_suite_partition_resume_queued = false;
                    }
                    self->m_native_workbench_partition_consumers = 0;
                    self->m_native_workbench_partition_context = {};
                    self->refresh_controls();
                    self->refresh_post_generation_workbench();
                    self->publish_workbench_state();
                    return;
                }
                self->m_model_preview->restore_view(view);
                auto& preview = report->stage(NativePortrait::BuildStage::PreviewSwitch);
                preview.status = NativePortrait::BuildStageStatus::Succeeded;
                preview.duration_ms += portrait_elapsed_ms(preview_started);
                preview.detail = "Committed carrier loaded for transactional consumer activation.";
                report->success = true;
                report->error.clear();
                report->total_ms = portrait_elapsed_ms(*report_started);
                bool automatic_applied = true;
                if (automatic_requested) {
                    automatic_applied = self->m_model_preview->activate_current_ai_palette(
                        int(NativePortrait::default_model_color_count));
                }
                bool portrait_adopted = !portrait_requested;
                if (automatic_applied && portrait_requested) {
                    // Adoption validates the exact displayed owner. Publish
                    // only the minimal temporary identity; all broad panel
                    // state remains unchanged until every consumer succeeds.
                    self->m_displayed_model_path = destination;
                    self->m_native_workbench_partition_reference = *reference;
                    self->m_native_workbench_partition_report =
                        NativePortrait::encode_native_portrait_partition_build_report(*report);
                    self->m_native_workbench_partition_certificate = *workbench_certificate;
                    self->m_native_workbench_partition_commit = *commit_document;
                    self->m_native_workbench_context = {destination, output_hash,
                        self->m_model_preview->geometry_id(),
                        self->m_model_preview->semantic_source_content_id(), self->m_sequence};
                    portrait_adopted =
                        self->bind_native_workbench_partition_commit_to_portrait() &&
                        self->adopt_native_workbench_partition_for_portrait();
                    if (portrait_adopted) {
                        *commit_document = self->m_native_workbench_partition_commit;
                        *workbench_certificate = self->m_native_workbench_partition_certificate;
                    }
                }

                if (!automatic_applied || !portrait_adopted) {
                    preview.status = NativePortrait::BuildStageStatus::Failed;
                    preview.detail = !automatic_applied
                        ? "Native partition loaded, but the requested six-colour automatic mapping failed; activation rolled back."
                        : "Native partition loaded, but portrait ownership adoption failed; activation rolled back.";
                    report->success = false;
                    report->error = preview.detail;
                    report->total_ms = portrait_elapsed_ms(*report_started);
                    self->m_displayed_model_path = activation_before->displayed_model_path;
                    self->m_displayed_model_job_id = activation_before->displayed_model_job_id;
                    const bool preview_restored = self->restore_beauty_candidate(activation_before->beauty);
                    self->m_native_workbench_context = context;
                    self->m_native_workbench_partition_reference = activation_before->partition_reference;
                    self->m_native_workbench_partition_report = activation_before->partition_report;
                    self->m_native_workbench_partition_certificate = activation_before->partition_certificate;
                    self->m_native_workbench_partition_commit = activation_before->partition_commit;
                    self->m_workbench_check_result = activation_before->workbench_check;
                    self->m_workbench_check_path = activation_before->workbench_check_path;
                    self->m_workbench_check_revision = activation_before->workbench_check_revision;
                    if (portrait_requested) {
                        self->m_portrait_print = activation_before->portrait_print;
                        self->m_portrait_verified_check = activation_before->portrait_verified_check;
                        if (!self->save_portrait_print())
                            BOOST_LOG_TRIVIAL(error) << "Failed to persist portrait flow after native activation rollback.";
                    }
                    if (automatic_requested) self->clear_pending_workbench_ai_palette();
                    if (honor_suite_requested) {
                        self->m_honor_suite_partition_pending = false;
                        self->m_honor_suite_partition_resume_queued = false;
                    }
                    self->m_native_workbench_partition_consumers = 0;
                    self->m_native_workbench_partition_context = {};
                    log_native_portrait_partition_report(*report,
                        preview_restored ? "activation-rolled-back" : "activation-rollback-failed");
                    const std::string failure = !automatic_applied
                        ? "原生原子分区缓存已保留，但 6 色 AI配色应用失败；模型与配色已回滚。"
                        : "原生原子分区缓存已保留，但人物流程身份绑定失败；模型与配色已回滚。";
                    if (portrait_requested && self->m_portrait_print.active())
                        self->pause_portrait_print(preview_restored ? failure :
                            failure + " 旧预览恢复失败，请重新打开原模型。");
                    else {
                        self->m_workbench_operation_notice = preview_restored ? failure :
                            failure + " 旧预览恢复失败，请重新打开原模型。";
                        self->publish_workbench_state();
                    }
                    self->refresh_controls();
                    self->refresh_post_generation_workbench();
                    self->publish_workbench_state();
                    return;
                }

                self->select_local_finishing_version(destination,
                    "native-workbench-partition-" + partition->partition_digest.substr(0, 16));
                self->m_model_preview_ready = true;
                self->m_model_stats->SetLabel(wxString::Format(
                    _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                    static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
                    dimensions.x(), dimensions.y(), dimensions.z()));
                preview.detail = "Committed carrier and all requested consumers activated transactionally.";
                report->total_ms = portrait_elapsed_ms(*report_started);
                log_native_portrait_partition_report(*report, "succeeded");
                self->m_native_workbench_partition_reference = *reference;
                self->m_native_workbench_partition_report =
                    NativePortrait::encode_native_portrait_partition_build_report(*report);
                self->m_native_workbench_partition_certificate = *workbench_certificate;
                self->m_native_workbench_partition_commit = *commit_document;
                self->m_native_workbench_context = {destination, output_hash,
                    self->m_model_preview->geometry_id(),
                    self->m_model_preview->semantic_source_content_id(), self->m_sequence};
                self->m_workbench_check_result = portrait_partition_check_result(
                    *partition, *report, output_hash);
                self->m_workbench_check_path = destination.string();
                self->m_workbench_check_revision = self->m_sequence;
                if (automatic_requested) self->clear_pending_workbench_ai_palette();
                self->m_native_workbench_partition_consumers = 0;
                self->m_native_workbench_partition_context = {};
                if (honor_suite_requested)
                    self->queue_honor_suite_after_native_partition();
                self->refresh_controls();
                self->refresh_post_generation_workbench();
                self->publish_workbench_state();
            });
        });
    } catch (const std::exception& failure) {
        const auto consumers = m_native_workbench_partition_consumers;
        const bool automatic_requested =
            (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::AutomaticPalette)) != 0;
        const bool portrait_requested =
            (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::PortraitFlow)) != 0;
        const bool honor_suite_requested =
            (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::HonorSuite)) != 0;
        m_native_workbench_partition_running = false;
        m_native_workbench_partition_cancel.reset();
        m_native_workbench_partition_consumers = 0;
        m_native_workbench_partition_context = {};
        if (honor_suite_requested) {
            m_honor_suite_partition_pending = false;
            m_honor_suite_partition_resume_queued = false;
        }
        m_busy = false;
        report->success = false;
        report->error = failure.what();
        report->total_ms = portrait_elapsed_ms(*report_started);
        log_native_portrait_partition_report(*report, "startup-failed");
        if (portrait_requested && m_portrait_print.partition_pending) {
            m_portrait_print.fail_partition(
                NativePortrait::encode_native_portrait_partition_build_report(*report));
            save_portrait_print();
        }
        const auto message = "Orca 原生工作台分区未能启动，原文件保持不变：" +
            std::string(failure.what());
        if (portrait_requested && m_portrait_print.active())
            pause_portrait_print(message);
        else {
            if (automatic_requested) clear_pending_workbench_ai_palette();
            m_workbench_operation_notice = message;
        }
        refresh_controls();
        refresh_post_generation_workbench();
        publish_workbench_state();
        return false;
    }
    return true;
}

void ModelGenerationPanel::run_portrait_print()
{
    const bool owns_processing_context = portrait_owns_display_context() ||
        (m_portrait_print.stage == PP::Stage::SaveColor && portrait_owns_save_candidate());
    if (!m_portrait_print.running() || !m_portrait_print.from_workbench() ||
        !owns_processing_context || m_portrait_advancing || !m_page_initialized) return;
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
        m_workbench_operation_notice = "正在核验模型身份和拓扑风险；几何处理仅在后续 Orca 原生分区管线内执行。";
        if (m_workbench_check_path != m_displayed_model_path.string() ||
            m_workbench_check_revision != m_sequence || m_workbench_check_result.status == WorkbenchCheckStatus::NotRun) {
            if (!request_check_workbench(true)) pause_portrait_print("模型身份与风险检查未能启动，当前模型保留。");
            return;
        }
        if (m_workbench_check_result.status == WorkbenchCheckStatus::Failed ||
            m_workbench_check_result.model_sha256 != m_portrait_print.model_hash ||
            m_workbench_check_result.candidate_status != RepairCandidateStatus::None ||
            !m_finishing_candidate.empty()) {
            pause_portrait_print("模型身份或风险检查未通过，原文件保持不变。" ); return;
        }
        if (m_portrait_print.source_checked) {
            if (m_portrait_print.source_check_hash != m_portrait_print.model_hash) {
                pause_portrait_print("来源检查与当前模型身份不匹配，未开始原生分区。"); return;
            }
            advance_portrait_print(id, stage, PP::Stage::Partition);
            return;
        }
        auto source_report = portrait_source_check_report(m_workbench_check_result);
        try {
            source_report["workbench_check_certificate"] =
                PP::encode_workbench_check_certificate(m_workbench_check_result,
                    WorkbenchCheckPurpose::PortraitSourceRisk,
                    m_portrait_print.geometry_id, m_portrait_print.appearance_id,
                    {}, source_report);
        } catch (const std::exception& failure) {
            pause_portrait_print("来源风险检查证书无法生成，原模型保持不变：" +
                std::string(failure.what()));
            return;
        }
        if (m_portrait_print.accept_source_check(m_portrait_print.model_hash,
                std::move(source_report))) {
            advance_portrait_print(id, stage, PP::Stage::Partition);
        }
        return;
    }
    if (stage == PP::Stage::Partition) {
        if (m_native_workbench_partition_running) return;
        if (m_portrait_print.model_color_count != int(NativePortrait::portrait_model_color_count) ||
            m_portrait_print.physical_slot_count != int(honor_people_suite().physical_slots.size()) ||
            m_portrait_print.support_slot_count != 1) {
            pause_portrait_print("人物六色流程的 5 个模型槽和 1 个支撑槽身份无效，未建立分区。");
            return;
        }
        if (m_model_preview->has_native_portrait_partition_conflict() ||
            (m_beauty_controls && m_beauty_controls->has_changes())) {
            pause_portrait_print("检测到手工面色、子面或轮廓编辑；原编辑已保留，未作为 v11 原生分区输入。");
            return;
        }
        const auto consumers = PP::native_workbench_partition_consumers(
            NativeWorkbenchPartitionConsumer::PortraitFlow,
            NativeWorkbenchPartitionConsumer::AutomaticPalette);
        if (!ensure_native_workbench_partition(consumers) &&
            !m_portrait_print.paused)
            pause_portrait_print("Orca 原生工作台分区未能启动，当前模型保持不变。");
        return;
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
        if (!persist_portrait_workbench_check_certificate()) {
            pause_portrait_print("已保存配色模型的检查证书无法持久化，原成果保持不变。"); return;
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
    if (!portrait_color_save_snapshot_current() && m_finishing_candidate.empty() &&
        !m_portrait_export_started && portrait_owns_display_context()) {
        m_portrait_save_draft = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        m_portrait_save_draft->source = m_displayed_model_path;
        m_portrait_save_draft->candidate.clear();
        m_portrait_save_draft->model_sha256 = m_portrait_print.model_hash;
        m_portrait_save_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
        m_portrait_save_flow_id = m_portrait_print.id;
        m_portrait_save_sequence = m_sequence;
        m_portrait_save_edit_revision = m_model_preview->leaf_edit_revision();
        m_portrait_save_was_editing = m_workbench_editing;
        m_portrait_save_task = m_portrait_task && m_portrait_task->snapshot().running()
            ? m_portrait_task : nullptr;
        m_portrait_save_files.clear();
    }
    if (!m_finishing_candidate.empty()) {
        if (!m_finishing_result.success) {
            rollback_portrait_color_save("portrait export result failed: " + m_finishing_result.error); return;
        }
        if (m_finishing_result.source_sha256 != m_portrait_print.model_hash) {
            rollback_portrait_color_save("portrait export source identity mismatch"); return;
        }
        if (!portrait_owns_save_candidate()) {
            rollback_portrait_color_save("portrait export owner identity mismatch"); return;
        }
        if (m_model_preview->printer_filament_export_faces().empty() &&
            m_model_preview->import_subface_color_overrides(true).empty()) {
            rollback_portrait_color_save("portrait export has no verifiable colors"); return;
        }
        accept_model_finishing(); return;
    }
    if (m_portrait_export_started) { rollback_portrait_color_save("portrait export completed without a candidate"); return; }
    m_portrait_color_candidate_identity = {};
    m_portrait_export_started = true;
    export_semantic_candidate();
}

bool ModelGenerationPanel::portrait_color_published(const boost::filesystem::path& candidate,
    const WorkbenchCheckResult& final_check)
{
    const auto id = m_portrait_print.id;
    if (!m_portrait_print.current(id, PP::Stage::SaveColor) ||
        !portrait_owns_save_candidate()) return false;
    const auto exported_faces = m_model_preview->printer_filament_export_faces();
    const auto exported_subfaces = m_model_preview->import_subface_color_overrides(true);
    if (m_finishing_candidate != candidate || !m_model_preview->showing_model(candidate) ||
        m_finishing_result.output_sha256.empty() || AI::model_artifact_sha256(candidate) != m_finishing_result.output_sha256 ||
        m_model_preview->geometry_id() != m_portrait_print.geometry_id ||
        exported_faces.size() != m_finishing_result.faces_after || !exported_subfaces.empty() ||
        !workbench_topology_safe(final_check) ||
        final_check.model_sha256 != m_finishing_result.output_sha256) {
        BOOST_LOG_TRIVIAL(error) << "portrait publication identity mismatch"; return false;
    }
    const auto previous_flow = m_portrait_print;
    const auto previous_check = m_workbench_check_result;
    const auto previous_check_path = m_workbench_check_path;
    const auto previous_check_revision = m_workbench_check_revision;
    const auto previous_color_snapshot = m_portrait_color_snapshot;
    const auto previous_color_revision = m_portrait_color_edit_revision;
    try {
        boost::filesystem::ifstream input(AI::beauty_metadata_path(candidate, ModelGenerationPresentation::generated_models_root()));
        const auto metadata = nlohmann::json::parse(input);
        if (metadata.at("model_sha256") != m_portrait_print.model_hash ||
            metadata.at("portrait_print_id") != id)
            throw std::runtime_error("配色成果与流程身份不匹配");
        const auto& partition_reference = metadata.at("native_portrait_partition");
        const auto& region_semantics = metadata.at("region_semantics");
        const auto& native_color_reduction = metadata.at("native_color_reduction");
        const auto& final_slot_binding = metadata.at("final_slot_binding");
        const auto& post_color_check = metadata.at("post_color_check");
        const auto& workbench_certificate = metadata.at("workbench_check_certificate");
        const auto& certificate = metadata.at("native_portrait_topology_certificate");
        const auto& build_report = metadata.at("partition_build_report");
        if (!PP::valid_native_partition_reference(partition_reference,
                m_portrait_print.basis_signature, m_portrait_print.partition_topology_hash) ||
            !PP::same_model_path(u8(candidate),
                partition_reference.at("carrier_path").get<std::string>()) ||
            partition_reference.at("carrier_sha256") != m_finishing_result.output_sha256 ||
            partition_reference.at("appearance_id") !=
                m_model_preview->semantic_source_content_id() ||
            !PP::valid_region_semantics(region_semantics, m_portrait_print.basis_signature) ||
            !PP::valid_native_color_reduction(native_color_reduction,
                m_portrait_print.basis_signature, region_semantics.at("regions").size()) ||
            !PP::valid_final_slot_binding(final_slot_binding,
                m_portrait_print.basis_signature,
                native_color_reduction.at("mapping_digest").get<std::string>(),
                region_semantics.at("regions").size(),
                size_t(m_portrait_print.model_color_count)) ||
            !PP::valid_post_color_check(post_color_check, partition_reference) ||
            !PP::valid_workbench_check_certificate(workbench_certificate,
                WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                m_finishing_result.output_sha256,
                partition_reference.at("geometry_id").get<std::string>(),
                partition_reference.at("appearance_id").get<std::string>(),
                u8(AI::beauty_metadata_path(candidate,
                    ModelGenerationPresentation::generated_models_root())), metadata,
                m_portrait_print.basis_signature, m_portrait_print.partition_topology_hash) ||
            AI::beauty_leaf_digest(certificate.dump()) !=
                partition_reference.at("topology_hash") ||
            certificate.at("carrier_sha256") != partition_reference.at("carrier_sha256") ||
            certificate.at("geometry_id") != partition_reference.at("geometry_id") ||
            certificate.at("partition_digest") != partition_reference.at("partition_digest") ||
            build_report.value("schema", std::string{}) !=
                "orca.native-workbench-partition-build-report/v1" ||
            !build_report.value("success", false))
            throw std::runtime_error("原生分区、拓扑证书或着色后检查无效");

        WorkbenchCheckResult certified_check;
        auto expected_check = final_check;
        expected_check.purpose = WorkbenchCheckPurpose::PortraitPostColorReadOnly;
        if (!PP::decode_workbench_check_certificate(workbench_certificate, certified_check) ||
            !same_check_evidence(certified_check, expected_check))
            throw std::runtime_error("着色后检查证书与本次只读复检不一致");

        NativePortrait::NativePortraitPartitionIdentity independent_identity;
        std::string cache_error;
        if (!NativePortrait::load_native_portrait_partition_cache(
                partition_reference.at("cache_reference"),
                boost::filesystem::path(Slic3r::data_dir()) / "cache" /
                    "native-workbench-partitions",
                independent_identity, cache_error))
            throw std::runtime_error(cache_error);
        const auto native_partition = m_model_preview->native_portrait_partition();
        const auto color_blocks = m_model_preview->portrait_color_blocks();
        if (!native_partition || !color_blocks ||
            native_partition->partition_digest != independent_identity.partition_digest ||
            !color_blocks->native_partition_identity ||
            NativePortrait::encode_native_portrait_partition_identity(independent_identity) !=
                NativePortrait::encode_native_portrait_partition_identity(
                    *color_blocks->native_partition_identity) ||
            PortraitColorBlocks::encode(*color_blocks).at("region_semantics") != region_semantics ||
            PortraitColorBlocks::encode(*color_blocks).at("native_color_reduction") != native_color_reduction ||
            PortraitColorBlocks::encode(*color_blocks).at("final_slot_binding") != final_slot_binding)
            throw std::runtime_error("当前预览未恢复独立原生分区、减色映射或整区槽位绑定");

        const auto& layer = metadata.at("portrait_applied_color_layer");
        const auto& color_reference = layer.at("reference");
        if (layer.at("schema") != "orca.portrait-applied-color-layer/v1" ||
            color_reference != metadata.at("portrait_color_blocks") ||
            color_reference.at("geometry_id") != m_portrait_print.geometry_id ||
            layer.at("painted_faces").get<size_t>() != m_finishing_result.faces_after ||
            layer.at("painted_subfaces").get<size_t>() != 0)
            throw std::runtime_error("完整逐面颜色层未保存");
        if (metadata.contains("applied_printer_filament_match")) {
            AI::ModelMatchedColors matched;
            std::string matched_error;
            if (!AI::decode_model_matched_colors(
                    metadata.at("applied_printer_filament_match"), matched, matched_error) ||
                !matched.subface_slots.empty() ||
                matched.face_slots.size() != m_finishing_result.faces_after)
                throw std::runtime_error("人物 v9 耗材槽位不是完整逐面绑定");
        }
        if (!m_portrait_print.commit_colored_carrier(partition_reference, region_semantics,
                native_color_reduction, final_slot_binding, post_color_check,
                u8(candidate), m_finishing_result.output_sha256,
                m_model_preview->geometry_id(),
                m_model_preview->semantic_source_content_id(), workbench_certificate))
            throw std::runtime_error("配色成果无法提交到当前流程身份");
        m_portrait_print.partition_build_report = build_report;
        m_portrait_print.region_binding_digest =
            color_reference.at("content_id").get<std::string>();
    } catch (const std::exception& e) {
        m_portrait_print = previous_flow;
        BOOST_LOG_TRIVIAL(error) << "portrait publication validation: " << e.what();
        return false;
    }
    const auto state = m_model_preview->color_trial_state();
    m_portrait_print.base_binding = state.base_binding ? state.base_binding->encode() : nlohmann::json();
    m_portrait_color_snapshot = m_model_preview->color_trial_metadata();
    m_portrait_color_edit_revision = m_model_preview->leaf_edit_revision();
    m_workbench_check_result = final_check;
    m_workbench_check_result.purpose = WorkbenchCheckPurpose::PortraitPostColorReadOnly;
    m_workbench_check_path = candidate.string();
    m_workbench_check_revision = m_sequence;
    try {
        if (!m_portrait_print.advance(id, PP::Stage::SaveColor, PP::Stage::Base))
            throw std::runtime_error("Portrait save stage could not advance");
        PP::commit_save_ledger({flow_root() / (id + ".json"), flow_root() / "current.json"},
            PP::encode(m_portrait_print), atomic_json);
    } catch (const std::exception& failure) {
        BOOST_LOG_TRIVIAL(error) << "portrait publication ledger: " << failure.what();
        m_portrait_print = previous_flow;
        m_workbench_check_result = previous_check;
        m_workbench_check_path = previous_check_path;
        m_workbench_check_revision = previous_check_revision;
        m_portrait_color_snapshot = previous_color_snapshot;
        m_portrait_color_edit_revision = previous_color_revision;
        return false;
    }
    publish_ui_state();
    return true;
}
} // namespace Slic3r::GUI
