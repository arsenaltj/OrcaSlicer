#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "ModelGenerationPresentation.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem/fstream.hpp>

namespace Slic3r::GUI {
void ModelGenerationPanel::preserve_portrait_draft()
{
    if (!m_portrait_draft_before || !m_model_preview) return;
    try {
        const auto shapes = m_model_preview->portrait_shape_details();
        if (!shapes || !shapes->surface_partition) throw std::runtime_error("No continuous contour draft");
        const auto root = boost::filesystem::path(Slic3r::data_dir()) / "cache";
        const auto sha = m_portrait_draft_before->model_sha256;
        if (!AI::ShapeLockSet::sha256(sha) || AI::model_artifact_sha256(m_displayed_model_path) != sha)
            throw std::runtime_error("Draft source changed");
        nlohmann::json draft = {{"schema","orca.portrait-draft/v1"}, {"source_sha256",sha},
            {"geometry",m_model_preview->geometry_id()}, {"face_count",m_model_preview->triangle_count()},
            {"shapes",PortraitShapeCache::save(*shapes, root)},
            {"unlocked",m_model_preview->portrait_shapes_unlocked()},
            {"leaf_edits",m_model_preview->leaf_edit_metadata()},
            {"semantic",m_model_preview->semantic_result_metadata()},
            {"colors",m_model_preview->color_trial_metadata()},
            {"selection",m_model_preview->selection_metadata()},
            {"manual",m_model_preview->face_color_metadata()}};
        const auto ref = PortraitShapeCache::write_addressed(root,"portrait-draft-content","orca.portrait-draft-reference/v1",draft);
        const auto directory = root/"portrait-drafts";
        if (boost::filesystem::is_symlink(directory)) throw std::runtime_error("Unsafe draft directory");
        boost::filesystem::create_directories(directory);
        const auto file = directory/(sha+".json"), temporary = directory/(sha+".pending");
        if (boost::filesystem::is_symlink(file) || boost::filesystem::is_symlink(temporary)) throw std::runtime_error("Unsafe draft index");
        { boost::filesystem::ofstream out(temporary,std::ios::binary); out << ref.dump(); out.close();
          if (!out) throw std::runtime_error("Unable to persist draft"); }
        boost::filesystem::rename(temporary,file);
    } catch (const std::exception& error) {
        // The live draft remains editable even if durable recovery fails.
        if (m_finishing_status) m_finishing_status->SetLabel(_L("裁切草稿仅在本次会话保留，磁盘恢复保存失败：") + wxString::FromUTF8(error.what()));
        if (m_portrait_task) m_portrait_task->evidence(m_finishing_status->GetLabel().ToUTF8().data(),true);
    }
}

void ModelGenerationPanel::clear_portrait_draft()
{
    if (!m_portrait_draft_before) return;
    const auto sha = m_portrait_draft_before->model_sha256;
    if (!AI::ShapeLockSet::sha256(sha)) return;
    const auto directory = boost::filesystem::path(Slic3r::data_dir())/"cache"/"portrait-drafts";
    if (boost::filesystem::is_symlink(directory)) return;
    boost::system::error_code ignored;
    boost::filesystem::remove(directory/(sha+".json"),ignored);
}

void ModelGenerationPanel::restore_portrait_draft()
{
    if (!m_model_preview || !m_model_preview_ready || m_portrait_draft_before) return;
    std::shared_ptr<BeautyCandidateSnapshot> before;
    try {
        const auto root = boost::filesystem::path(Slic3r::data_dir())/"cache";
        const auto sha = AI::model_artifact_sha256(m_displayed_model_path);
        if (!AI::ShapeLockSet::sha256(sha)) return;
        const auto directory = root/"portrait-drafts", file = directory/(sha+".json");
        if (!boost::filesystem::is_regular_file(file)) return;
        if (boost::filesystem::is_symlink(directory) || boost::filesystem::is_symlink(file) || boost::filesystem::file_size(file)>4096)
            throw std::runtime_error("Unsafe portrait draft reference");
        nlohmann::json ref;
        { boost::filesystem::ifstream in(file); in >> ref; }
        const auto doc = PortraitShapeCache::read_addressed(root,"portrait-draft-content","orca.portrait-draft-reference/v1",ref);
        const auto geometry = m_model_preview->geometry_id();
        const auto faces = m_model_preview->triangle_count();
        if (doc.at("schema")!="orca.portrait-draft/v1" || doc.at("source_sha256")!=sha || doc.at("geometry")!=geometry || doc.at("face_count")!=faces)
            throw std::runtime_error("Portrait draft belongs to another source");
        auto shapes = PortraitShapeCache::load(doc.at("shapes"),root,geometry,faces,portrait_shape_runtime_fingerprint(),true);
        ModelPreview3D::FaceColorOverrides colors, manual;
        ModelPreview3D::SubfaceColorOverrides subfaces;
        ModelPreviewColorControls::State trial;
        ModelPreview3D::SelectionState selection;
        std::string error;
        if (!decode_semantic_result(doc.at("semantic"),geometry,faces,colors,subfaces) ||
            !AI::ColorTrialPersistence::decode(doc.at("colors"),faces,geometry,trial,error) ||
            !AI::SurfaceSelectionPersistence::decode_colors(doc.at("manual"),faces,geometry,manual,error) ||
            !AI::SurfaceSelectionPersistence::decode(doc.at("selection"),faces,geometry,selection,error))
            throw std::runtime_error("Portrait draft appearance failed validation: " + error);
        before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        m_model_preview->restore_portrait_shapes(shapes,doc.at("unlocked").get<bool>());
        m_model_preview->restore_manual_face_colors(std::move(manual));
        m_model_preview->restore_selection_state(std::move(selection));
        if (!m_model_preview->restore_leaf_edits(doc.at("leaf_edits"))) throw std::runtime_error("Portrait draft selection identity changed");
        m_model_preview->restore_color_trial_without_recognition(trial);
        if (!m_model_preview->set_saved_semantic_result(colors,subfaces)) throw std::runtime_error("Portrait draft preview failed");
        m_portrait_draft_before = before;
        m_semantic_mode = SemanticMode::Portrait;
        m_portrait_entered = true;
        m_model_preview->set_portrait_mode(true);
        m_portrait_task = std::make_shared<PortraitOptimizationTask>(ModelGenerationPresentation::new_request_id(),sha,m_sequence);
        if (m_beauty_controls) m_beauty_controls->set_dirty(true);
        finish_portrait_optimization(PortraitOutcome::DraftOnly,
            shapes->surface_partition && !shapes->surface_partition->value("tessellation_version",0)
            ? _L("已恢复同源裁切草稿；需要重建裁切网格。点击重试保存即可，无需重新识别。")
            : _L("已恢复同源裁切草稿；尚未保存为可导入模型，请重试保存或放弃。"));
    } catch (const std::exception& error) {
        if (before) restore_beauty_candidate(*before);
        if (m_finishing_status) m_finishing_status->SetLabel(_L("未恢复不兼容的裁切草稿，正式版本保留：") + wxString::FromUTF8(error.what()));
    }
}
} // namespace Slic3r::GUI
