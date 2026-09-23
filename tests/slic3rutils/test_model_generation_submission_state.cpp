#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationSubmissionState.hpp"

using namespace Slic3r::GUI::ModelGenerationPresentation;

TEST_CASE("Submission rejection survives unchanged preview recovery and repeated rendering",
          "[ModelGenerationSubmissionState][SidecarRecovery]")
{
    SubmissionState state;
    const SubmissionContext request {"design-a", "hunyuan", 7};
    state.begin(request);
    const std::string error = "Model generation is not configured.";
    REQUIRE(state.retain_error(request, error));
    for (int poll = 0; poll < 3; ++poll) {
        state.reconcile(request, "awaiting_confirmation");
        CHECK(state.error(request) == error);
        CHECK(state.error(request) == error); // Ordinary rendering is read-only.
    }
    state.retain_error(request, "Later recovery failed.");
    CHECK(state.error(request) == error);
}

TEST_CASE("Old job provider and submission callbacks cannot replace the current rejection",
          "[ModelGenerationSubmissionState][SidecarRecovery]")
{
    SubmissionState state;
    const SubmissionContext current {"design-a", "tripo", 8};
    state.begin(current);
    REQUIRE(state.retain_error(current, "Current rejection"));
    for (const SubmissionContext& old : {SubmissionContext {"design-b", "tripo", 8},
                                        SubmissionContext {"design-a", "hunyuan", 8},
                                        SubmissionContext {"design-a", "tripo", 7}}) {
        CHECK_FALSE(old.matches(current.job_id, current.provider, current.sequence));
        CHECK_FALSE(state.retain_error(old, "Old rejection"));
        state.reconcile(old, "running");
        CHECK(state.error(current) == "Current rejection");
        CHECK(state.error(old).empty());
    }
}

TEST_CASE("Explicit retry and switching provider discard the previous submission rejection",
          "[ModelGenerationSubmissionState][SidecarRecovery]")
{
    SubmissionState state;
    const SubmissionContext first {"design-a", "hunyuan", 7};
    state.begin(first);
    state.retain_error(first, "Model generation is not configured.");
    const SubmissionContext retry {"design-a", "hunyuan", 8};
    state.begin(retry);
    CHECK(state.error(retry).empty());
    CHECK_FALSE(state.retain_error(first, "Late failure"));
    state.retain_error(retry, "Second rejection");
    const SubmissionContext other_provider {"design-a", "tripo", 9};
    state.begin(other_provider);
    CHECK(state.error(other_provider).empty());
    CHECK_FALSE(state.retain_error(retry, "Late failure"));
    state.retain_error(other_provider, "Third rejection");
    state.clear();
    CHECK(state.error(other_provider).empty());
    CHECK_FALSE(state.retain_error(other_provider, "Late failure after reset"));
}

TEST_CASE("Authoritative task advancement replaces rejection while preflight recovery retains it",
          "[ModelGenerationSubmissionState][SidecarRecovery]")
{
    for (const std::string terminal_or_active : {"queued", "running", "ready", "failed", "stopping", "stopped"}) {
        SubmissionState state;
        const SubmissionContext request {"design-a", "hunyuan", 7};
        state.begin(request);
        state.retain_error(request, "The generated image is not suitable for 3D input");
        state.reconcile(request, "awaiting_confirmation");
        CHECK_FALSE(state.error(request).empty());
        state.reconcile(request, terminal_or_active);
        CHECK(state.error(request).empty());
    }
}

TEST_CASE("Configuration rejection is distinguished from ambiguous transport and provider failure",
          "[ModelGenerationSubmissionState][SidecarRecovery]")
{
    CHECK(is_model_provider_not_configured("Model generation is not configured."));
    CHECK_FALSE(is_model_provider_not_configured("AI sidecar request timed out."));
    CHECK_FALSE(is_model_provider_not_configured("Model generation request failed with HTTP 503."));
    CHECK_FALSE(is_model_provider_not_configured("Tripo authentication failed."));
}
