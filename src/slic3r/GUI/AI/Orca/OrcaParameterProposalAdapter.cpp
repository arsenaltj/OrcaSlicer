#include "OrcaParameterProposalAdapter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

std::vector<SliceCandidate> OrcaParameterProposalAdapter::priority_candidates(
    const WorkspaceRevision& revision, int64_t plate_id,
    const DynamicPrintConfig& base_config, const std::vector<ModelObject*>& targets) const
{
    const auto* nozzles = base_config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* minimums = base_config.option<ConfigOptionFloats>("min_layer_height");
    const auto* maximums = base_config.option<ConfigOptionFloats>("max_layer_height");
    if (targets.empty() || nozzles == nullptr || nozzles->values.empty() ||
        minimums == nullptr || minimums->values.empty() || maximums == nullptr || maximums->values.empty())
        return {};

    // Preserve custom variable layers and part overrides rather than claiming
    // a uniform profile which the native consumer would silently override.
    double current_height = 0.0;
    for (const auto* object : targets) {
        if (object == nullptr || object->has_custom_layering())
            return {};
        DynamicPrintConfig effective(base_config);
        effective.apply(object->config.get(), true);
        const auto* height = effective.option<ConfigOptionFloat>("layer_height");
        if (height == nullptr || !std::isfinite(height->value))
            return {};
        if (current_height != 0.0 && std::abs(current_height - height->value) > 1e-9)
            return {};
        current_height = height->value;
        for (const auto* volume : object->volumes)
            if (volume != nullptr && volume->config.has("layer_height"))
                return {};
    }
    if (current_height < 0.04 || current_height > 0.40)
        return {};

    double nozzle = 1.0, lower = std::max(0.04, current_height - 0.12);
    double upper = std::min(0.40, current_height + 0.12);
    for (size_t i = 0; i < nozzles->values.size(); ++i) {
        const double diameter = nozzles->values[i];
        const double minimum = minimums->get_at(i), maximum = maximums->get_at(i);
        if (!std::isfinite(diameter) || diameter <= 0.0 || !std::isfinite(minimum) ||
            minimum < 0.0 || !std::isfinite(maximum) || maximum < 0.0)
            return {};
        nozzle = std::min(nozzle, diameter);
        lower = std::max(lower, minimum);
        // 70% stays within the nozzle even when native maximum is automatic.
        upper = std::min(upper, maximum > 0.0 ? std::min(maximum, diameter * 0.7) : diameter * 0.7);
    }
    if (lower > upper)
        return {};
    const auto bounded_height = [&](double ratio) {
        return std::clamp(nozzle * ratio, lower, upper);
    };
    const double quality = bounded_height(0.3), balanced = bounded_height(0.4), speed = bounded_height(0.7);
    if (quality + 1e-9 >= balanced || balanced + 1e-9 >= speed)
        return {}; // Do not present duplicate constrained settings as three choices.

    const std::array<double, 3> heights{balanced, speed, quality};
    const std::array<const char*, 3> ids{
        "parameter-layer-0-balanced-v1", "parameter-layer-1-speed-v1", "parameter-layer-2-quality-v1"};
    const std::array<const char*, 3> explanations{
        "layer_height_balanced_candidate", "layer_height_speed_candidate", "layer_height_quality_candidate"};
    std::vector<SliceCandidate> candidates;
    for (size_t i = 0; i < heights.size(); ++i) {
        SliceCandidate candidate;
        candidate.id = ids[i];
        candidate.base_revision = revision;
        candidate.explanation = explanations[i];
        if (std::abs(heights[i] - current_height) > 1e-9) {
            candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, plate_id,
                "layer_height", current_height, heights[i], explanations[i]});
            std::vector<OrcaObjectParameterPatch> patches;
            if (!prepare_object_patches(candidate.parameters, plate_id, base_config, targets, patches).accepted)
                return {};
        }
        // An unchanged profile is still measured as a read-only alternative.
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

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

OrcaParameterApplyResult OrcaParameterProposalAdapter::prepare_object_patches(
    const ParameterProposal& proposal, int64_t expected_plate_id, const DynamicPrintConfig& base_config,
    const std::vector<ModelObject*>& targets, std::vector<OrcaObjectParameterPatch>& patches) const
{
    const auto domain = ParameterProposalValidator().validate(proposal);
    if (!domain.accepted())
        return rejected(parameter_rejection_code_name(domain.rejections.front().code));
    if (targets.empty())
        return rejected("parameter_target_objects_missing");

    // Plate process proposals are stored in existing native object overrides.
    // Orca's 3MF plate metadata only persists a fixed set of plate settings.
    const PrintObjectConfig object_options;
    const PrintRegionConfig region_options;
    for (const auto& entry : proposal.entries)
        if (!object_options.has(entry.key) && !region_options.has(entry.key))
            return rejected("parameter_not_supported_by_object_config");

    std::vector<OrcaObjectParameterPatch> working;
    for (const auto* object : targets) {
        if (object == nullptr)
            return rejected("parameter_target_objects_missing");
        DynamicPrintConfig effective(base_config);
        effective.apply(object->config.get(), true);
        DynamicPrintConfig patched;
        const auto result = validate_and_apply(proposal, expected_plate_id, effective, patched);
        if (!result.accepted)
            return result;
        DynamicPrintConfig overrides(object->config.get());
        for (const auto& entry : proposal.entries) {
            for (const auto* volume : object->volumes)
                if (volume != nullptr && volume->config.has(entry.key))
                    return rejected("parameter_volume_override_conflict");
            overrides.set_key_value(entry.key, patched.option(entry.key)->clone());
        }
        OrcaObjectParameterPatch patch;
        patch.object_id = object->id().id;
        patch.original = object->config;
        patch.replacement = object->config;
        patch.replacement.assign_config(overrides);
        working.push_back(std::move(patch));
    }
    patches = std::move(working);
    return {true, {}};
}

OrcaParameterApplyResult OrcaParameterProposalAdapter::apply_object_patches(
    Model& model, const std::vector<OrcaObjectParameterPatch>& patches) const
{
    std::vector<ModelObject*> targets;
    for (const auto& patch : patches) {
        const auto target = std::find_if(model.objects.begin(), model.objects.end(), [&](const ModelObject* object) {
            return object != nullptr && object->id().id == patch.object_id;
        });
        if (target == model.objects.end())
            return rejected("parameter_target_objects_missing");
        if ((*target)->config.get() != patch.original.get())
            return rejected("parameter_expected_value_changed");
        targets.push_back(*target);
    }
    for (size_t i = 0; i < patches.size(); ++i)
        targets[i]->config.assign_config(patches[i].replacement);
    return {true, {}};
}

} // namespace Slic3r::GUI
