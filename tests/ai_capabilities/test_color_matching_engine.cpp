#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ColorMatching/ColorMatchingEngine.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPackMapping.hpp"

using namespace Slic3r;
namespace Matching = AI::ColorMatching;

namespace {
Matching::Input color_input()
{
    Matching::Input input;
    input.identity.source_sha256 = std::string(64, 'a');
    input.identity.geometry_id = "two-face-model";
    input.identity.material_fingerprint = "two-materials";
    input.identity.process_fingerprint = "default-process";
    input.identity.requested_color_count = 2;
    input.identity.face_count = 2;
    input.identity.physical_channels = {{3, "#FF0000", "PLA", true}, {11, "#0000FF", "PLA", true}};
    input.faces = {{{1, 0, 0}, 9}, {{0, 0, 1}, 1}};
    input.identity.regions = {{"locked-red", "", "user selection", 1, true, {0}, true, 3}};
    return input;
}

class RecordingEngine final : public Matching::IColorMatchingEngine {
public:
    mutable size_t calls {0};
    const char* algorithm_id() const noexcept override { return "recording"; }
    const char* algorithm_version(const Matching::Input&) const noexcept override { return "recording-v1"; }
    Matching::Computation compute(const Matching::Input& input) const override
    {
        ++calls;
        auto result = Matching::baseline_engine()->compute(input);
        result.result.confirmed = true; // The public boundary still returns a candidate.
        return result;
    }
};
}

TEST_CASE("Color matching retains sparse physical slots and leaves the input unchanged", "[ColorMatchingEngine]")
{
    auto input = color_input();
    input.identity.confirmed = true;
    const auto result = Matching::compute(input);
    REQUIRE(result.ok());
    REQUIRE(result.result.face_targets.size() == 2);
    REQUIRE(result.result.targets.size() == 2);
    CHECK(result.result.targets[result.result.face_targets[0]].physical_slot == std::optional<size_t>(3));
    CHECK(result.result.targets[result.result.face_targets[1]].physical_slot == std::optional<size_t>(11));
    CHECK(result.algorithm_id == "region-matching");
    CHECK(result.result.algorithm_version == "region-direct-v5");
    CHECK_FALSE(result.result.confirmed);
    CHECK(input.identity.confirmed);
    CHECK(input.identity.targets.empty());
    CHECK(input.identity.face_targets.empty());
    CHECK(input.identity.material_fingerprint == result.result.material_fingerprint);
    CHECK(input.identity.process_fingerprint == result.result.process_fingerprint);
}

TEST_CASE("Layered matching retains its persisted algorithm variant", "[ColorMatchingEngine]")
{
    auto input = color_input();
    input.identity.requested_color_count = 7;
    const auto result = Matching::compute(input);
    REQUIRE(result.ok());
    CHECK(result.result.mode == AI::PrintColorMode::Layered);
    CHECK(result.result.algorithm_version == "region-layered-v2");
    CHECK(result.result.targets[result.result.face_targets[0]].physical_slot == std::optional<size_t>(3));
}

TEST_CASE("The desktop compatibility entry uses the strategy captured for the operation", "[ColorMatchingEngine]")
{
    auto input = color_input();
    const auto captured = std::make_shared<RecordingEngine>();
    input.engine = captured;
    const auto operation = input;
    const auto later = std::make_shared<RecordingEngine>();
    input.engine = later;
    const auto result = GUI::LocalPrintColorMatching::compute(operation);
    REQUIRE(result.ok());
    CHECK(captured->calls == 1);
    CHECK(later->calls == 0);
    CHECK(result.algorithm_id == "recording");
    CHECK(result.result.algorithm_version == "recording-v1");
    CHECK_FALSE(result.result.confirmed);
    CHECK(result.result.targets[result.result.face_targets[0]].physical_slot == std::optional<size_t>(3));
}

TEST_CASE("Color matching preserves cancellation and rejects mismatched geometry", "[ColorMatchingEngine]")
{
    auto input = color_input();
    input.cancelled = [] { return true; };
    const auto cancelled = Matching::compute(input);
    CHECK(cancelled.cancelled);
    CHECK_FALSE(cancelled.ok());
    CHECK_FALSE(cancelled.result.confirmed);

    input.cancelled = {};
    ++input.identity.face_count;
    const auto invalid = Matching::compute(input);
    CHECK_FALSE(invalid.ok());
    CHECK(invalid.error == "Face samples do not match the source geometry.");
    CHECK_FALSE(invalid.result.confirmed);
}
