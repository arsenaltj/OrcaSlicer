#include "slic3r/GUI/ModelGenerationPanel.hpp"

#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"

#include <boost/filesystem.hpp>

#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

#include <chrono>
#include <utility>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

void ModelGenerationPanel::download_model_preview(uint64_t sequence)
{
    if (!m_ready || m_job_id.empty() || m_shutdown)
        return;
    if (m_artifact_format != "obj") {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("只能预览和导入生成的 OBJ 模型。"));
        m_result_summary->SetLabel(_L("当前生成结果不是受支持的 OBJ 格式。"));
        refresh_controls();
        return;
    }
    if (m_artifact_color_encoding != "vertex_colors") {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("生成的 OBJ 不包含受支持的顶点颜色。"));
        m_result_summary->SetLabel(_L("缺少颜色信息，无法继续彩色模型流程。"));
        refresh_controls();
        return;
    }

    m_artifact_path = temp_path(m_job_id, m_artifact_format);
    m_color_intent_path.clear();
    m_busy = true;
    update_progress(94, 4, _L("下载模型"));
    m_status->SetLabel(_L("正在下载并校验生成的 OBJ 模型..."));
    m_model_stats->SetLabel(_L("正在加载模型..."));
    m_model_preview_message->SetLabel(_L("下载完成后将在此处显示彩色 3D 预览。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_artifact_path = path;
                if (weak->m_color_intent_schema.empty()) {
                    weak->finish_model_preview_download(path, sequence);
                    return;
                }
                weak->m_status->SetLabel(_L("正在校验模型颜色意图与 OBJ 的绑定..."));
                weak->m_color_intent_path = temp_path(weak->m_job_id + "-color-intent", "json");
                weak->m_client.download_color_intent(
                    weak->m_job_id, weak->m_color_intent_schema, weak->m_color_intent_sha256,
                    path, weak->m_color_intent_path,
                    [weak, sequence, path](boost::filesystem::path manifest_path) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, path = std::move(path),
                                              manifest_path = std::move(manifest_path)]() mutable {
                            if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                                return;
                            weak->m_color_intent_path = std::move(manifest_path);
                            weak->finish_model_preview_download(path, sequence);
                        });
                    },
                    [weak, sequence](std::string error) mutable {
                        if (!weak)
                            return;
                        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                            if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                                return;
                            weak->m_color_intent_path.clear();
                            weak->m_busy = false;
                            weak->m_artifact_download_started = false;
                            weak->m_model_preview_ready = false;
                            weak->m_status->SetLabel(_L("颜色意图清单校验失败，模型未进入导入流程。"));
                            weak->m_result_summary->SetLabel(_L("清单错误：") + from_u8(error));
                            weak->m_model_stats->SetLabel(_L("OBJ 已下载，颜色意图未通过校验"));
                            weak->refresh_controls();
                        });
                    });
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_busy = false;
                weak->m_artifact_download_started = false;
                weak->m_model_preview_ready = false;
                weak->m_status->SetLabel(_L("模型下载失败，请重试。"));
                weak->m_result_summary->SetLabel(_L("下载错误：") + from_u8(error));
                weak->m_model_stats->SetLabel(_L("模型尚未下载"));
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::finish_model_preview_download(const boost::filesystem::path& path, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!m_color_intent_schema.empty() &&
        !AIModelGenerationClient::validate_color_intent_manifest_file(
            m_color_intent_path, m_color_intent_schema, m_color_intent_sha256, path)) {
        m_color_intent_path.clear();
        m_busy = false;
        m_artifact_download_started = false;
        m_model_preview_ready = false;
        m_status->SetLabel(_L("颜色意图清单与 OBJ 不一致，模型未进入导入流程。"));
        m_result_summary->SetLabel(_L("请重新下载；现有 OBJ 已保留用于诊断。"));
        m_model_stats->SetLabel(_L("OBJ 已下载，颜色意图未通过校验"));
        refresh_controls();
        return;
    }

    size_t triangle_count = 0;
    size_t color_count = 0;
    Vec3d dimensions = Vec3d::Zero();
    std::string error;
    const auto load_started = std::chrono::steady_clock::now();
    if (m_model_preview == nullptr ||
        !m_model_preview->load_model(path, m_job_palette, triangle_count, dimensions, color_count, error)) {
        m_busy = false;
        m_artifact_download_started = false;
        m_model_preview_ready = false;
        m_status->SetLabel(_L("OBJ 模型解析失败，已保留本地文件。"));
        m_result_summary->SetLabel(_L("无法显示 3D 预览：") + from_u8(error));
        m_model_stats->SetLabel(_L("模型预览不可用"));
        m_model_preview_message->SetLabel(_L("请重试下载，或检查 generated_models/downloads 中的 OBJ 文件。"));
        refresh_controls();
        return;
    }
    const double load_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - load_started).count();

    m_artifact_path = path;
    m_displayed_model_path = path;
    m_displayed_model_job_id = m_job_id;
    m_displayed_model_palette = m_job_palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_busy = false;
    m_model_preview_ready = true;
    m_library_model_loaded = false;
    update_progress(100, 4, _L("检查并导入"));
    const bool visual_gate_blocked = m_visual_quality.available && !m_visual_quality.import_recommended;
    m_status->SetLabel(visual_gate_blocked
        ? _L("模型已生成，但人脸相似度或材料归属未通过；建议重新优化。")
        : _L("3D 模型已生成，请确认外观后再导入准备页。"));
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 种颜色\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(), model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(
        _L("模型已自动摆正；拖动旋转、滚轮缩放，随时可点击“摆正模型”。"));
    m_result_summary->SetLabel(visual_gate_blocked
        ? _L("模型文件与结构可用，但外观门禁未通过；强制导入前会再次确认。")
        : m_color_intent_path.empty()
            ? _L("模型已下载并通过 OBJ 解析，可继续按旧版兼容方式导入准备页。")
            : _L("模型与颜色意图已校验，可继续导入准备页。"));
    const size_t artifact_size = boost::filesystem::file_size(path);
    save_library_entry(artifact_size, triangle_count, dimensions.x(), dimensions.y(),
                       dimensions.z(), color_count, load_seconds);
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    wxWeakRef<ModelGenerationPanel> weak(this);
    wxGetApp().CallAfter([weak]() {
        if (weak && weak->m_model_preview != nullptr)
            weak->m_model_preview->refresh();
    });
    refresh_controls();
}

} // namespace Slic3r::GUI
