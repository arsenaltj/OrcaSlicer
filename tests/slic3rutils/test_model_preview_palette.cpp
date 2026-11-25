#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitPaletteRoles.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchPaletteCount.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPackMapping.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "test_utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <limits>

using namespace Slic3r::GUI::PreviewPalette;
using Slic3r::GUI::WorkbenchPaletteCountRequests;
using Slic3r::GUI::WorkbenchPaletteCountSeed;
using Slic3r::GUI::prepare_workbench_palette_count;
using Slic3r::GUI::prepare_workbench_palette_reset;

TEST_CASE("Live palette count keeps the latest drag target and coalesces the release events", "[ModelPreviewPalette]")
{
    using namespace std::chrono_literals;
    WorkbenchPaletteCountRequests requests;
    const auto start = WorkbenchPaletteCountRequests::Clock::time_point{};
    requests.request(4, false);
    const auto first = requests.start(start);
    REQUIRE(first);

    requests.request(5, false);
    requests.request(8, false);
    CHECK_FALSE(requests.start(start + 20ms));
    CHECK_FALSE(requests.complete(*first));
    CHECK(requests.wait(start + 30ms) == 70ms);
    CHECK_FALSE(requests.start(start + 99ms));

    requests.request(8, true);
    const auto final = requests.start(start + 99ms);
    REQUIRE(final);
    CHECK(final->count == 8);
    CHECK(final->final);
    requests.request(8, true);
    CHECK(requests.complete(*final));
    CHECK_FALSE(requests.pending());
    CHECK_FALSE(requests.tracking());
    CHECK_FALSE(requests.start(start + 200ms));
}

TEST_CASE("Live palette count can finish a tracked value and rejects results from a replaced model", "[ModelPreviewPalette]")
{
    using namespace std::chrono_literals;
    WorkbenchPaletteCountRequests requests;
    const auto start = WorkbenchPaletteCountRequests::Clock::time_point{};
    requests.request(3, false);
    const auto tracked = requests.start(start);
    REQUIRE(tracked);
    CHECK(requests.complete(*tracked));
    CHECK(requests.tracking());
    requests.request(3, true);
    CHECK_FALSE(requests.start(start + 100ms));

    requests.request(6, false);
    const auto outdated = requests.start(start + 100ms);
    REQUIRE(outdated);
    requests.invalidate();
    CHECK_FALSE(requests.pending());
    requests.request(6, true);
    CHECK_FALSE(requests.start(start + 200ms));
    CHECK_FALSE(requests.complete(*outdated));

    const auto replacement = requests.start(start + 200ms);
    REQUIRE(replacement);
    CHECK_FALSE(requests.complete(*outdated));
    CHECK(requests.complete(*replacement));
    CHECK_FALSE(requests.pending());
}

TEST_CASE("Live manual color trial recovers colors and assignments when the drag reverses", "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1};
    Histogram histogram;
    histogram.add(0x00ff00, 1.);
    WorkbenchPaletteCountSeed seed;
    seed.colors = {red, green, blue};
    seed.mapping = {blue, red, green};
    seed.semantic_colors = seed.colors;
    seed.semantic_mapping = seed.mapping;
    seed.locks[1] = true;

    const auto lower = prepare_workbench_palette_count(seed, 1, histogram, nullptr);
    REQUIRE(lower.palette.colors.size() == 1);
    CHECK(lower.palette.colors[0] == green);
    CHECK(lower.palette.mapping[0] == red);
    CHECK(lower.palette.locks[0]);
    const auto restored = prepare_workbench_palette_count(seed, 3, histogram, nullptr);
    CHECK(restored.palette.colors == seed.colors);
    CHECK(restored.palette.mapping == seed.mapping);
    CHECK(restored.palette.semantic_mapping == seed.semantic_mapping);
    CHECK(restored.palette.locks == seed.locks);
}

TEST_CASE("Manual trial moves the retained last row with its color and mapping",
          "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1}, cyan {0, 1, 1};
    Histogram histogram;
    WorkbenchPaletteCountSeed seed;
    seed.colors = {red, green, blue, cyan, green, cyan};
    seed.mapping = {green, red, cyan, blue, blue, red};
    seed.semantic_colors = {red, green, blue, cyan, red, green};
    seed.semantic_mapping = {blue, cyan, red, blue, green, cyan};
    seed.locks[5] = true;

    const auto reduced = prepare_workbench_palette_count(seed, 4, histogram, nullptr);
    CHECK(reduced.palette.colors == (std::vector<Color>{red, green, blue, cyan}));
    CHECK(reduced.palette.mapping == (std::vector<Color>{green, red, cyan, red}));
    CHECK(reduced.palette.semantic_colors.back() == seed.semantic_colors[5]);
    CHECK(reduced.palette.semantic_mapping.back() == seed.semantic_mapping[5]);
    CHECK(reduced.palette.locks[3]);
    CHECK_FALSE(reduced.palette.locks[5]);
    CHECK_FALSE(reduced.limited_by_locks);
}

TEST_CASE("Equal retained manual colors keep separate source assignments and set the color limit",
          "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1};
    Histogram histogram;
    WorkbenchPaletteCountSeed seed;
    seed.colors = {red, green, red, blue, red};
    seed.mapping = {red, green, blue, red, green};
    seed.locks[2] = true;
    seed.locks[4] = true;

    const auto reduced = prepare_workbench_palette_count(seed, 1, histogram, nullptr);
    CHECK(reduced.limited_by_locks);
    CHECK(reduced.palette.colors == (std::vector<Color>{red, red}));
    CHECK(reduced.palette.mapping == (std::vector<Color>{blue, green}));
    CHECK(reduced.palette.locks[0]);
    CHECK(reduced.palette.locks[1]);

    seed.locks[4] = false;
    const auto unlocked = prepare_workbench_palette_count(seed, 1, histogram, nullptr);
    CHECK_FALSE(unlocked.limited_by_locks);
    CHECK(unlocked.palette.mapping == (std::vector<Color>{blue}));
    CHECK(unlocked.palette.locks[0]);
}

TEST_CASE("Manual trial retains legacy colors by the displayed card slot",
          "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1};
    Histogram histogram;
    const ColorCardRemap card {"saved", {red, green, blue}, {10, 11, 12}, {2, 0}, 0.0};
    WorkbenchPaletteCountSeed seed;
    seed.colors = {blue, red};
    seed.mapping = {red, blue};
    seed.locks[2] = true;
    Slic3r::GUI::use_workbench_card_rows(seed, card);
    CHECK(seed.mapping == (std::vector<Color>{blue, green, red}));

    const auto reduced = prepare_workbench_palette_count(seed, 2, histogram, nullptr);
    CHECK(reduced.palette.colors == (std::vector<Color>{red, blue}));
    CHECK(reduced.palette.mapping == (std::vector<Color>{blue, red}));
    CHECK_FALSE(reduced.palette.locks[0]);
    CHECK(reduced.palette.locks[1]);
}

TEST_CASE("Resetting the trial drops a pending palette result even when the same count is requested again",
          "[ModelPreviewPalette]")
{
    using namespace std::chrono_literals;
    WorkbenchPaletteCountRequests requests;
    const auto now = WorkbenchPaletteCountRequests::Clock::time_point{};
    requests.request(6, false);
    const auto before_reset = requests.start(now);
    REQUIRE(before_reset);
    requests.request(7, true);

    requests.invalidate();
    CHECK_FALSE(requests.pending());
    CHECK_FALSE(requests.tracking());
    CHECK_FALSE(requests.start(now + 20ms));

    requests.request(6, true, WorkbenchPaletteCountRequests::Kind::Reset);
    CHECK_FALSE(requests.complete(*before_reset));
    const auto after_reset = requests.start(now + 20ms);
    REQUIRE(after_reset);
    CHECK(after_reset->count == 6);
    CHECK(after_reset->kind == WorkbenchPaletteCountRequests::Kind::Reset);
    CHECK(requests.complete(*after_reset));
}

TEST_CASE("Replacing a trial color keeps duplicate targets and source region assignments distinct",
          "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1};
    const std::vector<Color> mapping {red, green, blue};
    ColorTrialMapping trial {true, mapping, {red, red, green}};

    REQUIRE(replace_trial_target_color(trial.target_colors, trial.mapping_colors, 1, blue));
    CHECK(trial.target_colors == (std::vector<Color>{red, blue, green}));
    CHECK(trial.mapping_colors == mapping);
    CHECK_FALSE(replace_trial_target_color(trial.target_colors, trial.mapping_colors, 3, red));
    CHECK(trial.target_colors == (std::vector<Color>{red, blue, green}));
}

TEST_CASE("Default palette reset applies all available colors when the model has fewer than six",
          "[ModelPreviewPalette]")
{
    Histogram histogram;
    histogram.add(0xff0000, 1.);
    histogram.add(0x00ff00, 1.);

    const auto reset = prepare_workbench_palette_reset("model", histogram, nullptr);
    REQUIRE(reset.basis.valid());
    CHECK(reset.requested_count == 6);
    CHECK(reset.palette.automatic);
    CHECK(reset.palette.colors.size() == 2);
    CHECK(reset.palette.mapping == reset.palette.colors);
    CHECK(reset.palette.mapping == histogram.trial_palette(6, {}, true));
    CHECK(reset.palette.locks == RetainedRows{});
}

TEST_CASE("Default palette reset reuses a matching initial palette and validates native fallback",
          "[ModelPreviewPalette]")
{
    Histogram histogram;
    histogram.add(0xff0000, 1.);
    const Color blue {0, 0, 1}, green {0, 1, 0};
    const auto initial = Slic3r::GUI::workbench_basis_for_palette(histogram, nullptr, "model", 6, {blue});
    const auto native = Slic3r::GUI::workbench_basis_for_palette(histogram, nullptr, "model", 6, {green});
    bool queried_native = false;
    const auto match_native = [&](size_t count) -> std::optional<AutomaticColorBasis> {
        queried_native = true;
        CHECK(count == 6);
        return native;
    };
    const auto reuse = prepare_workbench_palette_reset("model", histogram, nullptr, match_native, &initial);
    CHECK_FALSE(queried_native);
    CHECK(reuse.palette.colors == initial.source_palette);
    CHECK(reuse.basis.basis_signature == initial.basis_signature);

    const auto other = Slic3r::GUI::workbench_basis_for_palette(histogram, nullptr, "another-model", 6, {blue});
    const auto reuse_native = prepare_workbench_palette_reset("model", histogram, nullptr, match_native, &other);
    CHECK(queried_native);
    CHECK(reuse_native.palette.colors == native.source_palette);
    CHECK(reuse_native.palette.semantic_mapping == native.source_palette);
}

TEST_CASE("Live automatic color count preserves locked colors and reports unavailable counts", "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1};
    Histogram histogram;
    histogram.add(0xff0000, 1.);
    histogram.add(0x00ff00, 1.);
    histogram.add(0x0000ff, 1.);
    WorkbenchPaletteCountSeed seed;
    seed.automatic = true;
    seed.colors = {red, green, blue};
    seed.locks[0] = seed.locks[1] = true;
    const auto lower = prepare_workbench_palette_count(seed, 1, histogram, nullptr);
    CHECK(lower.limited_by_locks);
    CHECK(lower.palette.colors == (std::vector<Color>{red, green}));
    CHECK(lower.palette.locks[0]);
    CHECK(lower.palette.locks[1]);
    const auto unavailable = prepare_workbench_palette_count(seed, 32, histogram, nullptr);
    CHECK(unavailable.available_colors == 3);
}

TEST_CASE("Workbench AI palette starts with six colors",
          "[ModelPreviewPalette]")
{
    CHECK(initial_trial_color_count() == 6);
    CHECK(initial_trial_color_count() <= max_preview_colors);
}

TEST_CASE("Retained palette colors stay in their rows when recommendations change", "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1}, black {0, 0, 0}, white {1, 1, 1};
    const std::vector<Color> previous {red, green, blue, white};
    RetainedRows retained {};
    retained[1] = retained[3] = true;
    std::vector<Color> recommended {green, white, black, red};
    place_retained_colors(recommended, previous, retained);
    CHECK(recommended == std::vector<Color>{black, green, red, white});
    CHECK(retained[1]);
    CHECK(retained[3]);

    recommended = {green, white, black};
    place_retained_colors(recommended, previous, retained);
    CHECK(recommended == std::vector<Color>{black, green, white});
    CHECK_FALSE(retained[0]);
    CHECK(retained[1]);
    CHECK(retained[2]);
    CHECK_FALSE(retained[3]);
}

TEST_CASE("Reducing the palette keeps a retained last row at the new end", "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1},
        black {0, 0, 0}, yellow {1, 1, 0}, white {1, 1, 1};
    const std::vector<Color> before {red, green, blue, black, yellow, white};
    RetainedRows locks {};
    locks[5] = true;
    std::vector<Color> reduced {white, green, red, blue};

    place_retained_colors(reduced, before, locks);
    CHECK(reduced == std::vector<Color>{green, red, blue, white});
    CHECK_FALSE(locks[0]);
    CHECK(locks[3]);
    CHECK_FALSE(locks[5]);
}

TEST_CASE("Retained colors keep their relative order when reduced rows collide", "[ModelPreviewPalette]")
{
    const Color red {1, 0, 0}, green {0, 1, 0}, blue {0, 0, 1},
        black {0, 0, 0}, yellow {1, 1, 0}, white {1, 1, 1};
    Histogram histogram;
    histogram.add(0x000000, 1.);
    WorkbenchPaletteCountSeed seed;
    seed.automatic = true;
    seed.colors = {black, red, green, blue, yellow, white};
    seed.locks[1] = seed.locks[2] = seed.locks[5] = true;

    const auto at_limit = prepare_workbench_palette_count(std::move(seed), 2, histogram, nullptr);
    REQUIRE(at_limit.limited_by_locks);
    CHECK(at_limit.palette.colors == std::vector<Color>{red, green, white});
    CHECK(at_limit.palette.mapping == at_limit.palette.colors);
    CAPTURE(at_limit.palette.locks);
    CHECK(std::count(at_limit.palette.locks.begin(), at_limit.palette.locks.end(), true) == 3);
    CHECK(at_limit.palette.locks[2]);
}

TEST_CASE("Approved project portrait colors retain eye and brow roles beside the inherited skin preset",
          "[PortraitPaletteRoles][Regression]")
{
    using namespace Slic3r::GUI;
    const std::vector<Color> builtin{{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    auto approved=builtin;approved[0]={236.f/255,195.f/255,178.f/255};
    for(size_t count:{size_t(3),size_t(4),size_t(5),size_t(6)}) {
        auto slots=std::vector<Color>(approved.begin(),approved.begin()+count);
        std::reverse(slots.begin(),slots.end());
        const auto mapping=portrait_card_slot_mapping(slots,builtin);
        REQUIRE(mapping.enabled);
        CHECK(mapping.target_colors==slots);
        CHECK(mapping.mapping_colors==slots);
        const auto roles=portrait_roles_for_card(slots,builtin);
        nlohmann::json partition={{"faces",nlohmann::json::array({{{"cells",nlohmann::json::array({
            {{"id","left-white"},{"label","le"}},{{"id","right-white"},{"label","re"}},
            {{"id","left-brow"},{"label","lb"}},{{"id","right-brow"},{"label","rb"}},
            {{"id","iris"},{"label","iris-le"}}})}}})}};
        const auto colors=portrait_contour_colors(partition,roles,slots);
        REQUIRE(colors.size()==5);
        CHECK(colors.at("left-white")==approved[2]);
        CHECK(colors.at("right-white")==approved[2]);
        CHECK(colors.at("left-brow")==approved[1]);
        CHECK(colors.at("right-brow")==approved[1]);
        CHECK(colors.at("iris")==approved[1]);
    }
    auto custom=approved;custom[0]={.3f,.2f,.4f};
    CHECK_FALSE(portrait_card_slot_mapping(custom,builtin).enabled);
}

TEST_CASE("Portrait optimization retains physical skin and light slots when source groups share roles",
          "[PortraitPaletteRoles][Regression]")
{
    using namespace Slic3r::GUI;
    const std::vector<Color> card{{236.f/255,195.f/255,178.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> source{{.10f,.10f,.10f}, {.14f,.14f,.14f}, {.85f,.85f,.85f},
        {.96f,.96f,.96f}, {.76f,.55f,.41f}, {.80f,.60f,.46f}};
    const auto groups = portrait_pack_mapping(source, card);
    REQUIRE(groups.enabled);
    REQUIRE(portrait_roles_for_card(groups.target_colors, card).empty());
    const auto physical = portrait_card_slot_mapping(card, card);
    REQUIRE(physical.enabled);
    REQUIRE(physical.target_colors == card);
    REQUIRE(physical.mapping_colors == card);
    const auto roles = portrait_roles_for_card(physical.target_colors, card);
    REQUIRE(roles.size() == 6);
    REQUIRE(roles[0] == "portrait-skin");
    REQUIRE(roles[2] == "portrait-light");
    REQUIRE(physical.target_colors[0] != physical.target_colors[2]);
}

TEST_CASE("Portrait physical role bindings survive reduced and reordered cards without changing RGB",
          "[PortraitPaletteRoles][Regression]")
{
    using namespace Slic3r::GUI;
    const std::vector<Color> card{{236.f/255,195.f/255,178.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    for (size_t count = 3; count <= 6; ++count) {
        DYNAMIC_SECTION("Count " << count) {
            std::vector<Color> targets(card.begin(), card.begin() + count);
            std::reverse(targets.begin(), targets.end());
            const auto mapped = portrait_card_slot_mapping(targets, card);
            REQUIRE(mapped.enabled);
            REQUIRE(mapped.target_colors == targets);
            const auto roles = portrait_roles_for_card(mapped.target_colors, card);
            REQUIRE(roles[count-1] == "portrait-skin");
            REQUIRE(roles[count-3] == "portrait-light");
            REQUIRE(mapped.target_colors[count-1] == card[0]);
            REQUIRE(mapped.target_colors[count-3] == card[2]);
        }
    }
    auto unknown = card;
    unknown[0] = {.2f,.8f,.4f};
    REQUIRE_FALSE(portrait_card_slot_mapping(unknown, card).enabled);
    auto duplicate = card;
    duplicate[0] = duplicate[2];
    REQUIRE_FALSE(portrait_card_slot_mapping(duplicate, card).enabled);
}



TEST_CASE("Portrait card keeps skin and lips separate and recolors cool clothing", "[FilamentColorPack]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> source {{46.f/255,46.f/255,49.f/255}, {57.f/255,67.f/255,59.f/255},
        {92.f/255,91.f/255,94.f/255}, {176.f/255,80.f/255,69.f/255},
        {204.f/255,148.f/255,112.f/255}, {233.f/255,233.f/255,236.f/255}};
    const auto mapping = portrait_pack_mapping(source, card);
    REQUIRE(mapping.enabled);
    REQUIRE(mapping.mapping_colors == source);
    CHECK(mapping.target_colors == std::vector<Color>{card[1], card[4], card[5], card[3], card[0], card[2]});
    auto reversed = source; std::reverse(reversed.begin(), reversed.end());
    auto expected = mapping.target_colors; std::reverse(expected.begin(), expected.end());
    CHECK(portrait_pack_mapping(reversed, card).target_colors == expected);
    CHECK(portrait_pack_mapping({{.96f,.95f,.94f}}, card).target_colors == std::vector<Color>{card[2]});
    CHECK_FALSE(portrait_pack_mapping({}, card).enabled);
    CHECK_FALSE(portrait_pack_mapping(source, {}).enabled);
}

TEST_CASE("Portrait matching preserves locally painted filament colors on repeated application", "[FilamentColorPack]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> edited {{.2f,.2f,.2f}, {92.f/255,91.f/255,94.f/255},
        {176.f/255,80.f/255,69.f/255}, card[4], {204.f/255,148.f/255,112.f/255},
        {233.f/255,233.f/255,236.f/255}};
    const auto mapped = portrait_pack_mapping(edited, card);
    CHECK(mapped.target_colors == std::vector<Color>{card[1], card[5], card[3], card[4], card[0], card[2]});
    CHECK(portrait_pack_mapping(mapped.target_colors, card).target_colors == mapped.target_colors);
    auto shuffled = card; std::reverse(shuffled.begin(), shuffled.end());
    CHECK(portrait_pack_mapping(shuffled, card).target_colors == shuffled);
    auto rounded = card[4]; rounded[1] = .54902f;
    CHECK(portrait_pack_mapping({rounded}, card).target_colors == std::vector<Color>{card[4]});
}

TEST_CASE("Portrait matching leaves pink unused for six neutral and golden source groups", "[FilamentColorPack][Regression]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    // Dark features, blue clothing and three golden fur shades contain no red accent.
    const std::vector<Color> source {card[1], card[4], {140.f/255,105.f/255,72.f/255},
        {201.f/255,164.f/255,118.f/255}, {232.f/255,205.f/255,164.f/255}, card[2]};
    const auto mapped = portrait_pack_mapping(source, card);
    REQUIRE(mapped.enabled);
    REQUIRE(mapped.mapping_colors == source);
    REQUIRE(mapped.target_colors.size() == source.size());
    CHECK(std::find(mapped.target_colors.begin(), mapped.target_colors.end(), card[3]) == mapped.target_colors.end());
    CHECK(mapped.target_colors[3] == card[0]);
    CHECK(mapped.target_colors[4] == mapped.target_colors[5]);
    CHECK(mapped.target_colors[0] == card[1]);
    CHECK(mapped.target_colors[1] == card[4]);
}

TEST_CASE("Portrait source groups retain their material when other groups use the same role", "[FilamentColorPack][Regression]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const Color skin {204.f/255,148.f/255,112.f/255};
    const auto alone = portrait_pack_mapping({skin}, card);
    const auto together = portrait_pack_mapping({skin, card[0], card[3]}, card);
    REQUIRE(alone.enabled);
    REQUIRE(together.enabled);
    REQUIRE(together.mapping_colors == std::vector<Color>{skin, card[0], card[3]});
    CHECK(together.target_colors[0] == alone.target_colors[0]);
    CHECK(together.target_colors[0] == card[0]);
    CHECK(together.target_colors[1] == card[0]);
    CHECK(together.target_colors[2] == card[3]);
    CHECK(portrait_pack_mapping(together.target_colors, card).target_colors == together.target_colors);
}

TEST_CASE("Portrait matching separates muted skin boundaries from red lips", "[FilamentColorPack][Regression]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    // Rounded area-weighted centers from a real portrait. The muted boundary
    // and lips have almost the same hue and lightness, but distinct chroma.
    const std::vector<Color> source {{38.f/255,38.f/255,37.f/255}, {55.f/255,54.f/255,54.f/255},
        {175.f/255,91.f/255,80.f/255}, {159.f/255,110.f/255,103.f/255},
        {184.f/255,148.f/255,130.f/255}, {227.f/255,222.f/255,226.f/255}};
    const auto mapped = portrait_pack_mapping(source, card);
    REQUIRE(mapped.enabled);
    REQUIRE(mapped.mapping_colors == source);
    CHECK(mapped.target_colors == std::vector<Color>{card[1], card[1], card[3], card[0], card[0], card[2]});
    CHECK(portrait_pack_mapping({source[3]}, card).target_colors == std::vector<Color>{card[0]});
}

TEST_CASE("Portrait matching retains red accents across lightness variations", "[FilamentColorPack][Regression]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> reds {{175.f/255,91.f/255,80.f/255}, {190.f/255,105.f/255,97.f/255}};
    CHECK(portrait_pack_mapping(reds, card).target_colors == std::vector<Color>{card[3], card[3]});
}

TEST_CASE("Portrait matching keeps gray and near-neutral ramps neutral and ordered", "[FilamentColorPack][Regression]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> neutral_targets {card[1], card[5], card[2]};
    for (int warm : {0, 1}) {
        float previous_lightness = -1.f;
        // The second ramp includes a small warm cast, as in baked cloth shading.
        for (int gray = warm ? 16 : 0; gray <= (warm ? 239 : 255); ++gray) {
            const Color source {(gray + 2*warm)/255.f, gray/255.f, (gray - warm)/255.f};
            INFO("gray=" << gray << ", warm=" << warm);
            const auto lab = to_lab(source);
            REQUIRE(std::hypot(lab[1], lab[2]) < .015f);
            const auto mapped = portrait_pack_mapping({source}, card);
            REQUIRE(mapped.enabled);
            REQUIRE(mapped.target_colors.size() == 1);
            const auto& target = mapped.target_colors.front();
            CHECK(std::find(neutral_targets.begin(), neutral_targets.end(), target) != neutral_targets.end());
            const float lightness = to_lab(target)[0];
            CHECK(lightness + 1e-6f >= previous_lightness);
            previous_lightness = lightness;
        }
    }
}

TEST_CASE("The portrait color pack preserves the supplied physical slot order", "[FilamentColorPack]")
{
    const auto pack = Slic3r::GUI::honor_people_color_pack();
    REQUIRE(pack.valid());
    REQUIRE(pack.colors == std::vector<std::string>{"#000000", "#AF6E5E", "#ECC3B2", "#FFFFFF", "#A7A5A4", "#FFFFFF"});
    REQUIRE(pack.labels.size() == pack.colors.size());
}


TEST_CASE("Color packs reject incomplete and nonphysical color cards", "[FilamentColorPack]")
{
    auto pack = Slic3r::GUI::honor_people_color_pack();
    SECTION("Incomplete slot metadata") { pack.labels.pop_back(); }
    SECTION("Non RGB color") { pack.colors[0] = "cmyk(5,14,14,0)"; }
    SECTION("Malformed channel") { pack.colors[0] = "#FFHHFF"; }
    SECTION("More than six physical colors") { pack.colors.push_back("#FFFFFF"); pack.labels.push_back("extra"); }
    SECTION("Unnamed card") { pack.name.clear(); }
    REQUIRE_FALSE(pack.valid());
}

TEST_CASE("Physical filament cards retain complete slot order beyond the legacy six-color preview", "[FilamentColorPack]")
{
    Slic3r::GUI::FilamentColorPack pack;
    pack.name = "printer-8";
    for (size_t i = 0; i < 8; ++i) {
        pack.colors.push_back(i % 2 ? "#112233" : "#AABBCC");
        pack.labels.push_back("slot " + std::to_string(i + 1));
        pack.presets.push_back("PLA " + std::to_string(i + 1));
    }
    CHECK_FALSE(pack.valid());
    CHECK(pack.valid_physical());
    pack.presets.pop_back();
    CHECK_FALSE(pack.valid_physical());
}

namespace {
struct ScopedColorPackLibrary : ScopedTemporaryDir {
    const std::string previous = Slic3r::data_dir();
    ScopedColorPackLibrary() : ScopedTemporaryDir("orca-color-pack") { Slic3r::set_data_dir(string()); }
    ~ScopedColorPackLibrary() { Slic3r::set_data_dir(previous); }
    boost::filesystem::path file() const { return path() / "filament_color_packs.json"; }
};
}

TEST_CASE("Saved physical color cards reload and replace all slots without merging equal colors", "[FilamentColorPack][Regression]")
{
    using namespace Slic3r::GUI;
    ScopedColorPackLibrary library;
    FilamentColorPack pack;
    pack.name = u8"已保存的八槽组合";
    for (size_t i = 0; i < 8; ++i) {
        pack.colors.push_back("#AABBCC");
        pack.labels.push_back("slot " + std::to_string(i + 1));
        pack.presets.push_back("PLA " + std::to_string(i + 1));
        pack.multi_colors.push_back("#AABBCC #112233");
        pack.color_types.push_back("0");
    }
    REQUIRE(save_filament_color_pack(pack));
    REQUIRE(boost::filesystem::exists(library.file()));
    auto cards = load_filament_color_packs();
    REQUIRE(cards.size() == honor_filament_suites().size() + 1);
    CHECK(cards.back().name == pack.name);
    CHECK(cards.back().colors == pack.colors);
    CHECK(cards.back().presets == pack.presets);
    CHECK(cards.back().multi_colors == pack.multi_colors);
    CHECK(cards.back().color_types == pack.color_types);
    pack.colors[7] = "#112233";
    REQUIRE(save_filament_color_pack(pack));
    cards = load_filament_color_packs();
    REQUIRE(cards.size() == honor_filament_suites().size() + 1);
    CHECK(cards.back().colors == pack.colors);
    pack.name = "another card";
    REQUIRE(save_filament_color_pack(pack));
    cards = load_filament_color_packs();
    REQUIRE(cards.size() == honor_filament_suites().size() + 2);
    CHECK(cards.back().name == pack.name);
}

TEST_CASE("Saving a physical card retains legacy cards and writes a complete v2 schema", "[FilamentColorPack][Regression]")
{
    using namespace Slic3r::GUI;
    ScopedColorPackLibrary library;
    const nlohmann::json legacy{{"name", "old trial"}, {"colors", {"#123456"}}, {"labels", {"old slot"}}};
    { boost::filesystem::ofstream output(library.file()); output << nlohmann::json::array({legacy}); }
    const FilamentColorPack pack{"new physical", {"#FFFFFF"}, {"slot"}, {"PLA"}};
    REQUIRE(save_filament_color_pack(pack));
    boost::filesystem::ifstream input(library.file());
    const auto saved = nlohmann::json::parse(input);
    REQUIRE(saved.size() == 2);
    CHECK(saved[0] == legacy);
    CHECK(saved[1]["schema"] == "orca.filament-pack/v2");
    CHECK(saved[1]["presets"] == pack.presets);
    const auto cards = load_filament_color_packs();
    REQUIRE(cards.size() == honor_filament_suites().size() + 2);
    CHECK(cards[honor_filament_suites().size()].name == "old trial");
    CHECK(cards[honor_filament_suites().size() + 1].presets == pack.presets);
}

TEST_CASE("Failed color card saves preserve the existing library", "[FilamentColorPack][Regression]")
{
    using namespace Slic3r::GUI;
    ScopedColorPackLibrary library;
    const FilamentColorPack valid{"physical", {"#FFFFFF"}, {"slot"}, {"PLA"}};
    SECTION("Invalid physical card") {
        REQUIRE(save_filament_color_pack(valid));
        auto invalid = valid; invalid.presets.clear();
        CHECK_FALSE(save_filament_color_pack(invalid));
        const auto cards = load_filament_color_packs();
        REQUIRE(cards.size() == honor_filament_suites().size() + 1);
        CHECK(cards.back().presets == valid.presets);
    }
    SECTION("Malformed existing library") {
        const std::string original = "{broken json";
        { boost::filesystem::ofstream output(library.file()); output << original; }
        CHECK_FALSE(save_filament_color_pack(valid));
        boost::filesystem::ifstream input(library.file());
        const std::string retained((std::istreambuf_iterator<char>(input)), {});
        CHECK(retained == original);
    }
    SECTION("Unwritable library path") {
        boost::filesystem::create_directory(library.file());
        CHECK_FALSE(save_filament_color_pack(valid));
        CHECK(boost::filesystem::is_directory(library.file()));
    }
}

TEST_CASE("Preview palettes stay bounded and deterministic for a color gradient", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned i = 0; i < 256; ++i) histogram.add((i << 16) | ((255-i) << 8) | (i/2), 1);
    const auto colors = histogram.palette();
    REQUIRE(colors.size() == 6);
    REQUIRE(histogram.palette() == colors);
    REQUIRE(histogram.palette(100).size() == 6);
    REQUIRE(histogram.palette(0).empty());
    for (const auto& color : colors) for (float c : color) {
        REQUIRE(std::isfinite(c)); REQUIRE(c >= 0); REQUIRE(c <= 1);
    }
}

TEST_CASE("Preview area weights are independent of repeated tessellation samples", "[ModelPreviewPalette]")
{
    Histogram coarse, dense;
    coarse.add(0xff0000, 90); coarse.add(0x0000ff, 10);
    dense.add(0xff0000, 90);
    for (int i = 0; i < 1000; ++i) dense.add(0x0000ff, .01);
    const auto first = coarse.palette(1), second = dense.palette(1);
    REQUIRE(distance(first[0], second[0]) < 1e-8f);
    REQUIRE(distance(to_lab(first[0]), to_lab({1,0,0})) < distance(to_lab(first[0]), to_lab({0,0,1})));
}

TEST_CASE("Preview palettes handle empty and monochrome inputs", "[ModelPreviewPalette]")
{
    Histogram histogram;
    REQUIRE(histogram.palette().empty());
    histogram.add(0xff0000, 0);
    histogram.add(0xff0000, -1);
    REQUIRE(histogram.palette().empty());
    histogram.add(0xe8e7ec, 10);
    const auto colors = histogram.palette();
    REQUIRE(colors.size() == 1);
    REQUIRE(distance(colors[0], {232.f/255,231.f/255,236.f/255}) < 1e-8f);
}

TEST_CASE("Preview perceptual conversion round trips primary and neutral colors", "[ModelPreviewPalette]")
{
    for (Color color : {Color{0,0,0}, Color{1,1,1}, Color{1,0,0}, Color{0,1,0}, Color{0,0,1}, Color{.8f,.5f,.3f}})
        REQUIRE(distance(to_rgb(to_lab(color)), color) < 1e-8f);
}

TEST_CASE("Preview Oklab coordinates match the published sRGB primary reference", "[ModelPreviewPalette]")
{
    using Catch::Matchers::WithinAbs;
    // Bjorn Ottosson's 2021 linear-sRGB matrices, including the sRGB transfer
    // function. An internally consistent but wrong forward/inverse pair must
    // not pass solely because it round-trips.
    const Color red = to_lab({1, 0, 0});
    REQUIRE_THAT(red[0], WithinAbs(.62795536, 1e-6));
    REQUIRE_THAT(red[1], WithinAbs(.22486306, 1e-6));
    REQUIRE_THAT(red[2], WithinAbs(.12584630, 1e-6));
    const Color gray = to_lab({.5f, .5f, .5f});
    REQUIRE_THAT(gray[0], WithinAbs(.59818073, 1e-6));
    REQUIRE_THAT(gray[1], WithinAbs(0, 1e-6));
    REQUIRE_THAT(gray[2], WithinAbs(0, 1e-6));
}

TEST_CASE("Preview nearest color preserves group assignment when only target colors change", "[ModelPreviewPalette]")
{
    const std::vector<Color> original {{.15f, .4f, .2f}, {.85f, .2f, .15f}, {.95f, .95f, .95f}};
    std::vector<Color> mapping;
    for (const Color color : original) mapping.push_back(to_lab(color));
    auto edited = original;
    edited[0] = {0, 0, 1};
    const size_t original_group = nearest_lab_index(to_lab({.16f, .41f, .21f}), mapping);
    REQUIRE(original_group == 0);
    REQUIRE(edited[original_group] == Color{0, 0, 1});
    REQUIRE(nearest_lab_index(to_lab({.84f, .21f, .15f}), mapping) == 1);
    // Identical/tied mapping centers use the first group on CPU and GPU.
    REQUIRE(nearest_lab_index(to_lab(original[0]), {mapping[0], mapping[0]}) == 0);
    REQUIRE(nearest_lab_index(to_lab(original[0]), {}) == 0);
}

TEST_CASE("Preview nearest colors use the same chroma priority across interpolated source samples", "[ModelPreviewPalette]")
{
    const std::vector<Color> centers {{.5f, 0, 0}, {.8f, .08f, 0}, {.2f, 0, -.12f}};
    // The shader interpolates source RGB before conversion and selects one
    // palette entry at each fragment; target colors must never be interpolated.
    for (int i = 0; i <= 100; ++i) {
        const float t = i / 100.f;
        const Color source = to_lab({t, 1 - t, .4f});
        size_t reference = 0;
        float best = std::numeric_limits<float>::max();
        for (size_t j = 0; j < centers.size(); ++j) {
            const float dl = (source[0] - centers[j][0]) * .35f;
            const float da = source[1] - centers[j][1], db = source[2] - centers[j][2];
            const float d = dl * dl + da * da + db * db;
            if (d < best) { best = d; reference = j; }
        }
        REQUIRE(nearest_lab_index(source, centers) == reference);
    }
}

TEST_CASE("Color cards use a deterministic weighted one-to-one assignment", "[ModelPreviewPalette][ColorCardRemap]")
{
    AutomaticColorBasis basis;
    basis.geometry_id = "geometry";
    basis.source_color_signature = "source";
    basis.requested_count = 2;
    basis.source_palette = {{.48f,.48f,.48f}, {.55f,.55f,.55f}};
    basis.source_area = {100.0, 1.0};
    basis.basis_signature = automatic_basis_signature(basis);
    const std::vector<Color> card {{.49f,.49f,.49f}, {1.f,1.f,1.f}};

    const auto remap = minimum_cost_color_card_remap(basis, card, {17, 42});
    REQUIRE(remap.valid(2));
    REQUIRE(remap.target_palette == card);
    REQUIRE(remap.target_slot_ids == std::vector<size_t>{17, 42});
    CHECK(remap.source_to_target_slot == std::vector<size_t>{0, 1});
    CHECK(remap.source_order_targets() == std::vector<Color>{card[0], card[1]});

    const double swapped = 100.0 * distance(to_lab(basis.source_palette[0]), to_lab(card[1])) +
        distance(to_lab(basis.source_palette[1]), to_lab(card[0]));
    CHECK(remap.weighted_error < swapped);
    CHECK(minimum_cost_color_card_remap(basis, card, {17, 42}).source_to_target_slot ==
        remap.source_to_target_slot);
}

TEST_CASE("Equal RGB color-card slots keep independent identities", "[ModelPreviewPalette][ColorCardRemap]")
{
    AutomaticColorBasis basis;
    basis.geometry_id = "geometry";
    basis.source_color_signature = "source";
    basis.requested_count = 2;
    basis.source_palette = {{1.f,0.f,0.f}, {0.f,0.f,1.f}};
    basis.source_area = {1.0, 1.0};
    basis.basis_signature = automatic_basis_signature(basis);
    const std::vector<Color> card {{.2f,.3f,.4f}, {.2f,.3f,.4f}};
    const auto remap = minimum_cost_color_card_remap(basis, card, {3, 9});
    REQUIRE(remap.valid(2));
    REQUIRE(remap.source_to_target_slot.size() == 2);
    CHECK(remap.source_to_target_slot[0] != remap.source_to_target_slot[1]);
    CHECK(remap.target_slot_ids[remap.source_to_target_slot[0]] !=
        remap.target_slot_ids[remap.source_to_target_slot[1]]);

    const auto identity_tie = minimum_cost_color_card_remap(basis, card, {42, 17});
    REQUIRE(identity_tie.valid(2));
    CHECK(identity_tie.source_to_target_slot == std::vector<size_t>{1, 0});
    CHECK_FALSE(minimum_cost_color_card_remap(basis, card, {17, 17}).valid(2));
}

TEST_CASE("Automatic color basis is stable and does not invent empty groups", "[ModelPreviewPalette][ColorCardRemap]")
{
    Histogram histogram;
    histogram.add(0xff0000, 10.0);
    const auto first = make_automatic_color_basis(histogram, 6, "geometry", true);
    const auto second = make_automatic_color_basis(histogram, 6, "geometry", true);
    REQUIRE(first.valid());
    CHECK(first.requested_count == 6);
    CHECK(first.source_palette.size() == 1);
    CHECK(first.basis_signature == second.basis_signature);
    const std::vector<Color> card {{1,0,0}, {0,1,0}, {0,0,1}, {1,1,1}, {0,0,0}, {.5f,.5f,.5f}};
    const auto remap = minimum_cost_color_card_remap(first, card);
    REQUIRE(remap.valid(1));
    CHECK(remap.source_to_target_slot.size() == 1);
}

TEST_CASE("Automatic color basis captures native unit membership and area", "[ModelPreviewPalette][AutomaticColorBasis]")
{
    Histogram histogram;
    histogram.add(0xff0000, 10.0);
    histogram.add(0x0000ff, 5.0);
    AutomaticColorUnits units;
    units.colors = {{1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}};
    units.area = {4.0, 5.0, 6.0};

    const auto basis = make_automatic_color_basis(histogram, 2, "geometry", false, &units);
    REQUIRE(basis.valid());
    REQUIRE(basis.unit_source_slots.size() == units.colors.size());
    CHECK(basis.unit_source_slots[0] == basis.unit_source_slots[2]);
    CHECK(basis.unit_source_slots[0] != basis.unit_source_slots[1]);
    CHECK_THAT(basis.source_area[basis.unit_source_slots[0]], Catch::Matchers::WithinAbs(10.0, 1e-12));
    CHECK_THAT(basis.source_area[basis.unit_source_slots[1]], Catch::Matchers::WithinAbs(5.0, 1e-12));

    auto changed = basis;
    changed.unit_area[0] += 1.0;
    changed.basis_signature = automatic_basis_signature(changed);
    CHECK(changed.basis_signature != basis.basis_signature);
}

TEST_CASE("Protected preview palettes keep supported minority hues among dominant gray shades", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x18392d, 300);
    histogram.add(0x915647, 150);
    histogram.add(0x0000ff, 1); // A tiny blue speck must not reserve a channel.
    const auto area = histogram.palette(6), protected_colors = histogram.palette(6, {}, true);
    auto error = [](const std::vector<Color>& palette, Color target) {
        float best = 100;
        for (const auto& color : palette) best = std::min(best, distance(to_lab(color), to_lab(target)));
        return best;
    };
    REQUIRE(protected_colors.size() == 6);
    for (Color target : {Color{24.f/255,57.f/255,45.f/255}, Color{145.f/255,86.f/255,71.f/255}}) {
        REQUIRE(error(protected_colors, target) < 1e-8f);
        REQUIRE(error(protected_colors, target) < error(area, target));
    }
    REQUIRE(error(protected_colors, {0,0,1}) > .001f);
    REQUIRE(histogram.hue_candidates().size() == 2);
    REQUIRE(histogram.palette(6, {}, true) == protected_colors);
}

TEST_CASE("Protected palettes separate saturated accents from a dominant muted hue", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x413231, 3000);
    histogram.add(0xd5a084, 2000);
    // The accent shares the muted material's hue but has distinct chroma.
    // Averaging both into one protected hue loses this supported small color.
    histogram.add(0xbe6961, 50);
    const Color accent {190.f/255, 105.f/255, 97.f/255};
    const auto colors = histogram.palette(6, {}, true);
    REQUIRE(colors.size() == 6);
    float accent_error = 100, muted_error = 100;
    for (const auto& color : colors) {
        accent_error = std::min(accent_error, distance(to_lab(color), to_lab(accent)));
        muted_error = std::min(muted_error, distance(to_lab(color), to_lab({65.f/255, 50.f/255, 49.f/255})));
    }
    REQUIRE_THAT(accent_error, Catch::Matchers::WithinAbs(0, 1e-8));
    REQUIRE(muted_error < .000225f);
    REQUIRE(histogram.palette(6, {}, true) == colors);
}

TEST_CASE("A saturated speck cannot reserve a protected color within a muted hue", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x413231, 3000);
    histogram.add(0xd5a084, 2000);
    histogram.add(0xbe6961, 2);
    const Color speck {190.f/255, 105.f/255, 97.f/255};
    float error = 100;
    for (const auto& color : histogram.palette(6, {}, true))
        error = std::min(error, distance(to_lab(color), to_lab(speck)));
    REQUIRE(error > .000225f);
}

TEST_CASE("Protected palettes retain small accents within an already chromatic material", "[ModelPreviewPalette][Regression]")
{
    Histogram histogram;
    histogram.add(0x262626, 5000);
    histogram.add(0xececec, 5000);
    histogram.add(0xca9c81, 10000);
    histogram.add(0xb65c46, 20);
    // Both colors have C > .045 and fall in the same 30-degree hue bucket.
    // The accent has greater chroma, rather than merely different lightness.
    const Color accent {182.f/255, 92.f/255, 70.f/255};
    const auto candidates = histogram.hue_candidates();
    REQUIRE(candidates.size() == 2);
    float candidate_error = 100;
    for (const auto& color : candidates)
        candidate_error = std::min(candidate_error, distance(to_lab(color), to_lab(accent)));
    CHECK_THAT(candidate_error, Catch::Matchers::WithinAbs(0, 1e-8));

    const auto colors = histogram.palette(6, {}, true);
    float palette_error = 100;
    for (const auto& color : colors)
        palette_error = std::min(palette_error, distance(to_lab(color), to_lab(accent)));
    CHECK_THAT(palette_error, Catch::Matchers::WithinAbs(0, 1e-8));
}

TEST_CASE("Unsupported chromatic tails do not reserve an accent color", "[ModelPreviewPalette][Regression]")
{
    Histogram histogram;
    histogram.add(0x262626, 5000);
    histogram.add(0xececec, 5000);
    histogram.add(0xca9c81, 10000);
    // The same accent occupies only 0.01% of the surface here.
    histogram.add(0xb65c46, 2);
    const auto candidates = histogram.hue_candidates();
    REQUIRE(candidates.size() == 1);
    const Color accent {182.f/255, 92.f/255, 70.f/255};
    CHECK(distance(to_lab(candidates.front()), to_lab(accent)) > .000225f);
}

TEST_CASE("Baked lightness variations do not create chromatic accent candidates", "[ModelPreviewPalette][Regression]")
{
    Histogram histogram;
    const auto material = to_lab({202.f/255, 156.f/255, 129.f/255});
    for (float lightness : {.5f, .65f, .8f}) {
        auto sample = material;
        sample[0] = lightness;
        const auto rgb = to_rgb(sample);
        const auto channel = [](float c) { return uint32_t(std::lround(c*255)); };
        histogram.add((channel(rgb[0]) << 16) | (channel(rgb[1]) << 8) | channel(rgb[2]), 1000);
    }
    CHECK(histogram.hue_candidates().size() == 1);
}

TEST_CASE("Supported middle gray retains a source group in a chromatic palette", "[ModelPreviewPalette][Regression]")
{
    Histogram histogram;
    histogram.add(0x262626, 20000);
    histogram.add(0xececec, 20000);
    histogram.add(0x808080, 5000);
    for (uint32_t rgb : {0xff0000u, 0x00ff00u, 0x0000ffu, 0xffff00u})
        histogram.add(rgb, 1000);
    // Four supported hues must not consume every position between black and white.
    const auto colors = histogram.palette(6, {}, true);
    REQUIRE(colors.size() == 6);
    const Color gray {128.f/255, 128.f/255, 128.f/255};
    std::vector<Color> centers;
    for (const auto& color : colors) centers.push_back(to_lab(color));
    const size_t group = nearest_lab_index(to_lab(gray), centers);
    REQUIRE(group < colors.size());
    CHECK_THAT(distance(centers[group], to_lab(gray)), Catch::Matchers::WithinAbs(0, 1e-8));

    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const auto mapped = portrait_pack_mapping(colors, card);
    REQUIRE(mapped.target_colors.size() == colors.size());
    CHECK(mapped.target_colors[group] == card[5]);
}

TEST_CASE("Neutral and accent protection respect user locks and small palette limits", "[ModelPreviewPalette][Regression]")
{
    Histogram histogram;
    histogram.add(0x262626, 20000);
    histogram.add(0xececec, 20000);
    histogram.add(0x808080, 5000);
    histogram.add(0xca9c81, 10000);
    histogram.add(0xb65c46, 50);
    histogram.add(0x39433b, 1000);
    const std::vector<Color> locks {{.713f,.297f,.631f}, {.128f,.923f,.317f}};
    CHECK(histogram.palette(0, locks, true).empty());
    for (size_t limit = 1; limit <= 6; ++limit) {
        INFO("limit=" << limit);
        const auto colors = histogram.palette(limit, locks, true);
        REQUIRE(colors.size() <= limit);
        REQUIRE(colors.size() >= std::min(limit, locks.size()));
        for (size_t i = 0; i < std::min(limit, locks.size()); ++i)
            CHECK(colors[i] == locks[i]);
        CHECK(histogram.palette(limit, locks, true) == colors);
        for (const auto& color : colors) for (float c : color) {
            CHECK(std::isfinite(c));
            CHECK(c >= 0);
            CHECK(c <= 1);
        }
    }
}

TEST_CASE("Preview color locks retain exact values and order across reclustering", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned i = 0; i < 256; ++i) histogram.add((i << 16) | ((255-i) << 8) | i, i+1);
    const std::vector<Color> locked {{.713f,.297f,.631f}, {.128f,.923f,.317f}};
    for (bool protect : {false, true}) {
        const auto colors = histogram.palette(6, locked, protect);
        REQUIRE(colors.size() == 6);
        REQUIRE(colors[0] == locked[0]);
        REQUIRE(colors[1] == locked[1]);
    }
    REQUIRE(histogram.palette(1, locked, true).size() == 1);
    REQUIRE(histogram.palette(1, locked, true)[0] == locked[0]);
    REQUIRE(histogram.palette(2, locked, true) == locked);
}

TEST_CASE("Protected palettes retain muted materials and small saturated accents with neutral support", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    // Muted material with C below .025, plus a strong accent covering less
    // than .15% of the surface. Neither is an isolated texture speck.
    histogram.add(0x39433b, 600);
    histogram.add(0xb35145, 42);
    histogram.add(0x0000ff, 1);
    const auto colors = histogram.palette(6, {}, true);
    REQUIRE(colors.size() == 6);
    for (Color target : {Color{57.f/255,67.f/255,59.f/255}, Color{179.f/255,81.f/255,69.f/255}}) {
        float best = 100;
        for (const auto& color : colors) best = std::min(best, distance(to_lab(color), to_lab(target)));
        REQUIRE(best < 1e-8f);
    }
    float darkest = 1, lightest = 0;
    for (const auto& color : colors) {
        darkest = std::min(darkest, to_lab(color)[0]);
        lightest = std::max(lightest, to_lab(color)[0]);
    }
    REQUIRE(darkest < .15f);
    REQUIRE(lightest > .9f);
    REQUIRE(histogram.hue_candidates().size() == 2);
}

TEST_CASE("Preview color protection handles empty data invalid locks and small channel counts", "[ModelPreviewPalette]")
{
    Histogram histogram;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Color> locks {{nan,0,0}, {-1,0,0}, {0,2,0}, {.25f,.5f,.75f}, {.25f,.5f,.75f}};
    histogram.add(0xffffff, std::numeric_limits<double>::infinity());
    histogram.add(0xffffff, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(histogram.palette(6, {}, true).empty());
    REQUIRE(histogram.hue_candidates().empty());
    REQUIRE(histogram.palette(6, locks, true).size() == 1);
    REQUIRE(histogram.palette(6, locks, true)[0] == locks[3]);
    REQUIRE(histogram.palette(0, locks, true).empty());
    histogram.add(0xeeeeee, 500);
    histogram.add(0x006030, 10);
    for (size_t channels : {size_t(1), size_t(2), size_t(3), size_t(6)}) {
        const auto colors = histogram.palette(channels, locks, true);
        REQUIRE(colors.size() <= channels);
        REQUIRE(colors[0] == locks[3]);
        for (const auto& color : colors) for (float c : color) {
            REQUIRE(std::isfinite(c)); REQUIRE(c >= 0); REQUIRE(c <= 1);
        }
    }
}

TEST_CASE("The suite library removes the retired people card and retains unrelated saved colors", "[FilamentColorPack][HonorFilamentSuite]")
{
    using namespace Slic3r::GUI;
    ScopedColorPackLibrary library;
    const nlohmann::json old{{"name", u8"人物基础 · 年轻向六色"}, {"colors", {"#123456"}}, {"labels", {"old"}}};
    const nlohmann::json custom{{"name", "my color"}, {"colors", {"#112233"}}, {"labels", {"one"}}};
    { boost::filesystem::ofstream output(library.file()); output << nlohmann::json::array({old, custom}); }
    const auto cards = load_filament_color_packs();
    REQUIRE(cards.size() == 3);
    CHECK(cards[0].name == u8"人物六色");
    CHECK(cards[1].name == u8"萌宠六色");
    CHECK(cards[2].name == "my color");
}
