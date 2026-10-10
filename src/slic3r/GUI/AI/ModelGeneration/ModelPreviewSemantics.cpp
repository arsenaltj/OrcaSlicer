#include "ModelPreview3D.hpp"
#include "BeautyColorPaint.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/stdpaths.h>

namespace Slic3r::GUI {
bool ModelPreview3D::restore_beauty_face_colors(const FaceColorOverrides& colors)
{
    if (!m_has_model || !m_semantic_source || !m_context || !m_canvas->SetCurrent(*m_context)) return false;
    try {
        const auto& mesh = m_semantic_source->mesh;
        std::unique_ptr<GLModel> model;
        if (!colors.empty()) {
            if (m_manual_corner_normals.size() != mesh.indices.size() * 3)
                m_manual_corner_normals = ModelPreviewNormals::corner_normals(mesh);
            GLModel::Geometry geometry;
            geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
            geometry.reserve_vertices(colors.size() * 3);
            geometry.reserve_indices(colors.size() * 3);
            for (const auto& item : colors) {
                if (item.first >= mesh.indices.size()) return false;
                const auto& face = mesh.indices[item.first];
                const auto packed = preview_rgb8(item.second[0], item.second[1], item.second[2]);
                const auto base = uint32_t(geometry.vertices_count());
                for (size_t corner = 0; corner < 3; ++corner)
                    geometry.add_vertex(mesh.vertices[face[corner]], m_manual_corner_normals[item.first * 3 + corner],
                                        Vec2f(float(packed), -1.f));
                geometry.add_triangle(base, base + 1, base + 2);
            }
            model = std::make_unique<GLModel>();
            model->init_from(std::move(geometry));
        }
        m_face_color_overrides = colors;
        m_manual_color_model = std::move(model);
        m_canvas->Refresh(false);
        return true;
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(warning) << "Manual color preview failed: " << error.what();
        return false;
    }
}

size_t ModelPreview3D::paint_beauty_faces(const std::vector<size_t>& faces, const RGBA& color)
{
    if (!m_beauty_view || !m_region_editor->ready() || selection_busy() || semantic_processing() ||
        !same_stamp(m_model_stamp, file_stamp(m_model_path))) return 0;
    auto selection = selection_state();
    selection.selected.assign(editing_face_count(), 0);
    for (size_t face : faces) if (face < selection.selected.size()) selection.selected[face] = 1;
    constrain_shape_selection(selection);
    const std::array<float, 3> target {color[0], color[1], color[2]};
    if (!m_leaf_editing) {
        const auto result = paint_beauty_face_colors(m_face_color_overrides, selection, faces, target);
        return result.changed && restore_beauty_face_colors(result.colors) ? result.changed : 0;
    }
    const auto eligible = paint_beauty_face_colors({}, selection, faces, target);
    if (!eligible.changed || !m_semantic_source || !m_context || !m_canvas->SetCurrent(*m_context)) return 0;
    auto colors = m_manual_leaf_colors;
    size_t changed = 0;
    for (const auto& item : eligible.colors) {
        const auto& key = m_leaf_editing->keys[item.first];
        const auto found = colors.find(key);
        if (found != colors.end() && found->second == target) continue;
        colors[key] = target;
        ++changed;
    }
    if (!changed) return 0;
    AI::BeautyLeafEdits::capture(*m_leaf_editing, m_portrait_shapes->locks, colors, selection_state());
    m_manual_leaf_colors = std::move(colors);
    ++m_leaf_edit_revision;
    refresh_leaf_colors();
    return changed;
}

void ModelPreview3D::update_semantic_coloring()
{
    if (!portrait_recognition_enabled() || !m_has_model || !m_semantic_source || !m_color_trial_enabled || !m_color_trial->semantic_optimization()) {
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_color_trial->set_semantic_region_availability({});
        m_color_trial->set_semantic_status(wxEmptyString, false);
        if (m_semantic_completion) {
            m_semantic_error = semantic_reoptimization_reason();
            if (m_semantic_error.empty()) m_semantic_error = _L("人像区域优化已停用，请重新确认色卡和语义输入。");
            auto callback = std::move(m_semantic_completion);
            callback(false);
        }
        return;
    }
    if (!m_semantic_controller) {
        const auto executable = std::filesystem::path(wxStandardPaths::Get().GetExecutablePath().ToStdWstring());
        m_semantic_controller = std::make_unique<ModelSemanticColoring>(executable.parent_path() / "ai" / "portrait_semantics",
            std::filesystem::u8path(Slic3r::data_dir()) / "cache" / "portrait_semantics");
    }
    std::shared_ptr<ModelSemanticColoring::SavedAppearance> saved;
    if(m_portrait_shapes && m_portrait_shapes->surface_partition && m_semantic_ready) {
        saved=std::make_shared<ModelSemanticColoring::SavedAppearance>();
        saved->geometry_id=m_semantic_source->geometry_id;saved->face_count=m_semantic_source->mesh.indices.size();
        saved->faces=m_semantic_analysis ? m_automatic_face_colors : m_saved_semantic_faces;
        saved->subfaces=m_semantic_analysis ? m_automatic_subface_colors : m_saved_semantic_subfaces;
    }
    if (m_semantic_controller->request(m_semantic_source, m_color_trial->semantic_mapping_palette(), m_color_trial->semantic_palette(),
                                      m_color_trial->semantic_portrait_card(), m_face_color_overrides,
                                      std::filesystem::path(m_model_path.native()), m_portrait_shapes,m_color_trial->semantic_palette_roles(),
                                      !m_shapes_unlocked && m_manual_leaf_colors.empty() && m_manual_cell_colors.empty(),
                                      m_manual_cell_colors, m_portrait_progress,std::move(saved))) {
        m_semantic_submission_edit_revision = m_leaf_edit_revision;
        m_semantic_submission_manual = m_face_color_overrides;
        m_semantic_submission_unlocked = m_shapes_unlocked;
        m_semantic_ready = false;
        m_semantic_analysis.reset();
        m_color_trial->set_semantic_region_availability({});
        if (m_context && m_canvas->SetCurrent(*m_context)) m_semantic_model.reset();
        m_automatic_face_colors.clear();
        m_automatic_subface_colors.clear();
        m_color_trial->set_semantic_status(_L("正在本机识别人像区域，可旋转模型或取消……"), true);
        m_semantic_timer.Start(100);
    } else if (m_semantic_completion) {
        m_semantic_error = _L("相同的人像区域优化请求已存在，请等待完成或取消后重试。");
        auto callback = std::move(m_semantic_completion);
        callback(false);
    }
}

void ModelPreview3D::rebuild_semantic_preview_from_cached_result()
{
    if (!m_semantic_ready || !m_semantic_source || !m_semantic_analysis || !m_context ||
        !m_canvas->SetCurrent(*m_context)) return;
    auto regional_faces = AI::SemanticColoring::apply_semantic_region_slot_overrides(
        m_automatic_face_colors, *m_semantic_analysis, m_color_trial->semantic_region_slots(),
        m_color_trial->semantic_palette());
    auto regional_subfaces = AI::SemanticColoring::apply_semantic_region_slot_overrides(
        m_automatic_subface_colors, *m_semantic_analysis, m_color_trial->semantic_region_slots(),
        m_color_trial->semantic_palette());
    FaceColorOverrides locked;
    if (m_portrait_shapes) for (const auto& item : m_automatic_face_colors)
        if (m_portrait_shapes->locks.face_locked(item.first)) locked.push_back(item);
    if (!(m_portrait_shapes && m_portrait_shapes->derived_boundary()))
        compose_portrait_shapes(*m_semantic_analysis, m_portrait_shapes.get(), regional_faces, regional_subfaces, locked);
    auto faces = AI::SemanticColoring::compose(regional_faces, m_face_color_overrides, true);
    auto subfaces = AI::SemanticColoring::compose_subfaces(regional_subfaces, m_face_color_overrides, true);
    if (m_portrait_shapes && m_portrait_shapes->locks.leaf_domain)
        AI::preserve_locked_leaf_colors(m_portrait_shapes->locks,m_automatic_face_colors,m_automatic_subface_colors,faces,subfaces);
    AI::compose_leaf_colors(faces,subfaces,m_manual_leaf_colors);
    const auto cells=import_cell_color_overrides();
    auto geometry = build_semantic_colored_geometry(*m_semantic_source, faces, subfaces,{},
        m_portrait_shapes ? m_portrait_shapes->surface_partition.get() : nullptr,&cells);
    if (geometry.is_empty()) return;
    auto model = std::make_unique<GLModel>();
    model->init_from(std::move(geometry));
    m_semantic_model = std::move(model);
}

void ModelPreview3D::finish_semantic_coloring()
{
    if (!m_semantic_controller) { m_semantic_timer.Stop(); return; }
    if (auto result = m_semantic_controller->poll()) {
        if (m_semantic_submission_edit_revision != m_leaf_edit_revision ||
            m_semantic_submission_manual != m_face_color_overrides ||
            m_semantic_submission_unlocked != m_shapes_unlocked) {
            m_semantic_controller->cancel();
            m_semantic_timer.Stop();
            m_semantic_error = _L("已保留新的手工编辑，过期的人像优化结果未应用。");
            m_color_trial->set_semantic_status(m_semantic_error, false);
            if (m_semantic_completion) {
                auto callback = std::move(m_semantic_completion);
                callback(false);
            }
            return;
        }
        if (result->error.empty()) restore_portrait_shapes(std::move(result->shape_details), m_shapes_unlocked);
        m_semantic_analysis = std::move(result->analysis);
        m_region_runtime_identity = std::move(result->region_runtime_identity);
        if (m_semantic_analysis && result->error.empty()) {
            auto evidence = SemanticRegionEvidence::from_analysis(*m_semantic_analysis, m_region_runtime_identity);
            if (evidence && restore_semantic_region_evidence(std::move(evidence))) {
                std::string detail_error;
                auto details = SecondaryRegionEvidence::from_primary(*m_region_evidence,
                    AI::model_artifact_sha256(m_model_path), m_semantic_source->content_id, detail_error,
                    result->verified_original_source);
                restore_secondary_region_evidence(std::move(details), detail_error);
            }
        }
        bool completed = false;
        if (!result->error.empty()) {
            m_semantic_error = wxString::FromUTF8(result->error.c_str());
            BOOST_LOG_TRIVIAL(warning) << "Local semantic coloring unavailable: " << result->error;
            m_color_trial->set_semantic_status(_L("人像区域优化暂不可用，已沿用原有配色。"), false);
        } else if (result->geometry.is_empty()) {
            m_semantic_error = _L("未找到足够可靠的人像区域。");
            m_color_trial->set_semantic_status(_L("未找到足够可靠的人像区域，已沿用原有配色。"), false);
        } else if (m_context && m_canvas->SetCurrent(*m_context)) {
            m_semantic_error.clear();
            auto model = std::make_unique<GLModel>();
            model->init_from(std::move(result->geometry));
            m_semantic_model = std::move(model);
            m_automatic_face_colors = std::move(result->automatic);
            m_automatic_subface_colors = std::move(result->automatic_subfaces);
            m_semantic_ready = true;
            if (m_leaf_editing && (!m_manual_leaf_colors.empty() || !m_manual_cell_colors.empty())) refresh_leaf_colors();
            m_color_trial->set_semantic_region_availability(
                AI::SemanticColoring::semantic_region_availability(*m_semantic_analysis));
            const bool has_region_override = std::any_of(
                m_color_trial->semantic_region_slots().begin(), m_color_trial->semantic_region_slots().end(),
                [](int slot) { return slot >= 0; });
            if (has_region_override) rebuild_semantic_preview_from_cached_result();
            if (result->parent_repair_cells) {
                m_color_trial->set_semantic_status(wxString::Format(
                    _L("已按人像区域优化并统一 %llu 个已确权皮肤/衣料单元；五官边界保持冻结。"),
                    static_cast<unsigned long long>(result->parent_repair_cells)), false);
            } else {
                m_color_trial->set_semantic_status(_L("已按人像区域优化；不明确的区域沿用原配色，可在局部改色中修正。"), false);
            }
            completed = true;
        } else {
            m_semantic_error = _L("OpenGL 预览无法加载识别结果。");
            // The CPU result was consumed, but could not be adopted by the
            // preview. Invalidate the request so a later user action can retry;
            // do not keep the timer alive or reuse an older semantic surface.
            m_semantic_controller->cancel();
            m_semantic_ready = false;
            m_semantic_analysis.reset();
            m_automatic_face_colors.clear();
            m_automatic_subface_colors.clear();
            m_color_trial->set_semantic_region_availability({});
            m_color_trial->set_semantic_status(_L("人像区域预览暂不可用，已沿用原有配色。切换试色后可重试。"), false);
        }
        BOOST_LOG_TRIVIAL(info) << "Local semantic coloring completed: ms=" << result->elapsed_ms
            << ", cache_hit=" << result->cache_hit << ", auto_faces=" << m_automatic_face_colors.size()
            << ", auto_subfaces=" << m_automatic_subface_colors.size()
            << ", added_triangles=" << result->subface_added_triangles
            << ", rejected_subfaces=" << result->subface_rejected_candidates
            << ", parent_repair_cells=" << result->parent_repair_cells
            << ", parent_repair_status=" << result->parent_repair_status
            << ", person=" << result->person_detected;
        if (!result->parent_repair_audit.is_null() && !result->parent_repair_audit.empty())
            BOOST_LOG_TRIVIAL(info) << "Portrait parent cleanup audit: " << result->parent_repair_audit.dump();
        if (!result->shape_error.empty()) BOOST_LOG_TRIVIAL(warning) << "Portrait shape details unavailable: " << result->shape_error;
        BOOST_LOG_TRIVIAL(info) << "Portrait shape locks: " << (m_portrait_shapes ? m_portrait_shapes->contour_locks ?
            m_portrait_shapes->contour_locks->document.at("locks").size() : m_portrait_shapes->locks.locks.size() : 0);
        m_canvas->Refresh(false);
        if (m_semantic_completion) {
            auto callback = std::move(m_semantic_completion);
            callback(completed);
        }
    }
    if (!m_semantic_controller->busy()) m_semantic_timer.Stop();
    else m_color_trial->set_semantic_status(wxString::Format(_L("正在本机识别人像区域 · %d%%"), m_semantic_controller->progress()), true);
}
} // namespace Slic3r::GUI
