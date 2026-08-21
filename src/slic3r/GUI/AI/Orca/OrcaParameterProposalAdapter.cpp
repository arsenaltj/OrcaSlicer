#include "OrcaParameterProposalAdapter.hpp"

#include <iomanip>
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
    const ParameterValidationResult domain_result = ParameterProposalValidator().validate(proposal);
    if (!domain_result.accepted())
        return rejected(parameter_rejection_code_name(domain_result.rejections.front().code));

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
            return rejected("parameter_not_supported_by_current_config");

        DynamicPrintConfig normalized_expected(working);
        DynamicPrintConfig normalized_new(working);
        try {
            normalized_expected.set_deserialize_strict(entry.key, serialize_value(entry.expected_value));
            normalized_new.set_deserialize_strict(entry.key, serialize_value(entry.new_value));
        } catch (...) {
            return rejected("parameter_native_deserialization_failed");
        }
        const ConfigOption* expected = normalized_expected.option(entry.key);
        const ConfigOption* replacement = normalized_new.option(entry.key);
        if (expected == nullptr || replacement == nullptr)
            return rejected("parameter_native_option_unavailable");
        if (current->serialize() != expected->serialize())
            return rejected("parameter_expected_value_changed");
        if (!normalized_value_in_range(*definition, *replacement))
            return rejected("parameter_native_range_violation");
        working.set_key_value(entry.key, replacement->clone());
    }

    patched_config = std::move(working);
    return {true, {}};
}

} // namespace Slic3r::GUI
