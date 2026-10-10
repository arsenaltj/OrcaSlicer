#pragma once

#include "../AI/ModelGeneration/PostGenerationUiState.hpp"
#include <string>

namespace Slic3r::GUI {

enum class GalleryNavigation { Embedded, Browser, Blocked };

// Scheme and host come from wxURI, normalized to lower case by the view.
inline GalleryNavigation gallery_navigation(const std::string& scheme, const std::string& host, bool installed_page = false)
{
    if (installed_page && scheme == "file" && (host.empty() || host == "localhost")) return GalleryNavigation::Embedded;
    if (scheme != "https" || host.empty()) return GalleryNavigation::Blocked;
    return GalleryNavigation::Browser;
}

inline bool print_project_save_available(bool has_model, bool importing, bool slicing,
    bool dirty_beauty, PostGenerationUiState::Status status)
{
    using Status = PostGenerationUiState::Status;
    return has_model && !importing && !slicing && !dirty_beauty && status != Status::Editing &&
        status != Status::Processing && status != Status::CandidateReady && status != Status::ComparingBefore;
}

} // namespace Slic3r::GUI
