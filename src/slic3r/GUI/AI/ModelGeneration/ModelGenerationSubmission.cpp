#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelGenerationStatusText.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"

#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <algorithm>
#include <utility>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
using namespace ModelGenerationStatusText;

namespace {

void classify_submission_error(const std::string& error, std::string& code, std::string& category,
                               bool& retryable, bool& ambiguous)
{
    code.clear();
    category.clear();
    retryable = false;
    ambiguous = false;
    if (error.find("provider_upload_unavailable") != std::string::npos ||
        error.find("provider_upload_timeout") != std::string::npos ||
        error.find("provider_upload_rate_limited") != std::string::npos) {
        if (error.find("provider_upload_timeout") != std::string::npos)
            code = "provider_upload_timeout";
        else if (error.find("provider_upload_rate_limited") != std::string::npos)
            code = "provider_upload_rate_limited";
        else
            code = "provider_upload_unavailable";
        category = "upload";
        retryable = true;
        return;
    }
    if (error.find("provider_upload_failed") != std::string::npos) {
        code = "provider_upload_failed";
        category = "upload";
        return;
    }
    if (error.find("provider_unavailable") != std::string::npos ||
        error.find("Could not connect to Tripo") != std::string::npos) {
        code = "provider_unavailable";
        category = "availability";
        retryable = true;
        return;
    }
    if (error.find("provider_timeout") != std::string::npos ||
        error.find("deadline expired") != std::string::npos ||
        error.find("timed out") != std::string::npos) {
        code = "provider_timeout";
        category = "availability";
        retryable = true;
        ambiguous = error.find("ambiguous") != std::string::npos;
        return;
    }
    if (error.find("provider_rejected") != std::string::npos ||
        error.find("rejected the request") != std::string::npos) {
        code = "provider_rejected";
        category = "request";
        return;
    }
    if (error.find("artifact_download_failed") != std::string::npos ||
        error.find("download") != std::string::npos) {
        code = "artifact_download_failed";
        category = "artifact";
        retryable = true;
        return;
    }
    if (error.find("AI sidecar is not reachable") != std::string::npos ||
        error.find("AI sidecar request timed out") != std::string::npos ||
        error.find("service_unavailable") != std::string::npos ||
        error.find("session_required") != std::string::npos) {
        code = "sidecar_unavailable";
        category = "local_sidecar";
        retryable = true;
        return;
    }
    if (error.find("ambiguous") != std::string::npos) {
        code = "provider_ambiguous";
        category = "availability";
        ambiguous = true;
        return;
    }
}

} // namespace

void ModelGenerationPanel::on_generate(wxCommandEvent&)
{
    if (!generation_options_valid()) {
        show_input_hint(_L("200 万面需要选择精细几何（+20 积分）。"));
        return;
    }
    const bool image_mode = m_job_preview_expected;
    if (!m_awaiting_confirmation || m_job_id.empty() || !job_inputs_match() || (image_mode && !m_style_preview_ready))
        return;
    if (use_printable_colors() != m_job_use_printable_colors || current_palette() != m_job_palette) {
        show_input_hint(_L("颜色模式或耗材色板发生了变化，请先重新生成预览。"));
        return;
    }
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    m_job_generation_options = current_generation_options();
    const wxString provider_label = m_job_generation_options.provider == "hunyuan" ? _L("腾讯混元3D") : wxString("Tripo");
    wxString message = wxString::Format(image_mode
        ? _L("要根据当前 AI 设计图创建 1 个付费 %s 生成任务吗？")
        : _L("要根据已确认的提示词创建 1 个付费 %s 生成任务吗？"), provider_label);
    message += "\n\n" + generation_options_summary(image_mode);
    if (m_job_generation_options.provider == "tripo" && m_job_generation_options.output_format == "obj")
        message += _L("\n本次还将创建 1 个 OBJ 基础转换任务（已计入估算）。");
    message += _L("\n停止：只停止本地等待；已提交的远端任务可能继续运行并计费。");
    RedesignMessageDialog confirm(this, message, _L("确认生成 3D 模型"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    if (image_mode)
        m_client.record_journey_event("preview_accepted", m_job_id);
    m_client.record_journey_event("model_submitted", m_job_id);
    m_journey_model_submitted = true;
    m_ui_model_generation_context = true;
    ++m_model_generation_session;
    m_busy = true;
    m_awaiting_confirmation = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    const uint64_t sequence = ++m_sequence;
    const SubmissionContext submission {m_job_id, m_job_generation_options.provider, sequence};
    m_submission_state.begin(submission);
    update_progress(40, 3, _L("生成模型"));
    m_workflow_phase->SetLabel(_L("生成模型"));
    m_status->SetLabel(_L("正在提交 3D 生成请求..."));
    refresh_controls();
    const std::string prepared = image_mode ? std::string() : m_prepared_prompt->GetValue().ToUTF8().data();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.generate(m_job_id, prepared, m_job_palette, m_job_generation_options,
        [weak, sequence, submission](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, submission, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || status.id != submission.job_id ||
                    !submission.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence)) return;
                weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence, submission](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, submission, error = std::move(error)]() {
                if (!weak || weak->m_shutdown ||
                    !submission.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence))
                    return;
                // A transient failure here is ambiguous: the sidecar may have
                // accepted and persisted the paid task before its HTTP response
                // was interrupted.  Keep polling the same job id so users do not
                // submit a duplicate paid task just to recover the UI.
                if (is_transient_sidecar_poll_error(error) && !weak->m_job_id.empty()) {
                    weak->m_status->SetLabel(_L("提交响应中断，正在查询已保存的任务..."));
                    weak->m_result_summary->SetLabel(
                        _L("不会重复提交；将使用同一任务编号恢复生成进度。"));
                    weak->m_poll_connection_failures = 0;
                    weak->m_poll_timer.StartOnce(500);
                    weak->refresh_controls();
                    return;
                }
                weak->handle_error(error, sequence);
                // Recover the approved preview and eligibility, while retaining
                // the rejected submission's notice over an unchanged preview GET.
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id.empty())
                    return;
                const std::string job_id = weak->m_job_id;
                weak->m_client.get_status(job_id,
                    [weak, sequence, submission](AIModelGenerationClient::JobStatus status) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, submission, status = std::move(status)]() mutable {
                            if (!weak || weak->m_shutdown || status.id != submission.job_id ||
                                !submission.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence)) return;
                            weak->handle_status(std::move(status), sequence);
                        });
                    },
                    [](std::string) {});
            });
        });
}

bool ModelGenerationPanel::request_retry_model()
{
    if (m_shutdown || !m_page_initialized || m_busy || m_job_id.empty() ||
        (m_job_state != "failed" && m_job_state != "stopped"))
        return false;
    if (m_provider_error_ambiguous && m_job_provider_task_id.empty()) {
        show_input_hint(_L("提交结果不明确，当前未保存 Provider 任务编号。请先在 Tripo 服务端确认任务状态，程序不会自动重复提交。"));
        return false;
    }

    const uint64_t sequence = ++m_sequence;
    // A failed job may leave the provider selector editable. Recovery must
    // follow the provider recorded with the job, otherwise a changed UI
    // selection can make a valid remote task look like a provider mismatch.
    const std::string provider = !m_job_provider_name.empty()
        ? m_job_provider_name
        : !m_job_generation_options.provider.empty()
        ? m_job_generation_options.provider
        : current_generation_options().provider;
    auto options = current_generation_options();
    options.provider = provider;
    const SubmissionContext submission {m_job_id, provider, sequence};
    m_submission_state.begin(submission);
    m_busy = true;
    m_ui_model_generation_context = true;
    m_ui_stopping = false;
    ++m_model_generation_session;
    m_status->SetLabel(m_job_provider_task_id.empty()
        ? _L("正在安全重试当前任务…") : _L("正在恢复原 Provider 任务…"));
    m_result_summary->SetLabel(m_job_provider_task_id.empty()
        ? _L("已确认没有 Provider 任务编号；将复用当前本地任务，不创建新的本地任务记录。")
        : _L("将查询并复用原 Provider 任务，不会再次创建付费任务。"));
    refresh_controls();

    const std::string prepared = m_job_preview_expected ? std::string() : m_prepared_prompt->GetValue().ToUTF8().data();
    const bool resume_existing = !m_job_provider_task_id.empty();
    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence, submission, provider](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, submission, provider, status = std::move(status)]() mutable {
            if (!weak || weak->m_shutdown || status.id != submission.job_id ||
                !submission.matches(weak->m_job_id, provider, weak->m_sequence)) return;
            weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence, submission, provider](std::string error) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, submission, provider, error = std::move(error)]() {
            if (!weak || weak->m_shutdown ||
                !submission.matches(weak->m_job_id, provider, weak->m_sequence)) return;
            weak->handle_error(error, sequence);
        });
    };
    m_client.generate(m_job_id, prepared, m_job_palette, options,
        std::move(success), std::move(failure), resume_existing);
    return true;
}

void ModelGenerationPanel::handle_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_submission_state.retain_error({m_job_id, current_generation_options().provider, sequence}, error);
    classify_submission_error(error, m_provider_error_code, m_provider_error_category,
                              m_provider_error_retryable, m_provider_error_ambiguous);
    if (m_journey_model_submitted) {
        m_client.record_journey_event("model_failed", m_job_id);
        m_journey_model_submitted = false;
    } else if (m_job_preview_expected) {
        m_client.record_journey_event("preview_failed", m_job_id);
    }
    m_poll_timer.Stop();
    m_busy = false;
    // A request-level failure may arrive before the sidecar publishes a new
    // job status (for example when the local service is unreachable). Keep the
    // job on the model route and expose the same failed state used by polled
    // provider failures so the diagnostic and safe-retry action are visible.
    m_job_state = "failed";
    m_job_phase = "failed";
    const bool paid_preflight_rejected =
        error.find("large square cutout") != std::string::npos ||
        error.find("missing body region") != std::string::npos ||
        error.find("shoulder silhouette") != std::string::npos ||
        error.find("background remnant") != std::string::npos ||
        error.find("not suitable for 3D input") != std::string::npos ||
        error.find("before paying for 3D generation") != std::string::npos;
    m_awaiting_confirmation = paid_preflight_rejected;
    if (!paid_preflight_rejected) {
        m_awaiting_palette_confirmation = false;
        m_palette_recommendation_confirmed = false;
    } else {
        // The preview itself remains a useful user artifact, but it may not be
        // resubmitted until a regenerated geometry reference passes preflight.
        m_model_input_eligible = false;
        m_model_input_primary_blocker =
            error.find("shoulder silhouette") != std::string::npos ||
            error.find("background remnant") != std::string::npos
                ? "portrait_shoulder_silhouette_unverified"
                : "subject_has_rectangular_cutout";
    }
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
    wxString message = localized_service_error(error);
    if (!m_job_id.empty())
        message += "\n" + _L("诊断 ID：") + from_u8(m_job_id);
    show_input_hint(message);
    m_result_summary->SetLabel(_L("模型尚未生成完成。"));
    if (m_job_preview_expected && !m_style_preview_ready)
        show_preview_failure(_L("图片预览未完成，请查看失败原因后重试。"));
    refresh_controls();
}

void ModelGenerationPanel::show_preview_failure(const wxString& message)
{
    m_style_preview_placeholder = _L("预览不可用");
    m_preview_message->SetLabel(message);
    m_result_summary->SetLabel(_L("输入已保留，可修改后重新生成图片预览。"));
    update_preview_view();
}

void ModelGenerationPanel::handle_poll_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!is_transient_sidecar_poll_error(error)) {
        handle_error(error, sequence);
        return;
    }

    ++m_poll_connection_failures;
    // A new sidecar nonce needs a new authenticated challenge. Keep ordinary
    // disconnected-task polling/cancellation behavior while the service is down.
    if (error == "A valid OrcaSlicer AI session is required." && m_service_retry_handler)
        m_service_retry_handler();
    const int delay_ms = std::min(10000, 1000 << std::min(m_poll_connection_failures - 1, 3));
    m_status->SetLabel(wxString::Format(
        _L("本地 AI 服务暂时断开，%d 秒后自动重连..."),
        std::max(1, delay_ms / 1000)));
    m_result_summary->SetLabel(
        _L("远端任务编号和当前进度已保存；重连只恢复查询，不会重复提交或重复计费。"));
    m_poll_timer.StartOnce(delay_ms);
    refresh_controls();
}

void ModelGenerationPanel::handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_submission_state.reconcile({status.id, current_generation_options().provider, sequence}, status.state);
    m_poll_connection_failures = 0;
    const bool became_model_ready = !m_ready && status.state == "ready" && status.artifact_ready;
    const bool job_changed = status.id != m_job_id;
    const bool palette_count_unchanged = current_palette_color_count() == m_job_palette_color_count;
    if (!job_changed)
        synchronize_palette_roles(current_palette(), m_palette_roles, m_job_palette, m_job_palette_roles,
                                  status.palette, status.palette_roles);
    if (job_changed) {
        m_job_provider_name.clear();
        m_job_provider_task_id.clear();
        m_job_provider_conversion_task_id.clear();
        m_provider_error_code.clear();
        m_provider_error_category.clear();
        m_provider_error_retryable = false;
        m_provider_error_ambiguous = false;
        m_color_intent_path.clear();
    }
    m_job_id = status.id;
    m_job_state = status.state;
    m_job_phase = status.phase;
    if (!status.provider_error_code.empty()) {
        m_provider_error_code = status.provider_error_code;
        m_provider_error_category = status.provider_error_category;
        m_provider_error_retryable = status.provider_error_retryable;
        m_provider_error_ambiguous = status.provider_error_ambiguous;
    } else if (status.state == "failed" && m_provider_error_code.empty()) {
        // Older sidecars only persisted a bounded message. Preserve a useful
        // diagnostic category instead of replacing it with a generic failure.
        classify_submission_error(status.message, m_provider_error_code, m_provider_error_category,
                                  m_provider_error_retryable, m_provider_error_ambiguous);
    } else if (status.state == "queued" || status.state == "running" || status.state == "ready") {
        m_provider_error_code.clear();
        m_provider_error_category.clear();
        m_provider_error_retryable = false;
        m_provider_error_ambiguous = false;
    }
    if (status.state == "stopped" || status.state == "cancelled" || status.state == "failed")
        m_ui_stopping = false;
    m_job_palette_color_count = status.palette_color_count;
    if (m_palette_color_count != nullptr &&
        (job_changed || palette_count_unchanged)) {
        m_palette_color_count->SetSelection(static_cast<int>(
            status.palette_color_count - Slic3r::AI::kMinTargetPaletteColors));
    }
    if (!status.provider_task_id.empty()) {
        m_job_provider_name = status.provider_name;
        m_job_provider_task_id = status.provider_task_id;
        m_job_provider_conversion_task_id = status.provider_conversion_task_id;
        m_job_generation_options = status.generation_options;
        m_job_face_limit = status.generation_options.face_limit;
        m_job_generation_profile = status.generation_profile;
    }
    if (status.palette_recommendation.available) {
        const bool new_recommendation = m_palette_recommendation_job_id != status.id;
        m_palette_recommendation = status.palette_recommendation;
        m_palette_recommendation_confirmed = status.palette_recommendation.confirmed;
        if (new_recommendation) {
            m_palette_recommendation_job_id = status.id;
            m_user_adjusted_palette_colors.clear();
            m_custom_palette.clear();
            m_palette_roles.clear();
            if (status.palette_recommendation.confirmed && !status.palette.empty()) {
                m_custom_palette = status.palette;
                m_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
                for (const std::string& color : status.palette) {
                    const auto recommended = std::find_if(
                        status.palette_recommendation.colors.begin(), status.palette_recommendation.colors.end(),
                        [&color](const AIModelGenerationClient::PaletteRecommendationColor& item) { return item.hex == color; });
                    if (recommended == status.palette_recommendation.colors.end())
                        m_user_adjusted_palette_colors.emplace_back(color);
                }
            } else {
                for (const auto& color : status.palette_recommendation.colors) {
                    m_custom_palette.emplace_back(color.hex);
                    m_palette_roles[color.role] = color.hex;
                }
            }
            m_palette_roles_source = m_custom_palette;
            if (m_palette_source != nullptr)
                m_palette_source->SetSelection(2);
            m_job_use_printable_colors = true;
        }
    }
    if (!status.palette.empty()) {
        m_job_palette = status.palette;
        m_job_use_printable_colors = true;
    }
    m_status->SetLabel(localized_job_status(status));
    m_busy = status.state == "recommending_palette" || status.state == "preprocessing" ||
             status.state == "queued" || status.state == "running" || status.state == "stopping";
    m_awaiting_palette_confirmation = status.state == "awaiting_palette_confirmation";
    m_awaiting_confirmation = status.state == "awaiting_confirmation";
    m_ready = status.state == "ready" && status.artifact_ready;
    if (m_journey_model_submitted &&
        (status.state == "failed" ||
         (status.state == "awaiting_confirmation" && status.phase == "multiview_retry"))) {
        m_client.record_journey_event("model_failed", status.id);
        m_journey_model_submitted = false;
    }
    if (became_model_ready) {
        m_client.record_journey_event("model_ready", status.id);
        m_journey_model_submitted = false;
    }
    m_artifact_format = status.artifact_format;
    m_artifact_color_encoding = status.artifact_color_encoding;
    if (!status.color_intent_ready) {
        m_color_intent_path.clear();
        m_color_intent_schema.clear();
        m_color_intent_sha256.clear();
    } else {
        if (status.color_intent_schema != m_color_intent_schema ||
            status.color_intent_sha256 != m_color_intent_sha256)
            m_color_intent_path.clear();
        m_color_intent_schema = status.color_intent_schema;
        m_color_intent_sha256 = status.color_intent_sha256;
    }
    m_raw_preview_available = status.raw_preview_ready;
    m_preview_output_available = status.preview_ready || status.raw_preview_ready ||
                                 status.model_reference_ready;
    m_preview_output = status.model_reference_ready ? "model-reference" : status.preview_ready ? "preview" : "raw-preview";
    const bool new_model_views = status.model_views_ready && !m_model_views_available;
    m_model_views_available = status.model_views_ready;
    m_model_reference_available = status.model_reference_ready;
    m_strict_preview_available = status.strict_preview_ready;
    m_heatmap_available = status.heatmap_ready;
    if (status.preview_ready || status.raw_preview_ready || status.model_reference_ready || status.strict_preview_ready)
        m_job_preview_expected = true;
    else if (status.state == "awaiting_confirmation" && status.source == "text" && status.palette.empty())
        m_job_preview_expected = false;
    m_preview_metrics_available = status.metadata_ready;
    m_preview_changed_pixel_ratio = status.changed_pixel_ratio;
    m_preview_minimum_feature_px = status.minimum_feature_px;
    m_palette_quality_ok = status.palette_quality_ok;
    m_material_fragmentation_ok = status.material_fragmentation_ok;
    m_model_input_eligible = status.model_input_eligible;
    m_model_input_primary_blocker = status.model_input_blockers.empty() ? std::string() : status.model_input_blockers.front();
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    if (m_ready) {
        m_displayed_model_job_id = status.id;
        apply_model_quality(status.model_quality);
        apply_visual_quality(status.visual_quality);
        apply_model_refinement(status.refinement);
    }
    if (!status.palette_roles.empty())
        m_job_palette_roles = status.palette_roles;
    if (!status.prepared_prompt.empty())
        m_prepared_prompt->SetValue(wxString::FromUTF8(status.prepared_prompt));
    if (m_preview_output_available && !m_preview_download_cancelled &&
        !m_preview_download_in_flight && !m_style_preview_ready && !m_restoring_input) {
        m_status->SetLabel(_L("正在加载 AI 风格预览..."));
        m_style_preview_placeholder = _L("正在加载 AI 生成图...");
        update_preview_view();
        download_preview(sequence);
    }
    if (m_ready) {
        wxString summary;
        summary << (m_model_preview_ready ? _L("3D 模型已可预览") : _L("模型已生成，正在准备 3D 预览"))
                << _L(" · ") << wxString::FromUTF8(m_artifact_format);
        if (status.artifact_size > 0)
            summary << wxString::Format(_L(" · %.1f MB"), double(status.artifact_size) / (1024.0 * 1024.0));
        if (status.visual_quality.available && !status.visual_quality.import_recommended)
            summary << _L(" · AI 外观存在提醒，不建议直接导入");
        m_result_summary->SetLabel(summary);
    } else if (m_awaiting_palette_confirmation) {
        m_result_summary->SetLabel(
            _L("AI 推荐配色已准备好。可以替换、删除或补充颜色；确认后再匹配实际耗材。"));
    } else if (m_awaiting_confirmation && status.phase == "multiview_retry") {
        m_result_summary->SetLabel(
            _L("四视图在付费前检查阶段停止，当前预览和配色均已保留；可重试，或先换一张图片。"));
    } else if (m_awaiting_confirmation) {
        if (status.preview_ready && !status.model_input_eligible) {
            m_result_summary->SetLabel(model_input_quality_label(m_model_input_primary_blocker));
        } else if (status.preview_ready && !status.palette.empty() && !status.palette_quality_ok) {
            const int required_colors = std::min<int>(status.palette.size(), 3);
            if (status.meaningful_subject_color_count < required_colors) {
                m_result_summary->SetLabel(wxString::Format(
                    _L("配色不足：主体只有 %d 种有效耗材色，至少需要 %d 种。请重新生成预览。"),
                    status.meaningful_subject_color_count, required_colors));
            } else if (status.printable_subject_area_ratio < 0.18) {
                m_result_summary->SetLabel(_L("主体占画面比例过小，请放大主体后重新生成预览。"));
            } else if (status.largest_subject_component_ratio < 0.90) {
                m_result_summary->SetLabel(_L("主体被背景分成多个不相连区域，请调整构图后重新生成预览。"));
            } else if (status.largest_detached_subject_diagonal_ratio >= 0.08) {
                m_result_summary->SetLabel(_L("检测到细长部件与主体分离，请重新生成并确认把手、枝条或支撑已连接。"));
            } else if (!status.material_fragmentation_ok) {
                m_result_summary->SetLabel(_L("检测到肤色或衣服颜色形成错误杂色块，请重新生成图片预览后再生成 3D。"));
            } else {
                m_result_summary->SetLabel(_L("预览未通过打印性检查，请调整构图或配色后重新生成。"));
            }
        } else if (status.preview_ready &&
                   std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                             "foreground_segmentation_uncertain") != status.model_input_warnings.end()) {
            m_result_summary->SetLabel(
                _L("主体与背景颜色接近，自动检查无法可靠判断轮廓；请确认主体完整、背景干净后再生成 3D。"));
        } else if (status.preview_ready && !status.palette.empty()) {
            m_result_summary->SetLabel(
                (m_job_style == "realistic" || m_job_style == "portrait_sketch") && m_job_generation_profile == "quality"
                    ? _L("AI 设计图已准备好，请确认脸型、五官、姿态和构图。")
                    : _L("AI 设计图与配色已准备好，确认后即可生成 3D。"));
        } else {
            m_result_summary->SetLabel(m_job_preview_expected
                ? _L("AI 风格预览加载完成后即可生成 3D 模型。")
                : _L("请确认提示词后再开始生成 3D 模型。"));
        }
    } else {
        m_result_summary->SetLabel(localized_job_status(status));
    }
    if (m_awaiting_confirmation && status.model_input_eligible &&
        std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                  "reference_visual_review_unavailable") != status.model_input_warnings.end())
        m_result_summary->SetLabel(m_result_summary->GetLabel() +
            _L("\nAI 视觉复核暂不可用，请自行对照原图检查脸型、姿态和配色。"));
    if ((status.state == "failed" || status.state == "stopped" || status.state == "cancelled") &&
        m_job_preview_expected && !m_style_preview_ready && m_preview_path.empty() &&
        !m_restoring_input && !m_preview_output_available)
        show_preview_failure(status.state == "failed"
            ? _L("图片预览未完成，请查看失败原因后重试。")
            : _L("图片预览已停止，输入已保留。"));
    update_workflow(&status);
    const bool palette_recommendation_fallback =
        status.state == "failed" && !m_custom_palette.empty() &&
        status.message.find("palette recommendation") != std::string::npos;
    if (palette_recommendation_fallback) {
        m_palette_source->SetSelection(2);
        m_palette_recommendation_confirmed = true;
        m_awaiting_palette_confirmation = false;
        m_job_palette.clear();
        m_job_palette_roles.clear();
        m_job_use_printable_colors = true;
        refresh_palette();
        update_progress(0, 1, _L("输入"));
        m_workflow_steps->SetLabel(_L("AI 推荐未通过对比度检查，已保留当前颜色，可直接生成图片预览"));
        m_status->SetLabel(_L("AI 推荐颜色过于接近，已自动保留当前有效配色。"));
        m_result_summary->SetLabel(_L("当前 1–6 种颜色仍可编辑，也可直接生成图片预览。"));
    }
    if (m_busy)
        m_poll_timer.StartOnce(1500);
    refresh_controls();
    if (m_ready && !m_artifact_download_started && !m_model_preview_ready) {
        m_artifact_download_started = true;
        download_model_preview(sequence);
    }
    if (new_model_views && !m_preview_path.empty())
        download_auxiliary_previews(sequence, 2);
}

void ModelGenerationPanel::schedule_poll()
{
    if (m_shutdown || m_job_id.empty() || !m_busy)
        return;
    const uint64_t sequence = m_sequence;
    const SubmissionContext request {m_job_id, current_generation_options().provider, sequence};
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(m_job_id,
        [weak, sequence, request](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, request, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || status.id != request.job_id ||
                    !request.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence)) return;
                weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence, request](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, request, error = std::move(error)]() {
                if (!weak || weak->m_shutdown ||
                    !request.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence))
                    return;
                weak->handle_poll_error(error, sequence);
                if (is_transient_sidecar_poll_error(error)) return;
                // Reconcile the saved job without losing a submission rejection.
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id.empty())
                    return;
                const std::string job_id = weak->m_job_id;
                weak->m_client.get_status(job_id,
                    [weak, sequence, request](AIModelGenerationClient::JobStatus status) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, request, status = std::move(status)]() mutable {
                            if (!weak || weak->m_shutdown || status.id != request.job_id ||
                                !request.matches(weak->m_job_id, weak->current_generation_options().provider, weak->m_sequence)) return;
                            weak->handle_status(std::move(status), sequence);
                        });
                    },
                    [](std::string) {});
            });
        });
}

} // namespace Slic3r::GUI
