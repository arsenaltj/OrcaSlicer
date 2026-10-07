#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "LocalModelImportState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/filedlg.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
void ModelGenerationPanel::choose_local_model() {
    if(m_busy || m_shutdown)return;
    if(m_beauty_controls && m_beauty_controls->has_changes()) {
        m_status->SetLabel(_L("请先保存当前美颜修改，再导入其他模型。"));return;
    }
    wxFileDialog picker(this,_L("导入到历史资产"),wxEmptyString,wxEmptyString,
        _L("全彩模型 (*.glb;*.obj)|*.glb;*.obj"),wxFD_OPEN|wxFD_FILE_MUST_EXIST);
    if(picker.ShowModal()!=wxID_OK)return;
    import_local_model(boost::filesystem::path(picker.GetPath().ToStdWstring()), false);
}

void ModelGenerationPanel::import_local_model(const boost::filesystem::path& source, bool open_beauty) {
    if(m_busy || m_shutdown)return;
    if(m_beauty_controls && m_beauty_controls->has_changes()) {
        m_status->SetLabel(_L("请先保存当前美颜修改，再导入其他模型。"));return;
    }
    const std::string id="finish-import-"+new_request_id();
    const auto destination=temp_path(id,"glb"),metadata_path=library_metadata_path(id),root=generated_models_root();
    const auto reference_image_path=open_beauty && !m_reference_image_path.empty() && path_is_inside(root,m_reference_image_path)
        ? m_reference_image_path : boost::filesystem::path();
    const auto ai_image_path=open_beauty && !m_raw_preview_path.empty() && path_is_inside(root,m_raw_preview_path)
        ? m_raw_preview_path : boost::filesystem::path();
    if(m_library_import_worker.joinable())m_library_import_worker.join();
    auto* operation = local_model_import_state(m_library_import);
    if (!operation) return;
    const auto canceled = std::make_shared<std::atomic<bool>>(false);
    operation->cancel = canceled;
    operation->open_beauty = open_beauty;
    operation->parsing = false;
    const auto owned = std::make_shared<std::pair<bool,bool>>(false,false);
    operation->rollback_after_join = [destination,metadata_path,owned] {
        boost::system::error_code ignored;
        if (owned->second) boost::filesystem::remove(metadata_path,ignored);
        if (owned->first) boost::filesystem::remove(destination,ignored);
    };
    m_busy=true;
    const wxString progress=open_beauty
        ? _L("正在本地创建可美颜的 GLB 副本，原模型保持不变……")
        : _L("正在导入模型和贴图到历史资产，原文件保持不变……");
    m_status->SetLabel(progress);
    if(open_beauty)m_model_preview_message->SetLabel(progress);
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {m_library_import_worker=std::thread([weak,source,destination,metadata_path,root,id,
                                              reference_image_path,ai_image_path,open_beauty,canceled,owned] {
        std::string error;
        bool archived=false, record_owned=false, success=false;
        // Only these freshly created paths are ours. Never delete the source,
        // an existing history entry, or the current workbench's draft.
        auto cleanup = [destination,metadata_path](bool model_owned, bool metadata_owned) {
            boost::system::error_code ignored;
            if (metadata_owned) boost::filesystem::remove(metadata_path,ignored);
            if (model_owned) boost::filesystem::remove(destination,ignored);
        };
        try {
            if (!canceled->load() && !boost::filesystem::exists(metadata_path))
                success=archived=AI::archive_local_model(source,destination,error);
            owned->first=archived;
            if(success && !canceled->load()) {
            nlohmann::json metadata={{"schema_version",4},{"job_id",id},{"source","local_import"},
                {"prompt",open_beauty?"3D 美颜副本":source.filename().string()},
                {"model_path",destination.lexically_relative(root).generic_string()},
                {"model_sha256",AI::model_artifact_sha256(destination)},{"generated_at",std::time(nullptr)},
                {"palette",nlohmann::json::array()},{"palette_roles",nlohmann::json::object()},{"use_printable_colors",false}};
            if(open_beauty && path_is_inside(root,source)) {
                metadata["source_model"]=source.lexically_relative(root).generic_string();
                metadata["source_sha256"]=AI::model_artifact_sha256(source);
            }
            if(!reference_image_path.empty())
                metadata["reference_image_path"]=reference_image_path.lexically_relative(root).generic_string();
            if(!ai_image_path.empty())
                metadata["ai_image_path"]=ai_image_path.lexically_relative(root).generic_string();
            // The operation has a unique ID, checked absent before archive.
            // Keep ownership even if the stream fails after creating a record.
            if (boost::filesystem::exists(metadata_path)) throw std::runtime_error("New local model record already exists");
            owned->second=record_owned=true;
            success=write_json(metadata_path,metadata);if(!success)error="模型记录保存失败，请检查磁盘空间。";
        }}catch(const std::exception& e){success=false;error=e.what();}
        if(!success || canceled->load()) cleanup(archived,record_owned);
        if(!success && !canceled->load())
            BOOST_LOG_TRIVIAL(warning) << "Local model import failed at "
                << (archived ? "metadata persistence" : "model archive") << ": " << error;
        wxGetApp().CallAfter([weak,destination,id,success,archived,record_owned,cleanup,canceled,reference_image_path,ai_image_path,open_beauty] {
            // Cancel can arrive after worker completion but before this callback.
            if(!weak || weak->m_shutdown || canceled->load()) cleanup(archived,record_owned);
            if(!weak || weak->m_shutdown)return;
            auto* operation=local_model_import_state(weak->m_library_import);
            if (!operation || operation->cancel != canceled) return;
            if(weak->m_library_import_worker.joinable())weak->m_library_import_worker.join();
            weak->m_busy=false;
            if(canceled->load()) {
                complete_local_model_import(weak->m_library_import,canceled,false);
                weak->refresh_controls();
                const wxString message=open_beauty
                    ? _L("已取消创建副本，原模型和当前编辑已保留。")
                    : _L("已取消本地导入，原文件和当前模型已保留。");
                weak->m_status->SetLabel(message);
                if(open_beauty)weak->m_model_preview_message->SetLabel(message);
                return;
            }
            if(!success){
                complete_local_model_import(weak->m_library_import,canceled,false);
                weak->refresh_controls();
                const wxString failure=archived
                    ? _L("无法保存到历史资产。请检查保存目录和磁盘空间后重试；原文件和当前模型保持不变。")
                    : _L("无法导入这个模型。请检查 GLB/OBJ 是否包含有效几何、关联贴图是否齐全，并确认保存目录可写；原文件和当前模型保持不变。");
                weak->m_status->SetLabel(failure);
                if(open_beauty)weak->m_model_preview_message->SetLabel(failure);
                return;
            }
            // Keep the local token through parsing: neither phase may stop
            // a remote generation job or commit a late canceled preview.
            operation->parsing=true;
            weak->load_library_entries();
            weak->load_library_entry(destination,reference_image_path,ai_image_path,
                {},{},false,{},{},{},id,
                open_beauty?_L("3D 美颜副本"):_L("本地导入模型"),open_beauty);
            // A guarded/refused load did not start a preview worker.
            if(operation->cancel==canceled && !weak->m_preview_loading) {
                complete_local_model_import(weak->m_library_import,canceled,false);
                weak->refresh_controls();
            }
        });
    });}catch(const std::exception& e){
        BOOST_LOG_TRIVIAL(warning) << "Cannot start local model import: " << e.what();
        operation->cancel.reset();
        operation->rollback_after_join={};
        m_busy=false;
        refresh_controls();
        const wxString failure=_L("暂时无法开始导入，请稍后重试；原文件和当前模型保持不变。");
        m_status->SetLabel(failure);
        if(open_beauty)m_model_preview_message->SetLabel(failure);
    }
}
}
