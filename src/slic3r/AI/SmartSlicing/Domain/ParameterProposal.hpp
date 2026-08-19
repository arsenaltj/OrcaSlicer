#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct ConfigPatchEntry
{
    std::string key;
    std::variant<bool, int64_t, double, std::string> value;
    std::string scope;
};

struct ParameterProposal
{
    std::vector<ConfigPatchEntry> entries;
    std::vector<std::string> explanation_codes;
};

} // namespace Slic3r::AI::SmartSlicing
