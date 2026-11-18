#pragma once
#include "ModelLibraryEntry.hpp"
#include <functional>

namespace Slic3r::GUI {

// Filesystem transaction only. Call on the existing background executor with
// an immutable selection; no window, Session or GUI callback is retained.
class ModelLibraryDeletion {
public:
    struct Result {
        bool committed {false}, canceled {false}, retained_dependencies {false};
        bool identity_changed {false};
        size_t removed_files {0};
        std::string error;
    };
    static Result remove(const boost::filesystem::path& root, const ModelLibraryEntry& entry,
        const std::vector<ModelLibraryEntry>& entries, const std::function<bool()>& canceled = {},
        const std::function<void()>& committed = {});
    // A removal receipt is independent of the exact certified metadata. A
    // referenced asset keeps its bytes and provenance, but stays off the list
    // after refresh/restart. This never grants permission to open a model.
    static bool removed(const boost::filesystem::path& root, const std::string& job_id,
        const boost::filesystem::path& asset, bool design_only = false);
    static bool appearance_companion(const boost::filesystem::path& path);
};
}
