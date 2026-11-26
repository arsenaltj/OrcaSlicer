#include "slic3r/GUI/Redesign/PrinterSettingsService.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyCandidateSnapshot.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "../test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::PrinterSettings;
namespace fs = boost::filesystem;

namespace {

struct PrinterFixture {
    ScopedTemporaryDir data{"orca-printer-settings"};
    std::string original_data = data_dir(), original_resources = resources_dir();
    PresetBundle bundle;
    AppConfig config;
    Model model;
    Choice ultra, honor;

    explicit PrinterFixture(bool use_ultra = true)
    {
        save_main_thread_id();
        set_data_dir(data.string());
        set_resources_dir((fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string());
        auto resources = read_missing_catalog(bundle);
        REQUIRE(resources);
        const auto variants = choices(bundle, resources.get());
        REQUIRE(variants.size() == 13);
        auto find = [&](const char* model) {
            const auto it = std::find_if(variants.begin(), variants.end(), [&](const auto& variant) {
                return variant.model == model && variant.diameter == 0.4;
            });
            REQUIRE(it != variants.end());
            return *it;
        };
        ultra = find("WonderMaker ZR Ultra");
        honor = find("Honor Forge");
        if (use_ultra) {
            bundle.merge_presets(std::move(*resources));
            config.set_variant(ultra.vendor, ultra.model, ultra.variant, true);
            config.set("presets", PRESET_PRINTER_NAME, ultra.preset);
            config.set_bool("remember_printer_config", false);
            bundle.load_selections(config);
            REQUIRE(bundle.printers.get_selected_preset_name() == ultra.preset);
        }
    }

    ~PrinterFixture()
    {
        set_data_dir(original_data);
        set_resources_dir(original_resources);
    }

    void memory(const Choice& choice, size_t count, const std::string& color = "#102030")
    {
        const auto material = bundle.filament_presets.front();
        config.set_printer_setting(choice.preset, PRESET_PRINTER_NAME, choice.preset);
        config.set_printer_setting(choice.preset, PRESET_FILAMENT_NAME, material);
        std::string colors;
        for (size_t i = 0; i < count; ++i) {
            if (i) config.set_printer_setting(choice.preset, "filament_0" + std::to_string(i), material);
            colors += (i ? "," : "") + color;
        }
        config.set_printer_setting(choice.preset, "filament_colors", colors);
    }

    Plan plan(const Choice& choice, const std::vector<Plate>& plates = {})
    { return prepare(bundle, config, choice, model, plates); }

    Result switch_to(const Choice& choice)
    {
        const auto reviewed = plan(choice);
        INFO(reviewed.error);
        REQUIRE_FALSE(blocks(reviewed));
        return PrinterSettings::apply(bundle, config, reviewed, model, {});
    }
};

} // namespace

TEST_CASE("Printer settings read all actual shipped variants without activating them", "[PrinterSettings]")
{
    PrinterFixture f;
    const auto original = f.config.vendors();
    const auto variants = choices(f.bundle);
    CHECK(variants.size() == 13);
    CHECK(describe(*f.bundle.printers.find_preset(f.ultra.preset, false)).diameters.size() == 4);
    CHECK(describe(*f.bundle.printers.find_preset(f.honor.preset, false)).diameters.size() == 6);
    const auto preview = f.plan(f.honor);
    INFO(preview.error);
    REQUIRE_FALSE(blocks(preview));
    CHECK_FALSE(preview.remembered);
    CHECK(preview.machine.space.z() > 0);
    CHECK(f.config.vendors() == original);
    CHECK_FALSE(fs::exists(f.config.config_path()));
    CHECK(f.bundle.printers.get_selected_preset_name() == f.ultra.preset);
    CHECK(PrinterSettings::apply(f.bundle, f.config, f.plan(f.ultra), f.model, {}) == Result::NoChange);
}

TEST_CASE("Each printer restores its own configuration with native remember preference disabled", "[PrinterSettings]")
{
    PrinterFixture f;
    REQUIRE_FALSE(f.bundle.filament_presets.empty());
    f.bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.front() = "#131313";
    f.memory(f.honor, 4);
    const auto target = f.plan(f.honor);
    INFO(target.error);
    REQUIRE_FALSE(blocks(target));
    CHECK(target.remembered);
    CHECK(target.materials.size() == 4);
    CHECK(target.materials.front().color == "#102030");
    REQUIRE(f.switch_to(f.honor) == Result::Applied);
    CHECK(f.bundle.filament_presets.size() == 4);
    REQUIRE(f.switch_to(f.ultra) == Result::Applied);
    CHECK(f.bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.front() == "#131313");
    REQUIRE(f.switch_to(f.honor) == Result::Applied);
    CHECK(f.bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.front() == "#102030");
    CHECK_FALSE(f.config.get_bool("remember_printer_config"));
    AppConfig startup_config;
    REQUIRE(startup_config.load(true).empty());
    PresetBundle restarted(f.bundle);
    restarted.load_selections(startup_config);
    CHECK(restarted.printers.get_selected_preset_name() == f.honor.preset);
    CHECK(restarted.filament_presets == f.bundle.filament_presets);
}

TEST_CASE("A printer switch protects actual slots on all plates and ignores unused swatches", "[PrinterSettings]")
{
    PrinterFixture f;
    f.memory(f.honor, 6);
    REQUIRE(f.switch_to(f.honor) == Result::Applied);
    f.memory(f.ultra, 4);
    auto* model = f.model.add_object("box", "", make_cube(10., 10., 10.));
    auto* volume = model->volumes.front();
    const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6);
    switch (variant) {
    case 0: model->config.set_key_value("extruder", new ConfigOptionInt(5)); break;
    case 1: volume->config.set_key_value("extruder", new ConfigOptionInt(6)); break;
    case 2: {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::Extruder6);
        volume->mmu_segmentation_facets.set(selector);
        auto paint = volume->mmu_segmentation_facets.get_data();
        paint.reset_used_states();
        volume->mmu_segmentation_facets.set_data(std::move(paint));
        break;
    }
    case 3: f.model.plates_custom_gcodes[1].gcodes.push_back({1., CustomGCode::ToolChange, 6, {}, {}}); break;
    case 5: model->layer_config_ranges[{0., 1.}].set("extruder", 6); break;
    case 6: model->config.set("support_interface_filament", 5); break;
    default: model->config.set("extruder", 4); break;
    }
    const auto frozen = f.bundle.project_config;
    const std::vector<Plate> plates {{nullptr, {}}, {nullptr, {0}}};
    const auto plan = f.plan(f.ultra, plates);
    INFO(plan.error);
    if (variant == 4) {
        REQUIRE_FALSE(blocks(plan));
    } else {
        REQUIRE(blocks(plan));
        CHECK(std::any_of(plan.impacts.begin(), plan.impacts.end(), [](const Impact& impact) {
            return impact.blocking && impact.reference.plate == 1 && impact.reference.slot >= 4;
        }));
        CHECK(PrinterSettings::apply(f.bundle, f.config, plan, f.model, plates) == Result::Blocked);
    }
    CHECK(f.bundle.project_config == frozen);
    CHECK(f.bundle.printers.get_selected_preset_name() == f.honor.preset);
}

TEST_CASE("Printer confirmation expires when project or target configuration changes", "[PrinterSettings]")
{
    PrinterFixture f;
    const auto reviewed = f.plan(f.honor);
    INFO(reviewed.error);
    REQUIRE_FALSE(blocks(reviewed));
    f.memory(f.honor, 1, "#DDCCBB");
    bool wrote = false;
    CHECK(PrinterSettings::apply(f.bundle, f.config, reviewed, f.model, {}, nullptr,
        [&](AppConfig&) { wrote = true; return true; }) == Result::Stale);
    CHECK_FALSE(wrote);
    CHECK(f.bundle.printers.get_selected_preset_name() == f.ultra.preset);
}

TEST_CASE("Printer save or prepublication failures never publish the candidate", "[PrinterSettings]")
{
    PrinterFixture f;
    REQUIRE(save_checked(f.config));
    const auto reviewed = f.plan(f.honor);
    INFO(reviewed.error);
    REQUIRE_FALSE(blocks(reviewed));
    const auto original = f.config.printer_settings();
    const int phase = GENERATE(0, 1, 2);
    int saves = 0;
    bool publishing = false;
    const auto result = PrinterSettings::apply(f.bundle, f.config, reviewed, f.model, {}, nullptr,
        [&](AppConfig& config) { ++saves; return phase != 0 && (phase != 2 || saves == 1) && save_checked(config); },
        [&](const PresetBundle&) { publishing = true; throw std::runtime_error("injected publication failure"); });
    CHECK(result == (phase == 2 ? Result::RestoreFailed : Result::SaveFailed));
    CHECK(publishing == (phase != 0));
    CHECK(saves == (phase == 0 ? 1 : 2));
    CHECK(f.bundle.printers.get_selected_preset_name() == f.ultra.preset);
    CHECK(f.config.printer_settings() == original);
    if (phase != 2) {
        AppConfig restored;
        REQUIRE(restored.load(true).empty());
        CHECK(restored.get("presets", PRESET_PRINTER_NAME) == f.ultra.preset);
    }
}

TEST_CASE("A missing shipped vendor is installed only for a confirmed setting", "[PrinterSettings]")
{
    PrinterFixture f(false);
    const auto catalog = read_missing_catalog(f.bundle);
    REQUIRE(catalog);
    const auto reviewed = prepare(f.bundle, f.config, f.honor, f.model, {}, catalog.get());
    INFO(reviewed.error);
    REQUIRE_FALSE(blocks(reviewed));
    CHECK_FALSE(is_vendor_installed(f.honor.vendor));
    CHECK(PrinterSettings::apply(f.bundle, f.config, reviewed, f.model, {}, catalog.get(),
              [](AppConfig&) { return false; }) == Result::SaveFailed);
    CHECK_FALSE(is_vendor_installed(f.honor.vendor));
    REQUIRE(PrinterSettings::apply(f.bundle, f.config, reviewed, f.model, {}, catalog.get()) == Result::Applied);
    CHECK(is_vendor_installed(f.honor.vendor));
    CHECK(f.bundle.printers.get_selected_preset_name() == f.honor.preset);
}

TEST_CASE("Workbench undo can restore appearance across printers and material state on its source printer", "[PrinterSettings]")
{
    BeautyCandidateSnapshot undo;
    undo.project_printer = "WonderMaker ZR Ultra 0.4 nozzle";
    CHECK(undo.can_restore_project_config("Honor Forge 0.4 nozzle"));
    undo.project_config = std::make_shared<DynamicPrintConfig>();
    CHECK_FALSE(undo.can_restore_project_config("Honor Forge 0.4 nozzle"));
    CHECK(undo.can_restore_project_config(undo.project_printer));
}

TEST_CASE("Printer settings reject unavailable and damaged machine variants", "[PrinterSettings]")
{
    PrinterFixture f;
    const bool damaged_specification = GENERATE(false, true);
    auto target = f.honor;
    if (damaged_specification) {
        auto* preset = f.bundle.printers.find_preset(f.honor.preset, false);
        REQUIRE(preset);
        auto* diameters = preset->config.option<ConfigOptionFloats>("nozzle_diameter");
        REQUIRE(diameters);
        diameters->values.clear();
    } else {
        target.variant = "unavailable-variant";
    }

    const auto candidates = choices(f.bundle);
    CHECK(std::find(candidates.begin(), candidates.end(), target) == candidates.end());
    CHECK(std::find(candidates.begin(), candidates.end(), f.ultra) != candidates.end());
    const auto preview = f.plan(target);
    CHECK(blocks(preview));
    CHECK_FALSE(preview.error.empty());
    CHECK(f.bundle.printers.get_selected_preset_name() == f.ultra.preset);
    CHECK_FALSE(fs::exists(f.config.config_path()));
}

TEST_CASE("A conflicting local printer does not silently replace a shipped variant", "[PrinterSettings]")
{
    PrinterFixture f(false);
    const auto catalog = read_missing_catalog(f.bundle);
    REQUIRE(catalog);
    const auto* shipped = catalog->printers.find_preset(f.honor.preset, false);
    REQUIRE(shipped);
    const auto& local = f.bundle.printers.load_preset("", f.honor.preset, shipped->config, false);
    REQUIRE_FALSE(local.is_system);
    const auto selected = f.bundle.printers.get_selected_preset_name();
    const auto preview = prepare(f.bundle, f.config, f.honor, f.model, {}, catalog.get());
    CHECK(blocks(preview));
    CHECK_FALSE(preview.error.empty());
    CHECK(f.bundle.printers.get_selected_preset_name() == selected);
    CHECK_FALSE(fs::exists(f.config.config_path()));
}

TEST_CASE("Printer preview identifies compatible replacements for missing saved filament", "[PrinterSettings]")
{
    PrinterFixture f;
    f.memory(f.honor, 1);
    const auto missing = "Filament removed from the local preset library";
    f.config.set_printer_setting(f.honor.preset, PRESET_FILAMENT_NAME, missing);

    const auto preview = f.plan(f.honor);
    INFO(preview.error);
    REQUIRE_FALSE(blocks(preview));
    CHECK(preview.remembered);
    REQUIRE_FALSE(preview.materials.empty());
    CHECK(preview.materials.front().valid);
    CHECK(preview.materials.front().name != missing);
    REQUIRE_FALSE(preview.replaced_saved_slots.empty());
    CHECK(preview.replaced_saved_slots.front() == 0);
    CHECK(f.config.get_printer_setting(f.honor.preset, PRESET_FILAMENT_NAME) == missing);
    CHECK(f.bundle.printers.get_selected_preset_name() == f.ultra.preset);
}
