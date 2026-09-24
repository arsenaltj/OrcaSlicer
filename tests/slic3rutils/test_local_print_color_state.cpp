#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"

using namespace Slic3r;

namespace {
AI::LocalPrintColorResult direct_result()
{
    AI::LocalPrintColorResult result;
    result.algorithm_version = "region-direct-v5";
    result.source_sha256 = std::string(64, 'a');
    result.geometry_id = "geometry-local-color";
    result.material_fingerprint = "materials-v1";
    result.process_fingerprint = "process-v1";
    result.requested_color_count = 2;
    result.source_color_count = 2;
    result.sampled_source_color_count = 2;
    result.face_count = 2;
    result.physical_channels = {
        {0, "#FFFFFF", "PLA", true},
        {1, "#000000", "PLA", true},
    };
    AI::PrintColorTarget white;
    white.source = {1.f, 1.f, 1.f}; white.output = white.source; white.area = .5;
    white.physical_slot = 0; white.evidence = AI::ColorEvidence::Measured;
    white.executable = true; white.within_tolerance = true;
    AI::PrintColorTarget black;
    black.source = {0.f, 0.f, 0.f}; black.output = black.source; black.area = .5;
    black.physical_slot = 1; black.evidence = AI::ColorEvidence::Measured;
    black.executable = true; black.within_tolerance = true;
    result.targets = {white, black};
    result.face_targets = {0, 1};
    result.confirmed = true;
    return result;
}
}

TEST_CASE("Local print color state round-trips a confirmed direct result", "[LocalPrintColorState]")
{
    const auto source = direct_result();
    std::string error;
    REQUIRE(source.valid(error));
    AI::LocalPrintColorResult restored;
    REQUIRE(GUI::LocalPrintColorState::decode(
        GUI::LocalPrintColorState::encode(source), source.source_sha256, source.geometry_id,
        source.material_fingerprint, source.process_fingerprint, restored, error));
    CHECK(restored.source_sha256 == source.source_sha256);
    CHECK(restored.geometry_id == source.geometry_id);
    CHECK(restored.face_targets == source.face_targets);
    CHECK(restored.physical_channels.size() == 2);
    CHECK(restored.confirmed);
}

TEST_CASE("Local print color state rejects stale identities without mutating destination", "[LocalPrintColorState]")
{
    const auto source = direct_result();
    AI::LocalPrintColorResult destination = source;
    destination.parent_version = "keep-this";
    std::string error;
    auto encoded = GUI::LocalPrintColorState::encode(source);
    REQUIRE_FALSE(GUI::LocalPrintColorState::decode(
        encoded, std::string(64, 'b'), source.geometry_id, source.material_fingerprint,
        source.process_fingerprint, destination, error));
    CHECK(destination.parent_version == "keep-this");
    CHECK_FALSE(error.empty());
}
