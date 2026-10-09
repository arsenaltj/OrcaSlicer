#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
constexpr int beauty_finishing_tools[] = {4, 1, 3, 5, 1};
// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
void wrap_workbench_text(wxStaticText* label, int width, bool preserve_lines = false)
{
    wxString source = label->GetLabel(), line, result;
    source.Replace("\r", "");
    if (!preserve_lines) source.Replace("\n", "");
    for (wxUniChar character : source) {
        if (character == '\n') {
            result += line + "\n"; line.clear();
            continue;
        }
        wxString next = line; next += character;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            result += line + "\n"; line.clear();
        }
        line += character;
    }
    const wxString wrapped = result + line;
    label->SetLabel(wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    label->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + label->FromDIP(2)));
}

void apply_workbench_theme(wxWindow* window, bool enabled,
                           const wxColour& enabled_surface = wxColour(32, 32, 35))
{
    if (!window) return;
    const wxColour surface = enabled ? enabled_surface : *wxWHITE;
    const wxColour text = enabled ? wxColour(238, 240, 244) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    window->SetBackgroundColour(surface);
    window->SetForegroundColour(text);
    for (wxWindow* child : window->GetChildren())
        apply_workbench_theme(child, enabled, enabled_surface);
    if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
}

void apply_workbench_parameter_theme(wxWindow* parameters)
{
    apply_workbench_theme(parameters, true, wxColour(40, 40, 43));
}

void repaint_workbench_surface(wxWindow* window)
{
    if (!window || !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh();
    window->Update();
    for (wxWindow* child : window->GetChildren())
        repaint_workbench_surface(child);
}
}
void ModelGenerationPanel::on_discard(wxCommandEvent&)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty()) return;
    if (m_ready || m_model_preview_ready || m_style_preview_ready) {
        const int answer = show_redesign_confirmation(this,
            _L("要先看看重新设计的建议吗？当前历史模型会保留，图片和描述可继续使用。"),
            _L("重新开始"), {wxYES_NO | wxCANCEL, 105,
                {{wxID_YES, _L("直接重新开始"), true}, {wxID_NO, _L("先看建议")},
                 {wxID_CANCEL, _L("留在当前作品")}}});
        if (answer == wxID_CANCEL) return;
        if (answer == wxID_NO) {
            wxString advice;
            if (m_model_refinement.available && !m_model_refinement.summary.empty())
                advice = _L("已有检查建议：\n") + wxString::FromUTF8(m_model_refinement.summary.c_str());
            else
                advice = _L("尚无这件作品的检查结论。可先按下面的方向对照原图：\n\n"
                    "• 人物不像：使用清晰正面参考，描述中明确五官、发型和姿态。\n"
                    "• 形体不完整：确认图片主体完整、遮挡较少，明确底座和连接部位。\n"
                    "• 颜色杂乱：先在3D美颜里试六色、保留嘴唇和服装等关键色。\n"
                    "• 表面小凹凸：先试局部美颜，通常不需要重新生成。\n\n"
                    "这些是设计建议，未运行新的AI分析。");
            if (show_redesign_confirmation(this, advice, _L("重新设计建议"), {wxOK | wxCANCEL, 105,
                {{wxID_CANCEL, _L("返回调整作品")}, {wxID_OK, _L("继续重新开始"), true}}}) != wxID_OK) return;
        }
    }
    const bool reuse_palette = m_palette_source->GetSelection() == 2 && !current_palette().empty();
    boost::system::error_code reference_error;
    if (!m_reference_image_path.empty() && boost::filesystem::is_regular_file(m_reference_image_path, reference_error)) {
        m_selected_image_path = m_reference_image_path;
        m_selected_image->SetLabel(wxString(m_selected_image_path.filename().wstring()));
    } else if (m_library_model_loaded) {
        m_selected_image_path.clear();
        m_selected_image->SetLabel(_L("未选择图片"));
    }
    if (m_finishing_workbench) set_finishing_workbench(false);
    reset(!m_ready);
    m_palette_recommendation_confirmed = reuse_palette;
    refresh_controls();
    m_prompt->SetFocus();
}

wxWindow* ModelGenerationPanel::build_model_finishing(wxWindow* parent)
{
    auto* scroll = new WorkbenchScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(280), FromDIP(480)), wxVSCROLL | wxBORDER_NONE);
    scroll->SetMinSize(wxSize(FromDIP(280), FromDIP(200)));
    scroll->SetScrollRate(0, FromDIP(12));
    scroll->set_rounded_corners(true);
    m_finishing_panel = scroll;
    scroll->SetBackgroundColour(wxColour(32, 32, 35));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(m_finishing_panel, wxID_ANY, _L("3D 美颜工作台"));
    title->SetFont(wxFontInfo(10).FaceName("HONOR Sans Design").Weight(wxFONTWEIGHT_MEDIUM));
    title_row->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(8));
    auto* close_editor = workbench_button(m_finishing_panel, _L("返回"));
    close_editor->SetFont(wxFontInfo(8).FaceName("HONOR Sans Design").Weight(wxFONTWEIGHT_MEDIUM));
    close_editor->SetPaddingSize(FromDIP(wxSize(12, 4)));
    close_editor->SetMinSize(FromDIP(wxSize(45, 27)));
    close_editor->SetCornerRadius(FromDIP(9));
    close_editor->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(96, 96, 100), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
    close_editor->SetTextColor(StateColor(*wxWHITE));
    close_editor->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        request_return_overview();
    });
    title_row->Add(close_editor, 0, wxTOP | wxBOTTOM, FromDIP(8));
    sizer->AddSpacer(FromDIP(17));
    sizer->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto* hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("Alt＋左键旋转，右键平移，滚轮缩放。原件保留，修改可撤销。"));
    wrap_workbench_text(hint, FromDIP(260));
    hint->Hide();
    auto* base_placeholder = new wxButton(m_finishing_panel, wxID_ANY, _L("增加底座（待实现）"));
    base_placeholder->Disable();
    base_placeholder->SetToolTip(_L("生成后底座几何编辑将在后续版本提供；当前不会改变模型。"));
    base_placeholder->Hide();
    m_finishing_tool = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("整体美颜"), _L("局部修整"), _L("多色试色"), _L("网格修复"), _L("统一这块颜色"), _L("清理小杂点")})
        m_finishing_tool->Append(label);
    m_finishing_tool->SetSelection(0);
    sizer->Add(m_finishing_tool, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_gray = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("灰模观察凹凸（仅显示）"));
    sizer->Add(m_finishing_gray, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_gray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_model_preview->set_gray_view(m_finishing_gray->GetValue()); });
    m_finishing_selection_section = workbench_button(m_finishing_panel, _L("选择区域"));
    m_finishing_selection_section->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_finishing_selection_open = !m_finishing_selection_open;
        refresh_model_finishing();
    });
    sizer->Add(m_finishing_selection_section, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_selection_controls = new wxPanel(m_finishing_panel);
    auto* selection = new wxBoxSizer(wxVERTICAL);
    auto* selection_title = new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("选择区域"));
    selection_title->SetFont(wxGetApp().bold_font());
    selection->Add(selection_title, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* selection_hint = new wxStaticText(m_finishing_selection_controls, wxID_ANY,
        _L("圈选当前可见表面，再涂抹补选或保护细节。橙色参与处理，蓝色受保护；不穿透背面。"));
    wrap_workbench_text(selection_hint, FromDIP(260));
    selection_hint->Hide();
    m_finishing_selection_operation = workbench_choice(m_finishing_selection_controls);
    for (const auto& label : {_L("圈选要修改的范围"), _L("涂抹补选"), _L("涂抹保护"), _L("点选相近颜色"), _L("转动模型")})
        m_finishing_selection_operation->Append(label);
    m_finishing_selection_operation->SetSelection(0);
    selection->Add(m_finishing_selection_operation, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    selection->Add(new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("笔刷大小")), 0);
    m_finishing_radius = new WorkbenchSlider(m_finishing_selection_controls, wxID_ANY, 3, 1, 10,
        wxColour(61, 127, 255), "workbench_parameter_thumb");
    selection->Add(m_finishing_radius, 0, wxEXPAND);
    m_finishing_selection_status = new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("尚未选区 · 点击模型开始"));
    selection->Add(m_finishing_selection_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* selection_actions = new wxBoxSizer(wxHORIZONTAL);
    auto* undo_selection = workbench_button(m_finishing_selection_controls, _L("撤销选区"));
    auto* clear_selection = workbench_button(m_finishing_selection_controls, _L("清空选区"));
    selection_actions->Add(undo_selection, 0, wxRIGHT, FromDIP(6));
    selection_actions->Add(clear_selection);
    selection->Add(selection_actions);
    auto* redo_selection = workbench_button(m_finishing_selection_controls, _L("重做选区"));
    selection->Add(redo_selection, 0, wxTOP, FromDIP(6));
    redo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->redo_selection(); });
    undo_selection->Hide();
    redo_selection->Hide();
    auto* refine_selection = workbench_button(m_finishing_selection_controls, _L("贴合选区边界"));
    refine_selection->SetToolTip(_L("圈选后，在要修改处涂抹补选、在要保留处涂抹保护，再沿颜色和表面边界修正。不会扩大到范围之外。"));
    selection->Add(refine_selection, 0, wxEXPAND | wxTOP, FromDIP(6));
    refine_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->refine_selection_boundary(); });
    auto* focus_selection = workbench_button(m_finishing_selection_controls, _L("放大选区"));
    focus_selection->SetToolTip(_L("将选中的区域放到画面中央；“完整显示模型”可恢复全貌。"));
    selection->Add(focus_selection, 0, wxEXPAND | wxTOP, FromDIP(8));
    auto* overlay_row = m_finishing_overlay_row = new wxPanel(m_finishing_panel);
    auto* overlay_layout = new wxBoxSizer(wxHORIZONTAL);
    overlay_layout->Add(new wxStaticText(overlay_row, wxID_ANY, _L("显示选区高亮")),
        1, wxALIGN_CENTER_VERTICAL);
    auto* show_selection = m_finishing_overlay = new WorkbenchSwitch(overlay_row, _L("显示选区高亮"));
    overlay_layout->Add(show_selection, 0, wxALIGN_CENTER_VERTICAL);
    overlay_row->SetSizer(overlay_layout);
    show_selection->SetValue(true);
    show_selection->SetToolTip(_L("关闭可看清选区内原本的颜色和细节；选区仍然有效。"));
    focus_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_model_preview->focus_selection()) {
            m_finishing_status->SetLabel(_L("请先点选模型上的区域，再放大查看。"));
            refresh_model_finishing();
        }
    });
    show_selection->Bind(wxEVT_TOGGLEBUTTON, [this, show_selection](wxCommandEvent&) {
        show_selection->SetValue(show_selection->GetValue());
        m_model_preview->set_selection_preview_suppressed(false);
        m_model_preview->set_selection_overlay_visible(show_selection->GetValue());
    });
    m_finishing_selection_controls->SetSizer(selection);
    sizer->Add(m_finishing_selection_controls, 0, wxEXPAND | wxALL, FromDIP(10));
    sizer->Add(overlay_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_selection_operation->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { update_finishing_selection(); });
    m_finishing_radius->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_finishing_selection(); });
    undo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->undo_selection(); });
    clear_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->clear_selection(); });
    m_finishing_preset = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("轻柔 · 保留细节"), _L("均衡 · 表面柔化"), _L("加强 · 平滑凹凸"), _L("仅修复 · 保留造型")})
        m_finishing_preset->Append(label);
    m_finishing_preset->SetSelection(0);
    sizer->Add(m_finishing_preset, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_smooth = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("表面美化（保护边界和锐边）"));
    m_finishing_smooth->SetValue(true);
    sizer->Add(m_finishing_smooth, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_strength = new WorkbenchSlider(m_finishing_panel, wxID_ANY, 15, 0, 100,
        wxColour(61, 127, 255), "workbench_parameter_thumb");
    m_finishing_cleanup_hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("先圈住杂点及周围主色，保护眼睛、花纹等细节。只合并孤立小色块；连续条带可用“统一这块颜色”。"));
    wrap_workbench_text(m_finishing_cleanup_hint, FromDIP(260));
    sizer->Add(m_finishing_cleanup_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Native wxSL_LABELS creates sibling labels that overlap after tool panels
    // are shown/hidden. Keep the value in our sizer with the rest of the form.
    m_finishing_strength_value = new wxStaticText(m_finishing_panel, wxID_ANY, _L("处理强度：15%"));
    sizer->Add(m_finishing_strength_value, 0, wxLEFT | wxRIGHT, FromDIP(10));
    m_finishing_strength->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    });
    m_finishing_strength->SetToolTip(_L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    sizer->Add(m_finishing_strength, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_repair = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("网格清理与面朝向修复"));
    m_finishing_repair->SetValue(false);
    sizer->Add(m_finishing_repair, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Keep action targets stable on narrow side rails. Two columns leave
    // enough room for Chinese labels and let the sizer collapse hidden
    // actions without overlapping adjacent controls.
    auto* actions = new wxGridSizer(0, 2, FromDIP(4), FromDIP(4));
    auto button = [&](wxButton*& target, const wxString& label) {
        target = new wxButton(m_finishing_panel, wxID_ANY, label);
        actions->Add(target, 0, wxEXPAND);
    };
    button(m_finishing_preview, _L("预览处理效果"));
    button(m_finishing_compare, _L("查看处理前"));
    button(m_finishing_accept, _L("接受并保存新版本"));
    button(m_finishing_discard, _L("放弃预览"));
    button(m_finishing_undo, _L("返回上个版本"));
    button(m_finishing_redo, _L("重做已保存修整"));
    button(m_finishing_cancel, _L("取消处理"));
    sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_status = new wxStaticText(m_finishing_panel, wxID_ANY, wxEmptyString);
    wrap_workbench_text(m_finishing_status, FromDIP(260));
    wrap_workbench_text(m_finishing_selection_status, FromDIP(240));
    sizer->Add(m_finishing_status, 0, wxEXPAND | wxALL, FromDIP(10));
    m_beauty_controls = new BeautyWorkbenchControls(m_finishing_panel, m_model_preview, m_palette_provider,
        [this] { if (m_finishing_panel) { m_finishing_panel->Layout(); static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside(); } });
    m_beauty_controls->on_original_view_changed = [this](bool enabled) {
        if (m_workbench_original) m_workbench_original->SetValue(enabled);
    };
    m_beauty_controls->on_boundary_adjust = [this] {
        if (m_model_preview) m_model_preview->set_selection_preview_suppressed(false);
        if (m_model_preview) m_model_preview->refine_selection_boundary();
        if (m_beauty_controls) m_beauty_controls->set_dirty(true);
        update_finishing_selection();
    };
    m_beauty_controls->on_operation_changed = [this](int operation) {
        if (!m_finishing_tool) return;
        // Keep the legacy implementation as an internal compatibility facade;
        // Beauty users only see the unified operation selector.
        if (operation < 0 || operation >= int(std::size(beauty_finishing_tools))) return;
        m_finishing_tool->SetSelection(beauty_finishing_tools[operation]);
        m_finishing_smooth->SetValue(beauty_finishing_tools[operation] == 1);
        m_finishing_repair->SetValue(beauty_finishing_tools[operation] == 3);
        refresh_local_recolor_controls();
        update_finishing_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_undo = [this] {
        if (m_beauty_transactions && m_beauty_transactions->undo_count()) {
            if (!m_beauty_transactions->undo())
                m_finishing_status->SetLabel(_L("无法撤销：历史模型文件缺失或已变更，当前预览保持不变。"));
            refresh_model_finishing(); return;
        }
        if (!m_finishing_undo_path.empty()) undo_model_finishing();
        else if (m_model_preview) m_model_preview->undo_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_redo = [this] {
        if (m_beauty_transactions && m_beauty_transactions->redo_count()) {
            if (!m_beauty_transactions->redo())
                m_finishing_status->SetLabel(_L("无法重做：历史候选文件缺失或已变更，当前预览保持不变。"));
            refresh_model_finishing(); return;
        }
        if (!m_finishing_redo_path.empty()) redo_model_finishing();
        else if (m_model_preview) m_model_preview->redo_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_auto_match = [this](const std::string& region) {
        if (!m_model_preview) return size_t(0);
        const auto before = m_model_preview->selection_state();
        // If the immutable editor is still being prepared, let the preview's
        // deferred path retain its native selection history. Once ready, the
        // controller owns the operation-level history entry.
        const bool deferred = !m_model_preview->beauty_editor();
        const size_t count = m_model_preview->select_semantic_region(region, deferred);
        if (count) m_model_preview->set_selection_preview_suppressed(false);
        if (count && m_beauty_transactions) {
            const auto after = m_model_preview->selection_state();
            m_beauty_transactions->record({
                BeautyWorkbenchTransactionController::OperationKind::Selection,
                "semantic region selection",
                [this, before] { if (m_model_preview) m_model_preview->restore_selection_state(before); },
                [this, after] { if (m_model_preview) m_model_preview->restore_selection_state(after); }
            });
        }
        return count;
    };
    m_beauty_controls->on_auto_detail_match = [this](const std::string& detail) {
        if (!m_model_preview) return size_t(0);
        const auto before = m_model_preview->selection_state();
        const bool deferred = !m_model_preview->beauty_editor();
        const size_t count = m_model_preview->select_semantic_detail(
            detail, deferred, m_beauty_controls && m_beauty_controls->preview_protected_details());
        if (count) m_model_preview->set_selection_preview_suppressed(false);
        if (count && m_beauty_transactions) {
            const auto after = m_model_preview->selection_state();
            m_beauty_transactions->record({
                BeautyWorkbenchTransactionController::OperationKind::Selection,
                "semantic detail selection",
                [this, before] { if (m_model_preview) m_model_preview->restore_selection_state(before); },
                [this, after] { if (m_model_preview) m_model_preview->restore_selection_state(after); }
            });
        }
        return count;
    };
    m_beauty_controls->on_import_secondary_evidence = [this] {
        if (!m_model_preview) return;
        wxFileDialog dialog(this, _L("导入只读二级证据"), wxEmptyString, wxEmptyString,
            _L("Evidence JSON (*.json)|*.json|所有文件 (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        dialog.SetMessage(_L("选择当前模型的二级语义证据 JSON；六视角 render-manifest.json 不能直接导入"));
        if (dialog.ShowModal() != wxID_OK) return;
        std::string error;
        if (!m_model_preview->import_secondary_region_evidence(
                boost::filesystem::path(dialog.GetPath().ToStdWstring()), error)) {
            const wxString message = _L("二级证据未导入：") + wxString::FromUTF8(error);
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("二级证据导入失败"), wxOK | wxICON_WARNING);
            result.ShowModal();
            refresh_model_finishing();
            return;
        }
        const wxString message = _L("二级证据已只读导入；未修改模型或材料树。\n现在可以选择二级细节，或勾选“预览全部二级证据（忽略授权门控）”。");
        m_finishing_status->SetLabel(message);
        wxMessageDialog result(this, message, _L("二级证据导入成功"), wxOK | wxICON_INFORMATION);
        result.ShowModal();
        refresh_model_finishing();
    };
    m_beauty_controls->on_regenerate_readonly_evidence = [this] {
        if (!m_model_preview) return;
        m_finishing_status->SetLabel(_L("正在生成只读六视角渲染包；模型和材料树保持不变。"));
        refresh_model_finishing();
        boost::filesystem::path output;
        std::string error;
        if (!m_model_preview->regenerate_readonly_evidence(output, error)) {
            const wxString message = _L("证据生成失败：") + wxString::FromUTF8(error);
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("只读证据生成失败"), wxOK | wxICON_WARNING);
            result.ShowModal();
        } else {
            const wxString message = _L("当前模型的只读六视角渲染包已生成。它还没有二级语义区域，不能直接导入为选区。\n输出目录：") +
                wxString::FromUTF8(output.generic_string());
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("只读证据已生成"), wxOK | wxICON_INFORMATION);
            result.ShowModal();
        }
        refresh_model_finishing();
    };
    m_beauty_controls->on_reoptimize = [this](wxString& reason) {
        if (!m_model_preview || !post_generation_ui_state().can_edit || !m_finishing_candidate.empty()) {
            reason = _L("当前模型不可编辑，请先完成任务或接受、放弃候选版本。");
            return false;
        }
        if (!m_model_preview->semantic_reoptimization_available()) {
            reason = m_model_preview->semantic_reoptimization_reason();
            return false;
        }
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization)) {
            reason = _L("当前仍有 Beauty 处理正在进行，请先完成或取消。");
            m_finishing_status->SetLabel(reason);
            refresh_model_finishing();
            return false;
        }
        m_beauty_reoptimization_before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        m_model_preview->set_semantic_completion_callback([this](bool success) {
            if (!success) {
                const auto error = m_model_preview->semantic_error();
                if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
                if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic optimization failed");
                if (m_finishing_status) m_finishing_status->SetLabel(
                    _L("人像区域优化未完成，当前模型和选区保持不变：") + error);
                refresh_model_finishing();
                return;
            }
            export_semantic_candidate();
        });
        if (!m_model_preview->request_semantic_reoptimization()) {
            if (!m_beauty_reoptimization_before) {
                reason = m_finishing_status->GetLabel();
                return false;
            }
            reason = _L("未重新识别人像区域：") + m_model_preview->semantic_reoptimization_reason();
            m_model_preview->set_semantic_completion_callback({});
            if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic request unavailable");
            m_finishing_status->SetLabel(reason);
            refresh_model_finishing();
            return false;
        }
        refresh_model_finishing();
        return true;
    };
    m_beauty_controls->on_save = [this] {
        request_save_and_return();
    };
    m_beauty_controls->on_preview = [this] { preview_model_finishing(); };
    m_beauty_controls->on_accept = [this] { accept_model_finishing(); };
    m_beauty_controls->on_discard = [this] { discard_model_finishing(); };
    m_beauty_controls->on_cancel = [this] {
        if (m_workbench_check_running && m_workbench_check_cancel) {
            m_workbench_check_cancel->store(true);
            return;
        }
        if (m_beauty_controls && m_beauty_controls->partitioning()) {
            m_beauty_controls->cancel_partition();
            if (m_beauty_transactions) m_beauty_transactions->request_cancel();
            m_finishing_status->SetLabel(_L("正在取消自动划区，原有分区和候选保持不变。"));
            refresh_model_finishing();
            return;
        }
        if (m_model_preview && m_model_preview->cancel_selection_calculation()) {
            m_finishing_status->SetLabel(_L("已取消选区边界计算，原有选区和候选保持不变。"));
            refresh_model_finishing();
            return;
        }
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_beauty_transactions) m_beauty_transactions->request_cancel();
        if (!m_finishing_running && m_model_preview && m_model_preview->semantic_processing()) {
            m_model_preview->cancel_semantic_request();
            refresh_model_finishing();
            return;
        }
        m_finishing_status->SetLabel(_L("正在取消，本次处理不会替换当前模型。"));
    };
    m_beauty_controls->on_partition_started = [this] {
        if (!post_generation_ui_state().can_edit) return false;
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::Selection)) return false;
        m_finishing_status->SetLabel(_L("正在划分模型区域，可旋转、缩放、查看和取消。"));
        refresh_model_finishing();
        return true;
    };
    m_beauty_controls->on_partition_finished = [this](bool success) {
        if (m_beauty_transactions) m_beauty_transactions->finish(success, false,
            success ? std::string {} : "partition canceled or failed");
        m_finishing_status->SetLabel(success ? _L("分区已就绪；点击模型上的区域，再补选、保护、改色或拉伸。")
            : _L("分区未完成，原有分区保持不变。"));
        refresh_model_finishing();
    };
    m_beauty_controls->on_pick_mode = [this] {
        if (!post_generation_ui_state().can_edit) return;
        m_model_preview->set_selection_preview_suppressed(false);
        m_finishing_selection_operation->SetSelection(3);
        update_region_mode();
        m_model_preview->set_selection_enabled(true);
    };
    m_beauty_controls->on_record = [this](const std::string& label,
        std::function<void()> undo, std::function<void()> redo) {
        if (m_beauty_transactions) m_beauty_transactions->record({
            BeautyWorkbenchTransactionController::OperationKind::Selection,
            label, std::move(undo), std::move(redo)
        });
        if (m_beauty_controls) {
            const auto state = post_generation_ui_state();
            m_beauty_controls->set_history_permissions(state.can_undo, state.can_redo);
        }
    };
    m_beauty_controls->on_available_colors = [this] { return local_recolor_palette(); };
    m_beauty_controls->on_color_slot_changed = [this](size_t slot) {
        m_region_color_index = int(slot);
        refresh_local_recolor_controls();
    };
    sizer->Insert(1, m_beauty_controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_panel->SetSizer(sizer);
    m_finishing_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { preview_model_finishing(); });
    m_finishing_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_model_finishing(); });
    m_finishing_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { discard_model_finishing(); });
    m_finishing_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { undo_model_finishing(); });
    m_finishing_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { redo_model_finishing(); });
    m_finishing_strength->Bind(wxEVT_SCROLL_THUMBRELEASE, [this](wxScrollEvent&) {
        if (!m_busy && m_finishing_workbench && (m_finishing_tool->GetSelection() < 2 || m_finishing_tool->GetSelection() == 5)) preview_model_finishing();
    });
    // These controls belong to the separate post-generation workbench and
    // are created after this legacy finishing panel. Bindings are installed
    // when the controls are created; keep this panel safe during construction.
    m_finishing_tool->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int tool = m_finishing_tool->GetSelection();
        m_finishing_smooth->SetValue(tool != 3 && tool != 5);
        m_finishing_repair->SetValue(tool == 3);
        if (tool == 5) {
            m_finishing_strength->SetValue(35);
        }
        // Color tools must show the surface colors, even when the previous
        // geometry tool was inspected as a gray model.
        if (tool == 2 || tool == 4 || tool == 5) {
            m_finishing_gray->SetValue(false);
            m_model_preview->set_gray_view(false);
        }
        m_finishing_status->SetLabel(tool == 2 ? _L("在模型下方试色，可切回原色。选定方案会带入导入配色，最终按实际耗材确认。")
            : tool == 5 ? _L("适用于衣物、头发、皮肤、底座等区域。先保护需要保留的细节，再预览清理结果。")
            : tool == 4 ? _L("点选模型，再用右侧工具换色。改色保存为新版本，原件保留。")
            : tool == 1 ? _L("先点选局部区域，再调整强度。边缘渐变柔化，未选区域保持原样。")
            : tool == 3 ? _L("清理重复／退化面并校正面朝向；不自动补洞或删除部件。")
            : _L("保守柔化整体小凹凸；按当前强度预览后可对比。"));
        refresh_local_recolor_controls(); update_finishing_selection(); refresh_model_finishing();
    });
    m_finishing_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_beauty_transactions && m_beauty_transactions->processing() && !m_finishing_running && m_model_preview)
            m_model_preview->cancel_semantic_request();
        if (m_beauty_transactions) m_beauty_transactions->request_cancel();
        m_finishing_status->SetLabel(_L("正在取消，本次处理不会替换当前模型。"));
        m_finishing_cancel->Disable();
    });
    auto compare = [this](wxCommandEvent&) {
        if (m_finishing_candidate.empty() || m_busy) return;
        if (show_finishing_version(m_finishing_before ? m_finishing_candidate : m_finishing_source)) {
            m_finishing_before = !m_finishing_before;
            m_finishing_compare->SetLabel(m_finishing_before ? _L("查看处理后") : _L("查看处理前"));
            m_finishing_compare_model->SetLabel(m_finishing_compare->GetLabel());
            m_model_preview_message->SetLabel(m_finishing_before
                ? _L("处理前 · 当前已保存版本") : _L("处理后 · 尚未接受"));
            refresh_model_finishing();
        }
    };
    m_finishing_compare->Bind(wxEVT_BUTTON, compare);
    m_finishing_compare_model->Bind(wxEVT_BUTTON, [this, compare](wxCommandEvent& event) {
        if (m_finishing_workbench) compare(event);
    });
    m_finishing_compare_model->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_finishing_workbench) { event.Skip(); return; }
        if (m_busy || m_finishing_candidate.empty() || m_finishing_before) return;
        if (show_finishing_version(m_finishing_source)) {
            m_finishing_compare_held = true;
            m_finishing_compare_model->CaptureMouse();
            m_model_preview_message->SetLabel(_L("处理前 · 松开恢复处理后"));
            refresh_model_finishing();
        }
    });
    auto release_compare = [this] {
        if (!m_finishing_compare_held) return;
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        if (!m_finishing_candidate.empty()) show_finishing_version(m_finishing_candidate);
        m_model_preview_message->SetLabel(_L("处理后 · 尚未保存"));
        refresh_model_finishing();
    };
    m_finishing_compare_model->Bind(wxEVT_LEFT_UP, [this, release_compare](wxMouseEvent& event) {
        if (m_finishing_workbench) event.Skip();
        else release_compare();
    });
    m_finishing_compare_model->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release_compare](wxMouseCaptureLostEvent&) { release_compare(); });
    m_finishing_smooth->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_model_finishing(); });
    m_finishing_preset->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int preset = m_finishing_preset->GetSelection();
        m_finishing_smooth->SetValue(preset != 3);
        m_finishing_repair->SetValue(preset == 3 && m_finishing_tool->GetSelection() != 1);
        m_finishing_strength->SetValue(preset == 0 ? 15 : preset == 2 ? 65 : 35);
        refresh_model_finishing();
    });
    m_finishing_panel->Bind(wxEVT_SIZE, [this, hint](wxSizeEvent& event) {
        const int width = std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34));
        wrap_workbench_text(hint, width); wrap_workbench_text(m_finishing_status, width); event.Skip();
    });
    for (wxWindow* child : m_finishing_panel->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(280), -1));
    for (wxWindow* child : m_finishing_selection_controls->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(260), -1));
    close_editor->SetMaxSize(FromDIP(wxSize(45, 27)));
    m_finishing_panel->Hide();
    // The old selector remains as an internal compatibility facade. Beauty
    // users choose operations from BeautyWorkbenchControls below.
    m_finishing_tool->Hide();
    return m_finishing_panel;
}
} // namespace Slic3r::GUI
