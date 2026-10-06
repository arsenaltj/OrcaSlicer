#include "StartupSetupService.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <boost/filesystem.hpp>

namespace Slic3r::GUI {
namespace {

constexpr std::array<const char*, 3> models = {
    "WonderMaker ZR", "WonderMaker ZR Ultra", "WonderMaker ZR Ultra S"
};
constexpr std::array<const char*, 3> labels = { "ZR", "ZR Ultra", "ZR Ultra S" };

const StartupSetupFilament* find_filament(const StartupSetupPrinter& printer, const std::string& name)
{
    auto it = std::find_if(printer.filaments.begin(), printer.filaments.end(),
                           [&](const auto& filament) { return filament.preset == name; });
    return it == printer.filaments.end() ? nullptr : &*it;
}

std::string first_string(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionStrings>(key);
    return option && !option->values.empty() ? option->values.front() : std::string();
}

void copy_setup_bundle(PresetBundle& destination, PresetBundle& source)
{
    destination = source;
    // The core copy operator covers preset collections but omits runtime data.
    destination.filament_ams_list = source.filament_ams_list;
    destination.extruder_ams_counts = source.extruder_ams_counts;
    destination.ams_multi_color_filment = source.ams_multi_color_filment;
    destination.m_config_maps = source.m_config_maps;
    destination.m_filament_id_maps = source.m_filament_id_maps;
    destination.dir_user_presets_local = source.dir_user_presets_local;
    destination.dir_user_presets_subscribed = source.dir_user_presets_subscribed;
    {
        std::shared_lock source_lock(source.bundles.RWMtx);
        std::unique_lock destination_lock(destination.bundles.RWMtx);
        destination.bundles.m_bundles = source.bundles.m_bundles;
    }
    destination.calibrate_printer = nullptr;
    destination.calibrate_filaments.clear();
    for (const auto& printer : source.printers)
        if (&printer == source.calibrate_printer)
            destination.calibrate_printer = destination.printers.find_preset(printer.name, false);
    for (const auto& filament : source.filaments)
        if (source.calibrate_filaments.count(&filament))
            if (const auto* copied = destination.filaments.find_preset(filament.name, false))
                destination.calibrate_filaments.insert(copied);
}

void save_checked(AppConfig& config)
{
    // AppConfig::save() returns void and can report a write failure only in its log.
    config.set_dirty();
    config.save();
    if (config.dirty())
        throw std::runtime_error("Could not write the application configuration.");
    AppConfig persisted;
    const std::string error = persisted.load();
    if (!error.empty() || persisted.get("firstguide", "finish") != "1" ||
        persisted.get("region") != config.get("region") ||
        persisted.get("presets", PRESET_PRINTER_NAME) != config.get("presets", PRESET_PRINTER_NAME))
        throw std::runtime_error("Could not verify the saved application configuration.");
    const auto printer = config.get("presets", PRESET_PRINTER_NAME);
    for (const char* key : {PRESET_FILAMENT_NAME, "filament_01", "filament_02", "filament_03",
                            "filament_colors", "extruder_ams_count"})
        if (persisted.get_printer_setting(printer, key) != config.get_printer_setting(printer, key))
            throw std::runtime_error("Could not verify the saved filament selections.");
}

}

bool apply_startup_vendor_selection(PresetBundle& bundle, AppConfig& config,
    const AppConfig::VendorMap& vendors, const std::map<std::string, std::string>& filaments,
    bool overwrite, const std::string& model, const std::string& variant,
    const std::string& preferred_filament)
{
    return bundle.apply_vendor_config(vendors, filaments, &config, overwrite, model, variant, preferred_filament);
}

StartupSetupService::StartupSetupService(const AppConfig& config)
{
    m_state.draft.region = region_from_config(config.get("region"));
    // Custom colour edits remain in the draft until the entire setup is saved.
    AppConfig stored = config;
    m_state.draft.custom_colours = stored.get_custom_color_from_config();
}

bool StartupSetupService::needs_setup(bool config_exists, bool valid_printer)
{
    return !config_exists || !valid_printer;
}

StartupSetupRegion StartupSetupService::region_from_config(const std::string& value)
{
    if (value == "Asia-Pacific") return StartupSetupRegion::AsiaPacific;
    if (value == "Europe") return StartupSetupRegion::Europe;
    if (value == "North America" || value == "USA") return StartupSetupRegion::NorthAmerica;
    return StartupSetupRegion::China;
}

std::string StartupSetupService::region_config_value(StartupSetupRegion value)
{
    switch (value) {
    case StartupSetupRegion::AsiaPacific: return "Asia-Pacific";
    case StartupSetupRegion::Europe: return "Europe";
    case StartupSetupRegion::NorthAmerica: return "North America";
    default: return "China";
    }
}

bool StartupSetupService::valid_colour(const std::string& value)
{
    return value.size() == 7 && value.front() == '#' &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::vector<StartupSetupPrinter> StartupSetupService::catalog_from_bundle(
    const PresetBundle& bundle, const std::string& resources)
{
    std::vector<StartupSetupPrinter> result;
    for (size_t index = 0; index < models.size(); ++index) {
        StartupSetupPrinter printer;
        printer.model = models[index];
        printer.label = labels[index];
        printer.image_path = (boost::filesystem::path(resources) / "profiles" / "WonderMaker" /
                               (printer.model + "_cover.png")).string();
        for (const Preset& preset : bundle.printers()) {
            const auto* model = preset.config.option<ConfigOptionString>("printer_model");
            const auto* variant = preset.config.option<ConfigOptionString>("printer_variant");
            const auto* nozzles = preset.config.option<ConfigOptionFloats>("nozzle_diameter");
            if (!preset.is_system || !model || model->value != printer.model || !variant ||
                variant->value != "0.4" || !nozzles || nozzles->values.empty() ||
                !std::all_of(nozzles->values.begin(), nozzles->values.end(),
                             [](double value) { return std::abs(value - 0.4) < 1e-6; }))
                continue;
            printer.preset = preset.name;
            printer.head_count = nozzles->values.size();
            for (const Preset& filament : bundle.filaments()) {
                if (!filament.is_system || filament.is_default ||
                    !is_compatible_with_printer(PresetWithVendorProfile(filament, filament.vendor),
                                               PresetWithVendorProfile(preset, preset.vendor)))
                    continue;
                StartupSetupFilament item;
                item.preset = filament.name;
                item.label = filament.name;
                const std::string prefix = "WonderMaker ";
                if (item.label.compare(0, prefix.size(), prefix) == 0)
                    item.label.erase(0, prefix.size());
                item.type = first_string(filament.config, "filament_type");
                item.colour = first_string(filament.config, "filament_colour");
                if (!valid_colour(item.colour)) item.colour = "#FFFFFF";
                printer.filaments.push_back(std::move(item));
            }
            break;
        }
        std::stable_sort(printer.filaments.begin(), printer.filaments.end(), [](const auto& a, const auto& b) {
            if ((a.preset == "WonderMaker PLA Basic") != (b.preset == "WonderMaker PLA Basic"))
                return a.preset == "WonderMaker PLA Basic";
            return a.label < b.label;
        });
        if (printer.preset.empty()) printer.error = "Missing 0.4 mm printer preset: " + printer.model;
        else if (printer.filaments.empty()) printer.error = "No compatible filament presets for " + printer.model;
        result.push_back(std::move(printer));
    }
    return result;
}

std::vector<StartupSetupPrinter> StartupSetupService::load_catalog(const std::string& resources,
    const std::string& data, const std::atomic<bool>& cancel)
{
    namespace fs = boost::filesystem;
    PresetBundle catalog;
    auto source = [&](const std::string& vendor) {
        fs::path installed = fs::path(data) / PRESET_SYSTEM_DIR;
        return fs::exists(installed / (vendor + ".json")) || fs::exists(installed / (vendor + ".opc")) ?
            installed : fs::path(resources) / "profiles";
    };
    const std::string library = PresetBundle::ORCA_FILAMENT_LIBRARY;
    if (cancel) return {};
    catalog.load_vendor_configs_from_json(source(library).string(), library, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    if (cancel) return {};
    PresetBundle vendor;
    vendor.load_vendor_configs_from_json(source("WonderMaker").string(), "WonderMaker", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent, &catalog);
    if (cancel) return {};
    catalog.merge_presets(std::move(vendor));
    return catalog_from_bundle(catalog, resources);
}

const StartupSetupPrinter* StartupSetupService::selected_printer() const
{
    auto it = std::find_if(m_state.printers.begin(), m_state.printers.end(),
                           [&](const auto& printer) { return printer.model == m_state.draft.printer_model; });
    return it == m_state.printers.end() ? nullptr : &*it;
}

bool StartupSetupService::editable() const
{
    return !m_state.submitting && !m_state.completed && !m_state.cancelled;
}

bool StartupSetupService::valid_draft() const
{
    const auto* printer = selected_printer();
    if (m_state.loading || !printer || !printer->error.empty() || printer->head_count == 0 ||
        m_state.draft.heads.size() != printer->head_count)
        return false;
    return std::all_of(m_state.draft.heads.begin(), m_state.draft.heads.end(), [&](const auto& head) {
        return find_filament(*printer, head.filament) && valid_colour(head.colour);
    });
}

StartupSetupSnapshot StartupSetupService::snapshot() const
{
    auto state = m_state;
    state.can_next = editable() && (state.page != StartupSetupPage::Printers ||
                                     (!state.loading && selected_printer()));
    state.can_finish = editable() && state.page == StartupSetupPage::Filaments &&
                       state.error.empty() && valid_draft();
    return state;
}

std::uint64_t StartupSetupService::begin_load()
{
    if (!editable()) return m_revision;
    m_state.loading = true;
    m_state.error.clear();
    return ++m_revision;
}

bool StartupSetupService::finish_load(std::uint64_t revision, std::vector<StartupSetupPrinter> printers,
                                     std::string error)
{
    if (revision != m_revision || !editable()) return false;
    m_state.loading = false;
    m_state.printers = std::move(printers);
    m_state.error = std::move(error);
    reconcile_heads();
    return true;
}

bool StartupSetupService::choose_region(StartupSetupRegion region)
{
    if (!editable()) return false;
    m_state.draft.region = region;
    return true;
}

void StartupSetupService::reconcile_heads()
{
    const auto* printer = selected_printer();
    if (!printer) return;
    m_state.draft.heads.resize(printer->head_count);
    for (auto& head : m_state.draft.heads) {
        if (!find_filament(*printer, head.filament)) {
            head.filament = printer->filaments.empty() ? "" : printer->filaments.front().preset;
            if (!valid_colour(head.colour))
                head.colour = printer->filaments.empty() ? "#FFFFFF" : printer->filaments.front().colour;
        }
    }
    m_state.active_head = std::min(m_state.active_head, printer->head_count == 0 ? 0 : printer->head_count - 1);
}

bool StartupSetupService::choose_printer(const std::string& model)
{
    if (!editable() || m_state.loading) return false;
    const auto it = std::find_if(m_state.printers.begin(), m_state.printers.end(),
                                 [&](const auto& printer) { return printer.model == model; });
    if (it == m_state.printers.end()) return false;
    m_state.draft.printer_model = model;
    m_state.error.clear();
    reconcile_heads();
    return true;
}

bool StartupSetupService::choose_head(size_t index)
{
    if (!editable() || index >= m_state.draft.heads.size()) return false;
    m_state.active_head = index;
    return true;
}

bool StartupSetupService::choose_filament(const std::string& name)
{
    const auto* printer = selected_printer();
    if (!editable() || !printer || m_state.active_head >= m_state.draft.heads.size() ||
        !find_filament(*printer, name)) return false;
    m_state.draft.heads[m_state.active_head].filament = name;
    m_state.error.clear();
    return true;
}

bool StartupSetupService::choose_colour(const std::string& colour)
{
    if (!editable() || !valid_colour(colour) || m_state.active_head >= m_state.draft.heads.size()) return false;
    m_state.draft.heads[m_state.active_head].colour = colour;
    m_state.error.clear();
    return true;
}

void StartupSetupService::set_custom_colours(std::vector<std::string> colours)
{
    if (editable()) m_state.draft.custom_colours = std::move(colours);
}

bool StartupSetupService::next()
{
    if (!snapshot().can_next || m_state.page == StartupSetupPage::Filaments) return false;
    m_state.page = static_cast<StartupSetupPage>(static_cast<int>(m_state.page) + 1);
    return true;
}

bool StartupSetupService::back()
{
    if (!editable() || m_state.page == StartupSetupPage::Welcome) return false;
    m_state.error.clear();
    m_state.page = static_cast<StartupSetupPage>(static_cast<int>(m_state.page) - 1);
    return true;
}

void StartupSetupService::cancel()
{
    if (!editable()) return;
    ++m_revision;
    m_state.cancelled = true;
    m_state.draft = {};
}

bool StartupSetupService::complete(AppConfig& config, PresetBundle& bundle, SaveConfig save)
{
    if (!snapshot().can_finish) return false;
    m_state.submitting = true;
    m_state.error.clear();
    const auto draft = m_state.draft;
    const auto printer = *selected_printer();
    const AppConfig previous_config = config;
    bool persistence_started = false;
    bool publication_started = false;
    std::unique_ptr<PresetBundle> previous_bundle;
    try {
        // Install and validate against a private copy. The live preset selections
        // and AppConfig are published only after the complete configuration is saved.
        AppConfig staged_config = config;
        PresetBundle staged_bundle;
        copy_setup_bundle(staged_bundle, bundle);
        if (!is_vendor_installed(PresetBundle::ORCA_FILAMENT_LIBRARY) &&
            !install_vendor_bundles_from_resources({PresetBundle::ORCA_FILAMENT_LIBRARY}))
            throw std::runtime_error("Could not install the local filament library.");
        AppConfig::VendorMap vendors;
        vendors["WonderMaker"][printer.model].insert("0.4");
        std::map<std::string, std::string> filaments;
        for (const auto& head : draft.heads) filaments[head.filament] = "true";
        if (!apply_startup_vendor_selection(staged_bundle, staged_config, vendors, filaments, false,
                                            printer.model, "0.4", draft.heads.front().filament))
            throw std::runtime_error("Could not install the selected printer profiles.");
        const Preset& active = staged_bundle.printers.get_selected_preset();
        const auto* nozzles = active.config.option<ConfigOptionFloats>("nozzle_diameter");
        if (active.name != printer.preset || !nozzles || nozzles->values.size() != draft.heads.size() ||
            !std::all_of(nozzles->values.begin(), nozzles->values.end(),
                         [](double value) { return std::abs(value - 0.4) < 1e-6; }))
            throw std::runtime_error("The printer configuration changed. Reload the local profiles and select it again.");
        staged_bundle.set_num_filaments(static_cast<unsigned int>(draft.heads.size()));
        std::vector<std::string> colours;
        staged_bundle.extruder_ams_counts.assign(draft.heads.size(), {});
        for (size_t index = 0; index < draft.heads.size(); ++index) {
            const auto& head = draft.heads[index];
            const Preset* filament = staged_bundle.filaments.find_preset(head.filament, false);
            if (!filament || !filament->is_visible ||
                !is_compatible_with_printer(PresetWithVendorProfile(*filament, filament->vendor),
                                           PresetWithVendorProfile(active, active.vendor)))
                throw std::runtime_error("The selected filament is no longer compatible: " + head.filament);
            staged_bundle.set_filament_preset(index, head.filament);
            colours.push_back(head.colour);
        }
        staged_bundle.filaments.select_preset_by_name(draft.heads.front().filament, true);
        staged_bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = colours;
        staged_bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = colours;
        staged_bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values.assign(colours.size(), "1");
        staged_bundle.export_selections(staged_config);
        staged_config.set("region", region_config_value(draft.region));
        staged_config.save_custom_color_to_config(draft.custom_colours);
        staged_config.set_legacy_datadir(false);
        staged_config.set("firstguide", "finish", std::string("1"));
        previous_bundle = std::make_unique<PresetBundle>();
        copy_setup_bundle(*previous_bundle, bundle);
        persistence_started = true;
        if (save) save(staged_config);
        else save_checked(staged_config);
        publication_started = true;
        copy_setup_bundle(bundle, staged_bundle);
        config = staged_config;
        m_state.completed = true;
    } catch (const std::exception& error) {
        m_state.error = error.what();
        if (publication_started) {
            try {
                copy_setup_bundle(bundle, *previous_bundle);
                config = previous_config;
            } catch (const std::exception& restore_error) {
                m_state.error += std::string(" Selection restore failed: ") + restore_error.what();
            }
        }
        if (persistence_started && !save) {
            try {
                AppConfig restore = previous_config;
                restore.set_dirty();
                restore.save();
                if (restore.dirty()) m_state.error += " The previous configuration could not be written back.";
            } catch (const std::exception& restore_error) {
                m_state.error += std::string(" Restore failed: ") + restore_error.what();
            }
        }
    }
    m_state.submitting = false;
    return m_state.completed;
}

}
