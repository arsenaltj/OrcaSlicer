#include "CandidateSearchPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

constexpr double EPSILON = 1e-9;

size_t goal_index(RecommendationGoal goal)
{
    switch (goal) {
    case RecommendationGoal::Balanced: return 0;
    case RecommendationGoal::Speed: return 1;
    case RecommendationGoal::Quality: return 2;
    }
    throw std::invalid_argument("unsupported recommendation goal");
}

std::string number(double value)
{
    if (value == 0.0)
        value = 0.0;
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string value_key(const ConfigValue& value)
{
    if (const auto* boolean = std::get_if<bool>(&value))
        return *boolean ? "b:1" : "b:0";
    if (const auto* integer = std::get_if<int64_t>(&value))
        return "i:" + std::to_string(*integer);
    if (const auto* floating = std::get_if<double>(&value))
        return "f:" + number(*floating);
    return "s:" + std::get<std::string>(value);
}

bool finite_matrix(const std::array<double, 16>& matrix)
{
    return std::all_of(matrix.begin(), matrix.end(), [](double value) { return std::isfinite(value); });
}

std::string matrix_key(const std::array<double, 16>& matrix)
{
    std::string result;
    for (double value : matrix) {
        result += number(value);
        result.push_back(',');
    }
    return result;
}

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string stable_id(const std::string& prefix, const std::string& canonical)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << prefix << '-' << std::hex << std::setfill('0') << std::setw(16) << fnv1a64(canonical);
    return stream.str();
}

bool active_plate_lock(const CandidateSearchInput& input)
{
    if (input.plate_locked)
        return true;
    return std::any_of(input.intent_constraints.records.begin(), input.intent_constraints.records.end(),
        [](const IntentConstraintRecord& record) {
            return record.is_hard_constraint() && record.type == IntentConstraintType::PlatePlacementLock;
        });
}

bool active_object_lock(const CandidateSearchInput& input, const CandidateSearchObjectInput& object)
{
    if (object.locked)
        return true;
    return std::any_of(input.intent_constraints.records.begin(), input.intent_constraints.records.end(),
        [&](const IntentConstraintRecord& record) {
            if (!record.is_hard_constraint() || record.object_id != object.object_id)
                return false;
            if (record.type == IntentConstraintType::ObjectPlacementLock)
                return true;
            return record.type == IntentConstraintType::InstancePlacementLock &&
                   (record.instance_id == 0 || record.instance_id == object.instance_id);
        });
}

int orientation_rank(RecommendationGoal goal, OrientationStrategy strategy)
{
    switch (goal) {
    case RecommendationGoal::Balanced:
        switch (strategy) {
        case OrientationStrategy::StablePlane: return 0;
        case OrientationStrategy::LowSupport: return 1;
        case OrientationStrategy::ProtectedSurface: return 2;
        case OrientationStrategy::Current: return 3;
        }
        break;
    case RecommendationGoal::Speed:
        switch (strategy) {
        case OrientationStrategy::LowSupport: return 0;
        case OrientationStrategy::StablePlane: return 1;
        case OrientationStrategy::Current: return 2;
        case OrientationStrategy::ProtectedSurface: return 3;
        }
        break;
    case RecommendationGoal::Quality:
        switch (strategy) {
        case OrientationStrategy::ProtectedSurface: return 0;
        case OrientationStrategy::StablePlane: return 1;
        case OrientationStrategy::Current: return 2;
        case OrientationStrategy::LowSupport: return 3;
        }
        break;
    }
    return 4;
}

struct NormalizedOrientation
{
    std::string option_id;
    OrientationStrategy strategy{OrientationStrategy::Current};
    std::array<double, 16> transform{};
    double static_cost{0.0};
};

bool protected_regions_known(const CandidateSearchObjectInput& object)
{
    return object.protected_region_status == ProtectedRegionBindingStatus::Accepted &&
           !object.protected_regions.empty();
}

std::vector<NormalizedOrientation> orientations_for(const CandidateSearchInput& input,
                                                     const CandidateSearchObjectInput& object,
                                                     RecommendationGoal goal)
{
    std::vector<NormalizedOrientation> values;
    if (!finite_matrix(object.current_transform))
        return values;
    const NormalizedOrientation current{"current", OrientationStrategy::Current,
                                        object.current_transform, 0.0};
    if (!active_plate_lock(input) && !active_object_lock(input, object)) {
        for (const OrientationSearchOption& option : object.orientation_options) {
            if (option.option_id.empty() || option.evidence_availability != FeatureAvailability::Known ||
                option.evidence_source.empty() || option.evidence_version.empty() ||
                !std::isfinite(option.static_cost) || !finite_matrix(option.transform))
                continue;
            if (option.strategy == OrientationStrategy::ProtectedSurface && !protected_regions_known(object))
                continue;
            if (matrix_key(option.transform) != matrix_key(object.current_transform))
                values.push_back({option.option_id, option.strategy, option.transform, option.static_cost});
        }
    }
    std::stable_sort(values.begin(), values.end(), [&](const auto& lhs, const auto& rhs) {
        return std::tuple{orientation_rank(goal, lhs.strategy), lhs.static_cost, lhs.option_id,
                          matrix_key(lhs.transform)} <
               std::tuple{orientation_rank(goal, rhs.strategy), rhs.static_cost, rhs.option_id,
                          matrix_key(rhs.transform)};
    });
    std::set<std::string> matrices{matrix_key(object.current_transform)};
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
        return !matrices.insert(matrix_key(value.transform)).second;
    }), values.end());
    const size_t maximum = candidate_search_budget().maximum_orientations_per_object;
    if (values.size() >= maximum)
        values.resize(maximum - 1);
    values.push_back(current);
    return values;
}

struct Beam
{
    PlacementCandidate placement;
    double cost{0.0};
    std::string canonical;
};

std::vector<Beam> build_beam(const CandidateSearchInput& input,
                             const std::vector<CandidateSearchObjectInput>& objects,
                             RecommendationGoal goal,
                             std::vector<CandidateSearchObjectSummary>* summaries)
{
    std::vector<Beam> beam(1);
    for (const CandidateSearchObjectInput& object : objects) {
        const auto options = orientations_for(input, object, goal);
        if (summaries != nullptr)
            summaries->push_back({object.object_id, object.instance_id, options.size(),
                                  active_plate_lock(input) || active_object_lock(input, object)});
        if (options.empty())
            return {};
        std::vector<Beam> expanded;
        for (const Beam& current : beam) {
            for (const NormalizedOrientation& option : options) {
                Beam next = current;
                next.placement.transforms.push_back({object.object_id, object.instance_id, option.transform});
                next.cost += static_cast<double>(orientation_rank(goal, option.strategy)) + option.static_cost;
                next.canonical += std::to_string(object.object_id) + ":" +
                                  std::to_string(object.instance_id) + ":" + matrix_key(option.transform) + ";";
                expanded.push_back(std::move(next));
            }
        }
        std::stable_sort(expanded.begin(), expanded.end(), [](const Beam& lhs, const Beam& rhs) {
            return std::tuple{lhs.cost, lhs.canonical} < std::tuple{rhs.cost, rhs.canonical};
        });
        expanded.erase(std::unique(expanded.begin(), expanded.end(), [](const Beam& lhs, const Beam& rhs) {
            return lhs.canonical == rhs.canonical;
        }), expanded.end());
        if (expanded.size() > candidate_search_budget().beam_width)
            expanded.resize(candidate_search_budget().beam_width);
        beam = std::move(expanded);
    }
    return beam;
}

struct RiskSummary
{
    bool known_risk{false};
    bool unknown_risk{false};
};

template<class T> void accumulate_feature(const FeatureValue<std::vector<T>>& feature,
                                          RiskSummary& summary)
{
    if (feature.availability != FeatureAvailability::Known || !feature.value) {
        summary.unknown_risk = true;
        return;
    }
    if (!feature.value->empty())
        summary.known_risk = true;
}

RiskSummary risk_summary(const std::vector<CandidateSearchObjectInput>& objects)
{
    RiskSummary result;
    for (const CandidateSearchObjectInput& object : objects) {
        if (object.protected_region_status == ProtectedRegionBindingStatus::Accepted) {
            if (!object.protected_regions.empty())
                result.known_risk = true;
        } else {
            result.unknown_risk = true;
        }
        if (object.model_features.empty()) {
            result.unknown_risk = true;
            continue;
        }
        for (const ModelFeatureSnapshot& feature : object.model_features) {
            accumulate_feature(feature.small_text_candidates, result);
            accumulate_feature(feature.thin_wall_candidates, result);
            if (feature.overhangs.availability != FeatureAvailability::Known || !feature.overhangs.value) {
                result.unknown_risk = true;
            } else {
                for (const OverhangBandSummary& band : feature.overhangs.value->bands)
                    if (band.facet_count > 0)
                        result.known_risk = true;
            }
            if (feature.orientation_sensitive_surfaces.availability != FeatureAvailability::Known ||
                !feature.orientation_sensitive_surfaces.value) {
                result.unknown_risk = true;
            } else if (feature.orientation_sensitive_surfaces.value->facet_count > 0) {
                result.known_risk = true;
            }
        }
    }
    return result;
}

std::string object_search_signature(const CandidateSearchObjectInput& object)
{
    std::vector<std::string> orientations;
    orientations.reserve(object.orientation_options.size());
    for (const OrientationSearchOption& option : object.orientation_options) {
        orientations.push_back(option.option_id + ":" +
            std::to_string(static_cast<int>(option.strategy)) + ":" + matrix_key(option.transform) + ":" +
            std::to_string(static_cast<int>(option.evidence_availability)) + ":" +
            option.evidence_source + ":" + option.evidence_version + ":" + number(option.static_cost));
    }
    std::sort(orientations.begin(), orientations.end());
    const RiskSummary risks = risk_summary({object});
    std::string result = matrix_key(object.current_transform) + "|" + (object.locked ? "1" : "0") + "|" +
        std::to_string(static_cast<int>(object.protected_region_status)) + "|" +
        (object.protected_regions.empty() ? "0" : "1") + "|" +
        (risks.known_risk ? "1" : "0") + (risks.unknown_risk ? "1" : "0") + "|";
    for (const std::string& orientation : orientations)
        result += orientation + ";";
    return result;
}

std::string optional_number_key(const std::optional<double>& value)
{
    return value ? number(*value) : "-";
}

std::string option_signature(const ProfileParameterValue& option)
{
    return value_key(option.value) + ":" + (option.valid ? "1" : "0") + ":" +
        option.source_code + ":" + option.source_version;
}

std::string bound_identity(const ParameterBoundEvidence& bound)
{
    return std::to_string(static_cast<int>(bound.scope)) + ":" +
        std::to_string(static_cast<int>(bound.owner)) + ":" + std::to_string(bound.target_id) + ":" +
        bound.key + ":" + std::to_string(static_cast<int>(bound.source));
}

std::string bound_signature(const ParameterBoundEvidence& bound)
{
    std::string result = bound_identity(bound) + ":" + (bound.valid ? "1" : "0") + ":" +
        optional_number_key(bound.minimum) + ":" + optional_number_key(bound.maximum) + ":" +
        bound.evidence_version + ":";
    for (const std::string& value : bound.allowed_enum_values)
        result += value + ",";
    return result;
}

std::string parameter_identity(const CandidateSearchParameterInput& parameter)
{
    return std::to_string(static_cast<int>(parameter.scope)) + ":" +
        std::to_string(static_cast<int>(parameter.owner)) + ":" +
        std::to_string(parameter.target_id) + ":" + parameter.key;
}

std::string parameter_signature(const CandidateSearchParameterInput& parameter)
{
    std::string result = parameter_identity(parameter) + "|" + value_key(parameter.current_value) + "|" +
        parameter.current_source_code + "|" + parameter.current_source_version + "|";
    for (const ProfileParameterValue& option : parameter.options)
        result += option_signature(option) + ";";
    result += "|";
    for (const ParameterBoundEvidence& bound : parameter.bounds)
        result += bound_signature(bound) + ";";
    return result;
}

bool normalize_parameter_evidence(CandidateSearchParameterInput& parameter)
{
    std::sort(parameter.options.begin(), parameter.options.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple{value_key(lhs.value), option_signature(lhs)} <
               std::tuple{value_key(rhs.value), option_signature(rhs)};
    });
    for (size_t index = 1; index < parameter.options.size(); ++index) {
        if (value_key(parameter.options[index - 1].value) == value_key(parameter.options[index].value) &&
            option_signature(parameter.options[index - 1]) != option_signature(parameter.options[index]))
            return false;
    }
    parameter.options.erase(std::unique(parameter.options.begin(), parameter.options.end(),
        [](const auto& lhs, const auto& rhs) { return option_signature(lhs) == option_signature(rhs); }),
        parameter.options.end());

    for (ParameterBoundEvidence& bound : parameter.bounds) {
        std::sort(bound.allowed_enum_values.begin(), bound.allowed_enum_values.end());
        bound.allowed_enum_values.erase(
            std::unique(bound.allowed_enum_values.begin(), bound.allowed_enum_values.end()),
            bound.allowed_enum_values.end());
    }
    std::sort(parameter.bounds.begin(), parameter.bounds.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple{bound_identity(lhs), bound_signature(lhs)} <
               std::tuple{bound_identity(rhs), bound_signature(rhs)};
    });
    for (size_t index = 1; index < parameter.bounds.size(); ++index) {
        if (bound_identity(parameter.bounds[index - 1]) == bound_identity(parameter.bounds[index]) &&
            bound_signature(parameter.bounds[index - 1]) != bound_signature(parameter.bounds[index]))
            return false;
    }
    parameter.bounds.erase(std::unique(parameter.bounds.begin(), parameter.bounds.end(),
        [](const auto& lhs, const auto& rhs) { return bound_signature(lhs) == bound_signature(rhs); }),
        parameter.bounds.end());
    return true;
}

struct NormalizedParameterResult
{
    std::vector<CandidateSearchParameterInput> values;
    bool conflict{false};
};

NormalizedParameterResult normalized_parameters(const CandidateSearchInput& input)
{
    NormalizedParameterResult result;
    result.values = input.profile_parameters;
    for (CandidateSearchParameterInput& parameter : result.values) {
        if (!normalize_parameter_evidence(parameter)) {
            result.conflict = true;
            return result;
        }
    }
    std::sort(result.values.begin(), result.values.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple{parameter_identity(lhs), parameter_signature(lhs)} <
               std::tuple{parameter_identity(rhs), parameter_signature(rhs)};
    });
    for (size_t index = 1; index < result.values.size(); ++index) {
        if (parameter_identity(result.values[index - 1]) == parameter_identity(result.values[index]) &&
            parameter_signature(result.values[index - 1]) != parameter_signature(result.values[index])) {
            result.conflict = true;
            return result;
        }
    }
    result.values.erase(std::unique(result.values.begin(), result.values.end(), [](const auto& lhs, const auto& rhs) {
        return parameter_signature(lhs) == parameter_signature(rhs);
    }), result.values.end());
    return result;
}

const CandidateSearchParameterInput* find_parameter(
    const std::vector<CandidateSearchParameterInput>& parameters, const std::string& key)
{
    const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& value) {
        return value.key == key && !value.current_source_code.empty() && !value.current_source_version.empty();
    });
    return found == parameters.end() ? nullptr : &*found;
}

const ProfileParameterValue* exact_option(const CandidateSearchParameterInput& parameter,
                                          const ConfigValue& desired)
{
    const auto found = std::find_if(parameter.options.begin(), parameter.options.end(), [&](const auto& option) {
        if (!option.valid || option.source_code.empty() || option.source_version.empty())
            return false;
        if (option.value.index() != desired.index())
            return false;
        if (const auto* lhs = std::get_if<double>(&option.value))
            return std::abs(*lhs - std::get<double>(desired)) <= EPSILON;
        return option.value == desired;
    });
    return found == parameter.options.end() ? nullptr : &*found;
}

std::optional<double> numeric_value(const ConfigValue& value)
{
    if (const auto* integer = std::get_if<int64_t>(&value))
        return static_cast<double>(*integer);
    if (const auto* floating = std::get_if<double>(&value))
        return std::isfinite(*floating) ? std::optional<double>(*floating) : std::nullopt;
    return std::nullopt;
}

bool append_exact(ParameterProposal& proposal,
                  const std::vector<CandidateSearchParameterInput>& parameters,
                  const std::string& key, ConfigValue desired, const std::string& reason)
{
    const CandidateSearchParameterInput* parameter = find_parameter(parameters, key);
    if (parameter == nullptr)
        return false;
    if (parameter->current_value == desired)
        return true;
    const ProfileParameterValue* option = exact_option(*parameter, desired);
    if (option == nullptr)
        return false;
    proposal.entries.push_back({parameter->scope, parameter->owner, parameter->target_id,
                                 parameter->key, parameter->current_value, option->value, reason});
    return true;
}

bool append_minimum(ParameterProposal& proposal,
                    const std::vector<CandidateSearchParameterInput>& parameters,
                    const std::string& key, double minimum, const std::string& reason)
{
    const CandidateSearchParameterInput* parameter = find_parameter(parameters, key);
    if (parameter == nullptr)
        return false;
    const std::optional<double> current = numeric_value(parameter->current_value);
    if (!current || !std::isfinite(*current))
        return false;
    if (*current + EPSILON >= minimum)
        return true;

    const ProfileParameterValue* selected = nullptr;
    double selected_value = std::numeric_limits<double>::infinity();
    for (const ProfileParameterValue& option : parameter->options) {
        if (!option.valid || option.source_code.empty() || option.source_version.empty() ||
            option.value.index() != parameter->current_value.index())
            continue;
        const std::optional<double> value = numeric_value(option.value);
        if (value && *value + EPSILON >= minimum && *value < selected_value) {
            selected = &option;
            selected_value = *value;
        }
    }
    if (selected == nullptr)
        return false;
    proposal.entries.push_back({parameter->scope, parameter->owner, parameter->target_id,
                                parameter->key, parameter->current_value, selected->value, reason});
    return true;
}

struct ParameterTemplateResult
{
    std::vector<ParameterProposal> proposals;
    std::string diagnostic_code;
};

enum class SupportEnablement { Disabled, Enabled, Unknown };

SupportEnablement support_enablement(const std::vector<CandidateSearchParameterInput>& parameters)
{
    const CandidateSearchParameterInput* support = find_parameter(parameters, "enable_support");
    if (support == nullptr)
        return SupportEnablement::Unknown;
    const bool* enabled = std::get_if<bool>(&support->current_value);
    if (enabled == nullptr)
        return SupportEnablement::Unknown;
    return *enabled ? SupportEnablement::Enabled : SupportEnablement::Disabled;
}

bool append_shell_thickness_floors(ParameterProposal& proposal,
                                   const std::vector<CandidateSearchParameterInput>& parameters,
                                   RecommendationGoal goal, double layer_height)
{
    if (!std::isfinite(layer_height) || layer_height <= 0.0)
        return false;
    const double top_thickness = goal == RecommendationGoal::Quality ? 1.0 : 0.8;
    const double bottom_thickness = goal == RecommendationGoal::Quality ? 0.8 : 0.6;
    return append_minimum(proposal, parameters, "top_shell_layers",
                          std::ceil(top_thickness / layer_height - EPSILON),
                          "goal_top_thickness_floor") &&
           append_minimum(proposal, parameters, "bottom_shell_layers",
                          std::ceil(bottom_thickness / layer_height - EPSILON),
                          "goal_bottom_thickness_floor");
}

ParameterTemplateResult parameter_templates(const CandidateSearchInput& input,
                                            const std::vector<CandidateSearchParameterInput>& parameters,
                                            RecommendationGoal goal,
                                            const RiskSummary& risks)
{
    const GoalContract& contract = goal_contract(goal);
    ParameterProposal common;
    common.goal = goal;
    common.policy_version = input.parameter_policy_version;
    common.explanation_codes = contract.explanation_codes;

    if (parameters.empty()) {
        if (goal == RecommendationGoal::Speed)
            return {{}, "speed_usage_floor_unavailable"};
        ParameterTemplateResult result;
        result.proposals.push_back(std::move(common));
        return result;
    }

    if (goal == RecommendationGoal::Balanced) {
        append_exact(common, parameters, "wall_loops", int64_t{3}, "balanced_wall_baseline");
        append_exact(common, parameters, "sparse_infill_density", 15.0, "balanced_infill_baseline");
        append_exact(common, parameters, "seam_position", std::string("aligned_back"),
                     "balanced_hide_seam");
    } else if (goal == RecommendationGoal::Speed) {
        const double infill = input.usage_purpose == UsagePurpose::Decoration ? 10.0 : 15.0;
        if (!append_minimum(common, parameters, "wall_loops", 2.0, "speed_minimum_walls") ||
            !append_minimum(common, parameters, "sparse_infill_density", infill,
                            "speed_usage_infill_floor"))
            return {{}, "speed_usage_floor_unavailable"};
        append_exact(common, parameters, "seam_position", std::string("aligned_back"),
                     "speed_avoid_protected_seam");
        if (input.may_increase_process_speed && !input.tpu_speed_restricted) {
            const CandidateSearchParameterInput* speed = find_parameter(parameters, "outer_wall_speed");
            if (speed != nullptr) {
                const ProfileParameterValue* selected = nullptr;
                double selected_value = -std::numeric_limits<double>::infinity();
                for (const ProfileParameterValue& option : speed->options) {
                    const auto* value = std::get_if<double>(&option.value);
                    if (option.valid && value != nullptr && std::isfinite(*value) &&
                        !option.source_code.empty() && !option.source_version.empty() && *value > selected_value) {
                        selected = &option;
                        selected_value = *value;
                    }
                }
                if (selected != nullptr && selected->value != speed->current_value)
                    common.entries.push_back({speed->scope, speed->owner, speed->target_id, speed->key,
                                              speed->current_value, selected->value,
                                              "speed_profile_supported_process_speed"});
            }
        }
    } else {
        append_exact(common, parameters, "wall_loops", int64_t{3}, "quality_wall_baseline");
        append_exact(common, parameters, "sparse_infill_density",
                     input.usage_purpose == UsagePurpose::Functional ? 20.0 : 15.0,
                     "quality_usage_infill");
        append_exact(common, parameters, "seam_position", std::string("back"),
                     "quality_hide_seam");
    }

    const SupportEnablement support = support_enablement(parameters);
    if (support == SupportEnablement::Unknown)
        return {{}, "support_enablement_unknown"};
    if (support == SupportEnablement::Enabled) {
        const double minimum_interface_layers = goal == RecommendationGoal::Quality ? 3.0 : 2.0;
        if (!append_minimum(common, parameters, "support_interface_top_layers",
                            minimum_interface_layers, "support_interface_floor"))
            return {{}, "support_interface_floor_unavailable"};
    }

    std::vector<ParameterProposal> proposals;
    const CandidateSearchParameterInput* layer = find_parameter(parameters, "layer_height");
    const std::optional<double> current_layer =
        layer == nullptr ? std::nullopt : numeric_value(layer->current_value);
    if (!current_layer || !std::isfinite(*current_layer) || *current_layer <= 0.0)
        return {{}, "shell_thickness_evidence_unavailable"};
    for (double layer_height : contract.preferred_layer_heights_mm) {
        if (goal == RecommendationGoal::Speed && std::abs(layer_height - 0.28) <= EPSILON &&
            (risks.known_risk || risks.unknown_risk))
            continue;
        ParameterProposal proposal = common;
        if (!append_exact(proposal, parameters, "layer_height", layer_height,
                          "goal_profile_layer_height") ||
            !append_shell_thickness_floors(proposal, parameters, goal, layer_height))
            continue;
        proposals.push_back(std::move(proposal));
    }
    if (proposals.empty()) {
        ParameterProposal current = common;
        if (!append_shell_thickness_floors(current, parameters, goal, *current_layer))
            return {{}, "shell_thickness_floor_unavailable"};
        proposals.push_back(std::move(current));
    }

    std::set<std::string> seen;
    proposals.erase(std::remove_if(proposals.begin(), proposals.end(), [&](const auto& proposal) {
        std::string key;
        for (const ConfigPatchEntry& entry : proposal.entries)
            key += entry.key + "=" + value_key(entry.new_value) + ";";
        return !seen.insert(key).second;
    }), proposals.end());
    return {std::move(proposals), {}};
}

std::string proposal_canonical(const ParameterProposal& proposal)
{
    std::string result = recommendation_goal_id(proposal.goal) + std::string("|") + proposal.policy_version + "|";
    for (const ConfigPatchEntry& entry : proposal.entries) {
        result += std::to_string(static_cast<int>(entry.scope)) + ":" +
                  std::to_string(static_cast<int>(entry.owner)) + ":" +
                  std::to_string(entry.target_id) + ":" + entry.key + ":" +
                  value_key(entry.expected_value) + ":" + value_key(entry.new_value) + ";";
    }
    return result;
}

ParameterValidationContext validation_context(
    const CandidateSearchInput& input,
    const std::vector<CandidateSearchParameterInput>& parameters,
    RecommendationGoal goal)
{
    ParameterValidationContext context;
    context.goal = goal;
    context.intent_constraints = input.intent_constraints;
    context.native_validator = input.native_validator;
    for (const CandidateSearchParameterInput& parameter : parameters) {
        context.current_values.push_back({parameter.scope, parameter.owner, parameter.target_id,
                                          parameter.key, parameter.current_value});
        context.bounds.insert(context.bounds.end(), parameter.bounds.begin(), parameter.bounds.end());
    }
    return context;
}

std::string search_canonical_prefix(const CandidateSearchInput& input)
{
    return std::to_string(input.workspace_revision.model_revision) + ":" +
           std::to_string(input.workspace_revision.config_revision) + ":" +
           std::to_string(input.workspace_revision.plate_revision) + ":" +
           input.workspace_revision.fingerprint + ":" + input.parameter_policy_version + ":" +
           GOAL_CONTRACT_VERSION + ":" + CANDIDATE_SEARCH_BUDGET_VERSION + ":" +
           CANDIDATE_TRIAL_COST_POLICY_VERSION + ":" +
           std::to_string(static_cast<int>(input.usage_purpose));
}

bool placement_changes_current(const PlacementCandidate& placement,
                               const std::vector<CandidateSearchObjectInput>& objects)
{
    if (placement.transforms.size() != objects.size())
        return true;
    for (size_t index = 0; index < objects.size(); ++index) {
        const ObjectTransform& transform = placement.transforms[index];
        const CandidateSearchObjectInput& object = objects[index];
        if (transform.object_id != object.object_id || transform.instance_id != object.instance_id ||
            transform.matrix != object.current_transform)
            return true;
    }
    return false;
}

} // namespace

const char* orientation_strategy_name(OrientationStrategy strategy)
{
    switch (strategy) {
    case OrientationStrategy::Current: return "current";
    case OrientationStrategy::StablePlane: return "stable_plane";
    case OrientationStrategy::LowSupport: return "low_support";
    case OrientationStrategy::ProtectedSurface: return "protected_surface";
    }
    return "unknown";
}

GoalCandidateDrafts& CandidateSearchResult::goal(RecommendationGoal value)
{
    return goals.at(goal_index(value));
}

const GoalCandidateDrafts& CandidateSearchResult::goal(RecommendationGoal value) const
{
    return goals.at(goal_index(value));
}

CandidateSearchResult CandidateSearchPipeline::search(const CandidateSearchInput& input) const
{
    CandidateSearchResult result;
    result.budget_version = candidate_search_budget().version;
    result.trial_cost_policy_version = CANDIDATE_TRIAL_COST_POLICY_VERSION;
    result.baseline.workspace_revision = input.workspace_revision;
    result.baseline.parameters.entries.clear();
    result.baseline.parameters.explanation_codes.clear();
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index)
        result.goals[index].goal = RECOMMENDATION_GOALS[index];
    if (!input.workspace_revision.valid()) {
        result.baseline.trial_slots = 0;
        result.total_trial_slots = 0;
        for (GoalCandidateDrafts& goal : result.goals)
            goal.diagnostic_codes.push_back("workspace_revision_invalid");
        return result;
    }
    result.baseline.candidate_id = stable_id("baseline", search_canonical_prefix(input));
    result.baseline.trial_slots = candidate_search_budget().baseline_trial_slots;
    result.total_trial_slots = result.baseline.trial_slots;

    std::vector<CandidateSearchObjectInput> objects = input.objects;
    std::stable_sort(objects.begin(), objects.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.object_id, lhs.instance_id) < std::tie(rhs.object_id, rhs.instance_id);
    });
    bool conflicting_object_identity = false;
    for (size_t index = 1; index < objects.size(); ++index) {
        if (objects[index - 1].object_id == objects[index].object_id &&
            objects[index - 1].instance_id == objects[index].instance_id &&
            object_search_signature(objects[index - 1]) != object_search_signature(objects[index])) {
            conflicting_object_identity = true;
            break;
        }
    }
    if (conflicting_object_identity) {
        for (GoalCandidateDrafts& goal : result.goals)
            goal.diagnostic_codes.push_back("candidate_object_identity_conflict");
        return result;
    }
    objects.erase(std::unique(objects.begin(), objects.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.object_id == rhs.object_id && lhs.instance_id == rhs.instance_id;
    }), objects.end());

    NormalizedParameterResult normalized = normalized_parameters(input);
    if (normalized.conflict) {
        for (GoalCandidateDrafts& goal : result.goals)
            goal.diagnostic_codes.push_back("candidate_parameter_evidence_conflict");
        return result;
    }
    const auto& parameters = normalized.values;
    const RiskSummary risks = risk_summary(objects);
    const std::string prefix = search_canonical_prefix(input);

    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        GoalCandidateDrafts& output = result.goal(goal);
        std::vector<CandidateSearchObjectSummary> summaries;
        const std::vector<Beam> beams = build_beam(input, objects, goal, &summaries);
        output.board_beam_count = beams.size();
        if (result.objects.empty())
            result.objects = std::move(summaries);
        if (input.parameter_policy_version != PARAMETER_POLICY_VERSION) {
            output.diagnostic_codes.push_back("parameter_policy_version_mismatch");
            continue;
        }
        if (objects.empty() || beams.empty()) {
            output.diagnostic_codes.push_back("candidate_orientation_unavailable");
            continue;
        }
        ParameterTemplateResult template_result = parameter_templates(input, parameters, goal, risks);
        if (template_result.proposals.empty()) {
            output.diagnostic_codes.push_back(template_result.diagnostic_code.empty() ?
                "goal_profile_option_unavailable" : template_result.diagnostic_code);
            continue;
        }
        const ParameterValidationContext context = validation_context(input, parameters, goal);

        struct RankedDraft
        {
            double rank{0.0};
            CandidateSearchDraft draft;
        };
        std::vector<RankedDraft> generated;
        std::set<CandidateId> seen_ids;
        for (size_t template_index = 0; template_index < template_result.proposals.size(); ++template_index) {
            for (const Beam& beam : beams) {
                if (template_result.proposals[template_index].entries.empty() &&
                    !placement_changes_current(beam.placement, objects))
                    continue;
                CandidateSearchDraft draft;
                draft.goal = goal;
                draft.estimated_trial_cost =
                    static_cast<double>(template_index) * CANDIDATE_TRIAL_PARAMETER_TEMPLATE_STRIDE + beam.cost;
                draft.placement = beam.placement;
                draft.parameters = template_result.proposals[template_index];
                draft.status = CandidateStatus::Draft;
                if (risks.unknown_risk)
                    draft.diagnostic_codes.push_back("feature_or_protected_region_evidence_unknown");
                draft.explanation_codes = draft.parameters.explanation_codes;
                const std::string canonical = prefix + "|" + recommendation_goal_id(goal) + "|" +
                                              beam.canonical + "|" + proposal_canonical(draft.parameters);
                draft.candidate_id = stable_id(recommendation_goal_id(goal), canonical);
                if (!seen_ids.insert(draft.candidate_id).second)
                    continue;
                generated.push_back({draft.estimated_trial_cost, std::move(draft)});
            }
        }
        std::stable_sort(generated.begin(), generated.end(), [](const RankedDraft& lhs, const RankedDraft& rhs) {
            return std::tuple{lhs.rank, lhs.draft.candidate_id} <
                   std::tuple{rhs.rank, rhs.draft.candidate_id};
        });
        for (RankedDraft& ranked : generated) {
            ParameterValidationResult validation;
            if (!ranked.draft.parameters.entries.empty())
                validation = ParameterProposalValidator().validate(ranked.draft.parameters, context);
            if (!validation.accepted()) {
                RejectedCandidateDraft rejected;
                rejected.candidate_id = ranked.draft.candidate_id;
                rejected.goal = goal;
                rejected.diagnostic_codes.push_back(
                    parameter_rejection_code_name(validation.rejections.front().code));
                if (!validation.rejections.front().diagnostic_code.empty())
                    rejected.diagnostic_codes.push_back(validation.rejections.front().diagnostic_code);
                if (output.rejected_drafts.size() < candidate_search_budget().maximum_static_drafts_per_goal)
                    output.rejected_drafts.push_back(std::move(rejected));
                continue;
            }
            output.drafts.push_back(std::move(ranked.draft));
            if (output.drafts.size() == candidate_search_budget().maximum_static_drafts_per_goal)
                break;
        }
        output.generated_static_draft_count = output.drafts.size();
        if (output.drafts.empty() && output.rejected_drafts.empty())
            output.diagnostic_codes.push_back("goal_candidate_no_effective_change");
        const size_t selected = std::min(output.drafts.size(),
                                         candidate_search_budget().maximum_trial_selected_per_goal);
        output.selected_for_trial.assign(output.drafts.begin(), output.drafts.begin() + selected);
        result.total_trial_slots += selected;
    }
    if (result.total_trial_slots > candidate_search_budget().maximum_total_trial_slots)
        result.total_trial_slots = candidate_search_budget().maximum_total_trial_slots;
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
