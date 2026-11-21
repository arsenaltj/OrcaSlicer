#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r {

struct TransferFilament {
    std::string material;
    std::string color;
};

struct DeviceTransferFilament {
    int slot;
    TransferFilament filament;
};

using DeviceTransferFilaments = std::map<int, DeviceTransferFilament>;

struct SlicedTransferFilament {
    int id;
    std::optional<int> tool;
    TransferFilament filament;
};

enum class TransferFilamentMatch { Unverified, Same, Different };

inline std::string transfer_filament_color(std::string color)
{
    if (!color.empty() && color.front() == '#')
        color.erase(0, 1);
    if ((color.size() != 6 && color.size() != 8) || color.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return {};
    color.resize(6);
    for (char& digit : color)
        if (digit >= 'A' && digit <= 'F')
            digit += 'a' - 'A';
    return '#' + color;
}

inline std::string transfer_filament_material(std::string material)
{
    constexpr auto whitespace = " \t\r\n";
    const auto start = material.find_first_not_of(whitespace);
    if (start == std::string::npos)
        return {};
    material = material.substr(start, material.find_last_not_of(whitespace) - start + 1);
    for (char& letter : material)
        if (letter >= 'a' && letter <= 'z')
            letter -= 'a' - 'A';
    return material;
}

inline void apply_transfer_filament_metadata(TransferFilament& filament, const TransferFilament& metadata)
{
    if (auto material = transfer_filament_material(metadata.material); !material.empty())
        filament.material = std::move(material);
    if (auto color = transfer_filament_color(metadata.color); !color.empty())
        filament.color = std::move(color);
}

inline TransferFilamentMatch compare_transfer_filaments(const TransferFilament& sliced, const TransferFilament& device)
{
    const auto sliced_material = transfer_filament_material(sliced.material);
    const auto device_material = transfer_filament_material(device.material);
    const auto sliced_color = transfer_filament_color(sliced.color);
    const auto device_color = transfer_filament_color(device.color);
    const bool materials_known = !sliced_material.empty() && !device_material.empty();
    const bool colors_known = !sliced_color.empty() && !device_color.empty();
    if ((materials_known && sliced_material != device_material) || (colors_known && sliced_color != device_color))
        return TransferFilamentMatch::Different;
    return materials_known && colors_known ? TransferFilamentMatch::Same : TransferFilamentMatch::Unverified;
}

inline DeviceTransferFilaments read_virtual_printer_transfer_filaments(const nlohmann::json& state,
                                                                       const std::string& instance_uuid)
{
    if (instance_uuid.empty() || !state.is_object() || !state.contains("instance_uuid") ||
        state["instance_uuid"] != instance_uuid || !state.contains("config"))
        return {};

    const auto& config = state["config"];
    if (!config.is_object() || !config.contains("schema") || config["schema"] != 1 ||
        !config.contains("tools") || !config["tools"].is_array() ||
        !config.contains("slots") || !config["slots"].is_array())
        return {};

    const auto& slots = config["slots"];
    const auto& tools = config["tools"];
    DeviceTransferFilaments result;
    for (size_t tool_id = 0; tool_id < tools.size(); ++tool_id) {
        const auto& tool = tools[tool_id];
        if (!tool.is_object() || !tool.contains("slot") || !tool["slot"].is_number_integer() ||
            tool["slot"] < 0 || tool["slot"] >= slots.size())
            continue;

        const auto slot_id = tool["slot"].get<int>();
        const auto& slot = slots[slot_id];
        if (!slot.is_object() || !slot.contains("remaining_g") || !slot["remaining_g"].is_number() ||
            slot["remaining_g"].get<double>() <= 0.0)
            continue;

        const auto text = [&slot](const char* key) -> std::string {
            const auto field = slot.find(key);
            return field != slot.end() && field->is_string() ? field->get<std::string>() : std::string{};
        };
        TransferFilament filament{transfer_filament_material(text("material")), transfer_filament_color(text("color"))};
        if (!filament.material.empty() || !filament.color.empty())
            result.emplace(static_cast<int>(tool_id), DeviceTransferFilament{slot_id, std::move(filament)});
    }
    return result;
}

} // namespace Slic3r
