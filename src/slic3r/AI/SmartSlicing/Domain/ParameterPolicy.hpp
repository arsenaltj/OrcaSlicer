#pragma once

#include "ParameterProposal.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ConfigValueKind { Boolean, Integer, Floating, Enumeration };

enum class ParameterRiskClass { StrategyControlled, CalibratedRange, ImmutableFact };

enum class BoundSource {
    PolicyRange,
    ProcessProfile,
    MachineProcessMaterialIntersection,
    CalibratedProfileIntersection,
    ImmutableFact,
};

enum class BoundEvidenceSource { MachineProfile, ProcessProfile, MaterialProfile };

enum class IntentConflictRule {
    None,
    PlacementLock,
    SupportPainting,
    SeamPainting,
    MulticolorPainting,
    ExplicitParameterOverride,
};

struct NumericPolicyBounds
{
    double minimum{0.0};
    double maximum{0.0};
};

struct ParameterPolicyEntry
{
    std::string key;
    ConfigValueKind value_kind{ConfigValueKind::Boolean};
    std::vector<ConfigScope> scopes;
    PresetOwner owner{PresetOwner::Process};
    ParameterRiskClass risk_class{ParameterRiskClass::StrategyControlled};
    BoundSource bound_source{BoundSource::PolicyRange};
    std::vector<std::string> allowed_enum_values;
    std::vector<RecommendationGoal> allowed_goals;
    IntentConflictRule intent_rule{IntentConflictRule::None};
    std::string explanation_code;
    std::optional<NumericPolicyBounds> safety_bounds;
    std::optional<double> maximum_change;
};

struct ParameterPatchSetBudget
{
    std::string version;
    size_t maximum_registered_changes{0};
    size_t maximum_serialized_bytes{4096};
    std::array<size_t, 4> maximum_changes_per_scope{{16, 8, 4, 4}};
};

class ParameterPolicyRegistry
{
public:
    static const ParameterPolicyRegistry& current();

    const std::string& version() const { return m_version; }
    const std::vector<ParameterPolicyEntry>& entries() const { return m_entries; }
    const ParameterPatchSetBudget& budget() const { return m_budget; }
    const ParameterPolicyEntry* find(const std::string& key) const;

private:
    ParameterPolicyRegistry();

    std::string m_version;
    std::vector<ParameterPolicyEntry> m_entries;
    ParameterPatchSetBudget m_budget;
};

const char* config_value_kind_name(ConfigValueKind kind);
const char* parameter_risk_class_name(ParameterRiskClass risk_class);
const char* bound_source_name(BoundSource source);
const char* intent_conflict_rule_name(IntentConflictRule rule);
size_t config_scope_index(ConfigScope scope);
size_t parameter_patch_set_serialized_bytes(const ParameterProposal& proposal);

} // namespace Slic3r::AI::SmartSlicing
