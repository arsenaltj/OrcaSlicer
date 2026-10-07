#include "TrialMetrics.hpp"

#include <cmath>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

template<class T>
void normalize_evidence(MetricValue<T>& metric)
{
    std::sort(metric.evidence_codes.begin(), metric.evidence_codes.end());
    metric.evidence_codes.erase(
        std::unique(metric.evidence_codes.begin(), metric.evidence_codes.end()), metric.evidence_codes.end());
}

template<class T>
void validate_presence(const MetricValue<T>& metric, const char* field, std::vector<std::string>& codes)
{
    if (metric.availability == MetricAvailability::Known && !metric.value)
        codes.emplace_back(std::string("known_metric_missing_value:") + field);
    if (metric.availability != MetricAvailability::Known && metric.value)
        codes.emplace_back(std::string("non_known_metric_has_value:") + field);
    if (metric.availability != MetricAvailability::Unknown &&
        (metric.evidence_codes.empty() ||
         std::any_of(metric.evidence_codes.begin(), metric.evidence_codes.end(),
                     [](const auto& code) { return code.empty(); })))
        codes.emplace_back(std::string("metric_missing_evidence:") + field);
}

void validate_non_negative(const MetricValue<double>& metric, const char* field, std::vector<std::string>& codes)
{
    validate_presence(metric, field, codes);
    if (metric.value && (!std::isfinite(*metric.value) || *metric.value < 0.0))
        codes.emplace_back(std::string("invalid_non_negative_metric:") + field);
}

bool component_available_as_zero(const MetricValue<double>& metric)
{
    return metric.availability == MetricAvailability::NotApplicable || metric.known();
}

double component_value_or_zero(const MetricValue<double>& metric)
{
    return metric.known() ? *metric.value : 0.0;
}

} // namespace

const char* metric_availability_name(MetricAvailability availability)
{
    switch (availability) {
    case MetricAvailability::Known: return "known";
    case MetricAvailability::Unknown: return "unknown";
    case MetricAvailability::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

TrialMetricsNormalizationResult normalize_trial_metrics(const TrialMetrics& input)
{
    TrialMetricsNormalizationResult result;
    result.metrics = input;

    if (result.metrics.schema != TRIAL_METRICS_SCHEMA)
        result.validation_codes.emplace_back("unsupported_trial_metrics_schema");
    if (result.metrics.version != TRIAL_METRICS_VERSION)
        result.validation_codes.emplace_back("unsupported_trial_metrics_version");
    if (result.metrics.policy_version != TRIAL_METRICS_POLICY_VERSION)
        result.validation_codes.emplace_back("unsupported_trial_metrics_policy_version");

    auto normalize_metric = [](auto& metric) { normalize_evidence(metric); };
    normalize_metric(result.metrics.estimated_time_seconds);
    normalize_metric(result.metrics.model_material_volume_mm3);
    normalize_metric(result.metrics.support_material_volume_mm3);
    normalize_metric(result.metrics.flush_material_volume_mm3);
    normalize_metric(result.metrics.wipe_tower_material_volume_mm3);
    normalize_metric(result.metrics.total_material_volume_mm3);
    normalize_metric(result.metrics.tool_change_count);
    normalize_metric(result.metrics.tool_change_time_seconds);
    normalize_metric(result.metrics.layer_tool_sequences);
    normalize_metric(result.metrics.physical_slots_compatible);
    normalize_metric(result.metrics.materials_compatible);
    normalize_metric(result.metrics.color_mapping_degraded);
    normalize_metric(result.metrics.wipe_tower_enabled);

    validate_non_negative(result.metrics.estimated_time_seconds, "estimated_time_seconds", result.validation_codes);
    validate_non_negative(result.metrics.model_material_volume_mm3, "model_material_volume_mm3", result.validation_codes);
    validate_non_negative(result.metrics.support_material_volume_mm3, "support_material_volume_mm3", result.validation_codes);
    validate_non_negative(result.metrics.flush_material_volume_mm3, "flush_material_volume_mm3", result.validation_codes);
    validate_non_negative(result.metrics.wipe_tower_material_volume_mm3, "wipe_tower_material_volume_mm3", result.validation_codes);
    validate_non_negative(result.metrics.total_material_volume_mm3, "total_material_volume_mm3", result.validation_codes);
    validate_presence(result.metrics.tool_change_count, "tool_change_count", result.validation_codes);
    validate_non_negative(result.metrics.tool_change_time_seconds, "tool_change_time_seconds", result.validation_codes);
    validate_presence(result.metrics.layer_tool_sequences, "layer_tool_sequences", result.validation_codes);
    validate_presence(result.metrics.physical_slots_compatible, "physical_slots_compatible", result.validation_codes);
    validate_presence(result.metrics.materials_compatible, "materials_compatible", result.validation_codes);
    validate_presence(result.metrics.color_mapping_degraded, "color_mapping_degraded", result.validation_codes);
    validate_presence(result.metrics.wipe_tower_enabled, "wipe_tower_enabled", result.validation_codes);

    if (result.metrics.total_material_volume_mm3.availability == MetricAvailability::Unknown &&
        component_available_as_zero(result.metrics.model_material_volume_mm3) &&
        component_available_as_zero(result.metrics.support_material_volume_mm3) &&
        component_available_as_zero(result.metrics.flush_material_volume_mm3) &&
        component_available_as_zero(result.metrics.wipe_tower_material_volume_mm3)) {
        result.metrics.total_material_volume_mm3 = MetricValue<double>::known(
            component_value_or_zero(result.metrics.model_material_volume_mm3) +
                component_value_or_zero(result.metrics.support_material_volume_mm3) +
                component_value_or_zero(result.metrics.flush_material_volume_mm3) +
                component_value_or_zero(result.metrics.wipe_tower_material_volume_mm3),
            {"computed_from_material_components"});
    }

    if (result.metrics.total_material_volume_mm3.known() &&
        component_available_as_zero(result.metrics.model_material_volume_mm3) &&
        component_available_as_zero(result.metrics.support_material_volume_mm3) &&
        component_available_as_zero(result.metrics.flush_material_volume_mm3) &&
        component_available_as_zero(result.metrics.wipe_tower_material_volume_mm3)) {
        const double component_total =
            component_value_or_zero(result.metrics.model_material_volume_mm3) +
            component_value_or_zero(result.metrics.support_material_volume_mm3) +
            component_value_or_zero(result.metrics.flush_material_volume_mm3) +
            component_value_or_zero(result.metrics.wipe_tower_material_volume_mm3);
        const double tolerance = std::max(TRIAL_MATERIAL_ABSOLUTE_TOLERANCE_MM3,
                                          std::abs(component_total) * TRIAL_MATERIAL_RELATIVE_TOLERANCE);
        if (std::abs(*result.metrics.total_material_volume_mm3.value - component_total) > tolerance)
            result.validation_codes.emplace_back("total_material_volume_mismatch");
    }

    if (result.metrics.layer_tool_sequences.value) {
        auto& sequences = *result.metrics.layer_tool_sequences.value;
        std::sort(sequences.begin(), sequences.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.layer_index, lhs.tool_ids) < std::tie(rhs.layer_index, rhs.tool_ids);
        });
    }

    std::sort(result.metrics.native_diagnostics.begin(), result.metrics.native_diagnostics.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.severity, lhs.code, lhs.message) <
                         std::tie(rhs.severity, rhs.code, rhs.message);
              });
    std::sort(result.metrics.effective_parameters.begin(), result.metrics.effective_parameters.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.scope_id, lhs.key, lhs.effective_value, lhs.source_code) <
                         std::tie(rhs.scope_id, rhs.key, rhs.effective_value, rhs.source_code);
              });
    for (const NativeDiagnostic& diagnostic : result.metrics.native_diagnostics) {
        if (diagnostic.code.empty())
            result.validation_codes.emplace_back("native_diagnostic_missing_code");
    }
    for (const EffectiveParameterSummary& parameter : result.metrics.effective_parameters) {
        if (parameter.scope_id.empty() || parameter.key.empty() || parameter.source_code.empty())
            result.validation_codes.emplace_back("invalid_effective_parameter_summary");
    }
    for (const ObjectTransformSummary& transform : result.metrics.object_transforms) {
        if (transform.object_id == 0 || transform.instance_id == 0)
            result.validation_codes.emplace_back("invalid_object_transform_identity");
        if (!std::all_of(transform.transform.begin(), transform.transform.end(),
                         [](double value) { return std::isfinite(value); }))
            result.validation_codes.emplace_back("invalid_object_transform_summary");
    }
    if (std::none_of(result.validation_codes.begin(), result.validation_codes.end(), [](const auto& code) {
            return code == "invalid_object_transform_summary";
        })) {
        std::sort(result.metrics.object_transforms.begin(), result.metrics.object_transforms.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return std::tie(lhs.object_id, lhs.instance_id, lhs.transform) <
                             std::tie(rhs.object_id, rhs.instance_id, rhs.transform);
                  });
        for (size_t index = 1; index < result.metrics.object_transforms.size(); ++index) {
            const auto& previous = result.metrics.object_transforms[index - 1];
            const auto& current = result.metrics.object_transforms[index];
            if (previous.object_id == current.object_id && previous.instance_id == current.instance_id) {
                result.validation_codes.emplace_back("duplicate_object_transform_identity");
                break;
            }
        }
    }

    std::sort(result.validation_codes.begin(), result.validation_codes.end());
    result.validation_codes.erase(std::unique(result.validation_codes.begin(), result.validation_codes.end()),
                                  result.validation_codes.end());
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
