#include "RedesignSettingsDialog.hpp"
#include "PrinterSettingsService.hpp"
#include "PrinterTransferDialogStyle.hpp"
#include "RedesignDetailsViewport.hpp"
#include "RedesignMessageDialog.hpp"
#include "StartupSetupService.hpp"

#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/DiagnosticLogExport.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Preferences.hpp"
#include "slic3r/GUI/Widgets/TabCtrl.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <boost/log/trivial.hpp>
#include <exception>
#include <wx/display.h>
#include <wx/simplebook.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {
namespace Printer = PrinterSettings;

wxString utf8(const std::string& text) { return wxString::FromUTF8(text); }

wxString dimension(double value) { return wxString::Format("%g", value); }

wxString machine_detail(const Printer::Machine& machine)
{
    if (machine.diameters.empty()) return _L("规格不可用");
    wxString diameters;
    for (double d : machine.diameters) {
        const auto label = dimension(d);
        if (diameters.empty()) diameters = label;
        else if (diameters != label) { diameters.clear(); break; }
    }
    if (diameters.empty())
        for (double d : machine.diameters) {
            if (!diameters.empty()) diameters += " / ";
            diameters += dimension(d);
        }
    return wxString::Format(_L("%u 个喷头  ·  %s mm  ·  %s × %s × %s mm"), unsigned(machine.diameters.size()),
        diameters, dimension(machine.space.x()), dimension(machine.space.y()), dimension(machine.space.z()));
}

wxString reference_name(Printer::Reference::Kind kind)
{
    using Kind = Printer::Reference::Kind;
    switch (kind) {
    case Kind::Object: return _L("对象");
    case Kind::Part: return _L("零件");
    case Kind::Layer: return _L("层范围");
    case Kind::Paint: return _L("涂色");
    case Kind::Support: return _L("支撑");
    case Kind::ToolChange: return _L("换色或换刀");
    case Kind::WipeTower: return _L("清洗塔");
    case Kind::PrintOrder: return _L("打印顺序");
    case Kind::Custom: return _L("自定义 G-code");
    }
    return wxString{};
}

std::vector<Printer::Plate> project_plates(Plater& plater)
{
    std::vector<Printer::Plate> result;
    const auto& model = plater.model();
    for (auto* plate : plater.get_partplate_list().get_plate_list()) {
        auto& entry = result.emplace_back();
        entry.config = plate->config();
        for (const auto* object : plate->get_objects_on_this_plate()) {
            const auto found = std::find(model.objects.begin(), model.objects.end(), object);
            if (found != model.objects.end()) entry.objects.push_back(size_t(found - model.objects.begin()));
        }
    }
    return result;
}

bool switch_busy(MainFrame& frame, Plater* plater)
{
    return !plater || plater->only_gcode_mode() || plater->is_loading_project() ||
        !plater->get_ui_job_worker().is_idle() || plater->is_background_process_slicing() ||
        plater->background_process().is_export_scheduled() || plater->background_process().is_upload_scheduled() ||
        frame.settings_printer_switch_blocked();
}

bool switch_restore_required = false;

class RedesignSettingsDialog final : public DPIDialog {
public:
    explicit RedesignSettingsDialog(MainFrame& frame)
        : DPIDialog(&frame, wxID_ANY, _L("Settings"), wxDefaultPosition, wxDefaultSize, printer_transfer_dialog_style(true)),
          m_frame(frame), m_config(*wxGetApp().app_config)
    {
        SetBackgroundColour(RedesignTheme::panel_colour());
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(new PrinterTransferTitleBar(this, _L("Settings")), 0, wxEXPAND);
        m_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
        m_tabs->SetName("ai_content_color");
        m_tabs->SetBackgroundColour(RedesignTheme::panel_colour());
        m_tabs->SetBorderColor(RedesignTheme::divider_colour());
        RedesignTheme::style_text(m_tabs, RedesignTheme::primary_text_colour(), 11);
        for (const auto& title : {_L("General"), _L("Printer"), _L("Developer")}) {
            const auto index = m_tabs->AppendItem(title);
            m_tabs->SetItemTextColour(index, StateColor(
                std::make_pair(RedesignTheme::primary_text_colour(), int(StateColor::Checked)),
                std::make_pair(RedesignTheme::secondary_text_colour(), int(StateColor::Normal))));
        }
        layout->Add(m_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        m_book = new wxSimplebook(this, wxID_ANY);
        m_book->SetBackgroundColour(RedesignTheme::panel_colour());
        build_general(add_page());
        build_printer(add_page());
        build_developer(add_page());
        layout->Add(m_book, 1, wxEXPAND | wxTOP, FromDIP(12));
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        footer->AddStretchSpacer();
        auto* close = make_button(this, _L("关闭"), wxID_CANCEL);
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        footer->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_apply = make_button(this, _L("应用"));
        m_apply->SetName("settings_apply_printer");
        style_printer_transfer_primary_button(m_apply);
        const StateColor apply_fill(std::make_pair(wxColour(58, 58, 62), int(StateColor::Disabled)),
            std::make_pair(RedesignTheme::accent_colour(), int(StateColor::Normal)));
        m_apply->SetBackgroundColor(apply_fill);
        m_apply->SetBorderColor(apply_fill);
        m_apply->SetTextColor(StateColor(std::make_pair(wxColour(165, 165, 170), int(StateColor::Disabled)),
            std::make_pair(wxColour(22, 22, 25), int(StateColor::Normal))));
        m_apply->SetMinSize(FromDIP(wxSize(116, 38)));
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
        footer->Add(m_apply, 0, wxALIGN_CENTER_VERTICAL);
        layout->Add(footer, 0, wxEXPAND | wxALL, FromDIP(16));
        SetSizer(layout);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });
        for (const auto& page : m_pages)
            apply_printer_transfer_redesign_theme(page.content);
        m_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& event) {
            if (event.GetInt() >= 0)
                select(size_t(event.GetInt()));
        });
        m_tabs->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            const int direction = event.GetKeyCode() == WXK_LEFT ? -1 : event.GetKeyCode() == WXK_RIGHT ? 1 : 0;
            if (!direction || event.HasAnyModifiers()) {
                event.Skip();
                return;
            }
            const int next = m_tabs->GetSelection() + direction;
            if (next >= 0 && next < int(m_pages.size())) {
                m_tabs->SelectItem(next);
                m_tabs->SetFocus();
            }
        });
        resize_to_display();
        CentreOnParent();
        m_tabs->SelectItem(0);
        m_tabs->SetFocus();
    }

    bool did_apply() const { return m_did_apply; }

private:
    MainFrame& m_frame;
    AppConfig& m_config;
    wxSimplebook* m_book = nullptr;
    TabCtrl* m_tabs = nullptr;
    struct Page {
        RedesignDetailsViewport* viewport;
        wxPanel* content;
    };
    std::vector<Page> m_pages;
    static constexpr int field_label_width = 156;
    static constexpr int field_gap = 16;
    static constexpr int field_width = 204;
    static constexpr int field_height = 32;
    Button* m_apply = nullptr;
    ::ComboBox* m_model = nullptr;
    ::ComboBox* m_variant = nullptr;
    wxStaticText *m_current = nullptr, *m_target = nullptr, *m_source = nullptr;
    wxBoxSizer* m_materials = nullptr;
    std::vector<std::string> m_models;
    std::vector<Printer::Choice> m_choices, m_variants;
    std::unique_ptr<PresetBundle> m_missing;
    std::unique_ptr<Printer::Plan> m_plan;
    bool m_catalog_loaded = false;
    bool m_applying = false;
    bool m_did_apply = false;

    void on_dpi_changed(const wxRect&) override
    {
        if (m_tabs) m_tabs->Rescale();
        if (m_apply) { m_apply->Rescale(); m_apply->SetMinSize(FromDIP(wxSize(116, 38))); }
        resize_to_display();
        Layout();
        for (size_t index = 0; index < m_pages.size(); ++index) {
            m_pages[index].viewport->scroller()->SetScrollRate(0, FromDIP(12));
            fit_page(index);
        }
    }

    void resize_to_display()
    {
        const int screen = wxDisplay::GetFromWindow(&m_frame);
        const wxSize area = wxDisplay(screen == wxNOT_FOUND ? 0 : screen).GetClientArea().GetSize();
        const wxSize desired = FromDIP(wxSize(640, 540));
        const int margin = FromDIP(32);
        SetSize(wxSize(std::min(desired.x, area.x - margin), std::min(desired.y, area.y - margin)));
    }

    static Button* make_button(wxWindow* parent, const wxString& text, wxWindowID id = wxID_ANY)
    {
        auto* button = new Button(parent, text, wxEmptyString, wxBORDER_NONE, 0, id);
        button->SetMinSize(parent->FromDIP(wxSize(100, 38)));
        button->SetCornerRadius(parent->FromDIP(7));
        button->SetBackgroundColor(StateColor(
            std::make_pair(wxColour(58, 58, 62), int(StateColor::Hovered)),
            std::make_pair(RedesignTheme::control_colour(), int(StateColor::Normal))));
        button->SetBorderColor(RedesignTheme::divider_colour());
        button->SetTextColor(RedesignTheme::primary_text_colour());
        return button;
    }

    wxPanel* add_page()
    {
        auto* viewport = new RedesignDetailsViewport(m_book, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxVSCROLL | wxTAB_TRAVERSAL, RedesignTheme::panel_colour());
        auto* scroller = viewport->scroller();
        auto* content = new wxPanel(scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL);
        content->SetSizer(new wxBoxSizer(wxVERTICAL));
        const size_t index = m_pages.size();
        m_pages.push_back({viewport, content});
        scroller->Bind(wxEVT_SIZE, [this, index](wxSizeEvent& event) {
            event.Skip();
            fit_page(index);
        });
        m_book->AddPage(viewport, wxEmptyString);
        return content;
    }

    void fit_page(size_t index)
    {
        const auto& page = m_pages[index];
        auto* scroller = page.viewport->scroller();
        const wxSize extent(scroller->GetClientSize().x, page.content->GetSizer()->CalcMin().y);
        // The form owns its full height, even when the viewport clips it.
        page.viewport->SetContentExtent(extent);
        const wxPoint origin = scroller->CalcScrolledPosition(wxPoint(0, 0));
        page.content->SetSize(origin.x, origin.y, extent.x, extent.y);
        page.content->Layout();
    }

    wxStaticText* line(wxWindow* parent, const wxString& label, bool primary = true)
    {
        auto* text = new wxStaticText(parent, wxID_ANY, label);
        RedesignTheme::style_text(text, primary ? RedesignTheme::primary_text_colour() : RedesignTheme::secondary_text_colour(), 10);
        return text;
    }

    void add(wxPanel* page, wxSizerItem* item)
    {
        item->SetBorder(FromDIP(14));
        item->SetFlag(wxEXPAND | wxLEFT | wxRIGHT | wxTOP);
        page->GetSizer()->Add(item);
    }

    void row(wxPanel* page, const wxString& title, wxWindow* control)
    {
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* label = line(page, title);
        label->Wrap(FromDIP(field_label_width));
        label->SetMinSize(wxSize(FromDIP(field_label_width), -1));
        sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(field_gap));
        sizer->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        add(page, new wxSizerItem(sizer));
    }

    ::ComboBox* combo(wxWindow* parent)
    {
        auto* result = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                       FromDIP(wxSize(field_width, field_height)), 0, nullptr, wxCB_READONLY);
        result->GetDropDown().SetUseContentWidth(true);
        return result;
    }

    void show(const wxString& message)
    {
        RedesignMessageDialog(this, message, _L("Settings"), wxOK | wxICON_WARNING,
                              RedesignMessageDialog::Appearance::Settings).ShowModal();
    }

    bool confirm(const wxString& message)
    {
        RedesignMessageDialog dialog(this, message, _L("Settings"), wxOK | wxCANCEL,
                                     RedesignMessageDialog::Appearance::Settings);
        return dialog.ShowModal() == wxID_OK;
    }

    bool save_startup_language(const std::string& previous, const std::string& next)
    {
        if (next.empty())
            m_config.erase("app", next_startup_language_key);
        else
            m_config.set(next_startup_language_key, next);
        try {
            m_config.save();
            if (!m_config.dirty()) return true;
        } catch (const std::exception&) {
            BOOST_LOG_TRIVIAL(error) << "Could not save the language preference for the next startup.";
        }
        // Restore only the queued language. Other unsaved preferences still belong to AppConfig.
        if (previous.empty())
            m_config.erase("app", next_startup_language_key);
        else
            m_config.set(next_startup_language_key, previous);
        return false;
    }

    void build_general(wxPanel* page)
    {
        auto* language = combo(page);
        const auto available = available_preference_languages();
        for (const auto& option : available) language->Append(option.label);
        const auto saved = m_config.get(next_startup_language_key);
        language->SetSelection(current_preference_language(available, saved.empty() ? m_config.get("language") : saved));
        row(page, _L("Language"), language);
        auto* language_status = line(page, _L("Language saved. It will take effect on the next launch."), false);
        language_status->Wrap(FromDIP(field_width));
        auto* language_note = new wxBoxSizer(wxHORIZONTAL);
        language_note->AddSpacer(FromDIP(field_label_width + field_gap));
        language_note->Add(language_status);
        page->GetSizer()->Add(language_note, 0, wxLEFT | wxTOP, FromDIP(14));
        language_note->Show(!saved.empty());
        language->Bind(wxEVT_COMBOBOX, [this, language, language_note, available](wxCommandEvent&) {
            const int index = language->GetSelection();
            const auto previous = m_config.get(next_startup_language_key);
            const int active = current_preference_language(available, m_config.get("language"));
            const int saved_index = previous.empty() ? active : current_preference_language(available, previous);
            if (index < 0 || size_t(index) >= available.size() || index == saved_index) return;
            if (!confirm(_L("Save the selected language? It will take effect on the next launch."))) {
                language->SetSelection(saved_index);
                return;
            }
            const std::string next = index == active ? std::string{} : available[index].canonical;
            if (!save_startup_language(previous, next)) {
                language->SetSelection(saved_index);
                show(_L("Could not save the language selection. Please try again."));
                return;
            }
            language_note->Show(!next.empty());
            fit_page(0);
        });
        auto* dark_mode = new CheckBox(page);
        dark_mode->SetName("redesign_settings_checkbox");
        dark_mode->SetValue(true);
        dark_mode->Disable();
        row(page, _L("Enable dark Mode"), dark_mode);
        auto* splash = new CheckBox(page);
        splash->SetName("redesign_settings_checkbox");
        splash->SetValue(m_config.get("show_splash_screen") == "true");
        splash->Bind(wxEVT_TOGGLEBUTTON, [this, splash](wxCommandEvent&) {
            m_config.set("show_splash_screen", splash->GetValue() ? "true" : "false");
            m_config.save();
        });
        row(page, _L("Show splash screen"), splash);
    }

    void build_developer(wxPanel* page)
    {
        auto* level = combo(page);
        constexpr std::array<const char*, 6> levels {"fatal", "error", "warning", "info", "debug", "trace"};
        for (const char* item : levels) level->Append(wxString::FromUTF8(item));
        level->SetStringSelection(utf8(m_config.get("log_severity_level")));
        level->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& event) {
            if (event.GetSelection() < 0 || event.GetSelection() >= 6) return;
            const auto selected = Slic3r::get_string_logging_level(event.GetSelection());
            Slic3r::set_logging_level(Slic3r::level_string_to_boost(selected));
            m_config.set("log_severity_level", selected);
        });
        row(page, _L("Log Level"), level);
        auto* export_logs = make_button(page, _L("导出日志…"));
        export_logs->SetMinSize(FromDIP(wxSize(field_width, field_height)));
        export_logs->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            export_diagnostic_logs(this, DiagnosticLogAppearance::Settings);
        });
        row(page, _L("诊断日志"), export_logs);
    }

    void build_printer(wxPanel* page)
    {
        add(page, new wxSizerItem(line(page, _L("当前打印机"))));
        m_current = line(page, wxEmptyString, false);
        add(page, new wxSizerItem(m_current));
        m_model = combo(page);
        m_model->SetName("settings_printer_model");
        m_variant = combo(page);
        m_variant->SetName("settings_nozzle_size");
        row(page, _L("目标机型"), m_model);
        row(page, _L("喷嘴直径"), m_variant);
        m_target = line(page, wxEmptyString, false);
        add(page, new wxSizerItem(m_target));
        m_source = line(page, wxEmptyString);
        add(page, new wxSizerItem(m_source));
        m_materials = new wxBoxSizer(wxVERTICAL);
        add(page, new wxSizerItem(m_materials));
        page->GetSizer()->AddSpacer(FromDIP(8));
        m_model->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { choose_model(); });
        m_variant->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { preview(); });
    }

    void load_choices()
    {
        if (m_catalog_loaded) return;
        m_catalog_loaded = true;
        auto* bundle = wxGetApp().preset_bundle;
        if (!bundle) return;
        const auto& active = bundle->printers.get_edited_preset();
        const auto info = Printer::describe(active);
        m_current->SetLabel(utf8(info.name) + "\n" + machine_detail(info));
        try { m_missing = Printer::read_missing_catalog(*bundle); }
        catch (const std::exception&) { /* Existing profiles are still usable. */ }
        m_choices = Printer::choices(*bundle, m_missing.get());
        const auto* active_model = active.config.option<ConfigOptionString>("printer_model");
        for (const char* model : startup_printer_models) {
            m_models.emplace_back(model);
            m_model->Append(utf8(model));
        }
        if (active_model) {
            const auto current = std::find(m_models.begin(), m_models.end(), active_model->value);
            if (current != m_models.end()) m_model->SetSelection(int(current - m_models.begin()));
        }
        choose_model();
    }

    void choose_model()
    {
        m_variants.clear();
        m_variant->Clear();
        const int selected = m_model->GetSelection();
        if (selected >= 0 && size_t(selected) < m_models.size())
            for (const auto& entry : m_choices)
                if (entry.model == m_models[selected]) {
                    m_variants.push_back(entry);
                    m_variant->Append(dimension(entry.diameter) + " mm");
                }
        m_variant->Enable(!m_variants.empty());
        if (!m_variants.empty()) {
            const auto current = Printer::describe(wxGetApp().preset_bundle->printers.get_edited_preset());
            auto match = std::find_if(m_variants.begin(), m_variants.end(), [&](const Printer::Choice& option) {
                return !current.diameters.empty() && option.diameter == current.diameters.front();
            });
            if (match == m_variants.end())
                match = std::find_if(m_variants.begin(), m_variants.end(), [](const Printer::Choice& option) { return option.diameter == 0.4; });
            m_variant->SetSelection(match == m_variants.end() ? 0 : int(match - m_variants.begin()));
        }
        preview();
    }

    void preview()
    {
        auto* page = m_pages[1].content;
        m_plan.reset();
        m_materials->Clear(true);
        m_target->SetLabel(wxEmptyString);
        m_source->SetLabel(wxEmptyString);
        const int selected = m_variant->GetSelection();
        if (selected >= 0 && size_t(selected) < m_variants.size()) {
            const Model empty;
            m_plan = std::make_unique<Printer::Plan>(Printer::prepare(*wxGetApp().preset_bundle, m_config,
                m_variants[selected], empty, {}, m_missing.get()));
            if (!m_plan->error.empty()) {
                m_source->SetLabel(_L("此规格当前不可用。请检查本地打印机预设。"));
                m_source->SetToolTip(utf8(m_plan->error));
            } else {
                m_source->UnsetToolTip();
                m_target->SetLabel(machine_detail(m_plan->machine));
                const size_t loaded = std::count_if(m_plan->materials.begin(), m_plan->materials.end(),
                    [](const Printer::Material& material) { return !material.mixed; });
                const size_t idle = m_plan->machine.diameters.size() > loaded ? m_plan->machine.diameters.size() - loaded : 0;
                auto summary = m_plan->remembered ? _L("耗材 · 上次配置") : _L("耗材 · 预设默认");
                summary += wxString::Format(_L("  ·  %u 个实体槽  ·  %u 个闲置喷头"), unsigned(loaded), unsigned(idle));
                m_source->SetLabel(summary);
                for (const auto& material : m_plan->materials) {
                    auto* entry = new wxBoxSizer(wxHORIZONTAL);
                    auto* swatch = new wxPanel(page, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(16, 16)));
                    swatch->SetMinSize(FromDIP(wxSize(16, 16)));
                    const wxColour color(utf8(material.color));
                    swatch->SetBackgroundColour(color.IsOk() ? color : RedesignTheme::divider_colour());
                    entry->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
                    wxString label = wxString::Format("%u. ", unsigned(material.slot + 1));
                    label += material.mixed ? _L("叠色") + "  " + utf8(material.recipe) :
                        utf8(material.name) + "  ·  " + utf8(material.type);
                    label += "  " + utf8(material.color);
                    if (std::find(m_plan->replaced_saved_slots.begin(), m_plan->replaced_saved_slots.end(), material.slot) != m_plan->replaced_saved_slots.end())
                        label += "  ·  " + _L("已替换为兼容耗材");
                    auto* text = line(page, label, false);
                    text->Wrap(std::max(FromDIP(200), GetClientSize().x - FromDIP(80)));
                    entry->Add(text, 1, wxALIGN_CENTER_VERTICAL);
                    m_materials->Add(entry, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
                }
            }
        } else {
            m_source->SetLabel(m_model->GetSelection() == wxNOT_FOUND ? _L("选择机型和直径以查看配置。") : _L("此机型的本地规格不可用。"));
        }
        m_apply->Enable(!switch_restore_required && m_plan && !Printer::blocks(*m_plan) &&
            m_plan->target.preset != wxGetApp().preset_bundle->printers.get_selected_preset_name());
        fit_page(1);
    }

    void select(size_t page)
    {
        m_book->ChangeSelection(page);
        for (size_t i = 0; i < m_pages.size(); ++i)
            m_tabs->SetItemBold(i, i == page);
        m_apply->Show(page == 1);
        if (page == 1) load_choices();
        Layout();
        fit_page(page);
    }

    void apply()
    {
        if (m_applying || switch_restore_required || !m_plan || Printer::blocks(*m_plan)) return;
        struct Applying { bool& active; ~Applying() { active = false; } } once{m_applying};
        m_applying = true;
        auto& app = wxGetApp();
        auto* plater = app.plater();
        if (switch_busy(m_frame, plater)) {
            show(_L("工程或工作台正在处理，请在当前操作结束后重试。"));
            return;
        }
        if (app.has_unsaved_preset_changes()) {
            show(_L("请先在原生预设入口保存或放弃未保存的预设编辑，再切换打印机。"));
            return;
        }
        const auto target = m_plan->target;
        const auto reviewed = Printer::prepare(*app.preset_bundle, m_config, target, plater->model(), project_plates(*plater), m_missing.get());
        if (!reviewed.error.empty()) { show(_L("此打印机配置不可用。请重新检查规格和耗材。")); return; }
        const bool blocked = Printer::blocks(reviewed);
        wxString details;
        for (const auto& impact : reviewed.impacts) {
            if (blocked && !impact.blocking) continue;
            const auto& ref = impact.reference;
            if (ref.kind == Printer::Reference::Kind::Custom)
                details += wxString::Format(_L("\n第 %u 盘有无法安全映射的自定义 G-code。"), unsigned(ref.plate + 1));
            else if (impact.blocking)
                details += wxString::Format(_L("\n第 %u 盘的%s引用 %u 号槽，目标配置无法保留。"),
                    unsigned(ref.plate + 1), reference_name(ref.kind), unsigned(ref.slot + 1));
            else
                details += wxString::Format(_L("\n第 %u 盘 · %u 号槽 · %s：%s → %s"),
                    unsigned(ref.plate + 1), unsigned(ref.slot + 1), reference_name(ref.kind), utf8(impact.before), utf8(impact.after));
        }
        if (blocked) {
            show(_L("工程使用了目标配置无法保留的耗材槽位。") + details + "\n\n" +
                 _L("如果目标机器有足够的喷头，请先保存当前工程，再在空工程里准备目标的耗材配置。"));
            return;
        }
        if (!confirm(wxString::Format(_L("应用 %s 的打印机配置？"), utf8(target.preset)) + details + "\n\n" +
                     _L("应用后，依赖旧打印机的耗材匹配和切片需重新确认。"))) return;
        if (switch_busy(m_frame, plater) || app.has_unsaved_preset_changes()) {
            show(_L("当前状态已变化，请完成当前操作后重新检查。"));
            return;
        }
        switch (Printer::apply(*app.preset_bundle, m_config, reviewed, plater->model(), project_plates(*plater), m_missing.get())) {
        case Printer::Result::Applied: {
            PresetBundle::SelectionExportBlocker block(*app.preset_bundle);
            plater->suppress_background_process(true);
            app.load_current_presets(false, false);
            plater->get_partplate_list().invalid_all_slice_result();
            plater->reset_gcode_toolpaths();
            plater->reload_print();
            plater->suppress_background_process(true);
            plater->schedule_background_process(false);
            m_did_apply = true;
            EndModal(wxID_OK);
            break;
        }
        case Printer::Result::NoChange: preview(); break;
        case Printer::Result::Busy:
        case Printer::Result::Stale:
        case Printer::Result::Blocked:
            preview();
            show(_L("工程或打印机配置已变化，请检查预览后重新应用。"));
            break;
        case Printer::Result::SaveFailed:
            show(_L("无法保存打印机配置。当前配置仍可使用，请检查存储后重试。"));
            break;
        case Printer::Result::RestoreFailed:
            switch_restore_required = true;
            m_apply->Disable();
            show(_L("无法确认打印机配置的保存状态。请保存工程副本，重启后检查当前打印机。"));
            break;
        }
    }
};

} // namespace

void open_redesign_settings()
{
    auto& app = wxGetApp();
    auto* frame = app.mainframe;
    if (!frame || !app.app_config || !app.preset_bundle) return;
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus());
    bool applied;
    {
        RedesignSettingsDialog dialog(*frame);
        dialog.ShowModal();
        applied = dialog.did_apply();
    }
    wxWeakRef<MainFrame> frame_ref(frame);
    app.CallAfter([frame_ref, focus, applied] {
        if (frame_ref && applied) frame_ref->settings_printer_switch_applied();
        if (focus && focus->IsShownOnScreen()) focus->SetFocus();
    });
}

} // namespace Slic3r::GUI
