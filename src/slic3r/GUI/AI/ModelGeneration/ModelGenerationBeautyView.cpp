#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelGenerationInputStyle.hpp"
#include "ModelPreview3D.hpp"
#include "ModelViewportFacts.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/control.h>
#include <wx/msgdlg.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/tglbtn.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

namespace {
// This confirmation owns its popup theme; the existing OBJ import owns the copy.
class BeautyCopyConfirmation final : public MessageDialog, public AIThemeOwner {
public:
    explicit BeautyCopyConfirmation(wxWindow* parent) : MessageDialog(parent,
        _L("美颜需要创建一个本地可编辑的 GLB 副本。\n原 OBJ 模型和 Task ID 保留；取消不会创建文件。"),
        _L("创建美颜副本"), wxOK | wxCANCEL | wxCANCEL_DEFAULT) {
        SetButtonLabel(wxID_OK, _L("创建副本并打开"));
        SetButtonLabel(wxID_CANCEL, _L("取消"), true);
        get_button(wxID_OK)->SetName("input_primary");
        get_button(wxID_CANCEL)->SetName("input_quiet");
        logo->Hide();
        apply_ai_theme(true);
        Fit();
    }
    void apply_ai_theme(bool fonts) override {
        ModelGenerationInputStyle::apply(this, fonts);
        Refresh(false);
    }
};
// These two controls belong only to the beauty editor. Reuse the native
// dropdown and render the original Figma assets at their design dimensions.
class BeautyChoice final : public ComboBox {
public:
    explicit BeautyChoice(wxWindow* parent) : ComboBox(parent, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(-1, parent->FromDIP(56)), 0, nullptr, wxCB_READONLY),
        m_arrow(this, "figma-ux/beauty-mode-arrow", 13) {
        SetMinSize(FromDIP(wxSize(1, 56)));
        SetName("input_field");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::panel)); dc.Clear();
            dc.SetPen(wxPen(HasFocus() ? ModelGenerationInputStyle::yellow : ModelGenerationInputStyle::field));
            dc.SetBrush(wxBrush(ModelGenerationInputStyle::field));
            dc.DrawRoundedRectangle(GetClientRect(), FromDIP(12));
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled() ? ModelGenerationInputStyle::text : ModelGenerationInputStyle::secondary);
            const auto size = GetClientSize();
            const auto text = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END, std::max(1, size.x - FromDIP(48)));
            dc.DrawText(text, FromDIP(12), (size.y - dc.GetTextExtent(text).y) / 2);
            const auto asset = m_arrow.bmp().ConvertToImage();
            const int x = size.x - FromDIP(12) - m_arrow.GetBmpWidth(), y = size.y / 2 - FromDIP(14);
            dc.DrawBitmap(wxBitmap(asset.Rotate90(false)), x, y, true);
            dc.DrawBitmap(wxBitmap(asset.Rotate90(true)), x, y + FromDIP(11), true);
        });
    }
    void SetSelection(int value) override { ComboBox::SetSelection(value); SetMinSize(FromDIP(wxSize(1, 56))); }
    void Rescale() override { ComboBox::Rescale(); SetMinSize(FromDIP(wxSize(1, 56))); }
private:
    ScalableBitmap m_arrow;
};

class BeautyStrength final : public wxControl {
public:
    BeautyStrength(wxWindow* parent, std::function<void(int)> changed) :
        wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_NONE),
        m_changed(std::move(changed)), m_handle(this, "figma-ux/beauty-strength-handle", 26) {
        SetMinSize(FromDIP(wxSize(1, 36)));
        SetName("input_field");
        SetLabel(_L("处理强度：15%"));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::panel)); dc.Clear();
            const int inset = FromDIP(13), width = std::max(1, GetClientSize().x - 2 * inset);
            const int center = inset + width * m_value / 100, y = GetClientSize().y / 2;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(78, 78, 81)));
            dc.DrawRoundedRectangle(inset, y - FromDIP(3), width, FromDIP(6), FromDIP(3));
            dc.SetBrush(wxBrush(IsEnabled() ? wxColour(255, 194, 39) : ModelGenerationInputStyle::secondary));
            if(center > inset) dc.DrawRoundedRectangle(inset, y - FromDIP(3), center - inset, FromDIP(6), FromDIP(3));
            dc.DrawBitmap(m_handle.bmp(), center - m_handle.GetBmpWidth() / 2,
                y - m_handle.GetBmpHeight() / 2, true);
            if(HasFocus()) { dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.SetPen(wxPen(ModelGenerationInputStyle::yellow)); dc.DrawRectangle(GetClientRect()); }
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { if(!IsEnabled())return; SetFocus(); CaptureMouse(); from_x(event.GetX()); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { if(HasCapture() && event.LeftIsDown())from_x(event.GetX()); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) { if(HasCapture()) { from_x(event.GetX()); ReleaseMouse(); } });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            switch(event.GetKeyCode()) {
            case WXK_LEFT: case WXK_DOWN: SetValue(m_value - 1); break;
            case WXK_RIGHT: case WXK_UP: SetValue(m_value + 1); break;
            case WXK_PAGEUP: SetValue(m_value + 5); break;
            case WXK_PAGEDOWN: SetValue(m_value - 5); break;
            case WXK_HOME: SetValue(0); break;
            case WXK_END: SetValue(100); break;
            default: event.Skip(); break;
            }
        });
    }
    void SetValue(int value) {
        m_value = std::clamp(value, 0, 100);
        SetLabel(wxString::Format(_L("处理强度：%d%%"), m_value));
        Refresh(false); m_changed(m_value);
    }
private:
    int m_value {15};
    std::function<void(int)> m_changed;
    ScalableBitmap m_handle;
    void from_x(int x) { SetValue((x - FromDIP(13)) * 100 / std::max(1, GetClientSize().x - FromDIP(26))); }
};
}

void ModelGenerationPanel::refresh_model_overview_risks()
{
    if (!m_model_overview_risks) return;
    const auto stage = model_check_stage(m_model_preview_ready, m_quality_check_busy, m_quality_check_failed, m_model_quality);
    const bool valid = stage == ModelCheckStage::Passed || stage == ModelCheckStage::Review || stage == ModelCheckStage::Rejected;
    auto* sizer = m_model_overview_risks->GetSizer();
    sizer->Clear(true);
    const auto risks = valid ? model_check_risks(m_model_quality) : std::vector<ModelCheckRisk>{};
    for (const auto& risk : risks) {
        auto* row = new wxPanel(m_model_overview_risks);
        row->SetName("input_panel");
        auto* line = new wxBoxSizer(wxHORIZONTAL);
        const bool topology = risk.code.find("boundary_edges") != std::string::npos ||
            risk.code.find("non_manifold_edges") != std::string::npos;
        auto* icon = new wxStaticBitmap(row, wxID_ANY, create_scaled_bitmap(
            risk.blocking || topology ? "figma-ux/model-check-risk-error" : "figma-ux/model-check-risk-warning", row, 24));
        line->Add(icon, 0, wxTOP | wxRIGHT, FromDIP(12));
        auto* text = new wxBoxSizer(wxVERTICAL);
        auto* title = new Label(row, risk.title, LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(220, -1)));
        title->SetMinSize(wxSize(1, -1)); title->SetName("input_panel");
        title->SetFont(wxGetApp().bold_font());
        text->Add(title, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
        auto* detail = new Label(row, risk.detail, LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(220, -1)));
        detail->SetMinSize(wxSize(1, -1)); detail->SetName("input_secondary");
        text->Add(detail, 0, wxEXPAND);
        line->Add(text, 1, wxEXPAND);
        row->SetSizer(line);
        sizer->Add(row, 0, wxEXPAND | wxALL, FromDIP(12));
        ModelGenerationInputStyle::apply(row, false);
    }
    m_model_overview_risks->Show(!risks.empty());
    m_model_overview_check_details->Show(valid);
    if (!valid) m_model_overview_check_expanded = false;
    m_model_overview_check_details->SetLabel(m_model_overview_check_expanded
        ? _L("收起检查范围与指标") : _L("查看检查范围与指标"));
    m_model_overview_check_metrics->Show(valid && m_model_overview_check_expanded);
    m_model_overview->Layout();
    static_cast<wxScrolledWindow*>(m_model_overview)->FitInside();
    sync_model_overview_scroll_track();
}

bool ModelGenerationPanel::preserve_unsaved_finishing()
{
    bool has_changes = (m_beauty_controls && m_beauty_controls->has_changes()) || !m_finishing_candidate.empty();
    if (!has_changes && !m_displayed_model_path.empty()) {
        try { has_changes = BeautyWorkbenchControls::has_saved_draft(m_displayed_model_path); }
        catch (const std::exception& error) {
            wxMessageDialog(this, _L("暂时无法核对美颜草稿，当前模型已保留：") + from_u8(error.what()),
                _L("草稿读取失败"), wxOK | wxICON_ERROR).ShowModal();
            return true;
        }
    }
    if (!has_changes)
        return false;
    wxMessageDialog choice(this,
        _L("当前模型还有未保存的美颜修改。请先在工作台保存新版本，或在“更多操作”中明确放弃修改，再切换输入或资产。\n当前模型和输入会保持不变。"),
        _L("保留未保存修改"), wxOK | wxCANCEL | wxICON_INFORMATION);
    choice.SetOKCancelLabels(_L("返回工作台处理"), _L("暂不切换"));
    if (choice.ShowModal() == wxID_OK) set_finishing_workbench(true);
    return true;
}

void ModelGenerationPanel::on_discard(wxCommandEvent&)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty()) return;
    if (preserve_unsaved_finishing()) return;
    if (m_ready || m_model_preview_ready || m_style_preview_ready) {
        wxMessageDialog choice(this,
            _L("要先看看重新设计的建议吗？当前历史模型会保留，图片和描述可继续使用。"),
            _L("修改输入，重新设计"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        choice.SetYesNoCancelLabels(_L("修改输入"), _L("先看建议"), _L("留在当前作品"));
        const int answer = choice.ShowModal();
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
            wxMessageDialog guidance(this, advice, _L("重新设计建议"), wxOK | wxCANCEL | wxICON_INFORMATION);
            guidance.SetOKCancelLabels(_L("继续修改输入"), _L("返回调整作品"));
            if (guidance.ShowModal() != wxID_OK) return;
        }
    }
    const bool reuse_palette = m_legacy_generation_state.palette_source == 2 && !current_palette().empty();
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
    if (m_prepare_base) m_prepare_base->SetValue(false);
    m_palette_recommendation_confirmed = reuse_palette;
    // Redesign is an explicit destination change; the empty model page no
    // longer redirects implicitly to Image during presentation/layout.
    m_workspace_view = WorkspaceView::Image;
    refresh_controls();
    m_prompt->SetFocus();
}

wxWindow* ModelGenerationPanel::build_model_finishing(wxWindow* parent)
{
    auto* panel = new ModelGenerationInputStyle::RoundedPanel(parent);
    panel->SetMinSize(wxSize(FromDIP(340), FromDIP(400)));
    m_finishing_panel = panel;
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(panel, wxID_ANY, _L("3D 美颜工作台"));
    title->SetFont(wxGetApp().bold_font());
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    heading->Add(title, 1, wxALIGN_CENTER_VERTICAL);
    auto* back = new Button(panel, _L("返回"));
    back->SetName("input_quiet");
    back->SetMinSize(FromDIP(wxSize(64, 36)));
    back->SetPaddingSize(FromDIP(wxSize(12, 6)));
    back->SetToolTip(_L("返回模型总览使用检查、多色与切片工具；当前编辑和视角继续保留。"));
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_finishing_workbench(false); });
    heading->Add(back, 0, wxLEFT, FromDIP(8));
    sizer->Add(heading, 0, wxEXPAND | wxALL, FromDIP(16));
    m_finishing_version = new Label(panel, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(280, -1)));
    m_finishing_version->SetMinSize(wxSize(1, -1));
    m_finishing_version->SetName("input_secondary");
    sizer->Add(m_finishing_version, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    auto* scroll = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    scroll->SetMinSize(wxSize(1, 1));
    scroll->SetScrollRate(0, FromDIP(12));
    m_finishing_editor_scroll = scroll;
    auto* editor = new wxBoxSizer(wxVERTICAL);
    auto* gestures = new Label(scroll,
        _L("Alt + 左键旋转 · 右键平移 · 滚轮缩放\n原件保留，预览后可对比或放弃。"),
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(280, -1)));
    gestures->SetMinSize(wxSize(1, -1));
    gestures->SetName("input_secondary");
    editor->Add(gestures, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_finishing_mode = new BeautyChoice(scroll);
    m_finishing_mode->Append(_L("整体美颜"));
    m_finishing_mode->Append(_L("区域编辑 · 改色与修形"));
    m_finishing_mode->SetSelection(0);
    editor->Add(m_finishing_mode, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_finishing_gray = new wxCheckBox(scroll, wxID_ANY, _L("灰模观察凹凸（仅显示）"));
    m_finishing_gray->SetName(m_finishing_gray->GetLabel());
    editor->Add(m_finishing_gray, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_finishing_gray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        m_model_preview->set_gray_view(m_finishing_gray->GetValue());
    });

    auto* overall = new wxPanel(scroll);
    m_finishing_overall_controls = overall;
    auto* overall_sizer = new wxBoxSizer(wxVERTICAL);
    m_finishing_preset = new BeautyChoice(overall);
    for(const auto& name : {_L("轻柔 · 保留细节"), _L("适中 · 柔滑表面"), _L("自定义强度")}) m_finishing_preset->Append(name);
    m_finishing_preset->SetSelection(0);
    overall_sizer->Add(m_finishing_preset, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    m_finishing_surface = new wxCheckBox(overall, wxID_ANY, _L("表面美化（保护边界和锐边）"));
    m_finishing_surface->SetName(m_finishing_surface->GetLabel());
    m_finishing_surface->SetValue(true);
    overall_sizer->Add(m_finishing_surface, 0, wxBOTTOM, FromDIP(12));
    m_finishing_strength_label = new wxStaticText(overall, wxID_ANY, _L("处理强度：15%"));
    overall_sizer->Add(m_finishing_strength_label, 0, wxEXPAND);
    auto* strength = new BeautyStrength(overall, [this](int value) {
        m_finishing_strength = value;
        m_finishing_strength_label->SetLabel(wxString::Format(_L("处理强度：%d%%"), value));
        m_finishing_preset->SetSelection(value == 15 ? 0 : value == 35 ? 1 : 2);
        refresh_model_finishing(); // Parameter changes never start processing.
    });
    m_finishing_strength_control = strength;
    overall_sizer->Add(strength, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* hint = new Label(overall, _L("保守柔滑整体小凹凸；按当前强度预览后可对比。\n保留贴图和材质，不自动修补拓扑。"),
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(280, -1)));
    hint->SetMinSize(wxSize(1, -1)); hint->SetName("input_secondary");
    overall_sizer->Add(hint, 0, wxEXPAND);
    overall->SetSizer(overall_sizer);
    editor->Add(overall, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_finishing_preset->Bind(wxEVT_COMBOBOX, [this, strength](wxCommandEvent&) {
        if(m_finishing_preset->GetSelection() < 2) strength->SetValue(m_finishing_preset->GetSelection() == 0 ? 15 : 35);
    });
    m_finishing_surface->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_model_finishing(); });
    m_finishing_mode->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        if(m_finishing_mode->GetSelection() == 0 && m_beauty_controls->has_changes()) {
            m_finishing_mode->SetSelection(1);
            m_finishing_status->SetLabel(_L("请先保存或明确放弃区域草稿，再处理整体表面。"));
        } else {
            m_finishing_status->SetLabel(m_finishing_mode->GetSelection() == 0
                ? _L("调整整体强度后点击预览；确认效果再保存新版本。")
                : _L("点选拼图改色，调整范围后预览并保存新版本。"));
        }
        refresh_model_finishing();
    });

    m_beauty_controls = new BeautyWorkbenchControls(scroll, m_model_preview, m_palette_provider, [this] {
        wxWeakRef<ModelGenerationPanel> weak(this);
        wxGetApp().CallAfter([weak] {
            // Restored drafts also change the saved-version actions and report
            // scope in the overview, even before another navigation event.
            if (weak && !weak->m_shutdown) weak->refresh_controls();
        });
    });
    editor->Add(m_beauty_controls, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    scroll->SetSizer(editor);
    sizer->Add(scroll, 1, wxEXPAND);
    // Actions stay in the viewport while only the editor content scrolls.
    auto button = [this, sizer, panel](Button*& target, const wxString& label) {
        target = new Button(panel, label);
        target->SetName("input_field");
        target->SetMinSize(wxSize(-1, FromDIP(36)));
        sizer->Add(target, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    };
    button(m_finishing_preview, _L("预览新版本"));
    button(m_finishing_compare, _L("查看修改前"));
    button(m_finishing_accept, _L("保存为新版本"));
    button(m_finishing_discard, _L("返回继续编辑"));
    button(m_finishing_cancel, _L("取消生成预览"));
    button(m_finishing_color_match, _L("下一步：匹配打印颜色"));
    m_finishing_preview->SetName("input_primary");
    m_finishing_accept->SetName("input_primary");
    m_finishing_color_match->SetName("input_primary");
    m_finishing_color_match->SetToolTip(_L("保存美颜修改后，在当前模型上匹配打印耗材。"));
    m_finishing_status = new Label(panel, wxEmptyString,
        LB_AUTO_WRAP | wxST_NO_AUTORESIZE, FromDIP(wxSize(280, -1)));
    m_finishing_status->SetMinSize(wxSize(1, -1));
    m_finishing_status->SetName("input_secondary");
    sizer->Add(m_finishing_status, 0, wxEXPAND | wxALL, FromDIP(10));
    panel->SetSizer(sizer);

    m_finishing_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { preview_model_finishing(); });
    m_finishing_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_model_finishing(); });
    m_finishing_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { discard_model_finishing(); });
    m_finishing_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        m_finishing_status->SetLabel(_L("正在取消，本次预览不会替换当前模型。"));
        m_finishing_cancel->Disable();
    });
    m_finishing_color_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        open_model_color_matching(m_finishing_color_match);
    });
    auto compare = [this] {
        if (m_busy || m_finishing_candidate.empty()) return;
        const bool before = !m_finishing_before;
        show_finishing_version(before ? m_finishing_source : m_finishing_candidate, [this, before] {
            m_finishing_before = before;
            m_finishing_compare->SetLabel(before ? _L("查看修改后") : _L("查看修改前"));
            m_finishing_compare_model->SetLabel(m_finishing_compare->GetLabel());
            m_model_preview_message->SetLabel(before ? _L("修改前 · 当前版本") : _L("修改后 · 尚未保存"));
        });
    };
    m_finishing_compare->Bind(wxEVT_BUTTON, [compare](wxCommandEvent&) { compare(); });
    m_finishing_compare_model->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (m_busy || m_finishing_candidate.empty() || m_finishing_before) return;
        m_finishing_compare_held = true;
        m_finishing_compare_model->CaptureMouse();
        show_finishing_version(m_finishing_source, [this] {
            refresh_model_finishing();
            m_model_preview_message->SetLabel(_L("修改前 · 松开恢复修改后"));
        });
        if (m_preview_loading)
            m_model_preview_message->SetLabel(_L("正在准备修改前 · 松开恢复修改后"));
    });
    auto release_compare = [this] {
        if (!m_finishing_compare_held) return;
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        if (m_preview_loading) {
            // The candidate is still displayed. Retire the pending source without joining on the UI thread.
            if (m_preview_canceled) m_preview_canceled->store(true);
            m_model_preview_message->SetLabel(_L("修改后 · 尚未保存"));
        } else {
            m_model_preview_message->SetLabel(_L("修改前 · 当前版本"));
            show_finishing_version(m_finishing_candidate, [this] {
                refresh_model_finishing();
                m_model_preview_message->SetLabel(_L("修改后 · 尚未保存"));
            });
        }
    };
    m_finishing_compare_model->Bind(wxEVT_LEFT_UP, [release_compare](wxMouseEvent&) { release_compare(); });
    m_finishing_compare_model->Bind(wxEVT_MOUSE_CAPTURE_LOST,
        [release_compare](wxMouseCaptureLostEvent&) { release_compare(); });
    panel->Hide();
    return panel;
}

void ModelGenerationPanel::open_model_color_matching(wxWindow* entry)
{
    // Both entries advance the selected saved artifact through the same owner.
    if (!m_color_matching || !m_model_preview_ready || m_preview_loading || m_busy ||
        m_finishing_running || !m_finishing_candidate.empty() ||
        !m_beauty_controls || m_beauty_controls->has_changes() ||
        !is_nonempty_model(m_displayed_model_path)) return;
    AI::GeneratedModelArtifact artifact;
    artifact.local_path = m_displayed_model_path;
    artifact.job_id = m_displayed_model_job_id;
    artifact.format = AI::model_artifact_format(m_displayed_model_path);
    artifact.color_encoding = m_artifact_color_encoding;
    artifact.generation_palette = m_displayed_model_palette;
    artifact.used_printable_colors = m_job_use_printable_colors;
    m_color_matching_entry = entry;
    m_color_matching_entry_sequence = m_sequence;
    m_color_matching(artifact);
}

void ModelGenerationPanel::refresh_model_tools()
{
    if (!m_model_overview_color_match || !m_model_overview_slice) return;
    const bool clean = !m_busy && !m_preview_loading && m_model_preview_ready &&
        !m_finishing_running && m_finishing_candidate.empty() &&
        m_beauty_controls && !m_beauty_controls->has_changes();
    m_model_overview_color_match->Enable(clean && static_cast<bool>(m_color_matching));
    m_model_overview_color_match->SetToolTip(clean
        ? _L("为当前保存版本匹配打印耗材；确认后再进入准备页。")
        : _L("请先保存或放弃当前美颜修改，等待处理完成后再匹配打印颜色。"));
    m_model_overview_slice->Enable(clean && m_import->IsEnabled());
    const bool return_to_preparation = m_model_preview_ready && !m_ready && m_job_id.empty() &&
        m_prepare_navigation && !m_last_imported_model_path.empty() &&
        m_last_imported_model_path == m_displayed_model_path;
    m_model_overview_slice->SetLabel(return_to_preparation ? _L("返回工程准备") : _L("导入切片"));
    m_model_overview_slice->SetToolTip(clean
        ? _L("沿用当前版本进入准备页，设置尺寸和底座；不会自动切片或发送打印。")
        : _L("请先保存或放弃当前美颜修改；预览候选不能直接导入切片。"));
}

void ModelGenerationPanel::set_finishing_workbench(bool enabled)
{
    // A restarted application has no in-memory edit yet. Enter the mode that
    // restores its persisted draft before offering another model operation.
    if (enabled && !m_finishing_workbench && m_finishing_candidate.empty() &&
        !m_finishing_running && !m_displayed_model_path.empty()) {
        try {
            if (BeautyWorkbenchControls::has_saved_draft(m_displayed_model_path))
                m_finishing_mode->SetSelection(1);
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning) << "Cannot enter beauty workbench: " << error.what();
            m_status->SetLabel(_L("草稿暂时无法读取，当前模型已保留。请恢复文件访问或存储后重新进入。"));
            m_model_preview_message->SetLabel(m_status->GetLabel());
            refresh_controls();
            return;
        }
    }
    if(enabled && !m_finishing_workbench && m_finishing_candidate.empty() &&
        !m_finishing_running && !m_beauty_controls->has_changes())
        m_finishing_status->SetLabel(m_finishing_mode->GetSelection() == 0
            ? _L("调整整体强度后点击预览；确认效果再保存新版本。")
            : _L("点选拼图改色，调整范围后预览并保存新版本。"));
    if (m_finishing_workbench && !enabled)
        m_model_preview_message->SetLabel(_L("当前显示：3D 模型总览。"));
    if (enabled && m_library_drawer_open) set_library_drawer(false);
    m_finishing_workbench = enabled;
    if (enabled) m_status->SetLabel(_L("当前保存版本已保留；编辑准备状态见右侧工作台。"));
    m_model_preview->set_gray_view(enabled && m_finishing_gray && m_finishing_gray->GetValue());
    if (enabled && m_preview_book) { m_preview_book->SetSelection(0); show_model_comparison(); }
    m_preview_area->Show(!enabled);
    m_expand_images->SetValue(false);
    m_expand_images->Show(!enabled);
    m_zoom_out->Show(!enabled); m_zoom_in->Show(!enabled);
    m_zoom_fit->Show(!enabled); m_preview_zoom->Show(!enabled);
    m_finishing_shortcut->SetLabel(enabled ? _L("返回模型总览") : _L("3D 美颜工作台"));
    m_preview_stage_hint->Show(!enabled);
    m_preview_message->Show(!enabled);
    m_result_summary->Show(!enabled);
    m_preview_kind->Show(!enabled);
    m_preview_kind->SetLabel(enabled ? _L("美颜工作台") : _L("结果对照"));
    m_model_decision_panel->Hide();
    // The compact overview and beauty editor own their actions outside the GL
    // preview; keep the legacy trial state without exposing its old control row.
    m_model_preview->set_color_controls_visible(false);
    refresh_model_finishing();
    Layout();
    m_comparison_panel->Layout();
    m_model_page->Layout(); m_model_page->FitInside(); m_model_page->Scroll(0, 0);
    refresh_controls();
    if (enabled) {
        sync_finishing_editor_scroll_track();
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->m_finishing_editor_scroll->IsShownOnScreen()) return;
            weak->m_finishing_editor_scroll->FitInside();
            weak->m_finishing_editor_scroll->Refresh();
            weak->sync_finishing_editor_scroll_track();
        });
    }
    if (!enabled) {
        // The new history model can load while its overview is hidden behind
        // the editor. Rewrap after the overview regains its actual width.
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->m_model_overview->IsShownOnScreen()) return;
            for (wxStaticText* text : {weak->m_model_overview_stats,
                                       weak->m_model_overview_quality_status,
                                       weak->m_model_overview_quality_summary}) {
                static_cast<Label*>(text)->Wrap(text->GetSize().x);
                text->InvalidateBestSize();
            }
            weak->m_model_overview->Layout();
            static_cast<wxScrolledWindow*>(weak->m_model_overview)->FitInside();
            weak->sync_model_overview_scroll_track();
        });
    }
}

void ModelGenerationPanel::open_model_finishing()
{
    if (m_finishing_workbench || !m_finishing_shortcut->IsEnabled()) return;
    if (AI::model_artifact_format(m_displayed_model_path) == "obj") {
        const auto source = m_displayed_model_path;
        const auto source_job = m_displayed_model_job_id;
        BeautyCopyConfirmation confirm(this);
        if (confirm.ShowModal() != wxID_OK) return;
        // The modal event loop can deliver an asynchronous model update.
        // Accept only the artifact for which the user confirmed this copy.
        if (source != m_displayed_model_path || source_job != m_displayed_model_job_id ||
            !m_finishing_shortcut->IsEnabled()) return;
        import_local_model(source, true);
        return;
    }
    set_finishing_workbench(true);
}

void ModelGenerationPanel::refresh_model_finishing()
{
    if (!m_finishing_panel || !m_beauty_controls) return;
    const auto access = workbench_access(m_model_preview_ready, is_nonempty_model(m_displayed_model_path),
        m_preview_loading || m_preview_download_in_flight, m_busy);
    const bool can_enter = access == WorkbenchAccess::Available;
    const bool needs_glb_copy = can_enter && AI::model_artifact_format(m_displayed_model_path) == "obj";
    const wxString entry_label = m_finishing_workbench ? _L("返回模型总览")
        : needs_glb_copy ? _L("创建 GLB 副本并美颜")
        : can_enter ? _L("3D 美颜工作台")
        : access == WorkbenchAccess::Loading ? _L("美颜：等待模型加载")
        : access == WorkbenchAccess::Busy ? _L("美颜：等待当前任务")
        : _L("美颜（需 3D 模型）");
    if (m_finishing_shortcut->GetLabel() != entry_label) m_finishing_shortcut->SetLabel(entry_label);
    m_finishing_shortcut->Enable(m_finishing_workbench || can_enter);
    m_finishing_shortcut->SetToolTip(m_finishing_workbench ? _L("返回模型总览，保留当前编辑；保存后可将模型加入工程。")
        : needs_glb_copy ? _L("本地创建可编辑的 GLB 副本并打开美颜工作台；原 OBJ 模型和 Task ID 保持不变。")
        : can_enter ? _L("编辑当前 3D 模型；原件保留，保存后生成独立版本。")
        : access == WorkbenchAccess::Loading ? _L("正在加载 3D 模型，完成后即可进入美颜工作台。")
        : access == WorkbenchAccess::Busy ? _L("请等待当前生成或保存任务完成，再进入美颜工作台。")
        : _L("当前只有图片或文字。请先生成 3D，或在模型库中打开已有 3D 模型。"));
    if (!m_finishing_running && !m_finishing_candidate.empty() && m_displayed_model_path != m_finishing_source) {
        boost::system::error_code ignored;
        if (m_displayed_model_path != m_finishing_candidate)
            boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
        m_finishing_source.clear();
        m_finishing_serialized_workbench.clear();
        m_finishing_serialized_geometry_id.clear();
    }
    const bool pending = !m_finishing_candidate.empty();
    const bool ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    const bool visible = m_finishing_workbench && (ready || m_finishing_running || pending);
    const bool editable = visible && ready && !m_busy && !m_finishing_running && !pending;
    const bool overall = m_finishing_mode->GetSelection() == 0;
    const bool regional_changes = m_beauty_controls->has_changes();
    m_finishing_panel->Show(visible);
    m_beauty_controls->synchronize(m_displayed_model_path, editable && !overall, visible && !overall && !pending && !m_finishing_running);
    m_model_preview->set_selection_enabled(editable && !overall);
    m_model_preview->set_selection_overlay_visible(!overall);
    const auto stage = finishing_stage(overall ? ready : m_beauty_controls->ready(), m_finishing_running,
        pending, regional_changes, !overall && m_beauty_controls->preparation_failed());
    const bool editing = stage == FinishingStage::Editing;
    const bool saved = stage == FinishingStage::Saved;
    const bool preparation_failed = !overall && stage == FinishingStage::Failed;
    // Keep the flex item present even in preview, so footer actions stay at the bottom.
    for (auto* child : m_finishing_editor_scroll->GetChildren()) child->Show(!pending && !m_finishing_running);
    m_beauty_controls->Show(!overall && !pending && !m_finishing_running);
    m_finishing_overall_controls->Show(overall && !pending && !m_finishing_running);
    m_finishing_mode->Enable(editable);
    m_finishing_gray->Enable(visible && !m_finishing_running);
    m_finishing_overall_controls->Enable(editable);
    wxString version;
    switch (stage) {
    case FinishingStage::Preparing: version = _L("正在准备编辑 · 原件保留"); break;
    case FinishingStage::Failed:
        version = _L("准备失败 · 原件保留\n恢复文件访问或存储后可重新准备编辑。");
        break;
    case FinishingStage::Processing: version = _L("正在生成预览 · 草稿保留"); break;
    case FinishingStage::Preview:
        version = m_finishing_before || m_finishing_compare_held
            ? _L("修改前版本 · 对照查看\n待保存预览仍保留，可切回修改后。")
            : _L("修改后预览 · 尚未保存\n确认效果后保存为独立版本，原件保留。");
        break;
    case FinishingStage::Editing: version = _L("编辑草稿 · 修改待保存"); break;
    case FinishingStage::Saved: version = _L("当前版本 · 没有未保存修改"); break;
    }
    if (!overall && !pending && !m_finishing_running && m_beauty_controls->showing_original())
        version = _L("原始全彩 · 仅对照查看\n") + version;
    m_finishing_version->SetLabel(version);
    m_finishing_version->InvalidateBestSize();
    if (visible) m_model_preview_message->SetLabel(version.BeforeFirst('\n'));
    m_finishing_preview->SetLabel(preparation_failed ? _L("重新准备编辑") : overall ? _L("预览处理效果") : _L("预览新版本"));
    m_finishing_preview->Show(preparation_failed || editing || (overall && saved));
    m_finishing_preview->Enable(editable && (preparation_failed || (overall ? m_finishing_surface->GetValue() && m_finishing_strength > 0 && !regional_changes : regional_changes)));
    for (Button* action : {m_finishing_compare, m_finishing_accept, m_finishing_discard}) {
        action->Show(pending);
        action->Enable(pending && !m_busy);
    }
    m_finishing_compare_model->Show(pending);
    m_finishing_cancel->Show(m_finishing_running);
    m_finishing_cancel->Enable(m_finishing_running && m_finishing_canceled && !m_finishing_canceled->load());
    m_finishing_color_match->Show(saved);
    m_finishing_color_match->Enable(editable && (overall || m_beauty_controls->ready()) &&
        m_color_matching && !m_beauty_controls->has_changes());
    // Preview is the primary next step in the overall editor. Matching the
    // unchanged version remains available, but must not compete with preview.
    const bool match_primary = !overall;
    m_finishing_color_match->SetName(match_primary ? "input_primary" : "input_quiet");
    for (auto* action : {m_finishing_preview, m_finishing_accept})
        ModelGenerationInputStyle::apply_control(action, ModelGenerationInputStyle::Role::PrimaryAction);
    ModelGenerationInputStyle::apply_control(m_finishing_color_match,
        match_primary ? ModelGenerationInputStyle::Role::PrimaryAction : ModelGenerationInputStyle::Role::QuietAction);
    m_import->Enable(m_model_import_available && !pending && !m_finishing_running &&
        !m_beauty_controls->has_changes());
    if (m_finishing_running) m_stop->Hide();
    m_recheck_model->Enable(m_recheck_model->IsEnabled() && !pending && !m_finishing_running &&
        !m_beauty_controls->has_changes());
    m_recheck_model->SetToolTip(_L("检查当前保存版本；请先保存或撤销编辑中的修改。不会调用付费 AI、修形或切片。"));
    if (m_displayed_model_job_id.rfind("finish-", 0) == 0)
        m_visual_review_model->Disable();
    refresh_model_tools();
    m_finishing_status->InvalidateBestSize();
    m_finishing_panel->Layout();
    m_finishing_editor_scroll->Layout();
    m_finishing_editor_scroll->FitInside();
    sync_finishing_editor_scroll_track();
    if (auto* page = dynamic_cast<wxScrolledWindow*>(m_finishing_panel->GetParent())) {
        page->Layout(); page->FitInside();
    }
}

void ModelGenerationPanel::preview_model_finishing()
{
    const bool overall = m_finishing_mode->GetSelection() == 0;
    if (m_busy || m_shutdown || !m_finishing_workbench || !m_beauty_controls ||
        m_finishing_running || !m_finishing_candidate.empty()) return;
    if (!overall && m_beauty_controls->preparation_failed()) {
        if (!m_model_preview_ready || !is_nonempty_model(m_displayed_model_path)) return;
        m_beauty_controls->synchronize(m_displayed_model_path, false, false);
        m_beauty_controls->synchronize(m_displayed_model_path, true, true);
        m_status->SetLabel(_L("当前保存版本已保留；编辑准备状态见右侧工作台。"));
        refresh_model_finishing();
        return;
    }
    if (!overall && (!m_beauty_controls->ready() || !m_beauty_controls->has_changes())) return;
    if(overall && (!m_finishing_surface->GetValue() || m_finishing_strength <= 0 || m_beauty_controls->has_changes()))return;
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，请完成后再预览。"));
        return;
    }
    const auto source = m_displayed_model_path;
    if (!is_nonempty_model(source)) {
        m_finishing_status->SetLabel(_L("模型文件已不存在，请从历史资产重新加载。"));
        return;
    }
    BeautyWorkbenchControls::SaveCapture save;
    std::shared_ptr<AI::ModelFinishingOptions> options;
    try {
        options = std::make_shared<AI::ModelFinishingOptions>(AI::ModelFinishingOptions{false, false, 0.0});
        if(overall) {
            if(BeautyWorkbenchControls::has_saved_draft(source)) {
                m_finishing_status->SetLabel(_L("此模型有区域草稿。请切到区域编辑，保存或明确放弃后再处理整体表面。"));
                return;
            }
            options->smooth_surface=true;
            options->strength=m_finishing_strength/100.0;
        } else save=m_beauty_controls->capture_save();
    } catch (const std::exception& error) {
        m_finishing_status->SetLabel(from_u8(error.what()));
        return;
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    // The worker fills this snapshot before posting its result; history
    // acceptance reads it only after the worker is joined on the UI thread.
    m_finishing_options = options;
    m_finishing_serialized_workbench.clear();
    m_finishing_serialized_geometry_id.clear();
    m_finishing_source = source;
    m_finishing_id = "finish-" + new_request_id();
    const auto destination = source.parent_path() / temp_path(m_finishing_id, "glb").filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true;
    m_busy = true;
    m_finishing_status->SetLabel(_L("正在生成美颜预览，当前版本保持不变……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    const auto view = m_model_preview->view_state();
    try {
        m_finishing_worker = std::thread([weak, source, destination, options, save=std::move(save),
                                           canceled, sequence, view] {
            AI::ModelFinishingResult result;
            try {
                if(canceled->load())result.canceled=true;
                else {
                    if(save.prepare_record) {
                        options->beauty_puzzle=true;
                        options->beauty_document=save.prepare_record();
                        options->beauty_surface=std::move(save.surface);
                    }
                    if(canceled->load())result.canceled=true;
                    else result=AI::finish_model_artifact(source, destination, *options,
                        [canceled] { return canceled->load(); });
                }
            }catch(const std::exception& error){result.error=error.what();}
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string preview_error;
            if (result.success && result.changed() && !canceled->load()) {
                try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, {}, {}, true,
                    [canceled] { return canceled->load(); }); }
                catch (const std::exception& error) { preview_error = error.what(); }
                if(preview_error.empty() && !canceled->load()) try {
                    prepared->prepare_render_geometry([canceled]{return canceled->load();},true);
                } catch(const std::exception& error) {preview_error=error.what();}
            }
            std::string serialized_workbench, serialized_geometry_id;
            if (result.success && result.changed() && preview_error.empty() &&
                !prepared->geometry_id.empty() && options->beauty_puzzle && !canceled->load()) {
                try {
                    serialized_geometry_id = prepared->geometry_id;
                    serialized_workbench = BeautyWorkbenchControls::accepted_document(
                        *options, serialized_geometry_id).dump();
                } catch (const std::exception& error) {
                    // Keep the preview usable; acceptance can use the old writer.
                    BOOST_LOG_TRIVIAL(warning) << "Cannot pre-encode beauty history: " << error.what();
                    serialized_workbench.clear();
                }
            }
            wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled,
                                  prepared, preview_error, view,
                                  serialized_workbench=std::move(serialized_workbench),
                                  serialized_geometry_id=std::move(serialized_geometry_id)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    return;
                }
                auto* self = weak.get();
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = false;
                self->m_busy = false;
                self->m_finishing_result = result;
                if (result.canceled || canceled->load()) {
                    self->m_finishing_status->SetLabel(_L("已取消，当前版本保持不变。"));
                } else if (!result.success) {
                    BOOST_LOG_TRIVIAL(warning) << "Beauty preview failed: " << result.error;
                    const bool storage_write_failed =
                        result.error == "Unable to create the model file." ||
                        result.error == "Unable to write the complete model." ||
                        result.error == "Unable to create the edited GLB version." ||
                        result.error == "The edited GLB was not written completely." ||
                        result.error == "Cannot write the edited GLB." ||
                        result.error == "Cannot create a new GLB staging directory." ||
                        result.error == "Cannot create a puzzle staging directory.";
                    const auto detail = storage_write_failed
                        ? _L("无法写入美颜预览。请恢复保存目录权限或磁盘空间后重试；当前参数和原件已保留。")
                        : result.error == "This selection has vertex colors or untextured materials; texture appearance editing is not available for it yet."
                        ? _L("该模型使用顶点色或无贴图材质，目前无法保存区域改色。草稿已保留；可撤销改色后使用整体美颜，或换用带内嵌贴图的 GLB。")
                        : from_u8(result.error);
                    self->m_finishing_status->SetLabel(_L("预览未完成，当前版本已保留：") + detail);
                } else if (!result.changed()) {
                    self->m_finishing_status->SetLabel(_L("本次修改没有产生新版本。"));
                } else {
                    size_t triangles = 0, colors = 0;
                    Vec3d dimensions = Vec3d::Zero();
                    std::string error = preview_error;
                    if (!error.empty() || !self->m_model_preview->load_prepared_model(
                            std::move(*prepared), {}, triangles, dimensions, colors, error)) {
                        self->m_finishing_status->SetLabel(_L("预览加载失败，当前版本已保留：") + from_u8(error));
                    } else {
                        if (self->m_model_preview->geometry_id() == serialized_geometry_id) {
                            self->m_finishing_serialized_workbench = std::move(serialized_workbench);
                            self->m_finishing_serialized_geometry_id = std::move(serialized_geometry_id);
                        }
                        self->m_model_preview->restore_view(view);
                        set_model_viewport_facts(self->m_model_stats, triangles, colors);
                        self->m_model_stats->SetLabel(wxString::Format(
                            _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                            static_cast<unsigned long long>(triangles),
                            static_cast<unsigned long long>(colors),
                            dimensions.x(), dimensions.y(), dimensions.z()));
                        self->m_finishing_candidate = destination;
                        self->m_finishing_before = false;
                        self->m_finishing_compare->SetLabel(_L("查看修改前"));
                        self->m_model_preview_message->SetLabel(_L("修改后 · 尚未保存"));
                        self->m_finishing_status->SetLabel(_L("预览已就绪。对照修改前后，再保存到历史资产。"));
                    }
                }
                if (self->m_finishing_candidate.empty() && result.success) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(destination, ignored);
                }
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->refresh_controls();
            });
        });
    } catch (const std::exception& error) {
        m_finishing_running = false;
        m_busy = false;
        m_finishing_status->SetLabel(_L("暂时无法启动预览：") + from_u8(error.what()));
        refresh_controls();
    }
}

void ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path,
    std::function<void()> installed)
{
    if (m_shutdown || m_preview_loading) return;
    const auto view = m_model_preview->view_state();
    const auto source = m_finishing_source, candidate = m_finishing_candidate;
    const auto previous_message = m_model_preview_message->GetLabel();
    load_model_preview_async(path, {},
        [this, view, installed](size_t triangles, Vec3d dimensions, size_t colors, double) {
            m_model_preview->restore_view(view);
            m_model_preview->set_color_controls_visible(!m_finishing_workbench);
            m_model_preview_ready = true;
            set_model_viewport_facts(m_model_stats, triangles, colors);
            m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
                dimensions.x(), dimensions.y(), dimensions.z()));
            m_finishing_status->SetLabel(_L("预览已就绪。对照修改前后，再保存到历史资产。"));
            m_finishing_status->SetToolTip(wxEmptyString);
            installed();
            m_finishing_status->Wrap(FromDIP(280));
            refresh_controls();
        },
        [this, previous_message](std::string error) {
            m_finishing_compare_held = false;
            if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
            m_model_preview_message->SetLabel(previous_message);
            m_finishing_status->SetLabel(_L("模型加载失败，当前版本仍保留。\n请检查文件是否可用，再重试。"));
            m_finishing_status->SetToolTip(from_u8(error));
            m_finishing_status->Wrap(FromDIP(280));
            refresh_controls();
        }, {}, {}, [this, source, candidate] {
            return m_finishing_source == source && m_finishing_candidate == candidate &&
                   m_displayed_model_path == source;
        });
}

void ModelGenerationPanel::select_local_finishing_version(const boost::filesystem::path& path, const std::string& id)
{
    const bool preview_download_was_active = m_preview_download_in_flight;
    ++m_sequence;
    if (preview_download_was_active) m_client.cancel_current();
    m_preview_download_in_flight = false;
    if (preview_download_was_active) {
        m_preview_download_cancelled = true;
        m_preview_path.clear();
        m_style_preview_ready = false;
        m_preview_output_available = false;
    }
    m_poll_timer.Stop();
    m_job_id.clear(); m_job_palette.clear(); m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_artifact_path = m_displayed_model_path = path;
    m_displayed_model_job_id = id;
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_artifact_format = AI::model_artifact_format(path);
    m_artifact_color_encoding = "textures_or_vertex_colors";
    m_ready = true; m_library_model_loaded = true; m_artifact_download_started = false;
    m_awaiting_confirmation = false; m_awaiting_palette_confirmation = false;
    m_last_imported_model_path.clear();
    clear_model_quality();
    m_visual_quality = {};
    m_model_refinement = {};
}

void ModelGenerationPanel::accept_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    const bool showing_before=m_finishing_before || m_finishing_compare_held;
    show_finishing_version(m_finishing_candidate, [this, showing_before] {
    try {
        const auto root = generated_models_root();
        nlohmann::json metadata {
            {"schema_version", 4}, {"job_id", m_finishing_id},
            {"model_path", m_finishing_candidate.lexically_relative(root).generic_string()},
            {"source", "local_finishing"}, {"prompt", "3D 美颜"},
            {"source_model", m_finishing_source.lexically_relative(root).generic_string()},
            {"source_sha256", m_finishing_result.source_sha256},
            {"model_sha256", m_finishing_result.output_sha256},
            {"palette", nlohmann::json::array()}, {"palette_roles", nlohmann::json::object()},
            {"use_printable_colors", false}, {"generated_at", std::time(nullptr)},
            {"triangle_count", m_finishing_result.faces_after},
            {"dimensions", m_finishing_result.dimensions},
            {"finishing", {{"beauty_puzzle", m_finishing_options->beauty_puzzle},
                {"smooth_surface", m_finishing_options->smooth_surface}, {"repair_mesh", m_finishing_options->repair_mesh},
                {"strength", m_finishing_options->strength}, {"changed_texture_pixels", m_finishing_result.changed_texture_pixels}}}
        };
        metadata["face_color_intent"] = m_model_preview->face_color_metadata();
        metadata["color_trial"] = m_model_preview->color_trial_metadata();
        metadata["semantic_color_state"] = m_model_preview->semantic_color_metadata();
        if (!m_reference_image_path.empty() && path_is_inside(root, m_reference_image_path))
            metadata["reference_image_path"] = m_reference_image_path.lexically_relative(root).generic_string();
        if (!m_raw_preview_path.empty() && path_is_inside(root, m_raw_preview_path))
            metadata["ai_image_path"] = m_raw_preview_path.lexically_relative(root).generic_string();
        const bool regional=m_finishing_options->beauty_puzzle;
        const bool preencoded=regional && !m_finishing_serialized_workbench.empty() &&
            m_finishing_serialized_geometry_id==m_model_preview->geometry_id();
        if(regional && !preencoded) metadata["beauty_workbench"]=BeautyWorkbenchControls::accepted_document(
            *m_finishing_options,m_model_preview->geometry_id());
        AI::publish_beauty_version_record(library_metadata_path(m_finishing_id),
            m_finishing_candidate,m_finishing_source,metadata,
            preencoded?&m_finishing_serialized_workbench:nullptr);
    } catch(const std::exception& error) {
        // Retain the candidate, source, dirty flag and draft. Publication errors
        // must not advance the selected history version or clear its edit stack.
        m_finishing_before=showing_before;
        if(showing_before) show_finishing_version(m_finishing_source, [this]{refresh_model_finishing();});
        m_finishing_compare_held=false;
        m_finishing_compare->SetLabel(m_finishing_before?_L("查看修改后"):_L("查看修改前"));
        m_finishing_status->SetLabel(_L("保存未完成，尚未接受；预览与草稿已保留：")+from_u8(error.what()));
        m_status->SetLabel(m_finishing_status->GetLabel());
        refresh_controls();
        return;
    }
        if(m_finishing_options->beauty_puzzle) m_beauty_controls->mark_saved();
        select_local_finishing_version(m_finishing_candidate, m_finishing_id);
        m_finishing_candidate.clear();
        m_finishing_source.clear();
        m_finishing_serialized_workbench.clear();
        m_finishing_serialized_geometry_id.clear();
        m_finishing_status->SetLabel(_L("新版本已保存到历史资产。可继续编辑，或匹配打印颜色。"));
        m_status->SetLabel(m_finishing_status->GetLabel());
        m_model_preview_message->SetLabel(_L("当前显示：已保存的美颜版本。"));
        load_library_entries();
        refresh_controls();
    });
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    show_finishing_version(m_finishing_source, [this] {
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
        m_finishing_serialized_workbench.clear();
        m_finishing_serialized_geometry_id.clear();
        m_finishing_before = false;
        m_finishing_status->SetLabel(_L("已返回编辑，草稿保留。继续调整后可重新预览。"));
        m_status->SetLabel(m_finishing_status->GetLabel());
        m_model_preview_message->SetLabel(_L("编辑草稿 · 修改待保存"));
        refresh_controls();
    });
}

void ModelGenerationPanel::stop_model_finishing()
{
    if (m_finishing_compare_held) {
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        if (m_preview_canceled) m_preview_canceled->store(true);
    }
    if (m_finishing_canceled) m_finishing_canceled->store(true);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_running = false;
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    m_finishing_serialized_workbench.clear();
    m_finishing_serialized_geometry_id.clear();
}
} // namespace Slic3r::GUI
