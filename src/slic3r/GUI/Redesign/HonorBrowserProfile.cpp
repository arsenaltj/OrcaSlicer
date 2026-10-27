#include "HonorBrowserProfile.hpp"
#include <wx/filename.h>
#ifdef __WXMSW__
#include <WebView2.h>
#include <wrl.h>
#endif

namespace Slic3r::GUI::HonorBrowserProfile {

wxString path(const wxString& data, const HonorAccount::Config& config)
{
    if (data.empty()) return {};
    return wxFileName(data, "").GetPathWithSep() + "honor-webview/" + wxString::FromUTF8(config.key());
}

std::optional<wxWebViewConfiguration> configuration(const wxString& data, const HonorAccount::Config& config)
{
#ifdef __WXMSW__
    const auto backend = wxWebViewBackendEdge;
#else
    const auto backend = wxWebViewBackendDefault;
#endif
    const auto directory = path(data, config);
    if (directory.empty() || !wxWebView::IsBackendAvailable(backend)) return {};
    auto result = wxWebView::NewConfiguration(backend);
    result.SetDataPath(directory);
    if (!result.EnablePersistentStorage(true)) return {};
    return result;
}

bool prepare(wxWebView* browser)
{
    if (!browser || !browser->GetNativeBackend()) return false;
#ifdef __WXMSW__
    using Microsoft::WRL::ComPtr;
    auto* native = static_cast<ICoreWebView2*>(browser->GetNativeBackend());
    ComPtr<ICoreWebView2_13> view;
    ComPtr<ICoreWebView2Profile> profile;
    BOOL is_private = TRUE;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&view))) || FAILED(view->get_Profile(&profile)) ||
        FAILED(profile->get_IsInPrivateModeEnabled(&is_private)) || is_private) return false;
    // Remember the official site's device/session data, without offering to
    // save passwords or autofill personal information in this embedded view.
    ComPtr<ICoreWebView2Settings> settings;
    ComPtr<ICoreWebView2Settings4> settings4;
    return SUCCEEDED(native->get_Settings(&settings)) && SUCCEEDED(settings.As(&settings4)) &&
        SUCCEEDED(settings4->put_IsPasswordAutosaveEnabled(FALSE)) &&
        SUCCEEDED(settings4->put_IsGeneralAutofillEnabled(FALSE));
#else
    return true;
#endif
}

std::shared_ptr<Reset> reset(wxWebView* browser)
{
    auto result = std::make_shared<Reset>();
#ifdef __WXMSW__
    using Microsoft::WRL::ComPtr;
    if (browser && browser->GetNativeBackend()) {
        auto* native = static_cast<ICoreWebView2*>(browser->GetNativeBackend());
        ComPtr<ICoreWebView2_13> view;
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile2> profile2;
        if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&view))) && SUCCEEDED(view->get_Profile(&profile)) &&
            SUCCEEDED(profile.As(&profile2))) {
            // Use the native completion result. wx 3.3's implementation captures
            // its window until completion, which is unsafe if the user cancels.
            const auto hr = profile2->ClearBrowsingData(
                COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE,
                Microsoft::WRL::Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                    [result](HRESULT error) -> HRESULT {
                        result->success = SUCCEEDED(error);
                        result->done.store(true);
                        return S_OK;
                    }).Get());
            if (SUCCEEDED(hr)) return result;
        }
    }
#endif
    // Unsupported or failed reset must not navigate with a previous SSO login.
    result->done.store(true);
    return result;
}

} // namespace Slic3r::GUI::HonorBrowserProfile
