#include "BeautyWorkbenchControls.hpp"
#include "ModelPreview3D.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include <boost/filesystem/fstream.hpp>
#include <wx/button.h>
#include <wx/bmpcbox.h>
#include <wx/dcmemory.h>
#include <wx/dcclient.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/settings.h>
#include <wx/wrapsizer.h>
#include <algorithm>
#include <exception>

namespace Slic3r::GUI {

bool BeautyWorkbenchControls::preview_protected_details() const
{
    return m_preview_protected && m_preview_protected->GetValue();
}

BeautyWorkbenchControls::BeautyWorkbenchControls(wxWindow* parent, ModelPreview3D* preview,
                                                 AI::IPrintablePaletteProvider& palette,
                                                 std::function<void()> layout_changed)
    : wxPanel(parent), m_preview(preview), m_palette(palette), m_layout_changed(std::move(layout_changed))
{
    SetBackgroundColour(wxColour(32, 32, 35));
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto dark_choice = [this] { return workbench_choice(this); };
    m_status = new wxStaticText(this, wxID_ANY, _L("区域自定义 · 正在准备模型表面"));
    m_status->Wrap(FromDIP(250));
    root->Add(m_status, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* operation_row = new wxBoxSizer(wxHORIZONTAL);
    m_operation_label = new wxStaticText(this, wxID_ANY, _L("当前操作"));
    operation_row->Add(m_operation_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_operation = dark_choice();
    m_operation->Append(_L("局部改色"));
    m_operation->Append(_L("表面柔化"));
    m_operation->Append(_L("网格修复"));
    m_operation->Append(_L("杂点清理"));
    m_operation->Append(_L("局部拉伸"));
    m_operation->SetSelection(0);
    operation_row->Add(m_operation, 1);
    root->Add(operation_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_color_slot = dark_choice();
    m_color_slot->SetKeepDropArrow(true);
    m_color_slot->SetToolTip(_L("显示当前有效耗材色卡；将当前分区绑定到槽位，不修改工程耗材。"));
    root->Add(m_color_slot, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    m_details_toggle = workbench_button(this, _L("语义区域与分区"));
    m_details_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_details_open = !m_details_open;
        update_text();
    });
    root->Add(m_details_toggle, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_secondary_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_secondary_status->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_secondary_status->Wrap(FromDIP(250));
    root->Add(m_secondary_status, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    auto* secondary_row = new wxBoxSizer(wxVERTICAL);
    m_import_secondary = workbench_button(this, _L("导入外部二级证据"));
    m_import_secondary->SetToolTip(_L("只读导入与当前模型 source、geometry、面数和运行时身份匹配的二级证据；不会修改模型或材料树。"));
    secondary_row->Add(m_import_secondary, 0, wxEXPAND);
    m_regenerate_evidence = workbench_button(this, _L("生成六视角渲染包"));
    m_regenerate_evidence->SetToolTip(_L("为当前模型生成新的六视角只读渲染证据包；不会修改模型、颜色或材料树。"));
    secondary_row->Add(m_regenerate_evidence, 0, wxEXPAND | wxTOP, FromDIP(4));
    root->Add(secondary_row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    auto* partition_row = new wxBoxSizer(wxHORIZONTAL);
    m_auto_partition = workbench_button(this, _L("自动划区"));
    m_pick_partition = workbench_button(this, _L("点选分区"));
    m_apply_partition = workbench_button(this, _L("将选区划为分区"));
    partition_row->Add(m_auto_partition, 1, wxRIGHT, FromDIP(4));
    partition_row->Add(m_pick_partition, 1, wxRIGHT, FromDIP(4));
    root->Add(partition_row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    root->Add(m_apply_partition, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto switch_row = [this, root](const wxString& label, wxPanel*& row) {
        row = new wxPanel(this);
        row->SetBackgroundColour(GetBackgroundColour());
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* text = new wxStaticText(row, wxID_ANY, label);
        text->SetForegroundColour(wxColour(235, 235, 235));
        sizer->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto* toggle = new WorkbenchSwitch(row, label);
        sizer->Add(toggle, 0, wxALIGN_CENTER_VERTICAL);
        row->SetSizer(sizer);
        root->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        return toggle;
    };
    m_lighting = switch_row(_L("光照立体展示"), m_lighting_row);
    m_lighting->SetToolTip(_L("只改变预览明暗，不改变模型颜色或耗材槽位。"));
    m_original_view = switch_row(_L("查看模型原色"), m_original_view_row);
    m_displacement = new WorkbenchSlider(this, wxID_ANY, 2, -20, 20, wxColour(61, 127, 255), "workbench_parameter_thumb");
    m_displacement->SetToolTip(_L("局部表面推拉距离，单位 0.1 mm；负值向内。"));
    m_displacement_label = new wxStaticText(this, wxID_ANY, _L("推拉距离 · 0.1 mm/格"));
    root->Add(m_displacement_label, 0);
    root->Add(m_displacement, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_falloff = new WorkbenchSlider(this, wxID_ANY, 10, 1, 50, wxColour(61, 127, 255), "workbench_parameter_thumb");
    m_falloff->SetToolTip(_L("局部拉伸边缘渐变距离，单位 0.1 mm。"));
    m_falloff_label = new wxStaticText(this, wxID_ANY, _L("边缘衰减 · 0.1 mm/格"));
    root->Add(m_falloff_label, 0);
    root->Add(m_falloff, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* semantic_row = new wxBoxSizer(wxHORIZONTAL);
    m_auto_region = dark_choice();
    m_auto_region->Append(_L("头发"));
    m_auto_region->Append(_L("皮肤"));
    m_auto_region->Append(_L("眼睛"));
    m_auto_region->Append(_L("嘴唇"));
    m_auto_region->Append(_L("衣服"));
    m_auto_region->SetSelection(0);
    m_auto_match = workbench_button(this, _L("自动匹配区域"));
    m_auto_detail = dark_choice();
    m_auto_detail->SetToolTip(_L("可选的二级语义选区；只读取已验证证据，不自动改色或选择耗材。"));
    m_auto_detail->Hide();
    m_reoptimize = workbench_button(this, _L("重新优化人像区域"));
    semantic_row->Add(m_auto_region, 1, wxRIGHT, FromDIP(4));
    semantic_row->Add(m_auto_match, 0);
    root->Add(semantic_row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    root->Add(m_auto_detail, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    root->Add(m_reoptimize, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_preview_protected = switch_row(_L("预览全部二级证据"), m_preview_protected_row);
    m_preview_protected->SetToolTip(_L("临时把已加载的保护、单视角和低置信二级证据作为查看选区；不改变 Provider 授权、耗材槽位或生产材料树。"));
    m_preview_protected_row->Hide();
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_boundary = workbench_button(this, _L("贴合边界"));
    m_undo = workbench_button(this, _L("撤销"));
    m_redo = workbench_button(this, _L("重做"));
    m_save = workbench_button(this, _L("保存新 GLB"));
    m_preview_button = workbench_button(this, _L("预览修改"));
    m_accept = workbench_button(this, _L("接受候选"));
    m_discard = workbench_button(this, _L("放弃候选"));
    m_cancel = workbench_button(this, _L("取消处理"));
    for (Button* button : {m_boundary, m_undo, m_redo, m_save})
        actions->Add(button, 1, wxRIGHT, FromDIP(4));
    root->Add(actions, 0, wxEXPAND);
    auto* candidate_row = new wxBoxSizer(wxHORIZONTAL);
    for (Button* button : {m_preview_button, m_accept, m_discard, m_cancel})
        candidate_row->Add(button, 1, wxRIGHT, FromDIP(4));
    root->Add(candidate_row, 0, wxEXPAND | wxTOP, FromDIP(6));
    SetSizer(root);
    m_boundary->Hide();
    m_save->Hide();
    m_boundary->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_boundary_adjust) on_boundary_adjust(); });
    m_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_undo) on_undo(); });
    m_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_redo) on_redo(); });
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_save) on_save(); });
    m_preview_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_preview) on_preview(); });
    m_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_accept) on_accept(); });
    m_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_discard) on_discard(); });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_cancel) on_cancel(); });
    m_auto_partition->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_partition(); });
    m_import_secondary->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_import_secondary_evidence) on_import_secondary_evidence();
    });
    m_regenerate_evidence->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_regenerate_readonly_evidence) on_regenerate_readonly_evidence();
    });
    m_pick_partition->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_partition || m_partition_task) return;
        if (on_pick_mode) on_pick_mode();
        m_status->SetLabel(_L("请点击模型上的绿色分区；随后可补选、保护、改色或拉伸。"));
    });
    m_apply_partition->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_partition_selection(); });
    m_partition_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_partition(); }, m_partition_timer.GetId());
    m_lighting->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        m_lighting->SetValue(event.IsChecked());
        if (m_preview) m_preview->set_beauty_lighting(event.IsChecked());
    });
    m_original_view->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        m_original_view->SetValue(event.IsChecked());
        if (m_preview) m_preview->set_beauty_original_view(event.IsChecked());
        if (on_original_view_changed) on_original_view_changed(event.IsChecked());
    });
    m_operation->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        if (on_operation_changed && m_operation->GetSelection() >= 0)
            on_operation_changed(m_operation->GetSelection());
        update_text();
    });
    m_color_slot->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        if (on_color_slot_changed && m_color_slot->GetSelection() >= 0)
            on_color_slot_changed(size_t(m_color_slot->GetSelection()));
    });
    m_displacement->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_text(); });
    m_falloff->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_text(); });
    m_auto_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!on_auto_match || !m_auto_region || m_auto_region->GetSelection() < 0) return;
        static constexpr const char* keys[] = {"hair", "skin", "eyes", "lips", "clothes"};
        const int index = m_auto_region->GetSelection();
        const std::string region = index >= 0 && index < 5 ? keys[index] : std::string {};
        const bool detail_selected = m_auto_detail && m_auto_detail->GetSelection() > 0;
        const int detail_index = m_auto_detail ? m_auto_detail->GetSelection() : -1;
        const std::string detail = detail_selected && detail_index >= 0 && size_t(detail_index) < m_auto_detail_ids.size()
            ? m_auto_detail_ids[size_t(detail_index)] : std::string {};
        const size_t count = detail_selected && on_auto_detail_match ? on_auto_detail_match(detail) : on_auto_match(region);
        if (count > 0) m_dirty = true;
        update_text();
        if (count == 0) {
            if (!m_preview || !m_preview->semantic_regions_ready())
                m_status->SetLabel(m_preview ? m_preview->semantic_region_status() : _L("当前没有已加载模型。"));
            else if (!m_preview->beauty_editor())
                m_status->SetLabel(_L("正在准备模型选区数据，请稍后再次点击自动匹配区域。"));
            else
                m_status->SetLabel(wxString::Format(_L("%s 未匹配到可选面；保护排除 %llu 面，低置信度排除 %llu 面。原选区保留。"),
                    detail_selected ? wxString::FromUTF8(detail) : _L("一级分区"),
                    static_cast<unsigned long long>(m_preview->semantic_selection_protected_count()),
                    static_cast<unsigned long long>(m_preview->semantic_selection_low_confidence_count())));
        } else {
            m_status->SetLabel(wxString::Format(_L("%s 已匹配 %llu 个面；保护排除 %llu 面，低置信度排除 %llu 面。"),
                detail_selected ? wxString::FromUTF8(detail) : _L("一级分区"),
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(m_preview->semantic_selection_protected_count()),
                static_cast<unsigned long long>(m_preview->semantic_selection_low_confidence_count())));
        }
        wrap_status(m_status);
        Layout();
        if (m_layout_changed) m_layout_changed();
    });
    m_auto_region->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { update_secondary_details(); });
    m_preview_protected->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& event) {
        m_preview_protected->SetValue(event.IsChecked());
        update_text();
    });
    m_reoptimize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        update_text();
        if (!m_preview || !m_preview->semantic_reoptimization_available())
            m_status->SetLabel(_L("当前模型没有可用语义输入，或有效色卡不是 1 至 6 色。"));
        else {
            const bool started = on_reoptimize ? on_reoptimize() : false;
            m_status->SetLabel(started
                ? _L("已请求重新优化；Beauty 选区和手动保护保持不变。")
                : _L("人像区域优化未启动，当前模型和选区保持不变。"));
        }
        wrap_status(m_status);
        Layout();
        if (m_layout_changed) m_layout_changed();
    });
    Hide();
}

void BeautyWorkbenchControls::update_secondary_details()
{
    if (!m_auto_detail || !m_preview || !m_auto_region || m_auto_region->GetSelection() < 0) return;
    const std::string previous_detail = m_auto_detail->GetSelection() > 0 &&
        size_t(m_auto_detail->GetSelection()) < m_auto_detail_ids.size()
            ? m_auto_detail_ids[size_t(m_auto_detail->GetSelection())] : std::string {};
    static constexpr const char* keys[] = {"hair", "skin", "eyes", "lips", "clothes"};
    const int index = m_auto_region->GetSelection();
    const std::string parent = index >= 0 && index < 5 ? keys[index] : std::string {};
    m_auto_detail->Freeze();
    m_auto_detail->Clear();
    m_auto_detail_ids.clear();
    m_auto_detail->Append(_L("整个一级分区"));
    m_auto_detail_ids.emplace_back();
    const auto details = m_preview->secondary_region_details(parent);
    const auto evidence = m_preview->secondary_region_evidence();
    for (const auto& detail : details) {
        m_auto_detail_ids.push_back(detail);
        wxString label = wxString::FromUTF8(detail);
        if (evidence) for (const auto& entry : evidence->catalog())
            if (entry.detail_id == detail && entry.accepted_count == 0 && entry.protected_count > 0) {
                label += _L("（保护）"); break;
            }
        m_auto_detail->Append(label);
    }
    const auto previous = std::find(m_auto_detail_ids.begin(), m_auto_detail_ids.end(), previous_detail);
    m_auto_detail->SetSelection(previous == m_auto_detail_ids.end() ? 0 :
        int(std::distance(m_auto_detail_ids.begin(), previous)));
    m_auto_detail->Show(m_details_open && !details.empty());
    m_auto_detail->Thaw();
    Layout();
}

void BeautyWorkbenchControls::synchronize_preview_options()
{
    if (m_preview) {
        m_lighting->SetValue(m_preview->beauty_lighting());
        m_original_view->SetValue(m_preview->beauty_original_view());
    }
}

void BeautyWorkbenchControls::synchronize(const boost::filesystem::path& source, bool editable, bool visible,
                                          bool processing, bool candidate_ready)
{
    const auto previous_source = m_source;
    const std::string previous_geometry = m_geometry_id;
    m_source = source;
    synchronize_preview_options();
    m_editable = editable;
    m_processing = processing;
    m_candidate_ready = candidate_ready;
    m_visible = visible;
    if (!m_pending_secondary_partition_source.empty() &&
        source != m_pending_secondary_partition_source)
        m_pending_secondary_partition_source.clear();
    const std::string geometry = m_preview ? m_preview->geometry_id() : std::string {};
    if (geometry != previous_geometry) {
        m_geometry_id = geometry;
        m_surface.reset();
        m_document = AI::BeautyDocument {};
        m_partition.reset();
        m_selected_piece = 0;
        m_preview->set_beauty_partition({}, {});
        m_dirty = false;
    }
    const bool region_available = m_preview && !m_geometry_id.empty() && m_preview->region_editing_ready();
    const auto editor = m_preview ? m_preview->beauty_editor() : nullptr;
    if (visible && region_available && !editor) m_preview->prepare_beauty_editor();
    m_ready = region_available && bool(editor);
    update_secondary_details();
    if (m_ready && !m_partition && source != m_partition_restore_source && !source.empty()) {
        m_partition_restore_source = source;
        auto metadata_path = source;
        metadata_path.replace_extension(".json");
        boost::system::error_code error;
        if (boost::filesystem::is_regular_file(metadata_path, error) &&
            boost::filesystem::file_size(metadata_path, error) < 256ULL * 1024 * 1024) {
            boost::filesystem::ifstream input(metadata_path);
            const auto metadata = nlohmann::json::parse(input, nullptr, false);
            const auto workbench = metadata.is_object() ? metadata.find("beauty_workbench") : metadata.end();
            if (workbench != metadata.end() && workbench->is_object()) {
                const auto& record = *workbench;
                const std::string saved_hash = record.value("model_sha256", std::string {});
                if (record.contains("puzzle") &&
                    (saved_hash.empty() || saved_hash == AI::model_artifact_sha256(source)))
                    start_partition(record.at("puzzle"));
            }
        }
    }
    if (m_ready && m_partition && (source != previous_source || !m_preview->beauty_partition_visible())) {
        m_preview->set_beauty_partition(m_partition->face_piece, m_surface->face_neighbors);
        m_preview->set_beauty_pick_callback([this](size_t face) { select_piece(face); });
    }
    Show(visible);
    if (!m_pending_secondary_partition_source.empty() && m_ready && !m_processing &&
        m_visible && !m_partition_task && m_preview->secondary_regions_ready())
        start_partition();
    update_text();
}

void BeautyWorkbenchControls::request_secondary_partition(const boost::filesystem::path& source)
{
    m_pending_secondary_partition_source = source;
}

void BeautyWorkbenchControls::update_text()
{
    if (!m_status) return;
    if (!m_visible) m_status->SetLabel(_L("Beauty 工作台已关闭。"));
    else if (!m_ready) m_status->SetLabel(_L("正在准备 Beauty 编辑数据；当前选择和语义缓存保持不变。"));
    else if (m_partition_task) m_status->SetLabel(_L("正在划分区域；可旋转、缩放、查看或取消，其他编辑需等待完成。"));
    else if (m_preview && m_preview->selection_busy()) m_status->SetLabel(_L("正在计算选区边界；可旋转、缩放、查看或取消，其他编辑需等待完成。"));
    else if (m_processing) m_status->SetLabel(_L("当前正在处理；可旋转、缩放、查看原图和日志，提交修改需等待完成。"));
    else if (m_candidate_ready) m_status->SetLabel(_L("候选版本已就绪；可继续编辑、接受或放弃。"));
    else if (m_partition) m_status->SetLabel(wxString::Format(_L("区域自定义已就绪 · 当前分区 %u；点选分区后可编辑。"), m_selected_piece));
    else if (m_dirty) m_status->SetLabel(_L("Beauty 有未保存修改；保存后生成新的 GLB 版本。"));
    else m_status->SetLabel(_L("就绪"));
    if (m_secondary_status) {
        if (!m_preview) {
            m_secondary_status->SetLabel(_L("二级语义：未加载（没有模型证据）。"));
        } else if (!m_preview->secondary_regions_ready()) {
            m_secondary_status->SetLabel(m_preview->secondary_region_evidence_error().empty()
                ? _L("二级语义：未加载（当前模型没有二级证据引用）。")
                : wxString::Format(_L("二级语义：校验失败，已回退一级分区（%s）。"),
                    wxString::FromUTF8(m_preview->secondary_region_evidence_error())));
        } else {
            size_t detail_count = 0, accepted = 0, protected_count = 0, low_confidence = 0;
            for (const auto& entry : m_preview->secondary_region_evidence()->catalog()) {
                ++detail_count;
                accepted += entry.accepted_count;
                protected_count += entry.protected_count;
                low_confidence += entry.low_confidence_count;
            }
            if (accepted == 0)
                m_secondary_status->SetLabel(wxString::Format(
                    preview_protected_details()
                        ? _L("二级语义预览已开启：已加载 %llu 个细节；当前显示保护面 %llu，低置信 %llu。")
                        : _L("二级语义：已加载 %llu 个细节，但可选面为 0；保护 %llu，低置信 %llu。"),
                    static_cast<unsigned long long>(detail_count),
                    static_cast<unsigned long long>(protected_count),
                    static_cast<unsigned long long>(low_confidence)));
            else
                m_secondary_status->SetLabel(wxString::Format(
                    preview_protected_details()
                        ? _L("二级语义预览已开启：已加载 %llu 个细节；可选面 %llu，保护面 %llu，低置信 %llu。")
                        : _L("二级语义：已加载 %llu 个细节；可选面 %llu，保护 %llu，低置信 %llu。"),
                    static_cast<unsigned long long>(detail_count),
                    static_cast<unsigned long long>(accepted),
                    static_cast<unsigned long long>(protected_count),
                    static_cast<unsigned long long>(low_confidence)));
        }
    }
    const bool selection_calculating = m_preview && m_preview->selection_busy();
    const bool available = m_editable && !m_processing && !m_partition_task && !selection_calculating;
    const bool active = available && m_ready;
    if (on_available_colors) {
        const auto colors = on_available_colors();
        if (colors != m_palette_colors) {
            m_palette_colors = colors;
            m_color_slot->Clear();
            for (size_t slot = 0; slot < colors.size(); ++slot) {
                const wxString hex = wxString::FromUTF8(colors[slot]);
                wxColour color(hex);
                if (!color.IsOk()) color = wxColour(128, 128, 128);
                const int swatch_width = FromDIP(30), swatch_height = FromDIP(16);
                wxBitmap swatch(swatch_width, swatch_height, 32);
                wxMemoryDC dc(swatch);
                dc.SetBackground(wxBrush(wxColour(22, 22, 25)));
                dc.Clear();
                dc.SetBrush(wxBrush(color));
                dc.SetPen(wxPen(wxColour(80, 80, 80), FromDIP(1)));
                dc.DrawRoundedRectangle(0, 0, swatch_width, swatch_height, FromDIP(2));
                dc.SelectObject(wxNullBitmap);
                m_color_slot->Append(wxString::Format(_L("耗材 %llu · "),
                    static_cast<unsigned long long>(slot + 1)) + hex, swatch);
            }
            if (!colors.empty()) m_color_slot->SetSelection(0);
        }
    }
    m_color_slot->Show(m_operation->GetSelection() == 0);
    m_color_slot->Enable(active && !m_palette_colors.empty());
    m_auto_partition->Enable(active);
    m_pick_partition->Enable(active && bool(m_partition));
    m_apply_partition->Enable(active && bool(m_partition));
    m_apply_partition->SetLabel(m_selected_piece ? _L("应用补选与边界调整") : _L("将选区划为新分区"));
    m_boundary->Enable(active);
    m_undo->Enable(active && m_can_undo);
    m_redo->Enable(active && m_can_redo);
    m_save->Enable(active && (m_dirty || m_candidate_ready));
    const bool semantic = m_preview && m_preview->semantic_regions_ready();
    m_auto_region->Enable(active);
    m_auto_match->Enable(active);
    m_import_secondary->Enable(active && !m_source.empty());
    m_regenerate_evidence->Enable(active && !m_source.empty());
    const bool secondary_ready = m_preview && m_preview->secondary_regions_ready() && m_auto_detail->GetCount() > 1;
    m_preview_protected_row->Show(m_details_open && secondary_ready);
    m_preview_protected->Enable(active && secondary_ready);
    m_operation->Enable(available);
    m_auto_match->SetToolTip(semantic
        ? _L("按当前已缓存的人像语义结果选择区域，不会重新识别。")
        : m_preview ? m_preview->semantic_region_status() : _L("当前没有已加载模型。"));
    m_reoptimize->Enable(active && m_preview && m_preview->semantic_reoptimization_available());
    m_reoptimize->SetToolTip(m_preview && !m_preview->semantic_reoptimization_available()
        ? m_preview->semantic_reoptimization_reason()
        : _L("显式重新识别人像区域；选区与保护区保持不变。"));
    const bool deform = geometry_deform_selected();
    m_displacement_label->SetLabel(wxString::Format(_L("推拉距离 · %.1f mm"), displacement_mm()));
    m_falloff_label->SetLabel(wxString::Format(_L("边缘衰减 · %.1f mm"), falloff_mm()));
    m_displacement->Show(deform);
    m_falloff->Show(deform);
    m_displacement_label->Show(deform);
    m_falloff_label->Show(deform);
    m_displacement->Enable(active);
    m_falloff->Enable(active);
    m_preview_button->Enable(active);
    m_accept->Enable(available && m_candidate_ready);
    m_discard->Enable(available && m_candidate_ready);
    m_cancel->Enable(m_processing || bool(m_partition_task) || selection_calculating);
    m_details_toggle->SetLabel(m_details_open ? _L("收起语义区域与分区") : _L("语义区域与分区"));
    for (wxWindow* control : std::initializer_list<wxWindow*>{m_secondary_status, m_import_secondary,
             m_regenerate_evidence, m_auto_partition, m_pick_partition, m_apply_partition,
             m_auto_region, m_auto_match, m_reoptimize})
        control->Show(m_details_open);
    m_auto_detail->Show(m_details_open && m_auto_detail->GetCount() > 1);
    const wxString locked_reason = _L("当前正在计算分区或选区边界，请等待完成或取消。");
    for (wxWindow* control : std::initializer_list<wxWindow*>{m_auto_partition, m_pick_partition,
                              m_apply_partition, m_boundary, m_operation, m_preview_button,
                              m_save, m_accept, m_discard, m_undo, m_redo})
        control->SetToolTip(selection_calculating || m_partition_task ? locked_reason : wxEmptyString);
    if (selection_calculating || m_partition_task) {
        m_auto_region->SetToolTip(locked_reason);
        m_auto_match->SetToolTip(locked_reason);
        m_reoptimize->SetToolTip(locked_reason);
        m_color_slot->SetToolTip(locked_reason);
    } else {
        m_auto_region->SetToolTip(wxEmptyString);
        m_color_slot->SetToolTip(_L("将当前分区绑定到当前有效耗材槽位；不修改工程耗材。"));
    }
    for (auto* label : {m_status, m_secondary_status}) wrap_status(label);
    Layout();
    if (m_layout_changed) m_layout_changed();
}

void BeautyWorkbenchControls::wrap_status(wxStaticText* label)
{
    wxString source = label->GetLabel(), line, wrapped;
    source.Replace("\r", "");
    source.Replace("\n", "");
    const int width = GetClientSize().x > 0
        ? std::max(FromDIP(80), GetClientSize().x) : FromDIP(240);
    for (size_t offset = 0; offset < source.length();) {
        wxString token;
        const auto is_digit = [](wxUniChar value) { return value >= '0' && value <= '9'; };
        token += source[offset++];
        if (is_digit(token[0])) {
            while (offset < source.length() && is_digit(source[offset]))
                token += source[offset++];
        }
        wxString next = line + token;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            wrapped += line + "\n"; line.clear();
        }
        for (wxUniChar character : token) {
            next = line; next += character;
            if (!line.empty() && label->GetTextExtent(next).x > width) {
                wrapped += line + "\n"; line.clear();
            }
            line += character;
        }
    }
    wrapped += line;
    label->SetLabel(wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    label->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + FromDIP(2)));
}

bool BeautyWorkbenchControls::geometry_deform_selected() const { return m_operation && m_operation->GetSelection() == 4; }
int BeautyWorkbenchControls::operation_index() const { return m_operation ? m_operation->GetSelection() : 0; }

void BeautyWorkbenchControls::attach_parameters(wxWindow* surface, wxWindow* strength_label, wxWindow* strength)
{
    auto* parameters = surface->GetSizer();
    for (wxWindow* control : std::initializer_list<wxWindow*>{
             m_displacement_label, m_displacement, m_falloff_label, m_falloff,
             strength_label, strength, m_lighting_row, m_original_view_row}) {
        control->GetContainingSizer()->Detach(control);
        control->Reparent(surface);
        parameters->Add(control, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, surface->FromDIP(12));
    }
}

void BeautyWorkbenchControls::attach_actions(wxWindow* footer)
{
    auto* actions = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
    for (Button* button : {m_preview_button, m_accept, m_discard, m_cancel, m_undo, m_redo, m_save}) {
        button->GetContainingSizer()->Detach(button);
        button->Reparent(footer);
        if (button == m_undo || button == m_redo) {
            const wxString label = button->GetLabel();
            button->SetName(label);
            button->SetToolTip(label);
            button->SetLabel(wxEmptyString);
            button->SetIcon(button == m_undo ? "topbar_undo" : "topbar_redo");
            button->SetMinSize(FromDIP(wxSize(32, 32)));
            button->SetMaxSize(FromDIP(wxSize(32, 32)));
        } else {
            button->SetMinSize(FromDIP(wxSize(92, 32)));
        }
        actions->Add(button, 0, wxRIGHT | wxBOTTOM, FromDIP(4));
    }
    footer->SetSizer(actions);
}

void BeautyWorkbenchControls::set_history_permissions(bool undo, bool redo)
{
    m_can_undo = undo;
    m_can_redo = redo;
    update_text();
}
double BeautyWorkbenchControls::displacement_mm() const { return m_displacement ? 0.1 * m_displacement->GetValue() : 0.; }
double BeautyWorkbenchControls::falloff_mm() const { return m_falloff ? 0.1 * m_falloff->GetValue() : 1.; }

BeautyWorkbenchControls::~BeautyWorkbenchControls()
{
    m_partition_timer.Stop();
    cancel_partition();
    if (m_partition_worker.joinable()) m_partition_worker.join();
    if (m_preview) m_preview->set_beauty_pick_callback({});
}

void BeautyWorkbenchControls::cancel_partition()
{
    if (m_partition_task) m_partition_task->canceled.store(true);
}

void BeautyWorkbenchControls::start_partition(const nlohmann::json& saved)
{
    if (!m_ready || m_partition_task || m_processing) return;
    const auto pending_source = std::move(m_pending_secondary_partition_source);
    m_pending_secondary_partition_source.clear();
    if (on_partition_started && !on_partition_started()) {
        m_pending_secondary_partition_source = pending_source;
        return;
    }
    if (m_partition_worker.joinable()) m_partition_worker.join();
    auto task = m_partition_task = std::make_shared<PartitionTask>();
    task->geometry = m_geometry_id;
    task->saved = saved;
    task->restoring = saved.is_object() && !saved.empty();
    task->before_selection = m_preview->selection_state();
    const auto surface = pending_source.empty() ? m_surface : nullptr;
    const auto editor = m_preview->beauty_editor();
    const auto labels = m_preview->beauty_semantic_labels();
    const auto semantic_names = m_preview->beauty_semantic_names();
    try {
        m_partition_worker = std::thread([task, surface, editor, labels, semantic_names] {
            try {
                task->surface = surface ? surface : AI::BeautySurface::build(editor->mesh(), editor->vertex_colors(), {},
                    [task] { return task->canceled.load(); });
                if (task->restoring) {
                    task->result = AI::BeautyPuzzle::decode(task->saved, task->geometry,
                        editor->mesh().indices.size());
                    task->result.validate(*task->surface);
                } else task->result = AI::BeautyPuzzle::create_regions(*task->surface, labels,
                    [task] { return task->canceled.load(); }, semantic_names);
            } catch (const std::exception& error) { task->error = error.what(); }
            task->done.store(true);
        });
        m_partition_timer.Start(100);
        update_text();
    } catch (const std::exception& error) {
        m_partition_task.reset();
        if (on_partition_finished) on_partition_finished(false);
        m_status->SetLabel(_L("无法启动自动划区：") + wxString::FromUTF8(error.what()));
    }
}

void BeautyWorkbenchControls::finish_partition()
{
    if (!m_partition_task || !m_partition_task->done.load()) return;
    m_partition_timer.Stop();
    if (m_partition_worker.joinable()) m_partition_worker.join();
    auto task = std::move(m_partition_task);
    const bool success = !task->canceled && task->error.empty() && task->geometry == m_geometry_id && task->surface;
    if (success) {
        m_surface = std::move(task->surface);
        m_document.geometry_id = m_geometry_id;
        m_document.face_count = m_surface->face_patch.size();
        m_document.face_patch = m_surface->face_patch;
        auto before = m_partition;
        m_partition = std::move(task->result);
        m_selected_piece = 0;
        m_preview->set_beauty_partition(m_partition->face_piece, m_surface->face_neighbors);
        m_preview->set_beauty_pick_callback([this](size_t face) { select_piece(face); });
        if (on_pick_mode) on_pick_mode();
        if (on_partition_finished) on_partition_finished(true);
        if (on_record && !task->restoring) {
            const auto after = *m_partition;
            on_record("automatic partition",
                [this, before, selection = task->before_selection] {
                    if (before) restore_partition(*before, 0);
                    else clear_partition(selection);
                },
                [this, after] { restore_partition(after, 0); });
        }
    }
    if (!success && on_partition_finished) on_partition_finished(false);
    update_text();
    if (success && m_partition)
        m_status->SetLabel(wxString::Format(_L("已划分 %llu 个区域、%llu 个面；点击绿色边线内的区域继续编辑。"),
            static_cast<unsigned long long>(m_partition->piece_count()),
            static_cast<unsigned long long>(m_partition->face_piece.size())));
    if (!success) m_status->SetLabel(task->canceled ? _L("已取消划区，原有分区不变。")
        : _L("划区未完成，原有分区不变：") + wxString::FromUTF8(task->error));
}

void BeautyWorkbenchControls::clear_partition(
    const AI::SurfaceSelectionPersistence::SelectionState& selection)
{
    m_partition.reset();
    m_selected_piece = 0;
    m_preview->set_beauty_partition({}, {});
    if (selection.selected.size() == m_preview->triangle_count())
        m_preview->restore_selection_state(selection);
    update_text();
}

void BeautyWorkbenchControls::restore_partition(const AI::BeautyPuzzle& puzzle, uint32_t selected)
{
    if (!m_surface || puzzle.geometry_id != m_geometry_id) return;
    m_partition = puzzle;
    m_selected_piece = selected;
    m_preview->set_beauty_partition(puzzle.face_piece, m_surface->face_neighbors);
    if (selected) {
        const auto faces = puzzle.faces(selected);
        auto state = m_preview->selection_state();
        state.selected.assign(puzzle.face_piece.size(), 0);
        for (size_t face : faces) if (face < state.selected.size()) state.selected[face] = 1;
        state.foreground = state.selected;
        state.domain = state.selected;
        m_preview->restore_selection_state(std::move(state));
    }
    update_text();
}

void BeautyWorkbenchControls::select_piece(size_t face)
{
    if (!m_partition || m_partition_task || face >= m_partition->face_piece.size()) return;
    const auto before = m_preview->selection_state();
    const uint32_t previous = m_selected_piece;
    m_selected_piece = m_partition->face_piece[face];
    auto state = before;
    state.selected.assign(m_partition->face_piece.size(), 0);
    for (size_t i = 0; i < state.selected.size(); ++i)
        if (m_partition->face_piece[i] == m_selected_piece &&
            (i >= state.protected_faces.size() || !state.protected_faces[i])) state.selected[i] = 1;
    state.foreground = state.selected;
    state.domain = state.selected;
    m_preview->restore_selection_state(std::move(state));
    const auto after = m_preview->selection_state();
    if (on_record && (before.selected != after.selected || previous != m_selected_piece)) {
        const uint32_t chosen = m_selected_piece;
        on_record("select partition",
            [this, before, previous] {
                m_selected_piece = previous;
                m_preview->restore_selection_state(before);
                update_text();
            },
            [this, after, chosen] {
                m_selected_piece = chosen;
                m_preview->restore_selection_state(after);
                update_text();
            });
    }
    update_text();
    m_status->SetLabel(wxString::Format(_L("已选择分区 %u · %llu 个面；可补选、保护、改色或拉伸。"),
        m_selected_piece, static_cast<unsigned long long>(m_preview->selected_face_count())));
}

void BeautyWorkbenchControls::apply_partition_selection()
{
    if (!m_partition || !m_surface || !m_editable || m_partition_task) return;
    const auto state = m_preview->selection_state();
    if (state.selected.size() != m_partition->face_piece.size()) return;
    std::vector<size_t> selected, added, removed;
    for (size_t face = 0; face < state.selected.size(); ++face) {
        const bool protected_face = face < state.protected_faces.size() && state.protected_faces[face];
        if (state.selected[face] && !protected_face) selected.push_back(face);
        if (m_selected_piece && !protected_face) {
            if (state.selected[face] && m_partition->face_piece[face] != m_selected_piece) added.push_back(face);
            if (!state.selected[face] && m_partition->face_piece[face] == m_selected_piece) removed.push_back(face);
        }
    }
    if (selected.empty()) { m_status->SetLabel(_L("先在模型上圈选分区范围。")); return; }
    const auto before = *m_partition;
    const uint32_t previous = m_selected_piece;
    try {
        if (m_selected_piece && (!added.empty() || !removed.empty()))
            m_partition->reshape_region(m_selected_piece, added, removed, *m_surface, {}, true);
        else if (!m_selected_piece) m_selected_piece = m_partition->assign_region(selected, *m_surface);
        else return;
        m_preview->set_beauty_partition(m_partition->face_piece, m_surface->face_neighbors);
        const auto after = *m_partition;
        const uint32_t chosen = m_selected_piece;
        if (on_record) on_record("edit partition",
            [this, before, previous] { restore_partition(before, previous); },
            [this, after, chosen] { restore_partition(after, chosen); });
        m_dirty = true;
        update_text();
    } catch (const std::exception& error) {
        *m_partition = before;
        m_selected_piece = previous;
        m_status->SetLabel(_L("边界未修改：") + wxString::FromUTF8(error.what()));
    }
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
    if (!m_preview || m_source.empty()) return;
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
    auto document = m_document;
    if (!m_surface) {
        // The finishing worker builds the surface when no partition cache
        // exists, so a manual selection can be deformed directly.
        document.geometry_id = m_geometry_id;
        document.face_count = editor->mesh().indices.size();
    }
    options.beauty_document = document.encode();
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

nlohmann::json BeautyWorkbenchControls::partition_metadata() const
{
    return m_partition ? m_partition->encode() : nlohmann::json::object();
}

} // namespace Slic3r::GUI
