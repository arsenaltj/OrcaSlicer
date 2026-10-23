#include "ModelPreview3D.hpp"
#include "BeautyColorPaint.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/stdpaths.h>

namespace Slic3r::GUI {
void ModelPreview3D::capture_printer_filament_restore_snapshot()
{
    if (m_printer_filament_restore || m_geometry_id.empty() || !m_color_trial) return;
    PrinterFilamentRestoreSnapshot snapshot;
    snapshot.color_blocks=m_color_blocks;
    snapshot.geometry_id = m_geometry_id;
    snapshot.color_trial = m_color_trial->state();
    // Keep the in-memory source verbatim.  The trial must be able to restore
    // the exact automatic, printer, manual, or saved-card source after the
    // user leaves printer-filament preview.  Persistence normalizes a copy
    // below because dynamic saved-card indices are not stable on disk.
    snapshot.face_color_overrides = m_face_color_overrides;
    snapshot.automatic_face_colors = m_automatic_face_colors;
    snapshot.automatic_subface_colors = m_automatic_subface_colors;
    snapshot.saved_semantic_faces = m_saved_semantic_faces;
    snapshot.saved_semantic_subfaces = m_saved_semantic_subfaces;
    snapshot.portrait_shapes = m_portrait_shapes;
    snapshot.shapes_unlocked = m_shapes_unlocked;
    snapshot.leaf_edits = leaf_edit_metadata();
    m_printer_filament_restore = std::move(snapshot);
}

bool ModelPreview3D::restore_printer_filament_preview_source()
{
    if (!m_printer_filament_restore) return true;
    const auto target_state = m_color_trial ? m_color_trial->state() : ModelPreviewColorControls::State{};
    if (m_printer_filament_restore->geometry_id != m_geometry_id || !m_color_trial) {
        m_printer_filament_restore.reset();
        return false;
    }
    const auto& snapshot = *m_printer_filament_restore;

    m_color_blocks=snapshot.color_blocks;
    m_face_color_overrides = snapshot.face_color_overrides;
    m_automatic_face_colors = snapshot.automatic_face_colors;
    m_automatic_subface_colors = snapshot.automatic_subface_colors;
    m_saved_semantic_faces = snapshot.saved_semantic_faces;
    m_saved_semantic_subfaces = snapshot.saved_semantic_subfaces;
    m_portrait_shapes = snapshot.portrait_shapes;
    m_shapes_unlocked = snapshot.shapes_unlocked;
    // Keep the source the user just selected (automatic, manual, or a saved
    // trial card). Only the appearance layers are restored from before the
    // printer application.
    m_color_trial_enabled = target_state.enabled;
    m_trial_palette = m_color_trial->colors();
    if (!snapshot.leaf_edits.is_null() && !restore_leaf_edits(snapshot.leaf_edits)) {
        m_printer_filament_restore.reset();
        return false;
    }
    if (!m_face_color_overrides.empty()) {
        if (!restore_beauty_face_colors(m_face_color_overrides)) {
            m_printer_filament_restore.reset();
            return false;
        }
    } else {
        m_manual_color_model.reset();
    }
    m_semantic_model.reset();
    if (m_canvas) m_canvas->Refresh(false);
    m_printer_filament_restore.reset();
    BOOST_LOG_TRIVIAL(info) << "AI restored pre-printer appearance: source="
        << m_color_trial->state().source << ", colors=" << m_trial_palette.size();
    return true;
}

bool ModelPreview3D::restore_printer_filament_input_source()
{
    if (!m_printer_filament_restore) return true;
    const auto snapshot = *m_printer_filament_restore;
    if (!m_color_trial || snapshot.geometry_id != m_geometry_id) {
        m_printer_filament_restore.reset();
        return false;
    }

    // Rebuild the color-control source as well as the appearance layers. The
    // normal source-switch path intentionally keeps the printer source
    // selected for display, but a second Apply must use the original model
    // colors as matching input.
    m_restoring_printer_input = true;
    m_suppress_semantic_change = true;
    m_color_trial->restore(snapshot.color_trial);
    m_suppress_semantic_change = false;
    m_restoring_printer_input = false;
    m_color_blocks=snapshot.color_blocks;
    m_face_color_overrides = snapshot.face_color_overrides;
    m_automatic_face_colors = snapshot.automatic_face_colors;
    m_automatic_subface_colors = snapshot.automatic_subface_colors;
    m_saved_semantic_faces = snapshot.saved_semantic_faces;
    m_saved_semantic_subfaces = snapshot.saved_semantic_subfaces;
    m_portrait_shapes = snapshot.portrait_shapes;
    m_shapes_unlocked = snapshot.shapes_unlocked;
    if (!snapshot.leaf_edits.is_null() && !restore_leaf_edits(snapshot.leaf_edits)) {
        // A failed restore cannot be retried against a partially rebuilt
        // appearance. Drop the snapshot so a later trial cannot reuse stale
        // geometry-bound state.
        m_printer_filament_restore.reset();
        return false;
    }
    if (!m_face_color_overrides.empty()) {
        if (!restore_beauty_face_colors(m_face_color_overrides)) {
            m_printer_filament_restore.reset();
            return false;
        }
    } else {
        m_manual_color_model.reset();
    }
    m_semantic_model.reset();
    m_color_trial_enabled = m_color_trial->enabled();
    m_trial_palette = m_color_trial->colors();
    if (m_canvas) m_canvas->Refresh(false);
    m_printer_filament_restore.reset();
    return true;
}

bool ModelPreview3D::exit_printer_filament_trial()
{
    if (!m_printer_filament_restore) return true;
    if (!m_color_trial || m_printer_filament_restore->geometry_id != m_geometry_id) {
        m_printer_filament_restore.reset();
        return false;
    }
    // Restore the source selection first while suppressing the normal source
    // callback.  The regular preview-source restore then reinstates the
    // captured appearance layers and releases the snapshot atomically.
    const auto source = m_printer_filament_restore->color_trial;
    m_suppress_semantic_change = true;
    m_color_trial->restore(source);
    m_suppress_semantic_change = false;
    return restore_printer_filament_preview_source();
}

nlohmann::json ModelPreview3D::printer_filament_restore_metadata() const
{
    if (!m_printer_filament_restore || m_printer_filament_restore->geometry_id != m_geometry_id) return {};
    const auto& snapshot = *m_printer_filament_restore;
    auto persisted_trial = snapshot.color_trial;
    AI::ColorTrialPersistence::normalize_for_persistence(persisted_trial);
    auto semantic = [&](const FaceColorOverrides& faces, const SubfaceColorOverrides& subfaces) {
        nlohmann::json result = {{"schema", "orca.semantic-result/v1"}, {"geometry_id", m_geometry_id},
            {"face_count", m_triangle_count}, {"faces", nlohmann::json::array()}, {"subfaces", nlohmann::json::array()}};
        for (const auto& item : faces) result["faces"].push_back({item.first, item.second});
        for (const auto& item : subfaces) result["subfaces"].push_back({item.face_id, item.path.depth, item.path.value, item.color});
        return result;
    };
    nlohmann::json result = {{"schema", "orca.printer-filament-restore/v1"}, {"geometry_id", m_geometry_id},
        {"color_trial", AI::ColorTrialPersistence::encode(persisted_trial, m_triangle_count, m_geometry_id)},
        {"face_color_intent", AI::SurfaceSelectionPersistence::encode_colors(snapshot.face_color_overrides, m_triangle_count, m_geometry_id)},
        {"semantic_result", semantic(snapshot.saved_semantic_faces, snapshot.saved_semantic_subfaces)},
        {"automatic_semantic_result", semantic(snapshot.automatic_face_colors, snapshot.automatic_subface_colors)},
        {"leaf_edits", snapshot.leaf_edits}, {"shapes_unlocked", snapshot.shapes_unlocked}};
    if (snapshot.portrait_shapes && snapshot.portrait_shapes->compatible(m_geometry_id, m_triangle_count)) {
        auto reference = PortraitShapeCache::save(*snapshot.portrait_shapes, boost::filesystem::path(Slic3r::data_dir()) / "cache");
        reference["model_sha256"] = AI::model_artifact_sha256(m_model_path);
        result["portrait_shape_reference"] = std::move(reference);
    }
    if(snapshot.color_blocks)result["portrait_color_blocks"]=PortraitColorBlocks::save(*snapshot.color_blocks,
        boost::filesystem::path(Slic3r::data_dir())/"cache"/"portrait-color-blocks");
    return result;
}

bool ModelPreview3D::apply_project_filament_palette(const std::vector<size_t>* bindings)
{
    capture_printer_filament_restore_snapshot();
    const auto before = m_color_trial->state();
    m_suppress_semantic_change = true;
    const bool applied = bindings ? m_color_trial->apply_project_filament_bindings(*bindings) : m_color_trial->apply_project_filament_palette();
    m_suppress_semantic_change = false;
    if (!applied) return false;
    m_color_blocks.reset();m_selected_color_block.reset();
    const auto& palette = m_color_trial->project_colors();
    const auto after = m_color_trial->state();
    auto recolor = [&palette, &before, &after, bindings](auto& color) {
        const auto& target = bindings
            ? after.colors[PrinterFilamentPreview::nearest({color[0], color[1], color[2]}, before.colors)]
            : palette[PrinterFilamentPreview::nearest({color[0], color[1], color[2]}, palette)];
        for (size_t i = 0; i < 3; ++i) color[i] = target[i];
    };
    auto recolor_semantic = [&](auto& color) {
        if (bindings && !before.semantic_palette.empty()) {
            const auto& target = after.semantic_palette[PrinterFilamentPreview::nearest({color[0], color[1], color[2]}, before.semantic_palette)];
            for (size_t i = 0; i < 3; ++i) color[i] = target[i];
        } else recolor(color);
    };
    for (auto& item : m_face_color_overrides) recolor(item.second);
    for (auto& item : m_automatic_face_colors) recolor_semantic(item.second);
    for (auto& item : m_automatic_subface_colors) recolor_semantic(item.color);
    for (auto& item : m_manual_leaf_colors) recolor(item.second);
    for (auto& item : m_manual_cell_colors) recolor(item.second);
    if (m_portrait_shapes) {
        auto shapes = std::make_shared<PortraitShapeDetails>(*m_portrait_shapes);
        for (auto& item : shapes->cell_colors) recolor(item.second);
        m_portrait_shapes = std::move(shapes);
    }
    if (!m_face_color_overrides.empty() && !restore_beauty_face_colors(m_face_color_overrides)) return false;
    ++m_leaf_edit_revision;
    if (m_leaf_editing) refresh_leaf_colors();
    else m_semantic_model.reset();
    m_canvas->Refresh(false);
    return true;
}

ModelPreview3D::FaceColorOverrides ModelPreview3D::printer_filament_export_faces() const
{
    auto result = import_face_color_overrides(true);
    const auto trial = m_color_trial->state();
    if (trial.source != 1 || !project_matching_valid()) return result;
    const auto& mesh = m_semantic_source ? m_semantic_source->mesh :
        m_region_editor->ready() ? m_region_editor->mesh() : m_pending_mesh;
    const auto& vertex = m_semantic_source ? m_semantic_source->vertex_colors :
        m_region_editor->ready() ? m_region_editor->vertex_colors() : m_pending_vertex_colors;
    std::unordered_set<size_t> assigned;
    for (const auto& item : result) assigned.insert(item.first);
    for (size_t face = 0; face < mesh.indices.size(); ++face) {
        if (assigned.count(face)) continue;
        PreviewPalette::Color color{.7f, .7f, .7f};
        if (m_semantic_source && m_semantic_source->face_colors.size() == mesh.indices.size()) {
            const auto& rgba = m_semantic_source->face_colors[face];
            color = {rgba[0], rgba[1], rgba[2]};
        } else if (vertex.size() == mesh.vertices.size()) {
            color = {0.f, 0.f, 0.f};
            for (const int v : mesh.indices[face]) for (size_t c = 0; c < 3; ++c) color[c] += vertex[size_t(v)][c] / 3.f;
        }
        result.emplace_back(face, trial.colors[PrinterFilamentPreview::nearest(color, trial.mapping_colors)]);
    }
    return result;
}

bool ModelPreview3D::restore_beauty_face_colors(const FaceColorOverrides& colors)
{
    if (!m_has_model || !m_context || !m_canvas->SetCurrent(*m_context)) return false;
    try {
        const indexed_triangle_set* mesh = m_semantic_source ? &m_semantic_source->mesh :
            m_region_editor->ready() ? &m_region_editor->mesh() :
            current_region_preparation() ? &m_region_preparation->mesh :
            !m_pending_mesh.indices.empty() ? &m_pending_mesh : nullptr;
        if (!mesh) return false;
        std::unique_ptr<GLModel> model;
        if (!colors.empty()) {
            if (m_manual_corner_normals.size() != mesh->indices.size() * 3)
                m_manual_corner_normals = ModelPreviewNormals::corner_normals(*mesh);
            GLModel::Geometry geometry;
            geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
            geometry.reserve_vertices(colors.size() * 3);
            geometry.reserve_indices(colors.size() * 3);
            for (const auto& item : colors) {
                if (item.first >= mesh->indices.size()) return false;
                const auto& face = mesh->indices[item.first];
                const auto packed = preview_rgb8(item.second[0], item.second[1], item.second[2]);
                const auto base = uint32_t(geometry.vertices_count());
                for (size_t corner = 0; corner < 3; ++corner)
                    geometry.add_vertex(mesh->vertices[face[corner]], m_manual_corner_normals[item.first * 3 + corner],
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

bool ModelPreview3D::restore_portrait_color_blocks(std::shared_ptr<const PortraitColorBlocks::Result> blocks,bool refresh)
{
    if(blocks && (blocks->geometry_id!=m_geometry_id || blocks->face_count!=m_triangle_count))return false;
    (void)refresh;
    m_color_blocks=std::move(blocks);
    m_selected_color_block.reset();
    return true;
}

bool ModelPreview3D::switch_portrait_color_track(PortraitColorBlocks::Track track)
{
    if(!m_color_blocks || semantic_processing() || selection_busy())return false;
    auto copy=std::make_shared<PortraitColorBlocks::Result>(*m_color_blocks);copy->active=track;
    if(!restore_portrait_color_blocks(std::move(copy)))return false;
    m_selected_color_block.reset();m_selection_model.reset();
    ++m_leaf_edit_revision;
    if(m_portrait_input_changed)m_portrait_input_changed();
    return true;
}

bool ModelPreview3D::select_portrait_color_block(uint32_t id)
{
    if(!m_color_blocks || !m_semantic_source || id>=m_color_blocks->current().blocks.size() ||
        !m_context || !m_canvas->SetCurrent(*m_context))return false;
    GLModel::Geometry geometry;
    geometry.format={GLModel::Geometry::EPrimitiveType::Triangles,GLModel::Geometry::EVertexLayout::P3N3};
    BoundingBoxf3 bounds;
    const auto& mesh=m_semantic_source->mesh;
    for(const auto member:m_color_blocks->current().blocks[id].members) {
        const auto& unit=m_color_blocks->units[member];const auto bary=PortraitColorBlocks::corners(unit.path);
        std::array<Vec3f,3> positions;
        for(int k=0;k<3;++k) {
            positions[k]=Vec3f::Zero();
            for(int c=0;c<3;++c)positions[k]+=bary[k][c]*mesh.vertices[mesh.indices[unit.face][c]];
            bounds.merge(positions[k].cast<double>());
        }
        Vec3f normal=(positions[1]-positions[0]).cross(positions[2]-positions[0]);
        if(normal.squaredNorm()>1e-12f)normal.normalize();else normal=Vec3f::UnitZ();
        const auto base=uint32_t(geometry.vertices_count());
        for(const auto& v:positions)geometry.add_vertex(v,normal);
        geometry.add_triangle(base,base+1,base+2);
    }
    if(geometry.is_empty())return false;
    auto model=std::make_unique<GLModel>();model->init_from(std::move(geometry));
    model->set_color(ColorRGBA(.95f,.75f,.1f,1.f));m_selection_model=std::move(model);
    m_selected_color_block=id;m_selection_enabled=true;m_selection_overlay_visible=true;m_selection_preview_suppressed=false;
    const auto rotation=view_rotation();const Vec3d offset=rotation.linear()*(bounds.center()-m_bounds.center()).cast<double>();
    const double radius=std::max(.001,.5*m_bounds.size().norm());m_pan_x=-offset.x()/radius;m_pan_y=-offset.y()/radius;
    m_canvas->Refresh(false);return true;
}

bool ModelPreview3D::recolor_portrait_color_block(uint32_t id,const std::array<float,3>& color)
{
    if(!m_color_blocks || semantic_processing() || selection_busy())return false;
    auto copy=std::make_shared<PortraitColorBlocks::Result>(*m_color_blocks);
    if(!copy->recolor(id,color))return false;
    // Explicit block edits supersede earlier root paint only on affected roots.
    // Every other unit retains its captured ordinary appearance.
    const auto old_faces=m_face_color_overrides;
    const auto old_leaves=m_manual_leaf_colors;
    std::set<size_t> roots;
    for(auto member:copy->current().blocks[id].members)roots.insert(copy->units[member].face);
    m_face_color_overrides.erase(std::remove_if(m_face_color_overrides.begin(),m_face_color_overrides.end(),
        [&](const auto& f){return roots.count(f.first);}),m_face_color_overrides.end());
    // The captured ordinary track already contains the old edit appearance.
    // Remove only intersecting edit keys so they cannot mask an explicit block
    // color, while unaffected units keep their captured colors.
    for(auto it=m_manual_leaf_colors.begin();it!=m_manual_leaf_colors.end();) {
        bool overlap=false;
        for(auto member:copy->current().blocks[id].members) {
            const auto& unit=copy->units[member];const auto& key=it->first;
            if(unit.face!=key.source_face_id)continue;
            const auto a=unit.path,b=AI::SemanticColoring::SubfacePath{key.depth,key.path};
            overlap = a.depth<=b.depth ? (b.value>>(2*(b.depth-a.depth)))==a.value :
                (a.value>>(2*(a.depth-b.depth)))==b.value;
            if(overlap)break;
        }
        if(overlap)it=m_manual_leaf_colors.erase(it);else ++it;
    }
    if(!restore_portrait_color_blocks(std::move(copy))) {m_face_color_overrides=old_faces;m_manual_leaf_colors=old_leaves;return false;}
    restore_beauty_face_colors(m_face_color_overrides);
    refresh_leaf_colors();
    m_selected_color_block.reset();m_selection_model.reset();++m_leaf_edit_revision;
    if(m_portrait_input_changed)m_portrait_input_changed();
    return true;
}

nlohmann::json ModelPreview3D::portrait_color_blocks_metadata() const
{
    if(!m_color_blocks)return {};
    return PortraitColorBlocks::save(*m_color_blocks,boost::filesystem::path(Slic3r::data_dir())/"cache"/"portrait-color-blocks");
}

void ModelPreview3D::update_semantic_coloring()
{
    if (!portrait_recognition_enabled() || !m_has_model || !m_semantic_source || !m_color_trial_enabled || !m_color_trial->color_block_postprocess_enabled()) {
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_color_trial->set_semantic_region_availability({});
        m_color_trial->set_semantic_status(wxEmptyString, false);
        if (m_semantic_completion) {
            m_semantic_error = semantic_reoptimization_reason();
            if (m_semantic_error.empty()) m_semantic_error = _L("自动划区语义识别当前不可用，请重新确认模型输入。");
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
    auto saved=std::make_shared<ModelSemanticColoring::SavedAppearance>();
    saved->geometry_id=m_semantic_source->geometry_id;saved->face_count=m_semantic_source->mesh.indices.size();
    if(m_color_blocks) {
        auto baseline=*m_color_blocks;baseline.active=PortraitColorBlocks::Track::ProcessedFaceColor;
        for(auto& b:baseline.tracks[1].blocks)b.display=b.original;
        baseline.appearance(saved->faces,saved->subfaces);
        saved->color_blocks=m_color_blocks;
    } else if(m_semantic_ready) {
        saved->faces=import_face_color_overrides(true);saved->subfaces=import_subface_color_overrides(true);
    }
    AI::compose_leaf_colors(saved->faces,saved->subfaces,m_manual_leaf_colors);
    if (m_semantic_controller->request(m_semantic_source, m_color_trial->mapping_colors(), m_color_trial->colors(),
                                      m_color_trial->semantic_portrait_card(), m_face_color_overrides,
                                      std::filesystem::path(m_model_path.native()), m_portrait_shapes,m_color_trial->semantic_palette_roles(),
                                      !m_shapes_unlocked && m_manual_leaf_colors.empty() && m_manual_cell_colors.empty(),
                                       m_manual_cell_colors, m_portrait_progress,std::move(saved),
                                       size_t(std::clamp(m_color_trial->automatic_count(), 1, int(PreviewPalette::max_preview_colors))))) {
        m_semantic_submission_edit_revision = m_leaf_edit_revision;
        m_semantic_submission_manual = m_face_color_overrides;
        m_semantic_submission_unlocked = m_shapes_unlocked;
        // Retain the current immutable preview throughout recognition. Adoption
        // below is transactional; a failed job leaves it untouched.
        m_color_trial->set_semantic_status(_L("正在本机识别人像区域，可旋转模型或取消……"), true);
        m_semantic_timer.Start(100);
    } else if (m_semantic_completion) {
        m_semantic_error = _L("相同的自动划区语义识别请求已存在，请等待完成或取消后重试。");
        auto callback = std::move(m_semantic_completion);
        callback(false);
    }
}

void ModelPreview3D::rebuild_semantic_preview_from_cached_result()
{
    if (m_leaf_editing && (!m_manual_leaf_colors.empty() || !m_manual_cell_colors.empty())) {
        refresh_leaf_colors();
        return;
    }
    m_semantic_model.reset();
    m_semantic_ready = false;
    if (m_canvas) m_canvas->Refresh(false);
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
        bool completed = false;
        if (!result->error.empty()) {
            m_semantic_error = wxString::FromUTF8(result->error.c_str());
            BOOST_LOG_TRIVIAL(warning) << "Local semantic coloring unavailable: " << result->error;
            m_color_trial->set_semantic_status(_L("自动划区语义识别暂不可用；原生配色保持不变。"), false);
        } else if (result->analysis_only && result->analysis) {
            m_semantic_error.clear();
            m_semantic_analysis = std::move(result->analysis);
            m_region_runtime_identity = std::move(result->region_runtime_identity);
            m_person_detected = result->person_detected;
            auto evidence = SemanticRegionEvidence::from_analysis(*m_semantic_analysis, m_region_runtime_identity);
            if (evidence && restore_semantic_region_evidence(std::move(evidence))) {
                std::string detail_error;
                auto details = SecondaryRegionEvidence::from_primary(*m_region_evidence,
                    AI::model_artifact_sha256(m_model_path), m_semantic_source->content_id, detail_error,
                    result->verified_original_source);
                restore_secondary_region_evidence(std::move(details), detail_error);
            }
            m_color_trial->set_semantic_region_availability(
                AI::SemanticColoring::semantic_region_availability(*m_semantic_analysis));
            m_color_trial->set_semantic_status(m_person_detected
                ? _L("自动划区语义标签已就绪；颜色和区域边界未改变。")
                : _L("未检测到人像；将继续使用纯颜色自动划区。"), false);
            completed = true;
        } else if (result->geometry.is_empty()) {
            m_semantic_error = _L("未找到足够可靠的人像区域。");
            m_color_trial->set_semantic_status(_L("未找到足够可靠的人像区域，已沿用原有配色。"), false);
        } else if (m_context && m_canvas->SetCurrent(*m_context)) {
            m_semantic_error.clear();
            auto model = std::make_unique<GLModel>();
            model->init_from(std::move(result->geometry));
        if (result->error.empty()) restore_portrait_shapes(std::move(result->shape_details), m_shapes_unlocked);
        m_semantic_analysis = std::move(result->analysis);
        m_region_runtime_identity = std::move(result->region_runtime_identity);
        m_person_detected = result->person_detected;
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

            m_semantic_model = std::move(model);
            m_color_blocks=std::move(result->color_blocks);
            m_selected_color_block.reset();
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
            if(m_color_blocks) {
                m_color_trial->set_semantic_status(wxString::Format(_L("自动划区语义识别完成 · 已读取 %llu 个兼容旧区域。"),
                    static_cast<unsigned long long>(m_color_blocks->unresolved_count())),false);
            } else if (result->parent_repair_cells) {
                m_color_trial->set_semantic_status(wxString::Format(
                    _L("自动划区语义识别完成 · 已读取 %llu 个兼容旧单元。"),
                    static_cast<unsigned long long>(result->parent_repair_cells)), false);
            } else {
                m_color_trial->set_semantic_status(_L("自动划区语义标签已就绪；原生配色未改变。"), false);
            }
            completed = true;
        } else {
            m_semantic_error = _L("OpenGL 预览无法加载识别结果。");
            // The CPU result was consumed, but could not be adopted by the
            // preview. Invalidate the request so a later user action can retry;
            // do not keep the timer alive or reuse an older semantic surface.
            m_semantic_controller->cancel();
            // Keep the previous immutable appearance if GPU adoption fails.
            m_color_trial->set_semantic_status(_L("自动划区语义预览暂不可用；原生配色保持不变。"), false);
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
