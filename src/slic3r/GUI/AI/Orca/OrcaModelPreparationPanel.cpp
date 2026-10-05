#include "OrcaModelPreparationPanel.hpp"
#include "OrcaModelPreparation.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <cmath>

namespace Slic3r::GUI {

OrcaModelPreparationPanel::OrcaModelPreparationPanel(wxWindow* parent, Plater& plater,
    std::function<bool()> busy, std::function<void()> changed, std::function<void()> expanded)
    : wxPanel(parent), m_plater(plater), m_busy(std::move(busy)), m_changed(std::move(changed)),
      m_expanded(std::move(expanded)), m_timer(this)
{
    SetName("ai_model_preparation");
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(this, wxID_ANY, _L("调整当前模型（可跳过）"));
    title->SetFont(wxGetApp().bold_font());
    root->Add(title, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_context = new Label(this, "", LB_AUTO_WRAP);
    m_context->SetMinSize(wxSize(1, -1));
    root->Add(m_context, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    root->Add(new wxStaticText(this, wxID_ANY, _L("总高度（mm，含底座）")), 0, wxBOTTOM, FromDIP(4));
    m_height = new wxTextCtrl(this, wxID_ANY, "120");
    root->Add(m_height, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_base = new wxCheckBox(this, wxID_ANY, _L("添加预制底座（应用后生效）"));
    m_base->SetToolTip(_L("底座在模型生成后作为独立部件加入，可撤销、替换并随 3MF 保存。请在原生预览中检查连接与支撑。"));
    root->Add(m_base, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_base_template = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
        FromDIP(wxSize(-1, 36)), 0, nullptr, wxCB_READONLY);
    m_base_template->SetName("input_field");
    m_base_template->Append(_L("圆形底座"));
    m_base_template->Append(_L("椭圆底座"));
    m_base_template->Append(_L("矩形底座"));
    m_base_template->SetSelection(0);
    m_base_template->Enable(false);
    root->Add(m_base_template, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_apply = new Button(this, _L("应用尺寸与底座"));
    m_apply->SetName("input_field");
    m_apply->SetPaddingSize(FromDIP(wxSize(12, 8)));
    m_apply->SetMinSize(FromDIP(wxSize(-1, 36)));
    root->Add(m_apply, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_feedback = new Label(this, _L("编辑保存在当前工程，可使用 Orca 撤销。生成原件保留在模型库。"), LB_AUTO_WRAP);
    m_feedback->SetMinSize(wxSize(1, -1));
    m_feedback_text = m_feedback->GetLabel();
    root->Add(m_feedback, 0, wxEXPAND);
    SetSizer(root);
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
    m_base->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        m_base_template->Enable(m_base->GetValue());
        m_requested_base_object_id = m_base->GetValue() ? m_displayed_object_id : 0;
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (IsShownOnScreen()) refresh(); }, m_timer.GetId());
    m_timer.Start(1000);
    apply_ai_theme(true);
    refresh();
}

void OrcaModelPreparationPanel::apply_ai_theme(bool update_fonts)
{
    ModelGenerationInputStyle::apply(this, update_fonts);
    Refresh(false);
}

int OrcaModelPreparationPanel::editable_object(wxString& reason) const
{
    if ((m_busy && m_busy()) || m_plater.is_background_process_slicing() ||
        m_plater.get_view3D_canvas3D()->get_gizmos_manager().is_running()) {
        reason = _L("请先结束切片或当前编辑工具。");
        return -1;
    }
    if (m_plater.printer_technology() != ptFFF) {
        reason = _L("此准备操作用于当前 FDM 工程。");
        return -1;
    }
    const int index = m_plater.get_selected_object_idx();
    auto* plate = m_plater.get_partplate_list().get_curr_plate();
    if (index < 0 || static_cast<size_t>(index) >= m_plater.model().objects.size() ||
        !m_plater.get_selection().is_single_full_instance() ||
        m_plater.model().objects[index]->instances.size() != 1) {
        reason = _L("请在当前打印板完整选中一个只有单份实例的模型。");
        return -1;
    }
    if (plate == nullptr || plate->is_locked() || !plate->contain_instance(index, 0)) {
        reason = _L("请选中当前未锁定打印板上的模型。");
        return -1;
    }
    return index;
}

void OrcaModelPreparationPanel::refresh()
{
    wxString context;
    const int index = editable_object(context);
    if (index >= 0) {
        const auto* object = m_plater.model().objects[index];
        const double actual_height = object->instance_bounding_box(0).size().z();
        if (m_displayed_object_id != object->id().id) {
            // A different object must not inherit another object's dimensions
            // or pending base choice. Neither selecting nor suggesting edits it.
            m_height->ChangeValue(wxString::Format("%.3f", actual_height));
            m_base->SetValue(m_requested_base_object_id == object->id().id);
            m_base_template->SetSelection(0);
            m_base_template->Enable(m_base->GetValue());
            show_feedback(m_base->GetValue()
                ? _L("此作品选择了添加底座。请确认模板和含底座总高度，再点击应用；当前几何尚未改变。")
                : _L("已切换当前模型，尺寸取自该模型。确认后再应用；生成原件保留在模型库。"));
        }
        else if (std::abs(actual_height - m_displayed_height_mm) > 1e-5 ||
                 object->volumes.size() != m_displayed_volume_count) {
            // Native Undo/Redo can change the same object's geometry. Keep its
            // pending base request, but do not retain feedback for old geometry.
            m_height->ChangeValue(wxString::Format("%.3f", actual_height));
            show_feedback(_L("当前工程的模型已更新。已刷新实际高度，请确认后再应用。"));
        }
        m_displayed_height_mm = actual_height;
        m_displayed_volume_count = object->volumes.size();
        m_displayed_object_id = object->id().id;
        context = from_u8(object->name) + wxString::Format(_L(" · 当前高 %.1f mm"), object->instance_bounding_box(0).size().z());
    }
    if (wxGetApp().preset_bundle != nullptr)
        context += _L("\n设备：") + from_u8(wxGetApp().preset_bundle->printers.get_selected_preset().name) +
            _L("\n沿用工程材料与工艺。\n可在 Orca 原生设置中调整。");
    m_context->SetLabel(context);
    m_apply->Enable(index >= 0);
    Layout();
    GetParent()->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(GetParent())) scroll->FitInside();
}

void OrcaModelPreparationPanel::suggest_base_for_object(uint64_t object_id)
{
    m_requested_base_object_id = object_id;
    m_displayed_object_id = 0;
    show_feedback(_L("此作品选择了添加底座。请完整选中该模型，确认模板和含底座总高度，再点击应用；当前几何尚未改变。"));
    Show();
    if (m_expanded) m_expanded();
    refresh();
}

void OrcaModelPreparationPanel::show_feedback(const wxString& message)
{
    m_feedback_text = message;
    m_feedback->SetLabel(message);
}

void OrcaModelPreparationPanel::apply()
{
    wxString feedback;
    const int index = editable_object(feedback);
    double height = 0;
    if (index < 0) {
        show_feedback(feedback);
    } else if (m_plater.model().objects[index]->id().id != m_displayed_object_id) {
        show_feedback(_L("选中的模型已变化。请确认当前模型和高度后再次应用。"));
    } else if (!m_height->GetValue().ToDouble(&height) || !std::isfinite(height) || height < 1 || height > 1000 ||
               (m_base->GetValue() && height <= 3)) {
        show_feedback(_L("请输入 1–1000 mm 的高度；含底座时须大于 3 mm。"));
        m_height->SetFocus();
    } else if (!m_base->GetValue() &&
               std::abs(m_plater.model().objects[index]->instance_bounding_box(0).size().z() - height) < 1e-5) {
        show_feedback(_L("当前模型已是此高度，无需再次应用。"));
    } else {
        m_apply->Disable();
        bool transaction_started = false;
        try {
            auto* object = m_plater.model().objects[index];
            const ModelBaseTemplate base_template =
                m_base_template->GetSelection() == 1 ? ModelBaseTemplate::Oval :
                m_base_template->GetSelection() == 2 ? ModelBaseTemplate::Rectangle :
                ModelBaseTemplate::Round;
            auto proposal = prepare_model(*object, {height, m_base->GetValue(), 3.0, base_template});
            {
                Plater::TakeSnapshot snapshot(&m_plater, _u8L("调整模型尺寸与底座"));
                transaction_started = true;
                apply_model_preparation(*object, proposal);
                m_plater.select_view_3D("3D");
                auto* list = m_plater.sidebar().obj_list();
                if (object->volumes.size() > 1)
                    list->reorder_volumes_and_get_selection(index);
                else
                    m_plater.changed_object(index);
                list->notify_instance_updated(index);
                list->select_item(ObjectVolumeID{object, nullptr});
                list->update_plate_values_for_items();
                m_plater.show_object_info();
                save_object_mesh(*object);
                m_plater.update_title_dirty_status();
            }
            // Preserve this successful operation's feedback on the final refresh.
            const auto* updated_object = m_plater.model().objects[index];
            m_displayed_height_mm = updated_object->instance_bounding_box(0).size().z();
            m_displayed_volume_count = updated_object->volumes.size();
            m_base->SetValue(false);
            m_requested_base_object_id = 0;
            m_base_template->Enable(false);
            show_feedback(_L("已更新当前模型。请重新检查打印适配；可在 Orca 撤销，或保存 3MF 保留编辑版本。"));
            if (m_changed) m_changed();
        } catch (const std::exception& error) {
            if (transaction_started && m_plater.can_undo()) m_plater.undo();
            show_feedback(std::string(error.what()) == "base_already_exists" ?
                _L("该模型已有此底座。可以只调整高度，或使用 Orca 撤销后重新添加。") :
                _L("未能应用。请检查模型是否有效且未带切割连接件；原件仍保留，可以调整后重试。"));
        }
    }
    refresh();
    GetParent()->Layout();
}

} // namespace Slic3r::GUI
