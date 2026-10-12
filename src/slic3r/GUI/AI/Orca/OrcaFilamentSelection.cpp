#include "OrcaFilamentSelection.hpp"
#include "WorkbenchPrinterFilaments.hpp"
#include "slic3r/GUI/FilamentPickerDialog.hpp"
#include <wx/choicdlg.h>
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include <wx/colordlg.h>
#include <wx/vlbox.h>
#include <wx/msgdlg.h>

namespace Slic3r::GUI {
namespace {
// The standard list box uses the OS blue selection and very short rows. Keep
// native list keyboard/scroll behavior, with the design's 32 DIP item geometry.
class FilamentChoiceList final : public wxVListBox {
public:
    explicit FilamentChoiceList(wxWindow* parent)
        : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {}
    void Clear() { m_labels.clear(); m_colors.clear(); SetItemCount(0); }
    void Append(const wxString& label) { m_labels.push_back(label); SetItemCount(m_labels.size()); }
    void SetLabels(std::vector<wxString> labels, std::vector<wxColour> colors = {}) {
        m_labels = std::move(labels);
        m_colors = std::move(colors);
        if (GetItemCount() != m_labels.size()) SetItemCount(m_labels.size());
        RefreshAll();
    }
private:
    wxCoord OnMeasureItem(size_t) const override { return FromDIP(m_colors.empty() ? 32 : 48); }
    void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t item) const override {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(IsSelected(item) ? wxColour(62, 62, 65) : ModelGenerationInputStyle::panel));
        dc.DrawRectangle(rect);
        if (IsSelected(item) && HasFocus()) {
            dc.SetPen(wxPen(ModelGenerationInputStyle::yellow));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(wxRect(rect).Deflate(1));
        }
    }
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t item) const override {
        dc.SetFont(GetFont()); dc.SetTextForeground(ModelGenerationInputStyle::text);
        dc.SetClippingRegion(rect);
        const bool has_swatch = item < m_colors.size();
        if (has_swatch) {
            const wxRect swatch(rect.x + FromDIP(10), rect.y + (rect.height - FromDIP(24)) / 2, FromDIP(24), FromDIP(24));
            dc.SetPen(wxPen(wxColour(140, 140, 145)));
            dc.SetBrush(wxBrush(m_colors[item].IsOk() ? m_colors[item] : wxColour(70, 70, 75)));
            dc.DrawRectangle(swatch);
            if (!m_colors[item].IsOk()) dc.DrawLine(swatch.GetTopLeft(), swatch.GetBottomRight());
        }
        const int inset = FromDIP(has_swatch ? 44 : 10);
        const auto text = wxControl::Ellipsize(m_labels[item], dc, wxELLIPSIZE_END, std::max(1, rect.width - inset - FromDIP(10)));
        dc.DrawText(text, rect.x + inset, rect.y + (rect.height - dc.GetMultiLineTextExtent(text).y) / 2);
        dc.DestroyClippingRegion();
    }
    std::vector<wxString> m_labels;
    std::vector<wxColour> m_colors;
};
}
std::optional<size_t> choose_single_color_filament(wxWindow* parent, Plater& plater)
{
    using namespace ModelGenerationInputStyle;
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return std::nullopt;
    const auto initial_project = bundle->project_config;
    const auto initial_printer = bundle->printers.get_edited_preset().config;
    const auto initial_presets = bundle->filament_presets;
    std::vector<std::string> colors, names, types;
    std::vector<size_t> indices, selectable;
    plater.sidebar().collect_physical_filament_info(colors, names, types, &indices);
    wxDialog dialog(parent, wxID_ANY, _L("单色导入：选择耗材"), wxDefaultPosition,
        wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new Label(&dialog, _L("为整个模型选择一个实体耗材槽"));
    auto* note = new Label(&dialog, _L("单色导入不保留多色分配。请选择耗材后确认；返回仍可调整原配色，不修改工程或材料。"),
        LB_AUTO_WRAP);
    note->SetMinSize(wxSize(1, -1));
    auto* choice = new ComboBox(&dialog, wxID_ANY, _L("请选择实体耗材"), wxDefaultPosition,
        dialog.FromDIP(wxSize(430, 42)), 0, nullptr, wxCB_READONLY);
    for (size_t i = 0; i < indices.size(); ++i) {
        const size_t slot = indices[i];
        if (slot >= initial_presets.size() || i >= colors.size() || i >= names.size() || i >= types.size()) continue;
        const auto* preset = bundle->filaments.find_preset(initial_presets[slot]);
        if (!preset || !preset->is_compatible || types[i].empty() || !wxColour(from_u8(colors[i])).IsOk()) continue;
        selectable.push_back(slot);
        choice->Append(wxString::Format(_L("耗材 %llu"), static_cast<unsigned long long>(slot + 1)) +
            wxString::FromUTF8(" · ") + from_u8(colors[i]) + wxString::FromUTF8(" · ") + from_u8(types[i]) + wxString::FromUTF8(" · ") + from_u8(names[i]));
    }
    choice->SetSelection(wxNOT_FOUND);
    choice->SetValue(_L("请选择实体耗材"));
    auto* status = new Label(&dialog, selectable.empty()
        ? _L("当前没有兼容的实体耗材。请返回并核对打印机、材料和工艺。")
        : _L("尚未选择耗材。槽号保留，同色耗材不会合并。"), LB_AUTO_WRAP);
    status->SetMinSize(wxSize(1, -1));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* back = new Button(&dialog, _L("返回配色"));
    auto* confirm = new Button(&dialog, _L("确认单色并导入"));
    confirm->Enable(false);
    for (auto* control : {back, confirm}) {
        control->SetMinSize(dialog.FromDIP(wxSize(180, 44)));
        actions->Add(control, 0, wxALL, dialog.FromDIP(8));
    }
    for (wxWindow* control : std::array<wxWindow*, 4>{title, note, choice, status})
        root->Add(control, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    root->Add(actions, 0, wxALIGN_RIGHT | wxALL, dialog.FromDIP(8));
    choice->Bind(wxEVT_COMBOBOX, [&](wxCommandEvent&) {
        confirm->Enable(choice->GetSelection() != wxNOT_FOUND);
        apply_control(confirm, Role::PrimaryAction);
        status->SetLabel(_L("确认后整个模型使用所选槽位；原模型文件保持。"));
        dialog.Layout();
    });
    back->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    confirm->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        const int row = choice->GetSelection();
        if (row < 0 || static_cast<size_t>(row) >= selectable.size()) return;
        if (bundle->project_config != initial_project ||
            bundle->printers.get_edited_preset().config != initial_printer || bundle->filament_presets != initial_presets) {
            status->SetLabel(_L("工程配置已变化，请返回配色后重新选择耗材。"));
            confirm->Enable(false);
            apply_control(confirm, Role::PrimaryAction);
            dialog.Layout();
            return;
        }
        dialog.EndModal(wxID_OK);
    });
    dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    dialog.SetSizer(root);
    // Explicit roles: content and actions do not depend on recursive theme order.
    apply_control(&dialog, Role::Panel, true);
    apply_control(title, Role::Panel, true);
    apply_control(note, Role::Secondary, true);
    apply_control(status, Role::Secondary, true);
    apply_control(back, Role::QuietAction, true);
    apply_control(confirm, Role::PrimaryAction, true);
    apply(choice);
    dialog.SetMinSize(dialog.FromDIP(wxSize(510, 310)));
    dialog.SetSize(dialog.FromDIP(wxSize(540, 350)));
    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return selectable[static_cast<size_t>(choice->GetSelection())];
}

void show_orca_filament_selection(wxWindow* parent, Plater& plater, size_t selected_slot, bool add_slot,
    const std::vector<FilamentColorPack>& import_sources)
{
    using namespace ModelGenerationInputStyle;
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return;
    const auto initial_project = bundle->project_config;
    const auto initial_printer = bundle->printers.get_edited_preset().config;
    const auto initial_presets = bundle->filament_presets;
    const auto initial = PrinterFilamentConfig::capture(*bundle);
    PresetBundle staged(*bundle);
    // Normalize only the draft. Empty values remain pending, not default spools.
    const auto* old_colors = initial_project.option<ConfigOptionStrings>("filament_colour");
    const size_t described = old_colors ? old_colors->values.size() : 0;
    const size_t normalized_count = std::max(initial_presets.size(), initial.nozzles);
    PrinterFilamentConfig::resize(staged, normalized_count);
    for (size_t i = described; i < staged.filament_presets.size(); ++i) {
        staged.project_config.option<ConfigOptionStrings>("filament_colour")->values[i].clear();
        staged.filament_presets[i].clear();
    }
    std::vector<PrinterFilamentConfig::SlotChange> slot_changes;
    for (size_t i = initial_presets.size(); i < staged.filament_presets.size(); ++i) slot_changes.push_back({{i}, -1, -1});
    while (PrinterFilamentConfig::capture(staged).physical_count < initial.nozzles &&
           staged.filament_presets.size() < size_t(EnforcerBlockerType::ExtruderMax)) {
        const size_t before = PrinterFilamentConfig::capture(staged).physical_count;
        const size_t at = PrinterFilamentConfig::add(staged);
        if (PrinterFilamentConfig::capture(staged).physical_count <= before) break;
        slot_changes.push_back({{at}, -1, -1});
    }
    if (add_slot && staged.filament_presets.size() < size_t(EnforcerBlockerType::ExtruderMax)) {
        selected_slot = PrinterFilamentConfig::add(staged); slot_changes.push_back({{selected_slot}, -1, -1});
    }
    auto state = PrinterFilamentConfig::capture(staged);
    auto drafts = state.slots;

    wxDialog dialog(wxGetTopLevelParent(parent), wxID_ANY, _L("配置打印机耗材"), wxDefaultPosition,
        wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new Label(&dialog, from_u8(initial.printer) + wxString::Format(_L(" · %llu 个喷嘴"),
        static_cast<unsigned long long>(initial.nozzles)));
    root->Add(title, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
    auto* note = new Label(&dialog, _L("先选择兼容的材料预设，再选择实际装料色号或自定义装料颜色。完成一次提交；取消不改变工程。"), LB_AUTO_WRAP);
    note->SetMinSize(wxSize(1, -1)); root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(16));
    wxChoice* import_choice = nullptr;
    Button* import_button = nullptr;
    if (!import_sources.empty()) {
        auto* import_row = new wxBoxSizer(wxHORIZONTAL);
        import_choice = new wxChoice(&dialog, wxID_ANY);
        import_choice->Append(_L("选择自动建议、手动试色或已保存色卡"));
        for (const auto& source : import_sources) import_choice->Append(from_u8(source.name));
        import_choice->SetSelection(0);
        import_button = new Button(&dialog, _L("导入到配置草稿"));
        import_button->SetMinSize(dialog.FromDIP(wxSize(190, 42)));
        import_row->Add(import_choice, 1, wxEXPAND | wxRIGHT, dialog.FromDIP(8));
        import_row->Add(import_button, 0);
        root->Add(import_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    auto* slots = new FilamentChoiceList(&dialog);
    auto* presets = new FilamentChoiceList(&dialog);
    slots->SetMinSize(dialog.FromDIP(wxSize(185, 250)));
    presets->SetMinSize(dialog.FromDIP(wxSize(285, 250)));
    columns->Add(slots, 1, wxEXPAND | wxALL, dialog.FromDIP(8));
    columns->Add(presets, 2, wxEXPAND | wxALL, dialog.FromDIP(8));
    auto* color_panel = new wxPanel(&dialog);
    auto* color_sizer = new wxBoxSizer(wxVERTICAL);
    auto* swatch = new wxPanel(color_panel);
    swatch->SetName("ai_content_color"); swatch->SetMinSize(dialog.FromDIP(wxSize(160, 70)));
    auto* color_text = new Label(color_panel, "", LB_AUTO_WRAP);
    color_text->SetMinSize(wxSize(1, -1));
    auto* catalog = new Button(color_panel, _L("选择实物色号…"));
    auto* custom = new Button(color_panel, _L("自定义装料颜色…"));
    for (auto* control : {catalog, custom}) control->SetMinSize(dialog.FromDIP(wxSize(175, 44)));
    for (wxWindow* control : std::array<wxWindow*,4>{swatch,color_text,catalog,custom})
        color_sizer->Add(control, 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    color_panel->SetSizer(color_sizer); columns->Add(color_panel, 1, wxEXPAND);
    root->Add(columns, 1, wxEXPAND | wxALL, dialog.FromDIP(8));
    auto* status = new Label(&dialog, "", LB_AUTO_WRAP);
    status->SetMinSize(wxSize(1, -1)); root->Add(status, 0, wxEXPAND | wxALL, dialog.FromDIP(16));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* add = new Button(&dialog, _L("增加耗材"));
    auto* cancel = new Button(&dialog, _L("取消"));
    auto* done = new Button(&dialog, _L("完成"));
    for (auto* control : {add,cancel,done}) {
        control->SetMinSize(dialog.FromDIP(wxSize(150,44))); actions->Add(control, 0, wxALL, dialog.FromDIP(8));
    }
    root->Add(actions, 0, wxALIGN_RIGHT | wxBOTTOM, dialog.FromDIP(8));
    std::vector<std::string> available;
    auto write_drafts = [&] {
        for (const auto& draft : drafts) {
            // Pending nozzle rows can outnumber the native table when
            // virtual slots have exhausted its capacity.
            if (!draft.exists || draft.slot >= staged.filament_presets.size()) continue;
            staged.filament_presets[draft.slot] = draft.preset;
            staged.project_config.option<ConfigOptionStrings>("filament_colour")->values[draft.slot] = draft.color;
        }
    };
    auto validity = [&] {
        write_drafts(); const auto current = PrinterFilamentConfig::capture(staged);
        done->Enable(current.ready);
        status->SetLabel(current.ready ? _L("已配置；完成后保存真实耗材并使旧匹配失效，请重新应用打印机耗材。") : from_u8(current.reason));
        add->Enable(staged.filament_presets.size() < size_t(EnforcerBlockerType::ExtruderMax));
        apply_control(done, Role::PrimaryAction);
    };
    auto update_slot_colors = [&] {
        std::vector<wxString> labels;
        std::vector<wxColour> colors;
        for (const auto& draft : drafts) {
            const wxColour color(from_u8(draft.color));
            colors.push_back(color);
            labels.push_back(wxString::Format(_L("耗材 %llu"), static_cast<unsigned long long>(draft.slot + 1)) +
                "\n" + (color.IsOk() ? from_u8(draft.color) : _L("待配置颜色")));
        }
        slots->SetLabels(std::move(labels), std::move(colors));
    };
    auto refresh = [&] {
        update_slot_colors();
        presets->Clear(); available.clear();
        const int row = slots->GetSelection();
        if (row == wxNOT_FOUND || size_t(row) >= drafts.size()) { catalog->Disable(); custom->Disable(); validity(); return; }
        auto& draft = drafts[size_t(row)];
        for (const auto& preset : bundle->filaments.get_presets()) {
            if (!preset.is_compatible || !preset.is_visible) continue;
            available.push_back(preset.name); presets->Append(from_u8(preset.name));
            if (preset.name == draft.preset) presets->SetSelection(int(available.size()-1));
        }
        const auto* chosen = bundle->filaments.find_preset(draft.preset, false);
        const bool compatible = chosen && chosen->is_compatible;
        catalog->Enable(compatible && !chosen->filament_id.empty()); custom->Enable(compatible);
        const wxColour color(from_u8(draft.color));
        swatch->SetBackgroundColour(color.IsOk() ? color : wxColour(70,70,75)); swatch->Refresh();
        color_text->SetLabel(draft.color.empty() ? _L("待配置装料颜色") : from_u8(draft.color) + "\n" + _L("实际颜色以装料为准"));
        validity(); dialog.Layout();
    };
    auto rebuild = [&] {
        const int previous = slots->GetSelection();
        update_slot_colors();
        if (!drafts.empty()) slots->SetSelection(std::min(std::max(0,previous),int(drafts.size()-1)));
        refresh();
    };
    auto import_source = [&](const FilamentColorPack& pack) {
        write_drafts();
        std::string error;
        const auto choose_replacement = [&](const WorkbenchFilamentSlot& source,
            const std::vector<WorkbenchFilamentSlot>& replacements) -> std::optional<size_t> {
            wxArrayString labels;
            for (const auto& target : replacements)
                labels.Add(wxString::Format(_L("耗材 %llu · "), static_cast<unsigned long long>(target.slot + 1)) +
                    from_u8(target.preset) + " · " + from_u8(target.color));
            wxSingleChoiceDialog replacement_dialog(&dialog,
                wxString::Format(_L("色卡将减少耗材 %llu。请选择替代耗材，对象、涂色和混色引用会一起更新；取消保留原配置草稿。"),
                    static_cast<unsigned long long>(source.slot + 1)), _L("按色卡减少耗材槽位"), labels);
            if (replacement_dialog.ShowModal() != wxID_OK) return std::nullopt;
            const int row = replacement_dialog.GetSelection();
            return row >= 0 && size_t(row) < replacements.size()
                ? std::optional<size_t>(replacements[size_t(row)].slot) : std::nullopt;
        };
        if (!PrinterFilamentConfig::import_color_pack(staged, pack, slot_changes, error, choose_replacement)) {
            status->SetLabel(from_u8(error)); dialog.Layout(); return;
        }
        drafts = PrinterFilamentConfig::capture(staged).slots;
        rebuild();
        const auto current = PrinterFilamentConfig::capture(staged);
        status->SetLabel(wxString::Format(_L("已导入 %llu 色，当前 %llu 个实体耗材槽；"),
            static_cast<unsigned long long>(pack.colors.size()), static_cast<unsigned long long>(current.physical_count)) +
            (current.ready ? _L("请核对实际装料后完成提交。") : from_u8(current.reason)));
        dialog.Layout();
    };
    rebuild();
    for (size_t i=0;i<drafts.size();++i) if(drafts[i].slot==selected_slot) slots->SetSelection(int(i));
    slots->Bind(wxEVT_LISTBOX,[&](wxCommandEvent&){refresh();});
    presets->Bind(wxEVT_LISTBOX,[&](wxCommandEvent&){
        const int row=slots->GetSelection(), choice=presets->GetSelection();
        if(row==wxNOT_FOUND || choice==wxNOT_FOUND) return;
        auto& draft=drafts[size_t(row)];
        draft.preset = available[size_t(choice)];
        refresh();
    });
    if (import_button) import_button->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        const int choice = import_choice->GetSelection();
        if (choice <= 0 || size_t(choice - 1) >= import_sources.size()) return;
        import_source(import_sources[size_t(choice - 1)]);
    });
    auto set_color = [&](const wxColour& color, const std::string& multi, const std::string& type) {
        const int row=slots->GetSelection(); if(row==wxNOT_FOUND || !color.IsOk()) return;
        auto& draft=drafts[size_t(row)]; draft.color=into_u8(color.GetAsString(wxC2S_HTML_SYNTAX));
        staged.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[draft.slot]=multi.empty()?draft.color:multi;
        staged.project_config.option<ConfigOptionStrings>("filament_colour_type")->values[draft.slot]=type;
        refresh();
    };
    custom->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){
        const int row=slots->GetSelection();if(row==wxNOT_FOUND)return;
        wxColourData data; const wxColour color(from_u8(drafts[size_t(row)].color));
        if(color.IsOk())data.SetColour(color);data.SetChooseFull(true);
        wxColourDialog picker(&dialog,&data);picker.SetTitle(_L("自定义实际装料颜色"));
        if(picker.ShowModal()==wxID_OK)set_color(picker.GetColourData().GetColour(),"","1");
    });
    catalog->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){
        const int row=slots->GetSelection();if(row==wxNOT_FOUND)return;
        const auto& draft=drafts[size_t(row)];const auto* preset=bundle->filaments.find_preset(draft.preset,false);if(!preset)return;
        FilamentColor current;const wxColour color(from_u8(draft.color));if(color.IsOk())current.m_colors.insert(color);
        FilamentPickerDialog picker(&dialog,from_u8(preset->filament_id),current,preset->name);
        if(!picker.IsDataLoaded()){status->SetLabel(_L("当前预设没有可用实物色号；可使用自定义装料颜色。"));dialog.Layout();return;}
        if(picker.ShowModal()!=wxID_OK)return;
        const auto selected=picker.GetSelectedFilamentColor();std::string multi;
        for(const auto& c:selected.m_colors){if(!multi.empty())multi+=' ';multi+=into_u8(c.GetAsString(wxC2S_HTML_SYNTAX));}
        set_color(picker.GetSelectedColour(),multi,selected.m_color_type==FilamentColor::ColorType::GRADIENT_CLR?"0":"1");
    });
    add->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){
        if(staged.filament_presets.size() >= size_t(EnforcerBlockerType::ExtruderMax)) return; write_drafts();
        const auto at=PrinterFilamentConfig::add(staged);slot_changes.push_back({{at}, -1, -1});
        drafts=PrinterFilamentConfig::capture(staged).slots;rebuild();slots->SetSelection(int(drafts.size()-1));refresh();
    });
    cancel->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){dialog.EndModal(wxID_CANCEL);});
    done->Bind(wxEVT_BUTTON,[&](wxCommandEvent&){
        if(bundle->project_config!=initial_project || bundle->filament_presets!=initial_presets ||
           bundle->printers.get_edited_preset().config!=initial_printer){status->SetLabel(_L("打印机或工程已变化，请取消并重新打开配置。"));done->Disable();dialog.Layout();return;}
        write_drafts();std::string error;
        const auto ready = PrinterFilamentConfig::capture(staged);
        if (!ready.ready) error = ready.reason;
        if(!ready.ready ||
           !plater.apply_printer_filament_config(staged,slot_changes,error)){
            status->SetLabel(_L("配置未提交，工程保持原状：")+from_u8(error));dialog.Layout();return;
        }
        dialog.EndModal(wxID_OK);
    });
    dialog.SetSizer(root);apply(&dialog);apply_control(done,Role::PrimaryAction);apply_control(cancel,Role::QuietAction);
    dialog.SetMinSize(dialog.FromDIP(wxSize(760,480)));dialog.SetSize(dialog.FromDIP(wxSize(830,550)));
    refresh();dialog.CentreOnParent();dialog.ShowModal();
}

void delete_orca_printer_filament(wxWindow* parent, Plater& plater, size_t slot)
{
    auto* bundle=wxGetApp().preset_bundle;if(!bundle)return;
    const auto initial=PrinterFilamentConfig::capture(*bundle);
    const auto source=std::find_if(initial.slots.begin(),initial.slots.end(),[slot](const auto& s){return s.slot==slot;});
    if(source==initial.slots.end() || !source->removable){wxMessageBox(_L("基础喷嘴槽位不能删除，只能删除额外实体耗材。"),_L("打印机耗材"),wxOK|wxICON_INFORMATION,parent);return;}
    const auto before=bundle->project_config;const auto names=bundle->filament_presets;
    wxArrayString labels;std::vector<size_t> targets;
    for(const auto& target:initial.slots)if(target.slot!=slot && target.exists && target.configured && target.compatible){
        PresetBundle staged(*bundle);std::string error;
        if(!PrinterFilamentConfig::remove(staged,slot,target.slot,error))continue;
        targets.push_back(target.slot);labels.Add(wxString::Format(_L("耗材 %llu · "),static_cast<unsigned long long>(target.slot+1))+from_u8(target.preset)+" · "+from_u8(target.color));
    }
    if(targets.empty()){wxMessageBox(_L("没有合法替代耗材；此槽位不能安全删除。请先配置兼容耗材或调整混色配方。"),_L("打印机耗材"),wxOK|wxICON_INFORMATION,parent);return;}
    wxSingleChoiceDialog dialog(wxGetTopLevelParent(parent),_L("选择替代耗材。对象、涂色和混色配方会同时更新；取消不删除。"),_L("删除打印机耗材"),labels);
    if(dialog.ShowModal()!=wxID_OK)return;
    if(before!=bundle->project_config || names!=bundle->filament_presets || initial.signature!=PrinterFilamentConfig::capture(*bundle).signature){wxMessageBox(_L("配置已变化，请重新选择。"),_L("打印机耗材"),wxOK|wxICON_WARNING,parent);return;}
    const auto replacement=targets[size_t(dialog.GetSelection())];PresetBundle staged(*bundle);std::string error;
    if(!PrinterFilamentConfig::remove(staged,slot,replacement,error) ||
       !plater.apply_printer_filament_config(staged,{},int(slot),int(replacement),error))
        wxMessageBox(from_u8(error),_L("删除未提交"),wxOK|wxICON_ERROR,parent);
}
}
