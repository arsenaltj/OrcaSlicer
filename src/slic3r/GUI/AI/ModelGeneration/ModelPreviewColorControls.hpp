#pragma once

#include "ModelPreviewPalette.hpp"
#include "../Model/ColorTrialState.hpp"
#include "PortraitColorPackMapping.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "slic3r/GUI/GUI_App.hpp"
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
#include <wx/statline.h>
#include <wx/tooltip.h>
#include <wx/wrapsizer.h>
#include <wx/weakref.h>
#include <chrono>
#include <functional>
#include <memory>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
namespace SemanticColoring = AI::SemanticColoring;
// Trial colors do not modify the project. The separate color-pack action
// explicitly applies a physical-slot card through the Orca adapter.
class ModelPreviewColorControls final : public wxPanel {
public:
    using Color = PreviewPalette::Color;
    void set_workbench_detail_choices(bool enabled) {
        if (enabled == !m_detail_choices.empty()) return;
        if (enabled) {
            std::vector<wxChoice*> originals {m_source};
            originals.insert(originals.end(), m_region_slots.begin(), m_region_slots.end());
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
            std::vector<wxCheckBox*> switch_originals {m_fidelity, m_semantic, m_lighting};
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
        }
        wrap_status();
        Layout();
    }
    void set_workbench_editable(bool editable) {
        m_workbench_editable = editable;
        if (!m_workbench_source) return;
        m_workbench_source->Enable(editable && bool(m_histogram));
        if (m_workbench_mode) m_workbench_mode->Enable(editable && bool(m_histogram) && m_colors.size() <= 6);
        const int source = m_source->GetSelection();
        m_workbench_count->Enable(editable && bool(m_histogram) && (source == 0 || source == 2));
    }
    wxWindow* build_workbench_mode(wxWindow* parent) {
        auto* container = new wxPanel(parent);
        container->SetBackgroundColour(parent->GetBackgroundColour());
        auto* contents = new wxBoxSizer(wxVERTICAL);
        m_workbench_mode = new WorkbenchPaletteChoice(container);
        m_workbench_mode->Append(_L("通用模式"));
        m_workbench_mode->Append(_L("人像模式"));
        m_workbench_mode->SetName(_L("配色模式"));
        m_workbench_mode->SetToolTip(_L("人像模式使用现有的本地人像区域优化，支持 1 至 6 色。"));
        m_workbench_mode->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            if (!m_workbench_editable || !m_histogram || m_colors.size() > 6) return;
            if (on_semantic_mode) on_semantic_mode(m_workbench_mode->GetSelection() == 1);
            update();
        });
        contents->Add(m_workbench_mode, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        container->SetSizer(contents);
        return container;
    }
    void set_semantic_mode(bool portrait) {
        if (m_host_portrait_mode == portrait && m_semantic->GetValue() == portrait) return;
        m_host_portrait_mode = portrait; m_semantic->SetValue(portrait); update();
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
        auto* divider = new wxStaticLine(panel);
        divider->SetBackgroundColour(wxColour(46, 46, 49));
        root->Add(divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        m_workbench_count_label = new wxStaticText(panel, wxID_ANY,
            wxString::Format(_L("%d 色"), m_count->GetValue()), wxDefaultPosition, wxDefaultSize,
            wxALIGN_CENTER | wxST_NO_AUTORESIZE);
        m_workbench_count_label->SetForegroundColour(wxColour(235, 235, 235));
        root->Add(m_workbench_count_label, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
        m_workbench_count = new WorkbenchSlider(panel, wxID_ANY, m_count->GetValue(), 1,
            int(PreviewPalette::max_preview_colors), wxColour(255, 194, 39), "workbench_slider_thumb");
        root->Add(m_workbench_count, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        m_workbench_source->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
            m_source->SetSelection(m_workbench_source->GetSelection());
            wxCommandEvent event(wxEVT_CHOICE, m_source->GetId());
            event.SetEventObject(m_source); m_source->GetEventHandler()->ProcessEvent(event);
        });
        m_workbench_count->Bind(wxEVT_SCROLL_THUMBRELEASE, [this](wxScrollEvent&) {
            m_count->SetValue(m_workbench_count->GetValue()); recompute();
        });
        m_workbench_count->Bind(wxEVT_SCROLL_CHANGED, [this](wxScrollEvent&) {
            if (m_count->GetValue() == m_workbench_count->GetValue()) return;
            m_count->SetValue(m_workbench_count->GetValue()); recompute();
        });
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
        m_source->Append(_L("自动建议"));
        m_source->Append(_L("当前工程耗材"));
        m_source->Append(_L("手动试色"));
        m_packs = load_filament_color_packs();
        for (const auto& pack : m_packs) m_source->Append(wxString::FromUTF8(pack.name));
        m_source->SetSelection(0);
        row->Add(m_source, 1, wxRIGHT, FromDIP(6));
        m_count = new wxSpinCtrl(this, wxID_ANY, "6", wxDefaultPosition, wxSize(FromDIP(72), -1), wxSP_ARROW_KEYS,
            1, int(PreviewPalette::max_preview_colors), 6);
        m_count->SetName(_L("试色数量"));
        m_count->SetToolTip(_L("结果对照不再固定为六色，可按模型需要选择 1 至 32 色。"));
        row->Add(m_count, 0, wxRIGHT, FromDIP(4));
        row->Add(new wxStaticText(this, wxID_ANY, _L("色")), 0, wxALIGN_CENTER_VERTICAL);
        box->Add(row, 0, wxEXPAND | wxALL, FromDIP(6));
        auto* options = new wxBoxSizer(wxHORIZONTAL);
        m_fidelity = new wxCheckBox(this, wxID_ANY, _L("尽量保留小面积彩色"));
        m_fidelity->SetValue(true);
        m_fidelity->SetToolTip(_L("缩减颜色时，尽量保留有一定面积的少数彩色，例如嘴唇红、衣服绿。不是自动识别人脸或衣物；取消后更偏向大面积颜色。"));
        options->Add(m_fidelity, 1, wxALIGN_CENTER_VERTICAL);
        auto* reset = new wxButton(this, wxID_ANY, _L("重置试色"));
        options->Add(reset, 0);
        box->Add(options, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        auto* semantic_row = new wxBoxSizer(wxHORIZONTAL);
        m_semantic = new wxCheckBox(this, wxID_ANY, _L("人像区域优化"));
        m_semantic->SetValue(false);
        m_semantic->SetToolTip(_L("在本机识别皮肤、衣服和嘴唇后分别匹配颜色；识别不明确的区域沿用原有配色。"));
        semantic_row->Add(m_semantic, 1, wxALIGN_CENTER_VERTICAL);
        m_semantic_cancel = new wxButton(this, wxID_ANY, _L("取消识别"));
        semantic_row->Add(m_semantic_cancel, 0);
        m_semantic_cancel->Hide();
        box->Add(semantic_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        m_semantic_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        box->Add(m_semantic_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        m_semantic_status->Hide();
        m_semantic->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { if (on_semantic_mode) on_semantic_mode(m_semantic->GetValue()); });
        m_semantic_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (on_semantic_cancel) on_semantic_cancel(); });
        auto* pack_button = new wxButton(this, wxID_ANY, _L("应用 / 保存耗材包…"));
        m_detail_commands = {m_toggle, reset, m_semantic_cancel, pack_button};
        box->Add(pack_button, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        pack_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const bool applied = show_filament_color_packs(this);
            const wxString selected = m_source->GetStringSelection();
            while (m_source->GetCount() > 3) m_source->Delete(3);
            m_packs = load_filament_color_packs();
            for (const auto& pack : m_packs) m_source->Append(wxString::FromUTF8(pack.name));
            m_source->SetStringSelection(selected);
            if (!applied) return;
            read_project(); m_source->SetSelection(1);
            m_notice = _L("已应用工程色卡；原有涂色范围保留，当前显示实际耗材配色。");
            recompute();
        });
        m_lighting = new wxCheckBox(this, wxID_ANY, _L("光照立体展示"));
        m_lighting->SetValue(false);
        m_lighting->SetToolTip(_L("默认用纯色色块检查分色。开启后添加光照明暗，屏幕上会看到更多深浅，但基础颜色数量不变。"));
        box->Add(m_lighting, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        auto* swatches = new wxWrapSizer(wxHORIZONTAL);
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            m_swatches[i] = new wxButton(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(64), FromDIP(26)), wxBU_EXACTFIT);
            m_swatches[i]->SetName("ai_content_color");
            m_locks[i] = new wxCheckBox(this, wxID_ANY, _L("保留"));
            col->Add(m_swatches[i], 0, wxEXPAND);
            col->Add(m_locks[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(3));
            swatches->Add(col, 1, wxRIGHT, FromDIP(4));
            m_swatches[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { edit_color(i); });
            m_locks[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        }
        box->Add(swatches, 0, wxEXPAND | wxALL, FromDIP(6));
        auto* region_header = new wxBoxSizer(wxHORIZONTAL);
        region_header->Add(new wxStaticText(this, wxID_ANY, _L("语义区域槽位")), 1, wxALIGN_CENTER_VERTICAL);
        m_region_reset = new wxButton(this, wxID_ANY, _L("恢复自动区域颜色"));
        m_detail_commands.push_back(m_region_reset);
        region_header->Add(m_region_reset, 0);
        box->Add(region_header, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        const std::array<wxString, SemanticColoring::semantic_region_slot_count> region_names {{
            _L("眼白"), _L("虹膜"), _L("眉毛"), _L("嘴唇")
        }};
        auto* region_rows = new wxWrapSizer(wxHORIZONTAL);
        for (size_t i = 0; i < region_names.size(); ++i) {
            auto* region_row = new wxBoxSizer(wxVERTICAL);
            region_row->Add(new wxStaticText(this, wxID_ANY, region_names[i]), 0,
                wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(2));
            m_region_slots[i] = new wxChoice(this, wxID_ANY);
            m_region_slots[i]->SetName("ai_semantic_region_slot");
            m_region_slots[i]->SetMinSize(wxSize(FromDIP(138), -1));
            region_row->Add(m_region_slots[i], 0, wxEXPAND);
            region_rows->Add(region_row, 0, wxRIGHT | wxBOTTOM, FromDIP(5));
            m_region_slots[i]->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent&) {
                m_semantic_region_slots[i] = m_region_slots[i]->GetSelection() > 0
                    ? m_region_slots[i]->GetSelection() - 1 : -1;
                m_notice = _L("区域颜色已固定；修改区域槽位不会重新识别人像区域。");
                region_changed();
            });
        }
        box->Add(region_rows, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(3));
        m_region_reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
            m_notice = _L("已恢复自动区域颜色；识别结果保持不变。");
            region_changed();
        });
        m_status = new wxStaticText(this, wxID_ANY, "");
        box->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        SetSizer(box);
        m_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_source->GetSelection() == 1 && read_project()) {
                m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                recompute(false); return;
            }
            if (m_colors.empty()) return;
            m_enabled = !m_enabled; changed();
        });
        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear();
            if (m_source->GetSelection() == 1) read_project();
            recompute();
        });
        m_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
            if(!m_semantic_card.empty() && !m_semantic_roles.empty()) m_source->SetSelection(2);
            recompute();
        });
        m_fidelity->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        m_lighting->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            read_project();
            m_initial_count = int(PreviewPalette::initial_trial_color_count(m_project_filament_count, m_model_suggestion_count));
            m_source->SetSelection(0); m_count->SetValue(m_initial_count); m_fidelity->SetValue(true);
            m_semantic->SetValue(false);
            m_lighting->SetValue(false);
            m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear(); recompute();
        });
        Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
            const auto now = std::chrono::steady_clock::now();
            if (IsShownOnScreen() && m_histogram && m_source->GetSelection() == 1 &&
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
    void load(std::shared_ptr<const PreviewPalette::Histogram> histogram, std::vector<Color> colors) {
        m_histogram = std::move(histogram); m_colors = std::move(colors); m_mapping_colors = m_colors;
        m_model_suggestion_count = m_colors.size();
        read_project();
        m_initial_count = int(PreviewPalette::initial_trial_color_count(m_project_filament_count, m_model_suggestion_count));
        if (m_histogram && m_colors.size() != size_t(m_initial_count)) {
            m_colors = m_histogram->trial_palette(size_t(m_initial_count), {}, true);
            m_mapping_colors = m_colors;
        }
        m_semantic_colors.clear(); m_semantic_mapping.clear(); m_semantic_card.clear(); m_semantic_roles.clear();
        m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
        m_region_available = {};
        m_semantic->SetValue(false);
        sync_active_palette_state();
        set_semantic_status(wxEmptyString, false);
        m_enabled = false; m_source->SetSelection(0); m_count->SetValue(m_initial_count); m_fidelity->SetValue(true);
        m_lighting->SetValue(false);
        m_notice.clear();
        for (auto* lock : m_locks) lock->SetValue(false);
        update();
    }
    void clear() {
        m_histogram.reset(); m_colors.clear(); m_mapping_colors.clear();
        m_semantic_colors.clear(); m_semantic_mapping.clear(); m_semantic_card.clear(); m_semantic_roles.clear();
        m_semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
        m_region_available = {};
        m_enabled = false; Hide();
    }
    const std::vector<Color>& colors() const { return m_colors; }
    const std::vector<Color>& mapping_colors() const { return m_mapping_colors; }
    bool enabled() const { return m_enabled; }
    bool lighting() const { return m_lighting->GetValue(); }
    bool synchronize_project_colors(bool activate = false) {
        const bool different = read_project();
        if (!different && !activate) return false;
        if (m_source->GetSelection() != 1) return false;
        auto saved = state();
        if (m_project_colors.empty() || !AI::ColorTrialPersistence::synchronize_project_targets(
                saved, m_project_slots, m_project_colors, m_project_slot_identity)) {
            m_enabled = false;
            m_notice = _L("工程打印机或耗材槽位已变化，请重新确认配色来源。");
            changed(); return true;
        }
        m_colors = std::move(saved.colors);
        m_semantic_colors = std::move(saved.semantic_palette);
        m_count->SetValue(int(m_colors.size()));
        m_notice = _L("工程耗材颜色已同步，原有区域对应关系保留。");
        changed(); return true;
    }
    void activate_for_beauty_semantics() {
        if (m_colors.empty() || m_colors.size() > 6) return;
        m_enabled = true;
        m_semantic->SetValue(true);
        update();
    }
    bool semantic_optimization() const { return m_semantic->GetValue() && m_colors.size() <= 6; }
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
        m_semantic_status->SetLabel(message); m_semantic_status->Show(!m_workbench_mode && !message.empty());
        m_semantic_status->Wrap(std::max(FromDIP(220), GetClientSize().x - FromDIP(12)));
        m_semantic_cancel->Show(!m_workbench_mode && busy && m_detail_buttons.empty());
        sync_workbench_detail_choices(); Layout();
        if (auto* parent = GetParent()) {
            parent->Layout();
            if (auto* scroll = dynamic_cast<wxScrolledWindow*>(parent))
                scroll->FitInside();
            if (auto* grandparent = parent->GetParent()) grandparent->Layout();
        }
    }
    struct State : AI::ColorTrialPersistence::State {
        wxString project_signature, notice;
    };
    State state() const {
        State saved;
        saved.colors = m_colors; saved.mapping_colors = m_mapping_colors;
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) saved.locks[i] = m_locks[i]->GetValue();
        saved.source = m_source->GetSelection(); saved.count = m_count->GetValue();
        saved.enabled = m_enabled; saved.fidelity = m_fidelity->GetValue(); saved.lighting = lighting();
        saved.project_signature = m_project_signature; saved.notice = m_notice;
        saved.semantic_optimization = m_semantic->GetValue();
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
        }
        return saved;
    }
    // Same-workpiece comparison/editing keeps the user's assignments. A new
    // library model still uses load() and starts with its own suggestions.
    void restore(const State& saved) {
        if (!m_histogram || saved.colors.empty() || saved.colors.size() > PreviewPalette::max_preview_colors ||
            saved.colors.size() != saved.mapping_colors.size()) return;
        m_source->SetSelection(saved.source); m_count->SetValue(saved.count);
        m_fidelity->SetValue(saved.fidelity); m_lighting->SetValue(saved.lighting);
        m_colors = saved.colors; m_mapping_colors = saved.mapping_colors;
        m_semantic_colors = saved.semantic_palette; m_semantic_mapping = saved.semantic_mapping_palette;
        m_semantic_card = saved.semantic_portrait_card;
        m_semantic_roles=saved.semantic_role_uids.empty() ? portrait_roles_for_card(
            saved.semantic_palette.empty()?saved.colors:saved.semantic_palette,saved.semantic_portrait_card) : saved.semantic_role_uids;
        m_semantic_region_slots = saved.semantic_region_slots;
        sync_active_palette_state();
        m_semantic->SetValue(saved.semantic_optimization);
        m_enabled = saved.enabled; m_notice = saved.notice;
        m_bound_project_identity = saved.project_slot_identity;
        m_project_color_slots = saved.project_color_slots;
        m_project_semantic_slots = saved.project_semantic_slots;
        for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) m_locks[i]->SetValue(saved.locks[i]);
        if (saved.source == 1) {
            if (saved.project_slot_identity.empty()) m_source->SetSelection(2);
            else if (synchronize_project_colors(true)) return;
        }
        changed();
    }
    std::function<void(bool)> on_semantic_mode;
    std::function<void()> on_semantic_cancel;
    std::function<void()> on_changed;
    std::function<void()> on_region_changed;
    std::function<void()> on_project_colors_changed;
private:
    static wxColour wx_color(Color c) {
        return wxColour(int(std::lround(c[0]*255)), int(std::lround(c[1]*255)), int(std::lround(c[2]*255)));
    }
    // Same source as the native textured import: logical project slots. Existing
    // mixed recipes are excluded, never misreported as physical loaded materials.
    bool read_project() {
        std::vector<Color> colors; std::vector<wxString> names;
        std::vector<size_t> slots;
        wxString signature, error;
        nlohmann::json identity = nlohmann::json::array();
        size_t filament_count = 0;
        auto* bundle = wxGetApp().preset_bundle;
        if (bundle) {
            const auto* values = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            const auto* mixed = bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
            signature = wxString::FromUTF8(bundle->printers.get_edited_preset().name);
            identity.push_back(bundle->printers.get_edited_preset().name);
            for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
                const bool is_mixed = mixed && i < mixed->values.size() && mixed->values[i];
                const std::string hex = values && i < values->values.size() ? values->values[i] : "";
                signature += wxString::Format("|%u:%d:", unsigned(i), int(is_mixed)) + wxString::FromUTF8(hex) + wxString::FromUTF8(bundle->filament_presets[i]);
                if (is_mixed) continue;
                identity.push_back({i, bundle->filament_presets[i]});
                ++filament_count;
                const wxColour color(wxString::FromUTF8(hex));
                if (hex.size() != 7 || hex.front() != '#' || !color.IsOk()) { error = _L("工程存在未配置的耗材颜色，请到准备页补全。"); continue; }
                colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
                slots.push_back(i);
                names.push_back(wxString::Format(_L("工程耗材 %u · "), unsigned(i + 1)) + wxString::FromUTF8(bundle->filament_presets[i]));
            }
        }
        if (filament_count > PreviewPalette::max_preview_colors)
            error = _L("工程实体耗材超过结果对照支持的 32 个可涂色槽位，请先在准备页整理未使用槽位。");
        if (colors.empty() && error.empty()) error = _L("尚未读取到实体耗材，请在准备页配置，或选择手动试色。");
        if (!error.empty()) colors.clear();
        const bool different = signature != m_project_signature;
        m_project_signature = signature; m_project_filament_count = filament_count;
        m_project_colors = std::move(colors); m_project_names = std::move(names); m_project_error = error;
        m_project_slots = std::move(slots);
        m_project_slot_identity = identity.dump();
        return different;
    }
    void recompute(bool enable = true) {
        if (!m_histogram) return;
        const auto started = std::chrono::steady_clock::now();
        if (m_notice == _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。")) m_notice.clear();
        const int source = m_source->GetSelection();
        std::vector<Color> locked;
        for (size_t i = 0; i < m_colors.size(); ++i) if (m_locks[i]->GetValue()) locked.push_back(m_colors[i]);
        if (source == 1) {
            m_semantic_roles.clear();
            m_colors = m_project_colors;
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            const auto card = portrait_card_colors();
            const bool recognized_card = portrait_card_slot_mapping(m_colors, card).enabled;
            if (recognized_card) m_semantic_card = card;
            m_mapping_colors = m_colors;
            if (recognized_card) apply_portrait_mapping();
            m_project_color_slots = m_project_slots;
            m_project_semantic_slots = m_project_slots;
            m_bound_project_identity = m_project_slot_identity;
            if (m_colors != m_project_colors) {
                m_project_color_slots.clear();
                for (const auto& color : m_colors) {
                    const auto found = std::find(m_project_colors.begin(), m_project_colors.end(), color);
                    if (found == m_project_colors.end()) { m_bound_project_identity.clear(); break; }
                    m_project_color_slots.push_back(m_project_slots[size_t(found - m_project_colors.begin())]);
                }
            }
            if (!m_colors.empty()) m_count->SetValue(int(m_colors.size()));
        } else if (source >= 3 && size_t(source - 3) < m_packs.size()) {
            m_semantic_roles.clear();
            m_colors.clear();
            for (const auto& hex : m_packs[source - 3].colors) {
                const wxColour color(wxString::FromUTF8(hex));
                m_colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            }
            m_mapping_colors = m_colors; m_count->SetValue(int(m_colors.size()));
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            if (m_packs[source - 3].name == young_portrait_color_pack().name) m_semantic_card = portrait_card_colors();
            m_notice = _L("色卡仅用于试色；点击“应用 / 保存耗材包”可切换工程耗材。");
            if (m_packs[source - 3].name == young_portrait_color_pack().name) apply_portrait_mapping();
        } else if (source == 0) {
            m_semantic_roles.clear();
            if (locked.size() > size_t(m_count->GetValue())) {
                m_count->SetValue(int(locked.size()));
                m_notice = _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。");
            }
            m_colors = m_histogram->trial_palette(size_t(m_count->GetValue()), locked, m_fidelity->GetValue());
            m_mapping_colors = m_colors;
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            for (size_t i = 0; i < PreviewPalette::max_preview_colors; ++i) m_locks[i]->SetValue(i < m_colors.size() &&
                std::find(locked.begin(), locked.end(), m_colors[i]) != locked.end());
        } else if(!m_semantic_card.empty() && !m_semantic_roles.empty() && m_count->GetValue()>=3 && m_count->GetValue()<=6) {
            const auto old_colors=m_semantic_colors;
            const auto old_roles=m_semantic_roles;
            const auto count=size_t(m_count->GetValue());
            m_colors.assign(m_semantic_card.begin(),m_semantic_card.begin()+count);
            m_mapping_colors=m_colors;m_semantic_mapping=m_colors;m_semantic_colors=m_colors;
            m_semantic_roles.assign(portrait_role_names.begin(),portrait_role_names.begin()+count);
            for(size_t i=0;i<count;++i) {
                const auto previous=std::find(old_roles.begin(),old_roles.end(),m_semantic_roles[i]);
                if(previous!=old_roles.end() && size_t(previous-old_roles.begin())<old_colors.size())
                    m_colors[i]=m_semantic_colors[i]=old_colors[size_t(previous-old_roles.begin())];
            }
        } else {
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
        }
        sync_active_palette_state();
        m_enabled = enable && !m_colors.empty(); changed();
        BOOST_LOG_TRIVIAL(info) << "AI protected color palette: source=" << source << ", colors=" << m_colors.size()
            << ", locks=" << locked.size() << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    }
    void edit_color(size_t i) {
        if (semantic_optimization() && i < semantic_palette().size()) {
            const auto original = semantic_palette();
            wxColourData data; data.SetColour(wx_color(original[i])); data.SetChooseFull(true);
            wxColourDialog dialog(this, &data); dialog.SetTitle(_L("修改区域试色颜色（不改工程耗材）"));
            if (dialog.ShowModal() != wxID_OK) return;
            const wxColour color = dialog.GetColourData().GetColour();
            const Color replacement {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
            m_semantic_mapping = semantic_mapping_palette();
            m_semantic_colors = original; m_semantic_colors[i] = replacement;
            for (auto& target : m_colors) if (target == original[i]) target = replacement;
            sync_active_palette_state();
            m_source->SetSelection(2); m_notice = _L("已保留区域对应关系；工程耗材未修改。");
            m_enabled = true;
            changed(); return;
        }
        if (i >= m_colors.size()) return;
        wxColourData data; data.SetColour(wx_color(m_colors[i])); data.SetChooseFull(true);
        wxColourDialog dialog(this, &data);
        dialog.SetTitle(_L("修改试色颜色（不改工程耗材）"));
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
        m_source->SetSelection(2);
        m_notice = _L("已转为手动试色，保留各色对应范围；工程耗材未修改。");
        m_locks[i]->SetValue(true); recompute();
    }
    void changed() { update(); if (on_changed) on_changed(); }
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
        const int source = m_source->GetSelection();
        // The host owns the portrait mode, progress and cancellation card.
        // Palette details must not expose a second mode/task entry point.
        if (m_workbench_mode) {
            m_semantic->Hide();
            m_semantic_cancel->Hide();
            m_semantic_status->Hide();
        }
        if (m_workbench_source) {
            if (m_workbench_mode) {
                m_workbench_mode->SetSelection(m_host_portrait_mode ? 1 : 0);
                m_workbench_mode->Enable(m_workbench_editable && bool(m_histogram) && m_colors.size() <= 6);
            }
            m_workbench_source->SetSelection(source);
            m_workbench_source->Enable(m_workbench_editable && bool(m_histogram));
            m_workbench_count->SetValue(m_count->GetValue());
            m_workbench_count->Enable(m_workbench_editable && bool(m_histogram) && (source == 0 || source == 2));
            m_workbench_count_label->SetLabel(wxString::Format(_L("%d 色"), m_count->GetValue()));
        }
        const auto& displayed_colors = semantic_optimization() ? semantic_palette() : m_colors;
        const auto& region_palette = semantic_optimization() ? semantic_palette() : m_colors;
        Show(bool(m_histogram));
        m_toggle->Enable(!m_colors.empty());
        m_toggle->SetLabel(m_enabled ? _L("查看原色") : wxString::Format(_L("预览 %u 色"), unsigned(m_colors.size())));
        m_count->Enable(source == 0 || source == 2); m_fidelity->Enable(source == 0);
        m_semantic->Enable(m_colors.size() <= 6);
        m_semantic->SetToolTip(m_colors.size() <= 6
            ? _L("在本机识别皮肤、衣服和嘴唇后分别匹配颜色；识别不明确的区域沿用原有配色。")
            : _L("人像区域优化支持 1 至 6 色；将结果对照调回此范围后恢复。"));
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
            m_swatches[i]->SetMinSize(wxSize(
                std::max(FromDIP(64), m_swatches[i]->GetTextExtent(hex).x + FromDIP(16)), FromDIP(26)));
            m_swatches[i]->SetForegroundColour((color.Red()*299 + color.Green()*587 + color.Blue()*114 > 145000) ? *wxBLACK : *wxWHITE);
            const auto project_color = std::find(m_project_colors.begin(), m_project_colors.end(), displayed_colors[i]);
            const size_t project_index = size_t(std::distance(m_project_colors.begin(), project_color));
            m_swatches[i]->SetToolTip((source == 1 && project_index < m_project_names.size() ? m_project_names[project_index] + " · " : "") +
                (semantic_optimization() ? _L("区域配色候选 · ") : source != 0 && i < m_mapping_colors.size() ? wx_color(m_mapping_colors[i]).GetAsString(wxC2S_HTML_SYNTAX) + " → " : "") + hex + _L(" · 点击改为试色颜色"));
            m_locks[i]->SetToolTip(_L("固定此颜色，重新推荐时不被其他颜色合并。") + hex);
        }
        wxString text = m_enabled ? (lighting()
            ? wxString::Format(_L("立体展示 · %u 种基础色；光照会增加明暗深浅。"), unsigned(m_colors.size()))
            : wxString::Format(_L("纯分色 · %u 种色块，与所选色卡一致，无光照明暗。"), unsigned(m_colors.size())))
            : _L("原色显示 · ");
        text += source == 1 ? _L("使用工程耗材色卡。") : source == 2 ? _L("手动试色。") : source >= 3 ? _L("耗材包试色。") : _L("可点击改色并勾选保留。");
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
                entry.row->Show(!m_workbench_mode);
                entry.original->Hide();
            }
            if (entry.lock_index >= 0) {
                entry.row->Show(m_source->GetSelection() == 0 && m_swatches[entry.lock_index]->IsShown());
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
    bool m_host_portrait_mode {false};
    wxWeakRef<WorkbenchPaletteChoice> m_workbench_mode;
    wxWeakRef<WorkbenchPaletteChoice> m_workbench_source;
    wxWeakRef<wxSlider> m_workbench_count;
    wxWeakRef<wxStaticText> m_workbench_count_label;
    bool m_workbench_editable {false};
    wxButton* m_toggle;
    wxChoice* m_source;
    wxSpinCtrl* m_count;
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
    std::vector<Color> m_colors, m_project_colors, m_mapping_colors;
    std::vector<std::string> m_semantic_roles;
    std::vector<wxString> m_project_names;
    std::vector<size_t> m_project_slots;
    std::vector<size_t> m_project_color_slots, m_project_semantic_slots;
    std::string m_project_slot_identity, m_bound_project_identity;
    wxString m_notice, m_project_error, m_project_signature;
    size_t m_project_filament_count {0}, m_model_suggestion_count {0};
    int m_initial_count {6};
    bool m_enabled {false};
    std::chrono::steady_clock::time_point m_last_check {};
};
} // namespace Slic3r::GUI
