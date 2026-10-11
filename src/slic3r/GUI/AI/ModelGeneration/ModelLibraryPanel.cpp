#include "ModelLibraryPanel.hpp"
#include "ModelLibraryLoaderFactory.hpp"
#include "ModelLibraryExport.hpp"
#include "ModelGenerationInputStyle.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/listbox.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/dirdlg.h>
#include <wx/msgdlg.h>

namespace Slic3r::GUI {
ModelLibraryPanel::ModelLibraryPanel(wxWindow* parent, boost::filesystem::path root, ModelLibraryApplication::Ports ports)
    : ModelLibraryPanel(parent, std::make_shared<ModelLibraryApplication>(std::move(root), std::move(ports))) {}
ModelLibraryPanel::ModelLibraryPanel(wxWindow* parent, std::shared_ptr<ModelLibraryApplication> application)
    : wxPanel(parent), m_application_owner(std::move(application)), m_application(*m_application_owner), m_loader(make_model_library_loader()), m_timer(this)
{
    SetName("model_library_capability");
    auto* column = new wxBoxSizer(wxVERTICAL); SetSizer(column);
    auto* filter = new wxBoxSizer(wxHORIZONTAL);
    m_search = new wxTextCtrl(this, wxID_ANY); m_search->SetHint(_L("搜索名称或任务 ID"));
    m_category = new wxChoice(this, wxID_ANY);
    for (const auto& label : {_L("全部资产"), _L("设计图"), _L("模型"), _L("美颜版本")}) m_category->Append(label);
    m_category->SetSelection(0);
    auto* refresh = workbench_button(this, _L("刷新资产"));
    filter->Add(m_search, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    filter->Add(m_category, 0, wxRIGHT, FromDIP(8)); filter->Add(refresh, 0);
    column->Add(filter, 0, wxEXPAND | wxALL, FromDIP(12));
    m_status = new wxStaticText(this, wxID_ANY, _L("正在读取历史资产…"));
    column->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_list = new wxListBox(this, wxID_ANY);
    column->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(12));
    m_details = new wxStaticText(this, wxID_ANY, wxEmptyString);
    column->Add(m_details, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* actions = new wxWrapSizer(wxHORIZONTAL);
    for (const auto& action : std::vector<std::pair<wxString, ModelLibraryCommand>> {
            {_L("打开模型"), ModelLibraryCommand::OpenModel}, {_L("打开设计图"), ModelLibraryCommand::OpenImage},
            {_L("配色匹配"), ModelLibraryCommand::MatchColors}, {_L("导入准备页"), ModelLibraryCommand::Import},
            {_L("更换贴图"), ModelLibraryCommand::Retexture}, {_L("删除资产"), ModelLibraryCommand::Remove}}) {
        auto* button = workbench_button(this, action.first);
        button->Bind(wxEVT_BUTTON, [this, kind = action.second](wxCommandEvent&) { command(kind); });
        actions->Add(button, 0, wxRIGHT, FromDIP(8));
    }
    auto* export_button = workbench_button(this, _L("导出副本"));
    actions->Add(export_button, 0);
    auto* local = workbench_button(this, _L("打开本地模型"));
    actions->Add(local, 0, wxLEFT, FromDIP(8));
    local->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (!m_stopped) m_application.choose_local(); });
    column->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_assets(); });
    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { rebuild(); });
    m_category->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { rebuild(); });
    m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { select(); });
    m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
        const int selected = m_list->GetSelection();
        if (!m_stopped && selected != wxNOT_FOUND) command(m_application.entries()[m_visible[size_t(selected)]].design_only
            ? ModelLibraryCommand::OpenImage : ModelLibraryCommand::OpenModel);
    });
    export_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { export_selected(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        std::optional<ModelLibraryLoader::Snapshot> snapshot;
        std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
        m_loader->take(snapshot, thumbnails);
        if (snapshot && snapshot->revision == m_request_revision) {
            if (snapshot->failed) m_status->SetLabel(_L("读取失败，当前资产仍保留；可刷新重试。"));
            else { m_application.replace(std::move(snapshot->entries)); rebuild(); }
        }
        if (!m_loader->loading()) m_timer.Stop();
    });
    ModelGenerationInputStyle::apply(this, true);
    refresh_assets();
}
ModelLibraryPanel::~ModelLibraryPanel() { shutdown(); }
void ModelLibraryPanel::shutdown() { if (m_stopped) return; m_stopped = true; Disable(); m_timer.Stop(); m_loader->stop(); }
void ModelLibraryPanel::refresh_assets()
{
    if (m_stopped) return;
    auto request = std::make_shared<ModelLibraryLoader::Request>();
    request->revision = ++m_request_revision; request->scan = true; request->root = m_application.root();
    m_loader->submit(std::move(request)); m_status->SetLabel(_L("正在读取历史资产…")); m_timer.Start(50);
}
void ModelLibraryPanel::rebuild()
{
    if (m_stopped) return;
    m_visible = m_application.filter(static_cast<ModelLibraryCategory>(m_category->GetSelection()), m_search->GetValue());
    m_list->Freeze(); m_list->Clear();
    for (size_t index : m_visible) m_list->Append(m_application.entries()[index].title);
    for (size_t i = 0; i < m_visible.size(); ++i)
        if (m_application.entries()[m_visible[i]].job_id == m_selected_job_id) m_list->SetSelection(int(i));
    m_list->Thaw();
    m_status->SetLabel(wxString::Format(_L("%llu 件资产"), static_cast<unsigned long long>(m_visible.size())));
    select();
}
void ModelLibraryPanel::select()
{
    if (m_stopped) return;
    const int selected = m_list->GetSelection();
    m_selected_job_id = selected == wxNOT_FOUND ? std::string() : m_application.entries()[m_visible[size_t(selected)]].job_id;
    m_details->SetLabel(selected == wxNOT_FOUND ? _L("选择资产查看详情。") : m_application.entries()[m_visible[size_t(selected)]].details);
    m_details->Wrap(std::max(FromDIP(200), GetClientSize().x - FromDIP(24))); Layout();
}
void ModelLibraryPanel::command(ModelLibraryCommand kind)
{
    if (m_stopped) return;
    const int selected = m_list->GetSelection();
    if (selected == wxNOT_FOUND) return;
    if (!m_application.execute(kind, m_application.token(m_visible[size_t(selected)])))
        m_status->SetLabel(_L("此操作当前不可用，请先完成当前编辑或选择对应资产。"));
}
void ModelLibraryPanel::export_selected()
{
    if (m_stopped) return;
    const int selected = m_list->GetSelection(); if (selected == wxNOT_FOUND) return;
    const auto token = m_application.token(m_visible[size_t(selected)]);
    wxDirDialog dialog(this, _L("选择模型副本文件夹"), {}, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    const auto* entry = m_application.find(token); if (!entry) return;
    ModelLibraryExportRequest request {entry->model_path, entry->ai_image_path, entry->reference_image_path,
        entry->job_id, entry->title.ToUTF8().data(), entry->provider_task_id, entry->provider_conversion_task_id};
    boost::filesystem::path destination; std::string error;
    if (!export_model_library_copy(request, boost::filesystem::path(dialog.GetPath().ToStdWstring()), destination, error))
        wxMessageBox(wxString::FromUTF8(error), _L("导出未完成"), wxOK | wxICON_ERROR, this);
    else m_status->SetLabel(_L("已保存副本：") + wxString(destination.wstring()));
}
}
