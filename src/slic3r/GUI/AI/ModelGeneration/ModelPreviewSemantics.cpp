#include "ModelPreview3D.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/stdpaths.h>
#include <wx/image.h>
#include <fstream>

namespace Slic3r::GUI {
void ModelPreview3D::update_semantic_coloring()
{
    if (!m_has_model || !m_semantic_source || !m_color_trial_enabled) {
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_color_trial->set_semantic_status(wxEmptyString, false);
        return;
    }
    if (!m_semantic_controller) {
        const auto executable = std::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdWstring());
        m_semantic_controller = std::make_unique<ModelSemanticColoring>(executable.parent_path() / "ai" / "portrait_semantics",
            std::filesystem::u8path(Slic3r::data_dir()) / "cache" / "portrait_semantics");
        if (!m_validation_provider.empty()) m_semantic_controller->set_boundary_provider(m_validation_provider);
    }
    if (m_semantic_controller->request(m_semantic_source, m_color_trial->semantic_mapping_palette(), m_color_trial->semantic_palette(),
                                      m_color_trial->semantic_portrait_card(), m_face_color_overrides,
                                      m_color_trial->semantic_optimization() ? m_color_trial->semantic_slots(true) : m_color_trial->baseline_source_slots(),
                                      m_color_trial->semantic_slots(), m_manual_slot_intents, m_color_trial->semantic_optimization(),
                                      m_region_color_intents)) {
        m_semantic_ready = false;
        m_semantic_analysis.reset();
        if (m_context && m_canvas->SetCurrent(*m_context)) m_semantic_model.reset();
        m_automatic_face_colors.clear();
        m_automatic_subface_colors.clear();
        m_color_trial->set_semantic_status(_L("正在本机识别人像区域，可旋转模型或取消……"), true);
        m_semantic_timer.Start(100);
    }
}

void ModelPreview3D::finish_semantic_coloring()
{
    if (!m_semantic_controller) { m_semantic_timer.Stop(); return; }
    if (auto result = m_semantic_controller->poll()) {
        if (result->error.empty() && result->analysis && !result->slots.material_centers.empty() &&
            m_color_trial->set_material_centers(result->slots.material_centers,result->analysis->signature)) return;
        if (!m_pending_mapping.is_null()) {
            AI::SemanticColoring::SlotMappingResult restored; std::string error;
            if (source_slot_signature() == m_pending_mapping_slots_key && result->analysis &&
                result->analysis->signature == m_pending_mapping_signature &&
                AI::SemanticColoring::decode_slot_mapping(m_pending_mapping, m_triangle_count,
                    m_color_trial->semantic_slots(), restored, error)) {
                result->slots = std::move(restored);
                std::map<size_t,AI::SemanticColoring::Color> colors(result->slots.faces.begin(),result->slots.faces.end());
                for(auto& automatic:result->automatic) {
                    const auto found=colors.find(automatic.first); if(found!=colors.end()) automatic.second=found->second;
                }
                result->automatic_subfaces = result->slots.subfaces;
                result->geometry = build_semantic_colored_geometry(*m_semantic_source,
                    AI::SemanticColoring::compose(result->slots.faces, result->effective_manual, true),
                    AI::SemanticColoring::compose_subfaces(result->automatic_subfaces,result->effective_manual,true));
            } else BOOST_LOG_TRIVIAL(info) << "Saved semantic mapping recomputed after identity/slot validation: " << error;
            m_pending_mapping = nlohmann::json(); m_pending_mapping_signature.clear();
        }
        m_effective_manual_slots = std::move(result->effective_manual_slots);
        m_effective_manual_colors = std::move(result->effective_manual);
        m_semantic_slot_result = std::move(result->slots);
        m_region_color_intents = m_semantic_slot_result.region_overrides;
        m_validation_diagnostics = {{"elapsed_ms", result->elapsed_ms}, {"cache_hit", result->cache_hit},
            {"requested_boundary", result->requested_boundary}, {"actual_boundary", result->actual_boundary},
            {"fallback", result->boundary_fallback}, {"error", result->error},
            {"substituted_manual_slots",result->substituted_manual_slots},
            {"boundary_budget_fallback",m_semantic_slot_result.boundary_budget_fallback},
            {"automatic_faces", result->automatic.size()}, {"automatic_subfaces", result->automatic_subfaces.size()},
            {"added_triangles", result->subface_added_triangles}, {"person_detected", result->person_detected}};
        std::set<std::string> used;
        std::set<size_t> manually_painted;
        for(const auto& manual:m_effective_manual_slots) {used.insert(manual.slot_id);manually_painted.insert(manual.face_id);}
        std::map<size_t,double> covered;
        for(const auto& leaf:m_semantic_slot_result.subface_slots) if(!manually_painted.count(leaf.face_id)) {
            used.insert(leaf.slot_id); covered[leaf.face_id]+=1.0/double(1u<<(2*leaf.path.depth));
        }
        for(const auto& face:m_semantic_slot_result.face_slots) {
            const auto found=covered.find(face.face_id);
            if(!manually_painted.count(face.face_id) && (found==covered.end() || found->second<1.0)) used.insert(face.slot_id);
        }
        m_validation_diagnostics["used_material_slots"]=used.size();
        m_semantic_analysis = std::move(result->analysis);
        if (!result->error.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "Local semantic coloring unavailable: " << result->error;
            m_color_trial->set_semantic_status(_L("人像区域优化暂不可用，已沿用原有配色。"), false);
        } else if (result->geometry.is_empty()) {
            m_color_trial->set_semantic_status(_L("未找到足够可靠的人像区域，已沿用原有配色。"), false);
        } else if (m_context && m_canvas->SetCurrent(*m_context)) {
            auto model = std::make_unique<GLModel>();
            model->init_from(std::move(result->geometry));
            m_semantic_model = std::move(model);
            m_automatic_face_colors = std::move(result->automatic);
            m_automatic_subface_colors = std::move(result->automatic_subfaces);
            m_semantic_ready = true;
            m_color_trial->set_semantic_status(result->person_detected ? _L("已按人像区域优化；不明确的区域沿用原配色，可在局部改色中修正。") :
                _L("使用普通耗材匹配；人工改色优先保留。"), false);
            if (!result->boundary_fallback.empty())
                m_color_trial->set_semantic_status(_L("边界模型回退，保留基础配色：") + wxString::FromUTF8(result->boundary_fallback), false);
        } else {
            // The CPU result was consumed, but could not be adopted by the
            // preview. Invalidate the request so a later user action can retry;
            // do not keep the timer alive or reuse an older semantic surface.
            m_semantic_controller->cancel();
            m_semantic_ready = false;
            m_semantic_analysis.reset();
            m_automatic_face_colors.clear();
            m_automatic_subface_colors.clear();
            m_color_trial->set_semantic_status(_L("人像区域预览暂不可用，已沿用原有配色。切换试色后可重试。"), false);
        }
        BOOST_LOG_TRIVIAL(info) << "Local semantic coloring completed: ms=" << result->elapsed_ms
            << ", cache_hit=" << result->cache_hit << ", auto_faces=" << m_automatic_face_colors.size()
            << ", auto_subfaces=" << m_automatic_subface_colors.size()
            << ", added_triangles=" << result->subface_added_triangles
            << ", rejected_subfaces=" << result->subface_rejected_candidates
            << ", person=" << result->person_detected;
        if (result->substituted_manual_slots)
            m_color_trial->set_semantic_status(_L("部分人工耗材已停用，当前使用临时替代；恢复该槽位可还原。"), false);
        m_canvas->Refresh(false);
    }
    if (!m_semantic_controller->busy()) m_semantic_timer.Stop();
    else {
        const int progress = m_semantic_controller->progress();
        m_color_trial->set_semantic_status(progress < 0 ? _L("正在切换人像区域优化任务……") :
            wxString::Format(_L("正在本机优化人像配色 · %d%%"), progress), true);
    }
}

void ModelPreview3D::set_validation_provider(const std::string& provider)
{
    if (m_validation_provider == provider) return;
    m_validation_provider = provider;
    if (m_semantic_controller) m_semantic_controller->set_boundary_provider(provider);
    update_semantic_coloring();
}

nlohmann::json ModelPreview3D::validation_diagnostics() const
{
    auto result = m_validation_diagnostics;
    result["source"] = m_model_path.string();
    result["geometry_sha256"] = m_geometry_id;
    result["semantic_ready"] = m_semantic_ready;
    if (m_semantic_source) result["content_sha256"] = m_semantic_source->content_id;
    if (m_semantic_analysis) {
        result["analysis_signature"] = m_semantic_analysis->signature;
        result["body"] = m_semantic_analysis->body_identity;
        result["face"] = m_semantic_analysis->face_identity;
        result["boundary"] = m_semantic_analysis->boundary_identity;
        result["boundary_runs"] = nlohmann::json::array();
        for (const auto& run : m_semantic_analysis->boundary_runs)
            result["boundary_runs"].push_back({{"person_id",run.person_id},{"part",int(run.part)},
                {"side",int(run.side)},{"status",run.status},{"reason",run.reason},
                {"loading_ms",run.loading_ms},{"encoding_ms",run.encoding_ms},{"decoding_ms",run.decoding_ms},{"score",run.model_score},
                {"changed_pixels",run.changed_pixels}});
    }
    return result;
}

bool ModelPreview3D::capture_validation_image(const std::filesystem::path& path, int width, int height,
                                             int& samples, std::string& error)
{
    error.clear();
    if (!m_has_model || width < 1 || height < 1 || width > 4096 || height > 4096 ||
        !m_context || !m_canvas->SetCurrent(*m_context) || !wxGetApp().init_opengl()) {
        error = "Preview is not ready for capture."; return false;
    }
    GLint maximum = 0, previous = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maximum); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    samples = std::max(1, std::min(4, maximum));
    GLuint scene = 0, color = 0, depth = 0, resolve = 0, texture = 0;
    glGenFramebuffers(1, &scene); glBindFramebuffer(GL_FRAMEBUFFER, scene);
    glGenRenderbuffers(1, &color); glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depth); glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    bool okay = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenFramebuffers(1, &resolve); glBindFramebuffer(GL_FRAMEBUFFER, resolve);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    okay = okay && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (okay) {
        const bool old_overlay = m_selection_overlay_visible;
        m_selection_overlay_visible = false;
        render_preview(width, height, scene);
        m_selection_overlay_visible = old_overlay;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, scene); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve); glReadBuffer(GL_COLOR_ATTACHMENT0);
        wxImage image(width, height);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, image.GetData());
        okay = glGetError() == GL_NO_ERROR && image.Mirror(false).SaveFile(wxString(path.wstring()), wxBITMAP_TYPE_PNG);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, previous);
    glDeleteTextures(1, &texture); glDeleteRenderbuffers(1, &color); glDeleteRenderbuffers(1, &depth);
    glDeleteFramebuffers(1, &scene); glDeleteFramebuffers(1, &resolve);
    if (!okay) error = "OpenGL multisample capture failed.";
    refresh();
    return okay;
}
} // namespace Slic3r::GUI
