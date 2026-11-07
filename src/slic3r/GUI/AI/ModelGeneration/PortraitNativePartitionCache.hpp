#pragma once

#include "PortraitNativePartition.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafDomain.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/convert.hpp>
#include <functional>
#include <cstring>
#include <limits>

namespace Slic3r::GUI::NativePortrait {

inline constexpr size_t maximum_partition_cache_bytes = 256ULL * 1024 * 1024;

enum class PartitionCommitCheckpoint : uint8_t {
    BeforeAtomicPublish,
    AfterAtomicPublish,
    AfterReadbackValidation
};

using PartitionCommitObserver = std::function<void(PartitionCommitCheckpoint)>;

inline void write_validate_remove_native_portrait_partition_staging(
    const boost::filesystem::path& staging_path,
    const nlohmann::json& metadata)
{
    try {
        {
            boost::filesystem::ofstream output(staging_path, std::ios::binary);
            output << metadata.dump();
            output.close();
            if (!output)
                throw std::runtime_error("原生分区提交元数据暂存失败。");
        }
        {
            boost::filesystem::ifstream input(staging_path, std::ios::binary);
            if (!input || nlohmann::json::parse(input) != metadata)
                throw std::runtime_error("原生分区提交元数据暂存校验失败。");
        }
        boost::system::error_code filesystem_error;
        const bool removed = boost::filesystem::remove(staging_path, filesystem_error);
        if (filesystem_error || !removed)
            throw std::runtime_error("原生分区提交元数据暂存清理失败。" +
                (filesystem_error ? " " + filesystem_error.message() : std::string {}));
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(staging_path, ignored);
        throw;
    }
}

inline nlohmann::json publish_native_portrait_partition_commit(
    const boost::filesystem::path& metadata_path,
    const boost::filesystem::path& model,
    const boost::filesystem::path& source,
    const nlohmann::json& metadata,
    const PartitionCommitObserver& observer = {})
{
    if (!metadata.is_object() ||
        metadata.value("schema", std::string {}) != "orca.native-workbench-partition-commit/v1" ||
        metadata.value("transaction_status", std::string {}) != "committed")
        throw std::runtime_error("原生人像分区提交元数据无效。");

    const auto observe = [&](PartitionCommitCheckpoint checkpoint) {
        if (observer) observer(checkpoint);
    };
    bool published = false;
    try {
        observe(PartitionCommitCheckpoint::BeforeAtomicPublish);
        AI::publish_beauty_version_record(metadata_path, model, source, metadata);
        published = true;
        observe(PartitionCommitCheckpoint::AfterAtomicPublish);

        boost::system::error_code filesystem_error;
        const auto bytes = boost::filesystem::file_size(metadata_path, filesystem_error);
        if (filesystem_error || bytes == 0 || bytes > 128ULL * 1024 * 1024 ||
            boost::filesystem::is_symlink(metadata_path, filesystem_error) || filesystem_error)
            throw std::runtime_error("原生人像分区提交元数据缺失或不可读。");
        boost::filesystem::ifstream input(metadata_path, std::ios::binary);
        const auto read_back = nlohmann::json::parse(input);
        if (read_back != metadata || read_back.at("transaction_status") != "committed")
            throw std::runtime_error("原生人像分区提交元数据读回校验失败。");
        observe(PartitionCommitCheckpoint::AfterReadbackValidation);
        return read_back;
    } catch (...) {
        if (published) {
            boost::system::error_code ignored;
            boost::filesystem::remove(metadata_path, ignored);
        }
        throw;
    }
}

inline nlohmann::json replace_native_portrait_partition_commit(
    const boost::filesystem::path& metadata_path,
    const boost::filesystem::path& model,
    const boost::filesystem::path& source,
    const nlohmann::json& metadata,
    const PartitionCommitObserver& observer = {})
{
    if (!metadata.is_object() ||
        metadata.value("schema", std::string {}) != "orca.native-workbench-partition-commit/v1" ||
        metadata.value("transaction_status", std::string {}) != "committed")
        throw std::runtime_error("原生人像分区提交元数据无效。");
    boost::system::error_code filesystem_error;
    const auto bytes = boost::filesystem::file_size(metadata_path, filesystem_error);
    if (filesystem_error || bytes == 0 || bytes > 128ULL * 1024 * 1024 ||
        boost::filesystem::is_symlink(metadata_path, filesystem_error) || filesystem_error ||
        !boost::filesystem::is_regular_file(metadata_path, filesystem_error) || filesystem_error)
        throw std::runtime_error("原生人像分区原提交缺失或不可替换。");
    if (boost::filesystem::is_symlink(model) || boost::filesystem::is_symlink(source) ||
        AI::model_artifact_sha256(model) != metadata.value("model_sha256", std::string {}) ||
        AI::model_artifact_sha256(source) != metadata.value("source_sha256", std::string {}))
        throw std::runtime_error("原生人像分区模型或来源身份已变化。");

    nlohmann::json previous;
    {
        boost::filesystem::ifstream input(metadata_path, std::ios::binary);
        previous = nlohmann::json::parse(input);
    }
    if (!previous.is_object() ||
        previous.value("schema", std::string {}) != "orca.native-workbench-partition-commit/v1" ||
        previous.value("transaction_status", std::string {}) != "committed")
        throw std::runtime_error("原生人像分区原提交无效，未覆盖。");

    const auto staging_path = metadata_path.parent_path() /
        boost::filesystem::unique_path("native-partition-rebind-%%%%-%%%%.tmp");
    const auto rollback_path = metadata_path.parent_path() /
        boost::filesystem::unique_path("native-partition-rebind-%%%%-%%%%.rollback");
    bool replaced = false;
    bool rollback_created = false;
    try {
        {
            boost::filesystem::ofstream output(staging_path, std::ios::binary);
            output << metadata.dump();
            output.close();
            if (!output)
                throw std::runtime_error("原生分区人物身份暂存失败。");
        }
        {
            boost::filesystem::ifstream input(staging_path, std::ios::binary);
            if (!input || nlohmann::json::parse(input) != metadata)
                throw std::runtime_error("原生分区人物身份暂存校验失败。");
        }
        boost::filesystem::copy_file(metadata_path, rollback_path);
        rollback_created = true;
        {
            boost::filesystem::ifstream input(rollback_path, std::ios::binary);
            if (!input || nlohmann::json::parse(input) != previous)
                throw std::runtime_error("原生分区原提交回滚副本校验失败。");
        }
        if (observer) observer(PartitionCommitCheckpoint::BeforeAtomicPublish);
        if (const auto error = Slic3r::rename_file(
                boost::nowide::narrow(staging_path.wstring()),
                boost::nowide::narrow(metadata_path.wstring())))
            throw std::runtime_error("原生分区人物身份原子发布失败：" + error.message());
        replaced = true;
        if (observer) observer(PartitionCommitCheckpoint::AfterAtomicPublish);

        boost::filesystem::ifstream input(metadata_path, std::ios::binary);
        const auto read_back = nlohmann::json::parse(input);
        if (read_back != metadata || read_back.at("transaction_status") != "committed")
            throw std::runtime_error("原生分区人物身份读回校验失败。");
        if (observer) observer(PartitionCommitCheckpoint::AfterReadbackValidation);
        boost::filesystem::remove(rollback_path, filesystem_error);
        return read_back;
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(staging_path, ignored);
        if (replaced && rollback_created) {
            if (const auto rollback_error = Slic3r::rename_file(
                    boost::nowide::narrow(rollback_path.wstring()),
                    boost::nowide::narrow(metadata_path.wstring())))
                throw std::runtime_error("原生分区人物身份发布失败，原提交保留在 " +
                    rollback_path.string() + "；自动回滚失败：" + rollback_error.message());
        } else if (rollback_created) {
            boost::filesystem::remove(rollback_path, ignored);
        }
        throw;
    }
}

// Color carriers may split aliases; only exact ordered corner bits may restore
// the original closed native indices. No tolerance or coordinate welding.
inline bool same_native_triangle_corners(const indexed_triangle_set& left,
    const indexed_triangle_set& right, const std::function<bool()>& canceled = {})
{
    if (left.indices.empty() || left.indices.size() != right.indices.size()) return false;
    for (size_t face = 0; face < left.indices.size(); ++face) {
        if ((face & 4095) == 0 && canceled && canceled()) return false;
        for (int corner = 0; corner < 3; ++corner) {
            const int a = left.indices[face][corner], b = right.indices[face][corner];
            if (a < 0 || b < 0 || size_t(a) >= left.vertices.size() ||
                size_t(b) >= right.vertices.size() ||
                !left.vertices[a].allFinite() || !right.vertices[b].allFinite() ||
                std::memcmp(left.vertices[a].data(), right.vertices[b].data(),
                    3 * sizeof(float)) != 0) return false;
        }
    }
    return !(canceled && canceled());
}

inline nlohmann::json native_topology_mesh_document(const NativePortraitPartition& partition)
{
    static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559,
        "Native topology cache requires IEEE 754 binary32 coordinates.");
    nlohmann::json vertices = nlohmann::json::array(), indices = nlohmann::json::array();
    for (const auto& vertex : partition.painted.vertices) {
        for (float channel : vertex)
            if (!std::isfinite(channel)) throw std::runtime_error("Non-finite native topology coordinate.");
        std::array<uint32_t, 3> bits;
        for (int axis = 0; axis < 3; ++axis)
            std::memcpy(&bits[axis], &vertex[axis], sizeof(uint32_t));
        vertices.push_back(bits);
    }
    for (const auto& face : partition.painted.indices)
        indices.push_back({face[0], face[1], face[2]});
    return {{"schema", "orca.native-workbench-topology-mesh/v1"},
        {"coordinate_encoding", "ieee754-binary32-bits"},
        {"geometry_id", partition.geometry_id}, {"vertices", std::move(vertices)},
        {"indices", std::move(indices)}};
}

inline bool decode_native_topology_mesh(const nlohmann::json& document,
    const std::string& geometry_id, size_t face_count, TriangleMesh& mesh, std::string& error)
{
    try {
        if (document.at("schema") != "orca.native-workbench-topology-mesh/v1" ||
            document.at("coordinate_encoding") != "ieee754-binary32-bits" ||
            face_count == 0 || face_count > maximum_face_count ||
            document.at("geometry_id") != geometry_id || !document.at("vertices").is_array() ||
            document.at("vertices").empty() || document.at("vertices").size() > face_count * 3 ||
            !document.at("indices").is_array() || document.at("indices").size() != face_count)
            throw std::runtime_error("Native closed topology payload identity mismatch.");
        indexed_triangle_set decoded;
        decoded.vertices.reserve(document.at("vertices").size());
        decoded.indices.reserve(face_count);
        for (const auto& row : document.at("vertices")) {
            if (!row.is_array() || row.size() != 3)
                throw std::runtime_error("Native closed topology contains an invalid vertex.");
            Vec3f vertex;
            for (int axis = 0; axis < 3; ++axis) {
                if (!row[axis].is_number_integer() ||
                    (!row[axis].is_number_unsigned() && row[axis].get<int64_t>() < 0) ||
                    row[axis].get<uint64_t>() > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Invalid native coordinate bits.");
                const auto bits = row[axis].get<uint32_t>();
                std::memcpy(&vertex[axis], &bits, sizeof(uint32_t));
                if (!std::isfinite(vertex[axis]))
                    throw std::runtime_error("Non-finite native coordinate.");
            }
            decoded.vertices.push_back(vertex);
        }
        for (const auto& row : document.at("indices")) {
            if (!row.is_array() || row.size() != 3)
                throw std::runtime_error("Native closed topology contains an invalid face.");
            stl_triangle_vertex_indices face;
            for (int corner = 0; corner < 3; ++corner) {
                if (!row[corner].is_number_integer() ||
                    (row[corner].is_number_unsigned() &&
                        row[corner].get<uint64_t>() > uint64_t(std::numeric_limits<int>::max())))
                    throw std::runtime_error("Native closed topology index is not integral.");
                const auto index = row[corner].get<int64_t>();
                if (index < 0 || uint64_t(index) >= decoded.vertices.size())
                    throw std::runtime_error("Native closed topology index is out of range.");
                face[corner] = int(index);
            }
            if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0])
                throw std::runtime_error("Native closed topology contains a collapsed face.");
            decoded.indices.push_back(face);
        }
        if (AI::SurfaceSelectionPersistence::geometry_fingerprint(decoded) != geometry_id ||
            its_num_open_edges(decoded) != 0)
            throw std::runtime_error("Native topology payload is not the exact closed surface.");
        mesh = TriangleMesh(std::move(decoded));
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

inline nlohmann::json save_native_portrait_partition_cache(
    const NativePortraitPartition& partition,
    const boost::filesystem::path& cache)
{
    const auto identity = native_portrait_partition_identity(partition);
    auto document = encode_native_portrait_partition_identity(identity);
    document["native_topology_mesh"] = native_topology_mesh_document(partition);
    const auto mesh_sha256 = AI::beauty_leaf_digest(document.at("native_topology_mesh").dump());
    document["native_mesh_sha256"] = mesh_sha256;
    const auto bytes = document.dump();
    if (bytes.empty() || bytes.size() > maximum_partition_cache_bytes)
        throw std::runtime_error("原生人像分区缓存过大，未发布分区身份。");
    boost::filesystem::create_directories(cache);
    const auto temporary = cache / boost::filesystem::unique_path("native-partition-%%%%-%%%%.tmp");
    std::string content_id;
    try {
        boost::filesystem::ofstream output(temporary, std::ios::binary);
        output.write(bytes.data(), std::streamsize(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("原生人像分区缓存写入失败。");
        content_id = AI::model_artifact_sha256(temporary);
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(content_id))
            throw std::runtime_error("原生人像分区缓存校验失败。");
        const auto destination = cache / (content_id + ".json");
        if (boost::filesystem::exists(destination)) {
            if (boost::filesystem::is_symlink(destination) ||
                AI::model_artifact_sha256(destination) != content_id)
                throw std::runtime_error("原生人像分区缓存已损坏。");
            boost::filesystem::remove(temporary);
        } else {
            boost::filesystem::rename(temporary, destination);
        }
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        throw;
    }
    return {{"schema", "orca.native-workbench-partition-cache-reference/v1"},
        {"content_id", content_id}, {"partition_digest", identity.partition_digest},
        {"geometry_id", identity.geometry_id}, {"face_count", identity.face_count},
        {"native_mesh_sha256", mesh_sha256}};
}

inline bool load_native_portrait_partition_cache(
    const nlohmann::json& reference,
    const boost::filesystem::path& cache,
    NativePortraitPartitionIdentity& identity,
    std::string& error, TriangleMesh* exact_mesh = nullptr)
{
    error.clear();
    try {
        if (!reference.is_object() ||
            reference.at("schema") != "orca.native-workbench-partition-cache-reference/v1")
            throw std::runtime_error("原生工作台分区缓存引用版本不受支持或已过期。");
        const auto content_id = reference.at("content_id").get<std::string>();
        const auto partition_digest = reference.at("partition_digest").get<std::string>();
        const auto geometry_id = reference.at("geometry_id").get<std::string>();
        const auto face_count = reference.at("face_count").get<size_t>();
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(content_id) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(partition_digest) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(geometry_id) ||
            face_count == 0 || face_count > maximum_face_count)
            throw std::runtime_error("原生人像分区缓存引用无效。");
        const auto file = cache / (content_id + ".json");
        boost::system::error_code filesystem_error;
        const auto bytes = boost::filesystem::file_size(file, filesystem_error);
        if (filesystem_error || bytes == 0 || bytes > maximum_partition_cache_bytes ||
            boost::filesystem::is_symlink(file, filesystem_error) || filesystem_error ||
            !boost::filesystem::is_regular_file(file, filesystem_error) || filesystem_error)
            throw std::runtime_error("原生人像分区缓存缺失或校验失败。");
        boost::filesystem::ifstream input(file, std::ios::binary);
        std::string contents(size_t(bytes), '\0');
        if (!input.read(contents.data(), std::streamsize(contents.size())) ||
            input.peek() != std::char_traits<char>::eof() ||
            AI::beauty_leaf_digest(contents) != content_id)
            throw std::runtime_error("原生人像分区缓存读取或校验失败。");
        const auto document = nlohmann::json::parse(contents);
        NativePortraitPartitionIdentity decoded;
        if (!decode_native_portrait_partition_identity(document, decoded, error))
            throw std::runtime_error(error);
        if (decoded.partition_digest != partition_digest || decoded.geometry_id != geometry_id ||
            decoded.face_count != face_count)
            throw std::runtime_error("原生人像分区缓存身份不匹配。");
        TriangleMesh native_mesh;
        if (exact_mesh) {
            const auto mesh_sha256 = reference.at("native_mesh_sha256").get<std::string>();
            if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(mesh_sha256) ||
                document.at("native_mesh_sha256") != mesh_sha256 ||
                AI::beauty_leaf_digest(document.at("native_topology_mesh").dump()) != mesh_sha256 ||
                !decode_native_topology_mesh(document.at("native_topology_mesh"),
                    geometry_id, face_count, native_mesh, error))
                throw std::runtime_error(error.empty()
                    ? "Native closed topology cache hash mismatch." : error);
        }
        if (AI::model_artifact_sha256(file) != content_id)
            throw std::runtime_error("Native topology cache changed while reading.");
        identity = std::move(decoded);
        if (exact_mesh) *exact_mesh = std::move(native_mesh);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

inline void archive_native_portrait_partitions(
    const std::vector<nlohmann::json>& references,
    const boost::filesystem::path& cache,
    const boost::filesystem::path& auxiliaries)
{
    for (const auto& reference : references) {
        NativePortraitPartitionIdentity identity;
        std::string error;
        if (!load_native_portrait_partition_cache(reference, cache, identity, error))
            throw std::runtime_error(error);
        // Copy the content-addressed bytes verbatim. Re-serializing JSON can
        // change its digest even when the decoded identity is equivalent.
        const auto directory = auxiliaries / "OrcaNativeWorkbenchPartitions";
        boost::filesystem::create_directories(directory);
        const auto content_id = reference.at("content_id").get<std::string>();
        const auto source = cache / (content_id + ".json");
        const auto destination = directory / (content_id + ".json");
        if (!boost::filesystem::exists(destination)) {
            boost::filesystem::copy_file(source, destination);
            if (AI::model_artifact_sha256(destination) != content_id)
                throw std::runtime_error("原生人像分区归档写入失败。");
        } else if (AI::model_artifact_sha256(destination) != content_id) {
            throw std::runtime_error("原生人像分区归档已损坏。");
        }
    }
}

inline size_t restore_native_portrait_partition_archive(
    const boost::filesystem::path& auxiliaries,
    const boost::filesystem::path& cache)
{
    const auto directory = auxiliaries / "OrcaNativeWorkbenchPartitions";
    if (!boost::filesystem::is_directory(directory) || boost::filesystem::is_symlink(directory)) return 0;
    boost::filesystem::create_directories(cache);
    size_t restored = 0;
    size_t visited = 0;
    for (boost::filesystem::directory_iterator iterator(directory), end;
         iterator != end && visited++ < 256; ++iterator) {
        try {
            const auto file = iterator->path();
            const auto content_id = file.stem().string();
            if (file.extension() != ".json" ||
                !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(content_id) ||
                boost::filesystem::is_symlink(file) ||
                boost::filesystem::file_size(file) > maximum_partition_cache_bytes ||
                AI::model_artifact_sha256(file) != content_id) continue;
            boost::filesystem::ifstream input(file, std::ios::binary);
            NativePortraitPartitionIdentity identity;
            std::string error;
            if (!decode_native_portrait_partition_identity(nlohmann::json::parse(input), identity, error)) continue;
            const auto destination = cache / (content_id + ".json");
            if (!boost::filesystem::exists(destination)) boost::filesystem::copy_file(file, destination);
            if (AI::model_artifact_sha256(destination) == content_id) ++restored;
        } catch (const std::exception&) {
            // Optional damaged archive entries never block ordinary project loading.
        }
    }
    return restored;
}

} // namespace Slic3r::GUI::NativePortrait
