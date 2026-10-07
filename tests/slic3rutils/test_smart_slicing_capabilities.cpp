#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "slic3r/AI/SmartSlicing/Domain/MachineCapabilitySnapshot.hpp"
#include "slic3r/AI/SmartSlicing/Domain/MaterialCompatibility.hpp"

using namespace Slic3r::AI::SmartSlicing;

namespace {

template<class T> EvidenceValue<T> available(T value, std::string source = "test_fixture")
{
    return {EvidenceAvailability::Available, std::move(value), std::move(source)};
}

ProfileIdentity zr_ultra_identity()
{
    return {"xJBdCljSZVDINXnP",
            {"fdm_klipper_common"},
            "bea5e153635e3e0dc884f9d21ad604d21d8efaf18ff016ec814331b7387330e7"};
}

ProfileIdentity zr_ultra_s_identity()
{
    return {"8xoZ6vYv9ws0J97u",
            {"fdm_klipper_common"},
            "32a2d25a673c1f54fb27e85ba925354e31ad0593c83e62269a89091c9fa6cb81"};
}

MachineCapabilityEvidence complete_machine_evidence(size_t nozzle_count = 4)
{
    MachineCapabilityEvidence evidence;
    evidence.independently_addressable = available(true);
    for (size_t index = 0; index < nozzle_count; ++index) {
        NozzleCapabilitySnapshot nozzle;
        nozzle.physical_number = static_cast<int>(index + 1);
        nozzle.diameter_mm = 0.4;
        nozzle.offset = available(NozzleOffset{static_cast<double>(index) * 25.0, 0.0});
        nozzle.reachable_area = available(ReachableArea{0.0, 220.0, 0.0, 220.0});
        evidence.nozzles.push_back(std::move(nozzle));
    }
    evidence.collision_clearance = available(CollisionClearanceLimits{2.0, 20.0, 50.0});
    evidence.tool_change_gcode = available(ToolChangeGcodeCapability{"T{target_tool}", true});
    evidence.preheat_behavior = available(PreheatBehavior{15.0, 190.0});
    evidence.standby_behavior = available(StandbyBehavior{150.0, 5.0});
    evidence.retraction_behavior = available(RetractionBehavior{1.0, 30.0});
    evidence.tool_change_time_model = available(ToolChangeTimeModel{3.0, 5.0});
    evidence.prime_or_wipe = available(PrimeOrWipeCapability{true, true});
    evidence.flush_matrix = available(FlushMatrix{nozzle_count, std::vector<double>(nozzle_count * nozzle_count, 10.0)});
    evidence.wipe_tower_space_constraints = available(WipeTowerSpaceConstraints{30.0, 30.0, 5.0});
    evidence.specialized_validation_complete = available(true);
    return evidence;
}

bool has_reason(const MachineCapabilitySnapshot& snapshot, MachineCapabilityReason reason)
{
    return std::find(snapshot.reasons.begin(), snapshot.reasons.end(), reason) != snapshot.reasons.end();
}

ProfileIdentity pla_identity()
{
    return {"HRFI81hKAaT1LwjF",
            {"fdm_filament_pla"},
            "9752c34b03a95c963f9085d658d904a5b9f5b7d95fbe5e3c2491c3ef1bda0cb1"};
}

ProfileIdentity petg_identity()
{
    return {"ZtPZlMNIqq3Gaavc",
            {"fdm_filament_pet"},
            "ad769ce90f0ef80a2bda3b5967bf053f4759c03a2c4fd90ffe4232727c6b71c2"};
}

ProfileIdentity tpu_identity()
{
    return {"eUyRprcb22IdYNv3",
            {"fdm_filament_tpu"},
            "1ff207fb8ec4e396ca5c2b749bad6877ff1ba71c577795be859d2a4b3697ad82"};
}

} // namespace

TEST_CASE("complete allowlisted four-nozzle capability is enabled", "[AI][SmartSlicing]")
{
    const MachineCapabilitySnapshot snapshot =
        evaluate_machine_capability(zr_ultra_identity(), complete_machine_evidence());
    CHECK(snapshot.enabled());
    CHECK(snapshot.reasons.empty());
    CHECK(snapshot.evidence.nozzles.size() == 4);
    REQUIRE(snapshot.evidence.nozzles.front().offset.value);
    CHECK(snapshot.evidence.nozzles.front().offset.value->x_mm == 0.0);
    REQUIRE(snapshot.evidence.nozzles.front().reachable_area.value);
    CHECK(snapshot.evidence.nozzles.front().reachable_area.value->maximum_x_mm == 220.0);
    REQUIRE(snapshot.evidence.collision_clearance.value);
    CHECK(snapshot.evidence.collision_clearance.value->vertical_clearance_mm == 50.0);
    REQUIRE(snapshot.evidence.tool_change_gcode.value);
    CHECK(snapshot.evidence.tool_change_gcode.value->command_template == "T{target_tool}");
    REQUIRE(snapshot.evidence.flush_matrix.value);
    CHECK(snapshot.evidence.flush_matrix.value->volume_mm3.size() == 16);
    CHECK(snapshot.registry_version == machine_capability_registry_version());
}

TEST_CASE("every required machine capability keeps the profile pending when absent", "[AI][SmartSlicing]")
{
    using Mutator = void (*)(MachineCapabilityEvidence&);
    const std::array<std::pair<MachineCapabilityReason, Mutator>, 14> cases{{
        {MachineCapabilityReason::MissingIndependentAddressing, [](auto& value) { value.independently_addressable = {}; }},
        {MachineCapabilityReason::InvalidPhysicalNumbering, [](auto& value) { value.nozzles[2].physical_number = 2; }},
        {MachineCapabilityReason::MissingNozzleOffset, [](auto& value) { value.nozzles[2].offset = {}; }},
        {MachineCapabilityReason::MissingReachableArea, [](auto& value) { value.nozzles[1].reachable_area = {}; }},
        {MachineCapabilityReason::MissingCollisionClearance, [](auto& value) { value.collision_clearance = {}; }},
        {MachineCapabilityReason::MissingToolChangeGcode, [](auto& value) { value.tool_change_gcode = {}; }},
        {MachineCapabilityReason::MissingPreheatBehavior, [](auto& value) { value.preheat_behavior = {}; }},
        {MachineCapabilityReason::MissingStandbyBehavior, [](auto& value) { value.standby_behavior = {}; }},
        {MachineCapabilityReason::MissingRetractionBehavior, [](auto& value) { value.retraction_behavior = {}; }},
        {MachineCapabilityReason::MissingToolChangeTimeModel, [](auto& value) { value.tool_change_time_model = {}; }},
        {MachineCapabilityReason::MissingPrimeOrWipeCapability, [](auto& value) { value.prime_or_wipe = {}; }},
        {MachineCapabilityReason::MissingFlushMatrix, [](auto& value) { value.flush_matrix = {}; }},
        {MachineCapabilityReason::MissingWipeTowerSpaceConstraints, [](auto& value) { value.wipe_tower_space_constraints = {}; }},
        {MachineCapabilityReason::SpecializedValidationIncomplete, [](auto& value) { value.specialized_validation_complete = {}; }},
    }};

    for (const auto& [reason, mutate] : cases) {
        MachineCapabilityEvidence evidence = complete_machine_evidence();
        mutate(evidence);
        const MachineCapabilitySnapshot snapshot = evaluate_machine_capability(zr_ultra_identity(), std::move(evidence));
        INFO(machine_capability_reason_name(reason));
        CHECK(snapshot.support_status == MachineSupportStatus::PendingValidation);
        CHECK(has_reason(snapshot, reason));
    }
}

TEST_CASE("profile identity and six-nozzle policy cannot be bypassed by complete evidence", "[AI][SmartSlicing]")
{
    ProfileIdentity changed = zr_ultra_identity();
    changed.fingerprint[0] = changed.fingerprint[0] == '0' ? '1' : '0';
    const auto mismatched = evaluate_machine_capability(std::move(changed), complete_machine_evidence());
    CHECK(mismatched.support_status == MachineSupportStatus::PendingValidation);
    CHECK(has_reason(mismatched, MachineCapabilityReason::ProfileFingerprintMismatch));

    const auto six_nozzles = evaluate_machine_capability(zr_ultra_identity(), complete_machine_evidence(6));
    CHECK(six_nozzles.support_status == MachineSupportStatus::PendingValidation);
    CHECK(has_reason(six_nozzles, MachineCapabilityReason::SixNozzlePendingValidation));

    ProfileIdentity unknown = zr_ultra_identity();
    unknown.setting_id = "unknown-machine";
    const auto disabled = evaluate_machine_capability(std::move(unknown), complete_machine_evidence());
    CHECK(disabled.support_status == MachineSupportStatus::Disabled);
    CHECK(has_reason(disabled, MachineCapabilityReason::ProfileNotAllowlisted));

    ProfileIdentity unknown_six_identity = zr_ultra_identity();
    unknown_six_identity.setting_id = "unknown-six-nozzle-machine";
    const auto unknown_six =
        evaluate_machine_capability(std::move(unknown_six_identity), complete_machine_evidence(6));
    CHECK(unknown_six.support_status == MachineSupportStatus::PendingValidation);
    CHECK(has_reason(unknown_six, MachineCapabilityReason::ProfileNotAllowlisted));
    CHECK(has_reason(unknown_six, MachineCapabilityReason::SixNozzlePendingValidation));
}

TEST_CASE("current WonderMaker four-nozzle profiles stay pending without missing calibration evidence", "[AI][SmartSlicing]")
{
    MachineCapabilityEvidence profile_evidence;
    for (int index = 1; index <= 4; ++index) {
        NozzleCapabilitySnapshot nozzle;
        nozzle.physical_number = index;
        nozzle.diameter_mm = 0.4;
        profile_evidence.nozzles.push_back(std::move(nozzle));
    }

    for (const ProfileIdentity& identity : {zr_ultra_identity(), zr_ultra_s_identity()}) {
        const auto snapshot = evaluate_machine_capability(identity, profile_evidence);
        CHECK(snapshot.support_status == MachineSupportStatus::PendingValidation);
        CHECK(has_reason(snapshot, MachineCapabilityReason::MissingNozzleOffset));
        CHECK(has_reason(snapshot, MachineCapabilityReason::MissingCollisionClearance));
        CHECK(has_reason(snapshot, MachineCapabilityReason::SpecializedValidationIncomplete));
    }
}

TEST_CASE("material registry accepts colors within one exact family and rejects mixed families", "[AI][SmartSlicing]")
{
    const auto same_family = evaluate_material_compatibility({pla_identity(), pla_identity(), pla_identity()});
    CHECK(same_family.compatible());
    CHECK(same_family.common_family == MaterialFamily::PLA);
    CHECK(same_family.materials.size() == 3);
    CHECK(same_family.materials.front().boundary_policy.allowed_machine_setting_ids ==
          std::vector<std::string>{"xJBdCljSZVDINXnP", "8xoZ6vYv9ws0J97u"});
    CHECK(same_family.materials.front().boundary_policy.allowed_nozzle_diameters_mm == std::vector<double>{0.4});
    CHECK(same_family.materials.front().boundary_policy.temperature_boundary_source ==
          "profile:nozzle_temperature_range");
    CHECK(same_family.materials.front().boundary_policy.flow_boundary_source ==
          "profile:filament_max_volumetric_speed");
    CHECK(same_family.materials.front().boundary_policy.validated);

    const auto mixed = evaluate_material_compatibility({pla_identity(), petg_identity()});
    CHECK_FALSE(mixed.compatible());
    CHECK(std::find(mixed.reasons.begin(), mixed.reasons.end(), MaterialCompatibilityReason::MixedMaterialFamilies) !=
          mixed.reasons.end());
    CHECK(std::string(material_compatibility_reason_name(MaterialCompatibilityReason::MixedMaterialFamilies)) ==
          "unsupported_material_combination");
}

TEST_CASE("restricted and unknown material identities are rejected without display-name inference", "[AI][SmartSlicing]")
{
    const std::array<ProfileIdentity, 5> restricted{{
        {"Rnm03E1ifi5A2vrQ", {"fdm_filament_pet"}, "0ee35c31725dbfc30208717ad755ca06d622f4c9e0664d0c35cad292aa4e774c"},
        {"4B7gD5YwPbUCvLAh", {"fdm_filament_pla"}, "eba50b6924031a426c9b3b3f7ab3d989122db10489d42086c2d07ef5d546a3ee"},
        {"eudiDwK2EVLlGnha", {"fdm_filament_pla"}, "b0cfb5d937a77e2a91a7e7e5b2c8ac5c7093d928ef3f58cd984d1ccbc2a6bad7"},
        {"8kSWEkCpitdwX11c", {"fdm_filament_pla"}, "fe209511f86b563277ed93e873597f566480e84dbd38d23e09f64049a51f6f01"},
        {"PRdEoagTzltjzJCQ", {"fdm_filament_pva"}, "d067cb79dfb6beb6b24f3f92b424e84fe714d70ad30b6718077662d7f417090f"},
    }};
    for (const ProfileIdentity& profile : restricted) {
        const MaterialCapabilitySnapshot result = evaluate_material_profile(profile);
        INFO(profile.setting_id);
        CHECK_FALSE(result.enabled());
        CHECK(result.reasons == std::vector<MaterialCompatibilityReason>{MaterialCompatibilityReason::RestrictedVariant});
    }

    const MaterialCapabilitySnapshot unknown =
        evaluate_material_profile({"unknown", {"fdm_filament_pla"}, std::string(64, 'a')});
    CHECK_FALSE(unknown.enabled());
    CHECK(unknown.family == MaterialFamily::Unknown);
    CHECK(unknown.reasons ==
          std::vector<MaterialCompatibilityReason>{MaterialCompatibilityReason::ProfileNotAllowlisted});

    ProfileIdentity modified = pla_identity();
    modified.fingerprint[0] = '0';
    const auto fingerprint_mismatch = evaluate_material_profile(std::move(modified));
    CHECK_FALSE(fingerprint_mismatch.enabled());
    CHECK(fingerprint_mismatch.reasons ==
          std::vector<MaterialCompatibilityReason>{MaterialCompatibilityReason::ProfileFingerprintMismatch});
}

TEST_CASE("material speed policy preserves immutable facts and registry versions participate in revision", "[AI][SmartSlicing]")
{
    const std::array<MaterialCapabilitySnapshot, 3> material_policies{
        evaluate_material_profile(pla_identity()),
        evaluate_material_profile(petg_identity()),
        evaluate_material_profile(tpu_identity()),
    };
    for (const MaterialCapabilitySnapshot& material : material_policies) {
        REQUIRE(material.enabled());
        CHECK_FALSE(material.speed_policy.may_modify_max_volumetric_flow);
        CHECK_FALSE(material.speed_policy.may_modify_material_temperature);
        CHECK_FALSE(material.speed_policy.may_modify_machine_limits);
    }
    CHECK(material_policies[0].speed_policy.may_increase_process_speed);
    CHECK(material_policies[1].speed_policy.may_increase_process_speed);
    CHECK_FALSE(material_policies[2].speed_policy.may_increase_process_speed);
    CHECK(material_policies[2].family == MaterialFamily::TPU);

    MachineCapabilitySnapshot machine = evaluate_machine_capability(zr_ultra_identity(), complete_machine_evidence());
    MaterialCompatibilitySnapshot materials = evaluate_material_compatibility({pla_identity()});
    const std::string baseline = capability_registry_revision_token(machine, materials);
    machine.registry_version += "-changed";
    CHECK(capability_registry_revision_token(machine, materials) != baseline);
    machine.registry_version = machine_capability_registry_version();
    materials.registry_version += "-changed";
    CHECK(capability_registry_revision_token(machine, materials) != baseline);
}
