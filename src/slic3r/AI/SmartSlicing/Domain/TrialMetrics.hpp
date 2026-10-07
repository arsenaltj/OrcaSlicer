#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* TRIAL_METRICS_SCHEMA = "orcaslicer.smart-slicing.trial-metrics";
inline constexpr const char* TRIAL_METRICS_VERSION = "v1";
inline constexpr const char* TRIAL_METRICS_POLICY_VERSION = "trial-metrics-policy/v1";
inline constexpr double TRIAL_MATERIAL_ABSOLUTE_TOLERANCE_MM3 = 1e-6;
inline constexpr double TRIAL_MATERIAL_RELATIVE_TOLERANCE = 1e-9;

enum class MetricAvailability { Known, Unknown, NotApplicable };

const char* metric_availability_name(MetricAvailability availability);

template<class T> struct MetricValue
{
    MetricAvailability availability{MetricAvailability::Unknown};
    std::optional<T> value;
    std::vector<std::string> evidence_codes;

    static MetricValue known(T known_value, std::vector<std::string> evidence = {})
    {
        MetricValue result;
        result.availability = MetricAvailability::Known;
        result.value = std::move(known_value);
        result.evidence_codes = std::move(evidence);
        return result;
    }

    static MetricValue unknown(std::vector<std::string> evidence = {})
    {
        MetricValue result;
        result.availability = MetricAvailability::Unknown;
        result.evidence_codes = std::move(evidence);
        return result;
    }

    static MetricValue not_applicable(std::vector<std::string> evidence = {})
    {
        MetricValue result;
        result.availability = MetricAvailability::NotApplicable;
        result.evidence_codes = std::move(evidence);
        return result;
    }

    bool known() const { return availability == MetricAvailability::Known && value.has_value(); }
};

enum class NativeDiagnosticSeverity { Warning, Error };

struct NativeDiagnostic
{
    NativeDiagnosticSeverity severity{NativeDiagnosticSeverity::Warning};
    std::string code;
    std::string message;
};

struct LayerToolSequence
{
    size_t layer_index{0};
    std::vector<size_t> tool_ids;
};

struct EffectiveParameterSummary
{
    std::string scope_id;
    std::string key;
    std::string effective_value;
    std::string source_code;
};

struct ObjectTransformSummary
{
    uint64_t object_id{0};
    uint64_t instance_id{0};
    std::array<double, 16> transform{};
};

struct TrialMetrics
{
    std::string schema{TRIAL_METRICS_SCHEMA};
    std::string version{TRIAL_METRICS_VERSION};
    std::string policy_version{TRIAL_METRICS_POLICY_VERSION};

    MetricValue<double> estimated_time_seconds;
    MetricValue<double> model_material_volume_mm3;
    MetricValue<double> support_material_volume_mm3;
    MetricValue<double> flush_material_volume_mm3;
    MetricValue<double> wipe_tower_material_volume_mm3;
    MetricValue<double> total_material_volume_mm3;
    MetricValue<size_t> tool_change_count;
    MetricValue<double> tool_change_time_seconds;
    MetricValue<std::vector<LayerToolSequence>> layer_tool_sequences;
    MetricValue<bool> physical_slots_compatible;
    MetricValue<bool> materials_compatible;
    MetricValue<bool> color_mapping_degraded;
    MetricValue<bool> wipe_tower_enabled;

    std::vector<NativeDiagnostic> native_diagnostics;
    std::vector<EffectiveParameterSummary> effective_parameters;
    std::vector<ObjectTransformSummary> object_transforms;
};

struct TrialMetricsNormalizationResult
{
    TrialMetrics metrics;
    std::vector<std::string> validation_codes;

    bool valid() const { return validation_codes.empty(); }
};

TrialMetricsNormalizationResult normalize_trial_metrics(const TrialMetrics& metrics);

} // namespace Slic3r::AI::SmartSlicing
