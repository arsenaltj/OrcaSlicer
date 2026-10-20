#include "orca_slicing/recommendation_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>

TEST_CASE("Recommendation planner emits baseline and three native quality plans", "[OrcaSlicingSupport]")
{
    orca::slicing::LoadedProject project{
        Slic3r::Model{},
        Slic3r::DynamicPrintConfig::full_print_config(),
        "fixture.3mf",
        "",
        nlohmann::json::object()};

    const auto candidates = orca::slicing::recommendation_candidates(project);
    REQUIRE(candidates.size() == 4);
    CHECK(candidates[0].candidateId == "baseline");
    CHECK(candidates[1].candidateId == "balanced");
    CHECK(candidates[2].candidateId == "speed");
    CHECK(candidates[3].candidateId == "quality");
    CHECK(std::abs(candidates[2].config.option<Slic3r::ConfigOptionFloat>("layer_height")->value - 0.28) < 1e-9);
    CHECK(std::abs(candidates[3].config.option<Slic3r::ConfigOptionFloat>("layer_height")->value - 0.12) < 1e-9);
}

TEST_CASE("Recommendation selection consumes only validated native trial metrics", "[OrcaSlicingSupport]")
{
    const nlohmann::json trials = nlohmann::json::array({
        {{"candidateId", "baseline"}, {"quality", "standard"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 100.0}, {"filamentGrams", 20.0}}}},
        {{"candidateId", "speed"}, {"quality", "fast"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 70.0}, {"filamentGrams", 24.0}}}},
        {{"candidateId", "quality"}, {"quality", "fine"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 130.0}, {"filamentGrams", 18.0}}}},
        {{"candidateId", "proxy"}, {"quality", "fast"}, {"validated", false},
         {"metrics", {{"printTimeSeconds", 1.0}, {"filamentGrams", 1.0}}}}});

    CHECK(orca::slicing::select_recommendation(trials, "speed")["selectedCandidateId"] == "speed");
    CHECK(orca::slicing::select_recommendation(trials, "quality")["selectedCandidateId"] == "quality");
    CHECK(orca::slicing::select_recommendation(trials, "balanced")["selectedCandidateId"] == "speed");
}
