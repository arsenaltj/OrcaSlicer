#pragma once

#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include <functional>
#include <string>

namespace Slic3r::GUI {

// Synchronous, local rejection of this click only. Async service errors retain
// their existing route. Never forward raw provider messages into the Toast.
using GenerationRejected = std::function<void(const std::string&)>;

inline std::string generation_catalog_rejection(const AIModelGenerationClient::ProviderCatalog& catalog,
                                               const std::string& model)
{
    if (catalog.supports_model(model)) return {};
    if (catalog.state == "checking") return "provider_checking";
    if (catalog.state == "not_configured")
        return catalog.role == "geometry" ? "geometry_not_configured" :
            catalog.provider == "gpt" ? "gpt_not_configured" : "image_not_configured";
    if (catalog.state == "empty") return "provider_empty";
    if (catalog.state == "ready")
        return catalog.role == "geometry" ? "geometry_model_unavailable" : "image_model_unavailable";
    return "service_unavailable";
}

inline const char* generation_rejection_message(const std::string& reason)
{
    if (reason == "gpt_not_configured") return "请先配置GPT 2D模型。";
    if (reason == "image_not_configured") return "当前2D模型未配置，请先完成配置。";
    if (reason == "geometry_not_configured") return "当前3D模型未配置，请先完成配置。";
    if (reason == "provider_checking") return "正在检查模型配置，请稍后重试。";
    if (reason == "provider_empty") return "当前服务没有可用模型，请更换配置。";
    if (reason == "service_unavailable") return "当前模型服务不可用，请检查配置。";
    if (reason == "image_model_unavailable") return "当前2D模型不可用，请更换模型或检查配置。";
    if (reason == "geometry_model_unavailable") return "当前3D模型不可用，请更换模型或检查配置。";
    if (reason == "empty_input") return "请上传参考图或填写模型描述。";
    if (reason == "prompt_too_long") return "描述超过2000字节，请缩短后生成。";
    if (reason == "custom_style_empty") return "请填写自定义风格描述。";
    if (reason == "custom_style_too_long") return "自定义风格描述过长，请缩短后生成。";
    if (reason == "unsupported_style") return "当前风格不可用，请重新选择。";
    if (reason == "invalid_image") return "参考图无法使用，请重新上传有效图片。";
    if (reason == "missing_front") return "已上传辅助视角，请补充主参考图。";
    if (reason == "missing_current_view") return "请先生成可用的主视角及所选设计图。";
    if (reason == "design_regeneration_required") return "图片或风格已修改，请先重新生成对应设计图。";
    if (reason == "draft_changed") return "输入已修改，请先生成对应设计图。";
    if (reason == "image_unsupported") return "当前2D模型不支持本次生成方式，请更换模型。";
    if (reason == "geometry_unsupported") return "当前3D模型不支持所选设计图建模，请更换模型。";
    if (reason == "gpt_required") return "智能模式需要GPT 2D模型，请检查配置。";
    if (reason == "face_quality_mismatch") return "200万面需要选择精细几何。";
    if (reason == "palette_unavailable") return "当前耗材色板不可用，请选择有效色板。";
    if (reason == "recovery_required") return "存在待核对的请求，请先恢复原任务结果。";
    if (reason == "creation_read_only") return "当前为只读查看，请先返回AI创作恢复编辑。";
    if (reason == "group_busy" || reason == "workbench_guard" || reason == "history_loading")
        return "当前操作尚未完成，请稍后重试。";
    return "当前输入或模型配置未能同步，请检查后重试。";
}

} // namespace Slic3r::GUI
