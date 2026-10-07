#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/AI/ModelGeneration/ModelGenerationClient.hpp"

using namespace Slic3r::AI::ModelGeneration;
namespace {
struct Observations {
    int generated {0}, queried {0}, remote_stops {0}, local_cancels {0}, downloads {0}, checked {0};
    std::string asset_id, artifact_hash;
    bool read_only {false};
    std::string job_id, prepared_prompt, output_format;
    ModelGenerationTypes::GenerationOptions options;
    ModelGenerationTypes::StatusFn pending;
};
class RecordingGenerationService final : public IModelGenerationService {
public:
    explicit RecordingGenerationService(Observations& observed) : calls(observed) {}
    const char* implementation_id() const noexcept override { return "recording-service"; }
    const char* implementation_version() const noexcept override { return "recording-service-v2"; }
    void generate(const std::string& job_id, const std::string& prepared_prompt,
        const std::vector<std::string>&, const GenerationOptions& options,
        StatusFn on_complete, ErrorFn) override
    {
        ++calls.generated; calls.job_id = job_id; calls.prepared_prompt = prepared_prompt; calls.options = options;
        calls.pending = std::move(on_complete);
    }
    void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn) override
    { ++calls.queried; calls.job_id = job_id; calls.pending = std::move(on_complete); }
    void get_latest(LatestFn on_complete, ErrorFn) override
    { on_complete(std::nullopt); }
    void check_saved_artifact(const std::string& asset_id, const std::string& sha256,
        StatusFn on_complete, ErrorFn, bool read_only = false) override
    { ++calls.checked; calls.asset_id = asset_id; calls.artifact_hash = sha256;
      calls.read_only = read_only; calls.pending = std::move(on_complete); }
    void stop(const std::string& job_id, StatusFn, ErrorFn) override
    { ++calls.remote_stops; calls.job_id = job_id; }
    void cancel_current() override { ++calls.local_cancels; }
    void download_artifact(const std::string& job_id, const std::string& format,
        const boost::filesystem::path& path, PathFn on_complete, ErrorFn) override
    { ++calls.downloads; calls.job_id = job_id; calls.output_format = format; on_complete(path); }
    void preprocess_text(const std::string& request_id, const std::string& prompt,
                          const std::vector<std::string>& palette, const PaletteRoles& palette_roles,
                          bool palette_recommendation_confirmed,
                          const std::string& style, const std::string& custom_style,
                          const ImagePrintSettings& print_settings,
                          StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override
    { if (on_error) on_error("unused test operation"); }

    void preprocess_image(const std::string& request_id, const std::string& instruction,
                           const boost::filesystem::path& image_path, const std::vector<std::string>& palette,
                           const PaletteRoles& palette_roles, bool palette_recommendation_confirmed,
                           const std::string& style, const std::string& custom_style,
                           const ImagePrintSettings& print_settings,
                           StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override
    { if (on_error) on_error("unused test operation"); }

    void recommend_text_palette(const std::string& request_id, const std::string& prompt,
                                const std::string& style, const std::string& custom_style,
                                size_t palette_color_count,
                                const ImagePrintSettings& print_settings,
                                StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                const GenerationOptions& options) override
    { if (on_error) on_error("unused test operation"); }

    void recommend_image_palette(const std::string& request_id, const std::string& instruction,
                                 const boost::filesystem::path& image_path,
                                 const std::string& style, const std::string& custom_style,
                                 size_t palette_color_count,
                                 const ImagePrintSettings& print_settings,
                                 StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                 const GenerationOptions& options) override
    { if (on_error) on_error("unused test operation"); }

    void recommend_image_style(const std::string& prompt,
                               const boost::filesystem::path& image_path,
                               StyleRecommendationFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void confirm_palette(const std::string& job_id, const std::vector<std::string>& palette,
                         const PaletteRoles& palette_roles, StatusFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void update_generation_options(const std::string& job_id, const GenerationOptions& options,
                                   StatusFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void retexture(const std::string& reference_job_id, const std::string& geometry_job_id,
                   StatusFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void recheck(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void visual_review(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void download_image_output(const std::string& job_id, const std::string& output,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void download_input(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void download_color_intent(const std::string& job_id, const std::string& schema,
                               const std::string& sha256, const boost::filesystem::path& artifact_path,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { if (on_error) on_error("unused test operation"); }

    void record_journey_event(const std::string& event, const std::string& job_id = {}) override
    {}

private:
    Observations& calls;
};
}

TEST_CASE("Generation service replacement preserves the confirmed options and real job identity", "[GenerationService]")
{
    Observations calls;
    ModelGenerationClient client(std::make_unique<RecordingGenerationService>(calls));
    ModelGenerationClient::GenerationOptions options;
    options.provider = "hunyuan"; options.face_limit = 300000; options.geometry_quality = "standard";
    ModelGenerationClient::JobStatus received;
    client.generate("existing-job", "confirmed-prompt", {"#112233"}, options,
        [&](auto status) { received = std::move(status); }, [](auto) { FAIL("Unexpected error"); });
    REQUIRE(calls.generated == 1);
    CHECK(calls.job_id == "existing-job");
    CHECK(calls.prepared_prompt == "confirmed-prompt");
    CHECK(calls.options.provider == options.provider);
    CHECK(calls.options.face_limit == options.face_limit);
    CHECK(options.face_limit == 300000);
    ModelGenerationClient::JobStatus output;
    output.id = "existing-job"; output.provider_name = "hunyuan"; output.provider_task_id = "real-provider-task";
    output.provider_error_ambiguous = true; output.state = "failed";
    calls.pending(std::move(output));
    CHECK(received.id == "existing-job");
    CHECK(received.provider_task_id == "real-provider-task");
    CHECK(received.provider_error_ambiguous);
    CHECK(received.service_implementation_id == "recording-service");
    CHECK(received.service_implementation_version == "recording-service-v2");
    CHECK(calls.generated == 1); // Unknown submission is returned, not resubmitted.
    client.cancel_current();
    CHECK(calls.local_cancels == 1);
    CHECK(calls.remote_stops == 0);
    client.stop("existing-job", {}, {});
    CHECK(calls.remote_stops == 1);
}

TEST_CASE("Saved artifact checks preserve their identity and read-only intent across services", "[GenerationService]")
{
    const bool read_only = GENERATE(false, true);
    Observations calls;
    ModelGenerationClient client(std::make_unique<RecordingGenerationService>(calls));
    ModelGenerationClient::JobStatus received;
    const std::string hash(64, 'a');
    client.check_saved_artifact("existing-local-asset", hash,
        [&](auto status) { received = std::move(status); }, [](auto) { FAIL("Unexpected error"); }, read_only);
    REQUIRE(calls.checked == 1);
    CHECK(calls.asset_id == "existing-local-asset");
    CHECK(calls.artifact_hash == hash);
    CHECK(calls.read_only == read_only);
    ModelGenerationClient::JobStatus result;
    result.model_quality.artifact_sha256 = hash;
    result.model_quality.units = "mm";
    result.model_quality.report_metrics["minimum_thickness_mm"] = 0.4;
    calls.pending(std::move(result));
    CHECK(received.model_quality.artifact_sha256 == hash);
    CHECK(received.model_quality.units == "mm");
    CHECK_THAT(received.model_quality.report_metrics.at("minimum_thickness_mm"),
               Catch::Matchers::WithinAbs(0.4, 1e-12));
    CHECK(received.service_implementation_id == "recording-service");
    CHECK(calls.generated == 0);
}

TEST_CASE("Generation recovery and downloads never start a paid task", "[GenerationService]")
{
    Observations calls;
    ModelGenerationClient::JobStatus received;
    bool latest_called = false;
    boost::filesystem::path downloaded;
    {
        ModelGenerationClient client(std::make_unique<RecordingGenerationService>(calls));
        client.get_latest([&](auto latest) { latest_called = true; CHECK_FALSE(latest.has_value()); }, {});
        client.download_artifact("history-job", "glb", "local-copy.glb",
            [&](auto path) { downloaded = std::move(path); }, {});
        client.get_status("history-job", [&](auto status) { received = std::move(status); }, {});
    }
    // Metadata callbacks capture values, not a pointer to the destroyed facade.
    ModelGenerationClient::JobStatus historical;
    historical.id = "history-job"; historical.generation_options.face_limit = 1000000;
    calls.pending(std::move(historical));
    CHECK(latest_called);
    CHECK(downloaded == boost::filesystem::path("local-copy.glb"));
    CHECK(calls.output_format == "glb");
    CHECK(calls.generated == 0);
    CHECK(calls.queried == 1);
    CHECK(received.id == "history-job");
    CHECK(received.generation_options.face_limit == 1000000);
    CHECK(received.service_implementation_version == "recording-service-v2");
}
