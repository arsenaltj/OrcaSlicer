#include "ModelArtifactAcquisition.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelLibraryObservations.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <wx/weakref.h>
#include <ctime>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

ModelArtifactAcquisition::ModelArtifactAcquisition() : m_client(AISidecarClient::default_endpoint()) {}
ModelArtifactAcquisition::~ModelArtifactAcquisition() { cancel(); }
void ModelArtifactAcquisition::cancel() { ++m_generation; m_validation.cancel(); m_client.cancel_current(); }
bool ModelArtifactAcquisition::supported(const std::string& format, const std::string& encoding)
{ return format == "glb" || (format == "obj" && encoding == "vertex_colors"); }

void ModelArtifactAcquisition::start(const AIModelGenerationClient::JobStatus& status,
    Complete complete, AIModelGenerationClient::ErrorFn failed, std::function<void(size_t, size_t)> progress)
{
    cancel();
    if (status.id.empty() || !supported(status.artifact_format, status.artifact_color_encoding)) {
        failed("当前结果不是受支持的彩色 OBJ 或 GLB 模型。"); return;
    }
    const auto generation = m_generation;
    wxWeakRef<ModelArtifactAcquisition> weak(this);
    const auto fail = [weak, generation, failed](std::string error) {
        Presentation::post_gui([weak, generation, failed, error = std::move(error)] {
            if (weak && generation == weak->m_generation) failed(error);
        });
    };
    m_client.download_artifact(status.id, status.artifact_format, temp_path(status.id, status.artifact_format),
        [weak, generation, status, complete, fail](boost::filesystem::path path) {
            Presentation::post_gui([weak, generation, status, complete, fail, path] {
                if (!weak || generation != weak->m_generation) return;
                AI::GeneratedModelArtifact artifact;
                artifact.local_path = path; artifact.job_id = status.id;
                artifact.format = status.artifact_format; artifact.color_encoding = status.artifact_color_encoding;
                artifact.generation_palette = status.palette;
                artifact.used_printable_colors = !status.palette.empty();
                if (!status.color_intent_ready || status.color_intent_schema.empty()) { complete(artifact); return; }
                const auto manifest = temp_path(status.id + "-color-intent", "json");
                weak->m_client.download_color_intent(status.id, status.color_intent_schema, status.color_intent_sha256,
                    path, manifest,
                    [weak, generation, artifact, status, complete](boost::filesystem::path manifest) mutable {
                        Presentation::post_gui([weak, generation, artifact, status, complete, manifest]() mutable {
                            if (!weak || generation != weak->m_generation) return;
                            const auto task = weak->m_validation.begin(artifact.job_id, Presentation::TaskLifetime::Session);
                            const bool accepted = Presentation::background_tasks().submit(task,
                                [artifact, status, manifest](const Presentation::TaskContext& context) mutable {
                                    if (!context.canceled() && AIModelGenerationClient::validate_color_intent_manifest_file(
                                        manifest, status.color_intent_schema, status.color_intent_sha256, artifact.local_path))
                                        artifact.color_intent_manifest = AI::ColorIntentManifestRef{
                                            manifest.string(), status.color_intent_schema, status.color_intent_sha256};
                                    return artifact;
                                }, [weak, generation, task, artifact, complete](Presentation::TaskResult<AI::GeneratedModelArtifact> result) {
                                    if (!weak || generation != weak->m_generation || !weak->m_validation.matches(task) || result.canceled) return;
                                    // An unavailable/invalid optional manifest keeps the existing
                                    // model-color fallback; unvalidated intent is never installed.
                                    complete(result.value && !result.error ? std::move(*result.value) : artifact);
                                }, Presentation::TaskPriority::Visible);
                            if (!accepted) complete(artifact);
                        });
                    }, [weak, generation, artifact, complete](std::string) {
                        // Preserve the existing fallback to the model's own colors.
                        Presentation::post_gui([weak, generation, artifact, complete] {
                            if (weak && generation == weak->m_generation) complete(artifact);
                        });
                    });
            });
        }, fail, [weak, generation, progress](size_t received, size_t total) {
            if (progress) Presentation::post_gui([weak, generation, progress, received, total] {
                if (weak && generation == weak->m_generation) progress(received, total);
            });
        });
}

bool ModelArtifactAcquisition::publish_library_record(const std::string& job, nlohmann::json metadata)
{
    const auto path = library_metadata_path(job);
    const auto previous = read_json(path);
    // Download/recovery cannot replace a certified editing record.
    if (ModelLibraryObservations::has_check_certificate(previous)) return true;
    if (previous.is_object()) for (const auto* key : {"generated_at", "imported_at", "print_feedback", "print_feedback_at"})
        if (previous.contains(key)) metadata[key] = previous.at(key);
    if (!metadata.contains("generated_at")) metadata["generated_at"] = std::time(nullptr);
    return write_json(path, metadata);
}
}
