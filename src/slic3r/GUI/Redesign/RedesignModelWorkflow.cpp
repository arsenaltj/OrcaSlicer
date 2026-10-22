#include <glad/gl.h>
#include "RedesignShell.hpp"
#include "RedesignMessageDialog.hpp"
#include "RedesignFeatureFlags.hpp"
#include "RedesignModelRoute.hpp"
#include "RedesignTheme.hpp"
#include "AssetsWorkspace.hpp"
#include "AssetsWorkspacePolicy.hpp"
#include "RedesignSlicingControls.hpp"
#include "RedesignSlicingTools.hpp"
#include "PrinterWorkspace.hpp"
#include "OrcaPrinterAdapter.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../AI/ModelGeneration/WorkbenchImportPresentation.hpp"
#include "../AI/SmartSlicing/SmartSlicingFeatureHost.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../BBLTopbar.hpp"
#include "../Plater.hpp"
#include "../GLCanvas3D.hpp"
#include "../BackgroundSlicingProcess.hpp"
#include "../NotificationManager.hpp"
#include "../PresetComboBoxes.hpp"
#include "../Widgets/SpinInput.hpp"
#include "../Widgets/Label.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/activityindicator.h>
#include <wx/colordlg.h>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <sstream>

namespace Slic3r::GUI {
namespace {
using namespace AI::SmartSlicing;

bool fit_slicing_camera(Plater& plater)
{
    auto* plate = plater.get_partplate_list().get_curr_plate();
    if (!plate) return false;
    auto* canvas = plater.get_current_canvas3D();
    if (!canvas || canvas->is_reload_delayed()) return false;
    auto& camera = plater.get_camera();
    const auto size = canvas->get_canvas_size();
    const auto& viewport = camera.get_viewport();
    // The import history panel is removed before fitting. Wait until the
    // canvas has rendered at its resulting size and consumed native zooms.
    if (size.get_width() <= 0 || size.get_height() <= 0 ||
        viewport[2] != int(size.get_width()) || viewport[3] != int(size.get_height()) ||
        camera.requires_zoom_to_bed || camera.requires_zoom_to_volumes ||
        camera.requires_zoom_to_plate > REQUIRES_ZOOM_TO_PLATE_IDLE) {
        canvas->set_as_dirty();
        return false;
    }
    BoundingBoxf3 model_bounds;
    const auto& objects = plater.model().objects;
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto* object = objects[i];
        if (!object) continue;
        for (size_t j = 0; j < object->instances.size(); ++j)
            if (plate->contain_instance(int(i), int(j))) model_bounds.merge(object->instance_bounding_box(j));
    }
    if (!model_bounds.defined) return false;
    BoundingBoxf3 frame_bounds = model_bounds;
    // Wipe towers are GL auxiliary volumes, not objects in the model.
    size_t wipe_towers = 0;
    for (const auto* volume : canvas->get_volumes().volumes) {
        if (volume && volume->is_active && volume->is_wipe_tower &&
            volume->object_idx() == 1000 + plate->get_index()) {
            frame_bounds.merge(volume->transformed_bounding_box());
            ++wipe_towers;
        }
    }
    // Include this plate's working area without its printable-height box.
    BoundingBoxf3 bed = plate->get_bounding_box();
    Vec3d focus_target = model_bounds.center();
    if (bed.defined) {
        bed.min.z() = bed.max.z() = 0.;
        frame_bounds.merge(bed);
        // Match Orca's native plate view: the bed anchors the initial focus,
        // while auxiliary volumes only enlarge the framing bounds.
        focus_target = bed.center();
    }
    camera.select_view("right");
    const double distance = camera.get_distance();
    // Generated people face +X, so frame the current plate from that direction
    // with a shallow downward angle.
    camera.look_at(focus_target + distance * Vec3d(.93969262, 0., .34202014),
        focus_target, Vec3d::UnitZ());
    camera.zoom_to_box(frame_bounds, 1.5);
    // zoom_to_box() recenters on frame_bounds; restore the plate/model focus
    // so the wipe tower cannot pull the model toward the edge of the canvas.
    camera.set_target(focus_target);
    canvas->set_as_dirty();
    BOOST_LOG_TRIVIAL(info) << "[RedesignCamera] plate=" << plate->get_index()
        << " viewport=" << viewport[2] << "x" << viewport[3]
        << " wipe_towers=" << wipe_towers << " margin=1.5 zoom=" << camera.get_zoom();
    return true;
}

WorkbenchButton* command(wxWindow* parent, const wxString& label, bool accent = false)
{
    auto* button = new WorkbenchButton(parent, label);
    button->SetCornerRadius(parent->FromDIP(6));
    button->SetBorderWidth(0);
    button->SetMinSize(parent->FromDIP(wxSize(-1, 36)));
    button->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(55, 55, 58), StateColor::Disabled),
        std::pair<wxColour, int>(accent ? wxColour(255, 210, 78) : wxColour(85, 85, 89), StateColor::Hovered),
        std::pair<wxColour, int>(accent ? RedesignTheme::accent_colour() : wxColour(70, 70, 73), StateColor::Normal)));
    button->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour(126, 126, 130), StateColor::Disabled),
        std::pair<wxColour, int>(accent ? wxColour(22, 22, 25) : RedesignTheme::primary_text_colour(), StateColor::Normal)));
    RedesignTheme::style_text(button, RedesignTheme::primary_text_colour(), 10);
    return button;
}

bool set_control_label(wxWindow* control, const wxString& text)
{
    if (control->GetLabel() == text) return false;
    control->SetLabel(text);
    return true;
}

void set_control_tooltip(wxWindow* control, const wxString& text)
{
    if (control->GetToolTipText() != text)
        control->SetToolTip(text);
}

bool set_wrapped_label(wxStaticText* label, const wxString& text, int width)
{
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    wxString wrapped;
    const wxSize extent = Label::split_lines(dc, width, text, wrapped);
    const bool changed = set_control_label(label, wrapped);
    // MSW can retain the previous control height after wrapping a longer label.
    const wxSize minimum(-1, extent.y + label->FromDIP(2));
    if (label->GetMinSize() != minimum) { label->SetMinSize(minimum); return true; }
    return changed;
}

wxString goal_status(GoalResultStatus status)
{
    switch (status) {
    case GoalResultStatus::Ready: return _L("已就绪");
    case GoalResultStatus::Analyzing: return _L("分析中");
    case GoalResultStatus::Stale: return _L("已失效");
    case GoalResultStatus::Applied: return _L("已应用");
    case GoalResultStatus::Failed: return _L("失败");
    default: return _L("不可用");
    }
}

wxString capability_reason(MachineCapabilityReason reason)
{
    switch (reason) {
    case MachineCapabilityReason::ProfileNotAllowlisted: return _L("机型尚未支持");
    case MachineCapabilityReason::ProfileFingerprintMismatch: return _L("机型预设身份未验证");
    case MachineCapabilityReason::UnsupportedNozzleCount: return _L("喷嘴数量未支持");
    case MachineCapabilityReason::SixNozzlePendingValidation: return _L("六喷嘴待验证");
    case MachineCapabilityReason::UnsupportedNozzleDiameter: return _L("喷嘴直径未支持");
    case MachineCapabilityReason::InvalidPhysicalNumbering: return _L("喷嘴编号无效");
    case MachineCapabilityReason::MissingIndependentAddressing: return _L("喷嘴寻址待验证");
    case MachineCapabilityReason::MissingNozzleOffset: return _L("缺少喷嘴偏移标定");
    case MachineCapabilityReason::MissingReachableArea: return _L("缺少喷嘴可达范围");
    case MachineCapabilityReason::MissingCollisionClearance: return _L("缺少碰撞间距标定");
    case MachineCapabilityReason::MissingToolChangeGcode: return _L("换喷嘴指令待验证");
    case MachineCapabilityReason::MissingPreheatBehavior:
    case MachineCapabilityReason::MissingStandbyBehavior: return _L("喷嘴温控待验证");
    case MachineCapabilityReason::MissingRetractionBehavior: return _L("回抽行为待验证");
    case MachineCapabilityReason::MissingToolChangeTimeModel: return _L("换喷嘴时间待验证");
    case MachineCapabilityReason::MissingPrimeOrWipeCapability:
    case MachineCapabilityReason::MissingFlushMatrix:
    case MachineCapabilityReason::MissingWipeTowerSpaceConstraints: return _L("擦料与预充待验证");
    case MachineCapabilityReason::SpecializedValidationIncomplete: return _L("机型专项验证未完成");
    }
    return _L("机型能力待验证");
}

wxString capability_reason(MaterialCompatibilityReason reason)
{
    switch (reason) {
    case MaterialCompatibilityReason::ProfileNotAllowlisted: return _L("耗材预设尚未支持");
    case MaterialCompatibilityReason::ProfileFingerprintMismatch: return _L("耗材预设身份未验证");
    case MaterialCompatibilityReason::RestrictedVariant: return _L("该耗材变体尚未支持");
    case MaterialCompatibilityReason::NoMaterialInUse: return _L("未选择实际使用的耗材");
    case MaterialCompatibilityReason::MixedMaterialFamilies: return _L("不同耗材类型的组合未支持");
    }
    return _L("耗材兼容性待验证");
}

template<class Reason>
wxString capability_reasons(const std::vector<Reason>& reasons)
{
    if (reasons.empty()) return _L("缺少验证证据");
    wxString text;
    std::vector<wxString> descriptions;
    for (const auto reason : reasons) {
        auto description = capability_reason(reason);
        if (std::find(descriptions.begin(), descriptions.end(), description) != descriptions.end()) continue;
        if (!text.empty()) text += _L("、");
        text += description;
        descriptions.push_back(std::move(description));
    }
    return text;
}

wxString parameter_value(const ConfigPatchEntry& entry)
{
    const auto* definition = print_config_def.get(entry.key);
    if (const auto* enabled = std::get_if<bool>(&entry.new_value))
        return *enabled ? _L("启用") : _L("关闭");
    std::ostringstream value;
    std::visit([&](const auto& item) { value << item; }, entry.new_value);
    const auto serialized = value.str();
    if (definition) {
        auto found = std::find(definition->enum_values.begin(), definition->enum_values.end(), serialized);
        if (found != definition->enum_values.end()) {
            const auto index = std::distance(definition->enum_values.begin(), found);
            if (size_t(index) < definition->enum_labels.size()) return _(definition->enum_labels[index]);
        }
    }
    return wxString::FromUTF8(serialized) + (definition && !definition->sidetext.empty()
        ? " " + _(definition->sidetext) : wxString());
}

struct SliceDetailRow
{
    wxString label;
    wxString value;
    wxString section;
};

wxString format_estimated_time(double seconds);

std::vector<SliceDetailRow> candidate_detail_rows(
    const std::vector<ConfigPatchEntry>& parameters, const std::optional<TrialMetrics>& metrics)
{
    std::vector<SliceDetailRow> rows;
    rows.reserve(parameters.size() + 3);
    if (metrics) {
        if (metrics->estimated_time_seconds.known())
            rows.push_back({_L("预计打印时间"), format_estimated_time(*metrics->estimated_time_seconds.value), _L("原生试切结果")});
        if (metrics->total_material_volume_mm3.known())
            rows.push_back({_L("耗材体积"), wxString::Format(_L("%.1f cm³"), *metrics->total_material_volume_mm3.value / 1000.), _L("原生试切结果")});
    }
    for (const auto& entry : parameters) {
        const auto* definition = print_config_def.get(entry.key);
        rows.push_back({
            definition && !definition->label.empty() ? _(definition->label) : wxString::FromUTF8(entry.key),
            parameter_value(entry),
            definition ? _(definition->category) : wxString()});
    }
    return rows;
}

wxString format_estimated_time(double seconds)
{
    const auto minutes = static_cast<long long>(std::llround(std::max(0.0, seconds) / 60.0));
    if (minutes >= 60)
        return wxString::Format(_L("%lld 小时 %lld 分钟"), minutes / 60, minutes % 60);
    return wxString::Format(_L("%lld 分钟"), minutes);
}

std::vector<SliceDetailRow> exact_estimate_detail_rows(
    const SliceRecommendation& recommendation, const ExactEstimateView& estimate)
{
    const auto support = recommendation.enable_support ? _L("树状") : _L("关闭");
    const auto adaptive = recommendation.adaptive_layer_height ? _L("启用") : _L("关闭");
    const auto wall_generator = recommendation.arachne_wall_generator ? _L("Arachne") : _L("经典");
    std::vector<SliceDetailRow> rows;
    if (estimate.status == ExactEstimateStatus::Ready) {
        rows.push_back({_L("预计打印时间"), estimate.estimated_time_seconds ?
            format_estimated_time(*estimate.estimated_time_seconds) : _L("暂无"), _L("原生模拟结果")});
        rows.push_back({_L("预计耗材"), estimate.material_grams ?
            wxString::Format(_L("%.2f g"), *estimate.material_grams) : _L("暂无"), _L("原生模拟结果")});
    } else {
        rows.push_back({_L("预计打印时间"), estimate.status == ExactEstimateStatus::Failed ?
            _L("暂无准确时间") : _L("精确计算中…"), _L("原生模拟结果")});
        if (estimate.status == ExactEstimateStatus::Failed)
            rows.push_back({_L("AI快速参考"),
                wxString::Format(_L("约 %.0f 分钟（可能存在较大偏差）"), recommendation.estimated_time_seconds / 60.0),
                _L("辅助参考")});
        rows.push_back({_L("预计耗材"), estimate.status == ExactEstimateStatus::Failed ?
            wxString::Format(_L("%.1f g（AI估算）"), recommendation.estimated_material_grams) :
            _L("精确计算中…"), _L("辅助参考")});
    }
    rows.push_back({_L("表面质量"), wxString::Format(_L("%s (%.0f/100)"),
                                         wxString::FromUTF8(recommendation.surface_quality_rating),
                                         recommendation.surface_quality_score), _L("预估指标")});
    rows.push_back({_L("悬垂风险"), wxString::Format(_L("%.0f/100"), recommendation.overhang_risk_score * 100.0), _L("预估指标")});
    rows.push_back({_L("层高"), wxString::Format(_L("%.2f mm"), recommendation.layer_height_mm), _L("推荐参数")});
    rows.push_back({_L("墙数"), wxString::Format("%d", recommendation.wall_loops), _L("推荐参数")});
    rows.push_back({_L("填充率"), wxString::Format(_L("%.0f%% · %s"), recommendation.sparse_infill_density_percent,
                                      wxString::FromUTF8(recommendation.sparse_infill_pattern)), _L("推荐参数")});
    rows.push_back({_L("外壁速度"), wxString::Format(_L("%.0f mm/s"), recommendation.outer_wall_speed_mm_s), _L("推荐参数")});
    rows.push_back({_L("支撑"), support, _L("推荐参数")});
    rows.push_back({_L("支撑角度"), wxString::Format(_L("%.0f°"), recommendation.support_threshold_angle_deg), _L("推荐参数")});
    rows.push_back({_L("自适应层高"), adaptive, _L("推荐参数")});
    rows.push_back({_L("墙生成器"), wall_generator, _L("推荐参数")});
    return rows;
}

std::vector<SliceDetailRow> official_detail_rows(PartPlate* plate)
{
    std::vector<SliceDetailRow> rows;
    if (plate == nullptr || !plate->is_slice_result_valid() || plate->get_slice_result() == nullptr)
        return rows;
    const auto& statistics = plate->get_slice_result()->print_statistics;
    const double seconds = statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    double volume = 0.0;
    for (const auto& [_, value] : statistics.total_volumes_per_extruder)
        if (std::isfinite(value) && value > 0.0) volume += value;
    double density = 1.24;
    if (wxGetApp().preset_bundle != nullptr) {
        DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
        config.apply(*plate->config(), true);
        if (const auto* densities = config.option<ConfigOptionFloats>("filament_density");
            densities != nullptr && !densities->values.empty() && std::isfinite(densities->values.front()) &&
            densities->values.front() > 0.0)
            density = densities->values.front();
    }
    rows.push_back({_L("打印时间"), format_estimated_time(seconds), _L("正式切片结果")});
    rows.push_back({_L("实际耗材"), wxString::Format(_L("%.2f g"), volume * density / 1000.0), _L("正式切片结果")});
    return rows;
}
}

bool RedesignShell::owns_model_workflow() const
{
    return m_workbench_page != nullptr && m_plater != nullptr;
}

bool RedesignShell::native_workspace_visible() const
{
    return owns_model_workflow() && m_active_page == Page::Model &&
        (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview);
}

void RedesignShell::start_slicing_from_workspace()
{
    if (m_import_in_progress || !native_workspace_visible()) return;
    if (m_native_slicing) {
        if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this))) frame->slice_native_workspace();
    } else start_workbench_slice();
}

bool RedesignShell::slicing_mode_switch_allowed() const
{
    return workbench_mode_switch_allowed(m_import_in_progress, m_plater && m_plater->is_background_process_slicing(),
        m_slicing_state.official.phase == OfficialSlicePhase::Slicing, m_slice_tools && m_slice_tools->busy(),
        m_slicing_state.analyzing);
}

void RedesignShell::refresh_native_theme()
{
    if (!native_slicing_visible()) return;
    std::vector<wxWindow*> swatches;
    for (auto* combo : m_plater->sidebar().combos_filament())
        if (combo && combo->clr_picker) swatches.push_back(combo->clr_picker);
    // These controls already have the workbench's selected/disabled styling.
    for (auto* navigation : {m_native_return_ai, m_native_return_overview, m_native_prepare, m_native_preview})
        if (navigation) swatches.push_back(navigation);
    m_native_theme.apply(&m_plater->sidebar(), swatches);
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
        if (frame->topbar()) m_native_theme.apply(frame->topbar()->NativeWorkspaceHost(), swatches);
}

void RedesignShell::build_model_workflow()
{
    if (!RedesignFeatureFlags::model_workflow_review_enabled() || !m_model_generation_host || !m_plater) return;
    wxWeakRef<RedesignShell> weak(this);
    auto* history = command(m_model_page, _L("打开历史模型"));
    history->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_model_workbench(); });
    m_model_stage_title->GetContainingSizer()->Add(history, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(24));
    m_workbench_page = new wxPanel(m_content_host);
    m_workbench_page->SetBackgroundColour(RedesignTheme::background_colour());
    m_workbench_page->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_content_host->GetSizer()->Add(m_workbench_page, 1, wxEXPAND);
    m_workbench_page->Hide();
    m_model_generation_host->mount_workbench(m_workbench_page);

    m_slicing_page = new wxPanel(m_content_host);
    m_slicing_page->SetBackgroundColour(RedesignTheme::background_colour());
    auto* page = new wxBoxSizer(wxVERTICAL);
    m_slicing_page->SetSizer(page);
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this))) {
        if (auto* topbar = frame->topbar()) {
            auto* host = topbar->NativeWorkspaceHost();
            auto* navigation = host->GetSizer();
            auto add_navigation = [&](wxWindow*& field, const wxString& label, auto action) {
                field = command(host, label);
                field->SetMinSize(FromDIP(wxSize(-1,28)));
                field->Bind(wxEVT_BUTTON, [weak, action](wxCommandEvent&) { if (weak) action(*weak); });
                navigation->Add(field, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            };
            add_navigation(m_native_return_ai, _L("← AI 智能切片"), [](auto& shell) { shell.select_slicing_tab(false); });
            add_navigation(m_native_return_overview, _L("返回模型总览"), [](auto& shell) {
                shell.return_model_overview();
            });
            add_navigation(m_native_prepare, _L("Prepare"), [](auto& shell) { shell.show_model_view(ModelView::Slicing); });
            add_navigation(m_native_preview, _L("Preview"), [](auto& shell) { shell.show_model_view(ModelView::Preview); });
            frame->mount_native_workspace_tools(host);
        }
    }
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    page->Add(row, 1, wxEXPAND | wxTOP, FromDIP(2));
    auto* settings = m_slicing_settings = new WorkbenchPanel(m_slicing_page, true);
    settings->SetBackgroundColour(wxColour(34,34,38));
    static_cast<WorkbenchPanel*>(settings)->set_corner_radius(12);
    settings->SetMinSize(FromDIP(wxSize(372, -1)));
    auto* root = new wxBoxSizer(wxVERTICAL);
    settings->SetSizer(root);
    row->Add(settings, 0, wxEXPAND | wxRIGHT, FromDIP(8));
    m_slice_return_overview = command(settings, _L("← 返回模型总览"));
    m_slice_return_overview->SetName("slicing-return-model-overview");
    m_slice_return_overview->SetMinSize(FromDIP(wxSize(-1,40)));
    m_slice_return_overview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { return_model_overview(); });
    root->Add(m_slice_return_overview, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    m_slice_save = command(settings, _L("保存工程（3MF）"));
    m_slice_save->SetToolTip(_L("保存模型、耗材颜色、摆放和打印参数；G-code 需切片后单独导出。"));
    root->AddSpacer(FromDIP(8));
    root->Add(m_slice_save, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_slice_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_print_project(); });
    auto* scroll = new WorkbenchScrolledWindow(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(RedesignTheme::panel_colour());
    scroll->SetScrollRate(0, FromDIP(12));
    auto* controls = new wxBoxSizer(wxVERTICAL);
    scroll->SetSizer(controls);
    auto* inset = new wxBoxSizer(wxHORIZONTAL);
    inset->AddSpacer(FromDIP(24)); inset->Add(scroll,1,wxEXPAND); inset->AddSpacer(FromDIP(7));
    root->Add(inset, 1, wxEXPAND | wxTOP, FromDIP(12));

    auto heading = [&](wxWindow* parent, wxSizer* target, const wxString& title) {
        auto* line = new wxBoxSizer(wxHORIZONTAL);
        auto* label = new wxStaticText(parent, wxID_ANY, title);
        RedesignTheme::style_text(label, RedesignTheme::primary_text_colour(), 12);
        line->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        if (title == _L("检查修复") || title == _L("3D 美颜")) {
            auto* info = slicing_link(parent, wxEmptyString);
            info->SetPaddingSize(FromDIP(wxSize(0,0)));
            info->set_scaled_icon("workbench_info",16);
            info->SetMinSize(FromDIP(wxSize(24,24)));
            info->SetName(title + _L("信息"));
            line->Add(info, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
            info->Bind(wxEVT_BUTTON, [this, title](wxCommandEvent&) {
                wxMessageBox(title == _L("检查修复") ? wxString::FromUTF8(m_slice_tools->check().summary.empty()
                    ? "检查当前打印板模型的开放边、非流形边、退化面、重复面和方向。" : m_slice_tools->check().summary)
                    : _L("进入现有 3D 美颜工作台；保存并重新导入后更新当前工程。"), title, wxOK, this);
            });
        }
        target->Add(line, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    };
    heading(scroll, controls, _L("检查修复"));
    auto* check_row = new WorkbenchPanel(scroll, true);
    check_row->set_corner_radius(12);
    check_row->SetBackgroundColour(slicing_control_colour());
    auto* check_sizer = new wxBoxSizer(wxHORIZONTAL); check_row->SetSizer(check_sizer);
    m_slice_check = slicing_link(check_row, _L("一键检查"));
    static_cast<WorkbenchButton*>(m_slice_check)->SetCenter(false);
    static_cast<WorkbenchButton*>(m_slice_check)->SetPaddingSize(FromDIP(wxSize(16,0)));
    static_cast<WorkbenchButton*>(m_slice_check)->SetLeftPadding(FromDIP(16));
    m_slice_check->SetMinSize(FromDIP(wxSize(-1,56)));
    RedesignTheme::style_text(m_slice_check,RedesignTheme::primary_text_colour(),12);
    check_sizer->Add(m_slice_check, 1, wxEXPAND);
    m_slice_check_status = new SlicingCheckBadge(check_row);
    m_slice_check_status->SetLabel(_L("未检查"));
    RedesignTheme::style_text(m_slice_check_status, RedesignTheme::secondary_text_colour(), 9);
    auto badge_font=m_slice_check_status->GetFont(); badge_font.SetFractionalPointSize(10.5);
    m_slice_check_status->SetFont(badge_font);
    check_sizer->Add(m_slice_check_status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    m_slice_check->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_slice_tools->request_check(); });
    controls->Add(check_row, 0, wxEXPAND);
    // Keep the controls alive for the existing palette and beauty workflows,
    // but omit their container from this page's layout.
    auto* hidden_sections = new wxPanel(scroll);
    auto* hidden_controls = new wxBoxSizer(wxVERTICAL);
    hidden_sections->SetSizer(hidden_controls);
    hidden_sections->Hide();
    heading(hidden_sections, hidden_controls, _L("多色模型"));
    m_slice_color_mode = new WorkbenchPaletteChoice(hidden_sections);
    m_slice_color_mode->set_minimum_height(56);
    m_slice_color_mode->set_corner_radius(12);
    m_slice_color_mode->set_text_colour(wxColour(222,222,223));
    m_slice_color_mode->SetBackgroundColour(slicing_control_colour());
    m_slice_color_mode->Append(_L("通用模式")); m_slice_color_mode->Append(_L("人像模式"));
    m_slice_color_mode->SetSelection(0); m_slice_color_mode->SetName("slicing-color-mode");
    RedesignTheme::style_text(m_slice_color_mode, RedesignTheme::primary_text_colour(), 12);
    hidden_controls->Add(m_slice_color_mode, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* color_panel = new WorkbenchPanel(hidden_sections, true);
    color_panel->set_corner_radius(12);
    color_panel->SetBackgroundColour(slicing_control_colour());
    auto* color_sizer = new wxBoxSizer(wxVERTICAL); color_panel->SetSizer(color_sizer);
    m_slice_color_source = new WorkbenchPaletteChoice(color_panel);
    m_slice_color_source->set_minimum_height(56);
    m_slice_color_source->set_corner_radius(12);
    m_slice_color_source->set_text_colour(wxColour(222,222,223));
    m_slice_color_source->SetBackgroundColour(slicing_control_colour());
    m_slice_color_source->Append(_L("AI 建议")); m_slice_color_source->Append(_L("当前工程耗材"));
    m_slice_color_source->Append(_L("手动试色"));
    for (const auto& pack : load_filament_color_packs()) m_slice_color_source->Append(wxString::FromUTF8(pack.name));
    m_slice_color_source->SetSelection(0); m_slice_color_source->SetName("slicing-color-source");
    RedesignTheme::style_text(m_slice_color_source, RedesignTheme::primary_text_colour(), 12);
    color_sizer->Add(m_slice_color_source, 0, wxEXPAND);
    auto* separator = new wxPanel(color_panel); separator->SetBackgroundColour(wxColour(47,47,51));
    separator->SetMinSize(FromDIP(wxSize(-1,1)));
    color_sizer->Add(separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_slice_color_count_label = new wxStaticText(color_panel, wxID_ANY, _L("6色"), wxDefaultPosition,
        wxDefaultSize, wxALIGN_CENTER);
    RedesignTheme::style_text(m_slice_color_count_label, RedesignTheme::primary_text_colour(), 10);
    color_sizer->Add(m_slice_color_count_label, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    m_slice_color_count = new WorkbenchSlider(color_panel, wxID_ANY, 6, 1, 32,
        RedesignTheme::accent_colour(), "workbench_slider_thumb");
    m_slice_color_count->SetName("slicing-color-count");
    color_sizer->Add(m_slice_color_count, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    hidden_controls->Add(color_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    m_slice_color_mode->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        m_slice_color_count->SetRange(1, m_slice_color_mode->GetSelection() == 1 ? 6 : 32);
        change_slicing_palette();
    });
    m_slice_color_source->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { change_slicing_palette(); });
    m_slice_color_count->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        m_slice_color_count_label->SetLabel(wxString::Format(_L("%d色"),m_slice_color_count->GetValue()));
    });
    auto commit_count = [this](wxScrollEvent&) {
        if (m_slice_color_count->GetValue() != m_slice_committed_count) change_slicing_palette();
    };
    m_slice_color_count->Bind(wxEVT_SCROLL_THUMBRELEASE, commit_count);
    m_slice_color_count->Bind(wxEVT_SCROLL_CHANGED, commit_count);
    m_slice_palette_host = new wxPanel(hidden_sections); m_slice_palette_host->SetBackgroundColour(scroll->GetBackgroundColour());
    m_slice_palette = new wxBoxSizer(wxVERTICAL);
    m_slice_palette_host->SetSizer(m_slice_palette);
    hidden_controls->Add(m_slice_palette_host, 0, wxEXPAND);
    m_slice_palette_all = slicing_link(hidden_sections, _L("查看全部  ›"));
    hidden_controls->Add(m_slice_palette_all, 0, wxALIGN_RIGHT | wxTOP, FromDIP(6));
    m_slice_palette_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        show_slicing_palette_details();
    });
    heading(hidden_sections, hidden_controls, _L("3D 美颜"));
    auto* beauty = command(hidden_sections, _L("3D 美颜工作台"));
    beauty->SetBackgroundColor(StateColor(slicing_control_colour()));
    beauty->SetCornerRadius(FromDIP(12));
    beauty->set_navigation_assets({},"workbench_native_arrow");
    beauty->SetMinSize(FromDIP(wxSize(-1,56)));
    RedesignTheme::style_text(beauty,RedesignTheme::primary_text_colour(),12);
    hidden_controls->Add(beauty, 0, wxEXPAND);
    beauty->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (slicing_mode_switch_allowed() && open_model_workbench()) m_model_generation_host->request_enter_beauty();
    });
    heading(scroll, controls, _L("切片"));
    m_ai_slicing_controls = new WorkbenchPanel(scroll, true);
    static_cast<WorkbenchPanel*>(m_ai_slicing_controls)->set_corner_radius(12);
    m_ai_slicing_controls->SetBackgroundColour(slicing_control_colour());
    auto* ai = new wxBoxSizer(wxVERTICAL);
    m_ai_slicing_controls->SetSizer(ai);
    auto* tabs = new wxBoxSizer(wxHORIZONTAL);
    m_ai_slicing_tab = slicing_link(m_ai_slicing_controls, _L("AI 智能切片"));
    m_native_slicing_tab = slicing_link(m_ai_slicing_controls, _L("orca原生"));
    RedesignTheme::style_medium_text(m_ai_slicing_tab,RedesignTheme::primary_text_colour(),12);
    RedesignTheme::style_medium_text(m_native_slicing_tab,RedesignTheme::secondary_text_colour(),12);
    m_ai_slicing_tab->SetMinSize(FromDIP(wxSize(-1,40)));
    m_native_slicing_tab->SetMinSize(FromDIP(wxSize(-1,40)));
    tabs->Add(m_ai_slicing_tab, 1, wxEXPAND | wxRIGHT, FromDIP(4));
    tabs->Add(m_native_slicing_tab, 1, wxEXPAND);
    ai->Add(tabs, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_ai_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(false); });
    m_native_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(true); });

    auto* goals = new wxGridSizer(2, FromDIP(12), FromDIP(12));
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    for (size_t i = 0; i < titles.size(); ++i) {
        auto* button = new SlicingGoalCard(m_ai_slicing_controls, titles[i]);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            if (m_slicing_host) m_slicing_host->select_goal(RECOMMENDATION_GOALS[i]);
        });
        m_slice_goals[i] = button;
        goals->Add(button, 0, wxEXPAND);
    }
    ai->Add(goals, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_slice_parameter_panel = new wxPanel(m_ai_slicing_controls);
    m_slice_parameter_panel->SetBackgroundColour(m_ai_slicing_controls->GetBackgroundColour());
    auto* parameters = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12)); parameters->AddGrowableCol(0);
    m_slice_parameter_panel->SetSizer(parameters);
    ai->Add(m_slice_parameter_panel,0,wxEXPAND | wxALL,FromDIP(16));
    m_slice_parameter_panel->Hide();
    m_slice_analyze = slicing_link(m_ai_slicing_controls, _L("重新分析"));
    ai->Add(m_slice_analyze, 0, wxEXPAND | wxALL, FromDIP(8));
    m_slice_analyze->Hide();
    m_slice_analyze->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_slicing_host) { m_slicing_host->analyze_workbench(); apply_slicing_state(m_slicing_host->workbench_snapshot()); }
    });
    m_slice_status = new wxStaticText(m_ai_slicing_controls, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_slice_status, RedesignTheme::secondary_text_colour(), 9);
    ai->Add(m_slice_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_slice_status->Hide();
    m_slice_status->SetCursor(wxCURSOR_HAND);
    m_slice_status->Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&) {
        wxMessageBox(m_slice_status->GetToolTipText(),_L("切片状态"),wxOK,this);
    });
    m_slice_cancel = slicing_link(m_ai_slicing_controls, _L("取消分析"));
    ai->Add(m_slice_cancel, 0, wxEXPAND); m_slice_cancel->Hide();
    m_slice_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (workbench_slice_running(m_plater && m_plater->is_background_process_slicing(),
                                    m_slicing_state.official.phase))
            m_plater->cancel_current_slice();
        else if (m_slicing_host) m_slicing_host->cancel_workbench_analysis();
    });
    m_slice_keep_mesh = command(m_ai_slicing_controls, _L("保留当前网格并分析"));
    ai->Add(m_slice_keep_mesh, 0, wxEXPAND | wxALL, FromDIP(12)); m_slice_keep_mesh->Hide();
    m_slice_keep_mesh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_slicing_host) return;
        const auto reviewed = m_slicing_host->workbench_snapshot();
        if (!reviewed.can_keep_current_mesh || !reviewed.preflight) return;
        if (show_redesign_confirmation(this, _L("网格存在开放边。保留当前网格继续分析可能产生缺面或悬空路径，请在切片预览中检查结果。"),
                _L("保留当前网格"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) return;
        m_slicing_host->keep_current_mesh_and_analyze(reviewed.preflight->revision);
    });
    m_slice_start = command(m_ai_slicing_controls, _L("开始切片"), true);
    static_cast<WorkbenchButton*>(m_slice_start)->SetCornerRadius(FromDIP(12));
    static_cast<WorkbenchButton*>(m_slice_start)->SetBackgroundColor(StateColor(
        std::pair<wxColour,int>(wxColour(68,68,69),StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(82,82,84),StateColor::Hovered),
        std::pair<wxColour,int>(wxColour(68,68,69),StateColor::Normal)));
    static_cast<WorkbenchButton*>(m_slice_start)->SetTextColor(StateColor(
        std::pair<wxColour,int>(wxColour(126,126,130),StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(229,229,230),StateColor::Normal)));
    ai->Add(m_slice_start, 0, wxEXPAND | wxALL, FromDIP(16));
    controls->Add(m_ai_slicing_controls,0,wxEXPAND);
    m_slice_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_workbench_slice(); });
    m_slice_export = command(settings, _L("导出 G-code"), true);
    m_slice_export->Hide();
    m_slice_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto* plate = m_plater->get_partplate_list().get_curr_plate();
        if (plate && OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) m_plater->export_gcode(false);
    });
    m_slice_print = command(settings, _L("去打印"), true);
    static_cast<WorkbenchButton*>(m_slice_print)->SetCornerRadius(FromDIP(12));
    static_cast<WorkbenchButton*>(m_slice_print)->SetBackgroundColor(StateColor(
        std::pair<wxColour,int>(wxColour(100,82,38),StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(255,210,78),StateColor::Hovered),
        std::pair<wxColour,int>(RedesignTheme::accent_colour(),StateColor::Normal)));
    m_slice_print->SetMinSize(FromDIP(wxSize(-1,48)));
    root->Add(m_slice_print, 0, wxEXPAND | wxALL, FromDIP(16));
    m_slice_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_print_preparation(); });
    auto* return_slice = m_return_slice = command(settings, _L("返回切片参数"));
    return_slice->Hide();
    return_slice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_model_view(ModelView::Slicing); });
    m_native_plater_host = new wxPanel(m_slicing_page);
    m_native_plater_host->SetMinSize(wxSize(1, 1));
    m_native_plater_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    row->Add(m_native_plater_host, 1, wxEXPAND);
    m_import_history_host = new wxPanel(m_slicing_page);
    m_import_history_host->SetBackgroundColour(RedesignTheme::background_colour());
    m_import_history_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    row->Add(m_import_history_host, 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT, FromDIP(12));
    m_import_history_host->Hide();
    m_content_host->GetSizer()->Add(m_slicing_page, 1, wxEXPAND);
    m_slicing_page->Hide();
    m_slice_model_stats = new wxStaticText(settings, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_slice_model_stats, RedesignTheme::secondary_text_colour(), 9);
    m_slice_model_stats->Hide();
    m_plater_original_parent = m_plater->GetParent();
    m_saved_sidebar_collapsed = m_plater->is_sidebar_collapsed();
    if (auto* sizer = m_plater->GetContainingSizer()) sizer->Detach(m_plater);
    m_plater->Reparent(m_native_plater_host);
    m_native_plater_host->GetSizer()->Add(m_plater, 1, wxEXPAND);
    m_plater->Hide();
    m_slicing_host = m_plater->smart_slicing_feature_host();
    m_slice_tools = std::make_unique<RedesignSlicingTools>(*m_plater, [this] {
        update_slicing_tools();
        if (m_slicing_host) apply_slicing_state(m_slicing_host->workbench_snapshot());
    });
    auto* canvas = m_plater->get_view3D_canvas3D()->get_wxglcanvas();
    m_slice_stats_overlay = new SlicingCanvasOverlay(canvas);
    m_slice_stats_overlay->SetSize(FromDIP(10),FromDIP(10),FromDIP(210),FromDIP(112));
    m_slice_progress_overlay = new SlicingCanvasOverlay(canvas);
    m_slice_progress_overlay->SetSize(FromDIP(wxSize(250,96)));
    m_slice_stats_overlay->Hide(); m_slice_progress_overlay->Hide();
    m_slice_camera_loading = new wxPanel(canvas);
    m_slice_camera_loading->SetName("slicing-camera-loading");
    m_slice_camera_loading->SetBackgroundColour(RedesignTheme::background_colour());
    m_slice_camera_loading->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_slice_camera_loading->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_slice_camera_loading);
        dc.SetBackground(wxBrush(RedesignTheme::background_colour()));
        dc.Clear();
    });
    auto* camera_loading_sizer = new wxBoxSizer(wxVERTICAL);
    m_slice_camera_loading->SetSizer(camera_loading_sizer);
    camera_loading_sizer->AddStretchSpacer();
    auto* camera_loading_activity = new wxActivityIndicator(m_slice_camera_loading, wxID_ANY,
        wxDefaultPosition, FromDIP(wxSize(32, 32)));
    camera_loading_activity->SetForegroundColour(RedesignTheme::accent_colour());
    camera_loading_activity->SetBackgroundColour(RedesignTheme::background_colour());
    camera_loading_activity->Start();
    camera_loading_sizer->Add(camera_loading_activity, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(16));
    auto* camera_loading_label = new wxStaticText(m_slice_camera_loading, wxID_ANY, _L("正在准备模型视图…"));
    RedesignTheme::style_text(camera_loading_label, RedesignTheme::secondary_text_colour(), 11);
    camera_loading_sizer->Add(camera_loading_label, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(16));
    camera_loading_sizer->AddStretchSpacer();
    m_slice_camera_loading->Hide();
    canvas->Bind(wxEVT_SIZE,[weak](wxSizeEvent& event) {
        event.Skip();
        if (weak) {
            weak->layout_slicing_overlays();
            weak->layout_slicing_camera_loading();
        }
    });
    canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, [weak](SimpleEvent& event) {
        event.Skip();
        if (weak) weak->CallAfter([weak] {
            if (weak && weak->native_workspace_visible()) {
                weak->update_slicing_tools();
                if (weak->m_slicing_host) weak->apply_slicing_state(weak->m_slicing_host->workbench_snapshot());
            }
        });
    });
    m_plater->get_preview_canvas3D()->get_wxglcanvas()->Bind(wxEVT_SIZE,[weak](wxSizeEvent& event) {
        event.Skip(); if (weak) weak->layout_slicing_overlays();
    });
    m_plater->Bind(EVT_SLICING_UPDATE,[weak](SlicingStatusEvent& event) {
        event.Skip();
        if (!weak || !weak->native_workspace_visible() || weak->m_native_slicing ||
            !workbench_slice_running(weak->m_plater && weak->m_plater->is_background_process_slicing(),
                                     weak->m_slicing_state.official.phase)) return;
        // The native Plater listener and this redesign listener receive the
        // same event.  Keep the native notification hidden on the AI page so
        // users see one progress surface while retaining the native cancel
        // callback and all error notifications.
        if (auto* notifications = weak->m_plater->get_notification_manager())
            notifications->set_slicing_progress_hidden();
        weak->m_slice_progress_percent = std::clamp(event.status.percent,0,100);
        weak->m_slice_progress_stage = wxString::FromUTF8(event.status.text);
        weak->m_slice_progress_overlay->update(weak->m_slice_progress_stage,weak->m_slice_progress_percent);
    });

    m_import_loading = new wxPanel(m_content_host);
    m_import_loading->SetBackgroundColour(RedesignTheme::background_colour());
    m_import_loading->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_import_loading->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_import_loading);
        dc.SetBackground(wxBrush(RedesignTheme::background_colour()));
        dc.Clear();
    });
    m_import_loading->SetName("workbench_import_loading");
    auto* loading = new wxBoxSizer(wxVERTICAL);
    m_import_loading->SetSizer(loading);
    loading->AddStretchSpacer();
    auto* activity = new wxActivityIndicator(m_import_loading, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(36, 36)));
    activity->SetForegroundColour(RedesignTheme::accent_colour());
    activity->SetBackgroundColour(RedesignTheme::background_colour());
    activity->Start();
    loading->Add(activity, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(20));
    m_import_stage = new wxStaticText(m_import_loading, wxID_ANY, _L("正在读取与验证模型…"));
    RedesignTheme::style_text(m_import_stage, RedesignTheme::primary_text_colour(), 12);
    loading->Add(m_import_stage, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(16));
    m_import_elapsed = new wxStaticText(m_import_loading, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_import_elapsed, RedesignTheme::secondary_text_colour(), 10);
    loading->Add(m_import_elapsed, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(12));
    m_import_cancel = command(m_import_loading, _L("取消"));
    m_import_cancel->SetMinSize(FromDIP(wxSize(160, 36)));
    loading->Add(m_import_cancel, 0, wxALIGN_CENTER);
    m_import_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_import_session && m_import_session->cancel()) {
            m_import_stage->SetLabel(_L("正在取消导入…"));
            m_import_cancel->Disable();
        }
    });
    loading->AddStretchSpacer();
    m_import_loading->Hide();
    m_content_host->Bind(wxEVT_SIZE, [weak](wxSizeEvent& event) {
        event.Skip();
        if (weak && weak->m_import_loading && weak->m_import_in_progress) {
            weak->CallAfter([weak] { if (weak) weak->layout_import_loading(); });
        }
    });
    m_import_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        m_import_elapsed->SetLabel(wxString::Format(_L("已用时 %.1f 秒"),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_import_started).count()));
        m_import_loading->Layout();
        layout_import_loading();
        check_import_first_frame();
    }, m_import_timer.GetId());
    m_slice_camera_retry_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!native_workspace_visible() || m_model_view != ModelView::Slicing || !m_plater) {
            m_slice_camera_retry_timer.Stop();
            m_slice_camera_loading_pending = false;
            m_slice_camera_waiting_for_frame = false;
            layout_slicing_camera_loading();
            return;
        }
        auto* canvas = m_plater->get_current_canvas3D();
        if (++m_slice_camera_retry_attempts > 30) {
            BOOST_LOG_TRIVIAL(warning) << "[RedesignCamera] initial fit timed out, reveal current view";
            m_slice_camera_fitted = true;
            m_slice_camera_loading_pending = false;
            m_slice_camera_waiting_for_frame = false;
            m_slice_camera_retry_timer.Stop();
            layout_slicing_camera_loading();
            return;
        }
        if (m_slice_camera_waiting_for_frame) {
            if (canvas->rendered_frames() > m_slice_camera_frame_baseline) {
                m_slice_camera_waiting_for_frame = false;
                m_slice_camera_loading_pending = false;
                m_slice_camera_retry_timer.Stop();
                layout_slicing_camera_loading();
            }
            return;
        }
        if (m_slice_camera_fitted) {
            m_slice_camera_loading_pending = false;
            m_slice_camera_retry_timer.Stop();
            layout_slicing_camera_loading();
            return;
        }
        if (!canvas->is_initialized() || canvas->rendered_frames() == 0)
            return;
        if (fit_slicing_camera(*m_plater)) {
            m_slice_camera_fitted = true;
            m_slice_camera_frame_baseline = canvas->rendered_frames();
            m_slice_camera_waiting_for_frame = true;
        }
    }, m_slice_camera_retry_timer.GetId());

    m_plater->get_view3D_canvas3D()->get_wxglcanvas()->Bind(wxEVT_PAINT, [weak](wxPaintEvent& event) {
        event.Skip();
        if (weak && weak->m_import_awaiting_frame)
            weak->CallAfter([weak] { if (weak) weak->check_import_first_frame(); });
    });
    auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this));
    if (auto* topbar = frame ? frame->topbar() : nullptr) {
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_active_page == Page::Model &&
                weak->m_model_view == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                (weak->m_model_view == ModelView::Slicing || weak->m_model_view == ModelView::Preview))) && weak->m_plater->can_undo());
        }, wxID_UNDO);
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_active_page == Page::Model &&
                weak->m_model_view == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                (weak->m_model_view == ModelView::Slicing || weak->m_model_view == ModelView::Preview))) && weak->m_plater->can_redo());
        }, wxID_REDO);
    }
    Bind(wxEVT_CHAR_HOOK, [weak](wxKeyEvent& event) {
        const auto key = event.GetKeyCode();
        if (!weak || weak->m_active_page != Page::Model ||
            (weak->m_model_view != ModelView::Workbench && !weak->native_workspace_visible()) ||
            !event.CmdDown() || event.AltDown() || event.ShiftDown() || (key != 'Z' && key != 'Y') ||
            dynamic_cast<wxTextEntry*>(wxWindow::FindFocus())) {
            event.Skip();
            return;
        }
        if (weak->m_import_in_progress) return;
        // Beauty owns its preview history; native slicing owns project history.
        if (weak->m_model_view == ModelView::Workbench &&
            (weak->m_workbench_state.editing || !weak->m_workbench_state.can_edit_project_colors)) {
            event.Skip();
            return;
        }
        if (key == 'Z' && weak->m_plater->can_undo()) weak->m_plater->undo();
        if (key == 'Y' && weak->m_plater->can_redo()) weak->m_plater->redo();
    });
    m_plater->sidebar().Bind(wxEVT_CHILD_FOCUS, [weak](wxChildFocusEvent& event) {
        event.Skip();
        if (weak && weak->native_workspace_visible() && weak->m_native_slicing)
            weak->CallAfter([weak] { if (weak) weak->refresh_native_theme(); });
    });
    m_model_generation_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_workbench_state(state); });
    Bind(wxEVT_IDLE, [weak](wxIdleEvent& event) {
        event.Skip();
        if (!weak || weak->m_active_page != Page::Model || weak->m_model_view != ModelView::Preview) return;
        if (weak->m_preview_auto_fit.pending() && weak->m_plater) {
            auto* canvas = weak->m_plater->get_preview_canvas3D();
            if (canvas) {
                auto& viewer = canvas->get_gcode_viewer();
                auto& camera = weak->m_plater->get_camera();
                const auto size = canvas->get_canvas_size();
                const auto& viewport = camera.get_viewport();
                const bool viewport_ready = canvas->is_initialized() && !canvas->is_reload_delayed() &&
                    size.get_width() > 0 && size.get_height() > 0 &&
                    viewport[2] == int(size.get_width()) && viewport[3] == int(size.get_height()) &&
                    !camera.requires_zoom_to_bed && !camera.requires_zoom_to_volumes &&
                    camera.requires_zoom_to_plate <= REQUIRES_ZOOM_TO_PLATE_IDLE;
                if (weak->m_preview_auto_fit.consume(viewer.loaded_result_id(), weak->m_plater->is_preview_loaded(),
                                                     viewer.has_data() && viewer.get_paths_bounding_box().defined,
                                                     viewport_ready)) {
                    canvas->zoom_to_gcode();
                    canvas->set_as_dirty();
                    BOOST_LOG_TRIVIAL(info) << "[RedesignPreview] auto-fit result=" << viewer.loaded_result_id()
                        << " viewport=" << viewport[2] << "x" << viewport[3];
                }
            }
        }
        if (weak->m_slicing_state.official.phase != OfficialSlicePhase::Completed) return;
        // The native preview finishes loading after the completion callback.
        // Refresh only when its result changes, without re-running analysis.
        const bool outside = OrcaPrinterAdapter(weak->m_plater).preview_toolpath_outside();
        if (outside != weak->m_preview_toolpath_outside)
            weak->apply_slicing_state(weak->m_slicing_state);
    });
    m_model_generation_host->set_workbench_results_handler([weak] { if (weak) weak->show_model_view(ModelView::Result); });
    m_model_generation_host->set_workbench_return_to_design_handler([weak] {
        if (weak) weak->return_to_image_design();
    });
    m_model_generation_host->set_workbench_import_handler([weak](const auto& request) { if (weak) weak->confirm_workbench_import(request); });
    if (m_slicing_host) m_slicing_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_slicing_state(state); });
}

bool RedesignShell::open_model_workbench()
{
    if (m_import_in_progress || !owns_model_workflow() || !m_model_generation_host->request_open_workbench()) return false;
    navigate_to(Page::Model);
    show_model_view(ModelView::Workbench);
    return true;
}

void RedesignShell::return_model_overview()
{
    if (slicing_mode_switch_allowed() && open_model_workbench())
        m_model_generation_host->request_return_overview();
}

void RedesignShell::show_model_view(ModelView view)
{
    if (!owns_model_workflow() || (m_import_in_progress && !m_import_switching_view)) return;
    if (view == ModelView::Preview) m_preview_auto_fit.request();

    // Route changes can arrive synchronously from both the button handler and
    // the model-generation state listener. Keep an already selected view
    // idempotent so a nested callback cannot repeatedly mutate the wx layout
    // while the original event is still being dispatched.
    if (m_active_page == Page::Model && m_model_view == view) {
        // Applying another SmartSlice plan temporarily returns Plater to the
        // 3D panel so the parameters can be installed.  The workbench route
        // remains Preview during that operation, so entering the same route
        // again must still restore Plater's actual preview panel and reload
        // the current G-code result.
        const bool panel_out_of_sync = m_plater &&
            ((view == ModelView::Preview && !m_plater->is_preview_shown()) ||
             (view == ModelView::Slicing && !m_plater->is_view3D_shown()));
        if (panel_out_of_sync) {
            m_plater->select_view_3D(view == ModelView::Preview ? "Preview" : "3D");
            m_plater->get_current_canvas3D()->use_workbench_appearance();
            m_plater->collapse_sidebar(!m_native_slicing);
        }
        refresh_workflow_layout();
        if (view == ModelView::Result)
            update_model_page(m_model_generation_state);
        return;
    }

    m_model_view = view;
    m_active_tab_id = view == ModelView::Preview ? TAB_ID_PREVIEW :
        view == ModelView::Slicing ? TAB_ID_PREPARE : TAB_ID_GENERATE_3D;
    if (m_slicing_host) m_slicing_host->set_workbench_active(view == ModelView::Slicing || view == ModelView::Preview, !m_native_slicing);
    if (view == ModelView::Slicing || view == ModelView::Preview) {
        m_plater->select_view_3D(view == ModelView::Preview ? "Preview" : "3D");
        m_plater->get_current_canvas3D()->use_workbench_appearance();
        m_plater->collapse_sidebar(!m_native_slicing);
        if (m_native_slicing) {
            if (auto* parameters = wxGetApp().params_panel()) parameters->OnActivate();
            refresh_native_theme();
        }
        if (view == ModelView::Slicing && !m_native_slicing && m_slice_tools->check().status == WorkbenchCheckStatus::NotRun)
            m_slice_tools->request_check();
    }
    refresh_workflow_layout();
    if (view == ModelView::Result) update_model_page(m_model_generation_state);
}

void RedesignShell::refresh_workflow_layout()
{
    if (!owns_model_workflow()) return;
    const bool model = m_active_page == Page::Model;
    const bool slicing = model && (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview);
    m_model_page->Show(model && m_model_view == ModelView::Result);
    m_workbench_page->Show(model && m_model_view == ModelView::Workbench);
    m_slicing_page->Show(slicing);
    m_plater->Show(slicing);
    m_slicing_settings->Show(!m_native_slicing);
    m_plater->get_current_canvas3D()->enable_render(slicing);
    if (slicing) m_plater->get_current_canvas3D()->set_as_dirty();
    m_slice_export->Hide();
    m_slice_print->Show(!m_native_slicing);
    m_slice_start->Show(!m_native_slicing);
    m_return_slice->Hide();
    if (m_slicing_host) m_slicing_host->set_workbench_active(slicing, !m_native_slicing);
    const bool compact = slicing && !m_native_slicing;
    if (compact && !m_plater->is_sidebar_collapsed()) m_plater->collapse_sidebar(true);
    m_plater->get_view3D_canvas3D()->use_compact_slicing_tools(compact);
    m_plater->get_preview_canvas3D()->use_compact_slicing_tools(compact);
    // Keep the native main toolbar and sidebar hidden, while exposing the
    // model-editing Gizmo row and its numeric transform panel in AI slicing.
    m_plater->get_view3D_canvas3D()->set_slicing_gizmos_visible(compact);
    m_plater->get_preview_canvas3D()->set_slicing_gizmos_visible(compact);
    m_plater->get_view3D_canvas3D()->suppress_gcode_window(slicing);
    m_plater->get_preview_canvas3D()->suppress_gcode_window(slicing);
    if (compact && !m_saved_collapse_toolbar) {
        m_saved_collapse_toolbar = m_plater->get_collapse_toolbar().is_enabled();
        m_plater->get_collapse_toolbar().set_enabled(false);
    } else if (!compact && m_saved_collapse_toolbar) {
        m_plater->get_collapse_toolbar().set_enabled(*m_saved_collapse_toolbar); m_saved_collapse_toolbar.reset();
    }
    const bool native = slicing && m_native_slicing;
    if (!native) m_native_theme.restore();
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this))) {
        if (native) frame->show_option(true);
        if (frame->topbar()) {
            frame->topbar()->ShowCompactSlicingTitlebar(compact);
            frame->topbar()->ShowNativeWorkspaceTools(native);
        }
    }
    if (native) refresh_native_theme();
    m_slice_return_overview->Enable(slicing_mode_switch_allowed());
    if (m_native_return_ai) {
        const bool can_switch = slicing_mode_switch_allowed();
        m_native_return_ai->Enable(can_switch);
        m_native_return_overview->Enable(can_switch);
        static_cast<WorkbenchButton*>(m_native_prepare)->SetTextColor(StateColor(m_model_view == ModelView::Slicing
            ? RedesignTheme::accent_colour() : RedesignTheme::primary_text_colour()));
        static_cast<WorkbenchButton*>(m_native_preview)->SetTextColor(StateColor(m_model_view == ModelView::Preview
            ? RedesignTheme::accent_colour() : RedesignTheme::primary_text_colour()));
    }
    layout_slicing_overlays();
    // Plater is reparented through two nested panels. Lay out each host when
    // the slicing page changes visibility so its canvas receives the new size.
    Layout();
    m_content_host->Layout();
    if (slicing) {
        m_slicing_page->Layout();
        m_native_plater_host->Layout();
        m_plater->Layout();
    }
    const bool camera_fit_pending = compact && m_model_view == ModelView::Slicing &&
        !m_slice_camera_fitted && m_plater && !m_plater->model().objects.empty();
    if (camera_fit_pending) {
        if (!m_slice_camera_loading_pending) {
            m_slice_camera_loading_pending = true;
            m_slice_camera_waiting_for_frame = false;
            m_slice_camera_retry_attempts = 0;
        }
        // The import overlay owns the canvas while the model is still being
        // committed. Start the camera retry after it is dismissed.
        if (!m_import_in_progress && !m_slice_camera_retry_timer.IsRunning())
            m_slice_camera_retry_timer.Start(100);
    } else if (!compact || m_model_view != ModelView::Slicing ||
               !m_plater || m_plater->model().objects.empty()) {
        m_slice_camera_loading_pending = false;
        m_slice_camera_waiting_for_frame = false;
        m_slice_camera_retry_timer.Stop();
    }
    if (m_import_in_progress && m_import_loading) {
        layout_import_loading();
        m_import_loading->Raise();
        m_import_loading->Refresh(false);
        m_import_loading->Update();
    }
    layout_slicing_camera_loading();
}

void RedesignShell::apply_workbench_state(const PostGenerationWorkbenchState& state)
{
    m_workbench_state = state;
    refresh_project_save_actions();
    if (m_model_back_to_design_button && m_model_back_to_design_button->IsShown())
        m_model_back_to_design_button->Enable(can_return_to_image_design());
    if (m_image_page_view_only)
        apply_model_generation_state(m_model_generation_state);
    if (!m_pending_workbench_job.empty() && state.asset_id == m_pending_workbench_job && state.actions.can_edit) {
        m_pending_workbench_job.clear();
        open_model_workbench();
    }
}

bool RedesignShell::can_return_to_image_design() const
{
    return model_generation_return_to_design_allowed(
        m_model_generation_state, m_workbench_state.actions,
        m_import_in_progress || m_submit_in_progress || m_model_preview_loading);
}

bool RedesignShell::return_to_image_design()
{
    if (!can_return_to_image_design())
        return false;

    if (m_workbench_state.dirty || !m_workbench_state.candidate_path.empty()) {
        if (show_redesign_confirmation(this,
                _L("当前 3D 工作台存在未保存修改或尚未接受的候选。返回图像设计后，如果开始新的生成任务，这些修改可能无法继续恢复。仍要返回吗？"),
                _L("返回图像设计"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES)
            return false;
    }

    const bool route_locked = m_model_route_locked;
    const bool view_only = m_image_page_view_only;
    const auto route_session = m_model_route_session;
    const auto route_id = m_model_route_id;
    m_model_route_locked = false;
    m_model_route_session = 0;
    m_model_route_id.clear();
    m_image_page_view_only = false;
    if (!navigate_to(Page::Image)) {
        m_model_route_locked = route_locked;
        m_image_page_view_only = view_only;
        m_model_route_session = route_session;
        m_model_route_id = route_id;
        return false;
    }
    // A missing historical style becomes a valid default only when the user
    // explicitly returns to editing. This never restarts or submits a task.
    if (current_generation_input().style != m_model_generation_state.input.style)
        synchronize_generation_input();
    apply_model_generation_state(m_model_generation_state);
    return true;
}

void RedesignShell::confirm_workbench_import(const AI::ModelImportRequest& request)
{
    if (m_import_in_progress || !m_workbench_state.actions.can_import) return;
    m_import_in_progress = true;
    m_import_started = std::chrono::steady_clock::now();
    m_import_session = std::make_shared<WorkbenchImportSession>();
    m_model_generation_host->set_workbench_import_feedback(true);
    layout_import_loading();
    m_import_elapsed->SetLabel(_L("已用时 0.0 秒"));
    m_import_loading->Raise();
    update_import_loading(WorkbenchImportPhase::Reading);
    m_import_loading->Update();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] feedback_ms=" <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_import_started).count();
    m_slicing_settings->Disable();
    m_import_timer.Start(100);
    wxWeakRef<RedesignShell> weak(this);
    const auto session = m_import_session;
    CallAfter([weak, session, request] {
        if (!weak || !session->valid()) return;
        if (session->cancelled()) { weak->finish_import_loading(); return; }
        try {
            const bool started = weak->m_model_generation_host && weak->m_model_generation_host->import_workbench_model_async(request, session,
                [weak, session](auto phase, const auto& message) {
                    if (weak && session->valid()) weak->update_import_loading(phase, message);
                },
                [weak, session](const auto& result) {
                    if (!weak || !session->valid()) return;
                    if (!result.imported()) {
                        weak->finish_import_loading();
                        if (result.outcome != AI::ModelImportOutcome::Cancelled)
                            wxMessageBox(wxString::FromUTF8(result.error.empty() ? "模型导入失败，工作副本已保留。" : result.error),
                                _L("模型导入"), wxOK | wxICON_ERROR, weak.get());
                        return;
                    }
                    weak->update_import_loading(WorkbenchImportPhase::UpdatingView);
                    weak->m_slice_camera_fitted = false;
                    weak->m_slice_camera_loading_pending = false;
                    weak->m_slice_camera_waiting_for_frame = false;
                    auto* history = weak->m_model_generation_host->workbench_import_history();
                    if (history && history->IsShown()) {
                        weak->m_import_history_host->SetMinSize(wxSize(history->GetSize().x, -1));
                        weak->m_model_generation_host->mount_workbench_import_history(weak->m_import_history_host);
                        weak->m_import_history_host->Show();
                    }
                    weak->m_import_switching_view = true;
                    weak->navigate_to(Page::Model);
                    weak->show_model_view(ModelView::Slicing);
                    weak->m_plater->Disable();
                    weak->m_import_switching_view = false;
                    auto* canvas = weak->m_plater->get_current_canvas3D();
                    weak->m_import_frame_baseline = canvas->rendered_frames();
                    weak->m_import_view_started = std::chrono::steady_clock::now();
                    weak->m_import_awaiting_frame = true;
                    weak->layout_import_loading();
                    canvas->set_as_dirty();
                    canvas->get_wxglcanvas()->Refresh(false);
                });
            if (!started) {
                weak->finish_import_loading();
                wxMessageBox(_L("模型或工程状态已变化，或正在执行其他处理任务。请完成当前操作后重新导入。"),
                    _L("模型导入"), wxOK | wxICON_WARNING, weak.get());
            }
        } catch (const std::exception& error) {
            if (!weak) return;
            weak->finish_import_loading();
            wxMessageBox(wxString::FromUTF8(error.what()), _L("模型导入"), wxOK | wxICON_ERROR, weak.get());
        }
    });
}

void RedesignShell::update_import_loading(WorkbenchImportPhase phase, const std::string& message)
{
    if (!m_import_in_progress) return;
    wxString label;
    switch (phase) {
    case WorkbenchImportPhase::Reading: label = _L("正在读取与验证模型…"); break;
    case WorkbenchImportPhase::Colors: label = _L("正在准备配色…"); break;
    case WorkbenchImportPhase::Placement: label = _L("正在校验模型摆放…"); break;
    case WorkbenchImportPhase::Committing: label = _L("正在加入工程…"); break;
    case WorkbenchImportPhase::UpdatingView: label = _L("正在更新模型视口…"); break;
    default: break;
    }
    if (!message.empty()) label = wxString::FromUTF8(message);
    if (!label.empty()) m_import_stage->SetLabel(label);
    m_import_cancel->Enable(m_import_session && m_import_session->can_cancel());
    m_import_loading->Layout();
    m_import_loading->Refresh(false);
    if (phase == WorkbenchImportPhase::Committing) m_import_loading->Update();
}

void RedesignShell::layout_import_loading()
{
    if (!m_import_in_progress || !m_import_loading || !m_model_generation_host) return;
    wxWindow* viewport = m_model_view == ModelView::Slicing ? m_plater->get_current_canvas3D()->get_wxglcanvas()
        : m_model_generation_host->workbench_import_viewport();
    if (!viewport) return;
    // Native GL overlays must be children of the canvas to survive buffer
    // swaps. The workbench preview is suspended while its sibling is shown.
    wxWindow* owner = m_model_view == ModelView::Slicing ? viewport : m_content_host;
    if (m_import_loading->GetParent() != owner) m_import_loading->Reparent(owner);
    wxRect bounds(owner->ScreenToClient(viewport->ClientToScreen(wxPoint(0, 0))), viewport->GetClientSize());
    bounds = workbench_import_feedback_bounds(bounds, owner->GetClientRect());
    if (bounds.IsEmpty()) { m_import_loading->Hide(); return; }
    if (m_import_loading->GetRect() != bounds) {
        m_import_loading->SetSize(bounds); m_import_loading->Layout();
    }
    m_import_loading->Show();
    m_import_loading->Raise();
}

void RedesignShell::finish_import_loading()
{
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] total_ms=" <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_import_started).count();
    m_import_timer.Stop();
    m_import_awaiting_frame = false;
    m_import_in_progress = false;
    m_import_switching_view = false;
    if (m_import_session) m_import_session->advance(WorkbenchImportPhase::Completed);
    m_import_session.reset();
    m_import_loading->Hide();
    if (m_import_loading->GetParent() != m_content_host) m_import_loading->Reparent(m_content_host);
    m_model_generation_host->restore_workbench_import_history();
    m_import_history_host->Hide();
    m_model_generation_host->set_workbench_import_feedback(false);
    m_slicing_settings->Enable();
    m_plater->Enable();
    refresh_workflow_layout();
    if (m_slicing_host) apply_slicing_state(m_slicing_host->workbench_snapshot());
}

void RedesignShell::check_import_first_frame()
{
    if (!m_import_awaiting_frame || !m_plater) return;
    auto* canvas = m_plater->get_current_canvas3D();
    if (canvas->is_initialized() && canvas->rendered_frames() > m_import_frame_baseline) {
        finish_import_loading();
        // Removing import history changes the canvas size. Fit after a render
        // at the final size, including any newly created auxiliary volumes.
        if (!m_slice_camera_fitted && m_model_view == ModelView::Slicing) {
            m_slice_camera_retry_attempts = 0;
            m_slice_camera_retry_timer.Start(100);
            canvas->set_as_dirty();
            canvas->get_wxglcanvas()->Refresh(false);
        }
    } else if (std::chrono::steady_clock::now() - m_import_view_started > std::chrono::seconds(15)) {
        finish_import_loading();
        wxMessageBox(_L("模型已导入工程，但视口尚未完成显示。请重新进入切片页面；无需重复导入。"),
            _L("模型视口"), wxOK | wxICON_WARNING, this);
    }
}

void RedesignShell::open_print_preparation()
{
    if (m_import_in_progress || !m_print_page || !OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) return;
    if (navigate_to(Page::Print)) m_print_page->show_prepare();
}

void RedesignShell::change_slicing_palette()
{
    if (!m_slice_tools || !m_slice_tools->editable() ||
        workbench_slice_running(m_plater && m_plater->is_background_process_slicing(),
                                m_slicing_state.official.phase)) return;
    std::vector<PreviewPalette::Color> colors;
    if (m_slice_color_source->GetSelection() == 2) {
        for (const auto& hex : m_slice_tools->palette()) colors.push_back(RedesignSlicingTools::rgb(hex));
        if (colors.empty()) return;
        // Manual colors are logical targets, subsequently mapped to compatible
        // physical slots. The picker never edits shared spool configuration.
        const auto guard=OrcaWorkspaceAdapter(m_plater,{}).capture_import_guard();
        wxColourData data; data.SetColour(wxColour(wxString::FromUTF8(m_slice_tools->palette().front())));
        wxColourDialog picker(this,&data);
        if (picker.ShowModal() != wxID_OK || !guard || !guard()) return;
        const auto c=picker.GetColourData().GetColour();
        colors.front()={c.Red()/255.f,c.Green()/255.f,c.Blue()/255.f};
    }
    m_slice_committed_count=m_slice_color_count->GetValue();
    m_slice_color_count_label->SetLabel(wxString::Format(_L("%d色"),m_slice_committed_count));
    m_slice_tools->request_colors(m_slice_color_mode->GetSelection()==1,m_slice_color_source->GetSelection(),
        size_t(m_slice_committed_count),std::move(colors));
}

void RedesignShell::show_slicing_palette_details()
{
    if (!m_slice_tools) return;
    const auto current=m_slice_tools->palette();
    wxDialog dialog(this,wxID_ANY,_L("模型配色"),wxDefaultPosition,FromDIP(wxSize(510,430)),wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetBackgroundColour(RedesignTheme::panel_colour());
    auto* root=new wxBoxSizer(wxVERTICAL); dialog.SetSizer(root);
    auto* scroll=new WorkbenchScrolledWindow(&dialog,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxVSCROLL);
    scroll->SetBackgroundColour(dialog.GetBackgroundColour()); scroll->SetScrollRate(0,FromDIP(16));
    auto* grid=new wxGridSizer(3,FromDIP(10),FromDIP(10)); scroll->SetSizer(grid);
    std::vector<PreviewPalette::Color> targets;
    for (const auto& hex:current) targets.push_back(RedesignSlicingTools::rgb(hex));
    for (size_t i=0;i<current.size();++i) {
        auto* swatch=command(scroll,wxString::FromUTF8(current[i]));
        swatch->SetBackgroundColor(StateColor(wxColour(wxString::FromUTF8(current[i]))));
        swatch->Enable(m_slice_tools->editable());
        swatch->Bind(wxEVT_BUTTON,[&dialog,&targets,swatch,i](wxCommandEvent&) {
            wxColourData data;
            const auto& c=targets[i]; data.SetColour(wxColour(int(c[0]*255+.5f),int(c[1]*255+.5f),int(c[2]*255+.5f)));
            wxColourDialog picker(&dialog,&data);
            if (picker.ShowModal()!=wxID_OK) return;
            const auto color=picker.GetColourData().GetColour();
            targets[i]={color.Red()/255.f,color.Green()/255.f,color.Blue()/255.f};
            swatch->SetBackgroundColor(StateColor(color)); swatch->SetLabel(color.GetAsString(wxC2S_HTML_SYNTAX));
        });
        grid->Add(swatch,0,wxEXPAND);
    }
    root->Add(scroll,1,wxEXPAND | wxALL,FromDIP(20));
    auto* note=new wxStaticText(&dialog,wxID_ANY,wxString::FromUTF8(m_slice_tools->message().empty()
        ? "修改模型区域的目标颜色，匹配到当前兼容耗材槽；取消不会更新工程。" : m_slice_tools->message()));
    RedesignTheme::style_text(note,RedesignTheme::secondary_text_colour(),10); note->Wrap(FromDIP(460));
    root->Add(note,0,wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,FromDIP(20));
    root->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL),0,wxEXPAND | wxALL,FromDIP(16));
    if (auto* apply=dialog.FindWindow(wxID_OK)) apply->Enable(m_slice_tools->editable() && !targets.empty());
    dialog.CenterOnParent();
    // An intervening native project edit is rejected by the tools' guard.
    const auto guard=OrcaWorkspaceAdapter(m_plater,{}).capture_import_guard();
    if (dialog.ShowModal()!=wxID_OK || !guard || !guard()) return;
    bool changed=false;
    for (size_t i=0;i<targets.size();++i) changed |= targets[i]!=RedesignSlicingTools::rgb(current[i]);
    if (!changed) return;
    m_slice_color_source->SetSelection(2);
    m_slice_tools->request_colors(m_slice_color_mode->GetSelection()==1,2,size_t(m_slice_color_count->GetValue()),std::move(targets));
}

void RedesignShell::update_slicing_tools()
{
    if (!m_slice_tools) return;
    m_slice_tools->refresh();
    const bool running = workbench_slice_running(m_plater && m_plater->is_background_process_slicing(),
                                                 m_slicing_state.official.phase);
    const bool editable=m_slice_tools->editable() && !running;
    m_slice_color_mode->Enable(editable); m_slice_color_source->Enable(editable); m_slice_color_count->Enable(editable);
    const auto edit_reason=wxString::FromUTF8(m_slice_tools->editability_reason());
    set_control_tooltip(m_slice_color_mode, edit_reason); set_control_tooltip(m_slice_color_source, edit_reason);
    set_control_tooltip(m_slice_color_count, edit_reason);
    set_control_tooltip(m_slice_color_count_label, wxString::FromUTF8(m_slice_tools->message()));
    m_slice_check->Enable(!m_slice_tools->busy() && !running);
    const auto& check=m_slice_tools->check();
    const std::array<wxString,6> labels{_L("未检查"),_L("检查中"),_L("正常"),_L("需注意"),_L("异常"),_L("检查失败")};
    bool badge_changed = set_control_label(m_slice_check_status, labels[size_t(check.status)]);
    const auto foreground = check.status==WorkbenchCheckStatus::Normal ? wxColour(255,255,255)
        : check.status==WorkbenchCheckStatus::Invalid || check.status==WorkbenchCheckStatus::Failed ? wxColour(255,150,146)
        : RedesignTheme::primary_text_colour();
    const auto background = check.status==WorkbenchCheckStatus::Normal ? wxColour(21,194,82)
        : slicing_control_colour();
    const auto minimum = FromDIP(wxSize(check.status==WorkbenchCheckStatus::Failed ? 80 : 60,36));
    if (m_slice_check_status->GetForegroundColour() != foreground) {
        m_slice_check_status->SetForegroundColour(foreground); badge_changed = true;
    }
    if (m_slice_check_status->GetBackgroundColour() != background) {
        m_slice_check_status->SetBackgroundColour(background); badge_changed = true;
    }
    if (m_slice_check_status->GetMinSize() != minimum) {
        m_slice_check_status->SetMinSize(minimum); badge_changed = true;
    }
    if (badge_changed) m_slice_check_status->Refresh(false);
    set_control_tooltip(m_slice_check_status, wxString::FromUTF8(check.summary));
    const auto& palette=m_slice_tools->palette();
    m_slice_palette_all->Show(palette.size()>6 || m_slicing_state.has_selected_goal || !m_slice_tools->message().empty());
    if (!palette.empty() && !m_slice_tools->busy() && !m_slice_color_count->HasCapture()) {
        m_slice_committed_count=int(std::clamp(palette.size(),size_t(1),
            m_slice_color_mode->GetSelection()==1 ? size_t(6) : size_t(32)));
        if (m_slice_color_count->GetValue() != m_slice_committed_count)
            m_slice_color_count->SetValue(m_slice_committed_count);
        set_control_label(m_slice_color_count_label, wxString::Format(_L("%d色"),m_slice_committed_count));
    }
    if (palette==m_slice_displayed_palette) return;
    m_slice_displayed_palette=palette;
    m_slice_palette->Clear(true);
    auto* grid=new wxGridSizer(3,FromDIP(6),FromDIP(6));
    for (size_t i=0;i<std::min(size_t(6),palette.size());++i) {
        const wxColour color(wxString::FromUTF8(palette[i]));
        auto* swatch=command(m_slice_palette_host,wxString::FromUTF8(palette[i]));
        swatch->SetName("slicing-color-"+wxString::Format("%zu",i+1));
        swatch->SetMinSize(FromDIP(wxSize(96,28))); swatch->SetCornerRadius(0);
        swatch->SetBackgroundColor(StateColor(color));
        swatch->SetTextColor(StateColor(color.Red()*.2126+color.Green()*.7152+color.Blue()*.0722>140
            ? wxColour(22,22,25) : wxColour(255,255,255)));
        RedesignTheme::style_text(swatch,RedesignTheme::primary_text_colour(),9);
        swatch->Bind(wxEVT_BUTTON,[this,i](wxCommandEvent&) {
            if (!m_slice_tools->editable()) return;
            const auto current=m_slice_tools->palette(); if (i>=current.size()) return;
            const auto guard=OrcaWorkspaceAdapter(m_plater,{}).capture_import_guard();
            wxColourData data; data.SetColour(wxColour(wxString::FromUTF8(current[i])));
            wxColourDialog picker(this,&data);
            if (picker.ShowModal()!=wxID_OK || !guard || !guard()) return;
            std::vector<PreviewPalette::Color> targets;
            for (const auto& hex:current) targets.push_back(RedesignSlicingTools::rgb(hex));
            const auto c=picker.GetColourData().GetColour(); targets[i]={c.Red()/255.f,c.Green()/255.f,c.Blue()/255.f};
            m_slice_color_source->SetSelection(2);
            m_slice_tools->request_colors(m_slice_color_mode->GetSelection()==1,2,
                size_t(m_slice_color_count->GetValue()),std::move(targets));
        });
        grid->Add(swatch,0,wxEXPAND);
    }
    m_slice_palette->Add(grid,0,wxEXPAND);
    m_slice_palette_host->Layout();
}

void RedesignShell::layout_slicing_overlays()
{
    if (!m_slice_stats_overlay || !m_plater) return;
    const bool visible=native_workspace_visible() && !m_native_slicing;
    auto* canvas=m_plater->get_current_canvas3D()->get_wxglcanvas();
    bool overlay_changed = false;
    if (m_slice_stats_overlay->GetParent()!=canvas) {
        m_slice_stats_overlay->Reparent(canvas); m_slice_progress_overlay->Reparent(canvas);
        overlay_changed = true;
    }
    const auto size=canvas->GetClientSize();
    if (m_slice_stats_overlay->IsShown()) m_slice_stats_overlay->Hide();
    const auto progress_size = canvas->FromDIP(wxSize(320, 46));
    const int margin = canvas->FromDIP(10);
    // Match the native 3D navigator's 128 DIP region at the bottom left.
    const int navigator_height = canvas->FromDIP(128);
    const int max_y = std::max(0, size.y - progress_size.y - margin);
    const int y = std::clamp(size.y - (navigator_height + progress_size.y) / 2,
        std::min(margin, max_y), max_y);
    const wxRect progress_bounds(std::max(FromDIP(10),size.x-FromDIP(440)),
        y, progress_size.x, progress_size.y);
    if (m_slice_progress_overlay->GetRect() != progress_bounds) {
        m_slice_progress_overlay->SetSize(progress_bounds);
        overlay_changed = true;
    }
    const bool running = workbench_slice_running(m_plater->is_background_process_slicing(),
                                                 m_slicing_state.official.phase);
    const bool show_progress = visible && running;
    if (m_slice_progress_overlay->IsShown() != show_progress) {
        m_slice_progress_overlay->Show(show_progress);
        overlay_changed = true;
    }
    if (show_progress && overlay_changed) m_slice_progress_overlay->Raise();
}

void RedesignShell::layout_slicing_camera_loading()
{
    if (!m_slice_camera_loading || !m_plater) return;
    auto* canvas = m_plater->get_view3D_canvas3D()->get_wxglcanvas();
    if (!canvas) return;
    if (m_slice_camera_loading->GetParent() != canvas)
        m_slice_camera_loading->Reparent(canvas);
    const bool visible = m_slice_camera_loading_pending && !m_import_in_progress &&
        native_workspace_visible() && !m_native_slicing && m_model_view == ModelView::Slicing;
    const auto size = canvas->GetClientSize();
    if (size.x <= 0 || size.y <= 0) {
        m_slice_camera_loading->Hide();
        return;
    }
    m_slice_camera_loading->SetSize(canvas->GetClientRect());
    m_slice_camera_loading->Show(visible);
    if (visible) {
        m_slice_camera_loading->Raise();
        m_slice_camera_loading->Refresh(false);
    }
}

void RedesignShell::select_slicing_tab(bool native)
{
    if (!m_slicing_host || !slicing_mode_switch_allowed()) return;
    const bool changed = m_native_slicing != native;
    m_native_slicing = native;
    m_ai_slicing_controls->Show(!native);
    m_plater->collapse_sidebar(!native);
    if (native) {
        if (auto* parameters = wxGetApp().params_panel()) parameters->OnActivate();
        refresh_native_theme();
    }
    m_slicing_host->set_workbench_active(native_workspace_visible(), !native);
    if (!native && !changed && !m_slicing_state.analyzing && m_slicing_state.session.state != RecommendationSessionState::Ready)
        m_slicing_host->analyze_workbench();
    apply_slicing_state(m_slicing_host->workbench_snapshot());
    m_slicing_page->Layout();
    refresh_workflow_layout();
}

void RedesignShell::apply_slicing_state(const SmartSlicingWorkbenchState& state)
{
    const bool background_slicing = m_plater && m_plater->is_background_process_slicing();
    const bool external_toolpath = m_plater &&
        (m_plater->only_gcode_mode() || m_plater->using_exported_file());
    const bool running = workbench_slice_running(background_slicing, state.official.phase);
    // The previous SmartSlice phase is the last UI snapshot.  Use it as the
    // edge detector; the current Plater flag is intentionally not reused here
    // because it would make a newly started background job look old already.
    const bool previously_running = m_slicing_state.official.phase == OfficialSlicePhase::Slicing;
    const bool began = running && !previously_running;
    const bool completed = state.official.phase == OfficialSlicePhase::Completed &&
        !background_slicing && m_slicing_state.official.phase != OfficialSlicePhase::Completed;
    m_slicing_state = state;
    if (state.official.phase == OfficialSlicePhase::Completed && background_slicing &&
        (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview) &&
        m_active_page == Page::Model &&
        !m_slice_completion_retry_pending) {
        // Plater publishes the completion event before it clears its running
        // flag. Defer one snapshot until that event returns so the successful
        // result can enter the native preview route without polling.
        m_slice_completion_retry_pending = true;
        wxWeakRef<RedesignShell> weak(this);
        CallAfter([weak] {
            if (!weak) return;
            weak->m_slice_completion_retry_pending = false;
            if (!weak->m_plater || weak->m_active_page != Page::Model ||
                (weak->m_model_view != ModelView::Slicing && weak->m_model_view != ModelView::Preview) ||
                weak->m_plater->is_background_process_slicing()) return;
            if (weak->m_slicing_host)
                weak->apply_slicing_state(weak->m_slicing_host->workbench_snapshot());
            auto* plate = weak->m_plater->get_partplate_list().get_curr_plate();
            if (plate && plate->is_slice_result_valid())
                weak->show_model_view(ModelView::Preview);
        });
    }
    if (!m_native_slicing && native_workspace_visible() && running) {
        if (auto* notifications = m_plater->get_notification_manager())
            notifications->set_slicing_progress_hidden();
    }
    if (began) {
        m_slice_progress_percent = 0; m_slice_progress_stage = _L("正在切片…");
        m_slice_progress_overlay->update(m_slice_progress_stage,0);
    }
    if (!m_slice_start) return;
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    size_t faces = 0, vertices = 0;
    const auto& objects = m_plater->model().objects;
    for (size_t i = 0; plate && i < objects.size(); ++i) {
        const auto* object = objects[i];
        if (!object) continue;
        bool on_plate = false;
        for (size_t j = 0; j < object->instances.size() && !on_plate; ++j)
            on_plate = plate->contain_instance(static_cast<int>(i), static_cast<int>(j));
        if (!on_plate) continue;
        for (const auto* volume : object->volumes) {
            if (!volume || !volume->is_model_part()) continue;
            faces += volume->mesh().facets_count();
            vertices += volume->mesh().its.vertices.size();
        }
    }
    auto grouped_count = [](size_t count) {
        auto value = std::to_string(count);
        for (int offset = int(value.size()) - 3; offset > 0; offset -= 3)
            value.insert(size_t(offset), ",");
        return wxString::FromUTF8(value);
    };
    const auto stats = _L("拓扑\t三角面\n面数\t") + grouped_count(faces) +
        _L("\n顶点数\t") + grouped_count(vertices);
    bool layout_changed = set_control_label(m_slice_model_stats, stats);
    m_slice_stats_overlay->update(stats);
    update_slicing_tools();
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    const auto quick_recommendation = [&](size_t index) -> const AI::SmartSlicing::SliceRecommendation* {
        if (!state.lightweight_analysis || !state.lightweight_analysis->completed) return nullptr;
        const auto mode = index == 1 ? AI::SmartSlicing::SliceMode::SPEED :
            index == 2 ? AI::SmartSlicing::SliceMode::QUALITY : AI::SmartSlicing::SliceMode::BALANCED;
        return state.lightweight_analysis->find(mode);
    };
    for (size_t i = 0; i < titles.size(); ++i) {
        const auto& goal = state.session.recommendation.goal_result(RECOMMENDATION_GOALS[i]);
        wxString layer;
        for (const auto& parameter : state.effective_parameters[i])
            if (parameter.key == "layer_height") { layer = parameter_value(parameter); break; }
        if (layer.empty()) {
            if (const auto* quick = quick_recommendation(i))
                layer = wxString::Format(_L("%.2f mm"), quick->layer_height_mm);
        }
        const bool quick_available = quick_recommendation(i) != nullptr;
        const wxString availability = quick_available ? _L("可用于原生切片") :
            !state.analyzing && state.candidates[i] ? goal_status(goal.status) :
            state.analyzing ? _L("分析中…") : _L("待分析");
        m_slice_goals[i]->Enable((state.candidates[i].has_value() || quick_available) &&
            !running && !state.official.workspace_mutated && !m_slice_tools->busy());
        auto* button = static_cast<SlicingGoalCard*>(m_slice_goals[i]);
        const bool pending = workbench_goal_is_pending(state, RECOMMENDATION_GOALS[i]);
        button->update(layer.empty() ? availability : layer,
            workbench_goal_is_currently_selected(state, RECOMMENDATION_GOALS[i]) || pending,
            workbench_goal_was_last_sliced(state, RECOMMENDATION_GOALS[i]));
        wxString tooltip = layer+"\n"+availability;
        if (workbench_goal_was_last_sliced(state, RECOMMENDATION_GOALS[i]))
            tooltip += _L("\n上次切片成功");
        set_control_tooltip(button, tooltip);
    }
    const auto display_goal = state.has_selected_goal ? state.selected_goal :
        state.pending_slice_goal.value_or(state.last_sliced_goal.value_or(RECOMMENDATION_GOALS.front()));
    const size_t selected = static_cast<size_t>(display_goal);
    const bool has_display_goal = state.has_selected_goal || state.pending_slice_goal.has_value() ||
        state.last_sliced_goal.has_value();
    const bool has_parameters = has_display_goal && state.candidates[selected].has_value();
    const auto* selected_quick = has_display_goal ? quick_recommendation(selected) : nullptr;
    std::vector<SliceDetailRow> detail_rows;
    const bool official_ready = state.official.phase == OfficialSlicePhase::Completed &&
        !background_slicing && plate && plate->is_slice_result_valid();
    if (official_ready)
        detail_rows = official_detail_rows(plate);
    else if (has_parameters)
        detail_rows = candidate_detail_rows(state.effective_parameters[selected], state.metrics[selected]);
    else if (selected_quick != nullptr) {
        ExactEstimateView estimate = state.exact_estimate;
        if (estimate.goal != display_goal ||
            (state.session.workspace_revision.valid() && estimate.revision != state.session.workspace_revision))
            estimate.status = ExactEstimateStatus::None;
        detail_rows = exact_estimate_detail_rows(*selected_quick, estimate);
    }

    const bool has_details = !detail_rows.empty();
    layout_changed = m_slice_parameter_panel->Show(has_details) || layout_changed;
    if (auto* parameters = m_slice_parameter_panel->GetSizer()) {
        while (m_slice_parameter_labels.size() < detail_rows.size()) {
            auto* label = new wxStaticText(m_slice_parameter_panel, wxID_ANY, wxEmptyString);
            RedesignTheme::style_text(label, RedesignTheme::secondary_text_colour(), 12);
            parameters->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            auto* value = new wxStaticText(m_slice_parameter_panel, wxID_ANY, wxEmptyString);
            RedesignTheme::style_text(value, RedesignTheme::primary_text_colour(), 12);
            parameters->Add(value, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
            m_slice_parameter_labels.push_back(label);
            m_slice_parameter_values.push_back(value);
            layout_changed = true;
        }
        bool parameter_layout_changed = false;
        for (size_t i = 0; i < m_slice_parameter_labels.size(); ++i) {
            const bool visible = i < detail_rows.size();
            const wxString label_text = visible ? detail_rows[i].label : wxEmptyString;
            const wxString value_text = visible ? detail_rows[i].value : wxEmptyString;
            parameter_layout_changed = parameters->Show(m_slice_parameter_labels[i], visible) || parameter_layout_changed;
            parameter_layout_changed = parameters->Show(m_slice_parameter_values[i], visible) || parameter_layout_changed;
            parameter_layout_changed = set_control_label(m_slice_parameter_labels[i],
                label_text) || parameter_layout_changed;
            parameter_layout_changed = set_control_label(m_slice_parameter_values[i],
                value_text) || parameter_layout_changed;
        }
        if (parameter_layout_changed) {
            parameters->Layout();
            layout_changed = true;
        }
    }
    const bool lightweight_ready = state.lightweight_analysis && state.lightweight_analysis->completed;
    const bool preflight_has_blocking_issue = state.preflight && state.preflight->has_blocking_issue();
    wxString status;
    if (state.analyzing) status = state.session.state == RecommendationSessionState::Canceled
        ? _L("正在取消分析…") : _L("正在分析三种方案…");
    else if (lightweight_ready && preflight_has_blocking_issue) {
        // The compact status is deliberately about analysis completion only.
        // Detailed preflight notes remain available through the status tooltip.
        status = _L("推荐方案已生成");
        if (state.preflight) for (const auto& issue : state.preflight->issues) {
            if (!issue.blocks_trial_slice) continue;
            switch (issue.code) {
            case IssueCode::OpenMesh: status += _L("\n网格存在开放边：Orca 原生流程将自行处理或提示。"); break;
            case IssueCode::OutsideBuildVolume: status += _L("\n模型超出打印空间：请调整尺寸和摆放。"); break;
            case IssueCode::UnsupportedMaterialCombination:
                status += _L("\n耗材兼容性未通过验证：") + capability_reasons(state.material_reasons);
                break;
            case IssueCode::IncompatiblePhysicalSlots: status += _L("\n耗材不兼容：请在 Orca 原生页选择兼容耗材。"); break;
            case IssueCode::MachineCapabilityUnavailable:
                status += _L("\nAI 机型能力未通过验证：") + capability_reasons(state.machine_reasons) +
                    _L("。本次仍按 Orca 原生流程执行。");
                break;
            default: status += "\n" + wxString::FromUTF8(issue.evidence); break;
            }
        }
    } else if (lightweight_ready)
        status = _L("推荐方案已生成");
    else if (state.diagnostic == "workspace_changed") status = _L("工程已变化，请重新分析。");
    else if (!state.diagnostic.empty()) status = _L("分析未完成：") + wxString::FromUTF8(state.diagnostic);
    if (running) status = _L("正在正式切片…");
    else if (state.official.phase == OfficialSlicePhase::Completed) status = _L("切片完成");
    else if (!state.official.diagnostic_code.empty()) {
        const auto& diagnostic = state.official.diagnostic_code;
        wxString detail;
        if (diagnostic == "workspace_changed") detail = _L("工程已变化，请重新切片。");
        else if (diagnostic == "official_slice_canceled") detail = _L("切片已取消。");
        else if (diagnostic == "official_slice_not_started") detail = _L("未能开始切片，请检查模型与打印配置。");
        else if (diagnostic == "official_slice_failed") detail = _L("切片失败，请检查模型与打印配置。");
        else detail = wxString::FromUTF8(diagnostic);
        status = (state.official.workspace_mutated ? _L("方案已应用。") : wxString()) + detail;
    }
    m_preview_toolpath_outside = OrcaPrinterAdapter(m_plater).preview_toolpath_outside();
    if (state.official.phase == OfficialSlicePhase::Completed && !background_slicing && plate) {
        const auto* result = plate->get_slice_result();
        const bool toolpath_outside = m_preview_toolpath_outside || (result && result->toolpath_outside);
        if (toolpath_outside) {
            status = wxString::FromUTF8(OrcaPrinterAdapter(m_plater).preview_toolpath_diagnostic());
            if (status.empty())
                status = _L("路径超出热床，请调整模型或擦料塔位置后重新切片。");
        } else if (!plate->is_slice_result_ready_for_export()) {
            status = _L("切片完成，导出前请处理预览中的错误。");
        }
    }
    set_control_tooltip(m_slice_status, status);
    const bool needs_attention = !state.analyzing && !state.diagnostic.empty();
    const bool recommendation_ready = lightweight_ready && !running &&
        state.official.phase != OfficialSlicePhase::Completed;
    // Keep the outcome and the explicit retry action visible after the
    // background worker completes.  The three-card flow is usable even when
    // the diagnostic-only SmartSlice preflight reports an issue, so hiding
    // these controls based solely on `diagnostic` made a completed analysis
    // look unavailable and removed the only way to rerun it.
    const bool show_analysis_status = running || state.analyzing || recommendation_ready || needs_attention ||
        state.official.phase == OfficialSlicePhase::Failed;
    layout_changed = m_slice_status->Show(show_analysis_status) || layout_changed;
    const wxString status_label = running
        ? status
        : recommendation_ready
        ? _L("推荐方案已生成")
        : (needs_attention ? _L("方案暂不可用 · 查看原因") : status);
    layout_changed = set_wrapped_label(m_slice_status, status_label, FromDIP(285)) || layout_changed;
    layout_changed = m_slice_analyze->Show(show_analysis_status && !running) || layout_changed;
    m_slice_analyze->SetLabel(_L("重新分析"));
    set_control_tooltip(m_slice_analyze, _L("重新执行三种轻量分析；选择方案后将直接使用 Orca 原生切片。"));
    layout_changed = m_slice_keep_mesh->Show(state.can_keep_current_mesh && !state.analyzing && !running) || layout_changed;
    const auto route = workbench_slice_route(m_native_slicing, running || m_import_in_progress || m_slice_tools->busy(),
        state.can_retry, state.can_start, state.can_start_native);
    m_slice_start->Enable(route != WorkbenchSliceRoute::Unavailable);
    m_slice_start->SetLabel(state.can_retry && !m_native_slicing ? _L("重试切片") : _L("开始切片"));
    if (route == WorkbenchSliceRoute::Unavailable && !m_native_slicing) {
        set_control_tooltip(m_slice_start, lightweight_ready
            ? _L("当前方案还未选择，或 Orca 原生切片暂时不可启动。")
            : _L("请等待三种方案分析完成后再开始切片。"));
    } else {
        set_control_tooltip(m_slice_start, wxEmptyString);
    }
    m_slice_analyze->Enable(state.can_analyze);
    layout_changed = m_slice_cancel->Show(state.analyzing || running) || layout_changed;
    m_slice_cancel->SetLabel(running ? _L("取消切片") : _L("取消分析"));
    const bool can_switch = slicing_mode_switch_allowed();
    m_ai_slicing_tab->Enable(can_switch);
    m_native_slicing_tab->Enable(can_switch);
    m_slice_return_overview->Enable(can_switch);
    auto* ai_tab = static_cast<WorkbenchButton*>(m_ai_slicing_tab);
    auto* native_tab = static_cast<WorkbenchButton*>(m_native_slicing_tab);
    const StateColor ai_tab_colour(m_native_slicing
        ? RedesignTheme::secondary_text_colour() : RedesignTheme::primary_text_colour());
    const StateColor native_tab_colour(m_native_slicing
        ? RedesignTheme::primary_text_colour() : RedesignTheme::secondary_text_colour());
    if (ai_tab->GetTextColor() != ai_tab_colour) ai_tab->SetTextColor(ai_tab_colour);
    if (native_tab->GetTextColor() != native_tab_colour) native_tab->SetTextColor(native_tab_colour);
    m_slice_export->Enable(OrcaPrinterAdapter(m_plater).snapshot().gcode_ready);
    refresh_project_save_actions();
    const bool can_open_print = !running && !m_import_in_progress && !m_slice_tools->busy() && OrcaPrinterAdapter(m_plater).snapshot().gcode_ready;
    m_slice_print->Enable(can_open_print);
    if (m_native_return_ai) {
        m_native_return_ai->Enable(can_switch);
        m_native_return_overview->Enable(can_switch);
    }
    if (completed && m_active_page == Page::Model &&
        (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview) &&
        plate && plate->is_slice_result_valid())
        show_model_view(ModelView::Preview);
    else if (!external_toolpath && !m_native_slicing && !running && plate && !plate->is_slice_result_valid() &&
             m_model_view == ModelView::Preview && m_active_page == Page::Model) {
        wxWeakRef<RedesignShell> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->native_workspace_visible() || weak->m_model_view != ModelView::Preview ||
                workbench_slice_running(weak->m_plater->is_background_process_slicing(),
                                        weak->m_slicing_state.official.phase)) return;
            auto* current = weak->m_plater->get_partplate_list().get_curr_plate();
            if (current && !current->is_slice_result_valid()) weak->show_model_view(ModelView::Slicing);
        });
    }
    if (layout_changed) {
        m_slice_status->GetParent()->Layout();
        if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_slice_status->GetParent())) scroll->FitInside();
        if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_ai_slicing_controls->GetParent())) scroll->FitInside();
    }
    layout_slicing_overlays();
}

bool RedesignShell::can_save_print_project() const
{
    return print_project_save_available(m_plater && !m_plater->model().objects.empty(), m_import_in_progress,
        workbench_slice_running(m_plater && m_plater->is_background_process_slicing(),
            m_slicing_state.official.phase) || (m_slice_tools && m_slice_tools->busy()),
        m_workbench_state.dirty, m_workbench_state.actions.status);
}

void RedesignShell::refresh_project_save_actions()
{
    const bool available = can_save_print_project();
    if (m_assets_workspace) m_assets_workspace->set_project_available(available);
    if (m_slice_save) m_slice_save->Enable(available);
}

void RedesignShell::save_print_project()
{
    if (!can_save_print_project()) {
        wxMessageBox(_L("请先保存美颜结果并导入切片工程，或等待当前操作完成。"),
            _L("保存打印工程"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    // The native Save As command owns 3MF serialization, overwrite confirmation,
    // preset metadata and the project's saved/dirty state.
    try {
        m_plater->save_project(true);
    } catch (const boost::filesystem::filesystem_error&) {
        wxMessageBox(_L("无法写入安装目录下的 models/projects 文件夹，请检查写入权限。"),
            _L("保存打印工程"), wxOK | wxICON_ERROR, this);
    }
    refresh_project_save_actions();
}

void RedesignShell::start_workbench_slice()
{
    if (m_import_in_progress || !m_slicing_host || !m_slice_tools || m_slice_tools->busy()) return;
    const auto reviewed = m_slicing_host->workbench_snapshot();
    const auto route = workbench_slice_route(m_native_slicing,
        workbench_slice_running(m_plater && m_plater->is_background_process_slicing(), reviewed.official.phase),
        reviewed.can_retry, reviewed.can_start, reviewed.can_start_native);
    if (route == WorkbenchSliceRoute::Unavailable) { apply_slicing_state(reviewed); return; }
    if (route == WorkbenchSliceRoute::Native) {
        BOOST_LOG_TRIVIAL(info) << "[WorkbenchSlice] native=" << m_native_slicing << " fallback_reason=" << reviewed.diagnostic;
        m_slicing_host->start_native_slice();
        return;
    }
    std::vector<RiskConfirmationKind> confirmations;
    const auto& goal = reviewed.session.recommendation.goal_result(reviewed.selected_goal);
    if (goal.risk_confirmation_contract) confirmations = goal.risk_confirmation_contract->required_confirmations();
    if (!confirmations.empty()) {
        wxString risks;
        for (auto risk : confirmations) risks += risk == RiskConfirmationKind::ProtectedRegionSupportContact ?
            _L("保护区域将接触支撑。\n") : _L("保护区域将出现接缝。\n");
        if (show_redesign_confirmation(this, risks, _L("切片风险确认"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) return;
    }
    const auto result = m_slicing_host->start_workbench_slice(reviewed, confirmations);
    if (result.phase == OfficialSlicePhase::Rejected)
        wxMessageBox(wxString::FromUTF8(result.diagnostic_code), _L("切片方案已变化"), wxOK | wxICON_WARNING, this);
}
}
