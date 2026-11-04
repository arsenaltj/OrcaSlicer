#pragma once

#include "PortraitPrintFlow.hpp"
#include <string>

namespace Slic3r::GUI::PortraitPrint {

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

} // namespace Slic3r::GUI::PortraitPrint
