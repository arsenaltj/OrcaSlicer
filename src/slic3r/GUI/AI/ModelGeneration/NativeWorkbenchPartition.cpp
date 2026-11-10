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
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
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
namespace PP = WorkbenchArtifacts;
namespace {
std::string u8(const boost::filesystem::path& path) { return wxString(path.wstring()).ToStdString(wxConvUTF8); }

boost::filesystem::path path8(const std::string& path) { return boost::filesystem::path(wxString::FromUTF8(path).ToStdWstring()); }

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
};
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
    if (!partition || !partition->valid()) return false;
    const auto& preview_reference = m_model_preview->native_workbench_partition_reference();
    // A newly published candidate was loaded before its metadata existed.
    // Resolve its current record, then validate it against the live file and
    // partition exactly as on reopen. Never carry the previous model's proof.
    const auto metadata_path = preview_reference.is_null()
        ? AI::beauty_metadata_path(m_native_workbench_context.model_path,
            ModelGenerationPresentation::generated_models_root())
        : PP::valid_native_partition_reference(preview_reference, partition->partition_digest)
            ? path8(preview_reference.at("metadata_path").get<std::string>())
            : boost::filesystem::path{};
    nlohmann::json metadata;
    nlohmann::json verified_reference;
    if (metadata_path.empty() || !read_portrait_check_metadata(metadata_path, metadata)) return false;
    const auto evidence = workbench_history_evidence_location(
        m_native_workbench_context.model_path, metadata_path, metadata);
    if (!restorable_native_partition_metadata_matches(metadata, evidence.metadata,
            evidence.model, m_native_workbench_context.source_sha256,
            m_native_workbench_context.geometry_id, m_native_workbench_context.appearance_id,
            verified_reference) ||
        !PP::valid_native_partition_reference(verified_reference, partition->partition_digest) ||
        (!preview_reference.is_null() && verified_reference != preview_reference))
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
            m_native_workbench_context.appearance_id, u8(evidence.metadata), metadata,
            partition->partition_digest,
            verified_reference.at("topology_hash").get<std::string>()))
        return false;

    m_native_workbench_partition_reference = verified_reference;
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
    const bool honor_suite_requested = has_consumer(
        NativeWorkbenchPartitionConsumer::HonorSuite);
    if (consumers == 0) return false;
    if (m_shutdown || !m_model_preview || !m_model_preview_ready ||
        !m_model_preview->showing_model(m_displayed_model_path)) return false;
    if (!native_workbench_context_matches(m_native_workbench_context))
        publish_native_workbench_context();
    if (!native_workbench_context_matches(m_native_workbench_context)) return false;

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

    if (restore_native_workbench_partition_checkpoint()) {
        m_native_workbench_partition_consumers |= consumers;
        bool completed = true;
        if (completed && automatic_requested)
            completed = m_model_preview->activate_current_ai_palette(
                int(NativePortrait::default_model_color_count));

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
            source_appearance_error, asset_id,
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
                        {"source", "native_workbench_partition"},
                        {"source_model", u8(source)}, {"source_sha256", source_hash},
                        {"model_path", u8(destination)}, {"model_sha256", output_hash},
                        {"generated_at", std::time(nullptr)},
                        {"triangle_count", partition->painted.indices.size()},
                        {"native_portrait_partition", *reference},
                        {"native_portrait_topology_certificate", topology_certificate},
                        {"partition_build_report",
                            NativePortrait::encode_native_portrait_partition_build_report(build_report)},
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
                // in the build report and log only after this call returns.
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
                if (self->m_native_workbench_partition_cancel != canceled) { cleanup(); return; }
                if (self->m_native_workbench_partition_worker.joinable()) self->m_native_workbench_partition_worker.join();
                const auto consumers = self->m_native_workbench_partition_consumers;
                const bool automatic_requested =
                    (consumers & std::uint8_t(NativeWorkbenchPartitionConsumer::AutomaticPalette)) != 0;
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
                    if (!self->m_shutdown && self->m_displayed_model_path == context.model_path &&
                        self->m_sequence == context.generation) {
                        if (automatic_requested) self->clear_pending_workbench_ai_palette();
                        self->fail_workbench_check(canceled->load()
                            ? "已取消检查与修复，原模型保留；可重新检查。"
                            : "模型在修复期间已变化，未应用过期结果；请重新检查。");
                    }
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

                    const auto failure = "Orca 原生工作台分区未完成，原文件和旧成果均已保留：" +
                        (error.empty() ? std::string("分区身份无效") : error);
                    self->fail_workbench_check(failure);
                    if (automatic_requested) self->clear_pending_workbench_ai_palette();

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
                    self->fail_workbench_check(failure);
                    if (automatic_requested) self->clear_pending_workbench_ai_palette();

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



                if (!automatic_applied) {
                    preview.status = NativePortrait::BuildStageStatus::Failed;
                    preview.detail = "Native partition loaded, but the requested six-colour automatic mapping failed; activation rolled back.";
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

                    if (automatic_requested) self->clear_pending_workbench_ai_palette();
                    if (honor_suite_requested) {
                        self->m_honor_suite_partition_pending = false;
                        self->m_honor_suite_partition_resume_queued = false;
                    }
                    self->m_native_workbench_partition_consumers = 0;
                    self->m_native_workbench_partition_context = {};
                    log_native_portrait_partition_report(*report,
                        preview_restored ? "activation-rolled-back" : "activation-rollback-failed");
                    const std::string failure = "原生原子分区缓存已保留，但 6 色 AI配色应用失败；模型与配色已回滚。";
                    self->fail_workbench_check(preview_restored ? failure :
                        failure + " 旧预览恢复失败，请重新打开原模型。");

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

        const auto message = "Orca 原生工作台分区未能启动，原文件保持不变：" +
            std::string(failure.what());
        if (automatic_requested) clear_pending_workbench_ai_palette();
        fail_workbench_check(message);

        refresh_controls();
        refresh_post_generation_workbench();
        publish_workbench_state();
        return false;
    }
    return true;
}

bool ModelGenerationPanel::restore_workbench_check_certificate()
{
    if (!m_model_preview || !m_model_preview_ready || m_displayed_model_path.empty() ||
        !m_model_preview->showing_model(m_displayed_model_path) ||
        m_model_preview->model_path() != m_displayed_model_path) return false;
    const auto model_hash = AI::model_artifact_sha256(m_displayed_model_path);
    const auto geometry_id = m_model_preview->geometry_id();
    const auto semantic_appearance_id = m_model_preview->semantic_source_content_id();
    const auto appearance_id = semantic_appearance_id.empty() ? model_hash : semantic_appearance_id;
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



    const auto metadata_path = AI::beauty_metadata_path(
        m_displayed_model_path, ModelGenerationPresentation::generated_models_root());
    nlohmann::json metadata;
    if (!read_portrait_check_metadata(metadata_path, metadata)) return false;
    const auto evidence = workbench_history_evidence_location(m_displayed_model_path, metadata_path, metadata);
    const auto embedded = metadata.value("workbench_check_certificate", nlohmann::json{});
    if (!metadata.contains("native_portrait_partition") &&
        metadata.value("source", std::string{}) == "local_finishing" &&
        metadata.value("schema_version", 0) == 4 &&
        metadata.value("model_sha256", std::string{}) == model_hash &&
        PP::valid_workbench_check_certificate(embedded, WorkbenchCheckPurpose::SavedModelReadOnly,
            model_hash, geometry_id, appearance_id, u8(evidence.metadata), metadata)) {
        WorkbenchCheckResult restored;
        if (PP::decode_workbench_check_certificate(embedded, restored) &&
            workbench_topology_safe(restored) &&
            metadata.value("triangle_count", size_t(0)) == restored.before_faces)
            return publish(embedded);
        return false;
    }
    const bool require_post_color = metadata.contains("post_color_check");
    nlohmann::json reference;
    if (!portrait_partition_metadata_matches(metadata, evidence.metadata, evidence.model,
            model_hash, geometry_id, appearance_id, require_post_color, reference)) return false;

    const auto certificate_valid = [&](const nlohmann::json& certificate) {
        if (!PP::valid_workbench_check_certificate(certificate,
                WorkbenchCheckPurpose::PortraitPostColorReadOnly, model_hash, geometry_id,
                appearance_id, u8(evidence.metadata), metadata,
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

    return certificate_valid(embedded) && publish(embedded);
}

} // namespace Slic3r::GUI
