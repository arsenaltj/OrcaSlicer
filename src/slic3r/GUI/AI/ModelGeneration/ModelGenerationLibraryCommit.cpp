#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelArtifactAcquisition.hpp"
#include "slic3r/GUI/DiagnosticStageTimer.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/Utils/PlatformLibrary/LibraryClient.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

void ModelGenerationPanel::queue_library_commit(std::string job, ModelLibraryCommitReason reason, bool foreground)
{
    if (m_shutdown || job.empty()) return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    const auto sequence = m_sequence;
    const auto request = ModelLibraryCommit::next_request();
    Presentation::TaskContext task; task.lifetime = Presentation::TaskLifetime::Transaction; task.identity = job;
    const auto root = generated_models_root();
    const bool accepted = Presentation::background_tasks().submit(task,
        [root, job, reason, request](const Presentation::TaskContext& context) -> std::optional<ModelLibraryCommit> {
            auto entries = ModelLibraryRepository::read(root, *context.cancellation, true, job);
            if (entries.empty()) return {};
            ModelLibraryCommit event; event.entry = std::move(entries.front()); event.reason = reason; event.request = request;
            event.record_sha256 = PlatformLibrary::file_digest(root / "downloads" / ("orcaslicer-ai-" + job + ".json"), context.cancellation.get());
            return event;
        }, [weak, sequence, foreground](Presentation::TaskResult<std::optional<ModelLibraryCommit>> result) {
            if (!weak || weak->m_shutdown) return;
            if (result.value && *result.value) {
                auto event = std::move(**result.value);
                event.foreground = foreground && sequence == weak->m_sequence && (weak->IsShownOnScreen() || weak->m_smart_restore_guard.presented() || weak->m_workbench_presented);
                if (weak->m_library_commit_listener) weak->m_library_commit_listener(std::move(event));
            } else {
                // Registration is durable even if the projection cannot be
                // read now. A later scan retries without generating again.
                if (weak->m_library_commit_listener) weak->m_library_commit_listener({});
            }
        });
    if (!accepted && m_library_commit_listener) m_library_commit_listener({});
}

void ModelGenerationPanel::apply_library_commit(const ModelLibraryCommit& event)
{
    if (m_shutdown || !m_library_application) return;
    m_library_application->merge_commits(m_library_snapshot_entries, event.revision - 1);
    if (event.foreground) {
        m_library_page = m_workbench_history_page = 0;
        m_workbench_history_filter = 0;
        if (m_workbench_history_search) m_workbench_history_search->ChangeValue(wxEmptyString);
        m_workbench_history_highlight = event.entry.job_id;
        m_workbench_history_reveal = true;
    }
    // A running scan keeps its epoch and may complete. Its result is merged
    // with the publication ledger before it is allowed to replace this list.
    const bool scanning = m_library_scan_pending;
    apply_library_removal_projection(true);
    if (scanning) m_library_rescan_pending = true;
    else if (m_page_initialized) request_library_thumbnails(true);
}

void ModelGenerationPanel::save_library_entry(size_t artifact_size, size_t triangle_count, double width,
    double depth, double height, size_t color_count, double load_seconds)
{
    if (m_job_id.empty() || m_displayed_model_path.empty()) return;
    const auto root = generated_models_root();
    const auto job = m_job_id;
    const auto path = m_displayed_model_path;
    const auto sequence = m_sequence;
    const bool foreground = m_library_foreground_job == job;
    const auto request = ModelLibraryCommit::next_request();
    const auto reference = !m_reference_image_path.empty() ? m_reference_image_path : m_job_image_path;
    const auto image = !m_raw_preview_path.empty() ? m_raw_preview_path : m_preview_path;
    const wxImage display = m_model_reference_image.IsOk() ? m_model_reference_image.Copy() : (m_clean_preview_image.IsOk() ? m_clean_preview_image.Copy() : wxImage());
    nlohmann::json metadata {
        {"schema_version", 6}, {"job_id", job}, {"model_path", path.lexically_relative(root).generic_string()},
        {"source", job_uses_image() ? (m_job_prompt.empty() ? "image" : "text_image") : "text"},
        {"style", m_job_style}, {"custom_style", m_job_custom_style},
        {"generation_profile", m_job_generation_profile}, {"face_limit", m_job_face_limit},
        {"provider", m_job_provider_task_id.empty() ? m_job_generation_options.provider : m_job_provider_name},
        {"geometry_quality", m_job_generation_options.geometry_quality},
        {"texture_quality", m_job_generation_options.texture_quality}, {"output_format", m_job_generation_options.output_format},
        {"prompt", m_job_prompt.ToStdString(wxConvUTF8)}, {"palette", m_job_palette}, {"palette_roles", m_job_palette_roles},
        {"use_printable_colors", m_job_use_printable_colors}, {"artifact_size", artifact_size},
        {"triangle_count", triangle_count}, {"color_count", color_count}, {"load_seconds", load_seconds},
        {"dimensions", {width, depth, height}}
    };
    if (!m_job_provider_task_id.empty()) metadata["provider_task_id"] = m_job_provider_task_id;
    if (!m_job_provider_conversion_task_id.empty()) metadata["provider_conversion_task_id"] = m_job_provider_conversion_task_id;
    if (!m_preview_path.empty()) metadata["preview_path"] = m_preview_path.lexically_relative(root).generic_string();
    if (!m_color_intent_path.empty()) {
        metadata["color_intent_path"] = m_color_intent_path.lexically_relative(root).generic_string();
        metadata["color_intent_schema"] = m_color_intent_schema; metadata["color_intent_sha256"] = m_color_intent_sha256;
    }
    struct Saved { std::optional<ModelLibraryCommit> event; boost::filesystem::path reference, image; bool saved {false}; };
    wxWeakRef<ModelGenerationPanel> weak(this);
    Presentation::TaskContext task; task.lifetime = Presentation::TaskLifetime::Transaction; task.identity = job;
    const bool accepted = Presentation::background_tasks().submit(task,
        [root, job, reference, image, display, request, metadata = std::move(metadata)](const Presentation::TaskContext& context) mutable {
            DiagnosticStageTimer timing("ORCASLICER_UI_LATENCY_TRACE", "HistoryPublication");
            Saved saved;
            try { saved.reference = archive_library_image(reference, job, "reference"); } catch (...) {}
            try { saved.image = archive_library_image(image, job, "ai"); } catch (...) {}
            if (!saved.reference.empty()) metadata["reference_image_path"] = saved.reference.lexically_relative(root).generic_string();
            if (!saved.image.empty()) {
                metadata["ai_image_path"] = saved.image.lexically_relative(root).generic_string();
                if (display.IsOk()) { auto target = saved.image; target += ".display.png"; display.SaveFile(target.wstring(), wxBITMAP_TYPE_PNG); }
            }
            if (metadata.contains("color_intent_path") && !path_is_inside(root, root / metadata.at("color_intent_path").get<std::string>())) {
                metadata.erase("color_intent_path"); metadata.erase("color_intent_schema"); metadata.erase("color_intent_sha256");
            }
            timing("images-archived");
            saved.saved = ModelArtifactAcquisition::publish_library_record(job, std::move(metadata));
            timing("record-committed");
            if (saved.saved) {
                auto entries = ModelLibraryRepository::read(root, *context.cancellation, true, job);
                if (!entries.empty()) {
                    ModelLibraryCommit event; event.entry = std::move(entries.front()); event.request = request;
                    event.record_sha256 = PlatformLibrary::file_digest(library_metadata_path(job), context.cancellation.get());
                    saved.event = std::move(event);
                }
            }
            timing("entry-prepared");
            return saved;
        }, [weak, job, path, sequence, foreground](Presentation::TaskResult<Saved> result) {
            if (!weak || weak->m_shutdown) return;
            const bool current = sequence == weak->m_sequence && job == weak->m_job_id && path == weak->m_displayed_model_path;
            if (result.value && result.value->saved) {
                auto& saved = *result.value;
                const bool new_foreground = foreground && current && weak->m_library_foreground_job == job;
                if (weak->m_library_foreground_job == job) weak->m_library_foreground_job.clear();
                if (saved.event && weak->m_library_commit_listener) {
                    saved.event->foreground = new_foreground && (weak->IsShownOnScreen() || weak->m_smart_restore_guard.presented() || weak->m_workbench_presented);
                    weak->m_library_commit_listener(std::move(*saved.event));
                } else weak->queue_library_commit(job, ModelLibraryCommitReason::Generated, new_foreground);
                if (current) {
                    if (!saved.reference.empty()) weak->m_reference_image_path = saved.reference;
                    if (!saved.image.empty()) weak->m_raw_preview_path = saved.image;
                }
            } else if (current) {
                weak->m_status->SetLabel(_L("模型文件已保留，但历史记录保存失败；请重试加载，复用现有模型登记。"));
                weak->m_ui_history_error = "模型文件已保留，但历史记录保存失败，请重试加载。";
                weak->publish_ui_state();
            }
        });
    if (!accepted) {
        m_status->SetLabel(_L("模型文件已保留，历史登记队列繁忙；请重试加载，无需重新生成。"));
        m_ui_history_error = "模型文件已保留，历史登记队列繁忙；请重试加载，无需重新生成。";
        publish_ui_state();
    }
}
}
