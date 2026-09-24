#include "BeautyWorkbenchControls.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <exception>

namespace Slic3r::GUI {

BeautyWorkbenchControls::BeautyWorkbenchControls(wxWindow* parent, ModelPreview3D* preview,
                                                 AI::IPrintablePaletteProvider& palette,
                                                 std::function<void()> layout_changed)
    : wxPanel(parent), m_preview(preview), m_palette(palette), m_layout_changed(std::move(layout_changed))
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    m_status = new wxStaticText(this, wxID_ANY, _L("Beauty 工作台适配已连接；使用上方选区工具编辑。"));
    m_status->Wrap(FromDIP(250));
    root->Add(m_status, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* semantic_row = new wxBoxSizer(wxHORIZONTAL);
    m_auto_region = new wxChoice(this, wxID_ANY);
    m_auto_region->Append(_L("头发"));
    m_auto_region->Append(_L("皮肤"));
    m_auto_region->Append(_L("眼睛"));
    m_auto_region->Append(_L("嘴唇"));
    m_auto_region->Append(_L("衣服"));
    m_auto_region->SetSelection(0);
    m_auto_match = new wxButton(this, wxID_ANY, _L("自动匹配区域"));
    m_reoptimize = new wxButton(this, wxID_ANY, _L("重新优化人像区域"));
    semantic_row->Add(m_auto_region, 1, wxRIGHT, FromDIP(4));
    semantic_row->Add(m_auto_match, 0, wxRIGHT, FromDIP(4));
    semantic_row->Add(m_reoptimize, 0);
    root->Add(semantic_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_boundary = new wxButton(this, wxID_ANY, _L("贴合边界"));
    m_undo = new wxButton(this, wxID_ANY, _L("撤销"));
    m_redo = new wxButton(this, wxID_ANY, _L("重做"));
    m_save = new wxButton(this, wxID_ANY, _L("保存新 GLB"));
    for (wxButton* button : {m_boundary, m_undo, m_redo, m_save})
        actions->Add(button, 1, wxRIGHT, FromDIP(4));
    root->Add(actions, 0, wxEXPAND);
    SetSizer(root);
    m_boundary->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_boundary_adjust) on_boundary_adjust(); });
    m_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_undo) on_undo(); });
    m_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_redo) on_redo(); });
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_save) on_save(); });
    m_auto_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!on_auto_match || !m_auto_region || m_auto_region->GetSelection() < 0) return;
        static constexpr const char* keys[] = {"hair", "skin", "eyes", "lips", "clothes"};
        const int index = m_auto_region->GetSelection();
        const std::string region = index >= 0 && index < 5 ? keys[index] : std::string {};
        const size_t count = on_auto_match(region);
        if (count == 0) m_status->SetLabel(_L("当前识别结果没有足够可靠的该区域；可继续手动补选。"));
        else m_status->SetLabel(wxString::Format(_L("已自动匹配 %llu 个面，可继续补选或涂抹保护。"),
            static_cast<unsigned long long>(count)));
        m_dirty = count > 0;
        update_text();
    });
    m_reoptimize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_reoptimize) on_reoptimize();
        m_status->SetLabel(_L("已请求重新优化；Beauty 选区和手动保护保持不变。"));
        update_text();
    });
    Hide();
}

void BeautyWorkbenchControls::synchronize(const boost::filesystem::path& source, bool editable, bool visible)
{
    const auto previous_source = m_source;
    const std::string previous_geometry = m_geometry_id;
    m_source = source;
    m_editable = editable;
    m_visible = visible;
    const std::string geometry = m_preview ? m_preview->geometry_id() : std::string {};
    if (geometry != previous_geometry || source != previous_source) {
        m_geometry_id = geometry;
        m_surface.reset();
        m_document = AI::BeautyDocument {};
        m_dirty = false;
    }
    m_ready = m_preview && !m_geometry_id.empty() && m_preview->region_editing_ready();
    if (m_ready && !m_surface) {
        const auto editor = m_preview->beauty_editor();
        if (editor) {
            try {
                m_surface = AI::BeautySurface::build(editor->mesh(), editor->vertex_colors());
                m_document.geometry_id = m_geometry_id;
                m_document.face_count = editor->mesh().indices.size();
                m_document.face_patch = m_surface->face_patch;
            } catch (const std::exception&) {
                m_surface.reset();
                m_ready = false;
            }
        }
    }
    Show(visible && m_ready);
    update_text();
}

void BeautyWorkbenchControls::update_text()
{
    if (!m_status) return;
    if (!m_visible) m_status->SetLabel(_L("Beauty 工作台已关闭。"));
    else if (!m_ready) m_status->SetLabel(_L("正在准备 Beauty 编辑数据；当前选择和语义缓存保持不变。"));
    else if (m_dirty) m_status->SetLabel(_L("Beauty 有未保存修改；保存后生成新的 GLB 版本。"));
    else m_status->SetLabel(_L("Beauty 工作台已连接；局部改色会继续经过语义色槽和手动覆盖。"));
    m_boundary->Enable(m_editable && m_ready);
    m_undo->Enable(m_editable && m_ready);
    m_redo->Enable(m_editable && m_ready);
    m_save->Enable(m_editable && m_ready && m_dirty);
    const bool semantic = m_preview && m_preview->semantic_regions_ready();
    m_auto_region->Enable(m_editable && m_ready && semantic);
    m_auto_match->Enable(m_editable && m_ready && semantic);
    m_reoptimize->Enable(m_editable && m_ready && m_preview && m_preview->semantic_optimization_enabled());
    Layout();
    if (m_layout_changed) m_layout_changed();
}

void BeautyWorkbenchControls::mark_saved()
{
    m_dirty = false;
    update_text();
}

void BeautyWorkbenchControls::prepare_options(
    AI::ModelFinishingOptions& options,
    const AI::SurfaceSelectionPersistence::SelectionState& selection) const
{
    if (!m_preview || !m_surface || m_source.empty()) return;
    const auto editor = m_preview->beauty_editor();
    if (!editor || selection.selected.empty()) return;
    options.beauty_appearance = true;
    options.smooth_surface = false;
    options.repair_mesh = false;
    options.recolor_selected = false;
    options.beauty_surface = m_surface;
    options.selected_faces.clear();
    for (size_t face = 0; face < selection.selected.size(); ++face)
        if (selection.selected[face]) options.selected_faces.push_back(face);
    options.beauty_protected_faces = selection.protected_faces;
    options.appearance.face_weights.assign(editor->mesh().indices.size(), 0.f);
    options.appearance.face_target_colors.resize(editor->mesh().indices.size());
    options.beauty_document = m_document.encode();
}

nlohmann::json BeautyWorkbenchControls::accepted_document(const AI::ModelFinishingOptions& options,
                                                          const std::string& output_geometry)
{
    if (!options.beauty_document.is_object()) return nlohmann::json::object();
    auto result = options.beauty_document;
    result["geometry_id"] = output_geometry;
    result["schema"] = "orca.beauty-workbench/v1";
    return result;
}

} // namespace Slic3r::GUI
