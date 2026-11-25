#pragma once

#include "slic3r/GUI/AI/Model/ModelFilePath.hpp"
#include "PortraitNativePartition.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafDomain.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <cstring>
#include <limits>

namespace Slic3r::GUI::NativePortrait::chunk_cache {
inline constexpr size_t rows_per_chunk = 16384;
inline constexpr size_t block_bytes = 16ULL * 1024 * 1024;
inline bool hash_valid(const std::string& hash) {
    return AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash);
}
inline std::string publish(const nlohmann::json& document, const boost::filesystem::path& root,
    const char* extension)
{
    const auto bytes = document.dump();
    if (bytes.empty() || bytes.size() > block_bytes) throw std::runtime_error("分区缓存块超过资源预算。");
    const auto hash = AI::beauty_leaf_digest(bytes);
    if (!hash_valid(hash)) throw std::runtime_error("分区缓存块摘要计算失败。");
    boost::filesystem::create_directories(AI::model_io_path(root));
    const auto path = root / (hash + extension);
    if (boost::filesystem::exists(AI::model_io_path(path))) {
        if (boost::filesystem::is_symlink(AI::model_io_path(path)) || AI::model_artifact_sha256(path) != hash)
            throw std::runtime_error("已有分区缓存块损坏。");
        return hash;
    }
    const auto temporary = root / boost::filesystem::unique_path("partition-block-%%%%-%%%%.tmp");
    try {
        boost::filesystem::ofstream output(AI::model_io_path(temporary), std::ios::binary);
        output.write(bytes.data(), std::streamsize(bytes.size())); output.close();
        if (!output || AI::model_artifact_sha256(temporary) != hash)
            throw std::runtime_error("分区缓存块写入或校验失败。");
        boost::filesystem::rename(AI::model_io_path(temporary), AI::model_io_path(path));
    } catch (...) {
        boost::system::error_code ignored; boost::filesystem::remove(AI::model_io_path(temporary), ignored); throw;
    }
    return hash;
}
inline nlohmann::json read(const boost::filesystem::path& root, const std::string& hash,
    const char* extension)
{
    if (!hash_valid(hash)) throw std::runtime_error("分区缓存块身份无效。");
    const auto path = root / (hash + extension);
    if (boost::filesystem::is_symlink(AI::model_io_path(path)) || !boost::filesystem::is_regular_file(AI::model_io_path(path)))
        throw std::runtime_error("分区缓存块缺失。");
    const auto count = boost::filesystem::file_size(AI::model_io_path(path));
    if (!count || count > block_bytes) throw std::runtime_error("分区缓存块大小无效。");
    std::string bytes(size_t(count), '\0');
    boost::filesystem::ifstream input(AI::model_io_path(path), std::ios::binary);
    if (!input.read(bytes.data(), std::streamsize(bytes.size())) || input.peek() != std::char_traits<char>::eof() ||
        AI::beauty_leaf_digest(bytes) != hash) throw std::runtime_error("分区缓存块摘要不符。");
    return nlohmann::json::parse(bytes);
}
inline nlohmann::json mesh_descriptor(const nlohmann::json& manifest)
{
    nlohmann::json chunks = nlohmann::json::array();
    for (const auto& row : manifest.at("chunks"))
        if (row.at("kind") == "vertices" || row.at("kind") == "indices") chunks.push_back(row);
    return {{"schema", "orca.native-workbench-chunk-mesh/v2"},
        {"geometry_id", manifest.at("geometry_id")}, {"chunks", std::move(chunks)}};
}
inline nlohmann::json reference_for(const nlohmann::json& manifest, const std::string& id)
{
    return {{"schema", "orca.native-workbench-partition-cache-reference/v2"},
        {"content_id", id}, {"partition_digest", manifest.at("partition_digest")},
        {"geometry_id", manifest.at("geometry_id")}, {"face_count", manifest.at("face_count")},
        {"native_mesh_sha256", manifest.at("native_mesh_sha256")}};
}
inline nlohmann::json save(const NativeWorkbenchPartition& partition, const boost::filesystem::path& root)
{
    if (!partition.valid()) throw std::runtime_error("分块分区输入无效。");
    nlohmann::json manifest {{"schema", "orca.native-workbench-partition-chunks/v2"},
        {"algorithm", partition.algorithm}, {"geometry_id", partition.geometry_id},
        {"face_count", partition.painted.indices.size()}, {"vertex_count", partition.painted.vertices.size()},
        {"region_count", partition.region_cluster_ids.size()}, {"cluster_colors", partition.painted.cluster_colors},
        {"partition_digest", partition.partition_digest}, {"boundary_digest", partition.boundary_digest},
        {"boundary", {{"edge_count", partition.boundary.edge_count}, {"boundary_edges", partition.boundary.boundary_edges},
            {"color_boundary_edges", partition.boundary.color_boundary_edges}, {"open_edges", partition.boundary.open_edges},
            {"nonmanifold_edges", partition.boundary.nonmanifold_edges}}}, {"chunks", nlohmann::json::array()}};
    const auto write = [&](const char* kind, size_t count, const auto& row) {
        for (size_t first = 0; first < count; first += rows_per_chunk) {
            auto rows = nlohmann::json::array();
            const size_t size = std::min(rows_per_chunk, count - first);
            for (size_t i = 0; i < size; ++i) rows.push_back(row(first + i));
            const auto hash = publish(rows, root, ".chunk");
            manifest["chunks"].push_back({{"kind", kind}, {"first", first}, {"count", size}, {"sha256", hash}});
        }
    };
    write("vertices", partition.painted.vertices.size(), [&](size_t i) {
        std::array<uint32_t, 3> bits;
        for (int a = 0; a < 3; ++a) std::memcpy(&bits[a], &partition.painted.vertices[i][a], sizeof(uint32_t));
        return nlohmann::json(bits);
    });
    write("indices", partition.painted.indices.size(), [&](size_t i) { return nlohmann::json(partition.painted.indices[i]); });
    write("face_cluster_ids", partition.face_cluster_ids.size(), [&](size_t i) { return nlohmann::json(partition.face_cluster_ids[i]); });
    write("face_region_ids", partition.face_region_ids.size(), [&](size_t i) { return nlohmann::json(partition.face_region_ids[i]); });
    write("region_cluster_ids", partition.region_cluster_ids.size(), [&](size_t i) { return nlohmann::json(partition.region_cluster_ids[i]); });
    write("region_areas", partition.region_areas.size(), [&](size_t i) { return nlohmann::json(partition.region_areas[i]); });
    size_t region = 0, neighbor = 0, adjacency_count = 0;
    for (const auto& list : partition.region_adjacency) adjacency_count += list.size();
    write("region_adjacency", adjacency_count, [&](size_t) {
        while (region < partition.region_adjacency.size() && neighbor == partition.region_adjacency[region].size()) { ++region; neighbor = 0; }
        return nlohmann::json::array({region, partition.region_adjacency[region][neighbor++]});
    });
    manifest["adjacency_count"] = adjacency_count;
    manifest["native_mesh_sha256"] = AI::beauty_leaf_digest(mesh_descriptor(manifest).dump());
    const auto id = publish(manifest, root, ".json");
    return reference_for(manifest, id);
}
inline uint64_t integer(const nlohmann::json& value, uint64_t maximum)
{
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > maximum) throw std::runtime_error("分区缓存块索引或容量无效。");
    return value.get<uint64_t>();
}
inline void load(const nlohmann::json& reference, const boost::filesystem::path& root,
    NativeWorkbenchPartitionIdentity& output, TriangleMesh* exact_mesh)
{
    if (reference.at("schema") != "orca.native-workbench-partition-cache-reference/v2")
        throw std::runtime_error("未知分块分区版本。");
    const auto manifest = read(root, reference.at("content_id").get<std::string>(), ".json");
    if (manifest.at("schema") != "orca.native-workbench-partition-chunks/v2" ||
        reference_for(manifest, reference.at("content_id").get<std::string>()) != reference ||
        AI::beauty_leaf_digest(mesh_descriptor(manifest).dump()) != manifest.at("native_mesh_sha256"))
        throw std::runtime_error("分块分区清单身份不符。");
    const size_t faces = size_t(integer(manifest.at("face_count"), maximum_face_count));
    const size_t vertices = size_t(integer(manifest.at("vertex_count"), uint64_t(std::numeric_limits<int>::max())));
    const size_t regions = size_t(integer(manifest.at("region_count"), faces));
    const size_t adjacent = size_t(integer(manifest.at("adjacency_count"), uint64_t(faces) * 3));
    if (!faces || !vertices || !regions || vertices > uint64_t(faces) * 3 ||
        !manifest.at("chunks").is_array() || manifest.at("chunks").size() >
            (uint64_t(vertices) + faces * 3ULL + regions * 2ULL + adjacent) / rows_per_chunk + 8 ||
        !MeshResourceBudget::editable(vertices, faces))
        throw std::runtime_error("分块分区容量无效。");
    NativeWorkbenchPartitionIdentity decoded;
    decoded.algorithm = manifest.at("algorithm").get<uint32_t>();
    if (!supported_partition_algorithm(decoded.algorithm)) throw std::runtime_error("未知分区算法。");
    decoded.geometry_id = manifest.at("geometry_id").get<std::string>(); decoded.face_count = faces;
    decoded.partition_digest = manifest.at("partition_digest").get<std::string>();
    decoded.boundary_digest = manifest.at("boundary_digest").get<std::string>();
    decoded.cluster_colors = manifest.at("cluster_colors").get<decltype(decoded.cluster_colors)>();
    const auto& boundary = manifest.at("boundary");
    decoded.boundary = {size_t(integer(boundary.at("edge_count"), faces * 3ULL)),
        size_t(integer(boundary.at("boundary_edges"), faces * 3ULL)),
        size_t(integer(boundary.at("color_boundary_edges"), faces * 3ULL)),
        size_t(integer(boundary.at("open_edges"), faces * 3ULL)),
        size_t(integer(boundary.at("nonmanifold_edges"), faces * 3ULL))};
    decoded.face_cluster_ids.resize(faces); decoded.face_region_ids.resize(faces);
    decoded.region_cluster_ids.resize(regions); decoded.region_areas.resize(regions); decoded.region_adjacency.resize(regions);
    indexed_triangle_set mesh;
    if (exact_mesh) { mesh.vertices.resize(vertices); mesh.indices.resize(faces); }
    const std::array<std::string, 7> kinds {"vertices", "indices", "face_cluster_ids", "face_region_ids",
        "region_cluster_ids", "region_areas", "region_adjacency"};
    const std::array<size_t, 7> totals {vertices, faces, faces, faces, regions, regions, adjacent};
    size_t section = 0, next = 0;
    for (const auto& chunk : manifest.at("chunks")) {
        while (section < totals.size() && next == totals[section]) { ++section; next = 0; }
        if (section == totals.size() || chunk.at("kind") != kinds[section] ||
            integer(chunk.at("first"), totals[section]) != next)
            throw std::runtime_error("分区缓存块顺序或范围不符。");
        const size_t count = size_t(integer(chunk.at("count"), std::min(rows_per_chunk, totals[section] - next)));
        const auto rows = read(root, chunk.at("sha256").get<std::string>(), ".chunk");
        if (!count || !rows.is_array() || rows.size() != count) throw std::runtime_error("分区缓存块行数不符。");
        for (size_t i = 0; i < count; ++i) {
            const size_t index = next + i; const auto& row = rows[i];
            if (section <= 1) {
                if (!row.is_array() || row.size() != 3) throw std::runtime_error("分区几何块行无效。");
                if (section == 0) {
                    Vec3f vertex;
                    for (int a = 0; a < 3; ++a) { const auto bits = uint32_t(integer(row[a], UINT32_MAX)); std::memcpy(&vertex[a], &bits, sizeof(bits)); }
                    if (!vertex.allFinite()) throw std::runtime_error("分区顶点非有限。");
                    if (exact_mesh) mesh.vertices[index] = vertex;
                } else {
                    Vec3i32 face;
                    for (int c = 0; c < 3; ++c) face[c] = int(integer(row[c], vertices - 1));
                    if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) throw std::runtime_error("分区面退化。");
                    if (exact_mesh) mesh.indices[index] = face;
                }
            } else if (section == 2) decoded.face_cluster_ids[index] = size_t(integer(row, 31));
            else if (section == 3) decoded.face_region_ids[index] = uint32_t(integer(row, regions - 1));
            else if (section == 4) decoded.region_cluster_ids[index] = size_t(integer(row, 31));
            else if (section == 5) { decoded.region_areas[index] = row.get<double>(); }
            else {
                if (!row.is_array() || row.size() != 2) throw std::runtime_error("分区邻接块无效。");
                decoded.region_adjacency[size_t(integer(row[0], regions - 1))].push_back(uint32_t(integer(row[1], regions - 1)));
            }
        }
        next += count;
    }
    while (section < totals.size() && next == totals[section]) { ++section; next = 0; }
    if (section != totals.size() || !decoded.valid()) throw std::runtime_error("分块分区不完整或标签无效。");
    if (exact_mesh && AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh) != decoded.geometry_id)
        throw std::runtime_error("分块分区几何身份不符。");
    // Only complete, immutable reads become visible to the caller.
    if (exact_mesh) *exact_mesh = TriangleMesh(std::move(mesh));
    output = std::move(decoded);
}
inline std::vector<boost::filesystem::path> members(const nlohmann::json& reference, const boost::filesystem::path& root)
{
    const auto id = reference.at("content_id").get<std::string>();
    if (!hash_valid(id)) throw std::runtime_error("分区缓存归档身份无效。");
    std::vector<boost::filesystem::path> result {root / (id + ".json")};
    if (reference.at("schema") == "orca.native-workbench-partition-cache-reference/v2") {
        const auto manifest = read(root, id, ".json");
        for (const auto& chunk : manifest.at("chunks")) {
            const auto hash = chunk.at("sha256").get<std::string>();
            if (!hash_valid(hash)) throw std::runtime_error("分区缓存归档块身份无效。");
            result.push_back(root / (hash + ".chunk"));
        }
    }
    return result;
}
}
