#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationInputStyle.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelLibraryExport.hpp"
#include "slic3r/GUI/AIModelOutputDirectory.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/utils.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::export_library_entry(const GeneratedModelEntry& entry)
{
    auto* config = wxGetApp().app_config;
    const boost::filesystem::path model_root = ai_model_output_directory().root();
    const boost::filesystem::path exports = model_root / "exports";
    const auto remembered = config->get("model_library_export_directory");
    boost::filesystem::path folder = remembered.empty() ? exports : boost::filesystem::path(remembered);
    boost::system::error_code ec;
    const auto relative = folder.lexically_relative(model_root);
    if (relative.empty() || *relative.begin() == ".." || !boost::filesystem::is_directory(folder, ec)) folder = exports;
    boost::filesystem::create_directories(folder, ec);
    if (ec) {
        wxMessageBox(_L("无法创建安装目录下的 models/exports 文件夹，请检查写入权限。"), _L("导出未完成"), wxOK | wxICON_ERROR, this);
        return;
    }
    wxDirDialog dialog(this, _L("选择模型副本的保存文件夹"), from_path(folder), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    folder = into_path(dialog.GetPath());
    ModelLibraryExportRequest request;
    request.model = entry.model_path;
    request.design = entry.ai_image_path;
    request.reference = entry.reference_image_path;
    request.id = entry.job_id;
    request.title = into_u8(entry.title);
    request.task_id = entry.provider_task_id;
    request.conversion_task_id = entry.provider_conversion_task_id;
    boost::filesystem::path destination;
    std::string error;
    bool exported;
    {
        wxBusyCursor busy;
        exported = export_model_library_copy(request, folder, destination, error);
    }
    if (!exported) {
        wxMessageBox(wxString::FromUTF8(error), _L("导出未完成"), wxOK | wxICON_ERROR, this);
        return;
    }
    config->set("model_library_export_directory", folder.string());
    MessageDialog completed(this, _L("已保存独立副本：\n") + from_path(destination) + "\n\n" +
        (entry.design_only ? _L("设计图已导出。") : _L("model.glb 保留模型材质和贴图。打印工程请另存 3MF。")),
        _L("导出完成"), wxYES_NO | wxNO_DEFAULT | wxICON_INFORMATION);
    completed.SetButtonLabel(wxID_YES, _L("打开文件夹"));
    completed.SetButtonLabel(wxID_NO, _L("完成"));
    if (completed.ShowModal() == wxID_YES) {
        wxString path = from_path(destination) + wxFileName::GetPathSeparator();
        if (!wxLaunchDefaultApplication(path))
            wxMessageBox(_L("副本已保存，但无法打开文件夹：") + from_path(destination), _L("导出完成"), wxOK, this);
    }
}

void ModelGenerationPanel::mount_assets(wxWindow* parent)
{
    initialize_for_shell_host();
    if (!parent || !m_library_scroller) return;
    auto* surface = m_library_scroller->GetParent();
    if (surface->GetParent() == parent) return;
    m_assets_home_parent = surface->GetParent();
    if (auto* book = dynamic_cast<wxNotebook*>(m_assets_home_parent)) {
        const int index = book->FindPage(surface);
        if (index != wxNOT_FOUND) book->RemovePage(index);
    }
    if (auto* sizer = surface->GetContainingSizer()) sizer->Detach(surface);
    surface->Reparent(parent);
    parent->GetSizer()->Add(surface, 1, wxEXPAND);
    surface->Show();
    m_library_appearance_handler = [](wxWindow* card) {
        ModelGenerationInputStyle::apply(card, true);
    };
    ModelGenerationInputStyle::apply(surface, true);
    parent->Layout();
}

void ModelGenerationPanel::unmount_assets()
{
    if (!m_assets_home_parent || !m_library_scroller) return;
    auto* surface = m_library_scroller->GetParent();
    if (auto* sizer = surface->GetContainingSizer()) sizer->Detach(surface);
    surface->Reparent(m_assets_home_parent);
    if (auto* book = dynamic_cast<wxNotebook*>(m_assets_home_parent))
        book->AddPage(surface, _L("历史资产"), false);
    surface->Hide();
    m_assets_home_parent = nullptr;
    m_library_appearance_handler = {};
}

} // namespace Slic3r::GUI
