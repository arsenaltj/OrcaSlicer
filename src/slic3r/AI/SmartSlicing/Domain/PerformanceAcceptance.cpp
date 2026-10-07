#include "PerformanceAcceptance.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace Slic3r::AI::SmartSlicing {
namespace {

template<class T>
bool valid_availability(MetricAvailability availability)
{
    return availability == MetricAvailability::Known || availability == MetricAvailability::Unknown ||
           availability == MetricAvailability::NotApplicable;
}

template<class T>
std::vector<std::string> normalize_evidence(const MetricValue<T>& metric)
{
    std::vector<std::string> evidence = metric.evidence_codes;
    std::sort(evidence.begin(), evidence.end());
    evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
    return evidence;
}

template<class T>
bool valid_shape(const MetricValue<T>& metric, std::vector<std::string>& diagnostics, const char* field)
{
    bool valid = valid_availability<T>(metric.availability);
    if (!valid)
        diagnostics.emplace_back("performance_value_availability_invalid");
    if (metric.availability == MetricAvailability::Known && !metric.value) {
        diagnostics.emplace_back(std::string("performance_evidence_missing:") + field);
        valid = false;
    }
    if (metric.availability != MetricAvailability::Known && metric.value) {
        diagnostics.emplace_back(std::string("performance_value_present_for_non_known:") + field);
        valid = false;
    }
    const std::vector<std::string> evidence = normalize_evidence(metric);
    if (evidence.empty() || std::any_of(evidence.begin(), evidence.end(), [](const auto& code) { return code.empty(); })) {
        diagnostics.emplace_back(std::string("performance_evidence_source_missing:") + field);
        valid = false;
    }
    return valid;
}

bool finite_non_negative(const MetricValue<double>& metric)
{
    return !metric.value || (std::isfinite(*metric.value) && *metric.value >= 0.0);
}

bool finite_positive(const MetricValue<double>& metric)
{
    return metric.value && std::isfinite(*metric.value) && *metric.value > 0.0;
}

template<class T>
std::vector<std::string> evidence_union(const MetricValue<T>& metric)
{
    return normalize_evidence(metric);
}

template<class T, class U>
std::vector<std::string> evidence_union(const MetricValue<T>& lhs, const MetricValue<U>& rhs)
{
    std::vector<std::string> evidence = lhs.evidence_codes;
    evidence.insert(evidence.end(), rhs.evidence_codes.begin(), rhs.evidence_codes.end());
    std::sort(evidence.begin(), evidence.end());
    evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
    return evidence;
}

template<class T>
MetricValue<bool> unknown_or_na(const MetricValue<T>& metric)
{
    return metric.availability == MetricAvailability::NotApplicable ?
               MetricValue<bool>::not_applicable(normalize_evidence(metric)) :
               MetricValue<bool>::unknown(normalize_evidence(metric));
}

template<class T>
bool is_unknown(const MetricValue<T>& metric)
{
    return metric.availability == MetricAvailability::Unknown;
}

void append_diagnostic(std::vector<std::string>& diagnostics, const std::string& code)
{
    diagnostics.push_back(code);
}

template<class T>
void validate_metric(const MetricValue<T>& metric, const char* field, std::vector<std::string>& diagnostics)
{
    valid_shape(metric, diagnostics, field);
}

void validate_double_metric(const MetricValue<double>& metric, const char* field,
                            std::vector<std::string>& diagnostics)
{
    validate_metric(metric, field, diagnostics);
    if (metric.value && !finite_non_negative(metric))
        diagnostics.emplace_back(std::string("performance_value_non_finite_or_negative:") + field);
}

MetricValue<bool> evaluate_timeout(const MetricValue<double>& elapsed, double maximum,
                                   const char* code, std::vector<std::string>& diagnostics)
{
    if (elapsed.availability != MetricAvailability::Known)
        return unknown_or_na(elapsed);
    if (!elapsed.value || !std::isfinite(*elapsed.value) || *elapsed.value < 0.0) {
        diagnostics.emplace_back(std::string("performance_value_out_of_bound:") + code);
        return MetricValue<bool>::known(false, normalize_evidence(elapsed));
    }
    const bool passed = *elapsed.value <= maximum;
    if (!passed)
        diagnostics.emplace_back(code);
    return MetricValue<bool>::known(passed, normalize_evidence(elapsed));
}

MetricValue<bool> evaluate_error(const MetricValue<double>& estimated, const MetricValue<double>& measured,
                                 double maximum, const char* code, std::vector<std::string>& diagnostics)
{
    if (estimated.availability == MetricAvailability::NotApplicable ||
        measured.availability == MetricAvailability::NotApplicable)
        return MetricValue<bool>::not_applicable(evidence_union(estimated, measured));
    if (estimated.availability != MetricAvailability::Known || measured.availability != MetricAvailability::Known)
        return MetricValue<bool>::unknown(evidence_union(estimated, measured));
    if (!estimated.value || !finite_positive(measured) || !finite_non_negative(estimated)) {
        diagnostics.emplace_back(std::string("performance_value_out_of_bound:") + code);
        return MetricValue<bool>::known(false, evidence_union(estimated, measured));
    }
    const double error = std::abs(*estimated.value - *measured.value) / *measured.value;
    const bool passed = std::isfinite(error) && error <= maximum;
    if (!passed)
        diagnostics.emplace_back(code);
    return MetricValue<bool>::known(passed, evidence_union(estimated, measured));
}

MetricValue<bool> evaluate_speed(const MetricValue<double>& baseline, const MetricValue<double>& candidate,
                                 std::vector<std::string>& diagnostics)
{
    if (baseline.availability == MetricAvailability::NotApplicable ||
        candidate.availability == MetricAvailability::NotApplicable)
        return MetricValue<bool>::not_applicable(evidence_union(baseline, candidate));
    if (baseline.availability != MetricAvailability::Known || candidate.availability != MetricAvailability::Known)
        return MetricValue<bool>::unknown(evidence_union(baseline, candidate));
    if (!baseline.value || !candidate.value || !finite_positive(baseline) || !finite_non_negative(candidate)) {
        diagnostics.emplace_back("performance_value_out_of_bound:speed_physical_time_reduction");
        return MetricValue<bool>::known(false, evidence_union(baseline, candidate));
    }
    const double reduction = (*baseline.value - *candidate.value) / *baseline.value;
    const bool passed = std::isfinite(reduction) && reduction >= PERFORMANCE_MIN_SPEED_REDUCTION;
    if (!passed)
        diagnostics.emplace_back("speed_reduction_below_threshold");
    return MetricValue<bool>::known(passed, evidence_union(baseline, candidate));
}

MetricValue<bool> evaluate_dimensional(const MetricValue<double>& nominal, const MetricValue<double>& error,
                                       std::vector<std::string>& diagnostics)
{
    if (nominal.availability == MetricAvailability::NotApplicable)
        return MetricValue<bool>::not_applicable(evidence_union(nominal, error));
    if (nominal.availability != MetricAvailability::Known || error.availability != MetricAvailability::Known)
        return MetricValue<bool>::unknown(evidence_union(nominal, error));
    if (!finite_positive(nominal) || !finite_non_negative(error)) {
        diagnostics.emplace_back("performance_value_out_of_bound:dimensional_error");
        return MetricValue<bool>::known(false, evidence_union(nominal, error));
    }
    const double tolerance = std::max(PERFORMANCE_MIN_DIMENSIONAL_TOLERANCE_MM,
                                      *nominal.value * PERFORMANCE_DIMENSIONAL_TOLERANCE_RATIO);
    const bool passed = *error.value <= tolerance;
    if (!passed)
        diagnostics.emplace_back("dimensional_error_exceeded");
    return MetricValue<bool>::known(passed, evidence_union(nominal, error));
}

MetricValue<bool> evaluate_appearance(const MetricValue<double>& baseline, const MetricValue<double>& candidate,
                                      std::vector<std::string>& diagnostics)
{
    if (baseline.availability == MetricAvailability::NotApplicable ||
        candidate.availability == MetricAvailability::NotApplicable)
        return MetricValue<bool>::not_applicable(evidence_union(baseline, candidate));
    if (baseline.availability != MetricAvailability::Known || candidate.availability != MetricAvailability::Known)
        return MetricValue<bool>::unknown(evidence_union(baseline, candidate));
    if (!baseline.value || !candidate.value || !std::isfinite(*baseline.value) || !std::isfinite(*candidate.value)) {
        diagnostics.emplace_back("performance_value_non_finite:appearance");
        return MetricValue<bool>::known(false, evidence_union(baseline, candidate));
    }
    const bool passed = *candidate.value >= *baseline.value;
    if (!passed)
        diagnostics.emplace_back("appearance_worse_than_baseline");
    return MetricValue<bool>::known(passed, evidence_union(baseline, candidate));
}

MetricValue<bool> evaluate_strength(const MetricValue<double>& retention, std::vector<std::string>& diagnostics)
{
    if (retention.availability != MetricAvailability::Known)
        return unknown_or_na(retention);
    if (!retention.value || !std::isfinite(*retention.value) || *retention.value < 0.0 || *retention.value > 1.0) {
        diagnostics.emplace_back("performance_value_out_of_bound:strength_retention");
        return MetricValue<bool>::known(false, normalize_evidence(retention));
    }
    const bool passed = *retention.value >= PERFORMANCE_MIN_STRENGTH_RETENTION;
    if (!passed)
        diagnostics.emplace_back("strength_retention_below_threshold");
    return MetricValue<bool>::known(passed, normalize_evidence(retention));
}

MetricValue<bool> evaluate_count(const MetricValue<size_t>& count, const char* code,
                                 std::vector<std::string>& diagnostics)
{
    if (count.availability != MetricAvailability::Known)
        return unknown_or_na(count);
    const bool passed = count.value && *count.value == 0;
    if (!passed)
        diagnostics.emplace_back(code);
    return MetricValue<bool>::known(passed, normalize_evidence(count));
}

bool has_failed(const PerformanceAcceptanceResult& result)
{
    const MetricValue<bool>* items[] = {&result.first_card_latency, &result.total_hard_timeout,
                                        &result.estimated_time_error, &result.estimated_material_error,
                                        &result.speed_physical_time_reduction, &result.dimensional_error,
                                        &result.appearance_not_worse_than_baseline, &result.strength_retention,
                                        &result.multicolor_errors, &result.collisions};
    return std::any_of(std::begin(items), std::end(items), [](const auto* item) {
        return item->availability == MetricAvailability::Known && item->value && !*item->value;
    });
}

bool has_unknown(const PerformanceAcceptanceResult& result)
{
    const MetricValue<bool>* items[] = {&result.first_card_latency, &result.total_hard_timeout,
                                        &result.estimated_time_error, &result.estimated_material_error,
                                        &result.speed_physical_time_reduction, &result.dimensional_error,
                                        &result.appearance_not_worse_than_baseline, &result.strength_retention,
                                        &result.multicolor_errors, &result.collisions};
    return std::any_of(std::begin(items), std::end(items), [](const auto* item) {
        return item->availability == MetricAvailability::Unknown;
    });
}

bool has_known_item(const PerformanceAcceptanceResult& result)
{
    const MetricValue<bool>* items[] = {&result.first_card_latency, &result.total_hard_timeout,
                                        &result.estimated_time_error, &result.estimated_material_error,
                                        &result.speed_physical_time_reduction, &result.dimensional_error,
                                        &result.appearance_not_worse_than_baseline, &result.strength_retention,
                                        &result.multicolor_errors, &result.collisions};
    return std::any_of(std::begin(items), std::end(items), [](const auto* item) {
        return item->availability == MetricAvailability::Known;
    });
}

} // namespace

const char* performance_acceptance_status_name(PerformanceAcceptanceStatus status)
{
    switch (status) {
    case PerformanceAcceptanceStatus::Passed: return "passed";
    case PerformanceAcceptanceStatus::Partial: return "partial";
    case PerformanceAcceptanceStatus::Unavailable: return "unavailable";
    case PerformanceAcceptanceStatus::Failed: return "failed";
    }
    return "failed";
}

PerformanceAcceptanceResult evaluate_performance_acceptance(const PerformanceAcceptanceInput& input)
{
    PerformanceAcceptanceResult result;
    result.schema = input.schema;
    result.version = input.version;
    result.policy_version = input.policy_version;

    if (input.schema != PERFORMANCE_ACCEPTANCE_SCHEMA)
        result.diagnostic_codes.emplace_back("unsupported_performance_schema");
    if (input.version != PERFORMANCE_ACCEPTANCE_VERSION)
        result.diagnostic_codes.emplace_back("unsupported_performance_version");
    if (input.policy_version != PERFORMANCE_ACCEPTANCE_POLICY_VERSION)
        result.diagnostic_codes.emplace_back("unsupported_performance_policy");

    validate_double_metric(input.first_card_elapsed_seconds, "first_card_elapsed_seconds", result.diagnostic_codes);
    validate_double_metric(input.total_elapsed_seconds, "total_elapsed_seconds", result.diagnostic_codes);
    validate_double_metric(input.estimated_time_seconds, "estimated_time_seconds", result.diagnostic_codes);
    validate_double_metric(input.measured_time_seconds, "measured_time_seconds", result.diagnostic_codes);
    validate_double_metric(input.estimated_material_volume_mm3, "estimated_material_volume_mm3", result.diagnostic_codes);
    validate_double_metric(input.measured_material_volume_mm3, "measured_material_volume_mm3", result.diagnostic_codes);
    validate_double_metric(input.speed_baseline_physical_time_seconds, "speed_baseline_physical_time_seconds", result.diagnostic_codes);
    validate_double_metric(input.speed_candidate_physical_time_seconds, "speed_candidate_physical_time_seconds", result.diagnostic_codes);
    validate_double_metric(input.dimensional_nominal_mm, "dimensional_nominal_mm", result.diagnostic_codes);
    validate_double_metric(input.dimensional_error_mm, "dimensional_error_mm", result.diagnostic_codes);
    validate_double_metric(input.appearance_baseline_score, "appearance_baseline_score", result.diagnostic_codes);
    validate_double_metric(input.appearance_candidate_score, "appearance_candidate_score", result.diagnostic_codes);
    validate_double_metric(input.strength_retention_ratio, "strength_retention_ratio", result.diagnostic_codes);
    validate_metric(input.multicolor_error_count, "multicolor_error_count", result.diagnostic_codes);
    validate_metric(input.collision_count, "collision_count", result.diagnostic_codes);

    result.first_card_latency = evaluate_timeout(input.first_card_elapsed_seconds,
                                                 PERFORMANCE_FIRST_CARD_TIMEOUT_SECONDS,
                                                 "first_card_timeout", result.diagnostic_codes);
    result.total_hard_timeout = evaluate_timeout(input.total_elapsed_seconds,
                                                 PERFORMANCE_TOTAL_HARD_TIMEOUT_SECONDS,
                                                 "total_hard_timeout", result.diagnostic_codes);
    result.estimated_time_error = evaluate_error(input.estimated_time_seconds, input.measured_time_seconds,
                                                 PERFORMANCE_MAX_ESTIMATED_TIME_ERROR,
                                                 "estimated_time_error_exceeded", result.diagnostic_codes);
    result.estimated_material_error = evaluate_error(input.estimated_material_volume_mm3,
                                                     input.measured_material_volume_mm3,
                                                     PERFORMANCE_MAX_ESTIMATED_MATERIAL_ERROR,
                                                     "estimated_material_error_exceeded", result.diagnostic_codes);
    result.speed_physical_time_reduction = evaluate_speed(input.speed_baseline_physical_time_seconds,
                                                          input.speed_candidate_physical_time_seconds,
                                                          result.diagnostic_codes);
    result.dimensional_error = evaluate_dimensional(input.dimensional_nominal_mm, input.dimensional_error_mm,
                                                    result.diagnostic_codes);
    result.appearance_not_worse_than_baseline = evaluate_appearance(input.appearance_baseline_score,
                                                                      input.appearance_candidate_score,
                                                                      result.diagnostic_codes);
    result.strength_retention = evaluate_strength(input.strength_retention_ratio, result.diagnostic_codes);
    result.multicolor_errors = evaluate_count(input.multicolor_error_count, "multicolor_error_present",
                                               result.diagnostic_codes);
    result.collisions = evaluate_count(input.collision_count, "collision_present", result.diagnostic_codes);

    std::sort(result.diagnostic_codes.begin(), result.diagnostic_codes.end());
    result.diagnostic_codes.erase(std::unique(result.diagnostic_codes.begin(), result.diagnostic_codes.end()),
                                  result.diagnostic_codes.end());
    if (input.schema != PERFORMANCE_ACCEPTANCE_SCHEMA || input.version != PERFORMANCE_ACCEPTANCE_VERSION ||
        input.policy_version != PERFORMANCE_ACCEPTANCE_POLICY_VERSION || !result.diagnostic_codes.empty()) {
        result.status = has_failed(result) || !result.diagnostic_codes.empty() ? PerformanceAcceptanceStatus::Failed :
                                                                              PerformanceAcceptanceStatus::Unavailable;
    } else if (has_failed(result)) {
        result.status = PerformanceAcceptanceStatus::Failed;
    } else if (!has_known_item(result)) {
        result.status = PerformanceAcceptanceStatus::Unavailable;
    } else if (has_unknown(result)) {
        result.status = PerformanceAcceptanceStatus::Partial;
    } else {
        result.status = PerformanceAcceptanceStatus::Passed;
    }
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
