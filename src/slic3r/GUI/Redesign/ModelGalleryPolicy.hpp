#pragma once

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

} // namespace Slic3r::GUI
