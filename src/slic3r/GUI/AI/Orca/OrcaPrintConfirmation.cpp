#include "OrcaPrintConfirmation.hpp"
#include "OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Redesign/HonorBrandMark.hpp"
#include "slic3r/GUI/Redesign/RedesignTheme.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include <wx/dialog.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/region.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace Slic3r::GUI {
namespace {

constexpr int confirmation_scrollbar_width = 10;
constexpr int confirmation_scrollbar_thumb_width = 4;

class ConfirmationCloseButton final : public wxPanel
{
public:
    explicit ConfirmationCloseButton(wxWindow* parent)
        : wxPanel(parent, wxID_CLOSE, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetCanFocus(true);
        SetToolTip(_L("关闭"));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        rescale();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) { m_hovered = true; Refresh(false); event.Skip(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = false; m_pressed = false; Refresh(false); event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { m_pressed = true; SetFocus(); Refresh(false); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const bool activate = m_pressed && GetClientRect().Contains(event.GetPosition());
            m_pressed = false;
            Refresh(false);
            if (activate) emit_click();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER ||
                event.GetKeyCode() == WXK_SPACE) {
                emit_click();
                return;
            }
            event.Skip();
        });
    }

    void rescale()
    {
        const wxSize size = FromDIP(wxSize(32, 32));
        SetMinSize(size);
        SetSize(size);
        Refresh(false);
    }

private:
    void emit_click()
    {
        wxCommandEvent event(wxEVT_BUTTON, GetId());
        event.SetEventObject(this);
        ProcessWindowEvent(event);
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        auto graphics = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!graphics) return;
        const wxSize size = GetClientSize();
        if (m_hovered || m_pressed) {
            graphics->SetPen(*wxTRANSPARENT_PEN);
            graphics->SetBrush(wxBrush(m_pressed ? wxColour(66, 66, 70) : wxColour(55, 55, 59)));
            graphics->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(6));
        }
        const int inset = FromDIP(10);
        graphics->SetPen(wxPen(RedesignTheme::primary_text_colour(), std::max(1, FromDIP(1))));
        graphics->StrokeLine(inset, inset, size.x - inset, size.y - inset);
        graphics->StrokeLine(size.x - inset, inset, inset, size.y - inset);
        if (FindFocus() == this) {
            graphics->SetBrush(*wxTRANSPARENT_BRUSH);
            graphics->SetPen(wxPen(RedesignTheme::accent_colour(), std::max(1, FromDIP(1))));
            graphics->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2),
                                           size.y - FromDIP(2), FromDIP(6));
        }
    }

    bool m_hovered {false};
    bool m_pressed {false};
};

class ConfirmationScrollViewport final : public wxPanel
{
public:
    ConfirmationScrollViewport(wxWindow* parent, const wxColour& background)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background);
        m_scroller = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxVSCROLL | wxCLIP_CHILDREN | wxBORDER_NONE);
        m_scroller->SetBackgroundColour(background);
        m_scroller->SetScrollRate(0, FromDIP(16));
        m_scroller->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);

        m_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_bar->SetBackgroundColour(background);
        m_bar->SetCursor(wxCursor(wxCURSOR_HAND));
        m_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_bar(); });
        m_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) { begin_pointer(event); });
        m_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { drag_pointer(event); });
        m_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { end_pointer(); });
        m_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { end_pointer(); });
        m_bar->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            wxMouseEvent forwarded(event);
            forwarded.SetEventObject(m_scroller);
            m_scroller->GetEventHandler()->ProcessEvent(forwarded);
        });

        const auto refresh = [this](wxScrollWinEvent& event) { update_bar(); event.Skip(); };
        m_scroller->Bind(wxEVT_SCROLLWIN_TOP, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_BOTTOM, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_LINEUP, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_LINEDOWN, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_PAGEUP, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_PAGEDOWN, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_THUMBTRACK, refresh);
        m_scroller->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, refresh);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { layout_children(); event.Skip(); });
        layout_children();
    }

    wxScrolledWindow* scroller() const { return m_scroller; }

    void fit_inside()
    {
        m_scroller->Layout();
        m_scroller->FitInside();
        update_bar();
    }

private:
    wxRect thumb_rect() const
    {
        const int height = m_bar->GetClientSize().y;
        const int virtual_height = std::max(1, m_scroller->GetVirtualSize().y);
        const int visible_height = m_scroller->GetClientSize().y;
        const int thumb_height = std::min(height,
            std::max(FromDIP(24), height * visible_height / virtual_height));
        int view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
        m_scroller->GetViewStart(&view_x, &view_y);
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int range = std::max(1, virtual_height - visible_height);
        const int top = std::clamp(int(double(view_y) * unit_y * (height - thumb_height) / range),
                                   0, std::max(0, height - thumb_height));
        return wxRect(FromDIP(3), top, FromDIP(confirmation_scrollbar_thumb_width), thumb_height);
    }

    void paint_bar()
    {
        wxAutoBufferedPaintDC dc(m_bar);
        dc.SetBackground(wxBrush(m_bar->GetBackgroundColour()));
        dc.Clear();
        if (m_scroller->GetVirtualSize().y <= m_scroller->GetClientSize().y) return;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(105, 105, 112)));
        dc.DrawRoundedRectangle(thumb_rect(), FromDIP(2));
    }

    void begin_pointer(wxMouseEvent& event)
    {
        const wxRect thumb = thumb_rect();
        if (thumb.Contains(event.GetPosition())) {
            m_drag_offset = event.GetY() - thumb.y;
            m_dragging = true;
            m_bar->CaptureMouse();
            return;
        }
        int view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
        m_scroller->GetViewStart(&view_x, &view_y);
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int page = m_scroller->GetClientSize().y / std::max(1, unit_y);
        m_scroller->Scroll(view_x, std::max(0, view_y + (event.GetY() < thumb.y ? -page : page)));
        update_bar();
    }

    void drag_pointer(wxMouseEvent& event)
    {
        if (!m_dragging || !m_bar->HasCapture()) return;
        const wxRect thumb = thumb_rect();
        const int travel = m_bar->GetClientSize().y - thumb.height;
        int unit_x = 0, unit_y = 0;
        m_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int range = std::max(0, m_scroller->GetVirtualSize().y - m_scroller->GetClientSize().y);
        const int position = std::clamp(event.GetY() - m_drag_offset, 0, std::max(0, travel));
        m_scroller->Scroll(0, travel > 0 ?
            int(double(position) * range / travel / std::max(1, unit_y) + .5) : 0);
        update_bar();
    }

    void end_pointer()
    {
        if (m_bar->HasCapture()) m_bar->ReleaseMouse();
        m_dragging = false;
    }

    void layout_children()
    {
        const wxSize size = GetClientSize();
        const int bar_width = FromDIP(confirmation_scrollbar_width);
        m_scroller->SetSize(0, 0, std::max(0, size.x - bar_width), std::max(0, size.y));
        m_bar->SetSize(std::max(0, size.x - bar_width), 0, bar_width, std::max(0, size.y));
        update_bar();
    }

    void update_bar()
    {
        const bool needed = m_scroller->GetVirtualSize().y > m_scroller->GetClientSize().y;
        m_bar->Show(needed);
        if (needed) m_bar->Raise();
        m_bar->Refresh(false);
    }

    wxScrolledWindow* m_scroller {nullptr};
    wxPanel* m_bar {nullptr};
    int m_drag_offset {0};
    bool m_dragging {false};
};

class PrintConfirmationDialog final : public DPIDialog
{
public:
    explicit PrintConfirmationDialog(wxWindow* parent)
        : DPIDialog(parent, wxID_ANY, _L("打印确认"), wxDefaultPosition, wxDefaultSize,
                    wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_SHAPED)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::panel_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_background(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { update_shape(); event.Skip(); });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_ESCAPE) EndModal(wxID_CANCEL);
            else event.Skip();
        });

        m_root = new wxBoxSizer(wxVERTICAL);
        m_title_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_title_bar->SetBackgroundColour(RedesignTheme::panel_colour());
        m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
        auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_title_bar->SetSizer(title_sizer);
        m_brand = new HonorBrandMark(m_title_bar);
        title_sizer->Add(m_brand, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
        m_title = new wxStaticText(m_title_bar, wxID_ANY, _L("打印确认"), wxDefaultPosition,
                                   wxDefaultSize, wxST_ELLIPSIZE_END);
        RedesignTheme::style_medium_text(m_title, RedesignTheme::primary_text_colour(), 12);
        title_sizer->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
        m_close = new ConfirmationCloseButton(m_title_bar);
        title_sizer->Add(m_close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        m_root->Add(m_title_bar, 0, wxEXPAND);
        auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)), wxBORDER_NONE);
        divider->SetName("ai_content_color");
        divider->SetBackgroundColour(RedesignTheme::divider_colour());
        m_root->Add(divider, 0, wxEXPAND);
        SetSizer(m_root);

        bind_title_drag(m_title_bar);
        bind_title_drag(m_title);
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });
    }

    wxBoxSizer* content_sizer() const { return m_root; }
    void refresh_shape() { update_shape(); }

private:
    void bind_title_drag(wxWindow* source)
    {
        source->Bind(wxEVT_LEFT_DOWN, [this, source](wxMouseEvent&) {
            m_drag_offset = wxGetMousePosition() - GetPosition();
            m_drag_source = source;
            if (!source->HasCapture()) source->CaptureMouse();
        });
        source->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (m_drag_source && event.LeftIsDown()) Move(wxGetMousePosition() - m_drag_offset);
            else event.Skip();
        });
        source->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { finish_drag(); });
        source->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_drag_source = nullptr; });
    }

    void finish_drag()
    {
        if (m_drag_source && m_drag_source->HasCapture()) m_drag_source->ReleaseMouse();
        m_drag_source = nullptr;
    }

    void paint_background()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(GetBackgroundColour()));
        dc.DrawRoundedRectangle(GetClientRect(), FromDIP(12));
    }

    void update_shape()
    {
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        wxBitmap mask(size.x, size.y);
        wxMemoryDC dc(mask);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(*wxWHITE_BRUSH);
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
        dc.SelectObject(wxNullBitmap);
        SetShape(wxRegion(mask, *wxBLACK));
    }

    void on_dpi_changed(const wxRect& suggested_rect) override
    {
        (void) suggested_rect;
        m_title_bar->SetMinSize(wxSize(-1, FromDIP(52)));
        m_brand->Rescale();
        m_close->rescale();
        Layout();
        update_shape();
    }

    wxBoxSizer* m_root {nullptr};
    wxPanel* m_title_bar {nullptr};
    HonorBrandMark* m_brand {nullptr};
    wxStaticText* m_title {nullptr};
    ConfirmationCloseButton* m_close {nullptr};
    wxWindow* m_drag_source {nullptr};
    wxPoint m_drag_offset;
};

} // namespace

OrcaPrintAction show_orca_print_confirmation(wxWindow* parent, Plater& plater,
                                             const OrcaPrintTargetSnapshot& target)
{
    using namespace ModelGenerationInputStyle;
    auto* plate = plater.get_partplate_list().get_curr_plate();
    auto* bundle = wxGetApp().preset_bundle;
    OrcaSmartSlicingAdapter workspace(&plater);
    const auto confirmed_revision = capture_orca_print_confirmation_revision(workspace);
    const bool ready = confirmed_revision && plate && !plater.is_background_process_slicing() &&
                       plate->is_slice_result_ready_for_export();
    auto* result = ready ? plate->get_slice_result() : nullptr;
    const unsigned int result_id = result ? result->id : 0;
    const std::string result_path = result ? result->filename : std::string();
    const DynamicPrintConfig confirmed_config = bundle ? bundle->printers.get_edited_preset().config : DynamicPrintConfig();

    auto confirmation_current = [&] {
        return ready && plate == plater.get_partplate_list().get_curr_plate() &&
            !plater.is_background_process_slicing() && plate->is_slice_result_ready_for_export() &&
            plate->get_slice_result() && plate->get_slice_result()->id == result_id &&
            plate->get_slice_result()->filename == result_path && bundle &&
            bundle->printers.get_edited_preset().config == confirmed_config &&
            orca_print_confirmation_revision_current(confirmed_revision, workspace);
    };

    PrintConfirmationDialog dialog(wxGetTopLevelParent(parent));
    auto* root = dialog.content_sizer();
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    auto* surface = new RoundedPanel(&dialog);
    surface->SetMinSize(dialog.FromDIP(wxSize(330, 1)));
    auto* details_view = new ConfirmationScrollViewport(surface, panel);
    auto* details = details_view->scroller();
    details->SetMinSize(wxSize(1, 1));
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto add_text = [&](const wxString& value, const char* role = "") {
        auto* label = new Label(details, value, LB_AUTO_WRAP);
        label->SetMinSize(wxSize(1, -1));
        label->SetName(role);
        column->Add(label, 0, wxEXPAND | wxALL, dialog.FromDIP(10));
        return label;
    };
    add_text(_L("当前打印机预设"), "input_accent");
    add_text(bundle ? from_u8(bundle->printers.get_selected_preset().name) : _L("未选择打印机预设"));
    add_text(_L("传输目标"), "input_accent");
    if (target.selected) {
        add_text(target.device_name.empty() ? from_u8(target.device_id) : from_u8(target.device_name));
        add_text(orca_print_target_ready(target) ? _L("真实设备已连接并完成状态上报。") :
            !target.online ? _L("真实设备当前离线。") :
            !target.connected ? _L("真实设备尚未连接。") : _L("已连接，正在等待设备状态上报。"),
            orca_print_target_ready(target) ? "input_secondary" : "ai_status_warning");
    } else if (target.legacy_print_host) {
        add_text(_L("已配置打印主机"));
        add_text(_L("继续时保留原有 PrintHost 发送流程；该入口不表示已发现局域网设备。"), "input_secondary");
    } else {
        add_text(_L("未选择真实设备"));
        add_text(_L("请返回打印页选择已发现并连接的打印机；本地导出仍可用。"), "ai_status_warning");
    }
    add_text(_L("当前打印板与切片"), "input_accent");
    if (result) {
        wxString filename;
        try {
            const auto base = plater.get_export_gcode_filename(wxEmptyString, true).ToUTF8();
            filename = plate->fff_print() ? from_u8(Slic3r::fold_utf8_to_ascii(plate->fff_print()->output_filename(base ? base.data() : ""))) :
                _L("由原生导出对话框确定文件名");
        }
        catch (...) { filename = _L("由原生导出对话框确定文件名"); }
        add_text(_L("导出文件名（可更改）：") + filename);
        const auto minutes = static_cast<long long>(std::llround(result->print_statistics.modes[0].time / 60.0));
        add_text(wxString::Format(_L("预计耗时：%lld 小时 %lld 分钟\n实际使用耗材：%llu 个"),
            minutes / 60, minutes % 60,
            static_cast<unsigned long long>(result->print_statistics.total_volumes_per_extruder.size())));
    }
    auto* feedback = add_text(ready ? _L("切片结果有效。打印前仍需在原生预览检查支撑、方向及警告。") :
        _L("当前没有可用的正式切片结果。请返回工程完成切片；旧文件不能作为当前打印依据。"),
        ready ? "input_secondary" : "ai_status_warning");
    add_text(_L("设备选项"), "input_accent");
    add_text(_L("调平、摄像头、AI 检测与材料通道，按所选设备实际支持情况在下一步确认。当前不预设这些能力或开关。"), "input_secondary");
    details->SetSizer(column);
    auto* surface_column = new wxBoxSizer(wxVERTICAL);
    surface_column->Add(details_view, 1, wxEXPAND);
    surface->SetSizer(surface_column);
    details->Bind(wxEVT_SIZE, [details_view](wxSizeEvent& event) { details_view->fit_inside(); event.Skip(); });
    body->Add(surface, 1, wxEXPAND | wxALL, dialog.FromDIP(12));
    if (ready && plate->thumbnail_data.is_valid()) {
        const auto& thumbnail = plate->thumbnail_data;
        wxImage preview(thumbnail.width, thumbnail.height);
        preview.InitAlpha();
        for (unsigned int y = 0; y < thumbnail.height; ++y)
            for (unsigned int x = 0; x < thumbnail.width; ++x) {
                const auto* pixel = thumbnail.pixels.data() + 4 * ((thumbnail.height - 1 - y) * thumbnail.width + x);
                preview.SetRGB(x, y, pixel[0], pixel[1], pixel[2]);
                preview.SetAlpha(x, y, pixel[3]);
            }
        const int size = dialog.FromDIP(260);
        wxBitmap bitmap(preview.Scale(size, size, wxIMAGE_QUALITY_HIGH));
        bitmap.SetScaleFactor(dialog.GetDPIScaleFactor());
        auto* image = new wxStaticBitmap(&dialog, wxID_ANY, bitmap);
        image->SetBackgroundColour(background);
        image->SetName("ai_content_color");
        body->Add(image, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dialog.FromDIP(12));
    }
    root->Add(body, 1, wxEXPAND);
    auto* actions = new wxWrapSizer(wxHORIZONTAL);
    auto button = [&](const wxString& title, bool primary, int result_code) {
        auto* control = new Button(&dialog, title);
        control->SetName(primary ? "input_primary" : "input_field");
        control->SetPaddingSize(dialog.FromDIP(wxSize(14, 10)));
        actions->Add(control, 0, wxALL, dialog.FromDIP(6));
        control->Bind(wxEVT_BUTTON, [&, result_code](wxCommandEvent&) {
            if (result_code != wxID_CANCEL) {
                if (!confirmation_current()) {
                    feedback->SetLabel(_L("工程或切片已变化。请关闭此页，完成切片后重新确认。"));
                    feedback->SetName("ai_status_warning"); apply_control(feedback, Role::Warning);
                    dialog.Layout(); return;
                }
            }
            dialog.EndModal(result_code);
        });
        return control;
    };
    constexpr int upload_only_id = wxID_HIGHEST + 310;
    constexpr int upload_and_print_id = wxID_HIGHEST + 311;
    button(_L("返回检查"), false, wxID_CANCEL);
    button(_L("导出到电脑…"), false, wxID_SAVE)->Enable(ready);
    if (target.legacy_print_host) {
        auto* legacy = button(_L("继续到打印主机…"), true, upload_and_print_id);
        legacy->Enable(ready && orca_print_action_available(OrcaPrintAction::UploadAndPrint, target));
        legacy->EnableTooltipEvenDisabled();
        legacy->SetToolTip(!ready ? _L("先完成当前工程的正式切片。") :
            _L("进入原有 PrintHost 发送流程，不把配置地址显示为已发现设备。"));
    } else {
        auto* upload = button(_L("仅上传 G-code"), false, upload_only_id);
        upload->Enable(ready && orca_print_action_available(OrcaPrintAction::UploadOnly, target));
        upload->EnableTooltipEvenDisabled();
        upload->SetToolTip(!ready ? _L("先完成当前工程的正式切片。") :
            !orca_print_target_ready(target) ? _L("请先选择并连接真实打印机，等待设备状态上报。") :
            _L("上传当前盘的 G-code 文件，但不启动打印。"));
        auto* print = button(_L("上传并打印"), true, upload_and_print_id);
        print->Enable(ready && orca_print_action_available(OrcaPrintAction::UploadAndPrint, target));
        print->EnableTooltipEvenDisabled();
        print->SetToolTip(!ready ? _L("先完成当前工程的正式切片。") :
            !orca_print_target_ready(target) ? _L("请先选择并连接真实打印机，等待设备状态上报。") :
            target.printer_busy ? _L("打印机正在执行任务；当前可以仅上传 G-code，任务结束后再启动新打印。") :
            _L("上传当前盘的 G-code，并在下一步确认后启动打印。"));
    }
    root->Add(actions, 0, wxEXPAND | wxALL, dialog.FromDIP(6));
    apply(&dialog);
    const auto minimum = dialog.FromDIP(wxSize(680, 400));
    const auto preferred = dialog.FromDIP(wxSize(700, 560));
    const auto available = wxGetTopLevelParent(parent)->GetClientSize() -
                           dialog.FromDIP(wxSize(32, 32));
    dialog.SetMinSize(minimum);
    dialog.SetSize(wxSize(std::max(minimum.x, std::min(preferred.x, available.x)),
                          std::max(minimum.y, std::min(preferred.y, available.y))));
    dialog.Layout();
    details_view->fit_inside();
    dialog.refresh_shape();
    dialog.CentreOnParent();
    const auto frame = dialog.ToDIP(dialog.GetSize());
    const auto owner = dialog.ToDIP(wxGetTopLevelParent(parent)->GetClientSize());
    BOOST_LOG_TRIVIAL(info) << "ux_print_confirmation_layout frame_dip=" << frame.x << "x" << frame.y
                           << " owner_client_dip=" << owner.x << "x" << owner.y;
    const int action = dialog.ShowModal();
    if (action != wxID_CANCEL && !confirmation_current()) {
        wxMessageBox(_L("工程或切片已变化。请完成切片后重新确认。"), _L("打印确认"),
                     wxOK | wxICON_WARNING, wxGetTopLevelParent(parent));
        return OrcaPrintAction::Cancel;
    }
    if (action == wxID_SAVE) return OrcaPrintAction::ExportLocal;
    if (action == upload_only_id) return OrcaPrintAction::UploadOnly;
    if (action == upload_and_print_id) return OrcaPrintAction::UploadAndPrint;
    return OrcaPrintAction::Cancel;
}
}
