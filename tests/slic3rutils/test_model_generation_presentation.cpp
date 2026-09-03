#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"

#include <algorithm>
#include <set>

using Slic3r::GUI::AIModelGenerationClient;
using namespace Slic3r::GUI::ModelGenerationPresentation;

TEST_CASE("model-generation progress maps service phases to stable UI milestones",
          "[ModelGenerationPresentation]")
{
    AIModelGenerationClient::JobStatus status;
    REQUIRE(status.palette_color_count == Slic3r::AI::kLegacyDefaultTargetPaletteColors);

    status.state = "recommending_palette";
    status.progress = 5;
    REQUIRE(display_progress(status) == 3);
    status.progress = 10;
    REQUIRE(display_progress(status) == 10);

    status.state = "awaiting_palette_confirmation";
    REQUIRE(display_progress(status) == 10);
    status.state = "preprocessing";
    status.progress = 15;
    REQUIRE(display_progress(status) == 25);
    status.state = "awaiting_confirmation";
    REQUIRE(display_progress(status) == 35);

    status.state.clear();
    status.phase = "generating";
    status.progress = 70;
    REQUIRE(display_progress(status) == 78);
    status.phase = "converting";
    status.progress = 95;
    REQUIRE(display_progress(status) == 90);
    status.phase = "downloading_artifact";
    REQUIRE(display_progress(status) == 92);
    status.phase = "checking_model";
    status.progress = 99;
    REQUIRE(display_progress(status) == 97);
    status.phase = "checking_visual";
    REQUIRE(display_progress(status) == 98);

    status.phase.clear();
    status.state = "ready";
    REQUIRE(display_progress(status) == 100);
}

TEST_CASE("automatic printable palette roles remain deterministic and distinct",
          "[ModelGenerationPresentation]")
{
    const std::vector<std::string> palette {
        "#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0066FF", "#9933CC"
    };
    for (size_t count = 1; count <= palette.size(); ++count) {
        DYNAMIC_SECTION(count << " colors receive a complete stable role prefix") {
            const std::vector<std::string> active(palette.begin(), palette.begin() + count);
            const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(active);
            REQUIRE(roles.size() == count);
            for (size_t index = 0; index < PALETTE_ROLE_IDS.size(); ++index)
                CHECK(roles.count(PALETTE_ROLE_IDS[index]) == (index < count ? 1 : 0));
            std::set<std::string> assigned;
            for (const auto& [role, color] : roles) {
                CHECK(std::find(active.begin(), active.end(), color) != active.end());
                assigned.insert(color);
            }
            CHECK(assigned.size() == count);
            CHECK(automatic_palette_roles(active) == roles);
        }
    }

    const std::vector<std::string> legacy_palette(palette.begin(), palette.begin() + 4);
    const AIModelGenerationClient::PaletteRoles roles = automatic_palette_roles(legacy_palette);
    REQUIRE(roles.at("structure") == "#000000");
    REQUIRE(roles.at("light") == "#FFFFFF");
    REQUIRE(roles.at("primary") == "#00FF00");
    REQUIRE(roles.at("accent") == "#FF0000");
    REQUIRE(same_palette_color("#aBc123", "#AbC123"));
    REQUIRE_FALSE(same_palette_color("#ABC123", "#ABC124"));
    CHECK(automatic_palette_roles({"#000000", "#FFFFFF", "#FF0000", "#00FF00",
                                   "#0066FF", "#9933CC", "#00FFFF"}).empty());
    CHECK(automatic_palette_roles({"#000000", "invalid"}).empty());
    CHECK(automatic_palette_roles({"#000000", "#000000"}).empty());
}
