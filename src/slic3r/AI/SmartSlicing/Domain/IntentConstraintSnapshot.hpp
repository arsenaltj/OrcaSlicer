#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class IntentConstraintType {
    PlatePlacementLock,
    ObjectPlacementLock,
    InstancePlacementLock,
    SupportPainting,
    SeamPainting,
    MulticolorPainting,
    FuzzySkinPainting,
    ModifierVolume,
    ObjectConfigOverride,
    LayerHeightRange,
    VariableLayerHeight
};

enum class IntentConstraintSource {
    OrcaPartPlate,
    UnavailableInOrcaModel,
    ModelVolumeAnnotation,
    ModelVolumeType,
    ModelObjectConfig,
    ModelLayerConfigRange,
    ModelLayerHeightProfile
};

enum class IntentConstraintState { Inactive, Active, Unknown };

struct IntentConstraintRecord
{
    IntentConstraintType type{IntentConstraintType::PlatePlacementLock};
    IntentConstraintSource source{IntentConstraintSource::UnavailableInOrcaModel};
    IntentConstraintState state{IntentConstraintState::Unknown};
    uint64_t object_id{0};
    uint64_t instance_id{0};
    uint64_t volume_id{0};
    uint64_t source_revision{0};
    int subtype_code{-1};
    bool has_layer_range{false};
    double lower_z{0.0};
    double upper_z{0.0};
    std::vector<std::string> parameter_keys;

    bool is_hard_constraint() const { return state == IntentConstraintState::Active; }
};

struct ProcessParameterBaselineSnapshot
{
    std::vector<std::string> parameter_keys;

    bool is_optimizable_baseline() const { return true; }
    bool is_hard_constraint() const { return false; }
};

struct IntentConstraintSnapshot
{
    std::vector<IntentConstraintRecord> records;
    ProcessParameterBaselineSnapshot global_process_parameters;
};

const char* intent_constraint_type_name(IntentConstraintType type);
const char* intent_constraint_source_name(IntentConstraintSource source);
uint64_t intent_constraints_revision(const IntentConstraintSnapshot& snapshot);

} // namespace Slic3r::AI::SmartSlicing
