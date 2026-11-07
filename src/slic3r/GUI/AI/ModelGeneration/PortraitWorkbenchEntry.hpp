#pragma once

#include "PortraitPrintFlow.hpp"
#include <cstdint>
#include <string>

namespace Slic3r::GUI::PortraitPrint {

enum class NativeWorkbenchPartitionConsumer : std::uint8_t {
    AutomaticPalette = 1,
    PortraitFlow = 2,
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

inline bool bind_native_workbench_partition_commit_identity(
    nlohmann::json& metadata, const std::string& portrait_print_id,
    const nlohmann::json& source_check)
{
    try {
        if (!metadata.is_object() || portrait_print_id.empty() || !source_check.is_object() ||
            metadata.at("schema") != "orca.native-workbench-partition-commit/v1" ||
            metadata.at("schema_version").get<int>() != 6 ||
            metadata.at("transaction_status") != "committed")
            return false;
        const auto current = metadata.value("portrait_print_id", std::string{});
        const auto requester = metadata.value("requesting_portrait_print_id", std::string{});
        if ((!current.empty() && current != portrait_print_id) ||
            (!requester.empty() && requester != portrait_print_id))
            return false;
        metadata["portrait_print_id"] = portrait_print_id;
        metadata["requesting_portrait_print_id"] = portrait_print_id;
        metadata["source_check"] = source_check;
        metadata.erase("workbench_check_certificate");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct WorkbenchEntryIntent {
    std::uint64_t generation {0};
    std::string model_path;
    std::string source_sha256;
    std::string geometry_id;
    std::string appearance_id;

    bool valid() const
    {
        return generation != 0 && !model_path.empty() && valid_sha256(source_sha256) &&
            !geometry_id.empty() && !appearance_id.empty();
    }
    bool matches(std::uint64_t current_generation, const std::string& current_model_path,
                 const std::string& current_source_sha256,
                 const std::string& current_geometry_id,
                 const std::string& current_appearance_id) const
    {
        return valid() && generation == current_generation &&
            same_model_path(model_path, current_model_path) &&
            source_sha256 == current_source_sha256 &&
            geometry_id == current_geometry_id &&
            appearance_id == current_appearance_id;
    }
    void clear()
    {
        generation = 0;
        model_path.clear();
        source_sha256.clear();
        geometry_id.clear();
        appearance_id.clear();
    }
};

inline bool palette_allows_automatic_workbench(int source, const std::string& suite_id, bool has_color_card)
{
    if (!suite_id.empty()) return suite_id == "honor.people.six";
    // Preserve explicit project/manual palettes, including old saved snapshots.
    // Only an untouched automatic palette can opt into the people suite here.
    return source == 0 && !has_color_card;
}

inline bool workbench_entry_is_duplicate(const State& flow, const std::string& path,
                                         const std::string& hash)
{
    if (!flow.from_workbench() || flow.id.empty() || flow.stage == Stage::Idle || hash.empty()) return false;
    // A completed old flow has no Continue action. Opening its model explicitly
    // opts into the new repair/partition contract, preserving the old result.
    // Active legacy attempts still belong to Continue; v4 completion is final.
    if (flow.record_version < 4 && !flow.active()) return false;
    return (same_model_path(path, flow.original_model_path) && hash == flow.original_model_hash) ||
        (same_model_path(path, flow.model_path) && hash == flow.model_hash);
}

enum class WorkbenchEntryOwnership { Available, Current, Alias, Foreign };

inline bool retire_legacy_generation_for_workbench(State& flow)
{
    if (!flow.active() || flow.from_workbench()) return false;
    flow.paused = true;
    flow.authorized = false;
    flow.reason = "旧生成流程已保存为暂停记录；当前模型可进入工作台。";
    return true;
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

inline WorkbenchEntryOwnership classify_workbench_entry(const State& flow,
    const std::string& path, const std::string& hash, const std::string& geometry,
    const std::string& appearance, const std::string& asset_id = {})
{
    if (!flow.active()) return WorkbenchEntryOwnership::Available;
    if (flow.owns_display_context(path, hash, geometry, appearance))
        return WorkbenchEntryOwnership::Current;
    if (flow.can_adopt_source_alias(path, hash, geometry, appearance, asset_id))
        return WorkbenchEntryOwnership::Alias;
    return WorkbenchEntryOwnership::Foreign;
}

} // namespace Slic3r::GUI::PortraitPrint
