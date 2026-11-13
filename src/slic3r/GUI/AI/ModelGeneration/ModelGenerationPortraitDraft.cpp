#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "ModelGenerationPresentation.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {
struct PortraitDraftRestore {
    bool found {false};
    nlohmann::json document;
    std::shared_ptr<const PortraitShapeDetails> shapes;
    ModelPreview3D::FaceColorOverrides colors, manual;
    ModelPreview3D::SubfaceColorOverrides subfaces;
    ModelPreviewColorControls::State trial;
    ModelPreview3D::SelectionState selection;
    std::shared_ptr<VerifiedModelFiles> files;
    std::string error;
};
}

void ModelGenerationPanel::preserve_portrait_draft(bool finalizing)
{
    Presentation::assert_gui_thread();
    if (!m_portrait_draft_before || !m_model_preview) return;
    const auto sha = m_portrait_draft_before->model_sha256;
    const auto source = m_displayed_model_path;
    const auto root = boost::filesystem::path(Slic3r::data_dir()) / "cache";
    const auto shapes = m_model_preview->portrait_shape_details();
    nlohmann::json draft;
    try {
        if (!shapes || !shapes->surface_partition) throw std::runtime_error("No continuous contour draft");
        draft = {{"schema","orca.portrait-draft/v1"}, {"source_sha256",sha},
            {"geometry",m_model_preview->geometry_id()}, {"face_count",m_model_preview->triangle_count()},
            {"unlocked",m_model_preview->portrait_shapes_unlocked()},
            {"leaf_edits",m_model_preview->leaf_edit_metadata()},
            {"semantic",m_model_preview->semantic_result_metadata()},
            {"colors",m_model_preview->color_trial_metadata()},
            {"selection",m_model_preview->selection_metadata()},
            {"manual",m_model_preview->face_color_metadata()}};
    } catch (const std::exception& error) {
        if (m_finishing_status) m_finishing_status->SetLabel(_L("裁切草稿仅在本次会话保留，磁盘恢复保存失败：") + wxString::FromUTF8(error.what()));
        return;
    }
    // Each asset has one writer. Replacing a snapshot cancels only that asset,
    // while a later clear is serialized after its last possible publication.
    auto& owned_scope = m_portrait_draft_tasks[sha];
    if (!owned_scope) owned_scope = std::make_shared<Presentation::TaskScope>();
    const auto scope = owned_scope;
    auto task = scope->begin(sha, Presentation::TaskLifetime::Transaction);
    task.coalesce_pending = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    auto work = [root, source, sha, shapes, draft = std::move(draft), scope](const Presentation::TaskContext& context) mutable {
        boost::filesystem::path temporary;
        try {
            VerifiedModelFiles files;
            if (!AI::ShapeLockSet::sha256(sha) || !files.verify(source, sha))
                throw std::runtime_error("Draft source changed");
            if (context.canceled()) return std::string{};
            draft["shapes"] = PortraitShapeCache::save(*shapes, root);
            const auto ref = PortraitShapeCache::write_addressed(root,"portrait-draft-content","orca.portrait-draft-reference/v1",draft);
            if (context.canceled()) return std::string{};
            const auto directory = root/"portrait-drafts";
            if (boost::filesystem::is_symlink(directory)) throw std::runtime_error("Unsafe draft directory");
            boost::filesystem::create_directories(directory);
            const auto file = directory/(sha+".json");
            temporary = directory/boost::filesystem::unique_path("draft-%%%%-%%%%.pending");
            if (boost::filesystem::is_symlink(file)) throw std::runtime_error("Unsafe draft index");
            { boost::filesystem::ofstream out(temporary,std::ios::binary); out << ref.dump(); out.close();
              if (!out) throw std::runtime_error("Unable to persist draft"); }
            if (!context.canceled()) {
                if (!files.current()) throw std::runtime_error("Draft source changed");
                boost::filesystem::rename(temporary,file);
            }
            boost::system::error_code ignored; boost::filesystem::remove(temporary,ignored);
            return std::string{};
        } catch (const std::exception& error) {
            boost::system::error_code ignored;
            if (!temporary.empty()) boost::filesystem::remove(temporary,ignored);
            return std::string(error.what());
        }
    };
    auto complete = [weak, source, scope, task](Presentation::TaskResult<std::string> result) {
        if (result.canceled || !scope->matches(task)) return;
        if (weak) weak->m_portrait_draft_tasks.erase(task.identity);
        const auto error = result.error ? std::string("Unable to persist draft") : result.value.value_or("");
        if (error.empty()) return;
        BOOST_LOG_TRIVIAL(warning) << "Portrait draft snapshot failed: " << error;
        if (!weak || weak->m_shutdown || source != weak->m_displayed_model_path) return;
        if (weak->m_finishing_status) weak->m_finishing_status->SetLabel(_L("裁切草稿仅在本次会话保留，磁盘恢复保存失败：") + wxString::FromUTF8(error));
        if (weak->m_portrait_task) weak->m_portrait_task->evidence(weak->m_finishing_status->GetLabel().ToUTF8().data(),true);
    };
    auto& runner = Presentation::background_tasks();
    const bool accepted = finalizing ? runner.submit_finalizer(task, std::move(work), std::move(complete))
        : runner.submit(task, std::move(work), std::move(complete));
    if (!accepted) {
        BOOST_LOG_TRIVIAL(warning) << "Portrait draft snapshot queue is full";
        if (!m_shutdown && m_finishing_status) m_finishing_status->SetLabel(_L("裁切草稿仅在本次会话保留，磁盘恢复保存失败：") + wxString::FromUTF8("Unable to persist draft"));
    }
}

void ModelGenerationPanel::clear_portrait_draft()
{
    Presentation::assert_gui_thread();
    if (!m_portrait_draft_before) return;
    const auto sha = m_portrait_draft_before->model_sha256;
    if (!AI::ShapeLockSet::sha256(sha)) return;
    auto& owned_scope = m_portrait_draft_tasks[sha];
    if (!owned_scope) owned_scope = std::make_shared<Presentation::TaskScope>();
    const auto scope = owned_scope;
    auto task = scope->begin(sha, Presentation::TaskLifetime::Transaction);
    task.coalesce_pending = true;
    const auto directory = boost::filesystem::path(Slic3r::data_dir())/"cache"/"portrait-drafts";
    const bool accepted = Presentation::background_tasks().submit(task,
        [directory, sha, scope](const Presentation::TaskContext& context) {
            if (context.canceled() || boost::filesystem::is_symlink(directory)) return false;
            boost::system::error_code ignored;
            boost::filesystem::remove(directory/(sha+".json"),ignored);
            return !ignored;
        }, [weak = wxWeakRef<ModelGenerationPanel>(this), scope, task](Presentation::TaskResult<bool> result) {
            if (!scope->matches(task)) return;
            if (weak) weak->m_portrait_draft_tasks.erase(task.identity);
            if (!result.canceled && (!result.value || !*result.value))
                BOOST_LOG_TRIVIAL(warning) << "Portrait draft clear failed";
        });
    if (!accepted) BOOST_LOG_TRIVIAL(warning) << "Portrait draft clear queue is full";
}

void ModelGenerationPanel::restore_portrait_draft()
{
    Presentation::assert_gui_thread();
    if (m_role != Role::WorkbenchAssets || m_shutdown || m_beauty_restore_running ||
        !m_model_preview || !m_model_preview_ready || m_portrait_draft_before) return;
    const auto sha = m_model_preview->model_sha256();
    if (!AI::ShapeLockSet::sha256(sha)) return;
    const auto root = boost::filesystem::path(Slic3r::data_dir())/"cache";
    const auto path = m_displayed_model_path;
    const auto geometry = m_model_preview->geometry_id();
    const auto faces = m_model_preview->triangle_count();
    const auto sequence = m_sequence;
    const auto model_session = m_model_generation_session;
    const auto task = m_beauty_restore_scope.begin(sha, Presentation::TaskLifetime::Transaction);
    m_beauty_restore_running = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    const bool accepted = Presentation::background_tasks().submit(task,
        [root, path, sha, geometry, faces](const Presentation::TaskContext& context) {
            auto result = std::make_shared<PortraitDraftRestore>();
            try {
                const auto directory = root/"portrait-drafts", file = directory/(sha+".json");
                if (context.canceled() || !boost::filesystem::is_regular_file(file)) return result;
                result->found = true;
                if (boost::filesystem::is_symlink(directory) || boost::filesystem::is_symlink(file) || boost::filesystem::file_size(file)>4096)
                    throw std::runtime_error("Unsafe portrait draft reference");
                result->files = std::make_shared<VerifiedModelFiles>();
                if (!result->files->verify(path, sha) || !result->files->verify(file, AI::model_artifact_sha256(file)))
                    throw std::runtime_error("Portrait draft belongs to another source");
                nlohmann::json ref;
                { boost::filesystem::ifstream in(file); in >> ref; }
                auto& doc = result->document;
                doc = PortraitShapeCache::read_addressed(root,"portrait-draft-content","orca.portrait-draft-reference/v1",ref);
                if (doc.at("schema")!="orca.portrait-draft/v1" || doc.at("source_sha256")!=sha || doc.at("geometry")!=geometry || doc.at("face_count")!=faces)
                    throw std::runtime_error("Portrait draft belongs to another source");
                result->shapes = PortraitShapeCache::load(doc.at("shapes"),root,geometry,faces,portrait_shape_runtime_fingerprint(),true);
                if (context.canceled()) return result;
                if (!decode_semantic_result(doc.at("semantic"),geometry,faces,result->colors,result->subfaces) ||
                    !AI::ColorTrialPersistence::decode(doc.at("colors"),faces,geometry,result->trial,result->error) ||
                    !AI::SurfaceSelectionPersistence::decode_colors(doc.at("manual"),faces,geometry,result->manual,result->error) ||
                    !AI::SurfaceSelectionPersistence::decode(doc.at("selection"),faces,geometry,result->selection,result->error))
                    throw std::runtime_error("Portrait draft appearance failed validation: " + result->error);
                doc.at("unlocked").get<bool>();
                doc.at("leaf_edits");
            } catch (const std::exception& error) { result->error = error.what(); }
            return result;
        }, [weak, task, sequence, model_session, sha](Presentation::TaskResult<std::shared_ptr<PortraitDraftRestore>> outcome) {
            if (!weak || !weak->m_beauty_restore_scope.owns(task)) return;
            weak->m_beauty_restore_running = false;
            if (weak->m_shutdown || weak->m_sequence != sequence || weak->m_model_generation_session != model_session) return;
            auto result = outcome.value ? *outcome.value : nullptr;
            if (outcome.canceled || !result || !result->found) {
                weak->refresh_controls(); weak->queue_workbench_check(); return;
            }
            std::shared_ptr<BeautyCandidateSnapshot> before;
            try {
                if (!result->error.empty()) throw std::runtime_error(result->error);
                if (!result->files || !result->files->current()) throw std::runtime_error("Portrait draft belongs to another source");
                before = std::make_shared<BeautyCandidateSnapshot>(weak->capture_beauty_candidate());
                auto* preview = weak->m_model_preview;
                preview->restore_portrait_shapes(result->shapes,result->document.at("unlocked").get<bool>());
                preview->restore_manual_face_colors(std::move(result->manual));
                preview->restore_selection_state(std::move(result->selection));
                if (!preview->restore_leaf_edits(result->document.at("leaf_edits"))) throw std::runtime_error("Portrait draft selection identity changed");
                preview->restore_color_trial_without_recognition(result->trial);
                if (!preview->set_saved_semantic_result(std::move(result->colors),std::move(result->subfaces))) throw std::runtime_error("Portrait draft preview failed");
                weak->m_portrait_draft_before = before;
                weak->m_semantic_mode = SemanticMode::Portrait;
                weak->m_portrait_entered = true;
                preview->set_portrait_mode(true);
                weak->m_portrait_task = std::make_shared<PortraitOptimizationTask>(ModelGenerationPresentation::new_request_id(),sha,sequence);
                if (weak->m_beauty_controls) weak->m_beauty_controls->set_dirty(true);
                weak->finish_portrait_optimization(PortraitOutcome::DraftOnly,
                    result->shapes->surface_partition && !result->shapes->surface_partition->value("tessellation_version",0)
                    ? _L("已恢复同源裁切草稿；需要重建裁切网格。点击重试保存即可，无需重新识别。")
                    : _L("已恢复同源裁切草稿；尚未保存为可导入模型，请重试保存或放弃。"));
            } catch (const std::exception& error) {
                const auto message = _L("未恢复不兼容的裁切草稿，正式版本保留：") + wxString::FromUTF8(error.what());
                result->files.reset();
                if (before) weak->restore_beauty_candidate_async(*before, [weak, message](bool) {
                    if (weak && !weak->m_shutdown && weak->m_finishing_status) weak->m_finishing_status->SetLabel(message);
                });
                else if (weak->m_finishing_status) weak->m_finishing_status->SetLabel(message);
            }
            weak->refresh_controls(); weak->queue_workbench_check();
        }, Presentation::TaskPriority::Visible);
    if (!accepted) { m_beauty_restore_running = false; refresh_controls(); }
}
} // namespace Slic3r::GUI
