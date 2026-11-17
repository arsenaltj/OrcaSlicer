#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AIModelGenerationClient.hpp"

using Slic3r::GUI::AIModelGenerationClient;

TEST_CASE("Legacy Tripo choices restore the same standard face cap without editing saved history",
          "[ModelGenerationClient][Regression]")
{
    for (const int faces : {300000, 1000000, 2000000}) {
        for (const bool null_geometry : {false, true}) {
            DYNAMIC_SECTION("faces=" << faces << " null=" << null_geometry) {
                nlohmann::json saved = {{"face_limit", faces}, {"output_format", "obj"}};
                if (null_geometry) saved["geometry_quality"] = nullptr;
                const auto original = saved;
                for (int restore = 0; restore < 2; ++restore) {
                    const auto options = AIModelGenerationClient::restore_generation_options(saved);
                    CHECK(options.provider == "tripo");
                    CHECK(options.face_limit == (faces == 2000000 ? 1000000 : faces));
                    CHECK(options.geometry_quality == "standard");
                    CHECK(options.texture_quality == "standard");
                    CHECK(options.output_format == "obj");
                    CHECK(saved == original);
                }
            }
        }
    }
}

TEST_CASE("Explicit model choices survive restoration for review without changing cost or face target",
          "[ModelGenerationClient][Regression]")
{
    for (const std::string provider : {"tripo", "yoyoclaw"}) {
        for (const std::string geometry : {"standard", "detailed"}) {
            DYNAMIC_SECTION(provider << " " << geometry) {
                const nlohmann::json saved = {{"provider", provider}, {"face_limit", 2000000},
                    {"geometry_quality", geometry}, {"texture_quality", "extreme"}, {"output_format", "obj"}};
                const auto options = AIModelGenerationClient::restore_generation_options(saved);
                CHECK(options.provider == provider);
                CHECK(options.face_limit == 2000000);
                CHECK(options.geometry_quality == geometry);
                CHECK(options.texture_quality == "extreme");
                CHECK(options.output_format == "obj");
            }
        }
    }
}

TEST_CASE("Tripo legacy restoration does not change YOYOClaw choices",
          "[ModelGenerationClient][Regression]")
{
    const auto options = AIModelGenerationClient::restore_generation_options(
        {{"provider", "yoyoclaw"}, {"face_limit", 1000000}, {"geometry_quality", nullptr}});
    CHECK(options.provider == "yoyoclaw");
    CHECK(options.face_limit == 1000000);
    CHECK(options.geometry_quality == "standard");
}

TEST_CASE("Unsupported saved providers fall back to Tripo",
          "[ModelGenerationClient][Regression]")
{
    const auto options = AIModelGenerationClient::restore_generation_options(
        {{"provider", "removed-provider"}, {"face_limit", 2000000}, {"geometry_quality", "detailed"}});
    CHECK(options.provider == "tripo");
    CHECK(options.face_limit == 2000000);
    CHECK(options.geometry_quality == "detailed");
}

TEST_CASE("Optional provider details preserve terminal classification and legacy records", "[ModelGenerationClient]")
{
    auto job = nlohmann::json{{"id", "failed-job"}, {"source", "image"}, {"state", "failed"},
        {"provider_failure", {{"code", "provider_task_failed"}, {"category", "provider"},
            {"retryable", false}, {"ambiguous", false}}}};
    auto legacy = AIModelGenerationClient::parse_job(job);
    REQUIRE(legacy);
    CHECK(legacy->provider_detail_code.empty());
    CHECK(legacy->provider_detail_message.empty());
    for (const auto& detail : {nlohmann::json("FailedOperation.InnerError"), nlohmann::json::array(), nlohmann::json(nullptr)}) {
        DYNAMIC_SECTION(detail.dump()) {
            job["provider_failure"]["provider_code"] = detail;
            job["provider_failure"]["provider_message"] = "输入图片生成几何失败，请更换图片或重试";
            const auto status = AIModelGenerationClient::parse_job(job);
            REQUIRE(status);
            CHECK(status->state == "failed");
            CHECK(status->provider_error_code == "provider_task_failed");
            CHECK_FALSE(status->provider_error_retryable);
            CHECK_FALSE(status->provider_error_ambiguous);
            CHECK(status->provider_detail_message == "输入图片生成几何失败，请更换图片或重试");
            CHECK(status->provider_detail_code == (detail.is_string() ? detail.get<std::string>() : ""));
        }
    }
    job["provider_failure"]["provider_code"] = "invalid\ncode";
    job["provider_failure"]["provider_message"] = nlohmann::json::object();
    auto invalid = AIModelGenerationClient::parse_job(job);
    REQUIRE(invalid);
    CHECK(invalid->provider_detail_code.empty());
    CHECK(invalid->provider_detail_message.empty());
    job["provider_failure"]["provider_message"] = std::string(600, 'a') + "末尾";
    auto bounded = AIModelGenerationClient::parse_job(job);
    REQUIRE(bounded);
    CHECK(bounded->provider_detail_message.size() == 512);
}

TEST_CASE("Malformed optional quality reports do not hide a valid job or rewrite its report", "[ModelGenerationClient]")
{
    const auto valid = nlohmann::json{{"id", "design-ready"}, {"source", "image"},
        {"state", "awaiting_confirmation"}, {"preview", {{"ready", true}}},
        {"image_metrics", {{"score", 42.0}, {"palette_quality_ok", false},
            {"model_input_quality", {{"score", 19.65}, {"model_input_eligible", false},
                {"blockers", {"subject_cropped"}}}}}}};
    for (const auto& field : {"score", "mean_color_error", "small_region_ratio_after", "changed_pixel_ratio",
             "boundary_complexity", "minimum_feature_px", "meaningful_palette_count", "meaningful_subject_color_count",
             "printable_subject_area_ratio", "largest_subject_component_ratio", "largest_detached_subject_diagonal_ratio",
             "design_reference", "palette_quality_ok", "material_fragmentation_ok"}) {
        for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json::object(), nlohmann::json::array()}) {
            DYNAMIC_SECTION(field << "=" << malformed.dump()) {
                auto job = valid;
                job["image_metrics"][field] = malformed;
                const auto original = job;
                const auto status = AIModelGenerationClient::parse_job(job);
                REQUIRE(status);
                CHECK(status->id == "design-ready");
                CHECK(status->preview_ready);
                CHECK_FALSE(status->model_input_eligible);
                CHECK(status->model_input_blockers == std::vector<std::string>{"subject_cropped"});
                CHECK(job == original);
            }
        }
    }
    for (const auto& report : {"model_input_quality", "generation_input_quality"}) {
        for (const auto& field : {"score", "model_input_eligible", "blockers", "warnings"}) {
            DYNAMIC_SECTION(report << "." << field) {
                auto job = valid;
                job["image_metrics"][report] = {{"score", 19.65}, {"model_input_eligible", false},
                    {"blockers", {"subject_cropped"}}, {"warnings", {"review_recommended"}}};
                job["image_metrics"][report][field] = nlohmann::json::object();
                const auto original = job;
                const auto status = AIModelGenerationClient::parse_job(job);
                REQUIRE(status);
                CHECK(status->state == "awaiting_confirmation");
                CHECK(status->preview_ready);
                CHECK_FALSE(status->palette_quality_ok);
                if (std::string(field) != "model_input_eligible") CHECK_FALSE(status->model_input_eligible);
                CHECK(job == original);
            }
        }
    }
}
