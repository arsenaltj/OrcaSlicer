#pragma once

#include "ModelLibraryEntry.hpp"
#include <atomic>

namespace Slic3r::GUI {

// No app configuration, window or provider access. The caller selects the root.
class ModelLibraryRepository final {
public:
    static std::vector<ModelLibraryEntry> read(const boost::filesystem::path& root,
                                              const std::atomic<bool>& cancelled);
};

} // namespace Slic3r::GUI
