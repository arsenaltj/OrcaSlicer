#include "OrcaParameterProposalAdapter.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace Slic3r::GUI {
namespace {

using namespace AI::SmartSlicing;

std::string serialize_value(const ConfigValue& value)
{
    if (const auto* boolean = std::get_if<bool>(&value))
        return *boolean ? "1" : "0";
    if (const auto* integer = std::get_if<int64_t>(&value))
        return std::to_string(*integer);
    if (const auto* floating = std::get_if<double>(&value)) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(17) << *floating;
        return stream.str();
    }
    return std::get<std::string>(value);
}

bool normalized_value_in_range(const ConfigOptionDef& definition, const ConfigOption& option)
{
    switch (option.type()) {
    case coFloat:
        return definition.is_value_valid(static_cast<const ConfigOptionFloat&>(option).value);
    case coPercent:
        return definition.is_value_valid(static_cast<const ConfigOptionFloat&>(option).value);
    case coFloatOrPercent:
        return definition.is_value_valid(static_cast<const ConfigOptionFloatOrPercent&>(option).value);
    case coInt:
        return definition.is_value_valid(static_cast<const ConfigOptionInt&>(option).value);
    default:
        return true;
    }
}

OrcaParameterApplyResult rejected(std::string diagnostic)
{
    return {false, std::move(diagnostic)};
}

} // namespace

OrcaParameterApplyResult OrcaParameterProposalAdapter::validate_and_apply(
    const ParameterProposal& proposal, int64_t expected_plate_id, const DynamicPrintConfig& base_config,
    DynamicPrintConfig& patched_config) const
{
    if (proposal.entries.empty()) {
        patched_config = base_config;
        return {true, {}};
    }
    return rejected("parameter_validation_context_required");
}

OrcaParameterApplyResult OrcaParameterProposalAdapter::validate_and_apply(
    const ParameterProposal& proposal, int64_t expected_plate_id, const DynamicPrintConfig& base_config,
    const IntentConstraintSnapshot& intent_constraints,
    const std::vector<ParameterBoundEvidence>& profile_bounds,
    DynamicPrintConfig& patched_config) const
{
    ParameterValidationContext context;
    context.goal = proposal.goal;
    context.intent_constraints = intent_constraints;
    context.bounds = profile_bounds;

    DynamicPrintConfig working(base_config);
    const ConfigDef* definitions = working.def();
    if (definitions == nullptr)
        return rejected("parameter_config_definition_unavailable");

    for (const ConfigPatchEntry& entry : proposal.entries) {
        if (entry.target_id != expected_plate_id)
            return rejected("parameter_target_mismatch");
        const ConfigOptionDef* definition = definitions->get(entry.key);
        const ConfigOption* current = working.option(entry.key);
        if (definition == nullptr || current == nullptr)
            continue;

        DynamicPrintConfig normalized_expected(working);
        try {
            normalized_expected.set_deserialize_strict(entry.key, serialize_value(entry.expected_value));
        } catch (...) {
            continue;
        }
        const ConfigOption* expected = normalized_expected.option(entry.key);
        if (expected == nullptr)
            continue;
        ConfigValue captured = entry.expected_value;
        if (current->serialize() != expected->serialize()) {
            if (auto* value = std::get_if<bool>(&captured)) *value = !*value;
            else if (auto* value = std::get_if<int64_t>(&captured)) ++*value;
            else if (auto* value = std::get_if<double>(&captured))
                *value = std::nextafter(*value, std::numeric_limits<double>::infinity());
            else std::get<std::string>(captured).append("#stale");
        }
        context.current_values.push_back(
            {entry.scope, entry.owner, entry.target_id, entry.key, std::move(captured)});
    }

    context.native_validator = [&](const ParameterProposal& native_proposal) {
        for (const ConfigPatchEntry& entry : native_proposal.entries) {
            const ConfigOptionDef* definition = definitions->get(entry.key);
            if (definition == nullptr || working.option(entry.key) == nullptr)
                return NativeParameterValidationResult{false, "parameter_not_supported_by_current_config"};
            if (!definition->is_scalar())
                return NativeParameterValidationResult{false, "parameter_native_vector_unsupported"};
            DynamicPrintConfig normalized_new(working);
            try {
                normalized_new.set_deserialize_strict(entry.key, serialize_value(entry.new_value));
            } catch (...) {
                return NativeParameterValidationResult{false, "parameter_native_deserialization_failed"};
            }
            const ConfigOption* replacement = normalized_new.option(entry.key);
            if (replacement == nullptr)
                return NativeParameterValidationResult{false, "parameter_native_option_unavailable"};
            if (!normalized_value_in_range(*definition, *replacement))
                return NativeParameterValidationResult{false, "parameter_native_range_violation"};
            working.set_key_value(entry.key, replacement->clone());
        }
        return NativeParameterValidationResult{true, {}};
    };

    const ParameterValidationResult domain_result =
        ParameterProposalValidator().validate(proposal, context);
    if (!domain_result.accepted())
        return rejected(parameter_rejection_code_name(domain_result.rejections.front().code));

    patched_config = std::move(working);
    return {true, {}};
}

} // namespace Slic3r::GUI
