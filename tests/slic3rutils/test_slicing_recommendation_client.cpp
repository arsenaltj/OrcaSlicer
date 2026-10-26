#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AI/SmartSlicing/SlicingRecommendationClient.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/GUI/AI/Orca/SmartSliceApplier.hpp"

#include <algorithm>

using Slic3r::AI::SmartSlicing::SliceMode;
using Slic3r::GUI::SlicingRecommendationClient;
using Slic3r::GUI::SlicingRecommendationRequest;
using Slic3r::GUI::SlicingRecommendationResult;
using Json = nlohmann::json;

namespace {

SlicingRecommendationRequest request()
{
    SlicingRecommendationRequest value;
    value.request_id = "smart-slicing-12";
    value.object_count = 2;
    value.metrics.bounding_box = {0.0, 0.0, 0.0, 80.0, 50.0, 40.0};
    value.metrics.volume_mm3 = 45000.0;
    value.metrics.surface_area_mm2 = 12000.0;
    value.metrics.triangle_count = 8200;
    value.metrics.closed_mesh = true;
    value.metrics.high_overhang_area_ratio_45_deg = 0.08;
    value.metrics.severe_overhang_area_ratio_60_deg = 0.02;
    value.metrics.top_micro_slope_surface_ratio = 0.10;
    value.metrics.thin_wall_feature_ratio = 0.03;
    value.machine.nozzle_diameter_mm = 0.4;
    value.machine.minimum_layer_height_mm = 0.05;
    value.machine.maximum_layer_height_mm = 0.28;
    value.machine.maximum_outer_wall_speed_mm_s = 120.0;
    value.filament.material_types = {"PLA"};
    value.local_baselines.completed = true;
    value.local_baselines.metrics = value.metrics;
    auto& balanced = value.local_baselines.recommendations[0];
    balanced.mode = SliceMode::BALANCED;
    balanced.layer_height_mm = 0.20;
    balanced.wall_loops = 3;
    balanced.sparse_infill_density_percent = 15.0;
    balanced.sparse_infill_pattern = "gyroid";
    balanced.outer_wall_speed_mm_s = 80.0;
    balanced.enable_support = false;
    balanced.support_type = "none";
    balanced.support_threshold_angle_deg = 50.0;
    balanced.line_width_mm = 0.42;
    balanced.arachne_wall_generator = true;
    auto& speed = value.local_baselines.recommendations[1];
    speed = balanced;
    speed.mode = SliceMode::SPEED;
    speed.layer_height_mm = 0.24;
    speed.wall_loops = 2;
    speed.sparse_infill_density_percent = 10.0;
    speed.sparse_infill_pattern = "grid";
    speed.outer_wall_speed_mm_s = 100.0;
    auto& quality = value.local_baselines.recommendations[2];
    quality = balanced;
    quality.mode = SliceMode::QUALITY;
    quality.layer_height_mm = 0.12;
    quality.outer_wall_speed_mm_s = 60.0;
    return value;
}

Json response()
{
    Json result = {
        {"request_id", "smart-slicing-12"},
        {"status", "success"},
        {"recommendations", {
            {{"goal", "balanced"}, {"changes", Json::array()}, {"summary", "保持综合基线"}},
            {{"goal", "speed"}, {"changes", {
                {{"parameter", "outer_wall_speed"}, {"value", 110.0}, {"rationale", "在设备能力内适度提速"}},
            }}, {"summary", "提高外墙速度"}},
            {{"goal", "quality"}, {"changes", {
                {{"parameter", "wall_loops"}, {"value", 4}, {"rationale", "薄壁统计提示增强外壳"}},
            }}, {"summary", "增强外壳"}},
        }},
        {"warnings", Json::array()},
        {"diagnostic_id", "11111111-2222-3333-4444-555555555555"},
    };
    return result;
}

} // namespace

TEST_CASE("Cloud slicing request contains only deidentified bounded summaries", "[SmartSlicing][CloudRecommendation]")
{
    const Json payload = SlicingRecommendationClient::build_request_json(request());
    CHECK(payload.size() == 6);
    CHECK_FALSE(payload.contains("goal"));
    REQUIRE(payload["baselines"].size() == 3);
    CHECK(payload["baselines"][0]["goal"] == "balanced");
    CHECK(payload["baselines"][1]["goal"] == "speed");
    CHECK(payload["baselines"][2]["goal"] == "quality");
    CHECK(payload["model"]["object_count"] == 2);
    CHECK(payload["material"]["types"] == Json::array({"PLA"}));
    CHECK(payload["policy"]["allowed_changes"].size() == 7);
    const std::string serialized = payload.dump();
    CHECK(serialized.find("path") == std::string::npos);
    CHECK(serialized.find("vertices") == std::string::npos);
    CHECK(serialized.find("preset_name") == std::string::npos);
}

TEST_CASE("Cloud slicing response merges seven allowed parameters into local baselines", "[SmartSlicing][CloudRecommendation]")
{
    const auto input = request();
    SlicingRecommendationResult result;
    std::string diagnostic;
    REQUIRE(SlicingRecommendationClient::parse_result(response().dump(), input, result, &diagnostic));
    CHECK(diagnostic.empty());
    REQUIRE(result.analysis.find(SliceMode::BALANCED) != nullptr);
    REQUIRE(result.analysis.find(SliceMode::SPEED) != nullptr);
    REQUIRE(result.analysis.find(SliceMode::QUALITY) != nullptr);
    CHECK(result.analysis.find(SliceMode::BALANCED)->outer_wall_speed_mm_s == 80.0);
    CHECK(result.analysis.find(SliceMode::SPEED)->outer_wall_speed_mm_s == 110.0);
    CHECK(result.analysis.find(SliceMode::QUALITY)->wall_loops == 4);
    CHECK(result.analysis.find(SliceMode::QUALITY)->support_threshold_angle_deg == 50.0);
    CHECK(result.analysis.find(SliceMode::QUALITY)->line_width_mm == 0.42);
    CHECK(result.analysis.find(SliceMode::QUALITY)->arachne_wall_generator);
    CHECK_FALSE(result.analysis.find(SliceMode::QUALITY)->adaptive_layer_height);
}

TEST_CASE("Every advertised cloud fill and support enum produces native Orca config", "[SmartSlicing][CloudRecommendation][SmartSliceApplier]")
{
    const auto input = request();
    const auto payload = SlicingRecommendationClient::build_request_json(input);
    const auto& ranges = payload["policy"]["ranges"];
    auto base = Slic3r::DynamicPrintConfig::full_print_config();
    for (size_t goal = 0; goal < 3; ++goal) {
        for (const auto& field : {"sparse_infill_pattern", "support_type"}) {
            for (const auto& value : ranges[field]["values"]) {
                CAPTURE(goal, field, value);
                auto body = response();
                auto& changes = body["recommendations"][goal]["changes"];
                changes = {{{"parameter", field}, {"value", value}, {"rationale", "检查原生参数兼容性"}}};
                if (std::string(field) == "support_type")
                    changes.push_back({{"parameter", "enable_support"}, {"value", value != "none"}, {"rationale", "匹配支撑开关"}});
                SlicingRecommendationResult result;
                std::string diagnostic;
                REQUIRE(SlicingRecommendationClient::parse_result(body.dump(), input, result, &diagnostic));
                for (const auto& plan : result.analysis.recommendations) {
                    Slic3r::DynamicPrintConfig output;
                    REQUIRE(Slic3r::GUI::SmartSliceApplier::BuildConfig(base, plan, output, &diagnostic));
                    CHECK(diagnostic.empty());
                    if (static_cast<size_t>(plan.mode) == goal &&
                        (std::string(field) != "support_type" || value != "none")) {
                        REQUIRE(output.option(field) != nullptr);
                        CHECK(output.option(field)->serialize() == value.get<std::string>());
                    }
                }
            }
        }
    }
}

TEST_CASE("Legacy plural fill names normalize across cloud requests responses and native application", "[SmartSlicing][CloudRecommendation][SmartSliceApplier]")
{
    auto input = request();
    input.local_baselines.recommendations[0].sparse_infill_pattern = "lines";
    const auto payload = SlicingRecommendationClient::build_request_json(input);
    CHECK(payload["baselines"][0]["sparse_infill_pattern"] == "line");
    const auto& values = payload["policy"]["ranges"]["sparse_infill_pattern"]["values"];
    CHECK(std::find(values.begin(), values.end(), "line") != values.end());
    CHECK(std::find(values.begin(), values.end(), "lines") == values.end());
    auto body = response();
    body["recommendations"][1]["changes"] = {
        {{"parameter", "sparse_infill_pattern"}, {"value", "lines"}, {"rationale", "兼容旧名称"}},
    };
    SlicingRecommendationResult result;
    REQUIRE(SlicingRecommendationClient::parse_result(body.dump(), input, result));
    CHECK(result.analysis.recommendations[0].sparse_infill_pattern == "line");
    CHECK(result.analysis.recommendations[1].sparse_infill_pattern == "line");
    auto base = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::DynamicPrintConfig output;
    REQUIRE(Slic3r::GUI::SmartSliceApplier::BuildConfig(base, input.local_baselines.recommendations[0], output));
    CHECK(output.opt_enum<Slic3r::InfillPattern>("sparse_infill_pattern") == Slic3r::ipLine);
    input.local_baselines.recommendations[0].sparse_infill_pattern = "unknown-pattern";
    std::string diagnostic;
    CHECK_FALSE(Slic3r::GUI::SmartSliceApplier::BuildConfig(base, input.local_baselines.recommendations[0], output, &diagnostic));
    CHECK(diagnostic == "smart_slice_infill_pattern_unavailable");
}

TEST_CASE("Rejected cloud enums retain their goal parameter value and diagnostic identity", "[SmartSlicing][CloudRecommendation]")
{
    auto body = response();
    body["recommendations"][1]["changes"] = {
        {{"parameter", "sparse_infill_pattern"}, {"value", "unknown-pattern"}, {"rationale", "无效图案"}},
    };
    SlicingRecommendationResult result;
    SlicingRecommendationClient::Error error;
    std::string diagnostic;
    CHECK_FALSE(SlicingRecommendationClient::parse_result(body.dump(), request(), result, &diagnostic, &error));
    CHECK(diagnostic == "cloud_enum_unavailable");
    CHECK(error.goal == "speed");
    CHECK(error.parameter == "sparse_infill_pattern");
    CHECK(error.value == "\"unknown-pattern\"");
    CHECK(error.diagnostic_id == body["diagnostic_id"].get<std::string>());
}

TEST_CASE("Cloud slicing response rejects stale duplicate forbidden and excessive changes", "[SmartSlicing][CloudRecommendation]")
{
    const auto input = request();
    std::vector<Json> invalid;
    Json stale = response();
    stale["request_id"] = "smart-slicing-11";
    invalid.push_back(stale);
    Json duplicate = response();
    duplicate["recommendations"].push_back(duplicate["recommendations"][0]);
    invalid.push_back(duplicate);
    Json forbidden = response();
    forbidden["recommendations"][0]["changes"] = {{{"parameter", "line_width"}, {"value", 0.5}, {"rationale", "越权"}}};
    invalid.push_back(forbidden);
    Json excessive = response();
    excessive["recommendations"][0]["changes"] = {{{"parameter", "layer_height"}, {"value", 0.29}, {"rationale", "变化过大"}}};
    invalid.push_back(excessive);
    Json extra = response();
    extra["recommendations"][0]["estimated_time"] = 100;
    invalid.push_back(extra);
    for (const Json& candidate : invalid) {
        SlicingRecommendationResult result;
        std::string diagnostic;
        CAPTURE(candidate.dump());
        CHECK_FALSE(SlicingRecommendationClient::parse_result(candidate.dump(), input, result, &diagnostic));
        CHECK_FALSE(diagnostic.empty());
    }
}

TEST_CASE("Cloud slicing response requires consistent support values", "[SmartSlicing][CloudRecommendation]")
{
    const auto input = request();
    Json invalid = response();
    invalid["recommendations"][0]["changes"] = {
        {{"parameter", "support_type"}, {"value", "tree(auto)"}, {"rationale", "增加树状支撑"}},
    };
    SlicingRecommendationResult result;
    std::string diagnostic;
    CHECK_FALSE(SlicingRecommendationClient::parse_result(invalid.dump(), input, result, &diagnostic));
    CHECK(diagnostic == "cloud_support_configuration_invalid");
}

TEST_CASE("Cloud slicing attempt gate rejects canceled and stale workspace responses", "[SmartSlicing][CloudRecommendation]")
{
    Slic3r::GUI::CloudRecommendationGate gate;
    const Slic3r::AI::SmartSlicing::WorkspaceRevision original {1, 2, 3, "original"};
    const auto first = gate.begin(original);
    CHECK(gate.accepts(first, original));
    const auto second = gate.begin(original);
    CHECK_FALSE(gate.accepts(first, original));
    CHECK(gate.accepts(second, original));
    auto changed = original;
    ++changed.config_revision;
    changed.fingerprint = "changed";
    CHECK_FALSE(gate.accepts(second, changed));
    gate.cancel();
    CHECK_FALSE(gate.accepts(second, original));
}

TEST_CASE("Cloud slicing requires all three unique goal results in one response", "[SmartSlicing][CloudRecommendation]")
{
    const auto input = request();
    for (size_t count : {size_t(1), size_t(2)}) {
        auto partial = response();
        while (partial["recommendations"].size() > count) partial["recommendations"].erase(count);
        SlicingRecommendationResult result;
        CHECK_FALSE(SlicingRecommendationClient::parse_result(partial.dump(), input, result));
    }
    auto duplicate = response();
    duplicate["recommendations"][1] = duplicate["recommendations"][0];
    SlicingRecommendationResult result;
    CHECK_FALSE(SlicingRecommendationClient::parse_result(duplicate.dump(), input, result));
    auto reordered = response();
    std::swap(reordered["recommendations"][0], reordered["recommendations"][2]);
    REQUIRE(SlicingRecommendationClient::parse_result(reordered.dump(), input, result));
    CHECK(result.analysis.find(SliceMode::SPEED)->outer_wall_speed_mm_s == 110.0);
    CHECK(result.analysis.find(SliceMode::QUALITY)->wall_loops == 4);
}

TEST_CASE("Exact estimates belong to the actual parameters as well as the goal and inputs", "[SmartSlicing][CloudRecommendation]")
{
    using namespace Slic3r::GUI;
    using Goal = Slic3r::AI::SmartSlicing::RecommendationGoal;
    auto parameters = request().local_baselines.recommendations[0];
    ExactEstimateView estimate;
    estimate.goal = Goal::Balanced;
    estimate.revision = {1,2,3,"inputs"};
    estimate.parameters = parameters;
    CHECK(exact_estimate_matches(estimate, estimate.revision, Goal::Balanced, parameters));
    parameters.layer_height_mm = 0.12;
    CHECK_FALSE(exact_estimate_matches(estimate, estimate.revision, Goal::Balanced, parameters));
    CHECK_FALSE(exact_estimate_matches(estimate, estimate.revision, Goal::Quality, *estimate.parameters));
    auto changed = estimate.revision; ++changed.plate_revision;
    CHECK_FALSE(exact_estimate_matches(estimate, changed, Goal::Balanced, *estimate.parameters));
}
