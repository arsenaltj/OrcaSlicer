#pragma once

#include "TrialMetrics.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* PERFORMANCE_ACCEPTANCE_SCHEMA = "orcaslicer.smart-slicing.performance-acceptance";
inline constexpr const char* PERFORMANCE_ACCEPTANCE_VERSION = "v1";
inline constexpr const char* PERFORMANCE_ACCEPTANCE_POLICY_VERSION = "performance-acceptance-policy/v1";
inline constexpr double PERFORMANCE_FIRST_CARD_TIMEOUT_SECONDS = 120.0;
inline constexpr double PERFORMANCE_TOTAL_HARD_TIMEOUT_SECONDS = 600.0;
inline constexpr double PERFORMANCE_MAX_ESTIMATED_TIME_ERROR = 0.15;
inline constexpr double PERFORMANCE_MAX_ESTIMATED_MATERIAL_ERROR = 0.10;
inline constexpr double PERFORMANCE_MIN_SPEED_REDUCTION = 0.10;
inline constexpr double PERFORMANCE_MIN_DIMENSIONAL_TOLERANCE_MM = 0.2;
inline constexpr double PERFORMANCE_DIMENSIONAL_TOLERANCE_RATIO = 0.005;
inline constexpr double PERFORMANCE_MIN_STRENGTH_RETENTION = 0.95;

enum class PerformanceAcceptanceStatus { Passed, Partial, Unavailable, Failed };

const char* performance_acceptance_status_name(PerformanceAcceptanceStatus status);

struct PerformanceAcceptanceInput
{
    std::string schema{PERFORMANCE_ACCEPTANCE_SCHEMA};
    std::string version{PERFORMANCE_ACCEPTANCE_VERSION};
    std::string policy_version{PERFORMANCE_ACCEPTANCE_POLICY_VERSION};

    MetricValue<double> first_card_elapsed_seconds;
    MetricValue<double> total_elapsed_seconds;
    MetricValue<double> estimated_time_seconds;
    MetricValue<double> measured_time_seconds;
    MetricValue<double> estimated_material_volume_mm3;
    MetricValue<double> measured_material_volume_mm3;
    MetricValue<double> speed_baseline_physical_time_seconds;
    MetricValue<double> speed_candidate_physical_time_seconds;
    MetricValue<double> dimensional_nominal_mm;
    MetricValue<double> dimensional_error_mm;
    MetricValue<double> appearance_baseline_score;
    MetricValue<double> appearance_candidate_score;
    MetricValue<double> strength_retention_ratio;
    MetricValue<size_t> multicolor_error_count;
    MetricValue<size_t> collision_count;
};

struct PerformanceAcceptanceResult
{
    std::string schema{PERFORMANCE_ACCEPTANCE_SCHEMA};
    std::string version{PERFORMANCE_ACCEPTANCE_VERSION};
    std::string policy_version{PERFORMANCE_ACCEPTANCE_POLICY_VERSION};

    MetricValue<bool> first_card_latency;
    MetricValue<bool> total_hard_timeout;
    MetricValue<bool> estimated_time_error;
    MetricValue<bool> estimated_material_error;
    MetricValue<bool> speed_physical_time_reduction;
    MetricValue<bool> dimensional_error;
    MetricValue<bool> appearance_not_worse_than_baseline;
    MetricValue<bool> strength_retention;
    MetricValue<bool> multicolor_errors;
    MetricValue<bool> collisions;

    PerformanceAcceptanceStatus status{PerformanceAcceptanceStatus::Unavailable};
    std::vector<std::string> diagnostic_codes;

    bool passed() const { return status == PerformanceAcceptanceStatus::Passed; }
};

PerformanceAcceptanceResult evaluate_performance_acceptance(const PerformanceAcceptanceInput& input);

inline PerformanceAcceptanceResult validate_performance_acceptance(const PerformanceAcceptanceInput& input)
{
    return evaluate_performance_acceptance(input);
}

} // namespace Slic3r::AI::SmartSlicing
