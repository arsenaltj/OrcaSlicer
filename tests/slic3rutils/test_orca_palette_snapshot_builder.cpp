#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Orca/OrcaPaletteSnapshotBuilder.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintPaletteSnapshot.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintRecipeApplication.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("capturing a staged native palette leaves the live bundle and physical identities unchanged",
          "[OrcaPaletteCapture]")
{
    PresetBundle live;live.set_num_filaments(3);
    live.project_config.option<ConfigOptionStrings>("filament_colour")->values={"#000000","#FFFFFF","#FF0000"};
    const auto identity=LocalPrintRecipeApplication::identity(live);
    const auto before=OrcaPrintPaletteSnapshot::capture(live);
    REQUIRE(before.physical_channels.size()==3);
    CHECK(before.project_colors==live.project_config.option<ConfigOptionStrings>("filament_colour")->values);
    PresetBundle staged(live);staged.set_num_filaments(4,"#808080");
    staged.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[3]=true;
    staged.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[3]="1,3";
    staged.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[3]="0.3,0.7";
    const auto after=OrcaPrintPaletteSnapshot::capture(staged);
    REQUIRE(after.mixed_recipes.size()==1);
    CHECK(after.mixed_recipes.front().uniform_color);
    staged.project_config.option<ConfigOptionBools>("filament_mixed_gradient")->values[3]=true;
    const auto gradient=OrcaPrintPaletteSnapshot::capture(staged);
    REQUIRE(gradient.mixed_recipes.size()==1);
    CHECK_FALSE(gradient.mixed_recipes.front().uniform_color);
    CHECK(gradient.mixed_recipes.front().existing_virtual_slot==after.mixed_recipes.front().existing_virtual_slot);
    REQUIRE(after.physical_channels.size()==before.physical_channels.size());
    REQUIRE(after.sublayer_materials.size()==before.sublayer_materials.size());
    for(size_t i=0;i<before.physical_channels.size();++i) {
        CHECK(after.physical_channels[i].slot==before.physical_channels[i].slot);
        CHECK(after.physical_channels[i].display_color==before.physical_channels[i].display_color);
        CHECK(after.physical_channels[i].compatible==before.physical_channels[i].compatible);
        CHECK(after.sublayer_materials[i].identity==before.sublayer_materials[i].identity);
    }
    CHECK(after.project_colors.size()==4);
    CHECK(after.material_fingerprint!=before.material_fingerprint);
    CHECK(after.process_fingerprint!=before.process_fingerprint);
    CHECK(after.sublayer_process.material_fingerprint==after.material_fingerprint);
    CHECK(after.sublayer_process.process_fingerprint==after.process_fingerprint);
    CHECK(LocalPrintRecipeApplication::identity(live)==identity);
    const auto repeated=OrcaPrintPaletteSnapshot::capture(live);
    CHECK(repeated.material_fingerprint==before.material_fingerprint);
    CHECK(repeated.process_fingerprint==before.process_fingerprint);
    CHECK(after.sublayer_process.z_resolution_mm==0.);
    CHECK(after.sublayer_process.surface_condition.empty());
    CHECK(after.sublayer_process.measurement_condition.empty());
}

TEST_CASE("native palette fingerprints track physical colors process edits and actual multi-nozzle routing",
          "[OrcaPaletteCapture]")
{
    PresetBundle bundle;bundle.set_num_filaments(3);
    SECTION("physical color changes only the material fingerprint") {
        const auto before=OrcaPrintPaletteSnapshot::capture(bundle);
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values[0]="#123456";
        const auto after=OrcaPrintPaletteSnapshot::capture(bundle);
        CHECK(after.material_fingerprint!=before.material_fingerprint);
        CHECK(after.process_fingerprint==before.process_fingerprint);
    }
    SECTION("process edit invalidates the process fingerprint") {
        const auto before=OrcaPrintPaletteSnapshot::capture(bundle);
        bundle.prints.get_edited_preset().config.set_key_value("layer_height",new ConfigOptionFloat(.25));
        const auto after=OrcaPrintPaletteSnapshot::capture(bundle);
        CHECK(after.material_fingerprint==before.material_fingerprint);
        CHECK(after.process_fingerprint!=before.process_fingerprint);
        REQUIRE(after.sublayer_process.layer_heights_mm.size()==1);
        CHECK_THAT(after.sublayer_process.layer_heights_mm[0],Catch::Matchers::WithinAbs(.25,1e-12));
    }
    SECTION("multi-nozzle routing is part of the process fingerprint") {
        auto& printer=bundle.printers.get_edited_preset().config;
        printer.set_key_value("nozzle_diameter",new ConfigOptionFloats({.4,.6}));
        PrintColorNozzleRouting routing;routing.mode=fmmManual;routing.filament_maps={1,1,1};
        const auto before=OrcaPrintPaletteSnapshot::capture(bundle,routing);
        routing.filament_maps[0]=2;
        const auto after=OrcaPrintPaletteSnapshot::capture(bundle,routing);
        CHECK(after.material_fingerprint==before.material_fingerprint);
        CHECK(after.process_fingerprint!=before.process_fingerprint);
    }
}

TEST_CASE("explicit live display colors retain their fingerprint semantics without fabricating material data",
          "[OrcaPaletteCapture]")
{
    const std::vector<std::string> colors={"#ff0000","#00ff00"};
    const auto snapshot=OrcaPrintPaletteSnapshot::capture(nullptr,colors);
    CHECK(snapshot.project_colors==colors);
    REQUIRE(snapshot.physical_channels.size()==2);
    CHECK(snapshot.physical_channels[0].display_color=="#FF0000");
    CHECK_FALSE(snapshot.material_metadata_complete);
    CHECK(snapshot.sublayer_materials.empty());
    CHECK(snapshot.material_fingerprint.size()==64);
    CHECK(snapshot.process_fingerprint.size()==64);
}

TEST_CASE("Orca palette snapshot preserves every supported physical cardinality",
          "[ModelGeneration][ColorIntent]")
{
    const std::vector<std::string> colors {
        "#000001", "#000002", "#000003", "#000004", "#000005", "#000006",
    };
    for (size_t count = 1; count <= AI::kMaxPhysicalColorChannels; ++count) {
        std::vector<OrcaPaletteSlotCapability> slots;
        for (size_t slot = 0; slot < count; ++slot)
            slots.push_back({slot, colors[slot], "PLA", false, true, {}});

        const AI::PrintablePaletteSnapshot snapshot = build_orca_palette_snapshot(slots);
        INFO("physical channel count: " << count);
        CHECK(snapshot.physical_channels.size() == count);
        CHECK(snapshot.valid_slots.size() == count);
        CHECK(snapshot.compatible_slots.size() == count);
    }
}

TEST_CASE("Orca palette snapshot keeps six physical channels and separates virtual recipes",
          "[ModelGeneration][ColorIntent]")
{
    std::vector<OrcaPaletteSlotCapability> slots {
        {0, "#000001", "PLA", false, true, {}},
        {1, "#000002", "PLA", false, true, {}},
        {2, "#000003", "PLA", false, true, {}},
        {3, "#000004", "PLA", false, true, {}},
        {4, "#000005", "PLA", false, true, {}},
        {5, "#000006", "PLA", false, true, {}},
        {6, "#778899", "PLA", true, true, {{0, 0.25}, {5, 0.75}}},
        {7, "#000007", "PLA", false, true, {}},
        {8, "#AABBCC", "PLA", true, true, {{0, 0.5}, {7, 0.5}}},
    };

    const AI::PrintablePaletteSnapshot snapshot = build_orca_palette_snapshot(slots);

    REQUIRE(snapshot.physical_channels.size() == 6);
    CHECK(snapshot.physical_channels.front().slot == 0);
    CHECK(snapshot.physical_channels.back().slot == 5);
    CHECK(snapshot.valid_slots == std::vector<size_t> {0, 1, 2, 3, 4, 5});
    CHECK(snapshot.compatible_slots == snapshot.valid_slots);
    CHECK(snapshot.supports(AI::ColorOutputMode::DiscreteFilament));
    CHECK(snapshot.supports(AI::ColorOutputMode::ProcessMix));

    REQUIRE(snapshot.mixed_recipes.size() == 1);
    CHECK(snapshot.mixed_recipes.front().existing_virtual_slot == 6);
    CHECK(snapshot.mixed_recipes.front().components.size() == 2);
}

TEST_CASE("Orca palette snapshot excludes incompatible components from process recipes",
          "[ModelGeneration][ColorIntent]")
{
    const std::vector<OrcaPaletteSlotCapability> slots {
        {0, "#112233", "PLA", false, true, {}},
        {1, "#445566", "PETG", false, false, {}},
        {2, "#778899", "PLA", true, true, {{0, 0.5}, {1, 0.5}}},
    };

    const AI::PrintablePaletteSnapshot snapshot = build_orca_palette_snapshot(slots);

    REQUIRE(snapshot.physical_channels.size() == 2);
    CHECK(snapshot.compatible_slots == std::vector<size_t> {0});
    CHECK(snapshot.mixed_recipes.empty());
    CHECK(snapshot.supports(AI::ColorOutputMode::DiscreteFilament));
    CHECK_FALSE(snapshot.supports(AI::ColorOutputMode::ProcessMix));

    const AI::PrintablePaletteSnapshot unverified = build_orca_palette_snapshot(
        {{0, "#112233", "", false, true, {}}, {1, "#445566", "", false, true, {}}}, false);
    CHECK(unverified.compatible_slots.size() == 2);
    CHECK_FALSE(unverified.supports(AI::ColorOutputMode::ProcessMix));
}
