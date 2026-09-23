#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
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
    const boost::filesystem::path source(picker.GetPath().ToStdWstring());
    const std::string id="finish-import-"+new_request_id();
    const auto destination=temp_path(id,"glb"),metadata_path=library_metadata_path(id),root=generated_models_root();
    if(m_library_import_worker.joinable())m_library_import_worker.join();
    m_busy=true;m_status->SetLabel(_L("正在导入模型和贴图到历史资产，原文件保持不变……"));refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {m_library_import_worker=std::thread([weak,source,destination,metadata_path,root,id] {
        std::string error;
        const bool archived=AI::archive_local_model(source,destination,error);
        bool success=archived;
        try {if(success) {
            const nlohmann::json metadata={{"schema_version",4},{"job_id",id},{"source","local_import"},
                {"prompt",source.filename().string()},{"model_path",destination.lexically_relative(root).generic_string()},
                {"model_sha256",AI::model_artifact_sha256(destination)},{"generated_at",std::time(nullptr)},
                {"palette",nlohmann::json::array()},{"palette_roles",nlohmann::json::object()},{"use_printable_colors",false}};
            success=write_json(metadata_path,metadata);if(!success)error="模型记录保存失败，请检查磁盘空间。";
        }}catch(const std::exception& e){success=false;error=e.what();}
        if(!success && archived){boost::system::error_code ignored;boost::filesystem::remove(destination,ignored);}
        wxGetApp().CallAfter([weak,destination,id,success,error] {
            if(!weak || weak->m_shutdown)return;
            if(weak->m_library_import_worker.joinable())weak->m_library_import_worker.join();
            weak->m_busy=false;weak->refresh_controls();
            if(!success){weak->m_status->SetLabel(_L("导入未完成：")+wxString::FromUTF8(error));return;}
            weak->load_library_entries();
            weak->load_library_entry(destination,{},{},{},{},false,{},{},{},id,_L("本地导入模型"));
        });
    });}catch(const std::exception& e){m_busy=false;refresh_controls();m_status->SetLabel(wxString::FromUTF8(e.what()));}
}
}
