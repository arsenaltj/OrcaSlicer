#include "slic3r/AI/SmartSlicing/Domain/SixNozzleValidation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

using namespace Slic3r::AI::SmartSlicing;

namespace {

template<class T>
EvidenceValue<T> available(T value, const char* source = "fixture")
{
    EvidenceValue<T> result;
    result.availability = EvidenceAvailability::Available;
    result.value = std::move(value);
    result.source_code = source;
    return result;
}

ProfileIdentity profile(const std::string& id)
{
    return {id, {"six-nozzle-parent"}, id + "-fingerprint"};
}

SixNozzleValidationInput complete_input()
{
    SixNozzleValidationInput input;
    input.machine.registry_version = machine_capability_registry_version();
    input.machine.profile = profile("six-nozzle-profile");
    input.machine.support_status = MachineSupportStatus::PendingValidation;
    input.machine.evidence.independently_addressable = available(true, "machine.addressing");
    input.machine.evidence.collision_clearance =
        available(CollisionClearanceLimits{0.4, 1.0, 2.0}, "machine.clearance");
    input.machine.evidence.tool_change_gcode =
        available(ToolChangeGcodeCapability{"T{target_tool}", true}, "machine.tool_change");
    input.machine.evidence.preheat_behavior = available(PreheatBehavior{2.0, 180.0}, "machine.preheat");
    input.machine.evidence.standby_behavior = available(StandbyBehavior{150.0, 5.0}, "machine.standby");
    input.machine.evidence.retraction_behavior = available(RetractionBehavior{0.8, 25.0}, "machine.retraction");
    input.machine.evidence.tool_change_time_model = available(ToolChangeTimeModel{4.0, 8.0}, "machine.time_model");
    input.machine.evidence.prime_or_wipe = available(PrimeOrWipeCapability{true, true}, "machine.prime_wipe");
    input.machine.evidence.flush_matrix = available(FlushMatrix{6, std::vector<double>(36, 1.0)}, "machine.flush");
    input.machine.evidence.wipe_tower_space_constraints =
        available(WipeTowerSpaceConstraints{20.0, 20.0, 1.0}, "machine.wipe_tower");
    input.machine.evidence.specialized_validation_complete = available(true, "machine.specialized");
    for (int number = 1; number <= 6; ++number) {
        NozzleCapabilitySnapshot nozzle;
        nozzle.physical_number = number;
        nozzle.diameter_mm = 0.4;
        nozzle.offset = available(NozzleOffset{static_cast<double>(number), 0.0}, "machine.offset");
        nozzle.reachable_area = available(ReachableArea{0.0, 300.0, 0.0, 300.0}, "machine.reachable");
        input.machine.evidence.nozzles.push_back(std::move(nozzle));

        MaterialCapabilitySnapshot material;
        material.profile = profile("material-" + std::to_string(number));
        material.family = MaterialFamily::PLA;
        material.support_status = MaterialSupportStatus::Enabled;
        input.materials.materials.push_back(std::move(material));

        SixNozzleSlotMapping mapping;
        mapping.physical_number = number;
        mapping.material_id = available("material-" + std::to_string(number), "slot.material");
        mapping.color_id = available("color-" + std::to_string(number), "slot.color");
        input.slot_mappings.push_back(std::move(mapping));
    }
    input.materials.registry_version = material_compatibility_registry_version();
    input.materials.combination_status = MaterialCombinationStatus::Compatible;
    input.materials.common_family = MaterialFamily::PLA;
    input.persistence.save_restore_round_trip = available(true, "persistence.round_trip");
    input.persistence.restored_fingerprint =
        available(canonical_six_nozzle_fingerprint(input), "persistence.fingerprint");
    return input;
}

} // namespace

TEST_CASE("Complete six nozzle evidence enables the contract", "[AI][SmartSlicing][D9T1]")
{
    const auto result = validate_six_nozzle(complete_input());
    CHECK(result.status == SixNozzleValidationStatus::Enabled);
    CHECK(result.enabled());
    CHECK(result.diagnostic_codes.empty());
    CHECK(result.canonical_fingerprint.rfind("fnv1a64:", 0) == 0);
}

TEST_CASE("Six nozzle canonical fingerprint is independent of input vector order", "[AI][SmartSlicing][D9T1]")
{
    auto ordered = complete_input();
    auto reversed = ordered;
    std::reverse(reversed.machine.evidence.nozzles.begin(), reversed.machine.evidence.nozzles.end());
    std::reverse(reversed.materials.materials.begin(), reversed.materials.materials.end());
    std::reverse(reversed.slot_mappings.begin(), reversed.slot_mappings.end());
    reversed.persistence.restored_fingerprint = available(canonical_six_nozzle_fingerprint(reversed), "persistence.fingerprint");
    const auto first = validate_six_nozzle(ordered);
    const auto second = validate_six_nozzle(reversed);
    CHECK(first.canonical_fingerprint == second.canonical_fingerprint);
    CHECK(first.diagnostic_codes == second.diagnostic_codes);
}

TEST_CASE("Missing six nozzle evidence remains pending validation", "[AI][SmartSlicing][D9T1]")
{
    const std::vector<std::function<void(SixNozzleValidationInput&)>> remove_evidence{
        [](auto& input) { input.machine.evidence.independently_addressable = {}; },
        [](auto& input) { input.machine.evidence.nozzles.front().offset = {}; },
        [](auto& input) { input.machine.evidence.nozzles.front().reachable_area = {}; },
        [](auto& input) { input.machine.evidence.collision_clearance = {}; },
        [](auto& input) { input.machine.evidence.tool_change_gcode = {}; },
        [](auto& input) { input.machine.evidence.preheat_behavior = {}; },
        [](auto& input) { input.machine.evidence.standby_behavior = {}; },
        [](auto& input) { input.machine.evidence.retraction_behavior = {}; },
        [](auto& input) { input.machine.evidence.tool_change_time_model = {}; },
        [](auto& input) { input.machine.evidence.prime_or_wipe = {}; },
        [](auto& input) { input.machine.evidence.flush_matrix = {}; },
        [](auto& input) { input.machine.evidence.wipe_tower_space_constraints = {}; },
        [](auto& input) { input.machine.evidence.specialized_validation_complete = {}; },
        [](auto& input) { input.slot_mappings.front().material_id = {}; },
        [](auto& input) { input.slot_mappings.front().color_id = {}; },
        [](auto& input) { input.persistence.save_restore_round_trip = {}; },
        [](auto& input) { input.persistence.restored_fingerprint = {}; },
        [](auto& input) { input.materials.combination_status = MaterialCombinationStatus::Unsupported; },
    };
    for (const auto& mutate : remove_evidence) {
        auto input = complete_input();
        mutate(input);
        input.persistence.restored_fingerprint = {};
        CHECK(validate_six_nozzle(input).status == SixNozzleValidationStatus::PendingValidation);
    }
}

TEST_CASE("Six nozzle duplicate or conflicting identities are rejected", "[AI][SmartSlicing][D9T1]")
{
    auto duplicate_nozzle = complete_input();
    duplicate_nozzle.machine.evidence.nozzles.back().physical_number = 1;
    CHECK(validate_six_nozzle(duplicate_nozzle).status == SixNozzleValidationStatus::Rejected);

    auto duplicate_slot = complete_input();
    duplicate_slot.slot_mappings.back().physical_number = 1;
    CHECK(validate_six_nozzle(duplicate_slot).status == SixNozzleValidationStatus::Rejected);

    auto bad_flush = complete_input();
    bad_flush.machine.evidence.flush_matrix.value->tool_count = 5;
    bad_flush.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(bad_flush).status == SixNozzleValidationStatus::PendingValidation);
}

TEST_CASE("Six nozzle save restore fingerprint and contract versions are guarded", "[AI][SmartSlicing][D9T1]")
{
    auto mismatch = complete_input();
    mismatch.persistence.restored_fingerprint =
        available(std::string("fnv1a64:wrong"), "persistence.fingerprint");
    CHECK(validate_six_nozzle(mismatch).status == SixNozzleValidationStatus::Rejected);

    auto future = complete_input();
    future.policy_version = "future-policy";
    CHECK(validate_six_nozzle(future).status == SixNozzleValidationStatus::Rejected);

    CHECK(production_six_nozzle_validation().status == SixNozzleValidationStatus::PendingValidation);
}

TEST_CASE("Six nozzle registry and numeric evidence boundaries fail closed", "[AI][SmartSlicing][D9T1]")
{
    auto future_material_registry = complete_input();
    future_material_registry.materials.registry_version = "future-material-registry";
    CHECK(validate_six_nozzle(future_material_registry).status == SixNozzleValidationStatus::Rejected);

    auto invalid_offset = complete_input();
    invalid_offset.machine.evidence.nozzles.front().offset.value->x_mm =
        std::numeric_limits<double>::quiet_NaN();
    invalid_offset.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_offset).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_reachable = complete_input();
    invalid_reachable.machine.evidence.nozzles.front().reachable_area.value->minimum_x_mm = 300.0;
    invalid_reachable.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_reachable).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_clearance = complete_input();
    invalid_clearance.machine.evidence.collision_clearance.value->nozzle_radius_mm =
        std::numeric_limits<double>::quiet_NaN();
    invalid_clearance.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_clearance).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_thermal = complete_input();
    invalid_thermal.machine.evidence.preheat_behavior.value->lead_time_seconds =
        std::numeric_limits<double>::quiet_NaN();
    invalid_thermal.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_thermal).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_retraction = complete_input();
    invalid_retraction.machine.evidence.retraction_behavior.value->length_mm = -1.0;
    invalid_retraction.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_retraction).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_time_model = complete_input();
    invalid_time_model.machine.evidence.tool_change_time_model.value->fixed_seconds = -1.0;
    invalid_time_model.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_time_model).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_flush = complete_input();
    invalid_flush.machine.evidence.flush_matrix.value->volume_mm3.front() =
        std::numeric_limits<double>::quiet_NaN();
    invalid_flush.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_flush).status == SixNozzleValidationStatus::PendingValidation);

    auto invalid_wipe = complete_input();
    invalid_wipe.machine.evidence.wipe_tower_space_constraints.value->minimum_width_mm = 0.0;
    invalid_wipe.persistence.restored_fingerprint = {};
    CHECK(validate_six_nozzle(invalid_wipe).status == SixNozzleValidationStatus::PendingValidation);
}
