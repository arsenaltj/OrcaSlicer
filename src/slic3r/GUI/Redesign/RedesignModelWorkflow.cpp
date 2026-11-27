#include "../AI/ModelGeneration/WorkbenchImportEligibility.hpp"
#include <glad/gl.h>
#include "RedesignShell.hpp"
#include "../DiagnosticStageTimer.hpp"
#include "AdvancedCreationWorkspace.hpp"
#include "RedesignMessageDialog.hpp"
#include "HonorBrandMark.hpp"
#include "RedesignFeatureFlags.hpp"
#include "RedesignModelRoute.hpp"
#include "RedesignTheme.hpp"
#include "RedesignModelLoadingCard.hpp"
#include "RedesignPaintPanel.hpp"
#include "AssetsWorkspace.hpp"
#include "AssetsWorkspacePolicy.hpp"
#include "RedesignSlicingControls.hpp"
#include "RedesignSlicingDetails.hpp"
#include "RedesignSlicingTools.hpp"
#include "PrinterWorkspace.hpp"
#include "OrcaPrinterAdapter.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/ModelPreview3D.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../AI/ModelGeneration/WorkbenchImportPresentation.hpp"
#include "../AI/SmartSlicing/SmartSlicingFeatureHost.hpp"
#include "../AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../ImGuiWrapper.hpp"
#include "../MainFrame.hpp"
#include "../Shortcuts.hpp"
#include "../BBLTopbar.hpp"
#include "../Plater.hpp"
#include "../ParamsPanel.hpp"
#include "../GLCanvas3D.hpp"
#include "../BackgroundSlicingProcess.hpp"
#include "../NativeSlicingEvents.hpp"
#include "../NotificationManager.hpp"
#include "../PresetComboBoxes.hpp"
#include "../Widgets/ComboBox.hpp"
#include "../Widgets/SpinInput.hpp"
#include "../Widgets/Label.hpp"
#include "../Widgets/TabCtrl.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Layer.hpp"

#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <wx/glcanvas.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/activityindicator.h>
#include <wx/colordlg.h>
#include <wx/dialog.h>
#include <wx/frame.h>
#include <wx/icon.h>
#include <wx/wupdlock.h>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace Slic3r::GUI {
namespace {
using namespace AI::SmartSlicing;

bool matching_workbench_result(const PartPlate* plate, bool custom, bool external,
                               const SmartSlicingWorkbenchState& state)
{
    if (!plate) return false;
    if (external) return true;
    if (custom) return !plate->uses_workbench_smart_process();
    return plate->uses_workbench_smart_process() && state.has_selected_goal &&
        plate->workbench_smart_goal() == static_cast<int>(state.selected_goal);
}

wxString custom_layer_summary(PartPlate& plate, const PresetBundle& bundle, bool have_result)
{
    const auto* global = bundle.prints.get_edited_preset().config.option<ConfigOptionFloat>("layer_height");
    if (!global) return {};
    wxString summary = wxString::Format(_L("全局层高 %.2f mm"), global->value);
    if (const auto* plate_height = plate.config()->option<ConfigOptionFloat>("layer_height"))
        summary += wxString::Format(_L(" · 盘级 %.2f mm"), plate_height->value);
    if (!have_result || !plate.fff_print()) return summary;

    bool local = false;
    double minimum = std::numeric_limits<double>::max();
    double maximum = 0.;
    for (const auto* object : plate.fff_print()->objects()) {
        const auto* source = object->model_object();
        if (source) {
            local = local || source->has_custom_layering() || source->config.has("layer_height");
            for (const auto* volume : source->volumes)
                local = local || volume->config.has("layer_height");
        }
        for (const auto* layer : object->layers()) {
            minimum = std::min(minimum, double(layer->height));
            maximum = std::max(maximum, double(layer->height));
        }
    }
    if (maximum > 0.) {
        summary += std::abs(maximum - minimum) < 0.005
            ? wxString::Format(_L("\n实际层高 %.2f mm"), maximum)
            : wxString::Format(_L("\n实际层高 %.2f–%.2f mm"), minimum, maximum);
        if (local) summary += _L(" · 含对象或分层覆盖");
    }
    return summary;
}

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
    if (bed.defined) {
        bed.min.z() = bed.max.z() = 0.;
        frame_bounds.merge(bed);
    }
    // Orca's front is the handle side of the build plate (-Y). Elevate the
    // initial view enough to show the plate while keeping the model front centered.
    constexpr double elevation = PI / 6.;
    const Vec3d target = frame_bounds.center();
    const Vec3d toward_camera(0., -std::cos(elevation), std::sin(elevation));
    camera.look_at(target + camera.get_distance() * toward_camera, target, Vec3d::UnitZ());
    camera.auto_type(Camera::EType::Perspective);
    camera.zoom_to_box(frame_bounds, 1.5);
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
    const bool changed = set_slicing_detail_label(*label, wrapped);
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

std::vector<SliceDetailRow> candidate_detail_rows(
    const std::vector<ConfigPatchEntry>& parameters, const std::optional<TrialMetrics>& metrics,
    const std::vector<SliceDetailRow>& official = {})
{
    std::vector<SliceDetailRow> rows = official;
    rows.reserve(parameters.size() + 3);
    if (official.empty() && metrics) {
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

void add_orientation_preview(wxWindow* parent, wxSizer* row, const wxString& label,
                             const std::vector<std::array<Vec3f, 3>>& mesh)
{
    auto* column = new wxBoxSizer(wxVERTICAL);
    column->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER | wxBOTTOM, parent->FromDIP(6));
    auto* canvas = new wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(208, 180)));
    canvas->SetMinSize(parent->FromDIP(wxSize(208, 180)));
    canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
    const auto project = [](const Vec3f& p) { return Vec2f(p.x() + 0.40f * p.y(), p.z() + 0.25f * p.y()); };
    std::vector<std::array<Vec2f, 3>> triangles;
    Vec2f lower = Vec2f::Constant(std::numeric_limits<float>::max());
    Vec2f upper = -lower;
    for (const auto& face : mesh) {
        auto& triangle = triangles.emplace_back();
        for (size_t i = 0; i < 3; ++i) {
            triangle[i] = project(face[i]);
            lower = lower.cwiseMin(triangle[i]);
            upper = upper.cwiseMax(triangle[i]);
        }
    }
    canvas->Bind(wxEVT_PAINT, [canvas, triangles = std::move(triangles), lower, upper](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(canvas);
        dc.SetBackground(wxBrush(RedesignTheme::secondary_text_colour().ChangeLightness(25)));
        dc.Clear();
        if (triangles.empty()) return;
        const auto size = canvas->GetClientSize();
        const Vec2f extent = (upper - lower).cwiseMax(Vec2f::Constant(1.f));
        const float scale = 0.82f * std::min(size.x / extent.x(), size.y / extent.y());
        const Vec2f center = (lower + upper) * 0.5f;
        dc.SetPen(wxPen(wxColour(168, 181, 190)));
        dc.SetBrush(wxBrush(wxColour(97, 130, 152)));
        for (const auto& triangle : triangles) {
            wxPoint vertices[3];
            for (size_t i = 0; i < 3; ++i) {
                const auto p = triangle[i] - center;
                vertices[i] = {int(size.x * 0.5f + p.x() * scale), int(size.y * 0.5f - p.y() * scale)};
            }
            dc.DrawPolygon(3, vertices);
        }
    });
    column->Add(canvas, 1, wxEXPAND);
    row->Add(column, 1, wxEXPAND | wxALL, parent->FromDIP(6));
}

std::vector<SliceDetailRow> official_detail_rows(PartPlate* plate)
{
    std::vector<SliceDetailRow> rows;
    if (plate == nullptr || !plate->is_slice_result_valid() || plate->get_slice_result() == nullptr)
        return rows;
    const auto& statistics = plate->get_slice_result()->print_statistics;
    const double seconds = statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    const auto grams = plate->get_slice_result()->material_weight_grams(statistics.total_volumes_per_extruder);
    rows.push_back({_L("打印时间"), format_estimated_time(seconds), _L("正式切片结果")});
    rows.push_back({_L("实际耗材"), grams ? wxString::Format(_L("%.2f g"), *grams) : _L("暂无"), _L("正式切片结果")});
    return rows;
}
}

bool RedesignShell::owns_model_workflow() const
{
    return m_workbench_page != nullptr && m_plater != nullptr;
}

bool RedesignShell::native_workspace_visible() const
{
    return owns_model_workflow() && m_page_coordinator.page() == Page::Model &&
        (m_page_coordinator.view() == ModelView::Slicing || m_page_coordinator.view() == ModelView::Preview);
}

void RedesignShell::start_slicing_from_workspace()
{
    if (m_import_in_progress || !native_workspace_visible()) return;
    start_workbench_slice();
}

bool RedesignShell::slicing_mode_switch_allowed() const
{
    return workbench_mode_switch_allowed(m_import_in_progress, m_plater && m_plater->is_background_process_slicing(),
        m_slicing_state.official.phase == OfficialSlicePhase::Slicing, m_slice_tools && m_slice_tools->busy(),
        m_slicing_state.analyzing);
}

void RedesignShell::refresh_native_theme()
{
    if (!m_custom_process_mount.active() || !m_custom_process_content) return;
    m_native_theme.apply(m_custom_process_content, {});
    if (auto* parameters = wxGetApp().params_panel()) parameters->Layout();
}

void RedesignShell::mount_custom_process(bool visible)
{
    auto* parameters = wxGetApp().params_panel();
    if (!m_custom_process_content || !parameters) return;
    if (visible && !m_custom_process_mount.active()) {
        if (!m_custom_process_mount.mount(parameters, m_custom_process_content, 0)) return;
        parameters->set_workbench_scroll_host(m_slicing_scroll);
        static constexpr ConfigOptionMode modes[] = {comSimple, comAdvanced, comExpert};
        parameters->set_workbench_process_mode(modes[m_custom_process_level]);
        parameters->Show();
        parameters->OnActivate();
        refresh_native_theme();
    } else if (!visible && m_custom_process_mount.active()) {
        parameters->set_workbench_scroll_host(nullptr);
        m_native_theme.restore();
        parameters->set_workbench_process_mode(-1);
        m_custom_process_mount.restore();
    }
    if (m_custom_process_content->Show(visible && m_custom_process_mount.active()))
        m_custom_process_content->InvalidateBestSize();
    if (m_smart_slicing_content) m_smart_slicing_content->Show(!visible);
    if (m_slicing_scroll) m_slicing_scroll->Layout();
}

void RedesignShell::build_model_workflow()
{
    if (!RedesignFeatureFlags::model_workflow_review_enabled() || !m_model_generation_host || !m_plater) return;
    wxWeakRef<RedesignShell> weak(this);
    m_print_page->set_return_to_preview_handler([weak] { if (weak) weak->return_to_slice_preview(); });
    m_workbench_page = new wxPanel(m_content_host, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
    m_workbench_page->SetBackgroundColour(RedesignTheme::background_colour());
    m_workbench_page->SetBackgroundStyle(wxBG_STYLE_ERASE);
    // The nested wxGLCanvas owns its swap-chain buffering. Enabling native
    // parent compositing on Windows can present a black child surface.
    m_workbench_page->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_content_host->GetSizer()->Add(m_workbench_page, 1, wxEXPAND);
    m_workbench_page->Hide();
    m_model_generation_host->mount_workbench(m_workbench_page);
    m_pages[static_cast<std::size_t>(Page::Model)] = m_workbench_page;

    m_slicing_page = new wxPanel(m_content_host, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                 wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
    m_slicing_page->SetBackgroundColour(RedesignTheme::background_colour());
    m_slicing_page->SetBackgroundStyle(wxBG_STYLE_ERASE);
    m_plater->SetBackgroundColour(RedesignTheme::background_colour());
    m_plater->SetBackgroundStyle(wxBG_STYLE_ERASE);
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
            add_navigation(m_native_return_ai, _L("← 智能切片"), [](auto& shell) { shell.select_slicing_tab(false); });
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
    m_slice_return_overview = command(settings, _L("← 返回模型编辑"));
    m_slice_return_overview->SetName("slicing-return-model-overview");
    m_slice_return_overview->SetMinSize(FromDIP(wxSize(-1,40)));
    m_slice_return_overview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { return_model_overview(); });
    root->Add(m_slice_return_overview, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    m_slice_save = command(settings, _L("保存工程（3MF）"));
    m_slice_save->SetToolTip(_L("保存模型、耗材颜色、摆放和打印参数；G-code 需切片后单独导出。"));
    root->AddSpacer(FromDIP(8));
    root->Add(m_slice_save, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_slice_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_print_project(); });
    auto* scroll = m_slicing_scroll = new WorkbenchScrolledWindow(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(RedesignTheme::panel_colour());
    scroll->SetScrollRate(0, FromDIP(12));
    auto* controls = new wxBoxSizer(wxVERTICAL);
    scroll->SetSizer(controls);
    auto* inset = new wxBoxSizer(wxHORIZONTAL);
    inset->AddSpacer(FromDIP(24)); inset->Add(scroll,1,wxEXPAND); inset->AddSpacer(FromDIP(7));
    root->Add(inset, 1, wxEXPAND | wxTOP, FromDIP(12));

    // Keep the controls alive for the existing palette and beauty workflows,
    // but omit their container from this page's layout.
    auto* hidden_sections = new wxPanel(scroll);
    auto* hidden_controls = new wxBoxSizer(wxVERTICAL);
    hidden_sections->SetSizer(hidden_controls);
    hidden_sections->Hide();
    auto* color_heading = new wxStaticText(hidden_sections, wxID_ANY, _L("多色模型"));
    RedesignTheme::style_text(color_heading, RedesignTheme::primary_text_colour(), 12);
    hidden_controls->Add(color_heading, 0, wxTOP | wxBOTTOM, FromDIP(12));
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
    auto* slicing_heading = new wxBoxSizer(wxHORIZONTAL);
    auto* slicing_heading_label = new wxStaticText(scroll, wxID_ANY, _L("切片"));
    RedesignTheme::style_text(slicing_heading_label, RedesignTheme::primary_text_colour(), 12);
    slicing_heading->Add(slicing_heading_label, 0, wxALIGN_CENTER_VERTICAL);
    slicing_heading->AddSpacer(FromDIP(12));
    m_slice_recommendation_source = create_slicing_recommendation_source(scroll);
    slicing_heading->Add(m_slice_recommendation_source, 0, wxALIGN_CENTER_VERTICAL);
    controls->Add(slicing_heading, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    m_ai_slicing_controls = new WorkbenchPanel(scroll, true);
    static_cast<WorkbenchPanel*>(m_ai_slicing_controls)->set_corner_radius(12);
    m_ai_slicing_controls->SetBackgroundColour(slicing_control_colour());
    auto* ai = new wxBoxSizer(wxVERTICAL);
    m_ai_slicing_controls->SetSizer(ai);
    auto* tabs = new wxBoxSizer(wxHORIZONTAL);
    m_ai_slicing_tab = new SlicingModeButton(m_ai_slicing_controls, _L("智能切片"));
    m_native_slicing_tab = new SlicingModeButton(m_ai_slicing_controls, _L("Orca原生切片"));
    auto* divider = new wxPanel(m_ai_slicing_controls);
    divider->SetBackgroundColour(wxColour(8, 8, 10));
    divider->SetMinSize(FromDIP(wxSize(2, 24)));
    tabs->Add(m_ai_slicing_tab, 1, wxEXPAND);
    tabs->Add(divider, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    tabs->Add(m_native_slicing_tab, 1, wxEXPAND);
    ai->Add(tabs, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    ai->AddSpacer(FromDIP(8));
    m_ai_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(false); });
    m_native_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(true); });

    m_smart_slicing_content = new wxPanel(m_ai_slicing_controls);
    m_smart_slicing_content->SetBackgroundColour(slicing_control_colour());
    auto* smart_content = new wxBoxSizer(wxVERTICAL);
    m_smart_slicing_content->SetSizer(smart_content);
    ai->Add(m_smart_slicing_content, 0, wxEXPAND);
    auto* goals = new wxGridSizer(2, FromDIP(12), FromDIP(12));
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    for (size_t i = 0; i < titles.size(); ++i) {
        auto* button = new SlicingGoalCard(m_smart_slicing_content, titles[i]);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            if (m_slicing_host) m_slicing_host->select_goal(RECOMMENDATION_GOALS[i]);
        });
        m_slice_goals[i] = button;
        goals->Add(button, 0, wxEXPAND);
    }
    smart_content->Add(goals, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_slice_parameter_panel = new wxPanel(m_smart_slicing_content);
    m_slice_parameter_panel->SetBackgroundColour(m_ai_slicing_controls->GetBackgroundColour());
    auto* parameters = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12)); parameters->AddGrowableCol(0);
    m_slice_parameter_panel->SetSizer(parameters);
    smart_content->Add(m_slice_parameter_panel,0,wxEXPAND | wxALL,FromDIP(16));
    m_slice_parameter_panel->Hide();
    m_slice_analyze = slicing_link(m_smart_slicing_content, _L("重新分析"));
    smart_content->Add(m_slice_analyze, 0, wxEXPAND | wxALL, FromDIP(8));
    m_slice_analyze->Hide();
    m_slice_analyze->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_slicing_host) { m_slicing_host->analyze_workbench(); apply_slicing_state(m_slicing_host->workbench_snapshot()); }
    });
    m_slice_orientation = slicing_link(m_smart_slicing_content, _L("比较摆放角度"));
    smart_content->Add(m_slice_orientation, 0, wxEXPAND | wxALL, FromDIP(8));
    m_slice_orientation->Hide();
    m_slice_orientation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_slicing_host) return;
        const auto current = m_slicing_host->workbench_snapshot();
        if (!current.can_start) return;
        if (!current.orientation_estimate) {
            m_slicing_host->request_orientation_comparison();
            return;
        }
        if (!current.orientation_estimate->native_details) return;
        const auto candidate = *current.orientation_estimate;
        const auto& effect = *candidate.native_details;
        wxDialog dialog(this, wxID_ANY, _L("比较摆放角度"));
        auto* content = new wxBoxSizer(wxVERTICAL);
        auto* figures = new wxBoxSizer(wxHORIZONTAL);
        add_orientation_preview(&dialog, figures, _L("原摆放"), effect.original_preview);
        add_orientation_preview(&dialog, figures, _L("备选摆放"), effect.oriented_preview);
        content->Add(figures, 1, wxEXPAND | wxALL, FromDIP(8));
        const auto mass = wxString::Format(_L("旋转 %.1f°，预计总耗材 %.2f g，支撑 %.2f g，清洗 %.2f g，擦料塔 %.2f g"),
            effect.orientation_degrees, candidate.material_grams.value_or(0.), effect.support_grams.value_or(0.),
            effect.flush_grams.value_or(0.), effect.tower_grams.value_or(0.));
        auto* summary = new wxStaticText(&dialog, wxID_ANY, mass + "\n" + _L("预计用时 ") +
            format_estimated_time(candidate.estimated_time_seconds.value_or(0.)));
        summary->Wrap(FromDIP(410));
        content->Add(summary, 0, wxEXPAND | wxALL, FromDIP(12));
        if (auto* buttons = dialog.CreateButtonSizer(wxOK | wxCANCEL))
            content->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
        if (auto* ok = dialog.FindWindow(wxID_OK))
            ok->SetLabel(_L("选择备选摆放"));
        dialog.SetSizerAndFit(content);
        if (dialog.ShowModal() == wxID_OK && m_slicing_host)
            m_slicing_host->select_orientation(candidate);
    });
    m_custom_process_content = new wxPanel(m_ai_slicing_controls);
    m_custom_process_content->SetBackgroundColour(RedesignTheme::panel_colour());
    auto* custom_content = new wxBoxSizer(wxVERTICAL);
    m_custom_process_content->SetSizer(custom_content);
    m_custom_process_content->Bind(wxEVT_TAB_SEL_CHANGED, [weak](wxCommandEvent& event) {
        if (weak) {
            // Search and keyboard reveal requests run after the category has been laid out.
            if (event.GetExtraLong() && weak->m_slicing_scroll) weak->m_slicing_scroll->Scroll(-1, 0);
            weak->refresh_native_theme();
        }
    });
    auto* setting_level = new ComboBox(m_custom_process_content, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(-1, 28)), 0, nullptr, wxCB_READONLY);
    setting_level->Append(_L("Simple settings"));
    setting_level->Append(_L("Advanced settings"));
    setting_level->Append(_L("Expert settings"));
    setting_level->SetSelection(0);
    setting_level->SetName("custom-slicing-settings-level");
    setting_level->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& event) {
        m_custom_process_level = std::clamp(event.GetSelection(), 0, 2);
        static constexpr ConfigOptionMode modes[] = {comSimple, comAdvanced, comExpert};
        if (auto* params = wxGetApp().params_panel()) params->set_workbench_process_mode(modes[m_custom_process_level]);
        refresh_native_theme();
    });
    custom_content->Add(setting_level, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    ai->Add(m_custom_process_content, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_custom_process_content->Hide();
    m_slice_cancel = slicing_link(settings, _L("取消分析"));
    root->Add(m_slice_cancel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    m_slice_cancel->Hide();
    m_slice_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (workbench_slice_running(m_plater && m_plater->is_background_process_slicing(),
                                    m_slicing_state.official.phase))
            m_plater->cancel_current_slice();
        else if (m_slicing_host) m_slicing_host->cancel_workbench_analysis();
    });
    m_slice_status = new wxStaticText(settings, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_slice_status, RedesignTheme::secondary_text_colour(), 9);
    root->Add(m_slice_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_slice_status->Hide();
    m_slice_status->SetCursor(wxCURSOR_HAND);
    m_slice_status->Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&) {
        wxMessageBox(m_slice_status->GetToolTipText(),_L("切片状态"),wxOK,this);
    });
    m_slice_keep_mesh = command(m_smart_slicing_content, _L("保留当前网格并分析"));
    smart_content->Add(m_slice_keep_mesh, 0, wxEXPAND | wxALL, FromDIP(12)); m_slice_keep_mesh->Hide();
    m_slice_keep_mesh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_slicing_host) return;
        const auto reviewed = m_slicing_host->workbench_snapshot();
        if (!reviewed.can_keep_current_mesh || !reviewed.preflight) return;
        if (show_redesign_confirmation(this, _L("网格存在开放边。保留当前网格继续分析可能产生缺面或悬空路径，请在切片预览中检查结果。"),
                _L("保留当前网格"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) return;
        m_slicing_host->keep_current_mesh_and_analyze(reviewed.preflight->revision);
    });
    m_slice_start = command(settings, _L("开始切片"), true);
    static_cast<WorkbenchButton*>(m_slice_start)->SetCornerRadius(FromDIP(12));
    static_cast<WorkbenchButton*>(m_slice_start)->SetBackgroundColor(StateColor(
        std::pair<wxColour,int>(wxColour(68,68,69),StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(82,82,84),StateColor::Hovered),
        std::pair<wxColour,int>(wxColour(68,68,69),StateColor::Normal)));
    static_cast<WorkbenchButton*>(m_slice_start)->SetTextColor(StateColor(
        std::pair<wxColour,int>(wxColour(126,126,130),StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(229,229,230),StateColor::Normal)));
    m_slice_start->SetMinSize(FromDIP(wxSize(-1, 40)));
    root->Add(m_slice_start, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
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
    m_slice_print->Disable();
    root->Add(m_slice_print, 0, wxEXPAND | wxALL, FromDIP(16));
    m_slice_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_print_preparation(); });
    auto* return_slice = m_return_slice = command(settings, _L("返回切片参数"));
    return_slice->Hide();
    return_slice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_model_view(ModelView::Slicing); });
    m_native_plater_host = new wxPanel(m_slicing_page);
    m_native_plater_host->SetBackgroundColour(RedesignTheme::background_colour());
    m_native_plater_host->SetBackgroundStyle(wxBG_STYLE_ERASE);
    m_native_plater_host->SetMinSize(wxSize(1, 1));
    m_native_plater_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    row->Add(m_native_plater_host, 1, wxEXPAND);
    m_content_host->GetSizer()->Add(m_slicing_page, 1, wxEXPAND);
    m_slicing_page->Hide();
    m_slice_model_stats = new wxStaticText(settings, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_slice_model_stats, RedesignTheme::secondary_text_colour(), 9);
    m_slice_model_stats->Hide();
    m_saved_sidebar_collapsed = m_plater->is_sidebar_collapsed();
    if (!m_plater_mount.mount(m_plater, m_native_plater_host))
        throw std::runtime_error("Native plater mount failed.");
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
    auto* camera_loading_activity = new WorkbenchActivityIndicator(
        m_slice_camera_loading, FromDIP(wxSize(32, 32)));
    camera_loading_activity->SetForegroundColour(RedesignTheme::accent_colour());
    camera_loading_activity->SetBackgroundColour(RedesignTheme::background_colour());
    camera_loading_activity->Start();
    camera_loading_sizer->Add(camera_loading_activity, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(16));
    auto* camera_loading_label = new WorkbenchLabel(
        m_slice_camera_loading, _L("正在准备模型视图…"), wxALIGN_CENTER);
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
    for (auto* view : {m_plater->get_view3D_canvas3D(), m_plater->get_preview_canvas3D()}) {
        view->get_wxglcanvas()->Bind(EVT_NOTIFICATION_LAYOUT_CHANGED, [weak](wxCommandEvent& event) {
            if (weak && weak->native_workspace_visible() &&
                weak->m_plater->get_current_canvas3D()->get_wxglcanvas() == event.GetEventObject())
                weak->layout_slicing_overlays();
        });
    }
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
        if (weak && weak->m_plater)
            Presentation::background_tasks().set_native_busy(weak->m_plater->is_background_process_slicing());
        if (!weak || weak->m_presentation_stopping || weak->m_native_slicing ||
            !workbench_slice_running(weak->m_plater && weak->m_plater->is_background_process_slicing(),
                                     weak->m_slicing_state.official.phase)) return;
        // The native Plater listener and this redesign listener receive the
        // same event.  Keep the native notification hidden on the AI page so
        // users see one progress surface while retaining the native cancel
        // callback and all error notifications.
        if (weak->native_workspace_visible())
            if (auto* notifications = weak->m_plater->get_notification_manager())
                notifications->set_slicing_progress_hidden();
        weak->m_slice_progress_percent = std::clamp(event.status.percent,0,100);
        const auto stage = wxString::FromUTF8(event.status.text);
        if (stage != weak->m_slice_progress_stage) ++weak->m_slice_progress_phase;
        weak->m_slice_progress_stage = stage;
        weak->m_slicing_dirty.invalidate(Presentation::Dirty::Overlay);
        if (weak->native_workspace_visible() &&
            weak->m_slice_progress_gate.accept(weak->m_slice_progress_phase, event.status.percent >= 100))
            weak->m_surface_refresh.request(Presentation::Dirty::Overlay);
    });
    m_plater->Bind(EVT_PROCESS_COMPLETED, [weak](SlicingProcessCompletedEvent& event) {
        event.Skip();
        if (!weak) return;
        // The native handler clears its running flag during this event. Do
        // not leave low-priority preparation paused when this page is hidden.
        weak->CallAfter([weak] {
            if (!weak || weak->m_presentation_stopping || !weak->m_plater) return;
            Presentation::background_tasks().set_native_busy(weak->m_plater->is_background_process_slicing());
            if (weak->m_slicing_host) weak->apply_slicing_state(weak->m_slicing_host->workbench_snapshot());
        });
    });

    const auto make_cancel = [](wxWindow* parent) { return command(parent, _L("取消")); };
    const auto import_card = create_model_loading_card(wxGetTopLevelParent(this),
        _L("正在读取与验证模型…"), make_cancel);
    m_import_loading = import_card.window;
    m_import_activity = import_card.activity;
    m_import_stage = import_card.stage;
    m_import_elapsed = import_card.elapsed;
    m_import_cancel = import_card.cancel;
    m_import_cancel->Bind(wxEVT_BUTTON, [weak](wxCommandEvent&) {
        if (weak && weak->m_import_session && weak->m_import_session->cancel()) {
            weak->m_import_stage->SetLabel(_L("正在取消导入…"));
            weak->m_import_cancel->Disable();
        }
    });
    m_import_loading->Bind(wxEVT_CLOSE_WINDOW, [weak](wxCloseEvent& event) {
        if (weak && weak->m_import_in_progress) event.Veto();
        else event.Skip();
    });
    m_import_loading->Hide();
    const auto history_card = create_model_loading_card(wxGetTopLevelParent(this),
        _L("正在读取模型与恢复外观…"), make_cancel);
    m_history_loading = history_card.window;
    m_history_activity = history_card.activity;
    m_history_stage = history_card.stage;
    m_history_elapsed = history_card.elapsed;
    m_history_cancel = history_card.cancel;
    m_history_cancel->Bind(wxEVT_BUTTON, [weak](wxCommandEvent&) {
        if (weak && weak->m_history_open_request && !weak->m_history_rollback_pending) {
            BOOST_LOG_TRIVIAL(info) << "[ModelPreviewLoad] user canceled request=" << weak->m_history_open_request;
            weak->finish_workbench_presentation(false, false);
        }
    });
    m_history_loading->Bind(wxEVT_CLOSE_WINDOW, [weak](wxCloseEvent& event) {
        if (weak && weak->m_history_open_request) event.Veto();
        else event.Skip();
    });
    m_content_host->Bind(wxEVT_SIZE, [weak](wxSizeEvent& event) {
        event.Skip();
        if (weak) {
            if (weak->m_import_target_staged) ++weak->m_import_layout_generation;
            weak->CallAfter([weak] { if (weak) {
                weak->request_viewport_frame();
                if (!weak->m_import_loading || !weak->m_import_in_progress) return;
                weak->layout_import_transition_cover();
                weak->layout_import_loading();
            } });
        }
    });
    m_import_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!m_import_in_progress || !m_import_transition || m_import_navigation_detached) return;
        const auto elapsed = wxString::Format(_L("已用时 %.1f 秒"),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_import_started).count());
        if (m_import_elapsed->GetLabel() != elapsed) m_import_elapsed->SetLabel(elapsed);
        layout_import_transition_cover();
        layout_import_loading();
        if (m_import_project_committed && !m_import_target_staged)
            stage_import_target_view();
        if (m_import_transition) check_import_first_frame(*m_import_transition);
    }, m_import_timer.GetId());
    m_slice_camera_retry_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!native_workspace_visible() || m_page_coordinator.view() != ModelView::Slicing ||
            m_plater->model().objects.empty()) {
            m_slice_camera_retry_timer.Stop();
            m_slice_camera_loading_pending = false;
            m_slice_camera_waiting_for_frame = false;
            layout_slicing_camera_loading();
            return;
        }
        if (m_plater->is_loading_project()) return;
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
        if (weak && weak->m_import_target_staged && weak->m_import_transition) {
            const auto token = *weak->m_import_transition;
            weak->CallAfter([weak, token] { if (weak) weak->check_import_first_frame(token); });
        }
    });
    m_plater->get_preview_canvas3D()->get_wxglcanvas()->Bind(wxEVT_PAINT, [weak](wxPaintEvent& event) {
        event.Skip();
        if (weak && weak->m_preview_transition) {
            const auto token = *weak->m_preview_transition;
            weak->CallAfter([weak, token] { if (weak) weak->check_preview_first_frame(token); });
        }
    });
    auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this));
    if (auto* topbar = frame ? frame->topbar() : nullptr) {
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_page_coordinator.page() == Page::Model &&
                weak->m_page_coordinator.view() == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                (weak->m_page_coordinator.view() == ModelView::Slicing || weak->m_page_coordinator.view() == ModelView::Preview))) && weak->m_plater->can_undo());
        }, wxID_UNDO);
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_page_coordinator.page() == Page::Model &&
                weak->m_page_coordinator.view() == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                (weak->m_page_coordinator.view() == ModelView::Slicing || weak->m_page_coordinator.view() == ModelView::Preview))) && weak->m_plater->can_redo());
        }, wxID_REDO);
    }
    Bind(wxEVT_CHAR_HOOK, [weak](wxKeyEvent& event) {
        auto* focus = wxWindow::FindFocus();
        if (weak && weak->native_workspace_visible() && !weak->m_import_in_progress &&
            !dynamic_cast<wxTextEntry*>(focus)) {
            const KeyChord chord = KeyChord::from_event(event);
            const auto shortcut = wxGetApp().shortcuts().lookup(ShortcutContext::Global, chord);
            const char* direction = nullptr;
            if (shortcut) {
                switch (*shortcut) {
                case Shortcut::ViewTop:    direction = "top"; break;
                case Shortcut::ViewBottom: direction = "bottom"; break;
                case Shortcut::ViewFront:  direction = "front"; break;
                case Shortcut::ViewRear:   direction = "rear"; break;
                case Shortcut::ViewLeft:   direction = "left"; break;
                case Shortcut::ViewRight:  direction = "right"; break;
                default: break;
                }
            }
            const auto* imgui = wxGetApp().imgui();
            if (direction && (!imgui || !imgui->owns_key(chord))) {
                weak->m_plater->select_view(direction);
                return;
            }
        }
        const auto key = event.GetKeyCode();
        if (!weak || weak->m_page_coordinator.page() != Page::Model ||
            (weak->m_page_coordinator.view() != ModelView::Workbench && !weak->native_workspace_visible()) ||
            !event.CmdDown() || event.AltDown() || event.ShiftDown() || (key != 'Z' && key != 'Y') ||
            dynamic_cast<wxTextEntry*>(focus)) {
            event.Skip();
            return;
        }
        if (weak->m_import_in_progress) return;
        // Beauty owns its preview history; native slicing owns project history.
        if (weak->m_page_coordinator.view() == ModelView::Workbench &&
            (weak->m_workbench_state.editing || !weak->m_workbench_state.can_edit_project_colors)) {
            event.Skip();
            return;
        }
        if (key == 'Z' && weak->m_plater->can_undo()) weak->m_plater->undo();
        if (key == 'Y' && weak->m_plater->can_redo()) weak->m_plater->redo();
    });
    m_model_generation_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_workbench_state(state); });
    Bind(wxEVT_IDLE, [weak](wxIdleEvent& event) {
        event.Skip();
        if (!weak || weak->m_page_coordinator.page() != Page::Model || weak->m_page_coordinator.view() != ModelView::Preview) return;
        if (weak->m_preview_transition)
            weak->check_preview_first_frame(*weak->m_preview_transition);
        if (weak->m_slicing_state.official.phase != OfficialSlicePhase::Completed) return;
        // The native preview finishes loading after the completion callback.
        // Refresh only when its result changes, without re-running analysis.
        const bool outside = OrcaPrinterAdapter(weak->m_plater).preview_toolpath_outside();
        if (outside != weak->m_preview_toolpath_outside)
            weak->apply_slicing_state(weak->m_slicing_state);
    });
    m_model_generation_host->set_workbench_return_to_design_handler([weak] {
        if (weak) weak->return_to_image_design();
    });
    m_model_generation_host->set_workbench_import_handler([weak](WorkbenchImportPreparation prepare) {
        return weak && weak->begin_workbench_import(std::move(prepare));
    });
    if (m_slicing_host) m_slicing_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_slicing_state(state); });
}

bool RedesignShell::open_model_workbench()
{
    return show_model_view(ModelView::Workbench);
}

bool RedesignShell::block_workbench_navigation(bool leaves_workbench)
{
    bool modal_blocking = m_workbench_state.modal_blocking;
    if (m_model_generation_host) modal_blocking = m_model_generation_host->workbench_snapshot().modal_blocking;
    if (workbench_navigation_allowed(modal_blocking, leaves_workbench)) return false;
    if (m_model_generation_host) m_model_generation_host->raise_workbench_modal();
    return true;
}

void RedesignShell::return_model_overview()
{
    if (slicing_mode_switch_allowed() && open_model_workbench())
        m_model_generation_host->request_return_overview();
}

bool RedesignShell::show_model_view(ModelView view)
{
    DiagnosticStageTimer timing("ORCASLICER_UI_LATENCY_TRACE", "SlicePreviewRoute");
    if (m_presentation_stopping || !owns_model_workflow()) return false;
    if (block_workbench_navigation(view != ModelView::Workbench)) return false;
    release_navigation_waits();
    if (m_page_coordinator.transition().active()) {
        queue_navigation(Page::Model, view);
        return true;
    }
    m_queued_navigation.reset();
    if (view == ModelView::Workbench) {
        restore_smart_model_history();
        m_model_generation_host->present_existing_workbench();
    }
    if (view == ModelView::Preview) {
        auto* plate = m_plater->get_partplate_list().get_curr_plate();
        if (!plate || !plate->is_slice_result_valid() || !plate->get_slice_result())
            return show_model_view(ModelView::Slicing);
    }

    // Route changes can arrive synchronously from both the button handler and
    // the model-generation state listener. Keep an already selected view
    // idempotent so a nested callback cannot repeatedly mutate the wx layout
    // while the original event is still being dispatched.
    if (m_page_coordinator.page() == Page::Model && m_page_coordinator.view() == view && view != ModelView::Preview) {
        const bool panel_out_of_sync = m_plater &&
            view == ModelView::Slicing && !m_plater->is_view3D_shown();
        if (panel_out_of_sync) {
            m_plater->select_view_3D("3D");
            m_plater->get_current_canvas3D()->use_workbench_appearance();
            m_plater->collapse_sidebar(true);
        }
        refresh_workflow_layout();
        present_active_page();
        if (m_model_generation_host)
            m_model_generation_host->set_workbench_presented(view == ModelView::Workbench);
        return true;
    }

    remember_active_page_focus();
    ++m_creation_navigation_epoch;
    const auto transition = m_page_coordinator.transition().begin(
        {int(m_page_coordinator.page()), int(m_page_coordinator.view())}, {int(Page::Model), int(view)});
    if (!transition) return false;
    if (view == ModelView::Preview) {
        m_preview_transition = *transition;
        if (!prepare_preview_transition()) {
            m_preview_transition.reset();
            m_page_coordinator.transition().rollback(*transition);
            return false;
        }
        m_preview_auto_fit.request();
    }
    if (view == ModelView::Slicing || view == ModelView::Preview) {
        m_plater->select_view_3D(view == ModelView::Preview ? "Preview" : "3D");
        timing("native-view-loaded");
        m_plater->get_current_canvas3D()->use_workbench_appearance();
        m_plater->collapse_sidebar(true);

    }
    auto* target_root = page_route_root(Page::Model, view);
    if (!target_root) {
        if (view == ModelView::Preview) cancel_preview_presentation();
        else m_page_coordinator.transition().rollback(*transition);
        return false;
    }
    if (m_content_host) target_root->SetSize(m_content_host->GetClientRect());
    target_root->Layout();
    timing("target-layout");
    if (view != ModelView::Preview && !m_page_coordinator.transition().mark_ready(*transition)) {
        m_page_coordinator.transition().rollback(*transition);
        return false;
    }
    {
        wxWindowUpdateLocker lock(this);
        m_image_page_view_only = false;
        m_page_coordinator.select_page(Page::Model);
        m_last_workbench_page = Page::Model;
        m_page_coordinator.select_view(view);
        m_active_tab_id = view == ModelView::Preview ? TAB_ID_PREVIEW :
            view == ModelView::Slicing ? TAB_ID_PREPARE : TAB_ID_GENERATE_3D;
        const auto model_index = static_cast<std::size_t>(Page::Model);
        for (std::size_t i = 0; i < m_pages.size(); ++i) {
            const bool active = i == model_index;
            const bool show_page = active && view == ModelView::Workbench;
            if (m_pages[i]) m_pages[i]->Show(show_page);
        }
        if (m_advanced_workspace) m_advanced_workspace->Hide();
        refresh_navigation();
        if (m_image_settings_panel) m_image_settings_panel->Hide();
        if (m_slicing_host)
            m_slicing_host->set_workbench_active(view == ModelView::Slicing || view == ModelView::Preview,
                                                  !m_native_slicing);
        refresh_workflow_layout();
    }
    if (m_print_page) m_print_page->set_active(false);
    present_active_page(view == ModelView::Preview);
    timing("gui-submitted");
    if (view == ModelView::Preview) {
        // The retained preview is in its final layout behind the cover. Only
        // a newly presented frame may commit this route.
        request_viewport_frame();
        return true;
    }
    if (!m_page_coordinator.transition().commit(*transition)) {
        m_page_coordinator.transition().rollback(*transition);
        return false;
    }
    if (view == ModelView::Slicing) refresh_print_preparation_action();
    if (m_model_generation_host)
        m_model_generation_host->set_workbench_presented(view == ModelView::Workbench);
    restore_active_page_focus();
    return true;
}

bool RedesignShell::prepare_preview_transition()
{
    auto* plate = m_plater ? m_plater->get_partplate_list().get_curr_plate() : nullptr;
    if (!m_preview_transition || !plate || !plate->is_slice_result_valid() || !plate->get_slice_result())
        return false;
    if (!m_preview_transition_cover) {
        m_preview_transition_cover = new wxFrame(wxGetTopLevelParent(this), wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxCLIP_CHILDREN);
        m_preview_transition_cover->SetName("slice-preview-transition");
        m_preview_transition_cover->SetBackgroundColour(RedesignTheme::background_colour());
        m_preview_transition_cover->SetBackgroundStyle(wxBG_STYLE_ERASE);
        m_preview_transition_cover->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(m_preview_transition_cover);
            dc.SetBackground(wxBrush(RedesignTheme::background_colour()));
            dc.Clear();
        });
        m_preview_transition_cover->Bind(wxEVT_CLOSE_WINDOW, [](wxCloseEvent& event) { event.Veto(); });
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->AddStretchSpacer();
        auto* message = new wxStaticText(m_preview_transition_cover, wxID_ANY, _L("正在准备切片预览…"));
        RedesignTheme::style_text(message, RedesignTheme::primary_text_colour(), 12);
        root->Add(message, 0, wxALIGN_CENTER);
        root->AddStretchSpacer();
        m_preview_transition_cover->SetSizer(root);
        m_preview_transition_timer.SetOwner(this, wxWindow::NewControlId());
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (m_preview_transition) check_preview_first_frame(*m_preview_transition);
        }, m_preview_transition_timer.GetId());
    }
    m_preview_frame_gate.begin(*m_preview_transition, plate->get_slice_result()->id,
        std::chrono::steady_clock::now());
    refresh_print_preparation_action();
    if (m_slice_print && m_slice_print->IsShownOnScreen()) m_slice_print->Update();
    layout_preview_transition_cover();
    m_preview_transition_cover->Update();
    m_preview_transition_timer.Start(50);
    BOOST_LOG_TRIVIAL(info) << "[RedesignPreview] staged token=" << *m_preview_transition
        << " result=" << plate->get_slice_result()->id;
    return true;
}

void RedesignShell::cancel_preview_presentation()
{
    if (!m_preview_transition) return;
    m_preview_transition_timer.Stop();
    m_page_coordinator.transition().rollback(*m_preview_transition);
    m_preview_transition.reset();
    m_preview_frame_gate.finish();
    if (m_preview_transition_cover) m_preview_transition_cover->Hide();
    refresh_print_preparation_action();
}

void RedesignShell::layout_preview_transition_cover()
{
    if (!m_preview_transition || !m_preview_transition_cover) return;
    auto* owner = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
    auto* canvas = m_plater ? m_plater->get_preview_canvas3D()->get_wxglcanvas() : nullptr;
    const bool paused = !owner || owner->IsIconized() || !owner->IsShown();
    m_preview_frame_gate.set_paused(paused, std::chrono::steady_clock::now());
    const auto bounds = !paused && canvas && canvas->IsShownOnScreen()
        ? preview_cover_bounds(wxRect(canvas->ClientToScreen(wxPoint()), canvas->GetClientSize()),
            wxRect(owner->ClientToScreen(wxPoint()), owner->GetClientSize())) : std::nullopt;
    if (!bounds) { m_preview_transition_cover->Hide(); return; }
    if (m_preview_transition_cover->GetRect() != *bounds) {
        m_preview_transition_cover->SetSize(*bounds);
        m_preview_transition_cover->Layout();
    }
    if (!m_preview_transition_cover->IsShown()) m_preview_transition_cover->ShowWithoutActivating();
}

void RedesignShell::check_preview_first_frame(RedesignPageTransition::Token token)
{
    if (!m_preview_transition || *m_preview_transition != token || !m_page_coordinator.transition().current(token) || !m_plater)
        return;
    layout_preview_transition_cover();
    auto* owner = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
    if (!owner || owner->IsIconized() || !owner->IsShown()) return;
    auto* canvas = m_plater->get_preview_canvas3D();
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    const auto size = canvas->get_canvas_size();
    const auto& camera = m_plater->get_camera();
    const auto& viewport = camera.get_viewport();
    const auto& viewer = canvas->get_gcode_viewer();
    const bool viewport_ready = canvas->is_initialized() && !canvas->is_reload_delayed() &&
        size.get_width() > 0 && size.get_height() > 0 &&
        viewport[2] == int(size.get_width()) && viewport[3] == int(size.get_height()) &&
        !camera.requires_zoom_to_bed && !camera.requires_zoom_to_volumes &&
        camera.requires_zoom_to_plate <= REQUIRES_ZOOM_TO_PLATE_IDLE;
    // Readiness is driven by both idle and the existing polling timer. A busy
    // event queue must not leave fitting pending until the frame gate expires.
    if (m_preview_auto_fit.consume(viewer.loaded_result_id(), m_plater->is_preview_loaded(),
            viewer.has_data() && viewer.get_paths_bounding_box().defined, viewport_ready)) {
        canvas->zoom_to_gcode();
        m_preview_frame_gate.invalidate_frame();
        BOOST_LOG_TRIVIAL(info) << "[RedesignPreview] auto-fit result=" << viewer.loaded_result_id()
            << " viewport=" << viewport[2] << "x" << viewport[3];
        request_viewport_frame();
        return;
    }
    const bool loaded = plate && plate->is_slice_result_valid() && plate->get_slice_result() &&
        plate->get_slice_result()->id == viewer.loaded_result_id() && m_plater->is_preview_loaded() &&
        viewer.has_data() && viewer.get_paths_bounding_box().defined && !m_preview_auto_fit.pending();
    const auto status = m_preview_frame_gate.observe(token, viewer.loaded_result_id(), loaded, viewport_ready,
        int(size.get_width()), int(size.get_height()), canvas->rendered_frames(), std::chrono::steady_clock::now());
    if (status == RedesignPreviewFrameGate::Status::TimedOut)
        BOOST_LOG_TRIVIAL(info) << "[RedesignPreview] not ready result=" << viewer.loaded_result_id()
            << " loaded=" << loaded << " viewport=" << viewport_ready << " fit_pending=" << m_preview_auto_fit.pending();
    if (status == RedesignPreviewFrameGate::Status::Ready || status == RedesignPreviewFrameGate::Status::TimedOut) {
        finish_preview_transition(token, status == RedesignPreviewFrameGate::Status::Ready);
        return;
    }
    // This is readiness polling, not a timed reveal. Paint/idle callbacks can
    // finish immediately; the timer also recovers a stalled or hidden canvas.
    request_viewport_frame();
}

void RedesignShell::finish_preview_transition(RedesignPageTransition::Token token, bool ready)
{
    if (!m_preview_transition || *m_preview_transition != token || !m_page_coordinator.transition().current(token)) return;
    m_preview_transition_timer.Stop();
    m_preview_frame_gate.finish();
    m_preview_transition.reset();
    BOOST_LOG_TRIVIAL(info) << "[RedesignPreview] " << (ready ? "presented" : "timeout") << " token=" << token
        << " frame=" << m_plater->get_preview_canvas3D()->rendered_frames();
    if (ready) {
        m_page_coordinator.transition().mark_ready(token);
        m_page_coordinator.transition().commit(token);
        if (m_model_generation_host) m_model_generation_host->set_workbench_presented(false);
    } else {
        m_page_coordinator.transition().rollback(token);
    }
    m_preview_transition_cover->Hide();
    refresh_print_preparation_action();
    if (ready) {
        restore_active_page_focus();
        return;
    }
    show_model_view(ModelView::Slicing);
    wxWeakRef<RedesignShell> weak(this);
    CallAfter([weak] {
        if (!weak) return;
        RedesignConfirmationOptions options;
        options.actions = {{wxID_YES, _L("重试切片预览"), true}, {wxID_NO, _L("留在切片设置"), false}};
        options.dismiss_result = wxID_NO;
        if (show_redesign_confirmation(weak.get(), _L("G-code 已保留，但预览尚未完成首帧。可重试预览，无需重新切片。"),
                _L("切片预览"), options) == wxID_YES && weak)
            weak->show_model_view(ModelView::Preview);
    });
}

void RedesignShell::request_viewport_frame()
{
    if (m_workbench_transition && !m_workbench_presentation_detached) {
        layout_workbench_transition_cover();
        return;
    }
    auto* owner = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
    if (!owner || owner->IsIconized() || !IsShownOnScreen()) return;
    layout_preview_transition_cover();
    if (m_page_coordinator.page() == Page::Model && m_page_coordinator.view() == ModelView::Workbench && m_model_generation_host) {
        // The workbench owns its overlay coordinates and one coalesced native
        // refresh. Revalidate them even when restore keeps the previous size.
        m_model_generation_host->set_workbench_presented(true);
    } else if ((native_workspace_visible() || m_print_canvas_mounted) && m_plater) {
        auto* canvas = m_plater->get_current_canvas3D();
        const auto size = canvas->get_canvas_size();
        if (size.get_width() <= 0 || size.get_height() <= 0) return;
        canvas->set_as_dirty();
        canvas->get_wxglcanvas()->Refresh(false);
    } else if (auto* root = active_page_focus_root()) {
        root->Refresh(false);
        queue_creation_history_refresh();
    }
}

void RedesignShell::present_restore_frame()
{
    Presentation::assert_gui_thread();
    if (m_workbench_transition && !m_workbench_presentation_detached) {
        layout_workbench_transition_cover();
        if (m_model_generation_host) m_model_generation_host->present_workbench_restore_frame();
        return;
    }
    if (m_import_in_progress && !m_import_navigation_detached) {
        layout_import_transition_cover();
        layout_import_loading();
        if (m_import_target_staged) {
            auto* canvas = m_plater->get_view3D_canvas3D();
            m_import_frame_gate.reset(canvas->rendered_frames());
            ++m_import_layout_generation;
            request_viewport_frame();
        }
    }
    using Presentation::Dirty;
    m_surface_refresh.flush(Dirty::Visibility | Dirty::Layout | Dirty::Overlay | Dirty::Viewport | Dirty::Paint);
    m_creation_history_refresh.flush(Dirty::Layout | Dirty::Overlay | Dirty::Paint);
}

void RedesignShell::refresh_workflow_layout()
{
    update_navigation();
    refresh_inline_generation();
    const bool presenting_workbench = m_workbench_transition && !m_workbench_presentation_detached;
    sync_page_settings_visibility(presenting_workbench ? Page::Model : m_page_coordinator.page());
    if (!owns_model_workflow()) return;
    const bool model = m_page_coordinator.page() == Page::Model;
    const bool slicing = model && (m_page_coordinator.view() == ModelView::Slicing || m_page_coordinator.view() == ModelView::Preview);
    const bool render_print = m_page_coordinator.page() == Page::Print && m_print_canvas_mounted;
    m_workbench_page->Show(presenting_workbench || (model && m_page_coordinator.view() == ModelView::Workbench));
    m_slicing_page->Show(slicing);
    m_plater->Show(slicing || render_print);
    m_slicing_settings->Show();
    m_plater->get_current_canvas3D()->enable_render(slicing || render_print);
    if (slicing || render_print) m_plater->get_current_canvas3D()->set_as_dirty();
    m_slice_export->Hide();
    m_slice_print->Show();
    m_slice_start->Show();
    m_return_slice->Hide();
    if (m_slicing_host) m_slicing_host->set_workbench_active(slicing && !m_import_target_staged, !m_native_slicing);
    const bool compact = render_print || slicing;
    if (compact && !m_plater->is_sidebar_collapsed()) m_plater->collapse_sidebar(true);
    m_plater->get_view3D_canvas3D()->use_compact_slicing_tools(compact);
    m_plater->get_preview_canvas3D()->use_compact_slicing_tools(compact);
    // Keep the native main toolbar and sidebar hidden, while exposing the
    // model-editing Gizmo row and its numeric transform panel in AI slicing.
    m_plater->get_view3D_canvas3D()->set_slicing_gizmos_visible(compact && !render_print);
    m_plater->get_preview_canvas3D()->set_slicing_gizmos_visible(compact && !render_print);
    m_plater->get_view3D_canvas3D()->suppress_gcode_window(compact);
    m_plater->get_preview_canvas3D()->suppress_gcode_window(compact);
    if (compact && !m_saved_collapse_toolbar) {
        m_saved_collapse_toolbar = m_plater->get_collapse_toolbar().is_enabled();
        m_plater->get_collapse_toolbar().set_enabled(false);
    } else if (!compact && m_saved_collapse_toolbar) {
        m_plater->get_collapse_toolbar().set_enabled(*m_saved_collapse_toolbar); m_saved_collapse_toolbar.reset();
    }
    mount_custom_process(slicing && m_native_slicing);
    if (!m_import_target_staged)
        if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this)))
            if (frame->topbar()) frame->topbar()->SetSlicingTitlebarMode(compact, false);
    m_slice_return_overview->Enable(slicing_mode_switch_allowed());
    if (m_native_return_ai) {
        const bool can_switch = slicing_mode_switch_allowed();
        m_native_return_ai->Enable(can_switch);
        m_native_return_overview->Enable(can_switch);
        static_cast<WorkbenchButton*>(m_native_prepare)->SetTextColor(StateColor(m_page_coordinator.view() == ModelView::Slicing
            ? RedesignTheme::accent_colour() : RedesignTheme::primary_text_colour()));
        static_cast<WorkbenchButton*>(m_native_preview)->SetTextColor(StateColor(m_page_coordinator.view() == ModelView::Preview
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
    const bool slicing_model_visible = slicing && m_page_coordinator.view() == ModelView::Slicing &&
        !m_plater->model().objects.empty();
    if (slicing_model_visible && (!m_slice_camera_fitted || m_slice_camera_waiting_for_frame)) {
        if (!m_slice_camera_loading_pending) {
            m_slice_camera_loading_pending = true;
            m_slice_camera_waiting_for_frame = false;
            m_slice_camera_retry_attempts = 0;
        }
        // The import overlay owns the canvas while the model is still being
        // committed. Start the camera retry after it is dismissed.
        if (!m_import_in_progress && !m_slice_camera_retry_timer.IsRunning())
            m_slice_camera_retry_timer.Start(100);
    } else if (!slicing_model_visible) {
        m_slice_camera_loading_pending = false;
        m_slice_camera_waiting_for_frame = false;
        m_slice_camera_retry_timer.Stop();
    }
    if (m_import_in_progress && m_import_loading) {
        layout_import_loading();
        if (m_import_loading->IsEnabled()) m_import_loading->Raise();
    }
    layout_slicing_camera_loading();
}

void RedesignShell::present_active_page(bool flush_children)
{
    if (m_presentation_stopping) return;
    Presentation::assert_gui_thread();
    m_page_coordinator.prepare_current();
    using Presentation::Dirty;
    auto flags = Dirty::Content | Dirty::Layout | Dirty::Overlay | Dirty::Paint | Dirty::Viewport;
    if (flush_children) flags = flags | Dirty::Barrier;
    m_surface_refresh.request(flags);
}

void RedesignShell::commit_active_page(Presentation::Dirty flags)
{
    Presentation::assert_gui_thread();
    auto* root = active_page_focus_root();
    if (!root || !root->IsShownOnScreen()) return;
    if (Presentation::has(flags, Presentation::Dirty::Layout)) root->Layout();
    if (native_workspace_visible() && (m_slicing_dirty.dirty() ||
        Presentation::has(flags, Presentation::Dirty::Content) || Presentation::has(flags, Presentation::Dirty::Barrier))) {
        // Retain hidden snapshots without touching their controls. A final
        // native layout and its floating progress surface share this commit.
        const auto epoch = m_page_coordinator.epoch();
        const auto dirty = m_slicing_dirty.take();
        if (Presentation::has(dirty, Presentation::Dirty::Content) ||
            Presentation::has(flags, Presentation::Dirty::Content) ||
            Presentation::has(flags, Presentation::Dirty::Layout) || Presentation::has(flags, Presentation::Dirty::Barrier))
            apply_slicing_snapshot();
        else if (m_slice_progress_overlay)
            m_slice_progress_overlay->update(m_slice_progress_stage, m_slice_progress_percent);
        if (epoch != m_page_coordinator.epoch()) return;
    }
    if (m_page_coordinator.page() == Page::Model &&
        m_page_coordinator.view() == ModelView::Workbench && m_model_generation_host) {
        // This subtree has one presenter, including restore and first frames.
        m_model_generation_host->set_workbench_presented(true);
        if (Presentation::has(flags, Presentation::Dirty::Barrier))
            m_model_generation_host->present_workbench_restore_frame();
    } else {
        const bool paint_surface = !native_workspace_visible() || slicing_surface_paint_required(flags);
        if (paint_surface) root->Refresh(false);
        if (paint_surface && m_image_settings_panel && m_image_settings_panel->IsShownOnScreen())
            m_image_settings_panel->Refresh(false);
        // A synchronous native/GL barrier is reserved for an existing import
        // or first-frame transition, never an ordinary status update.
        wxWindow* viewport = nullptr;
        if (native_workspace_visible() && m_plater)
            viewport = m_plater->get_current_canvas3D()->get_wxglcanvas();
        if (Presentation::has(flags, Presentation::Dirty::Barrier)) {
#ifdef __WXMSW__
            if (viewport) ::ValidateRect(viewport->GetHandle(), nullptr);
            ::RedrawWindow(root->GetHandle(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
#else
            root->Update();
#endif
        }
        if (viewport && viewport->IsShownOnScreen() &&
            (Presentation::has(flags, Presentation::Dirty::Viewport) || Presentation::has(flags, Presentation::Dirty::Barrier))) {
            viewport->Refresh(false);
            if (Presentation::has(flags, Presentation::Dirty::Barrier)) viewport->Update();
        }
    }
    if (!m_import_target_staged) m_page_coordinator.presented_current();
}

void RedesignShell::apply_workbench_state(const PostGenerationWorkbenchState& state)
{
    if (m_presentation_stopping) return;
    m_workbench_state = state;
    refresh_project_save_actions();
    if (m_image_page_view_only)
        apply_model_generation_state(m_model_generation_state);
    if (m_advanced_workspace && m_model_generation_host && m_model_generation_host->creation_editing_locked())
        m_advanced_workspace->render();
    if (!m_pending_workbench_job.empty() && state.asset_id == m_pending_workbench_job && state.actions.can_edit) {
        m_pending_workbench_job.clear();
        // A completed hidden operation updates its model, never the user's route.
    }
}

bool RedesignShell::can_return_to_image_design() const
{
    if (m_workbench_transition || m_preview_transition || m_import_in_progress ||
        (m_plater && m_plater->is_background_process_slicing()) || (m_slice_tools && m_slice_tools->busy())) return false;
    if (m_page_coordinator.page() == Page::Model && m_page_coordinator.view() == ModelView::Workbench)
        return !m_workbench_state.modal_blocking && !m_import_in_progress &&
            post_generation_return_to_design_allowed(m_workbench_state.actions);
    return model_generation_return_to_design_allowed(
        m_page_coordinator.advanced() ? m_advanced_model_state : m_model_generation_state,
        m_workbench_state.actions, m_import_in_progress || m_submit_in_progress);
}

bool RedesignShell::return_to_image_design()
{
    const bool from_editor = m_page_coordinator.page() == Page::Model;
    const bool return_advanced = m_model_generation_host && m_model_generation_host->creation_editing_locked() ?
        m_model_generation_host->workbench_returns_to_advanced() : from_editor && m_model_generation_host ?
        m_model_generation_host->workbench_returns_to_advanced() : m_page_coordinator.advanced();
    if (block_workbench_navigation()) return false;
    if (!can_return_to_image_design())
        return false;

    if (m_workbench_state.dirty || !m_workbench_state.candidate_path.empty()) {
        if (show_redesign_confirmation(this,
                m_smart_creation ? _L("当前 3D 工作台存在未保存修改或尚未接受的候选。返回AI创作后，如果开始新的生成任务，这些修改可能无法继续恢复。仍要返回吗？") :
                _L("当前 3D 工作台存在未保存修改或尚未接受的候选。返回图像设计后，如果开始新的生成任务，这些修改可能无法继续恢复。仍要返回吗？"),
                m_smart_creation ? _L("返回AI创作") : _L("返回图像设计"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES)
            return false;
    }

    // The lock anchors the source mode. A return command alone may change it.
    if (return_advanced != m_page_coordinator.advanced()) {
        m_page_coordinator.select_mode(return_advanced);
    }
    if (return_advanced != m_page_coordinator.advanced()) return false;

    const bool view_only = m_image_page_view_only;
    const auto saved_input = m_saved_creation_input;
    const auto saved_options = m_saved_creation_options;
    if (!return_advanced) {
        m_saved_creation_input.reset();
        m_saved_creation_options.reset();
    }
    m_image_page_view_only = false;
    if (!navigate_to(Page::Image)) {
        m_image_page_view_only = view_only;
        m_saved_creation_input = saved_input;
        m_saved_creation_options = saved_options;
        return false;
    }
    if (m_model_generation_host) m_model_generation_host->restore_creation_editing();
    if (saved_input && saved_options && m_model_generation_host) {
        // Existing-asset navigation never replaces the creation draft. Returning
        // synchronizes editable inputs only; it does not submit or restart a job.
        restore_image_input(*saved_input);
        m_model_generation_host->synchronize_input(*saved_input);
        m_model_generation_host->synchronize_options(*saved_options);
        apply_model_generation_state(m_model_generation_host->snapshot());
    }
    // A missing historical style becomes a valid default only when the user
    // explicitly returns to editing. This never restarts or submits a task.
    if (current_generation_input().style != m_model_generation_state.input.style)
        synchronize_generation_input();
    apply_model_generation_state(m_model_generation_state);
    return true;
}

bool RedesignShell::begin_workbench_import(WorkbenchImportPreparation prepare)
{
    if (m_import_in_progress || m_presentation_stopping || !m_model_generation_host || !owns_model_workflow() ||
        m_page_coordinator.page() != Page::Model || m_page_coordinator.view() != ModelView::Workbench)
        return false;

    if (retry_import_target_view()) return true;
    if (!begin_import_presentation(WorkbenchImportPhase::CheckingFilaments)) return false;
    m_import_project_committed = false;
    m_import_committed_object_id = 0;
    m_import_committed_model_path.clear();
    m_import_committed_model_sha256.clear();
    const auto session = m_import_session;
    const auto token = *m_import_transition;
    wxWeakRef<RedesignShell> weak(this);
    try {
        if (prepare(session, [weak, session, token](std::optional<AI::ModelImportRequest> request) {
            if (!weak || !session->valid() || weak->m_import_session != session || weak->m_import_transition != token)
                return;
            if (session->cancelled() || !request || !weak->m_page_coordinator.transition().current(token))
                weak->finish_import_loading();
            else
                weak->confirm_workbench_import(*request);
        })) return true;
    } catch (const std::exception& error) {
        finish_import_loading();
        wxMessageBox(wxString::FromUTF8(error.what()), _L("模型导入"), wxOK | wxICON_ERROR, this);
        return false;
    }
    finish_import_loading();
    return false;
}

bool RedesignShell::begin_import_presentation(WorkbenchImportPhase phase)
{
    const auto transition = m_page_coordinator.transition().begin(
        {int(m_page_coordinator.page()), int(m_page_coordinator.view())}, {int(Page::Model), int(ModelView::Slicing)});
    if (!transition) return false;
    remember_active_page_focus();
    m_import_transition = *transition;
    m_import_navigation_detached = false;
    m_import_in_progress = true;
    m_import_session = std::make_shared<WorkbenchImportSession>(phase);
    m_import_started = std::chrono::steady_clock::now();
    retain_import_source_page();
    update_import_loading(phase);
    show_import_loading();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] token=" << *transition << " retained=" << m_import_source_retained
        << " feedback_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_import_started).count();
    return true;
}

void RedesignShell::confirm_workbench_import(const AI::ModelImportRequest& request)
{
    if (!m_import_session || m_import_session->phase() != WorkbenchImportPhase::CheckingFilaments) return;
    // A view snapshot may still contain the busy state from asynchronous colour
    // preparation. Admission belongs to the live workbench, not the cached UI.
    const auto current = current_workbench_import_source(request,
        [this] { return m_model_generation_host->workbench_snapshot(); });
    if (!current) {
        BOOST_LOG_TRIVIAL(warning) << "[WorkbenchImport] rejected stage=admission asset=" << request.artifact.job_id;
        finish_import_loading();
        return;
    }
    m_workbench_state = *current;
    m_import_session->advance(WorkbenchImportPhase::Reading);
    update_import_loading(WorkbenchImportPhase::Reading);
    wxWeakRef<RedesignShell> weak(this);
    const auto session = m_import_session;
    const auto token = *m_import_transition;
    CallAfter([weak, session, request, token] {
        if (!weak || !session->valid() || weak->m_import_session != session || weak->m_import_transition != token) return;
        if (session->cancelled() || !weak->m_page_coordinator.transition().current(token)) {
            weak->finish_import_loading();
            return;
        }
        try {
            const bool started = weak->m_model_generation_host && weak->m_model_generation_host->import_workbench_model_async(request, session,
                [weak, session, token](auto phase, const auto& message) {
                    if (weak && session->valid() && weak->m_import_session == session && weak->m_import_transition == token)
                        weak->update_import_loading(phase, message);
                },
                [weak, session, request, token](const auto& result) {
                    if (!weak || !session->valid() || weak->m_import_session != session || weak->m_import_transition != token) return;
                    if (!result.imported()) {
                        weak->finish_import_loading();
                        if (!weak->m_import_navigation_detached && result.outcome != AI::ModelImportOutcome::Cancelled)
                            wxMessageBox(wxString::FromUTF8(result.error.empty() ? "模型导入失败，工作副本已保留。" : result.error),
                                _L("模型导入"), wxOK | wxICON_ERROR, weak.get());
                        return;
                    }
                    weak->m_import_project_committed = true;
                    weak->m_import_committed_object_id = weak->m_plater->model().objects.back()->id().id;
                    weak->m_import_committed_model_path = request.artifact.local_path;
                    weak->m_import_committed_model_sha256 = request.topology_check_sha256;
                    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] project committed token=" << token;
                    weak->update_import_loading(WorkbenchImportPhase::UpdatingView);
                    weak->request_initial_slicing_camera();
                    weak->stage_import_target_view();
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

void RedesignShell::show_import_loading()
{
    const auto* owner = wxGetTopLevelParent(this);
    const wxRect owner_bounds(owner->ClientToScreen(wxPoint()), owner->GetClientSize());
    const auto* viewport = m_model_generation_host->workbench_import_viewport();
    const auto viewport_bounds = viewport && viewport->IsShownOnScreen() ? viewport->GetScreenRect() : wxRect{};
    m_import_loading_anchor = WorkbenchLoadingCardAnchor(viewport_bounds, workbench_transition_bounds(), owner_bounds);
    m_model_generation_host->set_workbench_import_feedback(true);
    if (m_import_elapsed->GetLabel() != _L("已用时 0.0 秒"))
        m_import_elapsed->SetLabel(_L("已用时 0.0 秒"));
    if (m_import_activity && !m_import_activity->IsRunning()) m_import_activity->Start();
    layout_import_loading();
    m_import_loading->Raise();
    m_import_loading->Update();
    m_import_timer.Start(100);
}

void RedesignShell::update_import_loading(WorkbenchImportPhase phase, const std::string& message)
{
    if (!m_import_in_progress) return;
    wxString label;
    switch (phase) {
    case WorkbenchImportPhase::CheckingFilaments: label = _L("正在核对耗材配色…"); break;
    case WorkbenchImportPhase::Reading: label = _L("正在读取与验证模型…"); break;
    case WorkbenchImportPhase::Colors: label = _L("正在准备配色…"); break;
    case WorkbenchImportPhase::Placement: label = _L("正在校验模型摆放…"); break;
    case WorkbenchImportPhase::Committing: label = _L("正在加入工程…"); break;
    case WorkbenchImportPhase::UpdatingView: label = _L("正在更新模型视口…"); break;
    default: break;
    }
    if (!message.empty()) label = wxString::FromUTF8(message);
    bool layout_changed = false;
    if (!label.empty() && m_import_stage->GetLabel() != label) {
        m_import_stage->SetLabel(label);
        layout_changed = true;
    }
    const bool can_cancel = m_import_session && m_import_session->can_cancel();
    if (m_import_cancel->IsEnabled() != can_cancel) m_import_cancel->Enable(can_cancel);
    if (layout_changed) m_import_loading->Layout();
    m_import_loading->Refresh(false);
    if (phase == WorkbenchImportPhase::Committing) m_import_loading->Update();
}

void RedesignShell::layout_import_loading()
{
    if (!m_import_in_progress || !m_import_loading) return;
    const auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (m_import_navigation_detached || !owner || owner->IsIconized() || !IsShownOnScreen()) {
        m_import_loading->Hide();
        return;
    }
    const wxRect owner_bounds(owner->ClientToScreen(wxPoint()), owner->GetClientSize());
    wxSize requested = m_import_loading->GetBestSize();
    if (requested.x <= 0 || requested.y <= 0) requested = FromDIP(wxSize(410, 184));
    const wxRect bounds = m_import_loading_anchor.bounds(owner_bounds, requested);
    if (bounds.IsEmpty()) { m_import_loading->Hide(); return; }
    if (m_import_loading->GetRect() != bounds) {
        m_import_loading->SetSize(bounds);
        m_import_loading->Layout();
        apply_workbench_floating_card_shape(m_import_loading, 8, wxColour(72, 72, 78));
    }
    if (!m_import_loading->IsShown()) m_import_loading->ShowWithoutActivating();
    // A native colour-review dialog may be modal while this transaction is
    // still active. wxDialog disables sibling top-level windows in that case;
    // do not repeatedly raise this disabled card over the dialog the user must
    // review. It resumes its normal z-order when the modal window closes.
    if (m_import_loading->IsEnabled()) m_import_loading->Raise();
}

void RedesignShell::stage_import_target_view()
{
    if (m_import_navigation_detached) { finish_import_loading(true); return; }
    if (!m_import_in_progress || !m_import_project_committed || m_import_target_staged ||
        !m_import_transition || !m_page_coordinator.transition().current(*m_import_transition) ||
        !m_plater || !m_slicing_page || !m_workbench_page) return;
    const auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    if (!owner || owner->IsIconized() || !IsShownOnScreen()) return;

    // Lay out the final route behind the retained frame. The workbench and its
    // GL canvas stay under their original parent for the entire handoff.
    m_import_target_staged = true;
    layout_import_transition_cover();
    layout_import_loading();
    {
        wxWindowUpdateLocker layout_lock(m_content_host);
        m_page_coordinator.select_view(ModelView::Slicing);
        m_plater->select_view_3D("3D");
        auto* canvas = m_plater->get_current_canvas3D();
        canvas->use_workbench_appearance();
        refresh_workflow_layout();
        if (m_slicing_host) apply_slicing_state(m_slicing_host->workbench_snapshot());
    }
    auto* canvas = m_plater->get_view3D_canvas3D();
    m_import_frame_gate.reset(canvas->rendered_frames());
    m_import_frame_paused = false;
    m_import_presentation_deadline.begin(RedesignPresentationDeadline::Clock::now());
    canvas->set_as_dirty();
    present_active_page(true);
    m_surface_refresh.flush();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImportView] staged token=" << *m_import_transition
        << " frame=" << canvas->rendered_frames() << " cover=" << m_import_transition_cover->IsShown();
}

void RedesignShell::layout_import_transition_cover()
{
    if (m_import_navigation_detached) {
        if (m_import_transition_cover) m_import_transition_cover->Hide();
        return;
    }
    if ((!m_import_source_retained && !m_import_target_staged) || !m_import_transition_cover || !m_content_host) return;
    auto* owner = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(this));
    if (!owner || owner->IsIconized() || !m_content_host->IsShownOnScreen()) {
        m_import_transition_cover->Hide();
        if (m_import_loading) m_import_loading->Hide();
        return;
    }
    const wxRect bounds(m_content_host->ClientToScreen(wxPoint(0, 0)), m_content_host->GetClientSize());
    if (bounds.IsEmpty()) return;
    const bool resized = m_import_transition_cover->GetSize() != bounds.GetSize();
    const bool showing = !m_import_transition_cover->IsShown();
    if (m_import_transition_cover->GetRect() != bounds) m_import_transition_cover->SetSize(bounds);
    if (showing) m_import_transition_cover->ShowWithoutActivating();
    if (showing || resized) {
        m_import_transition_cover->Refresh(false);
        m_import_transition_cover->Update();
    }
}

void RedesignShell::rollback_import_target_view()
{
    if (!m_import_target_staged) return;
    wxWindowUpdateLocker lock(m_content_host);
    if (m_slicing_page) m_slicing_page->Hide();
    if (m_plater) {
        m_plater->Hide();
        m_plater->get_current_canvas3D()->enable_render(false);
    }
    if (m_import_transition) {
        const auto source = m_page_coordinator.transition().source();
        m_page_coordinator.select_page(static_cast<Page>(source.page));
        m_page_coordinator.select_view(static_cast<ModelView>(source.view));
        m_active_tab_id = TAB_ID_GENERATE_3D;
    }
    m_import_target_staged = false;
    if (m_workbench_page && m_page_coordinator.page() == Page::Model && m_page_coordinator.view() == ModelView::Workbench) {
        m_workbench_page->Show();
        m_workbench_page->Raise();
    }
    m_content_host->Layout();
}

void RedesignShell::commit_import_target_view()
{
    if (!m_import_transition || !m_page_coordinator.transition().current(*m_import_transition)) {
        finish_import_loading(true);
        return;
    }

    const auto token = *m_import_transition;
    if (!m_page_coordinator.transition().mark_ready(token) || !m_page_coordinator.transition().commit(token)) {
        finish_import_loading(true);
        return;
    }
    m_import_timer.Stop();
    m_import_in_progress = false;
    m_import_target_staged = false;
    m_import_transition.reset();
    if (m_import_session) m_import_session->advance(WorkbenchImportPhase::Completed);
    m_import_session.reset();
    m_import_committed_object_id = 0;
    m_import_project_committed = false;
    m_import_committed_model_path.clear();
    m_import_committed_model_sha256.clear();
    m_slice_camera_loading_pending = false;
    m_last_workbench_page = Page::Model;
    m_active_tab_id = TAB_ID_PREPARE;
    if (m_import_activity) m_import_activity->Stop();
    m_model_generation_host->set_workbench_presented(false);
    m_model_generation_host->set_workbench_import_feedback(false);
    if (auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this))) {
        if (auto* topbar = frame->topbar()) topbar->SetSlicingTitlebarMode(true, false);
    }
    // Apply the released controls and paint while the source is still covered.
    // The queued scheduler callback is consumed by this presentation barrier.
    present_active_page(true);
    m_surface_refresh.flush();
#ifdef __WXMSW__
    // Submit both owned windows' visibility changes together. Hiding them
    // separately gives the compositor a frame of the source without progress.
    if (m_import_transition_cover && m_import_transition_cover->IsShown()) {
        constexpr UINT flags = SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
        auto handoff = ::BeginDeferWindowPos(2);
        if (handoff)
            handoff = ::DeferWindowPos(handoff, m_import_loading->GetHandle(), nullptr, 0, 0, 0, 0, flags);
        if (handoff)
            handoff = ::DeferWindowPos(handoff, m_import_transition_cover->GetHandle(), nullptr, 0, 0, 0, 0, flags);
        if (handoff) ::EndDeferWindowPos(handoff);
    }
#endif
    m_import_loading->Hide();
    release_import_source_page();
    restore_active_page_focus();
    if (m_slicing_host) m_slicing_host->set_workbench_active(true, !m_native_slicing);
    const auto size = m_plater->get_view3D_canvas3D()->get_canvas_size();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImportView] revealed token=" << token
        << " frame=" << m_plater->get_view3D_canvas3D()->rendered_frames()
        << " viewport=" << size.get_width() << "x" << size.get_height();
}

bool RedesignShell::retry_import_target_view()
{
    if (m_import_in_progress || m_import_committed_object_id == 0 || !m_plater ||
        m_page_coordinator.page() != Page::Model || m_page_coordinator.view() != ModelView::Workbench) return false;
    const auto current = m_model_generation_host->workbench_snapshot();
    const auto& objects = m_plater->model().objects;
    if (current.model_path != m_import_committed_model_path.string() ||
        current.check.model_sha256 != m_import_committed_model_sha256 ||
        std::none_of(objects.begin(), objects.end(), [this](const auto* object) {
            return object && object->id().id == m_import_committed_object_id;
        }) || !begin_import_presentation(WorkbenchImportPhase::UpdatingView)) return false;
    m_import_project_committed = true;
    update_import_loading(WorkbenchImportPhase::UpdatingView,
        "正在重新准备切片视图；不会重复导入模型……");
    request_initial_slicing_camera();
    stage_import_target_view();
    return true;
}

void RedesignShell::finish_import_loading(bool retain_view_retry)
{
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] total_ms=" <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_import_started).count();
    m_import_timer.Stop();
    rollback_import_target_view();
    m_import_in_progress = false;
    if (m_import_project_committed && m_import_session)
        m_import_session->advance(WorkbenchImportPhase::Completed);
    m_import_session.reset();
    if (m_import_activity) m_import_activity->Stop();
    m_import_loading->Hide();
    m_model_generation_host->restore_workbench_import_history();
    m_model_generation_host->set_workbench_import_feedback(false);
    if (m_import_transition) m_page_coordinator.transition().rollback(*m_import_transition);
    m_import_transition.reset();
    m_import_project_committed = false;
    if (!retain_view_retry) {
        m_import_committed_object_id = 0;
        m_import_committed_model_path.clear();
        m_import_committed_model_sha256.clear();
    }
    {
        wxWindowUpdateLocker lock(this);
        refresh_workflow_layout();
    }
    if (m_slicing_host) apply_slicing_state(m_slicing_host->workbench_snapshot());
    if (!m_import_navigation_detached && m_page_coordinator.page() == Page::Model &&
        m_page_coordinator.view() == ModelView::Workbench) {
        present_active_page(true);
        m_surface_refresh.flush();
    }
    release_import_source_page();
    if (!m_import_navigation_detached) restore_active_page_focus();
}

void RedesignShell::check_import_first_frame(RedesignPageTransition::Token token)
{
    if (!m_import_in_progress || !m_import_target_staged || !m_plater || m_import_transition != token ||
        !m_page_coordinator.transition().current(token)) return;
    auto* canvas = m_plater->get_view3D_canvas3D();
    const auto* owner = wxDynamicCast(wxGetTopLevelParent(this), wxTopLevelWindow);
    const bool paused = !owner || owner->IsIconized() || !IsShownOnScreen();
    const bool expired = m_import_presentation_deadline.observe(RedesignPresentationDeadline::Clock::now(), paused);
    if (paused || m_import_frame_paused) m_import_frame_gate.reset(canvas->rendered_frames());
    m_import_frame_paused = paused;
    if (paused) return;

    const auto canvas_size = canvas->get_canvas_size();
    const wxSize viewport(canvas_size.get_width(), canvas_size.get_height());
    const auto& camera = m_plater->get_camera();
    const auto& rendered_viewport = camera.get_viewport();
    const bool layout_ready = m_plater->get_current_canvas3D() == canvas && canvas->is_initialized() &&
        canvas->get_wxglcanvas()->IsShownOnScreen() && viewport.x > 0 && viewport.y > 0 &&
        rendered_viewport[2] == viewport.x && rendered_viewport[3] == viewport.y &&
        !canvas->is_reload_delayed() && !camera.requires_zoom_to_bed && !camera.requires_zoom_to_volumes &&
        camera.requires_zoom_to_plate <= REQUIRES_ZOOM_TO_PLATE_IDLE;
    const Presentation::PresentationStamp layout {m_page_coordinator.epoch(), token, m_import_layout_generation,
        viewport.x, viewport.y, canvas->get_wxglcanvas()->GetDPI().x};
    using Status = WorkbenchImportFrameGate::Status;
    const auto status = m_import_frame_gate.observe(layout, layout_ready, canvas->rendered_frames());
    if (status == Status::Ready) {
        BOOST_LOG_TRIVIAL(info) << "[WorkbenchImportView] fitted frame token=" << token
            << " frame=" << canvas->rendered_frames() << " viewport=" << viewport.x << "x" << viewport.y
            << " dpi=" << layout.dpi;
        commit_import_target_view();
        return;
    }
    if (status == Status::FitCamera && fit_slicing_camera(*m_plater)) {
        m_slice_camera_fitted = true;
        m_import_frame_gate.camera_fitted(layout, canvas->rendered_frames());
    }
    if (expired) {
        finish_import_loading(true);
        RedesignConfirmationOptions options;
        options.actions = {{wxID_YES, _L("进入切片重试"), true},
                           {wxID_NO, _L("留在工作台"), false}};
        options.dismiss_result = wxID_NO;
        if (show_redesign_confirmation(this,
                _L("模型已导入工程，但切片视口尚未完成首帧。可只重试视图准备，不会再次导入模型。"),
                _L("模型视口"), options) == wxID_YES)
            retry_import_target_view();
        return;
    }
    canvas->set_as_dirty();
    canvas->get_wxglcanvas()->Refresh(false);
}

bool RedesignShell::can_open_print_preparation(bool gcode_ready) const
{
    if (!m_print_page || !m_plater) return false;
    const bool busy = m_import_in_progress || (m_slice_tools && m_slice_tools->busy()) ||
        workbench_slice_running(false, m_slicing_state.official.phase);
    const auto* plate = m_plater->get_partplate_list().get_curr_plate();
    const bool external = m_plater->only_gcode_mode() || m_plater->using_exported_file();
    const bool matching = matching_workbench_result(plate, m_native_slicing, external, m_slicing_state);
    return workbench_print_preparation_allowed(gcode_ready && matching, busy, m_page_coordinator.transition().active());
}

void RedesignShell::refresh_print_preparation_action()
{
    if (!m_slice_print || m_presentation_stopping) return;
    const bool gcode_ready = OrcaPrinterAdapter(m_plater).snapshot().gcode_ready;
    const bool available = can_open_print_preparation(gcode_ready);
    const bool changed = m_slice_print->Enable(available);
    const bool preview_pending = m_preview_transition.has_value();
    const bool label_changed = set_control_label(m_slice_print,
        preview_pending ? _L("正在准备切片预览…") : _L("去打印"));
    if (changed || label_changed)
        BOOST_LOG_TRIVIAL(info) << "[PrintPreparation] available=" << available
            << " gcode_ready=" << gcode_ready << " preview_pending=" << preview_pending;
}

void RedesignShell::open_print_preparation()
{
    if (block_workbench_navigation()) {
        BOOST_LOG_TRIVIAL(info) << "[PrintPreparation] click_blocked workbench_modal=true";
        return;
    }
    const auto state = OrcaPrinterAdapter(m_plater).snapshot();
    if (!can_open_print_preparation(state.gcode_ready)) {
        BOOST_LOG_TRIVIAL(info) << "[PrintPreparation] click_blocked gcode_ready=" << state.gcode_ready
            << " slicing=" << workbench_slice_running(state.slicing, m_slicing_state.official.phase)
            << " importing=" << m_import_in_progress << " tools_busy=" << (m_slice_tools && m_slice_tools->busy())
            << " transition_active=" << m_page_coordinator.transition().active();
        refresh_print_preparation_action();
        return;
    }
    m_print_page->show_prepare();
    const bool opened = navigate_to(Page::Print);
    BOOST_LOG_TRIVIAL(info) << "[PrintPreparation] navigation_opened=" << opened;
}

bool RedesignShell::update_print_preview(bool active, bool layers, bool support)
{
    if (!m_plater || !m_native_plater_host || !m_print_page) return false;
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    const auto state = OrcaPrinterAdapter(m_plater).snapshot();
    active = active && owns_model_workflow() && state.gcode_ready && !state.slicing && plate &&
        plate->is_slice_result_valid();
    wxString notice;
    m_print_page->set_preparation_notice(notice);
    if (m_print_canvas_mounted && active &&
        (!m_print_preview_revision_current || !m_print_preview_revision_current() ||
         plate->get_gcode_filename() != m_print_preview_gcode)) active = false;
    if (!active) {
        if (!m_print_canvas_mounted) return false;
        for (auto* canvas : {m_plater->get_view3D_canvas3D(), m_plater->get_preview_canvas3D()})
            canvas->set_print_preview_read_only(false);
        m_plater->get_preview_canvas3D()->get_gcode_viewer().set_support_visibility(true);
        if (!m_print_plater_mount.restore()) return false;
        m_print_canvas_mounted = false;
        m_print_preview_revision_current = {};
        m_plater->select_view_3D(m_page_coordinator.view() == ModelView::Preview ? "Preview" : "3D");
        refresh_workflow_layout();
        return false;
    }
    const bool mount = !m_print_canvas_mounted;
    if (mount) {
        m_print_preview_revision_current = OrcaWorkspaceAdapter(m_plater, {}).capture_import_guard();
        m_print_preview_gcode = plate->get_gcode_filename();
        if (!m_print_preview_revision_current || m_print_preview_gcode.empty() ||
            !boost::filesystem::is_regular_file(boost::filesystem::path(wxString::FromUTF8(m_print_preview_gcode).ToStdWstring()))) return false;
        if (!m_print_plater_mount.mount(m_plater, m_print_page->preview_host())) return false;
        m_print_canvas_mounted = true;
    }
    const bool mode_changed = mount || layers != m_print_canvas_layers;
    if (mode_changed) m_plater->select_view_3D(layers ? "Preview" : "3D");
    for (auto* canvas : {m_plater->get_view3D_canvas3D(), m_plater->get_preview_canvas3D()}) {
        canvas->set_print_preview_read_only(true);
        canvas->use_workbench_appearance();
    }
    if (mode_changed || support != m_print_canvas_support) {
        auto* canvas = m_plater->get_preview_canvas3D();
        canvas->get_gcode_viewer().set_view_type(libvgcode::EViewType::ColorPrint);
        canvas->get_gcode_viewer().set_support_visibility(support);
        canvas->set_as_dirty();
    }
    m_print_canvas_layers = layers; m_print_canvas_support = support;
    if (mount || mode_changed) {
        refresh_workflow_layout();
        m_print_page->preview_host()->Layout(); m_plater->Layout();
        m_plater->get_current_canvas3D()->zoom_to_volumes();
    }
    if (mount || mode_changed) {
        wxWeakRef<RedesignShell> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->m_print_canvas_mounted || weak->m_page_coordinator.page() != Page::Print) return;
            weak->m_print_page->preview_host()->Layout();
            weak->m_plater->Layout();
            auto* canvas = weak->m_plater->get_current_canvas3D();
            const auto before = canvas->rendered_frames();
            canvas->enable_render(true);
            canvas->reset_old_size();
            canvas->zoom_to_volumes();
            canvas->render();
            BOOST_LOG_TRIVIAL(info) << "[PrintPreview] first frame=" << before << "->" << canvas->rendered_frames();
        });
    }
    return true;
}

void RedesignShell::return_to_slice_preview()
{
    if (!owns_model_workflow()) return;
    show_model_view(workbench_return_view(OrcaPrinterAdapter(m_plater).snapshot().gcode_ready));
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
    dialog.SetIcon(wxIcon(Slic3r::var("honor_forge.ico"), wxBITMAP_TYPE_ICO));
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
    const bool visible = native_workspace_visible();
    auto* view = m_plater->get_current_canvas3D();
    auto* canvas = view->get_wxglcanvas();
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
    const wxRect preferred(size.x - canvas->FromDIP(440), y, progress_size.x, progress_size.y);
    auto* notifications = m_plater->get_notification_manager();
    const auto progress_bounds = progress_above_notifications(preferred, size,
        notifications->visible_notification_bounds(*view), canvas->FromDIP(8), margin);
    // Leave room for both the stage label and the percentage on a narrow canvas.
    const bool progress_fits = progress_bounds && progress_bounds->GetWidth() >= canvas->FromDIP(96);
    if (progress_fits && m_slice_progress_overlay->GetRect() != *progress_bounds) {
        m_slice_progress_overlay->SetSize(*progress_bounds);
        overlay_changed = true;
    }
    const bool running = workbench_slice_running(m_plater->is_background_process_slicing(),
                                                 m_slicing_state.official.phase);
    const bool show_progress = visible && running && progress_fits;
    if (m_slice_progress_overlay->IsShown() != show_progress) {
        m_slice_progress_overlay->Show(show_progress);
        overlay_changed = true;
    }
    if (show_progress && overlay_changed) m_slice_progress_overlay->Raise();
    if (overlay_changed) view->set_as_dirty();
}

void RedesignShell::request_initial_slicing_camera()
{
    m_slice_camera_retry_timer.Stop();
    m_slice_camera_fitted = false;
    m_slice_camera_loading_pending = false;
    m_slice_camera_waiting_for_frame = false;
}

void RedesignShell::layout_slicing_camera_loading()
{
    if (!m_slice_camera_loading || !m_plater) return;
    auto* canvas = m_plater->get_view3D_canvas3D()->get_wxglcanvas();
    if (!canvas) return;
    if (m_slice_camera_loading->GetParent() != canvas)
        m_slice_camera_loading->Reparent(canvas);
    const bool visible = m_slice_camera_loading_pending && !m_import_in_progress &&
        native_workspace_visible() && m_page_coordinator.view() == ModelView::Slicing;
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
    if (m_slice_recommendation_source) m_slice_recommendation_source->Show(!native);
    m_plater->collapse_sidebar(true);
    mount_custom_process(native && native_workspace_visible());
    m_slicing_host->set_workbench_active(native_workspace_visible(), !native);
    if (!native && !changed && !m_slicing_state.analyzing && m_slicing_state.session.state != RecommendationSessionState::Ready)
        m_slicing_host->analyze_workbench();
    apply_slicing_state(m_slicing_host->workbench_snapshot());
    m_slicing_page->Layout();
    refresh_workflow_layout();
}

void RedesignShell::on_slicing_recommendation_source_changed(int selection)
{
    if (!m_slicing_host) return;
    const bool cloud = selection == 1;
    if (cloud && (!wxGetApp().app_config ||
        wxGetApp().app_config->get("smart_slicing_cloud_notice_version") != "1")) {
        RedesignConfirmationOptions options;
        options.style = wxYES_NO | wxNO_DEFAULT;
        options.minimum_message_height = 250;
        options.actions = {
            {wxID_NO, _L("取消"), false},
            {wxID_YES, _L("同意并使用"), true},
        };
        const wxString message = _L(
            "云端 AI 推荐会把当前打印板的脱敏结构化摘要发送给 YOYOClaw，包括：\n\n"
            "• 汇总后的模型尺寸、体积、表面积、三角面数量、闭合性、悬垂和薄壁比例；\n"
            "• 喷嘴、层高、速度、加速度和流量限制；\n"
            "• 材料类型与流量限制；\n"
            "• 三套本地安全基线，以及允许调整的七类参数和范围。\n\n"
            "不会上传模型文件、网格顶点、模型名称、本地路径、预设显示名、凭据或身份信息。"
            "返回结果仍会在本地校验，并由 Orca 原生切片器生成 G-code。");
        if (show_redesign_confirmation(this, message, _L("云端 AI 推荐数据说明"), options) != wxID_YES) {
            set_slicing_recommendation_source_selection(0);
            return;
        }
        if (wxGetApp().app_config) {
            wxGetApp().app_config->set("smart_slicing_cloud_notice_version", "1");
            wxGetApp().app_config->save();
        }
    }
    const auto source = cloud ? SlicingRecommendationSource::Cloud : SlicingRecommendationSource::Local;
    if (!m_slicing_host->set_recommendation_source(source))
        set_slicing_recommendation_source_selection(
            m_slicing_state.recommendation_source == SlicingRecommendationSource::Cloud ? 1 : 0);
    apply_slicing_state(m_slicing_host->workbench_snapshot());
}

void RedesignShell::apply_slicing_state(const SmartSlicingWorkbenchState& state)
{
    Presentation::assert_gui_thread();
    if (m_presentation_stopping) return;
    const bool background_slicing = m_plater && m_plater->is_background_process_slicing();
    Presentation::background_tasks().set_native_busy(background_slicing);
    const bool running = workbench_slice_running(background_slicing, state.official.phase);
    // The previous SmartSlice phase is the last UI snapshot.  Use it as the
    // edge detector; the current Plater flag is intentionally not reused here
    // because it would make a newly started background job look old already.
    const bool previously_running = m_slicing_state.official.phase == OfficialSlicePhase::Slicing;
    const bool began = running && !previously_running;
    m_slicing_state = state;
    if (m_assets_workspace) m_assets_workspace->set_project_available(can_save_print_project());
    if (began) {
        m_slice_progress_percent = 0; m_slice_progress_stage = _L("正在切片…");
        ++m_slice_progress_phase;
    }
    ++m_slicing_notifications;
    m_slicing_dirty.invalidate(Presentation::Dirty::Content | Presentation::Dirty::Overlay);
    if (native_workspace_visible())
        m_surface_refresh.request(Presentation::Dirty::Content | Presentation::Dirty::Overlay);
}

void RedesignShell::apply_slicing_snapshot()
{
    Presentation::assert_gui_thread();
    if (m_presentation_stopping || !native_workspace_visible()) return;
    // Freeze one projection even if its native consumers publish another
    // state synchronously. That invalidation belongs to the next commit.
    const auto started = std::chrono::steady_clock::now();
    ++m_slicing_commits;
    const auto state = m_slicing_state;
    const bool background_slicing = m_plater && m_plater->is_background_process_slicing();
    const bool external_toolpath = m_plater &&
        (m_plater->only_gcode_mode() || m_plater->using_exported_file());
    const bool running = workbench_slice_running(background_slicing, state.official.phase);
    if (m_slice_recommendation_source) {
        const int source = state.recommendation_source == SlicingRecommendationSource::Cloud ? 1 : 0;
        set_slicing_recommendation_source_selection(source);
        m_slice_recommendation_source->Enable(!running && !m_import_in_progress && !m_slice_tools->busy());
    }
    if (state.official.phase == OfficialSlicePhase::Completed && background_slicing &&
        (m_page_coordinator.view() == ModelView::Slicing || m_page_coordinator.view() == ModelView::Preview) &&
        m_page_coordinator.page() == Page::Model &&
        !m_slice_completion_retry_pending) {
        // Plater publishes the completion event before it clears its running
        // flag. Defer one snapshot until that event returns so the successful
        // result can enter the native preview route without polling.
        m_slice_completion_retry_pending = true;
        wxWeakRef<RedesignShell> weak(this);
        CallAfter([weak] {
            if (!weak) return;
            weak->m_slice_completion_retry_pending = false;
            if (!weak->m_plater || weak->m_page_coordinator.page() != Page::Model ||
                (weak->m_page_coordinator.view() != ModelView::Slicing && weak->m_page_coordinator.view() != ModelView::Preview) ||
                weak->m_plater->is_background_process_slicing()) return;
            if (weak->m_slicing_host)
                weak->apply_slicing_state(weak->m_slicing_host->workbench_snapshot());
        });
    }
    if (native_workspace_visible() && running) {
        if (auto* notifications = m_plater->get_notification_manager())
            notifications->set_slicing_progress_hidden();
    }
    if (running) m_slice_progress_overlay->update(m_slice_progress_stage, m_slice_progress_percent);
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
    bool layout_changed = set_slicing_detail_label(*m_slice_model_stats, stats);
    m_slice_stats_overlay->update(stats);
    update_slicing_tools();
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    const auto quick_recommendation = [&](size_t index) -> const AI::SmartSlicing::SliceRecommendation* {
        if (!state.lightweight_analysis || !state.lightweight_analysis->completed ||
            !workbench_recommendation_parameters_ready(state)) return nullptr;
        const auto mode = index == 1 ? AI::SmartSlicing::SliceMode::SPEED :
            index == 2 ? AI::SmartSlicing::SliceMode::QUALITY : AI::SmartSlicing::SliceMode::BALANCED;
        return state.lightweight_analysis->find(mode);
    };
    SliceParameterGroup parameter_group;
    std::array<wxString, 3> availabilities;
    for (size_t i = 0; i < titles.size(); ++i) {
        const auto& goal = state.session.recommendation.goal_result(RECOMMENDATION_GOALS[i]);
        auto& layer = parameter_group.layers[i];
        if (state.recommendation_source == SlicingRecommendationSource::Local)
            for (const auto& parameter : state.effective_parameters[i])
                if (parameter.key == "layer_height") { layer = parameter_value(parameter); break; }
        if (layer.empty()) {
            if (const auto* quick = quick_recommendation(i))
                layer = wxString::Format(_L("%.2f mm"), quick->layer_height_mm);
        }
        const bool quick_available = quick_recommendation(i) != nullptr;
        const bool cloud = state.recommendation_source == SlicingRecommendationSource::Cloud;
        const auto cloud_status = state.cloud_recommendation.status;
        availabilities[i] = cloud && !quick_available ?
            ((cloud_status == CloudRecommendationStatus::Requesting ||
              state.analyzing) ? _L("AI推理中...") :
             cloud_status == CloudRecommendationStatus::Failed ? _L("未完成 · 可重试") : _L("待推荐")) : quick_available ? _L("可用于原生切片") :
            !state.analyzing && state.candidates[i] ? goal_status(goal.status) :
            state.analyzing ? _L("分析中…") : _L("待分析");
        if (!cloud) {
            const bool is_selected = workbench_goal_is_currently_selected(state, RECOMMENDATION_GOALS[i]);
            if (!quick_available && !state.candidates[i] && !is_selected) {
                availabilities[i] = _L("待计算");
            } else if (quick_available) {
                availabilities[i] = _L("已生成推荐参数");
                if (is_selected) {
                    if (state.can_start) availabilities[i] = _L("可用于原生切片");
                    else if (state.exact_estimate.status == ExactEstimateStatus::Running)
                        availabilities[i] = _L("正在试切");
                    else if (state.exact_estimate.status == ExactEstimateStatus::Failed)
                        availabilities[i] = _L("试切未通过");
                    else if (state.exact_estimate.status == ExactEstimateStatus::PositionChanged)
                        availabilities[i] = _L("原生预估待更新");
                }
            }
        }
        if (layer.empty()) layer = availabilities[i];
    }
    const auto display_goal = state.has_selected_goal ? state.selected_goal :
        state.pending_slice_goal.value_or(state.last_sliced_goal.value_or(RECOMMENDATION_GOALS.front()));
    const size_t selected = static_cast<size_t>(display_goal);
    const bool has_display_goal = state.has_selected_goal || state.pending_slice_goal.has_value() ||
        state.last_sliced_goal.has_value();
    const bool has_parameters = state.recommendation_source == SlicingRecommendationSource::Local &&
        has_display_goal && state.candidates[selected].has_value();
    const auto* selected_quick = has_display_goal ? quick_recommendation(selected) : nullptr;
    std::vector<SliceDetailRow> detail_rows;
    const bool official_ready = state.official_statistics_ready &&
        !background_slicing && plate && plate->is_slice_result_valid();
    const bool cloud_waiting = state.recommendation_source == SlicingRecommendationSource::Cloud &&
        (state.analyzing || !workbench_recommendation_parameters_ready(state));
    if (cloud_waiting) {
        const bool pending = state.analyzing;
        const auto value = pending ? _L("AI推理中...") : _L("暂无推荐");
        detail_rows = {{_L("预计打印时间"), pending ? _L("计算中...") : _L("暂无"), {}},
                       {_L("预计耗材"), pending ? _L("计算中...") : _L("暂无"), {}}};
        for (const auto& label : {_L("表面质量"), _L("悬垂风险"), _L("层高"), _L("墙数"),
                                 _L("填充率"), _L("外壁速度"), _L("支撑"), _L("支撑角度"),
                                 _L("自适应层高"), _L("墙生成器")})
            detail_rows.push_back({label, value, {}});
    }
    else if (has_parameters)
        detail_rows = candidate_detail_rows(state.effective_parameters[selected], state.metrics[selected],
            official_ready ? official_detail_rows(plate) : std::vector<SliceDetailRow>{});
    else if (selected_quick != nullptr) {
        auto estimate = slicing_display_estimate(state, display_goal, *selected_quick);
        if (running && !official_ready && estimate.status == ExactEstimateStatus::Failed)
            estimate.status = ExactEstimateStatus::None;
        detail_rows = exact_estimate_detail_rows(*selected_quick, estimate,
            official_ready ? official_detail_rows(plate) : std::vector<SliceDetailRow>{},
            estimate.revision != state.session.workspace_revision, running);
    }
    else if (official_ready)
        detail_rows = official_detail_rows(plate);

    const bool complete = slicing_parameter_group_complete(state, display_goal, official_ready);
    const bool terminal = slicing_parameter_group_terminal(state, display_goal);
    // Parameter values belong to the selected recommendation immediately. Its
    // statistics have their own state and are only shown for matching evidence.
    if (!complete && !terminal && state.recommendation_source == SlicingRecommendationSource::Cloud && state.analyzing)
        for (auto& layer : parameter_group.layers) layer = _L("计算中...");
    parameter_group.details = std::move(detail_rows);
    const bool parameters_changed = m_slicing_parameters.submit(state, std::move(parameter_group), complete, terminal);
    const auto& displayed = m_slicing_parameters.group();
    detail_rows = displayed.details;
    for (size_t i = 0; i < titles.size(); ++i) {
        const bool cloud = state.recommendation_source == SlicingRecommendationSource::Cloud;
        m_slice_goals[i]->Enable((cloud ? !state.analyzing && state.cloud_recommendation.status == CloudRecommendationStatus::Ready : state.native_workspace_available) &&
            !running && !state.official.workspace_mutated && !m_slice_tools->busy());
        auto* button = static_cast<SlicingGoalCard*>(m_slice_goals[i]);
        button->update(displayed.layers[i], workbench_goal_is_currently_selected(state, RECOMMENDATION_GOALS[i]) ||
            workbench_goal_is_pending(state, RECOMMENDATION_GOALS[i]),
            workbench_goal_was_last_sliced(state, RECOMMENDATION_GOALS[i]));
        wxString tooltip = displayed.layers[i] + "\n" + availabilities[i];
        if (workbench_goal_was_last_sliced(state, RECOMMENDATION_GOALS[i])) tooltip += _L("\n上次切片成功");
        set_control_tooltip(button, tooltip);
    }
    if (parameters_changed) {
        ++m_slicing_parameter_updates;
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
                parameter_layout_changed = set_slicing_detail_visible(
                    *parameters->GetItem(m_slice_parameter_labels[i]), visible) || parameter_layout_changed;
                parameter_layout_changed = set_slicing_detail_visible(
                    *parameters->GetItem(m_slice_parameter_values[i]), visible) || parameter_layout_changed;
                parameter_layout_changed = set_slicing_detail_label(*m_slice_parameter_labels[i],
                    label_text) || parameter_layout_changed;
                parameter_layout_changed = set_slicing_detail_label(*m_slice_parameter_values[i],
                    value_text) || parameter_layout_changed;
                set_control_tooltip(m_slice_parameter_labels[i], visible ? detail_rows[i].section : wxEmptyString);
                set_control_tooltip(m_slice_parameter_values[i], visible ? detail_rows[i].section : wxEmptyString);
            }
            if (parameter_layout_changed) {
                parameters->Layout();
                layout_changed = true;
            }
        }
    }
    const bool lightweight_ready = state.lightweight_analysis && state.lightweight_analysis->completed;
    const bool preflight_has_blocking_issue = state.preflight && state.preflight->has_blocking_issue();
    const auto recommendation_stage = workbench_recommendation_stage(state, display_goal);
    using RecommendationStage = RecommendationPresentationStage;
    wxString status;
    if (recommendation_stage == RecommendationStage::RequestingCloud)
        status = _L("正在获取云端 AI 推荐…");
    else if (recommendation_stage == RecommendationStage::PreparingCloud)
        status = _L("正在准备云端推荐数据…");
    else if (recommendation_stage == RecommendationStage::Analyzing) status = state.session.state == RecommendationSessionState::Canceled
        ? _L("正在取消分析…")
        : state.recommendation_source == SlicingRecommendationSource::Local
            ? _L("正在分析当前方案…") : _L("正在分析三种方案…");
    else if (recommendation_stage == RecommendationStage::CloudFailed) {
        status = state.recommendation_source == SlicingRecommendationSource::Local ?
            _L("云端 AI 推荐未完成，已切回本地推荐。") :
            _L("云端 AI 推荐未完成，可重新分析或切换本地推荐。");
        const auto& failure = state.cloud_recommendation;
        if (!failure.diagnostic.empty()) {
            wxString reason = wxString::FromUTF8(failure.diagnostic);
            if (failure.diagnostic == "smart_slice_infill_pattern_unavailable" ||
                (failure.diagnostic == "cloud_enum_unavailable" && failure.failed_parameter == "sparse_infill_pattern"))
                reason = _L("推荐的填充图案不受当前切片器支持。");
            else if (failure.diagnostic == "smart_slice_support_type_unavailable" ||
                     (failure.diagnostic == "cloud_enum_unavailable" && failure.failed_parameter == "support_type"))
                reason = _L("推荐的支撑类型不受当前切片器支持。");
            status += _L("\n原因：") + reason;
            if (failure.failed_goal == "balanced" || failure.failed_goal == "speed" || failure.failed_goal == "quality")
                status += _L("\n目标：") + titles[failure.failed_goal == "speed" ? 1 : failure.failed_goal == "quality" ? 2 : 0];
            if (!failure.failed_parameter.empty()) {
                wxString parameter = wxString::FromUTF8(failure.failed_parameter);
                if (failure.failed_parameter == "sparse_infill_pattern") parameter = _L("填充图案");
                else if (failure.failed_parameter == "support_type") parameter = _L("支撑类型");
                else if (failure.failed_parameter == "layer_height") parameter = _L("层高");
                else if (failure.failed_parameter == "wall_loops") parameter = _L("墙数");
                else if (failure.failed_parameter == "sparse_infill_density") parameter = _L("填充率");
                else if (failure.failed_parameter == "outer_wall_speed") parameter = _L("外壁速度");
                else if (failure.failed_parameter == "enable_support") parameter = _L("启用支撑");
                status += _L("\n参数：") + parameter;
                if (!failure.failed_value.empty()) status += " = " + wxString::FromUTF8(failure.failed_value);
            }
            status += _L("\n错误代码：") + wxString::FromUTF8(failure.diagnostic);
        }
        if (!state.cloud_recommendation.diagnostic_id.empty())
            status += _L("\n诊断 ID：") + wxString::FromUTF8(state.cloud_recommendation.diagnostic_id);
    }
    else if (recommendation_stage == RecommendationStage::EstimatingCloud)
        status = _L("云端参数已返回，正在精确计算…");
    else if (recommendation_stage == RecommendationStage::EstimatingLocal)
        status = _L("当前方案正在试切，正在计算打印时间和耗材…");
    else if (recommendation_stage == RecommendationStage::CloudReady)
        status = _L("云端 AI 推荐已生成");
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
            case IssueCode::IncompatiblePhysicalSlots: status += _L("\n耗材不兼容：请检查当前工程的耗材和喷嘴映射。"); break;
            case IssueCode::MachineCapabilityUnavailable:
                status += _L("\nAI 机型能力未通过验证：") + capability_reasons(state.machine_reasons) +
                    _L("。本次仍按 Orca 原生流程执行。");
                break;
            default: status += "\n" + wxString::FromUTF8(issue.evidence); break;
            }
        }
    } else if (lightweight_ready)
        status = _L("推荐方案已生成");
    else if (state.diagnostic == "smart_slice_layer_height_no_common_range")
        status = _L("当前参与打印的喷嘴没有共同的层高范围。请检查耗材映射和喷嘴层高设置。");
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
        else if (diagnostic == "smart_slice_rollback_unavailable") detail = _L("切片失败，工程在切片期间已变化。请检查当前设置。");
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
    const bool position_changed = !m_native_slicing && !running && state.exact_estimate.status == ExactEstimateStatus::PositionChanged;
    const bool trial_failed = !m_native_slicing && !running && (state.exact_estimate.status == ExactEstimateStatus::Failed ||
        (position_changed && !state.exact_estimate.diagnostic.empty())) &&
        state.exact_estimate.revision == state.session.workspace_revision && state.exact_estimate.goal == display_goal;
    if (trial_failed && state.official.diagnostic_code.empty()) {
        const auto& code = state.exact_estimate.diagnostic;
        if (code == "trial_tower_no_clear_position") status = _L("当前打印盘没有可验证的擦料塔位置。");
        else if (code == "trial_tower_outside") status = _L("擦料塔底面超出可打印区域，请调整位置。");
        else if (code == "trial_tower_excluded") status = _L("擦料塔底面进入禁打区，请调整位置。");
        else if (code == "trial_tower_model_overlap") status = _L("擦料塔与模型或首层支撑冲突，请调整位置。");
        else if (code == "trial_tower_position" && state.exact_estimate.diagnostic_message.empty())
            status = _L("当前方案的擦料塔需要重新摆放。");
        else if (code == "trial_support_needed") status = _L("原生试切发现需要支撑的区域。");
        else if (code == "workflow_timeout") status = _L("试切超时，可以重新分析。");
        else if (code == "smart_slice_preview_canceled") status = _L("已取消分析，可以重新分析。");
        else if (code == "trial_toolpath_outside") status = _L("试切路径超出可打印区。");
        else if (code == "candidate_budget_exceeded") status = _L("本轮试切已达到次数上限。");
        else if (!state.exact_estimate.diagnostic_message.empty())
            status = wxString::FromUTF8(state.exact_estimate.diagnostic_message);
        else status = _L("试切未完成，可以重新分析。");
    }
    if (position_changed && !trial_failed)
        status = _L("擦料塔位置已更新，原生预估待更新。");
    if (state.orientation_running) status = _L("正在试切备选摆放角度…");
    else if (state.orientation_diagnostic == "smart_slice_orientation_unavailable")
        status = _L("未找到符合条件的备选摆放，保留当前摆放。");
    else if (state.orientation_diagnostic == "smart_slice_orientation_no_benefit")
        status = _L("备选摆放未达到节料条件，保留当前摆放。");
    else if (!state.orientation_diagnostic.empty())
        status = _L("备选摆放试切未通过，保留当前摆放。");
    set_control_tooltip(m_slice_status, status);
    layout_changed = m_slice_orientation->Show(!m_native_slicing && !running && state.can_start &&
        state.recommendation_source == SlicingRecommendationSource::Local &&
        state.exact_estimate.status == ExactEstimateStatus::Ready) || layout_changed;
    m_slice_orientation->SetLabel(state.orientation_estimate ? _L("查看备选摆放") : _L("比较摆放角度"));
    m_slice_orientation->Enable(!state.orientation_running);
    const bool needs_attention = !state.analyzing && !state.diagnostic.empty();
    const bool recommendation_ready =
        (recommendation_stage == RecommendationStage::CloudReady || recommendation_stage == RecommendationStage::LocalReady) && !running &&
        state.official.phase != OfficialSlicePhase::Completed;
    // Keep the outcome and the explicit retry action visible after the
    // background worker completes.  The three-card flow is usable even when
    // the diagnostic-only SmartSlice preflight reports an issue, so hiding
    // these controls based solely on `diagnostic` made a completed analysis
    // look unavailable and removed the only way to rerun it.
    const bool prior_mode_result = !running && plate && plate->is_slice_result_valid() &&
        !matching_workbench_result(plate, m_native_slicing, external_toolpath, state);
    const bool show_analysis_status = running || prior_mode_result || state.analyzing || recommendation_ready || needs_attention || trial_failed ||
        position_changed || state.orientation_running || !state.orientation_diagnostic.empty() ||
        recommendation_stage == RecommendationStage::EstimatingCloud ||
        recommendation_stage == RecommendationStage::EstimatingLocal ||
        state.official.phase == OfficialSlicePhase::Completed ||
        state.official.phase == OfficialSlicePhase::Failed;
    const bool cloud_failed = recommendation_stage == RecommendationStage::CloudFailed;
    const bool official_status = running || state.official.phase == OfficialSlicePhase::Completed ||
        !state.official.diagnostic_code.empty();
    if (m_custom_process_content) m_custom_process_content->Enable(!running);
    wxString status_label = prior_mode_result && !trial_failed
        ? (plate->uses_workbench_smart_process() ? _L("当前预览：上一次智能切片") : _L("当前预览：Orca原生切片"))
        : m_native_slicing && !official_status
        ? (plate && plate->is_slice_result_valid()
            ? (plate->uses_workbench_smart_process() ? _L("当前预览：智能切片") : _L("当前预览：Orca原生切片"))
            : wxString{})
        : official_status
        ? status
        : trial_failed || position_changed || state.orientation_running || !state.orientation_diagnostic.empty() ||
          state.diagnostic == "smart_slice_layer_height_no_common_range" ? status
        : cloud_failed
        ? _L("云端推荐未完成 · 查看原因")
        : recommendation_ready && recommendation_stage == RecommendationStage::LocalReady
        ? _L("本地推荐方案已生成")
        : (needs_attention && recommendation_stage == RecommendationStage::Idle ?
            _L("方案暂不可用 · 查看原因") : status);
    if (m_native_slicing && !running && plate && wxGetApp().preset_bundle) {
        const auto summary = custom_layer_summary(*plate, *wxGetApp().preset_bundle,
            plate->is_slice_result_valid() && !plate->uses_workbench_smart_process());
        if (!summary.empty()) {
            if (!status_label.empty()) status_label += "\n";
            status_label += summary;
        }
    }
    layout_changed = m_slice_status->Show(show_analysis_status || (m_native_slicing && !status_label.empty())) || layout_changed;
    layout_changed = set_wrapped_label(m_slice_status, status_label, FromDIP(316)) || layout_changed;
    layout_changed = m_slice_analyze->Show(workbench_show_reanalyze(state) && !running) || layout_changed;
    m_slice_analyze->SetLabel(_L("重新分析"));
    set_control_tooltip(m_slice_analyze, state.recommendation_source == SlicingRecommendationSource::Cloud ?
        _L("重新请求综合最优、速度优先、质量优先三种方案。") :
        _L("重新计算当前选中的方案及其原生预估。"));
    layout_changed = m_slice_keep_mesh->Show(state.can_keep_current_mesh && !state.analyzing && !running) || layout_changed;
    const auto route = workbench_slice_route(m_native_slicing, running || m_import_in_progress || m_slice_tools->busy(),
        state.can_retry, state.can_start, plate && plate->has_printable_instances());
    m_slice_start->Enable(route != WorkbenchSliceRoute::Unavailable);
    m_slice_start->SetLabel(state.can_retry && !m_native_slicing ? _L("重试切片") : _L("开始切片"));
    if (route == WorkbenchSliceRoute::Unavailable && !m_native_slicing) {
        set_control_tooltip(m_slice_start, lightweight_ready
            ? _L("当前方案还未选择，或 Orca 原生切片暂时不可启动。")
            : _L("请等待当前方案完成分析后再开始切片。"));
    } else {
        set_control_tooltip(m_slice_start, wxEmptyString);
    }
    m_slice_analyze->Enable(state.can_analyze);
    layout_changed = m_slice_cancel->Show((!m_native_slicing && workbench_analysis_busy(state)) || running) || layout_changed;
    m_slice_cancel->SetLabel(running ? _L("取消切片") : _L("取消分析"));
    const bool can_switch = slicing_mode_switch_allowed();
    m_ai_slicing_tab->Enable(can_switch);
    m_native_slicing_tab->Enable(can_switch);
    m_slice_return_overview->Enable(can_switch);
    static_cast<SlicingModeButton*>(m_ai_slicing_tab)->SetValue(!m_native_slicing);
    static_cast<SlicingModeButton*>(m_native_slicing_tab)->SetValue(m_native_slicing);
    m_slice_export->Enable(OrcaPrinterAdapter(m_plater).snapshot().gcode_ready);
    refresh_project_save_actions();
    if (m_native_return_ai) {
        m_native_return_ai->Enable(can_switch);
        m_native_return_overview->Enable(can_switch);
    }
    if (m_page_coordinator.page() == Page::Model &&
        (m_page_coordinator.view() == ModelView::Slicing || m_page_coordinator.view() == ModelView::Preview) &&
        plate && plate->get_slice_result() && !m_page_coordinator.transition().active() &&
        m_slice_completion.consume(state.official.phase == OfficialSlicePhase::Completed,
            background_slicing, plate->is_slice_result_valid(), plate->get_slice_result()->id))
        show_model_view(ModelView::Preview);
    else if (!external_toolpath && !m_native_slicing && !running && plate && !plate->is_slice_result_valid() &&
             m_page_coordinator.view() == ModelView::Preview && m_page_coordinator.page() == Page::Model) {
        wxWeakRef<RedesignShell> weak(this);
        CallAfter([weak] {
            if (!weak || !weak->native_workspace_visible() || weak->m_page_coordinator.view() != ModelView::Preview ||
                workbench_slice_running(weak->m_plater->is_background_process_slicing(),
                                        weak->m_slicing_state.official.phase)) return;
            auto* current = weak->m_plater->get_partplate_list().get_curr_plate();
            if (current && !current->is_slice_result_valid()) weak->show_model_view(ModelView::Slicing);
        });
    }
    refresh_print_preparation_action();
    if (layout_changed) {
        ++m_slicing_layouts;
        m_slice_status->GetParent()->Layout();
        if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_slice_status->GetParent())) scroll->FitInside();
        if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_ai_slicing_controls->GetParent())) scroll->FitInside();
    }
    layout_slicing_overlays();
    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (std::getenv("ORCASLICER_UI_LATENCY_TRACE") &&
        (parameters_changed || layout_changed || m_slicing_commits % 60 == 0 || elapsed > 50.))
        BOOST_LOG_TRIVIAL(info) << "[SlicingRefresh] notifications=" << m_slicing_notifications
            << " commits=" << m_slicing_commits << " parameter_updates=" << m_slicing_parameter_updates
            << " layouts=" << m_slicing_layouts << " parameters_changed=" << parameters_changed
            << " layout_changed=" << layout_changed << " commit_ms=" << elapsed;
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
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    const auto route = workbench_slice_route(m_native_slicing,
        workbench_slice_running(m_plater && m_plater->is_background_process_slicing(), reviewed.official.phase),
        reviewed.can_retry, reviewed.can_start, plate && plate->has_printable_instances());
    if (route == WorkbenchSliceRoute::Unavailable) { apply_slicing_state(reviewed); return; }
    if (route == WorkbenchSliceRoute::Native) {
        BOOST_LOG_TRIVIAL(info) << "[WorkbenchSlice] native=" << m_native_slicing << " fallback_reason=" << reviewed.diagnostic;
        m_slicing_host->start_custom_slice();
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
