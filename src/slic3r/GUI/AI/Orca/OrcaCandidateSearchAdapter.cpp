#include "OrcaCandidateSearchAdapter.hpp"

#include "OrcaParameterProposalAdapter.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ParameterPolicy.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

namespace Slic3r::GUI {
namespace {

using namespace AI::SmartSlicing;

OrcaCandidateSearchCaptureResult rejected(OrcaCandidateSearchCaptureStatus status,
                                          std::string diagnostic)
{
    OrcaCandidateSearchCaptureResult result;
    result.status = status;
    result.diagnostic_code = std::move(diagnostic);
    return result;
}

bool finite_transform(const std::array<double, 16>& transform)
{
    if (!std::all_of(transform.begin(), transform.end(), [](double value) {
            return std::isfinite(value);
        }) || std::abs(transform[12]) > 1e-12 || std::abs(transform[13]) > 1e-12 ||
        std::abs(transform[14]) > 1e-12 || std::abs(transform[15] - 1.0) > 1e-12)
        return false;
    const double determinant =
        transform[0] * (transform[5] * transform[10] - transform[6] * transform[9]) -
        transform[1] * (transform[4] * transform[10] - transform[6] * transform[8]) +
        transform[2] * (transform[4] * transform[9] - transform[5] * transform[8]);
    return std::abs(determinant) > 1e-12;
}

bool supports_plate_scope(const ParameterPolicyEntry& policy)
{
    return std::find(policy.scopes.begin(), policy.scopes.end(), ConfigScope::Plate) !=
           policy.scopes.end();
}

std::optional<ConfigValue> scalar_value(const ConfigOption& option, ConfigValueKind expected_kind)
{
    if (!option.is_scalar() || option.is_nil())
        return std::nullopt;
    switch (expected_kind) {
    case ConfigValueKind::Boolean:
        if (option.type() == coBool)
            return static_cast<const ConfigOptionBool&>(option).value;
        break;
    case ConfigValueKind::Integer:
        if (option.type() == coInt)
            return static_cast<int64_t>(static_cast<const ConfigOptionInt&>(option).value);
        break;
    case ConfigValueKind::Floating:
        if (option.type() == coFloat || option.type() == coPercent)
            return static_cast<const ConfigOptionFloat&>(option).value;
        if (option.type() == coFloatOrPercent) {
            const auto& value = static_cast<const ConfigOptionFloatOrPercent&>(option);
            if (!value.percent)
                return value.value;
        }
        break;
    case ConfigValueKind::Enumeration:
        if (option.type() == coEnum || option.type() == coString)
            return option.serialize();
        break;
    }
    return std::nullopt;
}

bool verified_orientation(const OrientationSearchOption& option,
                          ProtectedRegionBindingStatus protected_status)
{
    if (option.strategy == OrientationStrategy::Current)
        return false;
    if (option.option_id.empty() || !finite_transform(option.transform) ||
        !std::isfinite(option.static_cost) ||
        option.evidence_availability != FeatureAvailability::Known ||
        option.evidence_source.empty() || option.evidence_version.empty())
        return false;
    return option.strategy != OrientationStrategy::ProtectedSurface ||
           protected_status == ProtectedRegionBindingStatus::Accepted;
}

std::string process_source_version(const ProfileIdentity& identity)
{
    return identity.setting_id + ":sha256:" + identity.fingerprint;
}

std::string effective_source_version(const OrcaCandidateSearchCaptureSource& source,
                                     const std::string& process_version)
{
    return process_version + ":workspace:" + source.context.revision.fingerprint;
}

} // namespace

OrcaCandidateSearchCaptureResult
OrcaCandidateSearchAdapter::capture(const OrcaCandidateSearchCaptureSource& source)
{
    if (!source.context.revision.valid() || source.plate_id == 0 || source.objects.empty())
        return rejected(OrcaCandidateSearchCaptureStatus::InvalidInput,
                        "candidate_capture_input_invalid");
    if (source.context.machine_capability.support_status == MachineSupportStatus::PendingValidation)
        return rejected(OrcaCandidateSearchCaptureStatus::MachineCapabilityPendingValidation,
                        "machine_capability_pending_validation");
    if (!source.context.machine_capability.enabled())
        return rejected(OrcaCandidateSearchCaptureStatus::MachineCapabilityDisabled,
                        "machine_capability_disabled");
    if (!source.context.material_compatibility.compatible() ||
        source.context.material_compatibility.materials.empty() ||
        std::any_of(source.context.material_compatibility.materials.begin(),
                    source.context.material_compatibility.materials.end(),
                    [](const MaterialCapabilitySnapshot& material) { return !material.enabled(); }))
        return rejected(OrcaCandidateSearchCaptureStatus::MaterialCompatibilityUnavailable,
                        "material_compatibility_unavailable");
    if (source.process_profile.dirty)
        return rejected(OrcaCandidateSearchCaptureStatus::ProcessProfileDirty,
                        "process_profile_dirty");
    if (source.process_profile.identity.setting_id.empty())
        return rejected(OrcaCandidateSearchCaptureStatus::ProcessProfileIdentityUnavailable,
                        "process_profile_stable_identity_unavailable");
    if (source.process_profile.identity.fingerprint.empty())
        return rejected(OrcaCandidateSearchCaptureStatus::ProcessProfileFingerprintUnavailable,
                        "process_profile_fingerprint_unavailable");

    CandidateSearchInput input;
    input.workspace_revision = source.context.revision;
    input.usage_purpose = source.usage_purpose;
    input.plate_locked = source.plate_locked;
    input.intent_constraints = source.context.intent_constraints;
    input.may_increase_process_speed = !source.context.material_compatibility.materials.empty() &&
        std::all_of(source.context.material_compatibility.materials.begin(),
                    source.context.material_compatibility.materials.end(),
                    [](const MaterialCapabilitySnapshot& material) {
                        return material.speed_policy.may_increase_process_speed;
                    });
    input.tpu_speed_restricted =
        source.context.material_compatibility.common_family == MaterialFamily::TPU ||
        !input.may_increase_process_speed;

    std::set<std::pair<uint64_t, uint64_t>> identities;
    input.objects.reserve(source.objects.size());
    for (const OrcaCandidateSearchObjectSource& object_source : source.objects) {
        if (object_source.object_id == 0 || object_source.instance_id == 0 ||
            !finite_transform(object_source.current_transform) ||
            !identities.emplace(object_source.object_id, object_source.instance_id).second)
            return rejected(OrcaCandidateSearchCaptureStatus::InvalidInput,
                            "candidate_capture_object_identity_invalid");

        CandidateSearchObjectInput object;
        object.object_id = object_source.object_id;
        object.instance_id = object_source.instance_id;
        object.current_transform = object_source.current_transform;
        object.locked = source.plate_locked || object_source.locked;
        object.model_features = object_source.model_features;
        object.protected_region_status = object_source.protected_region_status;
        object.protected_regions = object_source.protected_regions;

        OrientationSearchOption current;
        current.option_id = "current:" + std::to_string(object.object_id) + ":" +
                            std::to_string(object.instance_id);
        current.strategy = OrientationStrategy::Current;
        current.transform = object.current_transform;
        object.orientation_options.push_back(std::move(current));
        if (!object.locked) {
            for (const OrientationSearchOption& option : object_source.verified_orientation_options)
                if (verified_orientation(option, object.protected_region_status))
                    object.orientation_options.push_back(option);
        }
        input.objects.push_back(std::move(object));
    }

    const std::string process_version = process_source_version(source.process_profile.identity);
    const std::string current_version = effective_source_version(source, process_version);
    for (const ParameterPolicyEntry& policy : ParameterPolicyRegistry::current().entries()) {
        if (policy.owner != PresetOwner::Process ||
            policy.risk_class == ParameterRiskClass::ImmutableFact || !supports_plate_scope(policy))
            continue;
        const ConfigOption* profile_option = source.process_profile.process_config.option(policy.key);
        const ConfigOption* effective_option = source.process_profile.effective_config.option(policy.key);
        if (profile_option == nullptr && effective_option == nullptr)
            continue;
        if (profile_option == nullptr || effective_option == nullptr)
            return rejected(OrcaCandidateSearchCaptureStatus::ProfileEvidenceConflict,
                            "process_profile_parameter_evidence_missing." + policy.key);
        if (profile_option->type() != effective_option->type())
            return rejected(OrcaCandidateSearchCaptureStatus::ProfileEvidenceConflict,
                            "process_profile_parameter_type_conflict." + policy.key);
        if (profile_option->is_vector() && effective_option->is_vector())
            continue;
        if (profile_option->is_vector() || effective_option->is_vector())
            return rejected(OrcaCandidateSearchCaptureStatus::ProfileEvidenceConflict,
                            "process_profile_parameter_type_conflict." + policy.key);
        const std::optional<ConfigValue> current_value =
            scalar_value(*effective_option, policy.value_kind);
        const std::optional<ConfigValue> profile_value =
            scalar_value(*profile_option, policy.value_kind);
        if (!current_value || !profile_value)
            return rejected(OrcaCandidateSearchCaptureStatus::ProfileEvidenceConflict,
                            "process_profile_parameter_value_invalid." + policy.key);

        CandidateSearchParameterInput parameter;
        parameter.scope = ConfigScope::Plate;
        parameter.owner = PresetOwner::Process;
        parameter.target_id = source.plate_id;
        parameter.key = policy.key;
        parameter.current_value = *current_value;
        parameter.current_source_code = "orca_plate_effective_config";
        parameter.current_source_version = current_version;
        parameter.options.push_back(
            {*profile_value, true, "orca_active_process_profile", process_version});
        input.profile_parameters.push_back(std::move(parameter));
    }

    const auto base_config =
        std::make_shared<const DynamicPrintConfig>(source.process_profile.effective_config);
    const IntentConstraintSnapshot intent_constraints = input.intent_constraints;
    const int64_t plate_id = source.plate_id;
    std::vector<ParameterBoundEvidence> profile_bounds;
    for (const CandidateSearchParameterInput& parameter : input.profile_parameters)
        profile_bounds.insert(profile_bounds.end(), parameter.bounds.begin(), parameter.bounds.end());
    input.native_validator = [base_config, intent_constraints, plate_id,
                              profile_bounds = std::move(profile_bounds)](const ParameterProposal& proposal) {
        DynamicPrintConfig patched;
        const OrcaParameterApplyResult result = OrcaParameterProposalAdapter().validate_and_apply(
            proposal, plate_id, *base_config, intent_constraints, profile_bounds, patched);
        return NativeParameterValidationResult{result.accepted, result.diagnostic_code};
    };

    OrcaCandidateSearchCaptureResult result;
    result.status = OrcaCandidateSearchCaptureStatus::Complete;
    result.input = std::move(input);
    return result;
}

} // namespace Slic3r::GUI
