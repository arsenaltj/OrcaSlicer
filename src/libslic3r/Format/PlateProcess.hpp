#pragma once
// One strict contract for the native reader and read-only SDK inspection.
#include "libslic3r/PrintConfig.hpp"
#include <array>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <locale>
#include <sstream>
#include <string_view>

namespace Slic3r::PlateProcess {
inline constexpr std::string_view prefix = "orca_plate_process:";
inline constexpr std::array<std::string_view, 11> keys{
    "layer_height", "wall_loops", "sparse_infill_density", "sparse_infill_pattern",
    "outer_wall_line_width", "outer_wall_speed", "enable_support", "support_type",
    "support_threshold_angle", "support_on_build_plate_only", "wall_generator"};
inline bool has_prefix(const std::string& key) { return key.compare(0, prefix.size(), prefix) == 0; }
inline bool reserved_key(const std::string& key) {
    return has_prefix(key) || key.rfind("orca_workbench_slicing_",0)==0 || key.rfind("orca_workbench_curve_",0)==0;
}
inline double number(const std::string& text) {
    // Native scalar deserializers accept trailing junk; check complete lexical
    // consumption before invoking them, independently of the process locale.
    std::istringstream input(text); input.imbue(std::locale::classic());
    double value = 0.; input >> std::noskipws >> value;
    if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
        throw std::invalid_argument("PLATE_PROCESS_INVALID_NUMBER");
    return value;
}
inline void range(double value, double lo, double hi) {
    if (value < lo || value > hi) throw std::invalid_argument("PLATE_PROCESS_OUT_OF_RANGE");
}
inline DynamicPrintConfig parse(const std::string& key, const std::string& value) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
        throw std::invalid_argument("PLATE_PROCESS_UNKNOWN_KEY:" + key);
    if (value.empty() || value.size() > 1024)
        throw std::invalid_argument("PLATE_PROCESS_INVALID_LENGTH:" + key);
    const auto* def = print_config_def.get(key);
    if (!def) throw std::invalid_argument("PLATE_PROCESS_MISSING_DEFINITION:" + key);
    if (key == "enable_support" || key == "support_on_build_plate_only") {
        if (value != "0" && value != "1") throw std::invalid_argument("PLATE_PROCESS_INVALID_BOOL:" + key);
    } else if (key == "wall_loops" || key == "support_threshold_angle") {
        if (value.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("PLATE_PROCESS_INVALID_INT:" + key);
        range(number(value), def->min, def->max);
    } else if (key == "sparse_infill_pattern" || key == "support_type" || key == "wall_generator") {
        if (std::find(def->enum_values.begin(), def->enum_values.end(), value) == def->enum_values.end())
            throw std::invalid_argument("PLATE_PROCESS_INVALID_ENUM:" + key);
    } else if (key == "outer_wall_speed") {
        size_t begin = 0;
        do {
            const size_t end = value.find(',', begin);
            const auto token = value.substr(begin, end == std::string::npos ? end : end-begin);
            if (token != "nil") range(number(token), def->min, def->max);
            if (end == std::string::npos) break;
            begin = end + 1;
        } while (true);
    } else {
        const bool percent = value.back() == '%';
        if (percent && key == "layer_height") throw std::invalid_argument("PLATE_PROCESS_INVALID_TYPE:" + key);
        const double numeric = number(percent ? value.substr(0, value.size()-1) : value);
        range(numeric, def->min, def->max);
        if (key == "outer_wall_line_width" && !percent) range(numeric, 0., 10.);
    }
    DynamicPrintConfig parsed;
    parsed.set_deserialize_strict(key, value);
    return parsed;
}
inline void apply(DynamicPrintConfig& config, const std::string& key, const std::string& value) {
    auto parsed = parse(key, value);
    if (const auto* previous = config.option(key))
        if (previous->serialize() != parsed.option(key)->serialize())
            throw std::invalid_argument("PLATE_PROCESS_CONFLICTING_DUPLICATE:" + key);
    config.apply(parsed);
}
} // namespace Slic3r::PlateProcess
