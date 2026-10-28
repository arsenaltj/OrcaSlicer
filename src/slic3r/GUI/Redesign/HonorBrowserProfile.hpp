#pragma once

#include "../../Utils/HonorAccount/HonorOAuth.hpp"
#include <atomic>
#include <memory>
#include <optional>
#include <wx/webview.h>

namespace Slic3r::GUI::HonorBrowserProfile {

// The browser belongs to this application/configuration, never the legacy
// printer-account browser or the user's regular Edge profile.
wxString path(const wxString& data, const HonorAccount::Config& config);
std::optional<wxWebViewConfiguration> configuration(const wxString& data, const HonorAccount::Config& config);
bool prepare(wxWebView* browser);

struct Reset {
    std::atomic_bool done {false};
    bool success {false}; // Published before done; no window retained by callbacks.
};
std::shared_ptr<Reset> reset(wxWebView* browser);
// Keep the controller alive (it may be hidden) until done. WebView2 may abandon
// completion when its controller closes; application shutdown is still safe.

} // namespace Slic3r::GUI::HonorBrowserProfile
