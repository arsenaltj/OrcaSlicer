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
// Trial colors do not modify the project. Real slot edits use the native
// printer-filament configuration dialog.
class ModelPreviewColorControls final : public wxPanel {
public:
    using Color = PreviewPalette::Color;
    void set_workbench_detail_choices(bool enabled) {
        if (enabled == !m_detail_choices.empty()) return;
        if (enabled) {
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
        }
        wrap_status();
        Layout();
    }
    void set_workbench_editable(bool editable) {
        m_workbench_editable = editable;
        if (!m_workbench_source) return;
        m_workbench_source->Enable(editable && bool(m_histogram));
        if (m_workbench_mode) m_workbench_mode->Enable(editable && bool(m_histogram) && portrait_color_count_supported());
        const int source = m_source->GetSelection();
        m_workbench_count->Enable(editable && bool(m_histogram) && (source == 0 || source == 2));
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
        m_source->Append(_L("打印机耗材"));
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
        m_detail_commands = {m_toggle, reset};
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
            if (m_source->GetSelection() == 1 && read_project()) {
                m_notice = _L("打印机耗材已变化，已恢复原色，请重新对照。");
                m_enabled = false; changed(); return;
            }
            if (m_colors.empty()) return;
            m_enabled = !m_enabled; changed();
        });
        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const int previous = m_last_source;
            const int next = m_source->GetSelection();
            remember_source_count(previous);
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear();
            if (next == 1) {
                read_project(); m_enabled = false;
                m_bound_project_identity.clear(); m_project_color_slots.clear(); m_project_semantic_slots.clear();
                m_project_count = std::clamp(int(m_project_colors.size()), 1, int(PreviewPalette::max_preview_colors));
                m_count->SetValue(m_project_count);
                m_notice = _L("请使用工作台的“应用打印机耗材”按钮，保留模型原有分区并预览匹配。");
                m_last_source = next;
                if (on_source_changed) on_source_changed(previous, next);
                changed(); return;
            }
            if (next == 0) m_count->SetValue(m_automatic_count);
            else if (next == 2) m_count->SetValue(m_manual_count);
            else if (next >= 3 && size_t(next - 3) < m_packs.size())
                m_count->SetValue(std::clamp(int(m_packs[next - 3].colors.size()), 1, int(PreviewPalette::max_preview_colors)));
            m_last_source = next;
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
            m_initial_count = int(PreviewPalette::initial_trial_color_count(m_project_filament_count, m_model_suggestion_count));
            m_source->SetSelection(0); m_count->SetValue(m_initial_count);
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
    void load(std::shared_ptr<const PreviewPalette::Histogram> histogram, std::vector<Color> colors,
              std::string geometry_id = {},
              std::shared_ptr<const PreviewPalette::AutomaticColorUnits> automatic_units = {}) {
        m_geometry_id = std::move(geometry_id);
        m_automatic_units = std::move(automatic_units);
        m_histogram = std::move(histogram); m_colors = std::move(colors); m_mapping_colors = m_colors;
        m_suggestion_colors = m_colors; m_manual_colors.clear();
        m_model_suggestion_count = m_colors.size();
        read_project();
        m_initial_count = int(PreviewPalette::initial_trial_color_count(m_project_filament_count, m_model_suggestion_count));
        if (m_histogram && m_colors.size() != size_t(m_initial_count)) {
            m_colors = m_histogram->trial_palette(size_t(m_initial_count), {}, true);
            m_mapping_colors = m_colors;
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
        m_enabled = false; m_source->SetSelection(0); m_count->SetValue(m_initial_count); m_fidelity->SetValue(true);
        m_automatic_count = m_initial_count; m_manual_count = m_initial_count; m_project_count = 0; m_last_source = 0;
        m_lighting->SetValue(false);
        m_notice.clear();
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
        m_enabled = false; Hide();
    }
    const std::vector<Color>& colors() const { return m_colors; }
    int source() const { return m_source ? m_source->GetSelection() : 0; }
    bool color_card_source_active() const {
        // Every saved color card participates in region-preserving remapping;
        // the portrait card only adds role labels and is not the sole source
        // eligible for this path.
        return source() >= 3;
    }
    int automatic_count() const { return m_automatic_count; }
    void refresh_saved_color_packs() {
        // Keep existing source indices stable for history and 3MF state.
        const int selected = m_source->GetSelection();
        for (auto& pack : load_filament_color_packs()) {
            const auto found = std::find_if(m_packs.begin(), m_packs.end(),
                [&](const FilamentColorPack& cached) { return cached.name == pack.name; });
            if (found != m_packs.end()) *found = std::move(pack);
            else {
                m_source->Append(wxString::FromUTF8(pack.name));
                if (m_workbench_source) m_workbench_source->Append(wxString::FromUTF8(pack.name));
                m_packs.push_back(std::move(pack));
            }
        }
        m_source->SetSelection(selected);
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
        if (!m_suggestion_colors.empty()) result.push_back(make(u8"自动建议", m_suggestion_colors));
        const auto manual = manual_import_palette(state());
        if (!manual.empty()) result.push_back(make(u8"手动试色", manual));
        const auto saved = load_filament_color_packs();
        result.insert(result.end(), saved.begin(), saved.end());
        return result;
    }
    const std::vector<Color>& mapping_colors() const { return m_mapping_colors; }
    const PreviewPalette::AutomaticColorBasis& automatic_color_basis() const { return m_automatic_basis; }
    std::shared_ptr<const PreviewPalette::AutomaticColorUnits> automatic_color_units() const { return m_automatic_units; }
    const PreviewPalette::ColorCardRemap& color_card_remap() const { return m_color_card_remap; }
    const std::string& automatic_basis_signature() const { return m_automatic_basis.basis_signature; }
    bool enabled() const { return m_enabled; }
    bool lighting() const { return m_lighting->GetValue(); }
    bool synchronize_project_colors(bool activate = false) {
        const bool different = read_project();
        if (!different && !activate) return false;
        if (m_source->GetSelection() != 1) return false;
        auto saved = state();
        if (!m_project_error.empty() || m_project_colors.empty() || !AI::ColorTrialPersistence::synchronize_project_targets(
                saved, m_project_slots, m_project_colors, m_project_slot_identity)) {
            m_enabled = false;
            m_notice = _L("打印机或耗材槽位已变化，请重新应用打印机耗材。");
            changed(); return true;
        }
        m_colors = std::move(saved.colors);
        m_semantic_colors = std::move(saved.semantic_palette);
        m_count->SetValue(int(m_colors.size()));
        m_notice = _L("打印机耗材已同步，原有区域对应关系保留。");
        changed(); return true;
    }
    const std::vector<Color>& project_colors() const { return m_project_colors; }
    bool project_matching_valid() const {
        return m_source->GetSelection() != 1 || (m_enabled && m_project_error.empty() &&
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
    void activate_for_beauty_semantics() {
        m_semantic->SetValue(false);
        update();
    }
    bool semantic_optimization() const { return false; }
    bool color_block_postprocess_enabled() const { return false; }
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
        saved.source = m_source->GetSelection(); saved.count = m_count->GetValue();
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
        saved.manual_colors = m_manual_colors;
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
        }
        return saved;
    }
    // Same-workpiece comparison/editing keeps the user's assignments. A new
    // library model still uses load() and starts with its own suggestions.
    void restore(const State& saved) {
        if (!m_histogram || saved.colors.empty() || saved.colors.size() > PreviewPalette::max_preview_colors ||
            saved.colors.size() != saved.mapping_colors.size()) return;
        m_source->SetSelection(saved.source); m_last_source = saved.source;
        m_automatic_count = std::clamp(saved.automatic_count, 1, int(PreviewPalette::max_preview_colors));
        m_manual_count = std::clamp(saved.manual_count, 1, int(PreviewPalette::max_preview_colors));
        m_project_count = std::clamp(saved.project_count, 0, int(PreviewPalette::max_preview_colors));
        m_count->SetValue(saved.source == 0 ? m_automatic_count : saved.source == 2 ? m_manual_count : saved.count);
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
    // Same source as the native textured import: logical project slots. Existing
    // mixed recipes are excluded, never misreported as physical loaded materials.
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
        m_project_signature = signature; m_project_filament_count = snapshot.physical_count;
        m_project_colors = std::move(colors); m_project_names = std::move(names);
        m_project_error = snapshot.ready ? wxString{} : wxString::FromUTF8(snapshot.reason.empty() ? "请配置打印机耗材" : snapshot.reason);
        m_project_slots = std::move(slots); m_project_slot_identity = snapshot.identity;
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
            m_notice = wxString::Format(_L("已保留 %llu 个自动建议源区域，并与色卡槽位做全局一对一匹配。"),
                static_cast<unsigned long long>(m_mapping_colors.size()));
        } else if (source == 0) {
            m_color_card_remap = {};
            m_color_card_palette.clear();
            m_semantic_roles.clear();
            if (locked.size() > size_t(m_count->GetValue())) {
                m_count->SetValue(int(locked.size()));
                m_notice = _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。");
            }
            m_colors = m_histogram->trial_palette(size_t(m_count->GetValue()), locked, m_fidelity->GetValue());
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
    void edit_color(size_t i) {
        // In printer-filament mode a swatch represents a real slot. Open the
        // native draft configuration instead of silently creating a trial color.
        if (m_source->GetSelection() == 1) {
            if (on_project_color_edit && i < m_project_slots.size()) on_project_color_edit(m_project_slots[i]);
            return;
        }
        if (m_color_card_remap.valid(m_mapping_colors.size()) && i < m_color_card_palette.size()) {
            const auto source = std::find(m_color_card_remap.source_to_target_slot.begin(),
                m_color_card_remap.source_to_target_slot.end(), i);
            if (source == m_color_card_remap.source_to_target_slot.end()) return;
            wxColourData data; data.SetColour(wx_color(m_color_card_palette[i])); data.SetChooseFull(true);
            wxColourDialog dialog(this, &data);
            dialog.SetTitle(_L("修改色卡试色颜色（保留自动建议分区）"));
            if (dialog.ShowModal() != wxID_OK) return;
            const wxColour color = dialog.GetColourData().GetColour();
            const Color replacement {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
            m_color_card_palette[i] = replacement;
            m_color_card_remap.target_palette[i] = replacement;
            m_colors[size_t(source - m_color_card_remap.source_to_target_slot.begin())] = replacement;
            m_source->SetSelection(2);
            m_last_source = 2;
            m_manual_count = std::clamp(m_count->GetValue(), 1, int(PreviewPalette::max_preview_colors));
            m_notice = _L("已转为手动试色；自动建议源区域和一对一槽位身份保持不变。");
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
            m_source->SetSelection(2); m_notice = _L("已保留区域对应关系；打印机耗材未修改。");
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
        m_source->SetSelection(2);
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
        const int source = m_source->GetSelection();
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
            m_workbench_source->SetSelection(source);
            m_workbench_source->Enable(m_workbench_editable && bool(m_histogram));
            m_workbench_count->SetValue(m_count->GetValue());
            m_workbench_count->Enable(m_workbench_editable && bool(m_histogram) && (source == 0 || source == 2));
            m_workbench_count_label->SetLabel(source == 1 ? wxString::Format(_L("%u 个打印机耗材"), unsigned(m_project_colors.size())) : wxString::Format(_L("%d 色"), m_count->GetValue()));
        }
        const bool show_card_slots = source != 1 && m_color_card_remap.valid(m_mapping_colors.size()) &&
            !m_color_card_palette.empty();
        const auto& displayed_colors = source == 1 ? m_project_colors : show_card_slots ? m_color_card_palette :
            semantic_optimization() ? semantic_palette() : m_colors;
        const auto& region_palette = semantic_optimization() ? semantic_palette() : m_colors;
        Show(bool(m_histogram));
        m_toggle->Enable(source != 1 && !m_colors.empty());
        m_toggle->SetLabel(m_enabled ? _L("查看原色") : wxString::Format(_L("预览 %u 色"), unsigned(m_colors.size())));
        m_count->Enable(source == 0 || source == 2); m_fidelity->Enable(source == 0);
        m_semantic->Enable(portrait_color_count_supported());
        m_semantic->SetToolTip(_L("自动建议为四至六色时，按原始颜色建立区域并绑定人像语义；其他色数使用普通配色。"));
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
            const auto mapped_source = show_card_slots ? std::find(m_color_card_remap.source_to_target_slot.begin(),
                m_color_card_remap.source_to_target_slot.end(), i) : m_color_card_remap.source_to_target_slot.end();
            const bool card_slot_used = !show_card_slots || mapped_source != m_color_card_remap.source_to_target_slot.end();
            m_swatches[i]->Enable(source == 1 || card_slot_used);
            // Preserve slot identity when two physical slots have the same RGB.
            const size_t project_index = i;
            m_swatches[i]->SetToolTip((source == 1 && project_index < m_project_names.size() ? m_project_names[project_index] + " · " : "") +
                hex + (source == 1 ? _L(" · 点击配置真实耗材") : !card_slot_used ? _L(" · 当前模型未使用") :
                    show_card_slots ? _L(" · 已一对一映射到自动建议源槽，点击修改显示色") : _L(" · 点击改为试色颜色")));
            m_locks[i]->SetToolTip(_L("固定此颜色，重新推荐时不被其他颜色合并。") + hex);
        }
        wxString text = m_enabled ? (lighting()
            ? wxString::Format(_L("立体展示 · %u 个自动建议区域；光照会增加明暗深浅。"), unsigned(m_mapping_colors.size()))
            : wxString::Format(_L("纯分色 · %u 个自动建议区域；色卡只替换显示颜色。"), unsigned(m_mapping_colors.size())))
            : _L("原色显示 · ");
        text += source == 1 ? _L("使用打印机耗材色卡。") : source == 2 ? _L("手动试色。") : source >= 3 ? _L("耗材包试色。") : _L("可点击改色并勾选保留。");
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
    std::shared_ptr<const PreviewPalette::AutomaticColorUnits> m_automatic_units;
    std::vector<Color> m_colors, m_project_colors, m_mapping_colors;
    PreviewPalette::AutomaticColorBasis m_automatic_basis;
    PreviewPalette::ColorCardRemap m_color_card_remap;
    std::vector<Color> m_color_card_palette;
    std::string m_geometry_id;
    std::vector<Color> m_suggestion_colors, m_manual_colors;
    int m_automatic_count {6}, m_manual_count {6}, m_project_count {0};
    int m_last_source {0};
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
