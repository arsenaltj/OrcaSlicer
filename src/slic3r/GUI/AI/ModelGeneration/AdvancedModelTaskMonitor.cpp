#include "AdvancedModelTaskMonitor.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <boost/filesystem.hpp>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
AdvancedModelTaskMonitor::AdvancedModelTaskMonitor()
    : m_client(AISidecarClient::default_endpoint()), m_timer(this)
{
    wxWeakRef<AdvancedModelTaskMonitor> weak(this);
    m_flow = std::make_unique<AdvancedModelTaskFlow>(AdvancedModelTaskFlow::Ports{
        [this, weak](const std::string& job, auto complete, auto failed) {
            m_client.get_status(job, [weak, complete](auto status) {
                wxGetApp().CallAfter([weak, complete, status = std::move(status)]() mutable {
                    if (weak) complete(std::move(status));
                });
            }, [weak, failed](auto message) {
                wxGetApp().CallAfter([weak, failed, message = std::move(message)]() mutable {
                    if (weak) failed(std::move(message));
                });
            });
        },
        [this](const auto& status, auto complete, auto failed, auto progress) {
            m_acquisition.start(status, std::move(complete), std::move(failed), std::move(progress));
        },
        [this](auto artifact, const auto& status, const auto& reference, auto complete, auto failed) {
            prepare(std::move(artifact), status, reference, std::move(complete), std::move(failed));
        },
        [this](int delay) { if (delay > 0) m_timer.StartOnce(delay); else m_timer.Stop(); },
        [this] { cancel_requests(); }
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_flow->poll(); }, m_timer.GetId());
}
AdvancedModelTaskMonitor::~AdvancedModelTaskMonitor() { m_flow.reset(); }
void AdvancedModelTaskMonitor::cancel_requests()
{
    m_timer.Stop(); m_client.cancel_current(); m_acquisition.cancel();
    if (m_cancel) m_cancel->store(true);
    if (m_worker.joinable()) m_worker.join();
    m_cancel.reset();
}
void AdvancedModelTaskMonitor::reset() { m_flow->reset(); }
void AdvancedModelTaskMonitor::set_listener(ModelGenerationUIStateListener listener)
{ m_flow->set_listener(std::move(listener)); }
void AdvancedModelTaskMonitor::set_available(bool available) { m_flow->set_available(available); }
void AdvancedModelTaskMonitor::observe(const std::string& job, boost::filesystem::path reference, bool restart)
{ m_flow->observe(job, std::move(reference), restart); }
void AdvancedModelTaskMonitor::retry_local() { m_flow->retry_local(); }

void AdvancedModelTaskMonitor::prepare(AI::GeneratedModelArtifact artifact,
    AIModelGenerationClient::JobStatus status, boost::filesystem::path reference,
    std::function<void(ModelLibraryEntry)> complete, AIModelGenerationClient::ErrorFn failed)
{
    if (m_worker.joinable()) m_worker.join();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = m_cancel;
    wxWeakRef<AdvancedModelTaskMonitor> weak(this);
    m_worker = std::thread([weak, cancel, artifact, status, reference, complete, failed] {
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        ModelLibraryEntry entry;
        try {
            ModelPreview3D::PreparedModel prepared;
            if (ModelPreview3D::prepare_model(artifact.local_path, prepared, error, {}, {}, true,
                    [cancel] { return cancel->load(); })) {
                entry.model_path = artifact.local_path; entry.job_id = artifact.job_id;
                entry.content_sha256 = prepared.artifact_sha256;
                entry.palette = artifact.generation_palette; entry.palette_roles = status.palette_roles;
                entry.reference_image_path = reference; entry.ai_image_path = reference;
                entry.use_printable_colors = artifact.used_printable_colors;
                entry.title = _L("多视角模型");
                entry.triangle_count = prepared.triangles;
                entry.load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (artifact.color_intent_manifest) {
                    entry.color_intent_path = artifact.color_intent_manifest->local_path;
                    entry.color_intent_schema = artifact.color_intent_manifest->schema;
                    entry.color_intent_sha256 = artifact.color_intent_manifest->sha256;
                }
                const auto root = generated_models_root();
                const auto image = archive_library_image(reference, status.id, "reference");
                const auto dimensions = prepared.bounds.size();
                nlohmann::json record{{"schema_version", 6}, {"job_id", status.id}, {"source", status.source},
                    {"model_sha256", prepared.artifact_sha256},
                    {"model_path", entry.model_path.lexically_relative(root).generic_string()},
                    {"style", status.style}, {"custom_style", status.custom_style}, {"prompt", status.user_prompt},
                    {"face_limit", status.face_limit}, {"generation_profile", status.generation_profile},
                    {"provider", status.provider_name}, {"provider_task_id", status.provider_task_id},
                    {"provider_conversion_task_id", status.provider_conversion_task_id},
                    {"geometry_quality", status.generation_options.geometry_quality},
                    {"texture_quality", status.generation_options.texture_quality}, {"output_format", artifact.format},
                    {"palette", entry.palette}, {"palette_roles", entry.palette_roles},
                    {"use_printable_colors", entry.use_printable_colors},
                    {"artifact_size", boost::filesystem::file_size(entry.model_path)},
                    {"triangle_count", prepared.triangles}, {"color_count", prepared.colors},
                    {"load_seconds", entry.load_seconds}, {"dimensions", {dimensions.x(), dimensions.y(), dimensions.z()}}};
                if (!image.empty()) {
                    entry.reference_image_path = entry.ai_image_path = image;
                    record["reference_image_path"] = record["ai_image_path"] = image.lexically_relative(root).generic_string();
                }
                if (!entry.color_intent_path.empty()) {
                    record["color_intent_path"] = entry.color_intent_path.lexically_relative(root).generic_string();
                    record["color_intent_schema"] = entry.color_intent_schema;
                    record["color_intent_sha256"] = entry.color_intent_sha256;
                }
                if (!cancel->load() && !ModelArtifactAcquisition::publish_library_record(status.id, std::move(record)))
                    error = "模型文件已保留，但模型记录暂时无法保存。";
            }
        } catch (const std::exception& e) { error = e.what(); }
        wxGetApp().CallAfter([weak, cancel, error, entry = std::move(entry), complete, failed]() mutable {
            if (!weak || cancel->load()) return;
            if (weak->m_worker.joinable()) weak->m_worker.join();
            if (!error.empty() || entry.model_path.empty()) failed(error);
            else complete(std::move(entry));
        });
    });
}
} // namespace Slic3r::GUI
