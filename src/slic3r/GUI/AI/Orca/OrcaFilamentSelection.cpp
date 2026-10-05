#include "OrcaFilamentSelection.hpp"
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
    void Clear() { m_labels.clear(); SetItemCount(0); }
    void Append(const wxString& label) { m_labels.push_back(label); SetItemCount(m_labels.size()); }
private:
    wxCoord OnMeasureItem(size_t) const override { return FromDIP(32); }
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
        dc.DrawText(wxControl::Ellipsize(m_labels[item], dc, wxELLIPSIZE_END, rect.width - FromDIP(20)),
            rect.x + FromDIP(10), rect.y + (rect.height - dc.GetCharHeight()) / 2);
        dc.DestroyClippingRegion();
    }
    std::vector<wxString> m_labels;
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

void show_orca_filament_selection(wxWindow* parent, Plater& plater)
{
    using namespace ModelGenerationInputStyle;
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return;
    const auto initial_project = bundle->project_config;
    const auto initial_printer = bundle->printers.get_edited_preset().config;
    const auto initial_presets = bundle->filament_presets;
    std::vector<std::string> colors, names, types;
    std::vector<size_t> indices;
    plater.sidebar().collect_physical_filament_info(colors, names, types, &indices);
    struct Draft {
        size_t index;
        std::string preset;
        std::string color;
        bool color_changed = false;
    };
    std::vector<Draft> drafts;
    for (size_t i = 0; i < indices.size(); ++i)
        if (indices[i] < initial_presets.size() && i < colors.size())
            drafts.push_back({indices[i], initial_presets[indices[i]], colors[i], false});

    wxDialog dialog(wxGetTopLevelParent(parent), wxID_ANY, _L("选择耗材"), wxDefaultPosition,
        wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new Label(&dialog, _L("选择耗材"));
    title->SetName("input_accent");
    title->SetFont(Label::Head_20);
    root->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, dialog.FromDIP(20));
    auto* description = new Label(&dialog, _L("编辑实体耗材槽的预设和显示颜色。完成后应用并需重新切片；返回不保存。"), LB_AUTO_WRAP);
    description->SetMinSize(wxSize(1, -1));
    description->SetName("input_secondary");
    root->Add(description, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(20));
    auto* grid = new wxFlexGridSizer(1, 3, dialog.FromDIP(12), dialog.FromDIP(12));
    for (int col = 0; col < 3; ++col) grid->AddGrowableCol(col, col == 1 ? 2 : 1);
    grid->AddGrowableRow(0);
    auto column = [&](const wxString& label) {
        auto* panel = new wxPanel(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new Label(panel, label), 0, wxEXPAND | wxALL, dialog.FromDIP(12));
        panel->SetSizer(sizer);
        grid->Add(panel, 1, wxEXPAND);
        return panel;
    };
    auto* slots_panel = column(_L("实体耗材槽"));
    auto* presets_panel = column(_L("耗材预设"));
    auto* colors_panel = column(_L("颜色"));
    auto list = [&](wxWindow* panel) {
        auto* control = new FilamentChoiceList(panel);
        control->SetMinSize(dialog.FromDIP(wxSize(1, 220)));
        panel->GetSizer()->Add(control, 1, wxEXPAND | wxALL, dialog.FromDIP(8));
        return control;
    };
    auto* slots = list(slots_panel);
    auto* presets = list(presets_panel);
    auto* color_button = new Button(colors_panel, _L("选择颜色…"));
    color_button->SetName("input_field");
    color_button->SetPaddingSize(dialog.FromDIP(wxSize(12, 12)));
    auto* swatch = new wxPanel(colors_panel);
    swatch->SetName("ai_content_color");
    swatch->SetMinSize(dialog.FromDIP(wxSize(40, 60)));
    colors_panel->GetSizer()->Add(swatch, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    colors_panel->GetSizer()->Add(color_button, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* color_text = new Label(colors_panel, "", LB_AUTO_WRAP);
    color_text->SetMinSize(wxSize(1, -1));
    colors_panel->GetSizer()->Add(color_text, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    root->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(20));
    const auto* nozzle = initial_printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t nozzle_count = nozzle ? nozzle->values.size() : 0;
    auto* status = new Label(&dialog, drafts.empty() ? _L("当前没有可编辑的实体耗材槽。请先在工程中选择 FFF 打印机与耗材。") :
        wxString::Format(_L("当前预设：%llu 个喷头，%llu 个实体耗材槽。槽位不是已联网设备的装料状态；混色由工程单独管理。"),
            static_cast<unsigned long long>(nozzle_count), static_cast<unsigned long long>(drafts.size())), LB_AUTO_WRAP);
    status->SetMinSize(wxSize(1, -1));
    status->SetName("input_secondary");
    root->Add(status, 0, wxEXPAND | wxALL, dialog.FromDIP(20));
    std::vector<std::string> available;
    auto refresh = [&] {
        const int row = slots->GetSelection();
        presets->Clear(); available.clear();
        color_button->Enable(row != wxNOT_FOUND);
        if (row == wxNOT_FOUND) return;
        auto& draft = drafts[row];
        // Match the native slot's actual choices, including compatibility and
        // embedded preset identity; never expose wizard/separator menu items.
        for (auto* combo : plater.sidebar().combos_filament()) {
            if (!combo || combo->get_filament_idx() != static_cast<int>(draft.index)) continue;
            for (unsigned int item = 0; item < combo->GetCount(); ++item) {
                const auto alias = combo->GetItemAlias(item);
                if (alias.empty()) continue;
                const auto name = Preset::remove_suffix_modified(into_u8(alias));
                const auto* preset = bundle->filaments.find_preset(name);
                if (!preset || (!preset->is_compatible && name != draft.preset)) continue;
                available.push_back(name);
                presets->Append(from_u8(name));
                if (name == draft.preset) presets->SetSelection(static_cast<int>(available.size() - 1));
            }
        }
        if (presets->GetSelection() != wxNOT_FOUND) presets->SetToolTip(from_u8(draft.preset));
        swatch->SetBackgroundColour(wxColour(from_u8(draft.color))); swatch->Refresh();
        color_text->SetLabel(from_u8(draft.color) + "\n" + _L("实际颜色以装料为准。"));
        dialog.Layout();
    };
    for (const auto& draft : drafts)
        slots->Append(wxString::Format(_L("耗材 %llu"), static_cast<unsigned long long>(draft.index + 1)));
    if (!drafts.empty()) slots->SetSelection(0);
    slots->Bind(wxEVT_LISTBOX, [&](wxCommandEvent&) { refresh(); });
    presets->Bind(wxEVT_LISTBOX, [&](wxCommandEvent&) {
        if (slots->GetSelection() != wxNOT_FOUND && presets->GetSelection() != wxNOT_FOUND)
            drafts[slots->GetSelection()].preset = available[presets->GetSelection()];
    });
    color_button->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (slots->GetSelection() == wxNOT_FOUND) return;
        auto& draft = drafts[slots->GetSelection()];
        wxColourData data; data.SetColour(wxColour(from_u8(draft.color)));
        wxColourDialog picker(&dialog, &data);
        if (picker.ShowModal() != wxID_OK) return;
        draft.color = into_u8(picker.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX));
        draft.color_changed = true;
        refresh();
    });
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto button = [&](const wxString& label, const char* role, int id) {
        auto* control = new Button(&dialog, label);
        control->SetName(role);
        control->SetMinSize(dialog.FromDIP(wxSize(180, 44)));
        actions->Add(control, 0, wxALL, dialog.FromDIP(8));
        control->Bind(wxEVT_BUTTON, [&, id](wxCommandEvent&) {
            if (id == wxID_OK && (bundle->project_config != initial_project ||
                bundle->printers.get_edited_preset().config != initial_printer || bundle->filament_presets != initial_presets)) {
                status->SetLabel(_L("工程配置已变化。请返回后重新打开，避免覆盖新的设置。"));
                dialog.Layout(); return;
            }
            dialog.EndModal(id);
        });
        return control;
    };
    button(_L("返回"), "input_quiet", wxID_CANCEL);
    button(_L("完成"), "input_primary", wxID_OK)->Enable(!drafts.empty());
    root->Add(actions, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, dialog.FromDIP(12));
    dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    dialog.SetSizer(root);
    apply(&dialog);
    dialog.SetMinSize(dialog.FromDIP(wxSize(680, 500)));
    dialog.SetSize(dialog.FromDIP(wxSize(700, 540)));
    refresh();
    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK) return;

    // Submit through each native combo, preserving its preset checks, flushing,
    // mixed-material refresh and normal slicing invalidation. No second owner.
    bool changed = false;
    for (const auto& draft : drafts)
        changed |= draft.preset != initial_presets[draft.index] || draft.color_changed;
    if (!changed) return;
    wxString skipped;
    for (const auto& draft : drafts) {
        PlaterPresetComboBox* combo = nullptr;
        for (auto* candidate : plater.sidebar().combos_filament())
            if (candidate && candidate->get_filament_idx() == static_cast<int>(draft.index)) combo = candidate;
        if (!combo) {
            skipped += wxString::Format(_L("耗材 %llu：槽位不可用\n"), static_cast<unsigned long long>(draft.index + 1));
            continue;
        }
        if (draft.preset != initial_presets[draft.index]) {
            for (unsigned int item = 0; item < combo->GetCount(); ++item) {
                const auto alias = combo->GetItemAlias(item);
                if (alias.empty() || Preset::remove_suffix_modified(into_u8(alias)) != draft.preset) continue;
                combo->SetSelection(item);
                wxCommandEvent event(wxEVT_COMBOBOX, combo->GetId());
                event.SetInt(item); event.SetEventObject(combo);
                combo->GetEventHandler()->ProcessEvent(event);
                break;
            }
        }
        // Native preset selection can be cancelled. Never apply its color then.
        if (draft.index >= bundle->filament_presets.size() || bundle->filament_presets[draft.index] != draft.preset) {
            skipped += wxString::Format(_L("耗材 %llu：预设未切换（取消或选项已变化）\n"), static_cast<unsigned long long>(draft.index + 1));
            continue;
        }
        if (draft.color_changed) {
            for (auto* current : plater.sidebar().combos_filament())
                if (current && current->get_filament_idx() == static_cast<int>(draft.index)) {
                    current->sync_colour_config({draft.color}, false); break;
                }
        }
    }
    if (!skipped.empty())
        wxMessageBox(_L("以下修改未应用，其他已确认修改保留。请检查工程中的耗材设置：\n") + skipped,
            _L("耗材设置"), wxOK | wxICON_INFORMATION, wxGetTopLevelParent(parent));
}
}
