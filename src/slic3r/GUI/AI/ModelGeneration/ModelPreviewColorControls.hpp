#pragma once

#include "ModelPreviewPalette.hpp"
#include "../Model/ColorTrialState.hpp"
#include "PortraitColorPackMapping.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticPaletteMapping.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/colordlg.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <chrono>
#include <functional>
#include <memory>
#include <map>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
// Trial colors do not modify the project. The separate color-pack action
// explicitly applies a physical-slot card through the Orca adapter.
class ModelPreviewColorControls final : public wxPanel {
public:
    using Color = PreviewPalette::Color;
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
        m_count = new wxSpinCtrl(this, wxID_ANY, "6", wxDefaultPosition, wxSize(FromDIP(60), -1), wxSP_ARROW_KEYS, 1, 6, 6);
        m_count->SetName(_L("试色数量，最多六色"));
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
        m_semantic->SetValue(true);
        m_semantic->SetToolTip(_L("在本机识别皮肤、衣服和嘴唇后分别匹配颜色；识别不明确的区域沿用原有配色。"));
        semantic_row->Add(m_semantic, 1, wxALIGN_CENTER_VERTICAL);
        m_semantic_cancel = new wxButton(this, wxID_ANY, _L("取消识别"));
        semantic_row->Add(m_semantic_cancel, 0);
        m_semantic_cancel->Hide();
        box->Add(semantic_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
        m_semantic_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        box->Add(m_semantic_status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
        m_semantic_status->Hide();
        m_semantic->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            if (m_source->GetSelection() == 0 && !m_material_centers.empty()) recompute(m_enabled);
            else changed();
        });
        m_semantic_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_semantic->SetValue(false); changed(); });
        auto* pack_button = new wxButton(this, wxID_ANY, _L("应用 / 保存耗材包…"));
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
        auto* swatches = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < 6; ++i) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            m_swatches[i] = new wxButton(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(64), FromDIP(26)), wxBU_EXACTFIT);
            m_swatches[i]->SetName("ai_content_color");
            m_locks[i] = new wxCheckBox(this, wxID_ANY, _L("保留"));
            m_active[i] = new wxCheckBox(this, wxID_ANY, _L("启用"));
            m_active[i]->SetValue(true);
            col->Add(m_swatches[i], 0, wxEXPAND);
            col->Add(m_active[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(3));
            col->Add(m_locks[i], 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(3));
            swatches->Add(col, 1, wxRIGHT, FromDIP(4));
            m_swatches[i]->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { edit_color(i); });
            m_locks[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
            m_active[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        }
        box->Add(swatches, 0, wxEXPAND | wxALL, FromDIP(6));
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
        m_count->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { recompute(); });
        m_fidelity->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { recompute(); });
        m_lighting->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { changed(); });
        reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_source->SetSelection(0); m_count->SetValue(6); m_fidelity->SetValue(true);
            m_semantic->SetValue(true);
            m_lighting->SetValue(false);
            for (auto* lock : m_locks) lock->SetValue(false);
            m_notice.clear(); recompute();
        });
        Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
            const auto now = std::chrono::steady_clock::now();
            if (IsShownOnScreen() && m_histogram && m_source->GetSelection() == 1 &&
                now - m_last_check > std::chrono::seconds(1)) {
                m_last_check = now;
                if (read_project()) {
                    m_enabled = false;
                    m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                    recompute(false);
                }
            }
            event.Skip();
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { wrap_status(); event.Skip(); });
    }
    void load(std::shared_ptr<const PreviewPalette::Histogram> histogram, std::vector<Color> colors) {
        m_histogram = std::move(histogram); m_colors = std::move(colors); m_mapping_colors = m_colors;
        m_semantic_colors.clear(); m_semantic_mapping.clear(); m_semantic_card.clear(); m_semantic->SetValue(true);
        set_semantic_status(wxEmptyString, false);
        m_enabled = false; m_source->SetSelection(0); m_count->SetValue(6); m_fidelity->SetValue(true);
        m_lighting->SetValue(false);
        m_notice.clear();
        for (auto* lock : m_locks) lock->SetValue(false);
        for (auto* active : m_active) active->SetValue(true);
        m_slot_ids.clear(); m_legacy_slot_ids.clear(); m_dormant_slots.clear();
        m_material_centers.clear(); m_material_signature.clear(); m_palette_source = 0;
        update();
    }
    void clear() { m_histogram.reset(); m_colors.clear(); m_mapping_colors.clear(); m_enabled = false; Hide(); }
    const std::vector<Color>& colors() const { return m_render_colors; }
    const std::vector<Color>& mapping_colors() const { return m_render_mapping; }
    bool enabled() const { return m_enabled && !m_render_colors.empty(); }
    bool lighting() const { return m_lighting->GetValue(); }
    bool semantic_optimization() const { return m_semantic->GetValue(); }
    const std::vector<Color>& semantic_palette() const { return m_semantic_colors.empty() ? m_colors : m_semantic_colors; }
    const std::vector<Color>& semantic_mapping_palette() const { return m_semantic_mapping.empty() ? semantic_palette() : m_semantic_mapping; }
    const std::vector<Color>& semantic_portrait_card() const { return m_semantic_card; }
    std::vector<AI::SemanticColoring::PaletteSlot> semantic_slots(bool source_colors = false) const {
        const auto& palette = source_colors ? semantic_mapping_palette() : semantic_palette();
        std::vector<AI::SemanticColoring::PaletteSlot> result;
        for (size_t i = 0; i < palette.size(); ++i)
            result.push_back({i < m_slot_ids.size() ? m_slot_ids[i] : "slot-" + std::to_string(i + 1),
                              palette[i], m_active[i]->GetValue()});
        return result;
    }
    std::vector<AI::SemanticColoring::PaletteSlot> baseline_source_slots() const {
        std::vector<AI::SemanticColoring::PaletteSlot> result;
        for (size_t i=0;i<m_mapping_colors.size() && i<m_legacy_slot_ids.size();++i) {
            const auto found=std::find(m_slot_ids.begin(),m_slot_ids.end(),m_legacy_slot_ids[i]);
            if (found==m_slot_ids.end()) continue;
            const size_t index=size_t(found-m_slot_ids.begin());
            result.push_back({m_legacy_slot_ids[i],m_mapping_colors[i],m_active[index]->GetValue()});
        }
        return result.empty() ? semantic_slots(true) : result;
    }
    // Returns true only when the new evidence changed automatic candidates.
    // Hosts then discard the old mapping result and await its replacement.
    bool set_material_centers(const std::vector<AI::SemanticColoring::MaterialCenter>& centers,
                              const std::string& signature) {
        if (signature == m_material_signature) return false;
        m_material_signature = signature; m_material_centers = centers;
        if (m_source->GetSelection() != 0 || !semantic_optimization() || centers.empty()) return false;
        const auto before_colors = semantic_palette(); const auto before_ids = m_slot_ids;
        recompute(m_enabled);
        return before_colors != semantic_palette() || before_ids != m_slot_ids;
    }
    void set_semantic_status(const wxString& message, bool busy) {
        if (m_semantic_status->GetLabel() == message && m_semantic_cancel->IsShown() == busy) return;
        m_semantic_status->SetLabel(message); m_semantic_status->Show(!message.empty());
        m_semantic_status->Wrap(std::max(FromDIP(220), GetClientSize().x - FromDIP(12)));
        m_semantic_cancel->Show(busy); Layout(); GetParent()->Layout();
    }
    struct State : AI::ColorTrialPersistence::State {
        wxString project_signature, notice;
    };
    State state() const {
        State saved;
        saved.colors = m_colors; saved.mapping_colors = m_mapping_colors;
        for (size_t i = 0; i < 6; ++i) saved.locks[i] = m_locks[i]->GetValue();
        saved.source = m_source->GetSelection(); saved.count = m_count->GetValue();
        saved.enabled = m_enabled; saved.fidelity = m_fidelity->GetValue(); saved.lighting = lighting();
        saved.project_signature = m_project_signature; saved.notice = m_notice;
        saved.semantic_optimization = semantic_optimization();
        saved.semantic_palette = semantic_palette(); saved.semantic_mapping_palette = semantic_mapping_palette();
        saved.semantic_portrait_card = m_semantic_card;
        saved.slot_ids = m_slot_ids;
        saved.legacy_slot_ids = m_legacy_slot_ids; saved.dormant_slots = m_dormant_slots;
        for (size_t i = 0; i < semantic_palette().size(); ++i) saved.slot_enabled.push_back(m_active[i]->GetValue());
        return saved;
    }
    // Same-workpiece comparison/editing keeps the user's assignments. A new
    // library model still uses load() and starts with its own suggestions.
    void restore(const State& saved) {
        if (!m_histogram || saved.colors.empty() || saved.colors.size() > 6 ||
            saved.colors.size() != saved.mapping_colors.size()) return;
        m_source->SetSelection(saved.source); m_count->SetValue(saved.count);
        m_fidelity->SetValue(saved.fidelity); m_lighting->SetValue(saved.lighting);
        m_colors = saved.colors; m_mapping_colors = saved.mapping_colors;
        m_semantic_colors = saved.semantic_palette; m_semantic_mapping = saved.semantic_mapping_palette;
        m_semantic_card = saved.semantic_portrait_card;
        m_semantic->SetValue(saved.semantic_optimization);
        m_slot_ids = saved.slot_ids; m_legacy_slot_ids = saved.legacy_slot_ids;
        m_dormant_slots = saved.dormant_slots; m_palette_source = saved.source;
        for (size_t i = 0; i < 6; ++i)
            m_active[i]->SetValue(i >= saved.slot_enabled.size() || saved.slot_enabled[i]);
        m_enabled = saved.enabled; m_notice = saved.notice;
        for (size_t i = 0; i < 6; ++i) m_locks[i]->SetValue(saved.locks[i]);
        if (saved.source == 1) {
            m_project_signature = saved.project_signature;
            if (read_project()) {
                m_notice = _L("工程耗材已变化，已恢复原色，请重新对照。");
                recompute(false); return;
            }
        }
        changed();
    }
    std::function<void()> on_changed;
private:
    static wxColour wx_color(Color c) {
        return wxColour(int(std::lround(c[0]*255)), int(std::lround(c[1]*255)), int(std::lround(c[2]*255)));
    }
    // Same source as the native textured import: logical project slots. Existing
    // mixed recipes are excluded, never misreported as physical loaded materials.
    bool read_project() {
        std::vector<Color> colors; std::vector<wxString> names; std::vector<std::string> ids;
        wxString signature, error;
        auto* bundle = wxGetApp().preset_bundle;
        if (bundle) {
            const auto* values = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            const auto* mixed = bundle->project_config.option<ConfigOptionBools>("filament_is_mixed");
            signature = wxString::FromUTF8(bundle->printers.get_edited_preset().name);
            std::map<std::string, size_t> preset_occurrences;
            for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
                const bool is_mixed = mixed && i < mixed->values.size() && mixed->values[i];
                const std::string hex = values && i < values->values.size() ? values->values[i] : "";
                signature += wxString::Format("|%u:%d:", unsigned(i), int(is_mixed)) + wxString::FromUTF8(hex) + wxString::FromUTF8(bundle->filament_presets[i]);
                if (is_mixed) continue;
                const wxColour color(wxString::FromUTF8(hex));
                if (hex.size() != 7 || hex.front() != '#' || !color.IsOk()) { error = _L("工程存在未配置的耗材颜色，请到准备页补全。"); continue; }
                colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
                const std::string preset_identity = bundle->filament_presets[i];
                const size_t occurrence = preset_occurrences[preset_identity]++;
                ids.push_back(AI::ColorTrialPersistence::stable_slot_uid("project-filament", preset_identity, occurrence));
                names.push_back(wxString::Format(_L("工程耗材 %u · "), unsigned(i + 1)) + wxString::FromUTF8(bundle->filament_presets[i]));
            }
        }
        if (colors.size() > 6) error = _L("工程有超过六种实体耗材，请在准备页选定最多六色后再对照。");
        if (colors.empty() && error.empty()) error = _L("尚未读取到实体耗材，请在准备页配置，或选择手动试色。");
        if (!error.empty()) { colors.clear(); ids.clear(); }
        const bool different = signature != m_project_signature;
        m_project_signature = signature; m_project_colors = std::move(colors); m_project_slot_ids = std::move(ids); m_project_names = std::move(names); m_project_error = error;
        return different;
    }
    void recompute(bool enable = true) {
        if (!m_histogram) return;
        const auto started = std::chrono::steady_clock::now();
        if (m_notice == _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。")) m_notice.clear();
        const int source = m_source->GetSelection();
        std::map<std::string, bool> previous_enabled;
        for (size_t i = 0; i < m_slot_ids.size(); ++i) previous_enabled[m_slot_ids[i]] = m_active[i]->GetValue();
        if (source != m_palette_source) {
            m_slot_ids.clear(); m_legacy_slot_ids.clear(); m_dormant_slots.clear();
            for (auto* active : m_active) active->SetValue(true);
        }
        m_palette_source = source;
        std::vector<Color> locked;
        std::vector<AI::SemanticColoring::PaletteSlot> locked_slots;
        for (size_t i = 0; i < semantic_palette().size(); ++i)
            if (m_locks[i]->GetValue() && i < m_slot_ids.size())
                locked_slots.push_back({m_slot_ids[i], semantic_palette()[i], true});
        for (size_t i = 0; i < m_colors.size(); ++i) if (m_locks[i]->GetValue()) locked.push_back(m_colors[i]);
        if (source == 1) {
            m_colors = m_project_colors; m_slot_ids = m_project_slot_ids;
            m_legacy_slot_ids = m_slot_ids;
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            if (is_portrait_card(m_colors)) m_semantic_card = canonical_portrait_card();
            m_mapping_colors = m_colors;
            if (is_portrait_card(m_colors)) apply_portrait_mapping();
            if (!m_colors.empty()) m_count->SetValue(int(m_colors.size()));
        } else if (source >= 3 && size_t(source - 3) < m_packs.size()) {
            m_colors.clear();
            for (const auto& hex : m_packs[source - 3].colors) {
                const wxColour color(wxString::FromUTF8(hex));
                m_colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
            }
            m_slot_ids.clear();
            for (size_t i = 0; i < m_colors.size(); ++i)
                m_slot_ids.push_back("pack:" + m_packs[source - 3].name + ":" + std::to_string(i));
            m_legacy_slot_ids = m_slot_ids;
            m_mapping_colors = m_colors; m_count->SetValue(int(m_colors.size()));
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            if (m_packs[source - 3].name == young_portrait_color_pack().name) m_semantic_card = m_colors;
            m_notice = _L("色卡仅用于试色；点击“应用 / 保存耗材包”可切换工程耗材。");
            if (m_packs[source - 3].name == young_portrait_color_pack().name) apply_portrait_mapping();
        } else if (source == 0) {
            if (locked.size() > size_t(m_count->GetValue())) {
                m_count->SetValue(int(locked.size()));
                m_notice = _L("已保留锁定颜色；如需更少颜色，请先取消部分保留。");
            }
            std::vector<AI::SemanticColoring::PaletteSlot> suggested;
            std::string suggestion_error;
            const bool material_suggestions = semantic_optimization() && !m_material_centers.empty() &&
                AI::SemanticColoring::suggest_material_slots(m_material_centers, size_t(m_count->GetValue()),
                                                             locked_slots, suggested, suggestion_error);
            m_slot_ids.clear();
            if (material_suggestions) {
                m_colors.clear();
                for (const auto& slot : suggested) { m_colors.push_back(slot.color); m_slot_ids.push_back(slot.id); }
            } else {
                m_colors = m_histogram->palette(size_t(m_count->GetValue()), locked, m_fidelity->GetValue());
                for (size_t i = 0; i < m_colors.size(); ++i) m_slot_ids.push_back("auto-fallback-" + std::to_string(i));
                if (!suggestion_error.empty()) m_notice = _L("区域材质建议暂不可用，已保留原色分组建议。");
            }
            m_mapping_colors = m_colors; m_legacy_slot_ids = m_slot_ids;
            m_semantic_colors = m_colors; m_semantic_mapping = m_colors; m_semantic_card.clear();
            for (size_t i = 0; i < 6; ++i) m_locks[i]->SetValue(i < m_colors.size() &&
                (material_suggestions ? std::any_of(locked_slots.begin(), locked_slots.end(), [&](const auto& slot) {
                    return slot.id == m_slot_ids[i];
                }) : std::find(locked.begin(), locked.end(), m_colors[i]) != locked.end()));
        } else {
            // Manual count changes hide/restore the same paired source/target
            // slots; adding a slot never reconstructs a previously hidden one.
            auto manual = state(); manual.source = 2;
            AI::ColorTrialPersistence::resize_manual_slots(manual, size_t(m_count->GetValue()),
                                                           m_histogram->palette(6, {}, true));
            m_colors = manual.colors; m_mapping_colors = manual.mapping_colors;
            m_semantic_colors = manual.semantic_palette; m_semantic_mapping = manual.semantic_mapping_palette;
            m_slot_ids = manual.slot_ids; m_legacy_slot_ids = manual.legacy_slot_ids;
            m_dormant_slots = manual.dormant_slots;
            for (size_t i = 0; i < manual.slot_enabled.size(); ++i) m_active[i]->SetValue(manual.slot_enabled[i]);
        }
        if (source != 2) for (size_t i = 0; i < m_slot_ids.size(); ++i) {
            const auto previous = previous_enabled.find(m_slot_ids[i]);
            m_active[i]->SetValue(previous == previous_enabled.end() || previous->second);
        }
        m_enabled = enable && !m_colors.empty(); changed();
        BOOST_LOG_TRIVIAL(info) << "AI protected color palette: source=" << source << ", colors=" << m_colors.size()
            << ", locks=" << locked.size() << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
    }
    void edit_color(size_t i) {
        const auto& displayed = semantic_optimization() ? semantic_palette() : m_colors;
        if (i >= displayed.size()) return;
        wxColourData data; data.SetColour(wx_color(displayed[i])); data.SetChooseFull(true);
        wxColourDialog dialog(this, &data); dialog.SetTitle(_L("修改区域试色颜色（不改工程耗材）"));
        if (dialog.ShowModal() != wxID_OK) return;
        const wxColour color = dialog.GetColourData().GetColour();
        auto edited = state();
        const std::string id = semantic_optimization() && i < m_slot_ids.size() ? m_slot_ids[i] :
            i < m_legacy_slot_ids.size() ? m_legacy_slot_ids[i] : std::string();
        if (!AI::ColorTrialPersistence::edit_slot_color(edited, id,
                {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f})) return;
        edited.notice = _L("已保留该耗材对应区域；同色的其他耗材不变，工程耗材未修改。");
        AI::ColorTrialPersistence::remember_manual_slots(edited);
        restore(edited);
    }
    void changed() { update(); if (on_changed) on_changed(); }
    static std::vector<Color> canonical_portrait_card() {
        std::vector<Color> colors;
        for (const auto& hex : young_portrait_color_pack().colors) {
            const wxColour color(wxString::FromUTF8(hex));
            colors.push_back({color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f});
        }
        return colors;
    }
    static bool is_portrait_card(const std::vector<Color>& colors) {
        return PreviewPalette::matches_portrait_card_set(colors, canonical_portrait_card());
    }
    void apply_portrait_mapping() {
        const auto mapping = PreviewPalette::portrait_pack_mapping(m_histogram->palette(6, {}, true), m_semantic_card);
        if (!mapping.enabled) return;
        m_mapping_colors = mapping.mapping_colors; m_colors = mapping.target_colors;
        m_legacy_slot_ids.clear();
        for (const auto& target : m_colors) {
            const auto found = std::find(m_semantic_colors.begin(), m_semantic_colors.end(), target);
            const size_t index = size_t(found - m_semantic_colors.begin());
            m_legacy_slot_ids.push_back(index < m_slot_ids.size() ? m_slot_ids[index] : std::string());
        }
        m_count->SetValue(int(m_colors.size()));
        m_notice = _L("已按人物色卡建议配色，可通过人像区域优化进一步调整；局部修改请到 3D 美颜。");
    }
    void wrap_status() { m_status->Wrap(std::max(FromDIP(220), GetClientSize().x - FromDIP(12))); }
    void update() {
        const int source = m_source->GetSelection();
        const auto& full_palette = semantic_palette();
        m_slot_ids.resize(full_palette.size());
        for (size_t i = 0; i < m_slot_ids.size(); ++i)
            if (m_slot_ids[i].empty()) m_slot_ids[i] = source == 0 ?
                AI::ColorTrialPersistence::stable_slot_uid("auto-fallback", m_material_signature, i) :
                AI::ColorTrialPersistence::new_slot_uid();
        if (m_legacy_slot_ids.size() != m_colors.size() && m_colors == full_palette)
            m_legacy_slot_ids = m_slot_ids;
        const bool all_active = std::all_of(m_active.begin(), m_active.begin() + full_palette.size(),
            [](const wxCheckBox* active) { return active->GetValue(); });
        m_render_colors = m_colors; m_render_mapping = m_mapping_colors;
        if (!all_active) {
            m_render_colors.clear(); m_render_mapping.clear();
            for (size_t i = 0; i < full_palette.size(); ++i) if (m_active[i]->GetValue()) {
                m_render_colors.push_back(full_palette[i]);
                m_render_mapping.push_back(semantic_mapping_palette()[i]);
            }
        }
        const auto& displayed_colors = semantic_optimization() ? semantic_palette() : m_colors;
        Show(bool(m_histogram));
        m_toggle->Enable(!m_colors.empty());
        m_toggle->SetLabel(m_enabled ? _L("查看原色") : wxString::Format(_L("预览 %u 色"), unsigned(m_colors.size())));
        m_count->Enable(source == 0 || source == 2);
        m_fidelity->Enable(source == 0 && (!semantic_optimization() || m_material_centers.empty()));
        m_lighting->Enable(m_enabled);
        for (size_t i = 0; i < 6; ++i) {
            const bool visible = i < displayed_colors.size();
            m_swatches[i]->Show(visible); m_locks[i]->Show(visible && source == 0);
            m_active[i]->Show(visible);
            if (!visible) continue;
            const wxColour color = wx_color(displayed_colors[i]);
            const wxString hex = color.GetAsString(wxC2S_HTML_SYNTAX);
            m_swatches[i]->SetLabel(hex); m_swatches[i]->SetBackgroundColour(color);
            m_swatches[i]->SetForegroundColour((color.Red()*299 + color.Green()*587 + color.Blue()*114 > 145000) ? *wxBLACK : *wxWHITE);
            const auto project_color = std::find(m_project_colors.begin(), m_project_colors.end(), displayed_colors[i]);
            const size_t project_index = size_t(std::distance(m_project_colors.begin(), project_color));
            m_swatches[i]->SetToolTip((source == 1 && project_index < m_project_names.size() ? m_project_names[project_index] + " · " : "") +
                (semantic_optimization() ? _L("区域配色候选 · ") : source != 0 && i < m_mapping_colors.size() ? wx_color(m_mapping_colors[i]).GetAsString(wxC2S_HTML_SYNTAX) + " → " : "") + hex + _L(" · 点击改为试色颜色"));
            m_locks[i]->SetToolTip(_L("固定此颜色，重新推荐时不被其他颜色合并。") + hex);
        }
        wxString text = m_enabled ? (lighting()
            ? wxString::Format(_L("立体展示 · 最多 %u 种基础色；光照会增加明暗深浅。"), unsigned(m_colors.size()))
            : wxString::Format(_L("纯分色 · 最多 %u 种色块，与所选色卡一致，无光照明暗。"), unsigned(m_colors.size())))
            : _L("原色显示 · ");
        text += source == 1 ? _L("使用工程耗材色卡。") : source == 2 ? _L("手动试色。") : source >= 3 ? _L("耗材包试色。") : _L("可点击改色并勾选保留。");
        text += _L("仅供配色对照，不代表实际打印效果；");
        text += m_enabled ? _L("导入时可沿用当前试色，或从模型原色重新配色。") : _L("原色导入时可重新选择目标颜色数量，再匹配实际耗材。");
        if (source == 1 && !m_project_error.empty()) text += "\n" + m_project_error;
        if (!m_notice.empty()) text += "\n" + m_notice;
        size_t enabled_slots = 0;
        for (size_t i = 0; i < full_palette.size(); ++i) enabled_slots += m_active[i]->GetValue();
        text += wxString::Format(_L("\n启用耗材 %u / %u；停用后仍保留区域与原色信息。"),
            unsigned(enabled_slots), unsigned(full_palette.size()));
        if (enabled_slots == 0) text += _L(" 请至少启用一个耗材；当前显示原色。");
        m_status->SetLabel(text); wrap_status(); Layout();
        // Controls must not consume the model viewport's existing minimum height.
        GetParent()->SetMinSize(wxSize(FromDIP(420), FromDIP(300) + GetSizer()->CalcMin().y));
        GetParent()->Layout();
    }
    std::vector<FilamentColorPack> m_packs;
    wxButton* m_toggle;
    wxChoice* m_source;
    wxSpinCtrl* m_count;
    wxCheckBox* m_fidelity;
    wxCheckBox* m_lighting;
    wxCheckBox* m_semantic;
    wxButton* m_semantic_cancel;
    wxStaticText* m_semantic_status;
    std::vector<Color> m_semantic_colors, m_semantic_mapping, m_semantic_card;
    wxStaticText* m_status;
    std::array<wxButton*, 6> m_swatches {};
    std::array<wxCheckBox*, 6> m_locks {};
    std::array<wxCheckBox*, 6> m_active {};
    std::vector<std::string> m_slot_ids, m_legacy_slot_ids, m_project_slot_ids;
    std::vector<AI::ColorTrialPersistence::State::StoredSlot> m_dormant_slots;
    std::vector<AI::SemanticColoring::MaterialCenter> m_material_centers;
    std::string m_material_signature;
    int m_palette_source {0};
    std::vector<Color> m_render_colors, m_render_mapping;
    std::shared_ptr<const PreviewPalette::Histogram> m_histogram;
    std::vector<Color> m_colors, m_project_colors, m_mapping_colors;
    std::vector<wxString> m_project_names;
    wxString m_notice, m_project_error, m_project_signature;
    bool m_enabled {false};
    std::chrono::steady_clock::time_point m_last_check {};
};
} // namespace Slic3r::GUI
