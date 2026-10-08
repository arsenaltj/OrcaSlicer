#include "slic3r/GUI/Redesign/StartupSetupService.hpp"
#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "../test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>
#include <nlohmann/json.hpp>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

std::vector<StartupSetupPrinter> simple_catalog()
{
    const std::vector<StartupSetupFilament> common = {
        {"WonderMaker PLA Basic", "PLA Basic", "PLA", "#FFFFFF"},
        {"WonderMaker PETG Basic", "PETG Basic", "PETG", "#FFFFFF"}
    };
    return {
        {"WonderMaker ZR", "ZR", "ZR 0.4", "", 1, common, ""},
        {"WonderMaker ZR Ultra", "ZR Ultra", "Ultra 0.4", "", 4, common, ""},
        {"WonderMaker ZR Ultra S", "ZR Ultra S", "Ultra S 0.4", "", 4,
            {{"WonderMaker PLA Basic", "PLA Basic", "PLA", "#FFFFFF"}}, ""}
    };
}

void ready(StartupSetupService& service, std::vector<StartupSetupPrinter> catalog = simple_catalog())
{
    const auto revision = service.begin_load();
    REQUIRE(service.finish_load(revision, std::move(catalog)));
    REQUIRE(service.next());
    REQUIRE(service.next());
}

struct SetupData {
    ScopedTemporaryDir directory{"orca-startup-setup"};
    std::string old_resources = resources_dir();
    std::string old_data = data_dir();
    std::string source_resources = (fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string();

    SetupData()
    {
        save_main_thread_id();
        set_resources_dir(source_resources);
        set_data_dir(directory.string());
    }

    ~SetupData()
    {
        set_resources_dir(old_resources);
        set_data_dir(old_data);
    }

    std::vector<StartupSetupPrinter> catalog() const
    {
        const std::atomic<bool> cancel{false};
        return StartupSetupService::load_catalog(source_resources, directory.string(), cancel);
    }
};

}

TEST_CASE("First setup skips an existing valid printer and recovers a missing one", "[UiRedesign][StartupSetup]")
{
    CHECK_FALSE(StartupSetupService::needs_setup(true, true));
    CHECK(StartupSetupService::needs_setup(false, false));
    CHECK(StartupSetupService::needs_setup(true, false));
    CHECK(StartupSetupService::needs_setup(false, true));
}

TEST_CASE("Startup regions retain existing values and default empty configurations to China", "[UiRedesign][StartupSetup]")
{
    const auto value = GENERATE(std::string("China"), std::string("CHN"), std::string(""),
                               std::string("Asia-Pacific"), std::string("Europe"),
                               std::string("North America"), std::string("USA"));
    const auto region = StartupSetupService::region_from_config(value);
    const auto saved = StartupSetupService::region_config_value(region);
    if (value == "CHN" || value.empty()) CHECK(saved == "China");
    else if (value == "USA") CHECK(saved == "North America");
    else CHECK(saved == value);
    AppConfig config;
    config.set("region", value);
    StartupSetupService service(config);
    CHECK(service.snapshot().draft.region == region);
    service.choose_region(StartupSetupRegion::Europe);
    CHECK(config.get("region") == value);
}

TEST_CASE("Printer selection is single and back navigation keeps every head draft", "[UiRedesign][StartupSetup]")
{
    AppConfig config;
    StartupSetupService service(config);
    ready(service);
    CHECK_FALSE(service.snapshot().can_next);
    CHECK_FALSE(service.choose_printer("Another vendor"));
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra"));
    CHECK(service.snapshot().draft.heads.size() == 4);
    REQUIRE(service.next());
    REQUIRE(service.choose_head(2));
    REQUIRE(service.choose_filament("WonderMaker PETG Basic"));
    REQUIRE(service.choose_colour("#123456"));
    REQUIRE(service.back());
    CHECK(service.snapshot().page == StartupSetupPage::Printers);
    CHECK(service.snapshot().draft.printer_model == "WonderMaker ZR Ultra");
    REQUIRE(service.next());
    CHECK(service.snapshot().active_head == 2);
    CHECK(service.snapshot().draft.heads[2].filament == "WonderMaker PETG Basic");
    CHECK(service.snapshot().draft.heads[2].colour == "#123456");
    CHECK(service.snapshot().draft.heads[0].filament == "WonderMaker PLA Basic");
    CHECK(service.snapshot().draft.heads[0].colour == "#FFFFFF");
    CHECK(config.get("firstguide", "finish").empty());
}

TEST_CASE("Changing printers keeps compatible materials and replaces incompatible selections", "[UiRedesign][StartupSetup]")
{
    AppConfig config;
    StartupSetupService service(config);
    ready(service);
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra"));
    REQUIRE(service.choose_head(0));
    REQUIRE(service.choose_colour("#123456"));
    REQUIRE(service.choose_head(1));
    REQUIRE(service.choose_filament("WonderMaker PETG Basic"));
    REQUIRE(service.choose_colour("#ABCDEF"));
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra S"));
    CHECK(service.snapshot().draft.heads[0].colour == "#123456");
    CHECK(service.snapshot().draft.heads[1].filament == "WonderMaker PLA Basic");
    CHECK(service.snapshot().draft.heads[1].colour == "#ABCDEF");
    REQUIRE(service.choose_printer("WonderMaker ZR"));
    CHECK(service.snapshot().draft.heads.size() == 1);
    CHECK(service.snapshot().active_head == 0);
    CHECK(service.snapshot().draft.heads[0].colour == "#123456");
}

TEST_CASE("Missing compatible profiles disable completion and can be reloaded", "[UiRedesign][StartupSetup]")
{
    AppConfig config;
    StartupSetupService service(config);
    auto catalog = simple_catalog();
    catalog[1].filaments.clear();
    catalog[1].error = "No compatible filament presets.";
    ready(service, std::move(catalog));
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra"));
    REQUIRE(service.next());
    CHECK_FALSE(service.snapshot().can_finish);
    CHECK_FALSE(service.choose_filament("Invented PLA"));
    CHECK_FALSE(service.choose_colour("red"));
    const auto revision = service.begin_load();
    CHECK(service.snapshot().loading);
    CHECK_FALSE(service.snapshot().can_finish);
    REQUIRE(service.finish_load(revision, simple_catalog()));
    CHECK(service.snapshot().can_finish);
}

TEST_CASE("Cancelled setup discards selections and rejects late catalog callbacks", "[UiRedesign][StartupSetup]")
{
    AppConfig config;
    config.set("region", std::string("Europe"));
    StartupSetupService service(config);
    ready(service);
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra"));
    service.set_custom_colours({"#123456"});
    const auto revision = service.begin_load();
    service.cancel();
    CHECK(service.snapshot().cancelled);
    CHECK(service.snapshot().draft.printer_model.empty());
    CHECK(service.snapshot().draft.heads.empty());
    CHECK_FALSE(service.finish_load(revision, simple_catalog()));
    CHECK_FALSE(service.next());
    CHECK(config.get("region") == "Europe");
    CHECK(config.get("firstguide", "finish").empty());
    PresetBundle bundle;
    CHECK_FALSE(service.complete(config, bundle));
}

TEST_CASE("Catalog reload rejects results from an earlier session revision", "[UiRedesign][StartupSetup]")
{
    AppConfig config;
    StartupSetupService service(config);
    const auto old_revision = service.begin_load();
    const auto current_revision = service.begin_load();
    CHECK_FALSE(service.finish_load(old_revision, simple_catalog(), "Stale error"));
    CHECK(service.snapshot().loading);
    REQUIRE(service.finish_load(current_revision, simple_catalog()));
    CHECK_FALSE(service.snapshot().loading);
    CHECK(service.snapshot().error.empty());
}

TEST_CASE("The offline startup catalog contains exactly three 0.4 mm WonderMaker models", "[UiRedesign][StartupSetup]")
{
    SetupData data;
    const auto catalog = data.catalog();
    REQUIRE(catalog.size() == 3);
    CHECK(catalog[0].model == "WonderMaker ZR");
    CHECK(catalog[1].model == "WonderMaker ZR Ultra");
    CHECK(catalog[2].model == "WonderMaker ZR Ultra S");
    CHECK(catalog[0].head_count == 1);
    CHECK(catalog[1].head_count == 4);
    CHECK(catalog[2].head_count == 4);
    for (const auto& printer : catalog) {
        INFO(printer.model);
        CHECK(printer.error.empty());
        REQUIRE_FALSE(printer.filaments.empty());
        CHECK(printer.filaments.front().preset == "WonderMaker PLA Basic");
        CHECK(fs::exists(printer.image_path));
    }
    const std::atomic<bool> cancelled{true};
    CHECK(StartupSetupService::load_catalog(data.source_resources, data.directory.string(), cancelled).empty());
}

TEST_CASE("Startup setup persists each physical head and restores it on restart", "[UiRedesign][StartupSetup]")
{
    const auto model = GENERATE(size_t(0), size_t(1), size_t(2));
    SetupData data;
    const auto catalog = data.catalog();
    REQUIRE(catalog.size() == 3);
    REQUIRE(catalog[model].error.empty());
    AppConfig config;
    config.set("unrelated_user_setting", std::string("keep"));
    config.set_variant("OtherVendor", "Existing model", "0.6", "true");
    StartupSetupService service(config);
    ready(service, catalog);
    REQUIRE(service.choose_printer(catalog[model].model));
    REQUIRE(service.next());
    service.choose_region(StartupSetupRegion::Europe);
    const std::vector<std::string> head_colours = {"#123456", "#ABCDEF", "#654321", "#FEDCBA"};
    std::vector<std::string> materials;
    for (size_t index = 0; index < catalog[model].head_count; ++index) {
        REQUIRE(service.choose_head(index));
        const auto& material = catalog[model].filaments[index % catalog[model].filaments.size()].preset;
        materials.push_back(material);
        REQUIRE(service.choose_filament(material));
        REQUIRE(service.choose_colour(head_colours[index]));
    }
    service.set_custom_colours({"#010203"});
    PresetBundle bundle;
    bundle.setup_directories();
    const bool completed = service.complete(config, bundle);
    INFO(catalog[model].model);
    INFO(service.snapshot().error);
    REQUIRE(completed);
    CHECK(service.snapshot().completed);
    CHECK_FALSE(service.complete(config, bundle));
    CHECK(config.get("firstguide", "finish") == "1");
    CHECK(config.get("region") == "Europe");
    CHECK(config.get("unrelated_user_setting") == "keep");
    CHECK(config.get_variant("OtherVendor", "Existing model", "0.6"));
    CHECK(bundle.printers.get_selected_preset_name() == catalog[model].preset);
    CHECK(bundle.filament_presets == materials);
    for (const auto* collection : std::array<const PresetCollection*, 3>{
             &bundle.printers, &bundle.prints, &bundle.filaments}) {
        const auto* vendor = collection->get_edited_preset().vendor;
        REQUIRE(vendor != nullptr);
        CHECK(std::any_of(bundle.vendors.begin(), bundle.vendors.end(),
                         [vendor](const auto& entry) { return &entry.second == vendor; }));
    }
    AppConfig restored;
    REQUIRE(restored.load().empty());
    CHECK(restored.get("firstguide", "finish") == "1");
    PresetBundle restarted;
    restarted.setup_directories();
    restarted.load_presets(restored, ForwardCompatibilitySubstitutionRule::EnableSilent);
    CHECK(restarted.printers.get_selected_preset_name() == catalog[model].preset);
    CHECK(restarted.filament_presets == materials);
    const std::vector<std::string> expected_colours(head_colours.begin(), head_colours.begin() + catalog[model].head_count);
    CHECK(restarted.project_config.option<ConfigOptionStrings>("filament_colour")->values == expected_colours);
    CHECK(restored.get_custom_color_from_config() == std::vector<std::string>{"#010203"});
    CHECK_FALSE(StartupSetupService::needs_setup(true, !restarted.printers.only_default_printers()));
}

TEST_CASE("A failed configuration save preserves the live selections and permits recovery", "[UiRedesign][StartupSetup]")
{
    SetupData data;
    AppConfig config;
    config.set("region", std::string("North America"));
    config.set("presets", PRESET_PRINTER_NAME, std::string("Existing printer"));
    const auto previous_vendors = config.vendors();
    PresetBundle bundle;
    bundle.setup_directories();
    const auto previous_printer = bundle.printers.get_selected_preset_name();
    const auto previous_filaments = bundle.filament_presets;
    StartupSetupService service(config);
    ready(service, data.catalog());
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra"));
    REQUIRE(service.next());
    service.choose_region(StartupSetupRegion::Europe);
    int writes = 0;
    CHECK_FALSE(service.complete(config, bundle, [&](AppConfig&) {
        ++writes;
        CHECK_FALSE(service.complete(config, bundle));
        throw std::runtime_error("Disk is not writable.");
    }));
    CHECK(writes == 1);
    CHECK(config.get("region") == "North America");
    CHECK(config.get("presets", PRESET_PRINTER_NAME) == "Existing printer");
    CHECK(config.vendors() == previous_vendors);
    CHECK(config.get("firstguide", "finish").empty());
    CHECK(bundle.printers.get_selected_preset_name() == previous_printer);
    CHECK(bundle.filament_presets == previous_filaments);
    CHECK(service.snapshot().error == "Disk is not writable.");
    CHECK_FALSE(service.snapshot().can_finish);
    REQUIRE(service.back());
    REQUIRE(service.next());
    REQUIRE(service.complete(config, bundle));
    CHECK(config.get("firstguide", "finish") == "1");
}

TEST_CASE("A changed nozzle profile rejects a stale setup draft and recovers after reload", "[UiRedesign][StartupSetup]")
{
    SetupData data;
    AppConfig config;
    StartupSetupService service(config);
    ready(service, data.catalog());
    REQUIRE(service.choose_printer("WonderMaker ZR Ultra S"));
    REQUIRE(service.next());
    PresetBundle bundle;
    bundle.setup_directories();
    const auto previous_printer = bundle.printers.get_selected_preset_name();
    REQUIRE(install_vendor_bundles_from_resources({"WonderMaker"}));
    const auto path = fs::path(data.directory.string()) / PRESET_SYSTEM_DIR / "WonderMaker" /
                      "machine" / "WonderMaker ZR Ultra S 0.4 nozzle.json";
    nlohmann::json profile;
    {
        std::ifstream input(path.string());
        REQUIRE(input.good());
        input >> profile;
    }
    const auto original_nozzles = profile["nozzle_diameter"];
    profile["nozzle_diameter"] = {"0.6", "0.6", "0.6", "0.6"};
    {
        std::ofstream output(path.string());
        output << profile.dump(2);
        REQUIRE(output.good());
    }
    int writes = 0;
    CHECK_FALSE(service.complete(config, bundle, [&](AppConfig&) { ++writes; }));
    CHECK(writes == 0);
    CHECK(config.get("firstguide", "finish").empty());
    CHECK(bundle.printers.get_selected_preset_name() == previous_printer);
    CHECK_FALSE(service.snapshot().can_finish);
    CHECK(service.snapshot().error.find("changed") != std::string::npos);
    profile["nozzle_diameter"] = original_nozzles;
    {
        std::ofstream output(path.string());
        output << profile.dump(2);
        REQUIRE(output.good());
    }
    // Installed caches are versioned; restoring raw JSON also invalidates its cache.
    fs::remove(fs::path(data.directory.string()) / PRESET_SYSTEM_DIR / "WonderMaker.opc");
    REQUIRE(service.finish_load(service.begin_load(), data.catalog()));
    REQUIRE(service.snapshot().can_finish);
    REQUIRE(service.complete(config, bundle));
    CHECK(config.get("firstguide", "finish") == "1");
}
