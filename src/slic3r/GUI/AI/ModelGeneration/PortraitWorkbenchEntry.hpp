#pragma once
#include "WorkbenchArtifactValidation.hpp"
#include <cstdint>
#include <string>
namespace Slic3r::GUI::WorkbenchArtifacts {
enum class NativeWorkbenchPartitionConsumer : std::uint8_t {
    AutomaticPalette = 1,
    HonorSuite = 4
};

using NativeWorkbenchPartitionConsumers = std::uint8_t;

constexpr NativeWorkbenchPartitionConsumers native_workbench_partition_consumer(
    NativeWorkbenchPartitionConsumer consumer)
{
    return static_cast<NativeWorkbenchPartitionConsumers>(consumer);
}

constexpr NativeWorkbenchPartitionConsumers native_workbench_partition_consumers(
    NativeWorkbenchPartitionConsumer first, NativeWorkbenchPartitionConsumer second)
{
    return native_workbench_partition_consumer(first) |
        native_workbench_partition_consumer(second);
}

constexpr bool native_workbench_partition_requested(
    NativeWorkbenchPartitionConsumers consumers, NativeWorkbenchPartitionConsumer consumer)
{
    return (consumers & native_workbench_partition_consumer(consumer)) != 0;
}

inline bool palette_allows_automatic_workbench(int source, const std::string& suite_id, bool has_color_card)
{
    if (!suite_id.empty()) return suite_id == "honor.people.six";
    // Preserve explicit project/manual palettes, including old saved snapshots.
    // Only an untouched automatic palette can opt into the people suite here.
    return source == 0 && !has_color_card;
}

struct HonorSuiteContinuationIdentity {
    std::string model_path;
    std::string source_sha256;
    std::string geometry_id;
    std::string appearance_id;
    std::uint64_t generation {0};
    std::string partition_digest;

    bool valid() const
    {
        return !model_path.empty() && valid_sha256(source_sha256) &&
            !geometry_id.empty() && !appearance_id.empty() &&
            valid_sha256(partition_digest);
    }

    bool matches(const std::string& current_model_path,
                 const std::string& current_source_sha256,
                 const std::string& current_geometry_id,
                 const std::string& current_appearance_id,
                 std::uint64_t current_generation,
                 const std::string& current_partition_digest,
                 const std::string& persisted_partition_digest) const
    {
        return valid() && same_model_path(model_path, current_model_path) &&
            source_sha256 == current_source_sha256 &&
            geometry_id == current_geometry_id &&
            appearance_id == current_appearance_id &&
            generation == current_generation &&
            partition_digest == current_partition_digest &&
            partition_digest == persisted_partition_digest;
    }
};


}
