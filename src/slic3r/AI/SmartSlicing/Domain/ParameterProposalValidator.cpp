#include "ParameterProposalValidator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string_view>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

enum class ValueKind { Boolean, Integer, Floating, Enumeration };

struct Rule
{
    std::string_view key;
    ValueKind kind;
    double minimum{0.0};
    double maximum{0.0};
    double maximum_delta{0.0};
    std::initializer_list<std::string_view> values;
};

const std::array<Rule, 9>& rules()
{
    static const std::array<Rule, 9> value = {{
        {"layer_height", ValueKind::Floating, 0.04, 0.40, 0.12, {}},
        {"wall_loops", ValueKind::Integer, 1.0, 20.0, 4.0, {}},
        {"top_shell_layers", ValueKind::Integer, 0.0, 20.0, 5.0, {}},
        {"bottom_shell_layers", ValueKind::Integer, 0.0, 20.0, 5.0, {}},
        {"enable_support", ValueKind::Boolean, 0.0, 0.0, 0.0, {}},
        {"brim_width", ValueKind::Floating, 0.0, 30.0, 10.0, {}},
        {"initial_layer_print_height", ValueKind::Floating, 0.04, 1.0, 0.30, {}},
        {"support_interface_top_layers", ValueKind::Integer, 0.0, 10.0, 4.0, {}},
        {"seam_position", ValueKind::Enumeration, 0.0, 0.0, 0.0,
         {"nearest", "aligned", "aligned_back", "back", "random"}},
    }};
    return value;
}

bool forbidden_key(std::string_view key)
{
    static constexpr std::array<std::string_view, 12> forbidden = {
        "nozzle_diameter", "printable_area", "printable_height", "machine_max_acceleration_x",
        "machine_max_acceleration_y", "machine_max_speed_x", "machine_max_speed_y", "filament_flow_ratio",
        "pressure_advance", "nozzle_temperature", "bed_temperature", "flush_volumes_matrix"};
    return std::find(forbidden.begin(), forbidden.end(), key) != forbidden.end();
}

const Rule* find_rule(std::string_view key)
{
    const auto& values = rules();
    const auto found = std::find_if(values.begin(), values.end(), [key](const Rule& rule) { return rule.key == key; });
    return found == values.end() ? nullptr : &*found;
}

bool has_kind(const ConfigValue& value, ValueKind kind)
{
    switch (kind) {
    case ValueKind::Boolean: return std::holds_alternative<bool>(value);
    case ValueKind::Integer: return std::holds_alternative<int64_t>(value);
    case ValueKind::Floating: return std::holds_alternative<double>(value);
    case ValueKind::Enumeration: return std::holds_alternative<std::string>(value);
    }
    return false;
}

double numeric_value(const ConfigValue& value)
{
    return std::holds_alternative<int64_t>(value) ? static_cast<double>(std::get<int64_t>(value)) :
                                                    std::get<double>(value);
}

void reject(ParameterValidationResult& result, ParameterRejectionCode code, size_t index, const std::string& key)
{
    result.rejections.push_back({code, index, key});
}

} // namespace

ParameterValidationResult ParameterProposalValidator::validate(const ParameterProposal& proposal) const
{
    ParameterValidationResult result;
    if (proposal.entries.empty()) {
        reject(result, ParameterRejectionCode::EmptyProposal, 0, {});
        return result;
    }
    if (proposal.entries.size() > MAX_CHANGES) {
        reject(result, ParameterRejectionCode::TooManyChanges, proposal.entries.size(), {});
        return result;
    }

    std::set<std::tuple<ConfigScope, PresetOwner, int64_t, std::string>> seen;
    for (size_t index = 0; index < proposal.entries.size(); ++index) {
        const ConfigPatchEntry& entry = proposal.entries[index];
        if (entry.scope != ConfigScope::Plate) {
            reject(result, ParameterRejectionCode::ScopeNotAllowed, index, entry.key);
            continue;
        }
        if (entry.owner != PresetOwner::Process) {
            reject(result, ParameterRejectionCode::OwnerNotAllowed, index, entry.key);
            continue;
        }
        if (entry.target_id < 0) {
            reject(result, ParameterRejectionCode::TargetNotSpecified, index, entry.key);
            continue;
        }
        if (!seen.emplace(entry.scope, entry.owner, entry.target_id, entry.key).second) {
            reject(result, ParameterRejectionCode::DuplicateChange, index, entry.key);
            continue;
        }
        if (forbidden_key(entry.key)) {
            reject(result, ParameterRejectionCode::ForbiddenKey, index, entry.key);
            continue;
        }
        const Rule* rule = find_rule(entry.key);
        if (rule == nullptr) {
            reject(result, ParameterRejectionCode::UnknownKey, index, entry.key);
            continue;
        }
        if (!has_kind(entry.expected_value, rule->kind) || !has_kind(entry.new_value, rule->kind)) {
            reject(result, ParameterRejectionCode::TypeMismatch, index, entry.key);
            continue;
        }
        if (entry.expected_value == entry.new_value) {
            reject(result, ParameterRejectionCode::NoEffectiveChange, index, entry.key);
            continue;
        }
        if (rule->kind == ValueKind::Integer || rule->kind == ValueKind::Floating) {
            const double old_value = numeric_value(entry.expected_value);
            const double new_value = numeric_value(entry.new_value);
            if (!std::isfinite(old_value) || !std::isfinite(new_value) || old_value < rule->minimum ||
                old_value > rule->maximum || new_value < rule->minimum || new_value > rule->maximum) {
                reject(result, ParameterRejectionCode::RangeViolation, index, entry.key);
                continue;
            }
            if (std::abs(new_value - old_value) > rule->maximum_delta) {
                reject(result, ParameterRejectionCode::ChangeBudgetExceeded, index, entry.key);
                continue;
            }
        } else if (rule->kind == ValueKind::Enumeration) {
            const std::string& current = std::get<std::string>(entry.expected_value);
            const std::string& selected = std::get<std::string>(entry.new_value);
            if (std::find(rule->values.begin(), rule->values.end(), current) == rule->values.end() ||
                std::find(rule->values.begin(), rule->values.end(), selected) == rule->values.end()) {
                reject(result, ParameterRejectionCode::EnumViolation, index, entry.key);
                continue;
            }
        }
    }
    return result;
}

const char* parameter_rejection_code_name(ParameterRejectionCode code)
{
    switch (code) {
    case ParameterRejectionCode::EmptyProposal: return "empty_parameter_proposal";
    case ParameterRejectionCode::TooManyChanges: return "parameter_change_count_exceeded";
    case ParameterRejectionCode::DuplicateChange: return "duplicate_parameter_change";
    case ParameterRejectionCode::UnknownKey: return "parameter_key_not_allowed";
    case ParameterRejectionCode::TypeMismatch: return "parameter_type_mismatch";
    case ParameterRejectionCode::RangeViolation: return "parameter_range_violation";
    case ParameterRejectionCode::EnumViolation: return "parameter_enum_violation";
    case ParameterRejectionCode::ScopeNotAllowed: return "parameter_scope_not_allowed";
    case ParameterRejectionCode::OwnerNotAllowed: return "parameter_owner_not_allowed";
    case ParameterRejectionCode::TargetNotSpecified: return "parameter_target_missing";
    case ParameterRejectionCode::ForbiddenKey: return "parameter_key_forbidden";
    case ParameterRejectionCode::ChangeBudgetExceeded: return "parameter_change_budget_exceeded";
    case ParameterRejectionCode::NoEffectiveChange: return "parameter_change_is_noop";
    }
    return "parameter_validation_failed";
}

} // namespace Slic3r::AI::SmartSlicing
