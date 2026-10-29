#pragma once

#include "ModelPreviewPalette.hpp"
#include "ModelPreviewColorState.hpp"
#include "../Model/ColorTrialState.hpp"
#include "PortraitColorPackMapping.hpp"
#include "PortraitColorBlockPostprocess.hpp"
#include "PrinterFilamentPreview.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/tooltip.h>
#include <wx/wrapsizer.h>
#include <wx/weakref.h>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
namespace SemanticColoring = AI::SemanticColoring;
// Trial colors do not modify the project. Real slot edits use the native
// printer-filament configuration dialog.
class ModelPreviewColorControls final : public wxPanel {
public:
    using Color = PreviewPalette::Color;
    static constexpr int honor_suite_source = -1;
    void set_workbench_palette_hint(const wxString& hint) {
        if (m_workbench_palette_hint == hint) return;
        m_workbench_palette_hint = hint;
        sync_workbench_palette_tooltips();
    }
    void set_workbench_detail_choices(bool enabled) {
        if (enabled == !m_detail_choices.empty()) return;
        if (enabled) {
            // The detail dialog is a read/adjust surface.  Original-colour
            // comparison is owned by the workbench canvas, so do not expose
            // a second toggle here.
            m_toggle->Hide();
            std::vector<wxChoice*> originals {m_source};
            for (auto* original : originals) {
                auto* choice = workbench_choice(this);
                choice->SetName("ai_content_color");
                choice->GetDropDown().SetUseContentWidth(true, true);
                choice->SetMinSize(original->GetMinSize());
                original->GetContainingSizer()->Replace(original, choice);
                original->Hide();
                m_detail_choices.push_back({original, choice});
                choice->Bind(wxEVT_COMBOBOX, [this, original, choice](wxCommandEvent&) {
                    original->SetSelection(choice->GetSelection());
                    wxCommandEvent event(wxEVT_CHOICE, original->GetId());
                    event.SetEventObject(original);
                    event.SetInt(original->GetSelection());
                    event.SetString(original->GetStringSelection());
                    original->GetEventHandler()->ProcessEvent(event);
                });
            }
            m_detail_count_panel = new wxPanel(this);
            m_detail_count_panel->SetName("ai_content_color");
            m_detail_count_panel->SetBackgroundColour(wxColour(32, 32, 35));
            auto* count_row = new wxBoxSizer(wxHORIZONTAL);
            m_detail_count_slider = new WorkbenchSlider(m_detail_count_panel, wxID_ANY,
                m_count->GetValue(), m_count->GetMin(), m_count->GetMax());
            m_detail_count_slider->SetMinSize(wxSize(FromDIP(96), FromDIP(28)));
            count_row->Add(m_detail_count_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            m_detail_count_value = new wxStaticText(m_detail_count_panel, wxID_ANY, wxEmptyString);
            m_detail_count_value->SetForegroundColour(wxColour(235, 235, 235));
            m_detail_count_value->SetMinSize(wxSize(FromDIP(22), -1));
            count_row->Add(m_detail_count_value, 0, wxALIGN_CENTER_VERTICAL);
            m_detail_count_panel->SetSizer(count_row);
            m_detail_count_panel->SetToolTip(m_count->GetToolTipText());
            m_count->GetContainingSizer()->Replace(m_count, m_detail_count_panel);
            m_count->Hide();
            const auto change_count = [this](wxScrollEvent&) {
                if (m_count->GetValue() == m_detail_count_slider->GetValue()) return;
                m_count->SetValue(m_detail_count_slider->GetValue());
                recompute();
            };
            m_detail_count_slider->Bind(wxEVT_SCROLL_THUMBRELEASE, change_count);
            m_detail_count_slider->Bind(wxEVT_SCROLL_CHANGED, change_count);
            std::vector<wxCheckBox*> switch_originals {m_fidelity, m_lighting};
            switch_originals.insert(switch_originals.end(), m_locks.begin(), m_locks.end());
            for (auto* original : switch_originals) {
                auto* row = new wxPanel(this);
                row->SetName("ai_content_color");
                row->SetBackgroundColour(wxColour(32, 32, 35));
                auto* layout = new wxBoxSizer(wxHORIZONTAL);
                auto* label = new wxStaticText(row, wxID_ANY, original->GetLabel());
                label->SetForegroundColour(wxColour(235, 235, 235));
                layout->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
                auto* toggle = new WorkbenchSwitch(row, original->GetLabel());
                layout->Add(toggle, 0, wxALIGN_CENTER_VERTICAL);
                row->SetSizer(layout);
                original->GetContainingSizer()->Replace(original, row);
                if (original == m_lighting) {
                    auto* item = row->GetContainingSizer()->GetItem(row);
                    item->SetFlag(item->GetFlag() | wxEXPAND);
                }
                original->Hide();
                const auto lock = std::find(m_locks.begin(), m_locks.end(), original);
                const int lock_index = lock == m_locks.end() ? -1 : int(lock - m_locks.begin());
                m_detail_switches.push_back({original, row, toggle, lock_index});
                toggle->Bind(wxEVT_TOGGLEBUTTON, [original, toggle](wxCommandEvent&) {
                    toggle->SetValue(toggle->GetValue());
                    original->SetValue(toggle->GetValue());
                    wxCommandEvent event(wxEVT_CHECKBOX, original->GetId());
                    event.SetEventObject(original);
                    event.SetInt(original->GetValue());
                    original->GetEventHandler()->ProcessEvent(event);
                });
            }
            for (auto* original : m_detail_commands) {
                auto* button = workbench_button(this, original->GetLabel());
                button->SetName("ai_content_color");
                button->SetMinSize(wxSize(std::max(FromDIP(88), original->GetMinSize().x), FromDIP(32)));
                original->GetContainingSizer()->Replace(original, button);
                button->Show(original->IsShown());
                original->Hide();
                m_detail_buttons.push_back({original, button});
                button->Bind(wxEVT_BUTTON, [this, original](wxCommandEvent&) {
                    if (original == m_semantic_cancel && !m_semantic_busy) return;
                    wxCommandEvent event(wxEVT_BUTTON, original->GetId());
                    event.SetEventObject(original);
                    original->GetEventHandler()->ProcessEvent(event);
                });
            }
            sync_workbench_detail_choices();
        } else {
            for (const auto& entry : m_detail_buttons) {
                entry.button->GetContainingSizer()->Replace(entry.button, entry.original);
                entry.original->Show(entry.original == m_semantic_cancel ? m_semantic_busy : entry.button->IsShown());
                entry.button->Destroy();
            }
            m_detail_buttons.clear();
            m_detail_count_panel->GetContainingSizer()->Replace(m_detail_count_panel, m_count);
            m_count->Show();
            m_detail_count_panel->Destroy();
            m_detail_count_panel = nullptr;
            m_detail_count_slider = nullptr;
            m_detail_count_value = nullptr;
            for (const auto& entry : m_detail_switches) {
                entry.row->GetContainingSizer()->Replace(entry.row, entry.original);
                entry.original->Show(entry.row->IsShown());
                entry.row->Destroy();
            }
            m_detail_switches.clear();
            for (const auto& entry : m_detail_choices) {
                entry.choice->GetContainingSizer()->Replace(entry.choice, entry.original);
                entry.original->Show();
                entry.choice->Destroy();
            }
            m_detail_choices.clear();
            // Original-colour comparison is provided by the workbench canvas;
            // keep the legacy control hidden in both compact and detail views.
            m_toggle->Hide();
        }
        update();
    }
    void set_workbench_editable(bool editable) {
        m_workbench_editable = editable;
        if (!m_workbench_source) return;
        const bool source_enabled = editable && bool(m_histogram);
        if (m_workbench_source->IsEnabled() != source_enabled)
            m_workbench_source->Enable(source_enabled);
        if (m_workbench_mode) {
            const bool mode_enabled = source_enabled && portrait_color_count_supported();
            if (m_workbench_mode->IsEnabled() != mode_enabled)
                m_workbench_mode->Enable(mode_enabled);
        }
        const int source = current_source();
        const bool count_enabled = source_enabled && !honor_suite_active() && (source == 0 || source == 2);
        if (m_workbench_count->IsEnabled() != count_enabled)
            m_workbench_count->Enable(count_enabled);
        if (m_workbench_count_input && m_workbench_count_input->IsEnabled() != count_enabled)
            m_workbench_count_input->Enable(count_enabled);
        if (m_workbench_suite_category) m_workbench_suite_category->Enable(source_enabled && honor_suite_active());
        if (m_workbench_suite_choice) m_workbench_suite_choice->Enable(source_enabled && honor_suite_active());
    }
    wxWindow* build_workbench_mode(wxWindow* parent) {
        auto* container = new wxPanel(parent);
        container->SetBackgroundColour(parent->GetBackgroundColour());
        container->SetSizer(new wxBoxSizer(wxVERTICAL));
        container->Hide();
        return container;
    }
    void set_semantic_mode(bool portrait) {
        (void)portrait;
        m_host_portrait_mode = false;
        m_semantic->SetValue(false);
        update();
    }
    wxWindow* build_workbench_palette(wxWindow* parent) {
        auto* container = new wxPanel(parent);
        container->SetBackgroundColour(parent->GetBackgroundColour());
        auto* contents = new wxBoxSizer(wxVERTICAL);
        auto* panel = new WorkbenchPanel(container, true);
        panel->SetWindowStyle(panel->GetWindowStyle() | wxCLIP_CHILDREN);
        panel->SetBackgroundColour(wxColour(22, 22, 25));
        auto* root = new wxBoxSizer(wxVERTICAL);
        m_workbench_source = new WorkbenchPaletteChoice(panel);
        m_workbench_source->SetName(_L("色卡来源"));
        for (unsigned i = 0; i < m_source->GetCount(); ++i) m_workbench_source->Append(m_source->GetString(i));
        root->Add(m_workbench_source, 0, wxEXPAND);
        auto* divider = new wxPanel(panel, wxID_ANY, wxDefaultPosition,
            wxSize(-1, FromDIP(1)), wxBORDER_NONE);
        divider->SetBackgroundColour(wxColour(92, 92, 98));
        divider->SetMinSize(wxSize(-1, FromDIP(1)));
        divider->SetMaxSize(wxSize(-1, FromDIP(1)));
        root->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        m_workbench_suite_panel = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_workbench_suite_panel->SetBackgroundColour(panel->GetBackgroundColour());
        auto* suite_layout = new wxBoxSizer(wxVERTICAL);
        auto add_suite_choice = [&](const wxString& label, WorkbenchPaletteChoice*& choice) {
            auto* title = new WorkbenchStaticText(m_workbench_suite_panel, wxID_ANY, label);
            title->SetForegroundColour(wxColour(190, 190, 196));
            suite_layout->Add(title, 0, wxBOTTOM, FromDIP(4));
            choice = new WorkbenchPaletteChoice(m_workbench_suite_panel);
            choice->SetName(label);
            choice->GetDropDown().SetUseContentWidth(true, true);
            suite_layout->Add(choice, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        };
        add_suite_choice(_L("适用类型"), m_workbench_suite_category);
        add_suite_choice(_L("具体套装"), m_workbench_suite_choice);
        m_workbench_suite_category->Append(wxString::FromUTF8(young_portrait_honor_suite().category));
        m_workbench_suite_category->SetSelection(0);
        m_workbench_suite_choice->Append(wxString::FromUTF8(young_portrait_honor_suite().name));
        m_workbench_suite_choice->SetSelection(0);
        m_workbench_suite_summary = new WorkbenchStaticText(m_workbench_suite_panel, wxID_ANY,
            _L("6 个实体耗材 · 共 6 色"));
        m_workbench_suite_summary->SetForegroundColour(wxColour(235, 235, 235));
        suite_layout->Add(m_workbench_suite_summary, 0, wxTOP, FromDIP(2));
        m_workbench_suite_panel->SetSizer(suite_layout);
        m_workbench_suite_panel->Hide();
        root->Add(m_workbench_suite_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(12));
        m_workbench_count = new WorkbenchSlider(panel, wxID_ANY, m_count->GetValue(), 1,
            int(PreviewPalette::max_preview_colors), wxColour(255, 194, 39), "workbench_slider_thumb");
        root->Add(m_workbench_count, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        auto* count_row = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        count_row->SetBackgroundColour(panel->GetBackgroundColour());
        auto* count_layout = new wxBoxSizer(wxHORIZONTAL);
        m_workbench_count_label = new WorkbenchStaticText(count_row, wxID_ANY, _L("当前色数"));
        m_workbench_count_label->SetForegroundColour(wxColour(235, 235, 235));
        count_layout->Add(m_workbench_count_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_workbench_count_input = new WorkbenchNumberStepper(count_row, m_count->GetValue(), 1,
            int(PreviewPalette::max_preview_colors));
        m_workbench_count_input->SetToolTip(_L("输入 1 至 32，或使用上下键逐个调整当前色数。"));
        count_layout->Add(m_workbench_count_input, 0, wxALIGN_CENTER_VERTICAL);
        count_row->SetSizer(count_layout);
        m_workbench_count_row = count_row;
        root->Add(count_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(12));
        m_workbench_source->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            set_source_internal(internal_source(m_workbench_source->GetSelection()));
            wxCommandEvent event(wxEVT_CHOICE, m_source->GetId());
            event.SetEventObject(m_source); m_source->GetEventHandler()->ProcessEvent(event);
        });
        m_workbench_count->Bind(wxEVT_SCROLL_THUMBRELEASE, [this](wxScrollEvent&) {
            set_workbench_count(m_workbench_count->GetValue());
        });
        m_workbench_count->Bind(wxEVT_SCROLL_CHANGED, [this](wxScrollEvent&) {
            set_workbench_count(m_workbench_count->GetValue());
        });
        m_workbench_count_input->set_change_handler([this](int value) { set_workbench_count(value); });
        sync_workbench_palette_tooltips();
        panel->SetSizer(root);
        contents->Add(panel, 0, wxEXPAND);
        container->SetSizer(contents); update(); return container;
    }
    explicit ModelPreviewColorControls(wxWindow* parent) : wxPanel(parent) {
        auto* box = new wxBoxSizer(wxVERTICAL);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_toggle = new wxButton(this, wxID_ANY, _L("一键试色"));
        row->Add(m_toggle, 0, wxRIGHT, FromDIP(6));
        m_source = new wxChoice(this, wxID_ANY);
        // Keep the native source ids stable in persisted state while hiding
        // printer-filament input from the multicolor card selector.
        m_source_visible_to_internal = {0, 2, honor_suite_source};
        m_source->Append(_L("AI配色"));
        m_source->Append(_L("手动试色"));
        m_source->Append(_L("荣耀耗材套装"));
        m_packs = load_filament_color_packs();
        for (size_t i = 0; i < m_packs.size(); ++i) {
            if (m_packs[i].name == young_portrait_color_pack().name) continue;
            m_source->Append(wxString::FromUTF8(m_packs[i].name));
            m_source_visible_to_internal.push_back(int(3 + i));
        }
        m_source->SetSelection(0);
        row->Add(m_source, 1, wxRIGHT, FromDIP(6));
        m_count = new wxSpinCtrl(this, wxID_ANY, "6", wxDefaultPosition, wxSize(FromDIP(72), -1), wxSP_ARROW_KEYS,
            1, int(PreviewPalette::max_preview_colors), 6);
        m_count->SetName(_L("试色数量"));
        m_count->SetToolTip(_L("结果对照不再固定为六色，可按模型需要选择 1 至 32 色。"));
        row->Add(m_count, 0, wxRIGHT, FromDIP(4));
        m_count_suffix = new wxStaticText(this, wxID_ANY, _L("色"));
        row->Add(m_count_suffix, 0, wxALIGN_CENTER_VERTICAL);
        box->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
        m_suite_detail_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_suite_detail_panel->SetName("ai_content_color");
        m_suite_detail_panel->SetBackgroundColour(wxColour(32, 32, 35));
        auto* suite_detail_layout = new wxBoxSizer(wxVERTICAL);
        auto add_detail_choice = [&](const wxString& title, const wxString& value) {
            auto* label = new wxStaticText(m_suite_detail_panel, wxID_ANY, title);
            label->SetForegroundColour(wxColour(190, 190, 196));
            suite_detail_layout->Add(label, 0, wxBOTTOM, FromDIP(4));
            auto* choice = new wxChoice(m_suite_detail_panel, wxID_ANY);
            choice->Append(value);
            choice->SetSelection(0);
            choice->Enable(false);
            suite_detail_layout->Add(choice, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        };
        add_detail_choice(_L("适用类型"), wxString::FromUTF8(young_portrait_honor_suite().category));
        add_detail_choice(_L("具体套装"), wxString::FromUTF8(young_portrait_honor_suite().name));
        m_suite_detail_summary = new wxStaticText(m_suite_detail_panel, wxID_ANY,
            _L("6 个实体耗材 · 共 6 色"));
        m_suite_detail_summary->SetForegroundColour(wxColour(235, 235, 235));
        suite_detail_layout->Add(m_suite_detail_summary, 0, wxTOP, FromDIP(2));
        m_suite_detail_panel->SetSizer(suite_detail_layout);
        m_suite_detail_panel->Hide();
        box->Add(m_suite_detail_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        auto* options = new wxBoxSizer(wxHORIZONTAL);
        m_fidelity = new wxCheckBox(this, wxID_ANY, _L("尽量保留小面积彩色"));
        m_fidelity->SetValue(true);
        m_fidelity->SetToolTip(_L("缩减颜色时，尽量保留有一定面积的少数彩色，例如嘴唇红、衣服绿。不是自动识别人脸或衣物；取消后更偏向大面积颜色。"));
        options->Add(m_fidelity, 1, wxALIGN_CENTER_VERTICAL);
        auto* reset = new wxButton(this, wxID_ANY, _L("重置试色"));
        options->Add(reset, 0);
        box->Add(options, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        m_semantic = new wxCheckBox(this, wxID_ANY, _L("人像区域优化"));
        m_semantic->SetValue(false);
        m_semantic->Hide();
        m_semantic_cancel = new wxButton(this, wxID_ANY, _L("取消识别"));
        m_semantic_cancel->Hide();
        m_semantic_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_semantic_status->Hide();
        m_semantic_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_semantic_cancel) on_semantic_cancel(); });
        // Color cards are trial-only. Project filament configuration is owned by
        // the printer-filament workbench and is deliberately not writable here.
        m_detail_commands = {reset};
        m_lighting = new wxCheckBox(this, wxID_ANY, _L("光照立体展示"));
        m_lighting->SetValue(false);
        m_lighting->SetToolTip(_L("默认用纯色色块检查分色。开启后添加光照明暗，屏幕上会看到更多深浅，但基础颜色数量不变。"));
        box->Add(m_lighting, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        auto* swatches = new wxWrapSizer(wxHORIZONTAL);
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            m_swatches[i] = new wxButton(this, wxID_ANY, "", wxDefaultPosition,
                wxSize(FromDIP(128), FromDIP(34)), wxBU_EXACTFIT);
            m_swatches[i]->SetMinSize(wxSize(FromDIP(128), FromDIP(34)));
            m_swatches[i]->SetMaxSize(wxSize(FromDIP(128), FromDIP(34)));
            m_swatches[i]->SetName("ai_content_color");
            m_locks[i] = new wxCheckBox(this, wxID_ANY, _L("保留"));
            col->Add(m_swatches[i], 0, wxALIGN_LEFT);
            col->Add(m_locks[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(3));
            swatches->Add(col, 0, wxRIGHT, FromDIP(8));
            m_swatches[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { edit_color(i); });
            m_locks[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        }
        box->Add(swatches, 0, wxALIGN_LEFT | wxALL, FromDIP(6));
        m_region_reset = new wxButton(this, wxID_ANY, _L("恢复自动区域颜色"));
        m_region_reset->Hide();
        for (size_t i = 0; i < m_region_slots.size(); ++i) {
            m_region_slots[i] = new wxChoice(this, wxID_ANY);
            m_region_slots[i]->SetName("ai_semantic_region_slot");
            m_region_slots[i]->Hide();
            m_region_slots[i]->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent&) {
                m_semantic_region_slots[i] = m_region_slots[i]->GetSelection() > 0
                    ? m_region_slots[i]->GetSelection() - 1 : -1;
                m_notice = _L("区域颜色已固定；修改区域槽位不会重新识别人像区域。");
                region_changed();
            });
        }
        m_region_reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
            m_notice = _L("已恢复自动区域颜色；识别结果保持不变。");
            region_changed();
        });
        m_status = new wxStaticText(this, wxID_ANY, "");
        box->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        SetSizer(box);
        m_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (current_source() == 1 && read_project()) {
                m_notice = _L("打印机耗材已变化，已恢复原色，请重新对照。");
                m_enabled = false; changed(); return;
            }
            if (m_colors.empty()) return;
            m_enabled = !m_enabled; changed();
        });
        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const int previous = m_last_source;
            set_source_internal(internal_source(m_source->GetSelection()));
            const int next = current_source();
            remember_source_count(previous);
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear();
            m_honor_suite_fallback_reason.clear();
            if (next == 1) {
                read_project(); m_enabled = false;
                m_bound_project_identity.clear(); m_project_color_slots.clear(); m_project_semantic_slots.clear();
                m_project_count = std::clamp(int(m_project_colors.size()), 1, int(PreviewPalette::max_preview_colors));
                m_count->SetValue(m_project_count);
                m_notice = _L("已进入打印机耗材试色；修改耗材颜色会同步模型预览。");
                m_last_source = next;
                if (on_source_changed) on_source_changed(previous, next);
                changed(); return;
            }
            if (next == honor_suite_source) m_count->SetValue(int(young_portrait_honor_suite().physical_slots.size()));
            else if (next == 0) m_count->SetValue(m_automatic_count);
            else if (next == 2) m_count->SetValue(m_manual_count);
            else if (next >= 3 && size_t(next - 3) < m_packs.size())
                m_count->SetValue(std::clamp(int(m_packs[next - 3].colors.size()), 1, int(PreviewPalette::max_preview_colors)));
            m_last_source = next;
            if (next == honor_suite_source) {
                if (!prepare_honor_suite_request()) {
                    m_notice = _L("原始配色证据无法建立荣耀套装请求，已保留当前预览。");
                    update();
                }
                if (on_source_changed) on_source_changed(previous, next);
                return;
            }
            clear_honor_suite_request();
            recompute();
            if (on_source_changed) on_source_changed(previous, next);
        });
        m_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
            recompute();
        });
        m_fidelity->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        m_lighting->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            read_project();
            m_initial_count = int(PreviewPalette::initial_trial_color_count());
            set_source_internal(0); m_count->SetValue(m_initial_count);
            m_automatic_count = m_initial_count; m_manual_count = m_initial_count; m_project_count = 0; m_last_source = 0;
            m_fidelity->SetValue(true);
            m_semantic->SetValue(false);
            m_lighting->SetValue(false);
            m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear(); recompute();
        });
        Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
            const auto now = std::chrono::steady_clock::now();
            if (IsShownOnScreen() && m_histogram && current_source() == 1 &&
                now - m_last_check > std::chrono::seconds(1)) {
                m_last_check = now;
                if (on_project_colors_changed) on_project_colors_changed();
                else synchronize_project_colors();
            }
            event.Skip();
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            wrap_status();
            Layout();
            if (!m_status_text.empty() && event.GetSize().x != m_status_layout_width) {
                m_status_layout_width = event.GetSize().x;
                if (!m_status_layout_pending) {
                    m_status_layout_pending = true;
                    wxWeakRef<ModelPreviewColorControls> self(this);
                    // The enclosing scroller must remeasure after its current layout finishes.
                    CallAfter([self] {
                        if (!self) return;
                        self->m_status_layout_pending = false;
                        self->wrap_status();
                        self->Layout();
                        self->SetMinSize(wxSize(self->FromDIP(420), self->GetSizer()->CalcMin().y));
                        if (auto* parent = self->GetParent()) {
                            parent->Layout();
                            if (auto* scroll = dynamic_cast<wxScrolledWindow*>(parent)) scroll->FitInside();
                        }
                    });
                }
            }
            event.Skip();
        });
    }
    void load(std::shared_ptr<const PreviewPalette::Histogram> histogram, std::vector<Color> colors,
              std::string geometry_id = {},
              std::shared_ptr<const PreviewPalette::AutomaticColorUnits> automatic_units = {}) {
        m_honor_suite_request.reset();
        m_honor_suite_used_colors = 0;
        m_geometry_id = std::move(geometry_id);
        m_automatic_units = std::move(automatic_units);
        m_histogram = std::move(histogram); m_colors = std::move(colors); m_mapping_colors = m_colors;
        m_suggestion_colors = m_colors; m_manual_colors.clear();
        read_project();
        m_initial_count = int(PreviewPalette::initial_trial_color_count());
        wxString initial_notice;
        if (m_histogram && m_colors.size() != size_t(m_initial_count)) {
            m_colors = m_histogram->trial_palette(size_t(m_initial_count), {}, true);
            m_mapping_colors = m_colors;
            if (m_colors.size() != size_t(m_initial_count)) {
                const int requested = m_initial_count;
                m_initial_count = std::max(1, int(m_colors.size()));
                initial_notice = wxString::Format(
                    _L("原始配色证据只能形成 %d 个可区分颜色，未虚构第 %d 至 %d 色。"),
                    m_initial_count, m_initial_count + 1, requested);
            }
        }
        set_current_automatic_basis(size_t(m_initial_count), m_mapping_colors);
        m_color_card_remap = {};
        m_color_card_palette.clear();
        m_semantic_colors.clear(); m_semantic_mapping.clear(); m_semantic_card.clear(); m_semantic_roles.clear();
        m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
        m_region_available = {};
        m_semantic->SetValue(false);
        sync_active_palette_state();
        set_semantic_status(wxEmptyString, false);
        m_enabled = false; set_source_internal(0); m_count->SetValue(m_initial_count); m_fidelity->SetValue(true);
        m_automatic_count = m_initial_count; m_manual_count = m_initial_count; m_project_count = 0; m_last_source = 0;
        m_lighting->SetValue(false);
        m_notice = initial_notice;
        for (auto* lock : m_locks) lock->SetValue(false);
        update();
    }
    void clear() {
        m_histogram.reset(); m_colors.clear(); m_mapping_colors.clear();
        m_suggestion_colors.clear(); m_manual_colors.clear();
        m_semantic_colors.clear(); m_semantic_mapping.clear(); m_semantic_card.clear(); m_semantic_roles.clear();
        m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
        m_region_available = {};
        m_automatic_basis = {};
        m_color_card_remap = {};
        m_color_card_palette.clear();
        m_automatic_units.reset();
        m_geometry_id.clear();
        m_enabled = false; m_source_internal = 0; Hide();
        m_active_suite_id.clear(); m_active_suite_revision.clear();
        m_honor_suite_request.reset();
        m_honor_suite_used_colors = 0;
    }
    const std::vector<Color>& colors() const { return m_colors; }
    int source() const { return current_source(); }
    int current_source() const {
        return m_source ? m_source_internal : 0;
    }
    int visible_source(int internal) const {
        const auto found = std::find(m_source_visible_to_internal.begin(), m_source_visible_to_internal.end(), internal);
        return found == m_source_visible_to_internal.end() ? 0 : int(found - m_source_visible_to_internal.begin());
    }
    int internal_source(int visible) const {
        return visible >= 0 && size_t(visible) < m_source_visible_to_internal.size()
            ? m_source_visible_to_internal[size_t(visible)] : 0;
    }
    void set_source_internal(int internal) {
        if (!m_source) return;
        if (internal == honor_suite_source) {
            const auto& suite = young_portrait_honor_suite();
            m_active_suite_id = suite.id;
            m_active_suite_revision = suite.revision;
        } else {
            m_active_suite_id.clear();
            m_active_suite_revision.clear();
            m_honor_suite_request.reset();
            m_honor_suite_used_colors = 0;
        }
        m_source_internal = internal;
        const int visible = visible_source(internal);
        if (m_source->GetSelection() != visible) m_source->SetSelection(visible);
        if (m_workbench_source && m_workbench_source->GetSelection() != visible)
            m_workbench_source->SetSelection(visible);
    }
    std::optional<FilamentColorPack> current_color_pack() const {
        const int active = current_source();
        std::vector<Color> colors;
        std::string name;
        if (honor_suite_active()) {
            return active_honor_suite()->color_pack();
        } else if (active == 0) {
            colors = m_suggestion_colors.empty() ? m_colors : m_suggestion_colors;
            name = u8"AI配色";
        } else if (active == 2) {
            colors = manual_import_palette(state());
            name = u8"手动试色";
        } else if (active >= 3 && size_t(active - 3) < m_packs.size()) {
            return m_packs[size_t(active - 3)];
        } else {
            return std::nullopt;
        }
        if (colors.empty()) return std::nullopt;
        FilamentColorPack pack;
        pack.name = std::move(name);
        for (size_t i = 0; i < colors.size(); ++i) {
            const auto hex = wx_color(colors[i]).GetAsString(wxC2S_HTML_SYNTAX).ToUTF8();
            pack.colors.emplace_back(hex.data());
            pack.labels.emplace_back("槽位 " + std::to_string(i + 1));
        }
        return pack.valid_for_workbench_import() ? std::optional<FilamentColorPack>(std::move(pack)) : std::nullopt;
    }
    bool color_card_source_active() const {
        // Every saved color card participates in region-preserving remapping;
        // the portrait card only adds role labels and is not the sole source
        // eligible for this path.
        return honor_suite_active() || source() >= 3;
    }
    bool honor_suite_active() const {
        return current_source() == honor_suite_source && active_honor_suite() != nullptr;
    }
    const HonorFilamentSuite* active_honor_suite() const {
        return find_honor_filament_suite(m_active_suite_id, m_active_suite_revision);
    }
    int automatic_count() const { return m_automatic_count; }
    void refresh_saved_color_packs() {
        // Keep existing source indices stable for history and 3MF state.
        const int selected = current_source();
        for (auto& pack : load_filament_color_packs()) {
            const auto found = std::find_if(m_packs.begin(), m_packs.end(),
                [&](const FilamentColorPack& cached) { return cached.name == pack.name; });
            if (found != m_packs.end()) *found = std::move(pack);
            else {
                const bool legacy_suite_card = pack.name == young_portrait_color_pack().name;
                if (!legacy_suite_card) {
                    m_source->Append(wxString::FromUTF8(pack.name));
                    m_source_visible_to_internal.push_back(int(3 + m_packs.size()));
                    if (m_workbench_source) m_workbench_source->Append(wxString::FromUTF8(pack.name));
                }
                m_packs.push_back(std::move(pack));
            }
        }
        set_source_internal(selected);
        sync_workbench_detail_choices();
    }
    std::vector<FilamentColorPack> configuration_color_sources() const {
        auto make = [](const std::string& name, const std::vector<Color>& colors) {
            FilamentColorPack pack; pack.name = name;
            for (size_t i = 0; i < colors.size(); ++i) {
                const auto c = wx_color(colors[i]);
                pack.colors.push_back(c.GetAsString(wxC2S_HTML_SYNTAX).ToUTF8().data());
                pack.labels.push_back("槽位 " + std::to_string(i + 1));
            }
            return pack;
        };
        std::vector<FilamentColorPack> result;
        if (!m_suggestion_colors.empty()) result.push_back(make(u8"AI配色", m_suggestion_colors));
        const auto manual = manual_import_palette(state());
        if (!manual.empty()) result.push_back(make(u8"手动试色", manual));
        auto suite = young_portrait_honor_suite().color_pack();
        suite.name = u8"荣耀耗材套装 · 人物 · 年轻向六色";
        result.push_back(std::move(suite));
        const auto saved = load_filament_color_packs();
        for (const auto& pack : saved)
            if (pack.name != young_portrait_color_pack().name) result.push_back(pack);
        return result;
    }
    const std::vector<Color>& mapping_colors() const { return m_mapping_colors; }
    const PreviewPalette::AutomaticColorBasis& automatic_color_basis() const { return m_automatic_basis; }
    std::shared_ptr<const PreviewPalette::AutomaticColorUnits> automatic_color_units() const { return m_automatic_units; }
    const PreviewPalette::ColorCardRemap& color_card_remap() const { return m_color_card_remap; }
    const std::string& automatic_basis_signature() const { return m_automatic_basis.basis_signature; }
    bool honor_suite_request_pending() const {
        const auto* suite = active_honor_suite();
        return suite && m_honor_suite_request &&
            m_honor_suite_request->geometry_id == m_geometry_id &&
            m_honor_suite_request->suite_id == suite->id &&
            m_honor_suite_request->suite_revision == suite->revision;
    }
    const std::vector<Color>& honor_suite_request_palette() const {
        static const std::vector<Color> empty;
        return honor_suite_request_pending() ? m_honor_suite_request->palette : empty;
    }
    const std::vector<std::string>& honor_suite_request_roles() const {
        static const std::vector<std::string> empty;
        return honor_suite_request_pending() ? m_honor_suite_request->palette_roles : empty;
    }
    const std::map<std::string, size_t>& honor_suite_request_role_slots() const {
        static const std::map<std::string, size_t> empty;
        return honor_suite_request_pending() ? m_honor_suite_request->fixed_role_slots : empty;
    }
    std::string honor_suite_request_context() const {
        return honor_suite_request_pending() ? m_honor_suite_request->context : std::string{};
    }
    bool commit_honor_suite_semantic(size_t used_slots) {
        if (!honor_suite_request_pending() || used_slots == 0 ||
            used_slots > m_honor_suite_request->palette.size()) return false;
        const auto request = *m_honor_suite_request;
        m_automatic_basis = request.automatic_basis;
        m_mapping_colors = request.palette;
        m_colors = request.palette;
        m_color_card_remap = {};
        m_color_card_palette = request.palette;
        m_semantic_colors = request.palette;
        m_semantic_mapping = request.palette;
        m_semantic_card = request.palette;
        m_semantic_roles = request.palette_roles;
        m_count->SetValue(int(request.palette.size()));
        m_honor_suite_used_colors = used_slots;
        m_honor_suite_request.reset();
        m_honor_suite_fallback_reason.clear();
        m_enabled = true;
        m_notice = wxString::Format(_L("荣耀耗材套装语义配色已完成，模型当前使用 %llu/6 色。"),
            static_cast<unsigned long long>(used_slots));
        sync_active_palette_state();
        changed();
        return true;
    }
    bool commit_honor_suite_fallback(const wxString& reason) {
        if (!honor_suite_request_pending()) return false;
        const auto request = *m_honor_suite_request;
        if (!request.fallback.valid(request.automatic_basis.source_palette.size())) return false;
        m_automatic_basis = request.automatic_basis;
        m_color_card_remap = request.fallback;
        m_color_card_palette = request.palette;
        m_mapping_colors = request.automatic_basis.source_palette;
        m_colors = request.fallback.source_order_targets();
        if (m_colors.empty() || m_colors.size() != m_mapping_colors.size()) return false;
        m_semantic_colors = m_colors;
        m_semantic_mapping = m_mapping_colors;
        m_semantic_card.clear();
        m_semantic_roles.clear();
        m_count->SetValue(int(request.palette.size()));
        auto used = request.fallback.source_to_target_slot;
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        m_honor_suite_used_colors = used.size();
        m_honor_suite_request.reset();
        m_enabled = true;
        m_honor_suite_fallback_reason = reason.empty()
            ? _L("未检测到可靠人物语义，已回退为全局相近色匹配。")
            : reason;
        m_notice = m_honor_suite_fallback_reason;
        sync_active_palette_state();
        changed();
        return true;
    }
    size_t honor_suite_used_color_count() const {
        return honor_suite_active() ? m_honor_suite_used_colors : 0;
    }
    void set_honor_suite_used_color_count(size_t count) {
        const size_t bounded = std::min(count, young_portrait_honor_suite().physical_slots.size());
        if (m_honor_suite_used_colors == bounded) return;
        m_honor_suite_used_colors = bounded;
        update();
    }
    bool enabled() const { return m_enabled; }
    bool lighting() const { return m_lighting->GetValue(); }
    bool activate_current_ai_palette() {
        if (!m_histogram) return false;
        const int target = std::clamp(m_automatic_count, 1, int(PreviewPalette::max_preview_colors));
        set_source_internal(0);
        m_last_source = 0;
        m_count->SetValue(target);
        m_notice = wxString::Format(_L("模型检查已完成，已自动应用 %d 色 AI配色。"), target);
        recompute(true);
        return m_enabled && current_source() == 0 && int(m_colors.size()) == target;
    }
    bool synchronize_project_colors(bool activate = false) {
        const bool different = read_project();
        if (!different && !activate) return false;
        if (current_source() != 1) return false;
        auto saved = state();
        if (!m_project_error.empty() || m_project_colors.empty() || !AI::ColorTrialPersistence::synchronize_project_targets(
                saved, m_project_slots, m_project_colors, m_project_slot_identity)) {
            const wxString notice = _L("打印机或耗材槽位已变化，请重新进入打印机耗材试色。");
            const bool visual_change = m_enabled || m_notice != notice;
            m_enabled = false;
            m_notice = notice;
            if (visual_change) changed();
            return visual_change;
        }
        const bool visual_change = saved.colors != m_colors || saved.semantic_palette != m_semantic_colors;
        const bool binding_change = saved.project_slot_identity != m_bound_project_identity ||
            saved.project_color_slots != m_project_color_slots ||
            saved.project_semantic_slots != m_project_semantic_slots;
        const bool card_change = saved.color_card_target_palette != m_color_card_remap.target_palette ||
            saved.color_card_target_slot_ids != m_color_card_remap.target_slot_ids ||
            saved.source_to_target_slot != m_color_card_remap.source_to_target_slot;
        if (!visual_change && !binding_change && !card_change) return false;
        m_colors = std::move(saved.colors);
        m_semantic_colors = std::move(saved.semantic_palette);
        m_bound_project_identity = std::move(saved.project_slot_identity);
        m_project_color_slots = std::move(saved.project_color_slots);
        m_project_semantic_slots = std::move(saved.project_semantic_slots);
        if (card_change) {
            m_color_card_remap.target_palette = std::move(saved.color_card_target_palette);
            m_color_card_remap.target_slot_ids = std::move(saved.color_card_target_slot_ids);
            m_color_card_remap.source_to_target_slot = std::move(saved.source_to_target_slot);
            m_color_card_remap.weighted_error = saved.color_card_weighted_error;
            m_color_card_palette = m_color_card_remap.target_palette;
        }
        if (m_count->GetValue() != int(m_colors.size())) m_count->SetValue(int(m_colors.size()));
        if (!visual_change) return false;
        m_notice = _L("打印机耗材已同步，原有区域对应关系保留。");
        changed();
        return true;
    }
    const std::vector<Color>& project_colors() const { return m_project_colors; }
    bool project_matching_valid() const {
        return current_source() != 1 || (m_enabled && m_project_error.empty() &&
            m_bound_project_identity == m_project_slot_identity);
    }
    void set_project_configuration_override(std::shared_ptr<const PresetBundle> bundle) {
        m_project_configuration_override = std::move(bundle);
        read_project();
    }
    bool apply_project_filament_bindings(const std::vector<size_t>& bindings) {
        read_project();
        auto saved = state();
        std::vector<Color> matched;
        for (size_t slot : bindings) {
            const auto found = std::find(m_project_slots.begin(), m_project_slots.end(), slot);
            if (found == m_project_slots.end()) return false;
            matched.push_back(m_project_colors[size_t(found - m_project_slots.begin())]);
        }
        if (!PrinterFilamentPreview::apply_bound(saved, bindings, matched, m_project_slot_identity)) return false;
        // Selecting the printer palette is the explicit activation point for
        // legacy workpieces as well. Older accepted models persisted the
        // printer source with enabled=false, which made the subsequent face
        // export reject the match even though the slot identity was valid.
        saved.enabled = true;
        saved.notice = _L("正在应用并自动保存打印机耗材与叠色配方……");
        restore(saved); return true;
    }
    bool apply_project_filament_palette() {
        read_project();
        if (!m_project_error.empty()) return false;
        auto saved = state();
        if (!PrinterFilamentPreview::apply(saved, m_project_slots, m_project_colors, m_project_slot_identity)) return false;
        // Applying the palette must activate the project match. This also
        // upgrades old saved color-trial records that kept source=1 but
        // enabled=false, allowing a complete face-to-slot snapshot to be
        // captured and saved.
        saved.enabled = true;
        saved.notice = _L("已将模型预览匹配到打印机耗材；保存或导入后提交，可撤销。");
        restore(saved);
        return true;
    }
    bool apply_project_filament_basis(bool physical_only = false) {
        read_project();
        if (!m_histogram || !m_project_error.empty() || m_project_colors.empty()) return false;
        std::vector<Color> colors = m_project_colors;
        std::vector<size_t> slots = m_project_slots;
        if (physical_only) {
            colors.clear();
            slots.clear();
            const auto* bundle = m_project_configuration_override
                ? m_project_configuration_override.get() : wxGetApp().preset_bundle;
            if (!bundle) return false;
            const auto snapshot = PrinterFilamentConfig::capture(*bundle);
            if (!snapshot.ready) return false;
            for (const auto& slot : snapshot.slots) {
                if (!slot.exists || !slot.configured || !slot.compatible) continue;
                const wxColour color(wxString::FromUTF8(slot.color));
                if (!color.IsOk()) continue;
                colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
                slots.push_back(slot.slot);
            }
            if (colors.empty()) return false;
        }
        const auto basis = PreviewPalette::make_automatic_color_basis(*m_histogram, colors.size(),
            m_geometry_id, m_fidelity->GetValue(), m_automatic_units.get());
        auto saved = state();
        if (!PrinterFilamentPreview::apply_automatic_basis(saved, basis, slots,
                colors, m_project_slot_identity)) return false;
        saved.notice = physical_only
            ? wxString::Format(_L("已按 %llu 个真实实体耗材重建模型配色；空槽不会制造额外色块。"),
                static_cast<unsigned long long>(colors.size()))
            : wxString::Format(_L("已按 %llu 个打印机耗材与叠色槽重建模型配色；空槽不会制造额外色块。"),
                static_cast<unsigned long long>(m_project_colors.size()));
        restore(saved);
        return true;
    }
    void activate_for_beauty_semantics() {
        m_semantic->SetValue(false);
        update();
    }
    bool semantic_optimization() const { return false; }
    bool color_block_postprocess_enabled() const { return honor_suite_request_pending(); }
    bool portrait_color_count_supported() const { return false; }
    const std::vector<Color>& semantic_palette() const { return m_semantic_colors.empty() ? m_colors : m_semantic_colors; }
    const std::vector<Color>& semantic_mapping_palette() const { return m_semantic_mapping.empty() ? semantic_palette() : m_semantic_mapping; }
    const std::vector<Color>& semantic_portrait_card() const { return m_semantic_card; }
    const std::vector<std::string>& semantic_palette_roles() const { return m_semantic_roles; }
    const SemanticColoring::SemanticRegionSlotBindings& semantic_region_slots() const { return m_semantic_region_slots; }
    void set_semantic_region_availability(std::array<bool, SemanticColoring::semantic_region_slot_count> available) {
        m_region_available = available; update();
    }
    void set_semantic_status(const wxString& message, bool busy) {
        if (m_semantic_status->GetLabel() == message && m_semantic_busy == busy) return;
        m_semantic_busy = busy;
        m_semantic_status->SetLabel(message); m_semantic_status->Hide();
        m_semantic_status->Wrap(std::max(FromDIP(220), GetClientSize().x - FromDIP(12)));
        m_semantic_cancel->Hide();
        sync_workbench_detail_choices(); Layout();
        if (auto* parent = GetParent()) {
            parent->Layout();
            if (auto* scroll = dynamic_cast<wxScrolledWindow*>(parent))
                scroll->FitInside();
            if (auto* grandparent = parent->GetParent()) grandparent->Layout();
        }
    }
    using State = ModelPreviewColorState;
    // Same-workpiece reloads retain the import source even after switching
    // away from manual trial. The on-disk trial schema stays compatible.
    static std::vector<Color> manual_import_palette(const State& saved) {
        if (saved.source != 2) return saved.manual_colors;
        return saved.colors;
    }
    State state() const {
        State saved;
        saved.colors = m_colors; saved.mapping_colors = m_mapping_colors;
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) saved.locks[i] = m_locks[i]->GetValue();
        saved.source = honor_suite_active() ? 2 : current_source();
        saved.count = m_count->GetValue();
        saved.automatic_count = m_automatic_count;
        saved.manual_count = m_manual_count;
        saved.project_count = m_project_count;
        saved.automatic_basis_signature = m_automatic_basis.basis_signature;
        if (m_color_card_remap.valid(m_mapping_colors.size())) {
            saved.color_card_target_palette = m_color_card_remap.target_palette;
            saved.color_card_target_slot_ids = m_color_card_remap.target_slot_ids;
            saved.source_to_target_slot = m_color_card_remap.source_to_target_slot;
            saved.color_card_weighted_error = m_color_card_remap.weighted_error;
        }
        saved.enabled = m_enabled; saved.fidelity = m_fidelity->GetValue(); saved.lighting = lighting();
        saved.project_signature = m_project_signature; saved.notice = m_notice;
        saved.honor_suite_fallback_reason = m_honor_suite_fallback_reason;
        saved.manual_colors = m_manual_colors;
        if (honor_suite_active()) {
            saved.suite_id = m_active_suite_id;
            saved.suite_revision = m_active_suite_revision;
            saved.manual_count = saved.count;
        }
        saved.semantic_optimization = false;
        saved.semantic_region_slots = m_semantic_region_slots;
        saved.semantic_role_uids=m_semantic_roles;
        if (m_colors.size() <= 6) {
            saved.semantic_palette = semantic_palette(); saved.semantic_mapping_palette = semantic_mapping_palette();
            if (saved.semantic_palette.size() == m_colors.size() &&
                saved.semantic_mapping_palette.size() == m_colors.size())
                saved.semantic_portrait_card = m_semantic_card;
            else {
                saved.semantic_palette.clear(); saved.semantic_mapping_palette.clear();
            }
        }
        if (saved.source == 1) {
            saved.project_slot_identity = m_bound_project_identity;
            saved.project_color_slots = m_project_color_slots;
            if (!saved.semantic_palette.empty()) saved.project_semantic_slots = m_project_semantic_slots;
            PrinterFilamentPreview::bind_implicit_semantic_palette(saved);
        }
        return saved;
    }
    // Same-workpiece comparison/editing keeps the user's assignments. A new
    // library model still uses load() and starts with its own suggestions.
    void restore(const State& saved) {
        // Textured candidates can have no vertex/face histogram after their
        // printer colors are baked. Restore only the already-normalized,
        // identity-bound printer state in that case; every other source still
        // requires the model's own color evidence.
        const bool baked_printer_state = !m_histogram &&
            PrinterFilamentPreview::can_restore_baked_candidate_without_histogram(saved);
        if ((!m_histogram && !baked_printer_state) || saved.colors.empty() ||
            saved.colors.size() > PreviewPalette::max_preview_colors ||
            saved.colors.size() != saved.mapping_colors.size()) return;
        const auto* restored_suite = saved.source == 2
            ? find_honor_filament_suite(saved.suite_id, saved.suite_revision) : nullptr;
        set_source_internal(restored_suite ? honor_suite_source : saved.source);
        m_last_source = restored_suite ? honor_suite_source : saved.source;
        m_automatic_count = std::clamp(saved.automatic_count, 1, int(PreviewPalette::max_preview_colors));
        m_manual_count = std::clamp(saved.manual_count, 1, int(PreviewPalette::max_preview_colors));
        m_project_count = std::clamp(saved.project_count, 0, int(PreviewPalette::max_preview_colors));
        m_count->SetValue(restored_suite ? int(restored_suite->physical_slots.size()) :
            saved.source == 0 ? m_automatic_count : saved.source == 2 ? m_manual_count : saved.count);
        m_fidelity->SetValue(saved.fidelity); m_lighting->SetValue(saved.lighting);
        m_colors = saved.colors; m_mapping_colors = saved.mapping_colors;
        set_current_automatic_basis(size_t(std::max(1, saved.count)), m_mapping_colors);
        m_color_card_remap = {};
        m_color_card_palette.clear();
        if (!saved.color_card_target_palette.empty()) {
            m_color_card_remap = PreviewPalette::minimum_cost_color_card_remap(m_automatic_basis,
                saved.color_card_target_palette, saved.color_card_target_slot_ids);
            if (m_color_card_remap.valid(m_automatic_basis.source_palette.size())) {
                m_color_card_palette = m_color_card_remap.target_palette;
                m_mapping_colors = m_automatic_basis.source_palette;
                m_colors = m_color_card_remap.source_order_targets();
            } else m_color_card_remap = {};
        }
        m_manual_colors = manual_import_palette(saved);
        m_semantic_colors = saved.semantic_palette; m_semantic_mapping = saved.semantic_mapping_palette;
        m_semantic_card = saved.semantic_portrait_card;
        m_semantic_roles=saved.semantic_role_uids.empty() ? portrait_roles_for_card(
            saved.semantic_palette.empty()?saved.colors:saved.semantic_palette,saved.semantic_portrait_card) : saved.semantic_role_uids;
        m_semantic_region_slots = saved.semantic_region_slots;
        sync_active_palette_state();
        m_semantic->SetValue(false);
        m_enabled = saved.enabled; m_notice = saved.notice;
        m_honor_suite_request.reset();
        m_honor_suite_used_colors = restored_suite ? saved.colors.size() : 0;
        m_honor_suite_fallback_reason = restored_suite ? saved.honor_suite_fallback_reason : wxString{};
        if (!restored_suite && !saved.suite_id.empty())
            m_notice = _L("保存的荣耀耗材套装版本不可用，已保留实际六色外观并恢复为手动试色。");
        m_bound_project_identity = saved.project_slot_identity;
        m_project_color_slots = saved.project_color_slots;
        m_project_semantic_slots = saved.project_semantic_slots;
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) m_locks[i]->SetValue(saved.locks[i]);
        if (saved.source == 1) {
            if (saved.project_slot_identity.empty()) set_source_internal(2);
            else if (synchronize_project_colors(true)) return;
        }
        changed();
    }
    std::function<void(bool)> on_semantic_mode;
    std::function<void()> on_semantic_cancel;
    std::function<void()> on_changed;
    std::function<void()> on_region_changed;
    std::function<void()> on_project_colors_changed;
    std::function<void(size_t)> on_project_color_edit;
    std::function<void(int, int)> on_source_changed;
private:
    static wxColour wx_color(Color c) {
        return wxColour(int(std::lround(c[0]*255)), int(std::lround(c[1]*255)), int(std::lround(c[2]*255)));
    }
    void set_current_automatic_basis(size_t requested_count, const std::vector<Color>& source_palette) {
        m_automatic_basis = {};
        if (!m_histogram || source_palette.empty()) return;
        m_automatic_basis.geometry_id = m_geometry_id;
        m_automatic_basis.source_color_signature = m_histogram->signature();
        m_automatic_basis.requested_count = std::clamp(requested_count, size_t(1), PreviewPalette::max_preview_colors);
        m_automatic_basis.source_palette = source_palette;
        m_automatic_basis.source_area = m_histogram->group_weights(source_palette);
        PreviewPalette::attach_automatic_color_units(m_automatic_basis, m_automatic_units.get());
        m_automatic_basis.basis_signature = PreviewPalette::automatic_basis_signature(m_automatic_basis);
    }
    struct HonorSuiteRequest {
        std::string geometry_id;
        std::string suite_id;
        std::string suite_revision;
        std::string context;
        std::vector<Color> palette;
        std::vector<std::string> palette_roles;
        std::map<std::string, size_t> fixed_role_slots;
        PreviewPalette::AutomaticColorBasis automatic_basis;
        PreviewPalette::ColorCardRemap fallback;
    };
    bool prepare_honor_suite_request() {
        const auto* suite = active_honor_suite();
        if (!m_histogram || !suite || !suite->valid() || m_geometry_id.empty()) return false;
        HonorSuiteRequest request;
        request.geometry_id = m_geometry_id;
        request.suite_id = suite->id;
        request.suite_revision = suite->revision;
        request.palette_roles = suite->portrait_palette_roles();
        request.fixed_role_slots = suite->semantic_role_slot_indices();
        for (const auto& slot : suite->physical_slots) {
            const wxColour color(wxString::FromUTF8(slot.color));
            if (!color.IsOk()) return false;
            request.palette.push_back({color.Red() / 255.f, color.Green() / 255.f, color.Blue() / 255.f});
        }
        if (request.palette.size() != suite->physical_slots.size() ||
            request.palette_roles.size() != request.palette.size() ||
            request.fixed_role_slots.empty()) return false;
        request.automatic_basis = PreviewPalette::make_automatic_color_basis(*m_histogram,
            request.palette.size(), m_geometry_id, m_fidelity->GetValue(), m_automatic_units.get());
        request.fallback = PreviewPalette::minimum_cost_color_card_remap(
            request.automatic_basis, request.palette);
        if (!request.automatic_basis.valid() ||
            !request.fallback.valid(request.automatic_basis.source_palette.size())) return false;
        request.context = request.suite_id + "@" + request.suite_revision + ":" +
            request.geometry_id + ":" + request.automatic_basis.basis_signature;
        m_honor_suite_used_colors = 0;
        m_honor_suite_fallback_reason.clear();
        m_honor_suite_request = std::move(request);
        m_notice = _L("正在识别人像区域并应用荣耀套装，当前模型预览保持不变。 ");
        update();
        return true;
    }
    void clear_honor_suite_request() {
        m_honor_suite_request.reset();
        m_honor_suite_used_colors = 0;
    }
    // Same source as the native textured import: configured physical slots first,
    // followed by valid mixed recipes. Their native slot ids remain authoritative.
    std::shared_ptr<const PresetBundle> m_project_configuration_override;
    bool read_project() {
        std::vector<Color> colors; std::vector<wxString> names; std::vector<size_t> slots;
        WorkbenchPrinterFilaments snapshot;
        if (const auto* bundle = m_project_configuration_override ? m_project_configuration_override.get() : wxGetApp().preset_bundle) snapshot = PrinterFilamentConfig::capture(*bundle);
        for (const auto& slot : snapshot.slots) if (slot.exists && slot.configured) {
            const wxColour color(wxString::FromUTF8(slot.color));
            colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            slots.push_back(slot.slot);
            names.push_back(wxString::Format(_L("打印机耗材 %u · "), unsigned(slot.slot+1)) + wxString::FromUTF8(slot.preset));
        }
        for (const auto& recipe : snapshot.mixed) if (recipe.valid) {
            const wxColour color(wxString::FromUTF8(recipe.color));
            if (!color.IsOk()) continue;
            colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            slots.push_back(recipe.slot);
            names.push_back(wxString::Format(_L("叠色 %u · "), unsigned(recipe.slot + 1)) + wxString::FromUTF8(recipe.recipe));
        }
        const wxString signature = wxString::FromUTF8(snapshot.signature);
        const bool different = signature != m_project_signature;
        m_project_signature = signature;
        m_project_colors = std::move(colors); m_project_names = std::move(names);
        m_project_error = snapshot.ready ? wxString{} : wxString::FromUTF8(snapshot.reason.empty() ? "请配置打印机耗材" : snapshot.reason);
        m_project_slots = std::move(slots); m_project_slot_identity = snapshot.identity;
        return different;
    }
    void recompute(bool enable = true) {
        if (!m_histogram) return;
        const auto started = std::chrono::steady_clock::now();
        if (m_notice == _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。")) m_notice.clear();
        const int source = current_source();
        std::vector<Color> locked;
        for (size_t i = 0; i < m_colors.size(); ++i) if (m_locks[i]->GetValue()) locked.push_back(m_colors[i]);
        if (source == 1) {
            m_automatic_basis = PreviewPalette::make_automatic_color_basis(*m_histogram,
                size_t(std::max(1, m_automatic_count)), m_geometry_id, m_fidelity->GetValue(), m_automatic_units.get());
            m_color_card_remap = {};
            m_color_card_palette.clear();
            m_semantic_roles.clear();
            m_colors = m_project_colors;
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            m_mapping_colors = m_colors;
            m_project_color_slots = m_project_slots;
            m_project_semantic_slots = m_project_slots;
            m_bound_project_identity = m_project_slot_identity;
            if (!m_project_error.empty()) { m_colors.clear(); m_mapping_colors.clear(); }
            if (!m_colors.empty()) {
                m_project_count = int(m_colors.size());
                m_count->SetValue(m_project_count);
            }
        } else if (honor_suite_active()) {
            const auto* suite = active_honor_suite();
            std::vector<Color> card;
            for (const auto& slot : suite->physical_slots) {
                const wxColour color(wxString::FromUTF8(slot.color));
                card.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            }
            m_automatic_basis = PreviewPalette::make_automatic_color_basis(*m_histogram, card.size(),
                m_geometry_id, m_fidelity->GetValue(), m_automatic_units.get());
            m_color_card_remap = PreviewPalette::minimum_cost_color_card_remap(m_automatic_basis, card);
            if (!m_color_card_remap.valid(m_automatic_basis.source_palette.size())) {
                m_notice = _L("原始配色证据无法建立荣耀套装区域映射，已保留当前预览。");
                update();
                return;
            }
            m_mapping_colors = m_automatic_basis.source_palette;
            m_colors = m_color_card_remap.source_order_targets();
            m_color_card_palette = std::move(card);
            m_count->SetValue(int(suite->physical_slots.size()));
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            m_semantic_roles.clear();
            m_notice = wxString::Format(_L("荣耀耗材套装 · 模型当前使用 %llu/6 色。"),
                static_cast<unsigned long long>(m_mapping_colors.size()));
        } else if (source >= 3 && size_t(source - 3) < m_packs.size()) {
            m_semantic_roles.clear();
            std::vector<Color> card;
            for (const auto& hex : m_packs[source - 3].colors) {
                const wxColour color(wxString::FromUTF8(hex));
                card.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            }
            m_automatic_basis = PreviewPalette::make_automatic_color_basis(*m_histogram, card.size(),
                m_geometry_id, m_fidelity->GetValue(), m_automatic_units.get());
            m_color_card_remap = PreviewPalette::minimum_cost_color_card_remap(m_automatic_basis, card);
            m_mapping_colors = m_automatic_basis.source_palette;
            m_colors = m_color_card_remap.source_order_targets();
            m_color_card_palette = std::move(card);
            m_count->SetValue(int(m_color_card_palette.size()));
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            m_notice = wxString::Format(_L("已保留 %llu 个 AI 配色源区域，并与色卡槽位做全局一对一匹配。"),
                static_cast<unsigned long long>(m_mapping_colors.size()));
        } else if (source == 0) {
            m_color_card_remap = {};
            m_color_card_palette.clear();
            m_semantic_roles.clear();
            if (locked.size() > size_t(m_count->GetValue())) {
                m_count->SetValue(int(locked.size()));
                m_notice = _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。");
            }
            const int requested = m_count->GetValue();
            auto candidate = m_histogram->trial_palette(size_t(requested), locked, m_fidelity->GetValue());
            if (candidate.size() != size_t(requested)) {
                const int retained = std::clamp(m_automatic_count, 1, int(PreviewPalette::max_preview_colors));
                m_count->SetValue(retained);
                m_notice = wxString::Format(
                    _L("原始配色证据只能形成 %u 个可区分颜色，已保留此前 %d 色预览。"),
                    unsigned(candidate.size()), retained);
                update();
                return;
            }
            m_colors = std::move(candidate);
            m_mapping_colors = m_colors;
            set_current_automatic_basis(size_t(m_count->GetValue()), m_mapping_colors);
            m_suggestion_colors = m_colors;
            m_automatic_count = m_count->GetValue();
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) m_locks[i]->SetValue(i < m_colors.size() &&
                std::find(locked.begin(), locked.end(), m_colors[i]) != locked.end());
        } else {
            m_color_card_remap = {};
            m_color_card_palette.clear();
            const auto suggestions = m_histogram->trial_palette(PreviewPalette::max_preview_colors, {}, true);
            while (m_colors.size() < size_t(m_count->GetValue())) {
                const Color suggestion = suggestions.empty() ? Color{.5f,.5f,.5f} : suggestions[m_colors.size() % suggestions.size()];
                m_colors.push_back(suggestion); m_mapping_colors.push_back(suggestion);
            }
            m_colors.resize(size_t(m_count->GetValue()));
            m_mapping_colors.resize(m_colors.size());
            if (m_semantic_colors.size() != m_colors.size()) {
                const auto original_mapping = semantic_mapping_palette();
                const size_t previous_size = m_semantic_colors.size();
                m_semantic_colors.resize(m_colors.size()); m_semantic_mapping = original_mapping;
                m_semantic_mapping.resize(m_colors.size());
                for (size_t i = previous_size; i < m_colors.size(); ++i)
                    m_semantic_colors[i] = m_semantic_mapping[i] = m_colors[i];
                if(m_semantic_card.empty()) m_semantic_roles.clear();
            }
            set_current_automatic_basis(size_t(m_count->GetValue()), m_mapping_colors);
        }
        sync_active_palette_state();
        if (source == 0) m_automatic_count = m_count->GetValue();
        else if (source == 2) m_manual_count = m_count->GetValue();
        m_enabled = enable && !m_colors.empty(); changed();
        BOOST_LOG_TRIVIAL(info) << "AI protected color palette: source=" << source << ", colors=" << m_colors.size()
            << ", locks=" << locked.size() << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    }
    void set_workbench_count(int value) {
        value = std::clamp(value, 1, int(PreviewPalette::max_preview_colors));
        if (m_count->GetValue() == value) {
            if (m_workbench_count) m_workbench_count->SetValue(value);
            if (m_workbench_count_input) m_workbench_count_input->SetValue(value);
            return;
        }
        m_count->SetValue(value);
        recompute();
    }
    void edit_color(size_t i) {
        // In printer-filament mode a swatch represents a real slot. Open the
        // native draft configuration instead of silently creating a trial color.
        if (current_source() == 1) {
            if (on_project_color_edit && i < m_project_slots.size()) on_project_color_edit(m_project_slots[i]);
            return;
        }
        if (m_color_card_remap.valid(m_mapping_colors.size()) && i < m_color_card_palette.size()) {
            const auto source = std::find(m_color_card_remap.source_to_target_slot.begin(),
                m_color_card_remap.source_to_target_slot.end(), i);
            if (source == m_color_card_remap.source_to_target_slot.end()) return;
            wxColourData data; data.SetColour(wx_color(m_color_card_palette[i])); data.SetChooseFull(true);
            wxColourDialog dialog(this, &data);
            dialog.SetTitle(_L("修改色卡试色颜色（保留 AI 配色分区）"));
            if (dialog.ShowModal() != wxID_OK) return;
            const wxColour color = dialog.GetColourData().GetColour();
            const Color replacement {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
            m_color_card_palette[i] = replacement;
            m_color_card_remap.target_palette[i] = replacement;
            m_colors[size_t(source - m_color_card_remap.source_to_target_slot.begin())] = replacement;
            set_source_internal(2);
            m_last_source = 2;
            m_manual_count = std::clamp(m_count->GetValue(), 1, int(PreviewPalette::max_preview_colors));
            m_notice = _L("已转为手动试色；AI 配色源区域和一对一槽位身份保持不变。");
            m_enabled = true;
            changed();
            return;
        }
        if (semantic_optimization() && i < semantic_palette().size()) {
            const auto original = semantic_palette();
            wxColourData data; data.SetColour(wx_color(original[i])); data.SetChooseFull(true);
            wxColourDialog dialog(this, &data); dialog.SetTitle(_L("修改区域试色颜色（不改打印机耗材）"));
            if (dialog.ShowModal() != wxID_OK) return;
            const wxColour color = dialog.GetColourData().GetColour();
            const Color replacement {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
            m_semantic_mapping = semantic_mapping_palette();
            m_semantic_colors = original; m_semantic_colors[i] = replacement;
            for (auto& target : m_colors) if (target == original[i]) target = replacement;
            sync_active_palette_state();
            set_source_internal(2); m_notice = _L("已保留区域对应关系；打印机耗材未修改。");
            m_enabled = true;
            changed(); return;
        }
        if (i >= m_colors.size()) return;
        wxColourData data; data.SetColour(wx_color(m_colors[i])); data.SetChooseFull(true);
        wxColourDialog dialog(this, &data);
        dialog.SetTitle(_L("修改试色颜色（不改打印机耗材）"));
        if (dialog.ShowModal() != wxID_OK) return;
        const wxColour color = dialog.GetColourData().GetColour();
        const Color previous = m_colors[i];
        if (m_semantic_colors.empty()) m_semantic_colors = m_colors;
        m_semantic_mapping = semantic_mapping_palette();
        m_colors[i] = {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
        for (auto& candidate : m_semantic_colors) if (candidate == previous) candidate = m_colors[i];
        sync_active_palette_state();
        // Preserve the source assignment when a target color is edited. Editing
        // green to blue must recolor that group, not make the blue target unused.
        set_source_internal(2);
        m_notice = _L("已转为手动试色，保留各色对应范围；打印机耗材未修改。");
        m_locks[i]->SetValue(true); recompute();
    }
    void changed() {
        // Every manual path (ordinary edit, region edit, recompute and restore)
        // captures the displayed targets before the host can reload the model.
        m_manual_colors = manual_import_palette(state());
        update(); if (on_changed) on_changed();
    }
    void remember_source_count(int source) {
        if (source == 0)
            m_automatic_count = std::clamp(m_count->GetValue(), 1, int(PreviewPalette::max_preview_colors));
        else if (source == 2)
            m_manual_count = std::clamp(m_count->GetValue(), 1, int(PreviewPalette::max_preview_colors));
        else if (source == 1)
            m_project_count = std::max(0, m_count->GetValue());
    }
    void region_changed() { update(); if (on_region_changed) on_region_changed(); }
    static std::vector<Color> portrait_card_colors() {
        std::vector<Color> colors;
        for (const auto& hex : young_portrait_color_pack().colors) {
            const wxColour color(wxString::FromUTF8(hex));
            colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
        }
        return colors;
    }
    static bool is_portrait_card(const std::vector<Color>& colors) {
        const auto card = young_portrait_color_pack();
        if (colors.size() != card.colors.size()) return false;
        for (size_t i = 0; i < colors.size(); ++i)
            if (wx_color(colors[i]).GetAsString(wxC2S_HTML_SYNTAX).CmpNoCase(wxString::FromUTF8(card.colors[i])) != 0) return false;
        return true;
    }
    // Keep every palette-shaped state tied to the active physical/display
    // slots. A stale semantic snapshot is disposable; ordinary and manual
    // source/target assignments are preserved when their lengths are valid.
    void sync_active_palette_state() {
        if (m_mapping_colors.size() != m_colors.size()) {
            const size_t previous = m_mapping_colors.size();
            m_mapping_colors.resize(m_colors.size());
            for (size_t i = previous; i < m_mapping_colors.size(); ++i) m_mapping_colors[i] = m_colors[i];
        }
        if (m_semantic_colors.size() != m_colors.size() || m_semantic_mapping.size() != m_colors.size()) {
            m_semantic_colors = m_colors;
            m_semantic_mapping = m_colors;
        }
        if (m_colors.size() > 6 ||
            (!m_semantic_card.empty() && (m_semantic_card.size() != 6 || !is_portrait_card(m_semantic_card))))
            m_semantic_card.clear();
        if(!valid_portrait_roles(m_semantic_roles,m_colors.size())) m_semantic_roles.clear();
        if(m_semantic_roles.empty() && !m_semantic_card.empty())
            m_semantic_roles=portrait_roles_for_card(semantic_palette(),m_semantic_card);
        for (int& slot : m_semantic_region_slots)
            if (slot < -1 || size_t(slot) >= m_colors.size()) slot = -1;
    }
    void apply_portrait_mapping() {
        const auto mapping = portrait_card_slot_mapping(m_colors, m_semantic_card);
        if (!mapping.enabled) return;
        m_mapping_colors = mapping.mapping_colors; m_colors = mapping.target_colors;
        m_semantic_colors=m_colors;m_semantic_mapping=m_mapping_colors;
        m_semantic_roles=portrait_roles_for_card(m_colors,m_semantic_card);
        m_count->SetValue(int(m_colors.size()));
        sync_active_palette_state();
        m_notice = _L("已按人物色卡建议配色，可通过人像区域优化进一步调整；局部修改请到 3D 美颜。");
    }
    void wrap_status() {
        const int width = std::max(FromDIP(220), GetClientSize().x - FromDIP(12));
        wxString wrapped, line;
        for (wxUniChar character : m_status_text) {
            if (character == '\r') continue;
            if (character == '\n') {
                wrapped += line + "\n"; line.clear();
                continue;
            }
            wxString next = line; next += character;
            if (!line.empty() && m_status->GetTextExtent(next).x > width) {
                wrapped += line + "\n"; line.clear();
            }
            line += character;
        }
        wrapped += line;
        m_status->SetLabel(wrapped);
        wxClientDC dc(m_status);
        dc.SetFont(m_status->GetFont());
        m_status->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + FromDIP(2)));
    }
    void update() {
        const int source = current_source();
        const bool suite_active = honor_suite_active();
        // Semantic recognition is now an on-demand annotation step owned by
        // Beauty Workbench automatic partitioning.  Palette controls never
        // expose or start the frozen portrait recoloring workflow.
        m_semantic->Hide();
        m_semantic_cancel->Hide();
        m_semantic_status->Hide();
        if (m_workbench_source) {
            if (m_workbench_mode) {
                m_workbench_mode->SetSelection(m_host_portrait_mode ? 1 : 0);
                m_workbench_mode->Enable(m_workbench_editable && bool(m_histogram) && portrait_color_count_supported());
            }
            m_workbench_source->SetSelection(visible_source(source));
            m_workbench_source->Enable(m_workbench_editable && bool(m_histogram));
            m_workbench_count->SetValue(m_count->GetValue());
            m_workbench_count_input->SetValue(m_count->GetValue());
            const bool count_enabled = m_workbench_editable && bool(m_histogram) && !suite_active &&
                (source == 0 || source == 2);
            if (m_workbench_count->IsEnabled() != count_enabled) m_workbench_count->Enable(count_enabled);
            if (m_workbench_count_input->IsEnabled() != count_enabled) m_workbench_count_input->Enable(count_enabled);
            m_workbench_count->Show(!suite_active);
            if (m_workbench_count_row) m_workbench_count_row->Show(!suite_active);
            if (m_workbench_suite_panel) m_workbench_suite_panel->Show(suite_active);
            if (m_workbench_suite_category)
                m_workbench_suite_category->Enable(suite_active && m_workbench_editable && bool(m_histogram));
            if (m_workbench_suite_choice)
                m_workbench_suite_choice->Enable(suite_active && m_workbench_editable && bool(m_histogram));
        }
        if (m_suite_detail_panel) m_suite_detail_panel->Show(suite_active);
        if (m_detail_count_panel) m_detail_count_panel->Show(!suite_active);
        m_count->Show(!suite_active && m_detail_choices.empty());
        if (m_count_suffix) m_count_suffix->Show(!suite_active && m_detail_choices.empty());
        const bool show_card_slots = source != 1 && m_color_card_remap.valid(m_mapping_colors.size()) &&
            !m_color_card_palette.empty();
        const auto& displayed_colors = source == 1 ? m_project_colors : show_card_slots ? m_color_card_palette :
            semantic_optimization() ? semantic_palette() : m_colors;
        const auto& region_palette = semantic_optimization() ? semantic_palette() : m_colors;
        Show(bool(m_histogram));
        // Comparison belongs to the single GL canvas; the old card-level
        // preview/original toggle remains only as an internal compatibility
        // hook and is never exposed in the detail surface.
        m_toggle->Hide();
        m_count->Enable(!suite_active && (source == 0 || source == 2)); m_fidelity->Enable(source == 0);
        m_semantic->Enable(portrait_color_count_supported());
        m_semantic->SetToolTip(_L("AI配色为四至六色时，按原始颜色建立区域并绑定人像语义；其他色数使用普通配色。"));
        m_lighting->Enable(m_enabled);
        const std::array<wxString, SemanticColoring::semantic_region_slot_count> region_names {{
            _L("眼白"), _L("虹膜"), _L("眉毛"), _L("嘴唇")
        }};
        const bool regions_enabled = semantic_optimization() && m_enabled;
        m_region_reset->Enable(regions_enabled && std::any_of(m_semantic_region_slots.begin(),
            m_semantic_region_slots.end(), [](int slot) { return slot >= 0; }));
        for (size_t region = 0; region < region_names.size(); ++region) {
            auto* choice = m_region_slots[region];
            choice->Clear();
            if (!m_region_available[region]) {
                choice->Append(_L("未识别"));
                choice->SetSelection(0);
                choice->Enable(false);
                continue;
            }
            choice->Append(_L("自动区域颜色"));
            for (size_t slot = 0; slot < region_palette.size(); ++slot)
                choice->Append(wxString::Format(_L("槽位 %u · %s"), unsigned(slot + 1),
                    wx_color(region_palette[slot]).GetAsString(wxC2S_HTML_SYNTAX)));
            const int selected = m_semantic_region_slots[region];
            choice->SetSelection(selected >= 0 && size_t(selected) < region_palette.size() ? selected + 1 : 0);
            choice->Enable(regions_enabled);
        }
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) {
            const bool visible = i < displayed_colors.size();
            m_swatches[i]->Show(visible); m_locks[i]->Show(visible && source == 0);
            if (!visible) continue;
            const wxColour color = wx_color(displayed_colors[i]);
            const wxString hex = color.GetAsString(wxC2S_HTML_SYNTAX);
            m_swatches[i]->SetLabel(hex); m_swatches[i]->SetBackgroundColour(color);
            // Keep every slot identical so a long label never stretches one
            // card and shifts the following rows.
            m_swatches[i]->SetMinSize(wxSize(FromDIP(128), FromDIP(34)));
            m_swatches[i]->SetMaxSize(wxSize(FromDIP(128), FromDIP(34)));
            m_swatches[i]->SetForegroundColour((color.Red()*299 + color.Green()*587 + color.Blue()*114 > 145000) ? *wxBLACK : *wxWHITE);
            const auto mapped_source = show_card_slots ? std::find(m_color_card_remap.source_to_target_slot.begin(),
                m_color_card_remap.source_to_target_slot.end(), i) : m_color_card_remap.source_to_target_slot.end();
            const bool card_slot_used = !show_card_slots || mapped_source != m_color_card_remap.source_to_target_slot.end();
            m_swatches[i]->Enable(source == 1 || card_slot_used);
            // Preserve slot identity when two physical slots have the same RGB.
            const size_t project_index = i;
            wxString suite_name;
            if (suite_active && active_honor_suite() && i < active_honor_suite()->physical_slots.size())
                suite_name = wxString::Format(_L("实体耗材 %u · "), unsigned(i + 1)) +
                    wxString::FromUTF8(active_honor_suite()->physical_slots[i].name) + _L(" · ");
            m_swatches[i]->SetToolTip(suite_name +
                (source == 1 && project_index < m_project_names.size() ? m_project_names[project_index] + " · " : "") +
                hex + (source == 1 ? _L(" · 点击配置真实耗材") : !card_slot_used ? _L(" · 当前模型未使用") :
                    show_card_slots ? _L(" · 已一对一映射到 AI 配色源槽，点击修改显示色") : _L(" · 点击改为试色颜色")));
            m_locks[i]->SetToolTip(_L("固定此颜色，重新推荐时不被其他颜色合并。") + hex);
        }
        wxString text = m_enabled ? (lighting()
            ? wxString::Format(_L("立体展示 · %u 个 AI 配色区域；光照会增加明暗深浅。"), unsigned(m_mapping_colors.size()))
            : wxString::Format(_L("纯分色 · %u 个 AI 配色区域；色卡只替换显示颜色。"), unsigned(m_mapping_colors.size())))
            : _L("原色显示 · ");
        text += suite_active ? (m_honor_suite_request
                ? _L("荣耀耗材套装；正在识别人像区域。")
                : wxString::Format(_L("荣耀耗材套装；模型当前使用 %llu/6 色。"),
                    static_cast<unsigned long long>(m_honor_suite_used_colors))) :
            source == 1 ? _L("使用打印机耗材色卡。") : source == 2 ? _L("手动试色。") :
            source >= 3 ? _L("已保存色卡试色。") : _L("可点击改色并勾选保留。");
        text += _L("仅供配色对照，不代表实际打印效果；");
        text += m_enabled ? _L("导入时可沿用当前试色，或从模型原色重新配色。") : _L("原色导入时可重新选择目标颜色数量，再匹配实际耗材。");
        if (source == 1 && !m_project_error.empty()) text += "\n" + m_project_error;
        if (!m_notice.empty()) text += "\n" + m_notice;
        sync_workbench_detail_choices();
        m_status_text = text; wrap_status(); Layout();
        // Controls must not consume the model viewport's existing minimum height.
        SetMinSize(wxSize(FromDIP(420), GetSizer()->CalcMin().y));
        if (auto* parent = GetParent()) {
            parent->Layout();
            if (auto* scroll = dynamic_cast<wxScrolledWindow*>(parent))
                scroll->FitInside();
            if (auto* grandparent = parent->GetParent()) grandparent->Layout();
        }
    }
    void sync_workbench_detail_choices() {
        for (const auto& entry : m_detail_buttons) {
            entry.button->SetLabel(entry.original->GetLabel());
            const bool cancellation = entry.original == m_semantic_cancel;
            entry.button->Enable(entry.original->IsEnabled() && (!cancellation || m_semantic_busy));
            if (cancellation) entry.button->Show(!m_workbench_mode && m_semantic_busy);
            if (const auto* tooltip = entry.original->GetToolTip())
                entry.button->SetToolTip(tooltip->GetTip());
        }
        for (const auto& entry : m_detail_switches) {
            if (entry.original == m_semantic) {
                entry.row->Hide();
                entry.original->Hide();
            }
            if (entry.lock_index >= 0) {
                entry.row->Show(current_source() == 0 && m_swatches[entry.lock_index]->IsShown());
                entry.original->Hide();
            }
            entry.toggle->SetValue(entry.original->GetValue());
            entry.toggle->Enable(entry.original->IsEnabled());
            if (const auto* tooltip = entry.original->GetToolTip()) {
                entry.toggle->SetToolTip(tooltip->GetTip());
                entry.row->SetToolTip(tooltip->GetTip());
            }
        }
        for (const auto& entry : m_detail_choices) {
            const wxArrayString items = entry.original->GetStrings();
            if (entry.choice->GetStrings() != items) entry.choice->Set(items);
            if (entry.original != m_source) {
                wxClientDC native_dc(entry.choice);
                wxGCDC dc(native_dc);
                dc.SetFont(entry.choice->GetFont());
                int width = entry.original->GetMinSize().x;
                for (const auto& item : items)
                    width = std::max(width, dc.GetTextExtent(item).x + FromDIP(40));
                // WorkbenchChoice suppresses TextInput's automatic minimum width.
                // Reserve each wrapped slot's measured width in its layout instead.
                entry.choice->GetContainingSizer()->SetMinSize(wxSize(width, -1));
            }
            entry.choice->SetSelection(entry.original->GetSelection());
            entry.choice->Enable(entry.original->IsEnabled());
            entry.choice->SetToolTip(entry.original->GetStringSelection());
        }
        if (m_detail_count_slider) {
            m_detail_count_slider->SetValue(m_count->GetValue());
            m_detail_count_slider->Enable(m_count->IsEnabled());
            m_detail_count_value->SetLabel(wxString::Format("%d", m_count->GetValue()));
        }
    }
    void sync_workbench_palette_tooltips() {
        const wxString& unavailable = m_workbench_palette_hint;
        if (m_workbench_source)
            m_workbench_source->SetToolTip(unavailable.empty()
                ? _L("选择 AI配色、手动试色或已保存色卡") : unavailable);
        if (m_workbench_count)
            m_workbench_count->SetToolTip(unavailable.empty()
                ? _L("调整 AI 配色或手动试色的目标色数") : unavailable);
        if (m_workbench_count_input)
            m_workbench_count_input->SetToolTip(unavailable.empty()
                ? _L("输入 1 至 32，或使用上下键逐个调整当前色数。") : unavailable);
    }
    struct DetailChoice {
        wxChoice* original;
        ComboBox* choice;
    };
    std::vector<DetailChoice> m_detail_choices;
    struct DetailSwitch {
        wxCheckBox* original;
        wxPanel* row;
        WorkbenchSwitch* toggle;
        int lock_index;
    };
    std::vector<DetailSwitch> m_detail_switches;
    struct DetailButton {
        wxButton* original;
        WorkbenchButton* button;
    };
    std::vector<wxButton*> m_detail_commands;
    std::vector<DetailButton> m_detail_buttons;
    wxPanel* m_detail_count_panel {nullptr};
    WorkbenchSlider* m_detail_count_slider {nullptr};
    wxStaticText* m_detail_count_value {nullptr};
    std::vector<FilamentColorPack> m_packs;
    std::vector<int> m_source_visible_to_internal;
    bool m_host_portrait_mode {false};
    wxWeakRef<WorkbenchPaletteChoice> m_workbench_mode;
    wxWeakRef<WorkbenchPaletteChoice> m_workbench_source;
    wxPanel* m_workbench_suite_panel {nullptr};
    WorkbenchPaletteChoice* m_workbench_suite_category {nullptr};
    WorkbenchPaletteChoice* m_workbench_suite_choice {nullptr};
    wxStaticText* m_workbench_suite_summary {nullptr};
    wxWeakRef<wxSlider> m_workbench_count;
    wxWeakRef<WorkbenchNumberStepper> m_workbench_count_input;
    wxWeakRef<wxStaticText> m_workbench_count_label;
    wxPanel* m_workbench_count_row {nullptr};
    bool m_workbench_editable {false};
    wxButton* m_toggle;
    wxChoice* m_source;
    wxSpinCtrl* m_count;
    wxStaticText* m_count_suffix {nullptr};
    wxPanel* m_suite_detail_panel {nullptr};
    wxStaticText* m_suite_detail_summary {nullptr};
    wxCheckBox* m_fidelity;
    wxCheckBox* m_lighting;
    wxCheckBox* m_semantic;
    wxButton* m_semantic_cancel;
    wxStaticText* m_semantic_status;
    bool m_semantic_busy {false};
    wxButton* m_region_reset;
    std::array<wxChoice*, SemanticColoring::semantic_region_slot_count> m_region_slots {};
    std::array<bool, SemanticColoring::semantic_region_slot_count> m_region_available {};
    SemanticColoring::SemanticRegionSlotBindings m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
    std::vector<Color> m_semantic_colors, m_semantic_mapping, m_semantic_card;
    wxStaticText* m_status;
    wxString m_status_text;
    int m_status_layout_width = -1;
    bool m_status_layout_pending = false;
    std::array<wxButton*, PreviewPalette::max_preview_colors> m_swatches {};
    std::array<wxCheckBox*, PreviewPalette::max_preview_colors> m_locks {};
    std::shared_ptr<const PreviewPalette::Histogram> m_histogram;
    std::shared_ptr<const PreviewPalette::AutomaticColorUnits> m_automatic_units;
    std::vector<Color> m_colors, m_project_colors, m_mapping_colors;
    PreviewPalette::AutomaticColorBasis m_automatic_basis;
    PreviewPalette::ColorCardRemap m_color_card_remap;
    std::vector<Color> m_color_card_palette;
    std::string m_geometry_id;
    std::vector<Color> m_suggestion_colors, m_manual_colors;
    int m_automatic_count {6}, m_manual_count {6}, m_project_count {0};
    int m_last_source {0};
    // Printer-filament source id 1 is hidden from the card selector but is
    // still needed while the workbench is in native-material trial mode.
    int m_source_internal {0};
    std::string m_active_suite_id;
    std::string m_active_suite_revision;
    std::optional<HonorSuiteRequest> m_honor_suite_request;
    size_t m_honor_suite_used_colors {0};
    wxString m_honor_suite_fallback_reason;
    std::vector<std::string> m_semantic_roles;
    std::vector<wxString> m_project_names;
    std::vector<size_t> m_project_slots;
    std::vector<size_t> m_project_color_slots, m_project_semantic_slots;
    std::string m_project_slot_identity, m_bound_project_identity;
    wxString m_notice, m_project_error, m_project_signature;
    wxString m_workbench_palette_hint;
    int m_initial_count {6};
    bool m_enabled {false};
    std::chrono::steady_clock::time_point m_last_check {};
};
} // namespace Slic3r::GUI
