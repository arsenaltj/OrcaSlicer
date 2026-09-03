#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Orca/OrcaPaletteSnapshotBuilder.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

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
