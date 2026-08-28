#include "ModelGenerationStatusText.hpp"

#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI::ModelGenerationStatusText {

wxString localized_service_error(const std::string& error)
{
    wxString message = from_u8(error);
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
    return _L("操作失败：") + message;
}

wxString localized_job_status(const AIModelGenerationClient::JobStatus& status)
{
    if (status.state == "recommending_palette")
        return _L("AI 正在分析主体并推荐四种目标色...");
    if (status.state == "awaiting_palette_confirmation")
        return _L("AI 配色已准备好，请修改或确认后生成图片预览。");
    if (status.state == "preprocessing")
        return _L("AI 正在准备提示词和图片预览...");
    if (status.state == "awaiting_confirmation")
        return status.palette_quality_ok && status.model_input_eligible
            ? _L("预览已准备完成，请确认后继续生成 3D 模型。")
            : _L("预览未通过 3D 输入检查，请按提示重新生成。");
    if (status.state == "queued")
        return _L("3D 生成任务已排队...");
    if (status.state == "running" && status.phase == "generating")
        return _L("AI 正在生成 3D 模型...");
    if (status.state == "running" && status.phase == "converting")
        return _L("正在转换并优化彩色低模 OBJ...");
    if (status.state == "running" && status.phase == "downloading_artifact")
        return _L("正在整理模型文件...");
    if (status.state == "ready")
        return _L("3D 模型生成完成。");
    if (status.state == "stopping")
        return _L("正在停止生成任务...");
    if (status.state == "cancelled")
        return _L("生成任务已取消。");
    if (status.state == "failed") {
        wxString message;
        if (status.provider_error_code == "image_rate_limited")
            message = _L("图片服务请求较多，请稍后点击“重新生成图片预览”。不会自动重复调用。");
        else if (status.provider_error_code == "image_rejected")
            message = _L("图片服务拒绝了当前内容，请调整图片或描述后重新生成。");
        else if (status.provider_error_code == "image_service_unavailable")
            message = _L("图片服务暂时不可用，本次结果可能不明确。程序不会自动重试，请稍后手动重新生成。");
        else if (status.provider_error_code == "image_connection_failed")
            message = _L("连接图片服务失败，本次结果可能不明确。程序不会自动重试，请检查网络后手动重新生成。");
        else if (status.provider_error_ambiguous)
            message = _L("模型服务提交结果不明确，远端可能已创建付费任务。为避免重复计费，程序不会自动重试；请先到服务端确认任务状态。");
        else if (status.provider_error_code == "provider_rejected")
            message = _L("模型服务拒绝了当前图片或提示词。请调整受限内容后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_rate_limited")
            message = _L("模型服务当前请求过多，请稍后手动重试；程序不会自动创建新的付费任务。");
        else if (status.provider_error_code == "provider_timeout")
            message = _L("模型服务响应超时。请确认服务端没有遗留任务后再手动重试，避免重复计费。");
        else if (status.provider_error_code == "provider_unavailable")
            message = _L("模型服务暂时不可用，请稍后手动重试；程序不会自动创建新的付费任务。");
        else
            message = status.message.empty() ? _L("生成任务失败。") : localized_service_error(status.message);
        if (!status.id.empty())
            message += "\n" + _L("诊断 ID：") + from_u8(status.id);
        return message;
    }
    return status.message.empty() ? _L("正在处理...") : from_u8(status.message);
}

wxString model_input_quality_label(const std::string& code)
{
    if (code == "subject_not_detected") return _L("未识别到清晰主体，请换一张主体明确的图片或重新生成。");
    if (code == "subject_too_small") return _L("主体太小，请放大主体后重新生成图片预览。");
    if (code == "subject_or_background_fills_frame") return _L("主体或背景铺满画面，请保留清晰留白后重新生成。");
    if (code == "subject_cropped") return _L("主体贴边且可能被裁切，请让完整轮廓出现在画面内。");
    if (code == "fragmented_subject") return _L("图片包含多个断开的主体或碎片，请重新生成一个连贯主体。");
    if (code == "excessive_semitransparency") return _L("主体半透明区域过多，不适合作为 3D 输入，请重新生成。");
    if (code == "background_not_isolated") return _L("背景过于复杂，请使用透明或纯色背景后重新生成。");
    return _L("图片未通过 3D 输入检查，请按预览提示重新生成。");
}

} // namespace Slic3r::GUI::ModelGenerationStatusText
