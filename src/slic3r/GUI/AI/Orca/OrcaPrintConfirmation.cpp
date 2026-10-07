#include "OrcaPrintConfirmation.hpp"
#include "OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/Utils/ASCIIFolding.hpp"
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/statbmp.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {
void show_orca_print_confirmation(wxWindow* parent, Plater& plater)
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
    const auto* host = bundle ? bundle->printers.get_edited_preset().config.option<ConfigOptionString>("print_host") : nullptr;
    const bool managed_device = bundle && (bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents"));
    const bool configured = managed_device || (host && !host->value.empty());

    auto confirmation_current = [&] {
        return ready && plate == plater.get_partplate_list().get_curr_plate() &&
            !plater.is_background_process_slicing() && plate->is_slice_result_ready_for_export() &&
            plate->get_slice_result() && plate->get_slice_result()->id == result_id &&
            plate->get_slice_result()->filename == result_path && bundle &&
            bundle->printers.get_edited_preset().config == confirmed_config &&
            (bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents")) == managed_device &&
            orca_print_confirmation_revision_current(confirmed_revision, workspace);
    };

    wxDialog dialog(wxGetTopLevelParent(parent), wxID_ANY, _L("打印确认"), wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.Bind(wxEVT_CHAR_HOOK, [&dialog](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    auto* surface = new RoundedPanel(&dialog);
    surface->SetMinSize(dialog.FromDIP(wxSize(330, 1)));
    auto* details = new wxScrolledWindow(surface, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    details->SetMinSize(wxSize(1, 1));
    details->SetScrollRate(0, dialog.FromDIP(16));
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto add_text = [&](const wxString& value, const char* role = "") {
        auto* label = new Label(details, value, LB_AUTO_WRAP);
        label->SetMinSize(wxSize(1, -1));
        label->SetName(role);
        column->Add(label, 0, wxEXPAND | wxALL, dialog.FromDIP(10));
        return label;
    };
    add_text(_L("打印机"), "input_accent");
    add_text(bundle ? from_u8(bundle->printers.get_selected_preset().name) : _L("未选择打印机预设"));
    add_text(configured ? _L("已配置连接入口，在线状态将在设备确认中核实。") : ready ?
        _L("尚未配置打印机连接。可返回工程设置连接，或先导出 G-code。") :
        _L("尚未配置打印机连接。先返回工程完成正式切片，然后可配置连接或导出 G-code。"), "input_secondary");
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
    surface_column->Add(details, 1, wxEXPAND);
    surface->SetSizer(surface_column);
    details->Bind(wxEVT_SIZE, [details](wxSizeEvent& event) { details->FitInside(); event.Skip(); });
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
    button(_L("返回检查"), false, wxID_CANCEL);
    button(_L("导出 G-code…"), false, wxID_SAVE)->Enable(ready);
    auto* next = button(_L("继续到设备确认…"), true, wxID_OK);
    next->Enable(ready && configured);
    next->EnableTooltipEvenDisabled();
    next->SetToolTip(!ready ? _L("先完成当前工程的正式切片。") : !configured ?
        _L("先在工程页配置打印机连接；本地导出仍可用。") : _L("进入现有设备确认流程，不在此直接开始打印。"));
    root->Add(actions, 0, wxEXPAND | wxALL, dialog.FromDIP(6));
    dialog.SetSizer(root);
    apply(&dialog);
    const auto minimum = dialog.FromDIP(wxSize(680, 400));
    const auto preferred = dialog.FromDIP(wxSize(700, 560));
    const auto available = wxGetTopLevelParent(parent)->GetClientSize() -
                           dialog.FromDIP(wxSize(32, 32));
    dialog.SetMinSize(minimum);
    dialog.SetSize(wxSize(std::max(minimum.x, std::min(preferred.x, available.x)),
                          std::max(minimum.y, std::min(preferred.y, available.y))));
    dialog.Layout();
    dialog.CentreOnParent();
    const auto frame = dialog.ToDIP(dialog.GetSize());
    const auto owner = dialog.ToDIP(wxGetTopLevelParent(parent)->GetClientSize());
    BOOST_LOG_TRIVIAL(info) << "ux_print_confirmation_layout frame_dip=" << frame.x << "x" << frame.y
                           << " owner_client_dip=" << owner.x << "x" << owner.y;
    const int action = dialog.ShowModal();
    if ((action == wxID_SAVE || action == wxID_OK) && !confirmation_current()) {
        wxMessageBox(_L("工程或切片已变化。请完成切片后重新确认。"), _L("打印确认"),
                     wxOK | wxICON_WARNING, wxGetTopLevelParent(parent));
        return;
    }
    if (action == wxID_SAVE) plater.export_gcode(false);
    else if (action == wxID_OK) {
        SimpleEvent print_event(EVT_GLTOOLBAR_PRINT_PLATE);
        plater.GetEventHandler()->ProcessEvent(print_event);
    }
}
}
