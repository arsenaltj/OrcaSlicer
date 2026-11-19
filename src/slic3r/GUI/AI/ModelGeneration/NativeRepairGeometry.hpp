#pragma once
#include "libslic3r/MeshRepair.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>

namespace Slic3r::GUI {
inline nlohmann::json native_repair_receipt(const StrictMeshRepairResult& result)
{
    if (result.acceptance != MeshRepairAcceptance::OrcaNative || !result.native_repair_succeeded ||
        !result.success || result.canceled) return nullptr;
    return {{"schema", "orca.native-repair-acceptance/v1"},
        {"official", "93941dc34e8a534ce3966a5979fe1edac002cb68"},
        {"source_geometry_id", result.native_source_geometry_id},
        {"output_geometry_id", result.native_output_geometry_id}};
}

inline bool valid_native_repair_receipt(const nlohmann::json& receipt)
{
    try {
        const auto hash = [](const nlohmann::json& value) {
            if (!value.is_string()) return false;
            const auto text = value.get<std::string>();
            return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        };
        return receipt.is_object() && receipt.size() == 4 &&
            receipt.at("schema") == "orca.native-repair-acceptance/v1" &&
            receipt.at("official") == "93941dc34e8a534ce3966a5979fe1edac002cb68" &&
            hash(receipt.at("source_geometry_id")) && hash(receipt.at("output_geometry_id"));
    } catch (...) { return false; }
}

inline bool native_repair_receipt_matches(const nlohmann::json& receipt, const indexed_triangle_set& mesh)
{
    return valid_native_repair_receipt(receipt) &&
        mesh_repair_geometry_id(mesh) == receipt.at("output_geometry_id");
}

inline nlohmann::json native_repair_evidence(const StrictMeshRepairResult& result)
{
    const auto receipt = native_repair_receipt(result);
    if (receipt.is_null()) return nullptr;
    const auto& t = result.output_topology;
    return {{"schema", "orca.native-repair-evidence/v1"}, {"receipt", receipt},
        {"topology", {{"open_edges", t.open_edges}, {"nonmanifold_edges", t.nonmanifold_edges},
            {"nonmanifold_vertices", t.nonmanifold_vertices}, {"degenerate_faces", t.degenerate_faces},
            {"duplicate_faces", t.duplicate_faces}, {"inconsistent_edges", t.inconsistent_edges},
            {"self_intersections_checked", t.self_intersections_checked}, {"self_intersects", t.self_intersects}}},
        {"sampled_deviation_ratio", result.sampled_deviation_ratio}, {"area_change_ratio", result.area_change_ratio}};
}

inline bool valid_native_repair_evidence(const nlohmann::json& evidence, const nlohmann::json& receipt)
{
    try {
        if (evidence.at("schema") != "orca.native-repair-evidence/v1" ||
            evidence.at("receipt") != receipt || !valid_native_repair_receipt(receipt)) return false;
        const auto& t = evidence.at("topology");
        for (const char* key : {"open_edges", "nonmanifold_edges", "nonmanifold_vertices",
                "degenerate_faces", "duplicate_faces", "inconsistent_edges"}) {
            const auto& count = t.at(key);
            if (!count.is_number_unsigned() && (!count.is_number_integer() || count.get<int64_t>() < 0)) return false;
        }
        // A native execution may keep topology for which the strict CGAL
        // predicate is unavailable. Preserve that fact rather than inventing
        // a zero-intersection result or rejecting the native success again.
        (void)t.at("self_intersections_checked").get<bool>();
        (void)t.at("self_intersects").get<bool>();
        for (const char* key : {"sampled_deviation_ratio", "area_change_ratio"})
            if (!std::isfinite(evidence.at(key).get<double>()) || evidence.at(key).get<double>() < 0.) return false;
        return true;
    } catch (...) { return false; }
}

inline bool native_repair_topology_allowed(const nlohmann::json& strict, const nlohmann::json& native)
{
    try {
        if (!strict.at("success").get<bool>() || strict.at("canceled").get<bool>() ||
            strict.at("output_topology") != native.at("topology")) return false;
        if (native.contains("native_acceptance"))
            return native.at("schema") == "orca.native-workbench-topology-certificate/v2" &&
                strict.at("native_repair_succeeded").get<bool>() &&
                strict.at("acceptance") == "orca-native" &&
                strict.at("native_acceptance") == native.at("native_acceptance") &&
                valid_native_repair_receipt(native.at("native_acceptance"));
        if (native.at("schema") != "orca.native-workbench-topology-certificate/v1") return false;
        const auto& topology = native.at("topology");
        for (const char* key : {"open_edges", "nonmanifold_edges", "nonmanifold_vertices",
                "degenerate_faces", "duplicate_faces", "inconsistent_edges"})
            if (topology.at(key).get<size_t>() != 0) return false;
        return topology.at("self_intersections_checked").get<bool>() &&
            !topology.at("self_intersects").get<bool>();
    } catch (...) { return false; }
}

inline bool native_repair_workbench_facts_match(const nlohmann::json& check, const nlohmann::json& topology)
{
    try {
        if (check.at("boundary_edges") != topology.at("open_edges")) return false;
        for (const char* key : {"nonmanifold_edges", "nonmanifold_vertices", "degenerate_faces", "duplicate_faces", "inconsistent_edges"})
            if (check.at(key) != topology.at(key)) return false;
        return check.at("self_intersections_checked") == topology.at("self_intersections_checked") &&
            (check.at("self_intersections").get<size_t>() != 0) == topology.at("self_intersects").get<bool>();
    } catch (...) { return false; }
}
// One policy reader for restore and native import. Old strict receipts keep
// their original limits. Native acceptance must appear in BOTH the build
// report and the hashed topology certificate; a lone boolean never grants it.
inline bool native_repair_geometry_allowed(const nlohmann::json& strict, const nlohmann::json& native)
{
    try {
        const double deviation = strict.at("sampled_deviation_ratio").get<double>();
        const double area = strict.at("area_change_ratio").get<double>();
        if (!std::isfinite(deviation) || deviation < 0.0 || !std::isfinite(area) || area < 0.0 ||
            native.at("sampled_deviation_ratio").get<double>() != deviation ||
            native.at("area_change_ratio").get<double>() != area) return false;
        const StrictMeshRepairOptions limits;
        if (native.contains("native_acceptance"))
            return native_repair_topology_allowed(strict, native) &&
                strict.value("native_repair_compatible", false) && native.value("native_repair_compatible", false);
        if (strict.at("geometry_verified").get<bool>())
            return !strict.value("native_repair_compatible", false) &&
                !native.value("native_repair_compatible", false) &&
                deviation <= limits.maximum_sampled_deviation_ratio && area <= limits.maximum_area_change_ratio;
        return strict.value("native_repair_compatible", false) && native.value("native_repair_compatible", false) &&
            strict.at("success").get<bool>() && !strict.at("canceled").get<bool>() &&
            strict.at("enhanced_repair_attempted").get<bool>() && strict.at("strategy") == "orca-native";
    } catch (...) { return false; }
}
}
