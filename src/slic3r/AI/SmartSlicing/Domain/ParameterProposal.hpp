#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

using ConfigValue = std::variant<bool, int64_t, double, std::string>;

enum class ConfigScope { Plate, Object, Material, Workspace };
enum class PresetOwner { Process, Filament, Printer, Project };
enum class ParameterIntent { Unspecified, Stability, Quality, Speed, MaterialSaving };

struct ConfigPatchEntry
{
    ConfigScope scope{ConfigScope::Plate};
    PresetOwner owner{PresetOwner::Process};
    int64_t target_id{-1};
    std::string key;
    ConfigValue expected_value{false};
    ConfigValue new_value{false};
    std::string reason_code;
};

struct ParameterProposal
{
    ParameterIntent intent{ParameterIntent::Unspecified};
    std::vector<ConfigPatchEntry> entries;
    std::vector<std::string> explanation_codes;
};

} // namespace Slic3r::AI::SmartSlicing
