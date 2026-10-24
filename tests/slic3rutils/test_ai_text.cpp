#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/Text/AITextClient.hpp"
#include "slic3r/GUI/AI/Text/AITextPolishState.hpp"

#include <string>

using Slic3r::GUI::AITextClient;
using Slic3r::GUI::AITextPolishState;

TEST_CASE("Text polish responses accept only the bounded capability contract", "[AIText]")
{
    AITextClient::PolishResult result;
    REQUIRE(AITextClient::parse_polish_result(
        R"({"status":"success","optimized_prompt":"clear cat","warnings":[],"diagnostic_id":"diagnostic-1"})",
        result));
    CHECK(result.status == "success");
    CHECK(result.optimized_prompt == "clear cat");

    CHECK_FALSE(AITextClient::parse_polish_result(
        R"({"status":"success","optimized_prompt":"","warnings":[],"diagnostic_id":"diagnostic-1"})",
        result));
    CHECK_FALSE(AITextClient::parse_polish_result(
        R"({"status":"needs_clarification","optimized_prompt":"guessed","warnings":[],"diagnostic_id":"diagnostic-1"})",
        result));
    CHECK_FALSE(AITextClient::parse_polish_result(
        R"({"status":"success","optimized_prompt":"clear cat","warnings":[],"diagnostic_id":"diagnostic-1","model":"secret"})",
        result));
}

TEST_CASE("Text capability availability stays independent from generation status", "[AIText]")
{
    AITextClient::Availability availability;
    REQUIRE(AITextClient::parse_availability(
        R"({"capability":"image_prompt_polish","available":true,"reason":"","diagnostic_id":"diagnostic-1"})",
        availability));
    CHECK(availability.available);

    REQUIRE(AITextClient::parse_availability(
        R"({"capability":"image_prompt_polish","available":false,"reason":"preferred_text_model_unavailable","diagnostic_id":"diagnostic-2"})",
        availability));
    CHECK_FALSE(availability.available);
    CHECK(availability.reason == "preferred_text_model_unavailable");
}

TEST_CASE("Prompt polish restore keeps the first original across repeated success", "[AIText]")
{
    AITextPolishState state;
    const auto first = state.begin();
    REQUIRE(state.active());
    REQUIRE(state.finish(first, "original"));
    REQUIRE(state.can_restore());

    const auto second = state.begin();
    REQUIRE(state.finish(second, "first polish"));
    const auto original = state.restore();
    REQUIRE(original.has_value());
    CHECK(*original == "original");
    CHECK_FALSE(state.can_restore());
}

TEST_CASE("Prompt polish cancellation rejects stale responses and can clear restore memory", "[AIText]")
{
    AITextPolishState state;
    const auto obsolete = state.begin();
    state.cancel(false);
    CHECK_FALSE(state.finish(obsolete, "obsolete"));

    const auto current = state.begin();
    REQUIRE(state.finish(current, "original"));
    REQUIRE(state.can_restore());

    const auto stale_retry = state.begin();
    state.cancel(false);
    CHECK_FALSE(state.finish(stale_retry, "replacement"));
    REQUIRE(state.can_restore());
    CHECK(state.restore() == std::optional<std::string>("original"));

    const auto replacement = state.begin();
    REQUIRE(state.finish(replacement, "new original"));
    state.cancel(true);
    CHECK_FALSE(state.can_restore());
    CHECK_FALSE(state.active());
}
