#include "ModelGenerationStatusText.hpp"
#include "ModelGenerationSubmissionState.hpp"
#include "ModelGenerationHost.hpp"

#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <algorithm>

namespace Slic3r::GUI::ModelGenerationStatusText {

wxString localized_service_error(const std::string& error)
{
    if (ModelGenerationPresentation::is_model_provider_not_configured(error))
        return _L("所选模型服务尚未配置，本次未创建 3D 任务。设计图已保留，请配置服务后手动重试，或选择已配置的服务。");
    wxString message = from_u8(error);
    if (message.Contains("2-million-face target requires detailed geometry"))
        return _L("200 万面需要精细几何。请在 3D 设置中选择精细几何，或改为 100 万面后重试。");
    if (message.Contains("2-million-face option requires Tripo v3.1"))
        return _L("当前 Tripo 模型版本不支持 200 万面，请改为 100 万面，或检查服务的模型版本设置。");
    if (message.Contains("Could not connect to the preprocessing service"))
        return _L("无法连接图片生成服务，请检查网络、代理和服务地址后重试。");
    if (message.Contains("preprocessing service is temporarily unavailable"))
        return _L("图片生成服务暂时不可用，请稍后重试。");
    if (message.Contains("preprocessing service is rate limiting"))
        return _L("图片生成请求过于频繁，请稍后重试。");
    if (message.Contains("preprocessing service rejected the request"))
        return _L("图片生成服务拒绝了请求，请检查图片和提示词后重试。");
    if (message.Contains("rejected the request") || message.Contains("Tripo rejected"))
        return _L("模型服务拒绝了当前图片或提示词。请调整内容后手动重试；程序不会自动创建新的付费任务。");
    if (message.Contains("rate limiting") || message.Contains("rate limited"))
        return _L("模型服务当前请求过多，请稍后手动重试；程序不会自动创建新的付费任务。");
    if (message.Contains("deadline expired") || message.Contains("timed out"))
        return _L("模型服务响应超时。请先确认服务端没有遗留任务，再手动重试以避免重复计费。");
    if (message.Contains("not reachable") || message.Contains("Couldn't connect") ||
        message.Contains("Failed to connect") || message.Contains("Connection refused"))
        return _L("无法连接本地 AI 服务，请确认正式服务已启动后重试。");
    if (message.Contains("large square cutout") || message.Contains("missing body region"))
        return _L("人物出现方形缺口或身体区域缺失，本次未提交付费 3D 任务；请点击“换一张”重新生成。");
    return _L("操作未能完成，请保留任务记录并查看诊断日志后重试。");
}

wxString localized_advanced_service_error(const std::string& error)
{
    // HTTP adapter preserves a structured code before ':'. Never append the
    // supplier's untrusted/free-form English explanation to the visible UI.
    const auto separator = error.find(':');
    const auto code = separator == std::string::npos ? std::string() : error.substr(0, separator);
    const bool valid_code = !code.empty() && code.size() <= 64 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (!valid_code) return localized_service_error(error);
    wxString message = code == "stale_plan" || code == "stale_confirmation" || code == "confirmation_changed" || code == "revision_conflict"
        ? _L("当前设计或确认计划已变化，请刷新状态后重新确认。")
        : code == "group_busy" || code == "model_busy" ? _L("当前组仍有任务处理中，请等待或停止当前任务。")
        : code == "unsupported_multiview" ? _L("当前模型服务暂不支持四视角建模，请检查服务配置。")
        : code == "provider_task_ambiguous" ? _L("本次请求的受理结果暂不明确，请核对本次请求，不要重复提交。")
        : code == "idempotency_conflict" || code == "snapshot_unavailable"
            ? _L("本次请求身份或记录暂时无法核对，请保留请求记录并查看诊断日志。")
            : _L("高级任务处理未能完成，当前输入和已有结果已保留，请查看诊断日志。");
    return message + "\n" + _L("错误码：") + from_u8(code);
}

wxString localized_job_status(const AIModelGenerationClient::JobStatus& status, bool advanced)
{
    if (advanced && status.state == "accepted")
        return _L("3D任务已受理，正在读取任务进度…");
    if (advanced && status.state != "queued" && status.state != "running" && status.state != "ready" &&
        status.state != "stopping" && status.state != "stopped" && status.state != "cancelled" && status.state != "failed")
        return _L("正在处理当前3D模型任务…");
    if (status.state == "recommending_palette")
        return wxString::Format(
            _L("AI 正在分析主体并推荐 %llu 种目标色..."),
            static_cast<unsigned long long>(status.palette_color_count));
    if (status.state == "awaiting_palette_confirmation")
        return _L("AI 配色已准备好，请修改或确认后生成图片预览。");
    if (status.state == "preprocessing" && status.phase == "image_generation")
        return _L("图片服务正在生成写实预览，通常需要 1–3 分钟，请保持页面打开...");
    if (status.state == "preprocessing" && status.phase == "checking_image")
        return _L("图片已生成，正在检查人脸、构图和 3D 输入清晰度...");
    if (status.state == "preprocessing" && status.phase == "printability_check")
        return _L("正在分离肤色、衣服、点缀色和轮廓色，并清理杂色...");
    if (status.state == "preprocessing")
        return _L("AI 正在准备提示词和图片预览...");
    if (status.state == "awaiting_confirmation" && status.phase == "multiview_retry")
        return _L("四视图准备未通过，本次尚未创建付费 3D 生成任务；当前图片已保留，可直接重试。");
    if (status.state == "awaiting_confirmation" && status.palette_quality_ok && status.model_input_eligible &&
        std::find(status.model_input_warnings.begin(), status.model_input_warnings.end(),
                  "reference_visual_review_unavailable") != status.model_input_warnings.end())
        return _L("图片已生成；AI 视觉复核暂不可用。本地输入检查已通过，请对照原图确认后继续。");
    if (status.state == "awaiting_confirmation")
        return _L("设计图已准备完成，可以生成 3D 模型；内容检查仅供参考。");
    if (status.state == "queued")
        return _L("3D 生成任务已排队...");
    if (advanced && status.state == "running" && status.phase == "preparing_multiview")
        return _L("正在准备已确认的四视角模型输入…");
    if (!advanced && status.state == "running" && status.phase == "preparing_multiview")
        return _L("正在生成并核对写实人像四视图，尚未提交付费 3D 任务...");
    if (status.state == "running" && status.phase == "generating")
        return status.progress >= 69
            ? _L("远端正在收尾并封装高精度几何，可能停留数分钟；请保持软件运行，停止或退出后不会恢复本次生成。")
            : _L("AI 正在生成 3D 模型...");
    if (status.state == "running" && status.phase == "texturing") {
        if (advanced) return _L("正在生成模型纹理…");
        return status.progress >= 69
            ? _L("远端正在烘焙最终写实纹理，可能停留数分钟；请勿重复提交...")
            : _L("正在保留历史造型并重新生成写实颜色...");
    }
    if (status.state == "running" && status.phase == "converting") {
        if (advanced) return _L("正在转换模型文件…");
        return status.progress >= 94
            ? _L("远端 OBJ 转换正在封装最后文件，完成后会自动下载并检查...")
            : _L("正在转换高精度彩色 OBJ...");
    }
    if (status.state == "running" && status.phase == "downloading_artifact")
        return advanced ? _L("正在下载和整理模型文件…") : _L("正在整理模型文件...");
    if (status.state == "running" && status.phase == "checking_model") {
        if (advanced) return _L("正在校验模型文件…");
        if (status.progress <= 96) return _L("正在下载并把纹理烘焙为模型顶点色...");
        if (status.progress == 97) return _L("正在生成四视图并分离肤色、衣物、头发和底座...");
        if (status.progress == 98) return _L("正在清理杂色小块并平滑材料边界...");
        return _L("正在检查拓扑、底座、薄壁和高面数模型；高质量档可能需要几分钟...");
    }
    if (status.state == "running" && status.phase == "checking_visual" && advanced)
        return _L("正在检查模型外观…");
    if (status.state == "running" && status.phase == "checking_visual")
        return _L("正在生成五视图，对照原图检查人脸相似度和材料串色...");
    if (status.state == "ready" && advanced)
        return _L("3D模型已生成，正在准备本地预览。");
    if (status.state == "ready")
        return status.visual_quality.available && !status.visual_quality.import_recommended
            ? _L("模型已完成，但人脸相似度或材料串色检查未通过；请先查看风险项。")
            : _L("3D 模型生成与质量检查完成。");
    if (status.state == "stopping")
        return _L("正在停止生成任务...");
    if (status.state == "stopped" && advanced)
        return _L("已停止生成，四视图已保留；再次生成将创建新任务。");
    if (status.state == "stopped")
        return _L("已停止生成，输入与已有图片已保留；再次生成将创建新任务。");
    if (status.state == "cancelled")
        return _L("生成任务已取消。");
    if (status.state == "failed") {
        wxString message;
        if (status.provider_error_code == "image_rate_limited")
            message = _L("图片服务请求较多，请稍后点击“重新生成图片预览”。不会自动重复调用。");
        else if (status.provider_error_code == "image_auth_failed")
            message = _L("图片服务认证或模型权限异常，请检查服务配置后重试。");
        else if (status.provider_error_code == "image_rejected")
            message = _L("图片服务拒绝了请求，请检查图片格式、描述和模型参数；详细原因见诊断日志。");
        else if (status.provider_error_code == "image_download_failed")
            message = _L("服务已返回图片，但结果下载失败。请检查网络并保留诊断 ID；重新生图会再次消耗额度。");
        else if (status.provider_error_code == "image_service_unavailable")
            message = _L("图片服务暂时不可用，本次结果可能不明确。程序不会自动重试，请稍后手动重新生成。");
        else if (status.provider_error_code == "image_connection_failed")
            message = _L("连接图片服务失败，本次结果可能不明确。程序不会自动重试，请检查网络后手动重新生成。");
        else if (status.provider_error_code == "local_image_processing_failed")
            message = _L("图片已经生成，但本地处理未完成。请保留任务编号和已有图片，再按当前输入重新生成。");
        else if (status.provider_error_ambiguous)
            message = _L("模型服务提交结果不明确，远端可能已创建付费任务。为避免重复计费，程序不会自动重试；请先到服务端确认任务状态。");
        else if (status.provider_error_code == "provider_rejected")
            message = _L("模型服务拒绝了当前图片或提示词。请调整受限内容后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_rate_limited")
            message = _L("模型服务当前请求过多，请稍后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_timeout")
            message = _L("模型服务响应超时。请确认服务端没有遗留任务后再手动重试，避免重复计费。");
        else if (status.provider_error_code == "sidecar_unavailable")
            message = _L("本地 AI 服务不可用，请先重新检测服务；当前任务和输入已保留。");
        else if (status.provider_error_code == "provider_unavailable")
            message = _L("本地 AI 服务已连接，但 Tripo 暂时不可达。请检查代理或网络后重新生成；不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_upload_unavailable" ||
                 status.provider_error_code == "provider_upload_timeout" ||
                 status.provider_error_code == "provider_upload_rate_limited")
            message = _L("参考图上传到 Tripo 失败，尚未创建付费 3D 任务。请检查代理或网络后重试，程序会有限重试上传。");
        else if (status.provider_error_code == "provider_upload_failed")
            message = _L("参考图上传失败，尚未创建付费 3D 任务。请检查图片和服务配置后重试。");
        else if (status.provider_error_code == "artifact_dns_failed")
            message = _L("模型下载失败：无法解析下载服务器域名。请检查网络后重新生成。");
        else if (status.provider_error_code == "artifact_download_timeout")
            message = _L("模型下载失败：连接或传输超时。请检查网络后重新生成。");
        else if (status.provider_error_code == "artifact_transfer_failed")
            message = _L("模型下载失败，请查看诊断日志后重新生成。");
        else if (status.provider_error_code == "artifact_download_failed")
            message = _L("Tripo 任务已返回，但模型文件下载或校验失败。已有模型文件仍保留；可以从历史查看，或重新生成新任务。");
        else if (status.provider_error_code == "model_validation_failed")
            message = _L("模型文件已返回，但本地校验未通过。请保留诊断信息后重试或返回 2D 设计。");
        else if (status.provider_error_category == "authentication")
            message = _L("模型服务认证或权限异常，请检查服务账户配置。");
        else if (status.provider_error_category == "rate_limit")
            message = _L("模型服务请求过多，请稍后重新生成。");
        else if (status.provider_error_category == "network")
            message = _L("模型服务连接异常或超时，请检查网络后重试。");
        else if (advanced)
            message = _L("模型生成未完成，请保留任务信息并查看诊断记录。");
        else
            message = status.message.empty() ? _L("生成任务失败。") : localized_service_error(status.message);
        if (!status.provider_detail_code.empty() || !status.provider_detail_message.empty()) {
            const auto detail = from_u8(status.provider_detail_code).Lower();
            if (detail.Contains("insufficient") && (detail.Contains("balance") || detail.Contains("credit") || detail.Contains("quota")))
                message = _L("模型服务积分或额度不足，请检查服务账户。");
            else if (status.provider_detail_code == "FailedOperation.InnerError")
                message = _L("YOYOClaw生成几何失败。");
            if (!status.provider_detail_message.empty()) message += "\n" + from_u8(status.provider_detail_message);
            if (!status.provider_detail_code.empty()) message += "\n" + _L("错误码：") + from_u8(status.provider_detail_code);
        }
        if (advanced && status.provider_detail_code.empty() && !status.provider_error_code.empty())
            message += "\n" + _L("错误码：") + from_u8(status.provider_error_code);
        if (!advanced && !status.provider_error_category.empty())
            message += "\n" + _L("故障分类：") + from_u8(status.provider_error_category);
        if (!status.provider_task_id.empty())
            message += "\n" + (advanced ? _L("模型服务任务 ID：") : _L("Provider 任务 ID：")) + from_u8(status.provider_task_id);
        if (!status.provider_conversion_task_id.empty())
            message += "\n" + _L("转换任务 ID：") + from_u8(status.provider_conversion_task_id);
        if (status.provider_error_ambiguous)
            message += "\n" + _L("提交结果不明确，请先确认服务端任务状态，不要重复提交，避免重复计费。");
        if (!status.id.empty())
            message += "\n" + _L("诊断 ID：") + from_u8(status.id);
        return message;
    }
    if (advanced) return _L("正在处理当前3D模型任务…");
    return _L("正在处理当前任务，请稍候…");
}

void localize_advanced_model_state(ModelGenerationUIState& state)
{
    if (state.smart_mode || (state.job_id.empty() && state.stage == ModelGenerationUIStage::Input)) return;
    AIModelGenerationClient::JobStatus status;
    status.id = state.job_id;
    status.state = state.job_state;
    status.phase = state.job_phase;
    status.provider_error_code = state.provider_error_code;
    status.provider_detail_code = state.provider_detail_code; status.provider_detail_message = state.provider_detail_message;
    status.provider_error_ambiguous = state.provider_error_ambiguous;
    status.provider_task_id = state.provider_task_id;
    status.provider_conversion_task_id = state.provider_conversion_task_id;
    // UI progress can be estimated/remapped; it cannot imply a provider's final phase.
    status.progress = 0;
    wxString text;
    switch (state.stage) {
    case ModelGenerationUIStage::LoadingModel:
        text = _L("正在下载、校验并加载3D模型…"); break;
    case ModelGenerationUIStage::ModelReady:
        text = _L("3D模型生成完成，可进入工作台编辑。"); break;
    case ModelGenerationUIStage::GeneratingModel:
        // The observer keeps the last provider phase while reconnecting. Preserve
        // its existing Chinese connection notice instead of reusing that old phase.
        if (state.status_text == "正在重新连接当前任务")
            text = _L("正在重新连接当前任务…");
        break;
    case ModelGenerationUIStage::Stopping:
        status.state = "stopping"; break;
    case ModelGenerationUIStage::Stopped:
        if (status.state != "stopped" && status.state != "cancelled")
            text = _L("模型本地加载已取消，原任务与成果已保留。");
        break;
    case ModelGenerationUIStage::Failed:
        if (status.state != "failed")
            text = _L("模型本地加载未完成，原任务与成果已保留；可按原入口重试加载。");
        break;
    default: break;
    }
    if (text.empty()) text = localized_job_status(status, true);
    state.status_text = text.ToStdString(wxConvUTF8);
    // Raw messages remain on the original host snapshot; the display uses the mapped reason.
    state.summary_text.clear();
}

void present_advanced_model(ModelGenerationUIState& state)
{
    localize_advanced_model_state(state);
    state.workflow_guidance = _L("任务记录已保留，进度更新不会重复提交生成请求。").ToStdString(wxConvUTF8);
}

wxString model_input_quality_label(const std::string& code)
{
    wxString advice;
    if (code == "subject_not_detected") advice = _L("未能区分主体与背景。");
    else if (code == "subject_too_small") advice = _L("主体在画面中所占比例较小。");
    else if (code == "subject_or_background_fills_frame") advice = _L("主体或背景铺满画面。");
    else if (code == "subject_cropped") advice = _L("主体贴边，可能存在裁切。");
    else if (code == "fragmented_subject") advice = _L("图片可能包含断开的主体或碎片。");
    else if (code == "excessive_semitransparency") advice = _L("图片包含较多半透明区域。");
    else if (code == "background_not_isolated") advice = _L("图片背景较复杂。");
    else if (code == "subject_has_rectangular_cutout") advice = _L("图片可能存在方形缺口。");
    else if (code == "portrait_shoulder_silhouette_unverified") advice = _L("暂时无法确认完整主体轮廓。");
    else advice = _L("图片存在内容质量建议。");
    return advice + _L("检查仅供参考，不影响3D生成；请确认最终模型效果。");
}

} // namespace Slic3r::GUI::ModelGenerationStatusText
