#include "PrinterSettingsService.hpp"
#include "StartupSetupService.hpp"

#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace Slic3r::GUI::PrinterSettings {
namespace {
namespace fs = boost::filesystem;
constexpr const char* wondermaker = "WonderMaker";
using Json = nlohmann::json;
using Kind = Reference::Kind;

const Preset* exact(const PresetCollection& collection, const std::string& name)
{
    const auto* preset = collection.find_preset(name, false);
    return preset && preset->name == name ? preset : nullptr;
}

std::string string_option(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionString>(key);
    return option ? option->value : std::string();
}

Json config_key(const DynamicPrintConfig& config)
{
    Json key = Json::object();
    for (const auto& name : config.keys())
        key[name] = config.option(name)->serialize();
    return key;
}

Json selection_key(const PresetBundle& bundle)
{
    Json result = {
        bundle.printers.get_selected_preset_name(), config_key(bundle.printers.get_edited_preset().config),
        bundle.prints.get_selected_preset_name(), config_key(bundle.prints.get_edited_preset().config),
        bundle.filament_presets, config_key(bundle.project_config)
    };
    Json materials = Json::object();
    for (const auto& name : bundle.filament_presets)
        if (const auto* preset = exact(bundle.filaments, name))
            materials[name] = config_key(preset->config);
    result.push_back(std::move(materials));
    return result;
}

std::vector<Material> summarize(const PresetBundle& bundle)
{
    const auto snapshot = PrinterFilamentConfig::capture(bundle);
    const auto routing = effective_physical_extruder_map(bundle);
    const auto* logical = bundle.project_config.option<ConfigOptionInts>("filament_map");
    const bool manual = bundle.project_config.has("filament_map_mode") &&
        !is_auto_filament_map_mode(bundle.project_config.opt_enum<FilamentMapMode>("filament_map_mode"));
    const auto* maps = bundle.project_config.option<ConfigOptionInts>("filament_volume_map");
    const auto& printer = bundle.printers.get_edited_preset().config;
    const auto nozzles = snapshot.nozzles;
    const bool independent = nozzles > 1 && !printer.opt_bool("single_extruder_multi_material") && !bundle.is_bbl_vendor();
    std::vector<Material> result;
    result.reserve(bundle.filament_presets.size());
    for (const auto& slot : snapshot.slots) {
        size_t extruder = slot.slot;
        if (manual && logical && slot.slot < logical->values.size())
            extruder = logical->values[slot.slot] > 0 ? size_t(logical->values[slot.slot] - 1) : nozzles;
        const int head = independent ? (extruder < routing.values.size() ? routing.values[extruder] + 1 : 0) : 1;
        const int volume = maps && slot.slot < maps->values.size() ? maps->values[slot.slot] : 0;
        result.push_back({slot.slot, slot.preset, slot.material, slot.color, {},
            std::to_string(head) + "/" + std::to_string(volume), false,
            slot.configured && slot.compatible && head > 0 && size_t(head) <= nozzles &&
                snapshot.physical_count <= snapshot.physical_capacity});
    }
    for (const auto& mix : snapshot.mixed)
        result.push_back({mix.slot, {}, {}, mix.color, mix.recipe, {}, true, mix.valid});
    std::sort(result.begin(), result.end(), [](const Material& a, const Material& b) { return a.slot < b.slot; });
    return result;
}

std::string material_key(const Material& item)
{
    return Json({item.name, item.type, item.color, item.recipe, item.routing}).dump();
}

std::string label(const Material& item)
{
    return (item.mixed ? item.recipe : item.name) + "  " + item.color +
        (item.routing.empty() ? "" : "  [" + item.routing + "]");
}

// The point of no return is the native refresh. Until then we can put the disk
// and the selected configurations back without running any consumer on a draft.
class VendorFiles {
public:
    explicit VendorFiles(const PresetBundle* additional, const PresetBundle& live)
    {
        if (!additional)
            return;
        for (const auto& [name, entry] : additional->vendors)
            if (!live.vendors.count(name) && !is_vendor_installed(name))
                m_names.push_back(name);
    }

    void publish()
    {
        if (m_names.empty())
            return;
        const fs::path data = fs::canonical(fs::path(data_dir()));
        auto stage = data / fs::unique_path("printer-settings-%%%%%%%%");
        // Remove only a directory created by this operation under the resolved data root.
        if (stage.parent_path() != data || !fs::create_directory(stage))
            throw std::runtime_error("Cannot create temporary printer profile directory");
        m_stage = std::move(stage);
        if (!install_vendor_bundles_from_resources(m_names, "profiles", m_stage.filename().string()))
            throw std::runtime_error("Cannot prepare shipped printer profiles");
        const auto system = data / PRESET_SYSTEM_DIR;
        fs::create_directories(system);
        if (fs::canonical(system).parent_path() != data)
            throw std::runtime_error("The installed printer directory is outside the application data directory");
        for (const auto& name : m_names) {
            if (is_vendor_installed(name))
                throw std::runtime_error("Installed printer profiles changed");
            for (const std::string& entry : {name, name + ".json", name + ".opc"}) {
                const auto from = m_stage / entry;
                if (!fs::exists(from))
                    continue;
                const auto to = system / entry;
                if (fs::exists(to))
                    throw std::runtime_error("Printer profile destination already exists");
                fs::rename(from, to);
                m_published.emplace_back(to, from);
            }
        }
    }

    void keep() { m_keep = true; }
    ~VendorFiles()
    {
        boost::system::error_code ec;
        if (!m_keep)
            for (auto it = m_published.rbegin(); it != m_published.rend(); ++it)
                fs::rename(it->first, it->second, ec);
        if (!m_stage.empty())
            fs::remove_all(m_stage, ec);
    }

private:
    std::vector<std::string> m_names;
    fs::path m_stage;
    std::vector<std::pair<fs::path, fs::path>> m_published;
    bool m_keep = false;
};

bool has_local(const PresetBundle& bundle, const Choice& choice)
{
    const auto* preset = exact(bundle.printers, choice.preset);
    return preset && preset->is_system && preset->vendor && preset->vendor->id == choice.vendor;
}

void read_vendor(PresetBundle& destination, const std::string& name, const PresetBundle* base)
{
    const fs::path installed = fs::path(data_dir()) / PRESET_SYSTEM_DIR;
    const fs::path path = is_vendor_installed(name) ? installed : fs::path(resources_dir()) / "profiles";
    PresetBundle vendor;
    vendor.load_vendor_configs_from_json(path.string(), name, PresetBundle::LoadSystem,
                                        ForwardCompatibilitySubstitutionRule::EnableSilent, base);
    destination.merge_presets(std::move(vendor));
}

} // namespace

bool Choice::operator==(const Choice& other) const
{
    return std::tie(vendor, model, variant, preset, diameter) ==
           std::tie(other.vendor, other.model, other.variant, other.preset, other.diameter);
}

std::unique_ptr<PresetBundle> read_missing_catalog(const PresetBundle& live)
{
    if (live.vendors.count(wondermaker))
        return {};
    auto additional = std::make_unique<PresetBundle>();
    const std::string library = PresetBundle::ORCA_FILAMENT_LIBRARY;
    if (!live.vendors.count(library))
        read_vendor(*additional, library, nullptr);
    read_vendor(*additional, wondermaker, additional->vendors.count(library) ? additional.get() : &live);
    return additional;
}

std::vector<Choice> choices(const PresetBundle& live, const PresetBundle* additional)
{
    std::vector<Choice> result;
    for (const std::string_view model : startup_printer_models) {
        std::vector<Choice> variants;
        for (const auto* bundle : {&live, additional}) {
            if (!bundle)
                continue;
            for (const auto& preset : bundle->printers) {
                if (!preset.is_system || !preset.vendor || preset.vendor->id != wondermaker ||
                    string_option(preset.config, "printer_model") != model || preset.printer_technology() != ptFFF)
                    continue;
                const auto* nozzles = preset.config.option<ConfigOptionFloats>("nozzle_diameter");
                const auto variant = string_option(preset.config, "printer_variant");
                if (!nozzles || nozzles->values.empty() || variant.empty())
                    continue;
                const double diameter = nozzles->values.front();
                if (!(diameter > 0) || !std::isfinite(diameter) ||
                    !std::all_of(nozzles->values.begin(), nozzles->values.end(), [diameter](double d) { return d == diameter; }))
                    continue;
                if (std::none_of(variants.begin(), variants.end(), [&](const Choice& entry) { return entry.variant == variant; }))
                    variants.push_back({wondermaker, std::string(model), variant, preset.name, diameter});
            }
        }
        std::sort(variants.begin(), variants.end(), [](const Choice& a, const Choice& b) { return a.diameter < b.diameter; });
        result.insert(result.end(), variants.begin(), variants.end());
    }
    return result;
}

Machine describe(const Preset& preset)
{
    Machine result;
    result.name = preset.name;
    if (const auto* nozzles = preset.config.option<ConfigOptionFloats>("nozzle_diameter"))
        result.diameters = nozzles->values;
    const auto* area = preset.config.option<ConfigOptionPoints>("printable_area");
    if (area && !area->values.empty()) {
        Vec2d minimum = area->values.front(), maximum = minimum;
        for (const auto& point : area->values) {
            minimum = minimum.cwiseMin(point);
            maximum = maximum.cwiseMax(point);
        }
        result.space.head<2>() = maximum - minimum;
    }
    if (const auto* height = preset.config.option<ConfigOptionFloat>("printable_height"))
        result.space.z() = height->value;
    return result;
}

std::vector<Reference> collect_references(const Model& model, const std::vector<Plate>& plates)
{
    std::vector<Reference> refs;
    const auto use = [&](size_t plate, int slot, Kind kind) {
        if (slot > 0)
            refs.push_back({plate, size_t(slot - 1), kind});
    };
    const auto use_config = [&](size_t plate, const auto& cfg, Kind kind) {
        if (cfg.has("extruder"))
            use(plate, cfg.opt_int("extruder"), kind);
        for (const char* key : {"support_filament", "support_interface_filament"})
            if (cfg.has(key))
                use(plate, cfg.opt_int(key), Kind::Support);
        if (cfg.has("wipe_tower_filament"))
            use(plate, cfg.opt_int("wipe_tower_filament"), Kind::WipeTower);
    };
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const auto& object = *model.objects[oi];
        std::vector<size_t> membership;
        for (size_t pi = 0; pi < plates.size(); ++pi)
            if (std::find(plates[pi].objects.begin(), plates[pi].objects.end(), oi) != plates[pi].objects.end())
                membership.push_back(pi);
        if (membership.empty())
            membership.push_back(0);
        for (size_t pi : membership) {
            use_config(pi, object.config, Kind::Object);
            for (const auto& entry : object.layer_config_ranges)
                use_config(pi, entry.second, Kind::Layer);
            for (const auto* volume : object.volumes) {
                use_config(pi, volume->config, Kind::Part);
                for (const int slot : volume->get_extruders())
                    use(pi, slot, volume->is_mm_painted() ? Kind::Paint : Kind::Part);
            }
        }
    }
    for (size_t pi = 0; pi < plates.size(); ++pi)
        if (plates[pi].config && !plates[pi].objects.empty())
            use_config(pi, *plates[pi].config, Kind::Object);
    for (const auto& [plate, data] : model.plates_custom_gcodes)
        for (const auto& entry : data.gcodes)
            if (entry.type == CustomGCode::Type::ToolChange || entry.type == CustomGCode::Type::ColorChange)
                use(size_t(plate), entry.extruder, Kind::ToolChange);
            else if (entry.type == CustomGCode::Type::Custom || entry.type == CustomGCode::Type::Template)
                refs.push_back({size_t(plate), 0, Kind::Custom});
    std::sort(refs.begin(), refs.end(), [](const Reference& a, const Reference& b) {
        return std::tie(a.plate, a.slot, a.kind) < std::tie(b.plate, b.slot, b.kind);
    });
    refs.erase(std::unique(refs.begin(), refs.end(), [](const Reference& a, const Reference& b) {
        return std::tie(a.plate, a.slot, a.kind) == std::tie(b.plate, b.slot, b.kind);
    }), refs.end());
    return refs;
}

Plan prepare(const PresetBundle& live, const AppConfig& config, const Choice& target,
             const Model& model, const std::vector<Plate>& plates, const PresetBundle* additional)
{
    Plan plan;
    plan.target = target;
    try {
        const auto catalog = choices(live, additional);
        if (std::find(catalog.begin(), catalog.end(), target) == catalog.end())
            throw std::runtime_error("The exact printer variant is unavailable.");
        auto& staged = *(plan.staged = std::make_unique<PresetBundle>(live));
        auto& candidate = *(plan.persistence = std::make_unique<AppConfig>(config));
        const Preset& original = live.printers.get_edited_preset();
        // Embedded presets belong to their 3MF, never the global last-used machine list.
        if (original.printer_technology() == ptFFF && !original.is_project_embedded && !original.is_external) {
            staged.extruder_ams_counts = live.extruder_ams_counts;
            staged.export_selections(candidate, true);
        }
        if (!has_local(live, target)) {
            if (!additional || live.vendors.count(target.vendor) || exact(live.printers, target.preset))
                throw std::runtime_error("The installed printer profile conflicts with a shipped variant.");
            staged.merge_presets(PresetBundle(*additional));
        }
        plan.remembered = candidate.has_printer_setting(target.preset, PRESET_FILAMENT_NAME);
        std::vector<std::string> previous_materials;
        if (plan.remembered)
            for (size_t i = 0; i < 1000; ++i) {
                const auto key = i == 0 ? std::string(PRESET_FILAMENT_NAME) : "filament_" + std::string(i < 10 ? "0" : "") + std::to_string(i);
                const auto name = candidate.get_printer_setting(target.preset, key);
                if (name.empty()) break;
                previous_materials.push_back(name);
            }
        candidate.set_variant(target.vendor, target.model, target.variant, true);
        candidate.set("presets", PRESET_PRINTER_NAME, target.preset);
        staged.project_config = PresetBundle().project_config;
        staged.extruder_ams_counts.clear();
        staged.load_selections(candidate);
        if (staged.printers.get_selected_preset_name() != target.preset)
            throw std::runtime_error("Could not activate the exact printer variant.");
        plan.machine = describe(staged.printers.get_edited_preset());
        plan.materials = summarize(staged);
        if (plan.materials.empty() || std::none_of(plan.materials.begin(), plan.materials.end(), [](const Material& m) { return !m.mixed && m.valid; }))
            throw std::runtime_error("No compatible default or saved filament is available.");
        for (size_t i = 0; i < previous_materials.size(); ++i)
            if (i >= staged.filament_presets.size() || previous_materials[i] != staged.filament_presets[i])
                plan.replaced_saved_slots.push_back(i);

        auto refs = collect_references(model, plates);
        if (!model.objects.empty()) {
            // Inherited object and support assignments may come from the print preset.
            const auto effective = live.full_config();
            for (size_t pi = 0; pi < std::max<size_t>(plates.size(), 1); ++pi) {
                if (pi < plates.size() && plates[pi].objects.empty()) continue;
                for (const char* key : {"support_filament", "support_interface_filament", "wipe_tower_filament"})
                    if (const auto* option = effective.option<ConfigOptionInt>(key); option && option->value > 0)
                        refs.push_back({pi, size_t(option->value - 1), std::string_view(key) == "wipe_tower_filament" ? Kind::WipeTower : Kind::Support});
            }
        }
        const auto current = summarize(live);
        const auto explicit_refs = refs;
        for (const auto& ref : explicit_refs) {
            if (ref.kind == Kind::Custom || ref.slot >= current.size() || !current[ref.slot].mixed)
                continue;
            for (const unsigned slot : parse_mixed_components(live.project_config.opt_string("filament_mixed_components", ref.slot)))
                if (slot > 0)
                    refs.push_back({ref.plate, size_t(slot - 1), ref.kind});
        }
        std::sort(refs.begin(), refs.end(), [](const Reference& a, const Reference& b) {
            return std::tie(a.plate, a.slot, a.kind) < std::tie(b.plate, b.slot, b.kind);
        });
        refs.erase(std::unique(refs.begin(), refs.end(), [](const Reference& a, const Reference& b) {
            return std::tie(a.plate, a.slot, a.kind) == std::tie(b.plate, b.slot, b.kind);
        }), refs.end());
        const auto remembered = config.printer_settings().find(target.preset);
        Json fingerprint = {selection_key(live), selection_key(staged), plan.replaced_saved_slots,
            remembered == config.printer_settings().end() ? Json() : Json(remembered->second)};
        Json project = Json::array();
        for (const auto* object : model.objects) {
            Json parts = {object->id().id, static_cast<const ObjectBase&>(object->config).timestamp(), object->instances.size()};
            for (const auto* volume : object->volumes)
                parts.push_back({volume->id().id, static_cast<const ObjectBase&>(volume->config).timestamp(), volume->mmu_segmentation_facets.timestamp()});
            project.push_back(std::move(parts));
        }
        for (const auto& plate : plates)
            project.push_back({plate.objects, plate.config ? config_key(*plate.config) : Json()});
        fingerprint.push_back(std::move(project));
        for (const auto& ref : refs) {
            fingerprint.push_back({ref.plate, ref.slot, int(ref.kind)});
            if (ref.kind == Kind::Custom) {
                plan.impacts.push_back({ref, true, {}, {}});
                continue;
            }
            const auto* before = ref.slot < current.size() ? &current[ref.slot] : nullptr;
            const auto* after = ref.slot < plan.materials.size() ? &plan.materials[ref.slot] : nullptr;
            const bool blocked = !after || !after->valid || (before && before->mixed != after->mixed);
            if (blocked || (before && material_key(*before) != material_key(*after)))
                plan.impacts.push_back({ref, blocked, before ? label(*before) : "", after ? label(*after) : ""});
        }
        plan.fingerprint = fingerprint.dump();
        // Both entries are written on the copy. Source and target stay separate even
        // when switching diameter variants of the same machine.
        staged.export_selections(candidate, true);
    } catch (const std::exception& error) {
        plan.error = error.what();
    }
    return plan;
}

bool blocks(const Plan& plan)
{
    return !plan.error.empty() || std::any_of(plan.impacts.begin(), plan.impacts.end(), [](const Impact& impact) { return impact.blocking; });
}

bool save_checked(AppConfig& config)
{
    config.set_dirty();
    config.save();
    if (config.dirty())
        return false;
    AppConfig persisted;
    persisted.set_loading_path(config.config_path());
    if (!persisted.load(true).empty())
        return false;
    const auto name = config.get("presets", PRESET_PRINTER_NAME);
    return persisted.get("presets", PRESET_PRINTER_NAME) == name &&
        persisted.printer_settings() == config.printer_settings() && persisted.vendors() == config.vendors();
}

Result apply(PresetBundle& live, AppConfig& config, const Plan& reviewed, const Model& model,
             const std::vector<Plate>& plates, const PresetBundle* additional, Save save, Publish before_publish)
{
    if (reviewed.target.preset == live.printers.get_selected_preset_name())
        return Result::NoChange;
    if (blocks(reviewed))
        return Result::Blocked;
    auto fresh = prepare(live, config, reviewed.target, model, plates, additional);
    if (fresh.fingerprint != reviewed.fingerprint || blocks(fresh))
        return Result::Stale;
    const auto before = live.selection(true);
    const auto after = fresh.staged->selection(true);
    const bool install = !has_local(live, reviewed.target);
    VendorFiles files(install ? additional : nullptr, live);
    const bool checked_save = !save;
    if (!save) save = save_checked;
    bool persistence_started = false;
    bool live_changed = false;
    try {
        files.publish();
        persistence_started = true;
        if (!save(*fresh.persistence)) {
            // A dirty checked copy means the native atomic replacement failed before
            // the previous file was replaced. Its retry state is local to the copy.
            if (checked_save && !fresh.persistence->dirty()) {
                AppConfig rollback = config;
                return save(rollback) ? Result::SaveFailed : Result::RestoreFailed;
            }
            return Result::SaveFailed;
        }
        if (before_publish)
            before_publish(*fresh.staged);
        PresetBundle::SelectionExportBlocker block_export(live);
        live_changed = true;
        if (install) {
            const auto calibrating = live.calibrate_printer ? live.calibrate_printer->name : std::string();
            std::vector<std::string> calibrating_filaments;
            for (const auto* preset : live.calibrate_filaments)
                calibrating_filaments.push_back(preset->name);
            live.merge_presets(PresetBundle(*additional));
            live.load_installed_printers(*fresh.persistence); // Also refreshes the native name and alias maps.
            live.calibrate_printer = calibrating.empty() ? nullptr : exact(live.printers, calibrating);
            live.calibrate_filaments.clear();
            for (const auto& name : calibrating_filaments)
                if (const auto* preset = exact(live.filaments, name))
                    live.calibrate_filaments.insert(preset);
        }
        live.restore_selection(*after, true);
        config = std::move(*fresh.persistence);
        files.keep();
        return Result::Applied;
    } catch (const std::exception&) {
        try {
            if (live_changed) live.restore_selection(*before, true);
            AppConfig rollback = config;
            if (persistence_started && !save(rollback)) return Result::RestoreFailed;
        } catch (const std::exception&) {
            return Result::RestoreFailed;
        }
        return Result::SaveFailed;
    }
}

} // namespace Slic3r::GUI::PrinterSettings
