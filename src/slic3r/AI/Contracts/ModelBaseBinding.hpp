#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r::AI {

// Physical identity, independent of the portrait's region assignments. In
// particular model white (slot 4) is never support white (slot 6).
struct ModelBaseBinding {
    std::string suite_id {"honor.people.six"};
    std::string suite_revision {"1"};
    std::string slot_id {"charcoal"};
    size_t physical_slot {0};

    bool valid() const {
        static const std::array<const char*, 5> ids {{"charcoal", "peach", "skin-light", "milk-white", "gray-brown"}};
        return suite_id == "honor.people.six" && suite_revision == "1" &&
            physical_slot < ids.size() && slot_id == ids[physical_slot];
    }
    nlohmann::json encode() const {
        if (!valid()) throw std::invalid_argument("Invalid independent base slot.");
        return {{"schema", "orca.model-base-binding/v1"}, {"suite_id", suite_id},
            {"suite_revision", suite_revision}, {"slot_id", slot_id}, {"physical_slot", physical_slot}};
    }
    static bool decode(const nlohmann::json& j, ModelBaseBinding& result) {
        try {
            if (j.at("schema") != "orca.model-base-binding/v1" || !j.at("physical_slot").is_number_integer() ||
                j.at("physical_slot").get<int64_t>() < 0) return false;
            ModelBaseBinding b {j.at("suite_id").get<std::string>(), j.at("suite_revision").get<std::string>(),
                j.at("slot_id").get<std::string>(), j.at("physical_slot").get<size_t>()};
            if (!b.valid()) return false;
            result = std::move(b); return true;
        } catch (const std::exception&) { return false; }
    }
};
} // namespace Slic3r::AI
