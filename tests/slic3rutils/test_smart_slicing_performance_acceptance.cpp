#include "slic3r/AI/SmartSlicing/Domain/PerformanceAcceptance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

template<class T>
MetricValue<T> known(T value, std::initializer_list<const char*> evidence = {"fixture"})
{
    std::vector<std::string> sources;
    for (const char* source : evidence)
        sources.emplace_back(source);
    return MetricValue<T>::known(std::move(value), std::move(sources));
}

PerformanceAcceptanceInput complete_input()
{
    PerformanceAcceptanceInput input;
    input.first_card_elapsed_seconds = known(120.0, {"timer"});
    input.total_elapsed_seconds = known(600.0, {"timer"});
    input.estimated_time_seconds = known(100.0, {"estimate"});
    input.measured_time_seconds = known(100.0, {"print"});
    input.estimated_material_volume_mm3 = known(100.0, {"estimate"});
    input.measured_material_volume_mm3 = known(100.0, {"print"});
    input.speed_baseline_physical_time_seconds = known(100.0, {"baseline"});
    input.speed_candidate_physical_time_seconds = known(90.0, {"candidate"});
    input.dimensional_nominal_mm = known(100.0, {"drawing"});
    input.dimensional_error_mm = known(0.2, {"measurement"});
    input.appearance_baseline_score = known(4.0, {"baseline"});
    input.appearance_candidate_score = known(4.0, {"candidate"});
    input.strength_retention_ratio = known(0.95, {"coupon"});
    input.multicolor_error_count = known<size_t>(0, {"inspection"});
    input.collision_count = known<size_t>(0, {"inspection"});
    return input;
}

} // namespace

TEST_CASE("Performance acceptance passes exact boundaries", "[AI][SmartSlicing][D8T1]")
{
    const auto result = evaluate_performance_acceptance(complete_input());
    CHECK(result.status == PerformanceAcceptanceStatus::Passed);
    CHECK(result.first_card_latency.known());
    CHECK(result.total_hard_timeout.known());
    CHECK(result.speed_physical_time_reduction.known());
    CHECK(result.strength_retention.known());
}

TEST_CASE("Performance timeouts reject values over the fixed limits", "[AI][SmartSlicing][D8T1]")
{
    auto first_card = complete_input();
    first_card.first_card_elapsed_seconds = known(120.001, {"timer"});
    CHECK(evaluate_performance_acceptance(first_card).status == PerformanceAcceptanceStatus::Failed);
    CHECK(std::find(evaluate_performance_acceptance(first_card).diagnostic_codes.begin(),
                    evaluate_performance_acceptance(first_card).diagnostic_codes.end(),
                    "first_card_timeout") != evaluate_performance_acceptance(first_card).diagnostic_codes.end());

    auto total = complete_input();
    total.total_elapsed_seconds = known(600.001, {"timer"});
    CHECK(evaluate_performance_acceptance(total).status == PerformanceAcceptanceStatus::Failed);
}

TEST_CASE("Performance estimate, speed, dimensional, appearance and strength boundaries are enforced",
          "[AI][SmartSlicing][D8T1]")
{
    auto time = complete_input();
    time.estimated_time_seconds = known(115.0, {"estimate"});
    CHECK(evaluate_performance_acceptance(time).estimated_time_error.value == true);
    time.estimated_time_seconds = known(115.01, {"estimate"});
    CHECK(!*evaluate_performance_acceptance(time).estimated_time_error.value);

    auto material = complete_input();
    material.estimated_material_volume_mm3 = known(110.0, {"estimate"});
    CHECK(*evaluate_performance_acceptance(material).estimated_material_error.value);
    material.estimated_material_volume_mm3 = known(110.01, {"estimate"});
    CHECK(!*evaluate_performance_acceptance(material).estimated_material_error.value);

    auto speed = complete_input();
    speed.speed_candidate_physical_time_seconds = known(90.001, {"candidate"});
    CHECK(!*evaluate_performance_acceptance(speed).speed_physical_time_reduction.value);

    auto dimensions = complete_input();
    dimensions.dimensional_nominal_mm = known(20.0, {"drawing"});
    dimensions.dimensional_error_mm = known(0.2, {"measurement"});
    CHECK(*evaluate_performance_acceptance(dimensions).dimensional_error.value);
    dimensions.dimensional_error_mm = known(0.201, {"measurement"});
    CHECK(!*evaluate_performance_acceptance(dimensions).dimensional_error.value);
    dimensions.dimensional_nominal_mm = known(100.0, {"drawing"});
    dimensions.dimensional_error_mm = known(0.5, {"measurement"});
    CHECK(*evaluate_performance_acceptance(dimensions).dimensional_error.value);
    dimensions.dimensional_error_mm = known(0.501, {"measurement"});
    CHECK(!*evaluate_performance_acceptance(dimensions).dimensional_error.value);

    auto appearance = complete_input();
    appearance.appearance_candidate_score = known(3.99, {"candidate"});
    CHECK(!*evaluate_performance_acceptance(appearance).appearance_not_worse_than_baseline.value);

    auto strength = complete_input();
    strength.strength_retention_ratio = known(0.9499, {"coupon"});
    CHECK(!*evaluate_performance_acceptance(strength).strength_retention.value);
}

TEST_CASE("Performance dimension without a nominal declaration is not applicable", "[AI][SmartSlicing][D8T1]")
{
    auto input = complete_input();
    input.dimensional_nominal_mm = MetricValue<double>::not_applicable({"no_nominal_dimension"});
    input.dimensional_error_mm = MetricValue<double>::not_applicable({"no_nominal_dimension"});
    const auto result = evaluate_performance_acceptance(input);
    CHECK(result.dimensional_error.availability == MetricAvailability::NotApplicable);
    CHECK(result.status == PerformanceAcceptanceStatus::Passed);
}

TEST_CASE("Performance evidence, schema and non-finite values fail closed", "[AI][SmartSlicing][D8T1]")
{
    auto missing = complete_input();
    missing.total_elapsed_seconds = MetricValue<double>::known(1.0);
    const auto missing_result = evaluate_performance_acceptance(missing);
    CHECK(missing_result.status == PerformanceAcceptanceStatus::Failed);
    CHECK(std::find(missing_result.diagnostic_codes.begin(), missing_result.diagnostic_codes.end(),
                    "performance_evidence_source_missing:total_elapsed_seconds") != missing_result.diagnostic_codes.end());

    auto non_finite = complete_input();
    non_finite.total_elapsed_seconds = known(std::numeric_limits<double>::quiet_NaN(), {"timer"});
    CHECK(evaluate_performance_acceptance(non_finite).status == PerformanceAcceptanceStatus::Failed);

    auto unsupported = complete_input();
    unsupported.schema = "future-schema";
    unsupported.policy_version = "future-policy";
    const auto unsupported_result = evaluate_performance_acceptance(unsupported);
    CHECK(unsupported_result.status == PerformanceAcceptanceStatus::Failed);
    CHECK(std::find(unsupported_result.diagnostic_codes.begin(), unsupported_result.diagnostic_codes.end(),
                    "unsupported_performance_schema") != unsupported_result.diagnostic_codes.end());
    CHECK(std::find(unsupported_result.diagnostic_codes.begin(), unsupported_result.diagnostic_codes.end(),
                    "unsupported_performance_policy") != unsupported_result.diagnostic_codes.end());
}

TEST_CASE("Performance multicolor and collision counts must be zero", "[AI][SmartSlicing][D8T1]")
{
    auto input = complete_input();
    input.multicolor_error_count = known<size_t>(1, {"inspection"});
    input.collision_count = known<size_t>(1, {"inspection"});
    const auto result = evaluate_performance_acceptance(input);
    CHECK(result.status == PerformanceAcceptanceStatus::Failed);
    CHECK(!*result.multicolor_errors.value);
    CHECK(!*result.collisions.value);
}

TEST_CASE("Performance unknown evidence gives partial or unavailable without optimistic pass", "[AI][SmartSlicing][D8T1]")
{
    auto partial = complete_input();
    partial.measured_time_seconds = MetricValue<double>::unknown({"measurement_pending"});
    CHECK(evaluate_performance_acceptance(partial).status == PerformanceAcceptanceStatus::Partial);

    auto unavailable = complete_input();
    unavailable.first_card_elapsed_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.total_elapsed_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.estimated_time_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.measured_time_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.estimated_material_volume_mm3 = MetricValue<double>::unknown({"not_measured"});
    unavailable.measured_material_volume_mm3 = MetricValue<double>::unknown({"not_measured"});
    unavailable.speed_baseline_physical_time_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.speed_candidate_physical_time_seconds = MetricValue<double>::unknown({"not_measured"});
    unavailable.dimensional_nominal_mm = MetricValue<double>::unknown({"not_measured"});
    unavailable.dimensional_error_mm = MetricValue<double>::unknown({"not_measured"});
    unavailable.appearance_baseline_score = MetricValue<double>::unknown({"not_measured"});
    unavailable.appearance_candidate_score = MetricValue<double>::unknown({"not_measured"});
    unavailable.strength_retention_ratio = MetricValue<double>::unknown({"not_measured"});
    unavailable.multicolor_error_count = MetricValue<size_t>::unknown({"not_measured"});
    unavailable.collision_count = MetricValue<size_t>::unknown({"not_measured"});
    CHECK(evaluate_performance_acceptance(unavailable).status == PerformanceAcceptanceStatus::Unavailable);
}

TEST_CASE("Performance evidence and diagnostics normalize deterministically", "[AI][SmartSlicing][D8T1]")
{
    auto first = complete_input();
    first.first_card_elapsed_seconds = known(121.0, {"z-source", "a-source", "z-source"});
    auto second = first;
    second.first_card_elapsed_seconds.evidence_codes = {"a-source", "z-source"};
    const auto first_result = evaluate_performance_acceptance(first);
    const auto second_result = evaluate_performance_acceptance(second);
    CHECK(first_result.diagnostic_codes == second_result.diagnostic_codes);
    CHECK(first_result.first_card_latency.evidence_codes == second_result.first_card_latency.evidence_codes);
}
