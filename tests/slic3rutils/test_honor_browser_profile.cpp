#include "slic3r/GUI/Redesign/HonorBrowserProfile.hpp"
#include <catch2/catch_test_macros.hpp>
#include <wx/filename.h>

namespace Profile = Slic3r::GUI::HonorBrowserProfile;
namespace HA = Slic3r::HonorAccount;

TEST_CASE("Honor browser data stays separate across application configurations", "[HonorBrowserProfile]")
{
    HA::Config config;
    const auto original = Profile::path("/user-data", config);
    REQUIRE_FALSE(original.empty());
    REQUIRE(original == Profile::path("/user-data", config));
    config.client_secret = "fixture-only-secret";
    REQUIRE(original == Profile::path("/user-data", config));
    REQUIRE_FALSE(original.Contains(config.client_secret));
    config.client_id += "2";
    REQUIRE(original != Profile::path("/user-data", config));
    config = HA::Config{}; config.environment = "production";
    REQUIRE(original != Profile::path("/user-data", config));
    config = HA::Config{}; config.authorize_url += "/changed";
    REQUIRE(original != Profile::path("/user-data", config));
    REQUIRE(original != Profile::path("/other-user-data", HA::Config{}));
    REQUIRE(Profile::path({}, config).empty());
    REQUIRE_FALSE(Profile::configuration({}, config).has_value());
}

TEST_CASE("Unavailable Honor browser reset fails without a window callback", "[HonorBrowserProfile]")
{
    const auto reset = Profile::reset(nullptr);
    REQUIRE(reset->done.load());
    REQUIRE_FALSE(reset->success);
    REQUIRE_FALSE(Profile::prepare(nullptr));
}

#ifdef __WXMSW__
#include <chrono>
#include <functional>
#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/file.h>
#include <wx/filesys.h>
#include <wx/frame.h>
#include <wx/utils.h>

namespace {
bool until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        wxTheApp->Yield(true);
        wxTheApp->ProcessPendingEvents();
        wxTheApp->ProcessIdle();
        wxMilliSleep(10);
    }
    return predicate();
}
struct BrowserRuntime {
    wxFrame* frame {nullptr};
    std::unique_ptr<wxGUIEventLoop> loop;
    std::unique_ptr<wxEventLoopActivator> active;
    wxString data = wxFileName::GetTempDir() + "/orca-honor-browser-" + wxString::FromUTF8(HA::random_value());
    BrowserRuntime()
    {
        REQUIRE(wxTheApp == nullptr);
        wxApp::SetInstance(new wxApp);
        int argc = 1; char name[] = "orca-honor-browser-test"; char* argv[] = {name, nullptr};
        REQUIRE(wxEntryStart(argc, argv));
        REQUIRE(wxTheApp->CallOnInit());
        loop = std::make_unique<wxGUIEventLoop>();
        active = std::make_unique<wxEventLoopActivator>(loop.get());
        frame = new wxFrame(nullptr, wxID_ANY, "Honor browser storage regression", wxDefaultPosition, wxSize(480, 240));
        frame->Show();
        REQUIRE(wxFileName::Mkdir(data));
        wxFile page(data + "/probe.html", wxFile::write);
        REQUIRE(page.IsOpened());
        REQUIRE(page.Write("<!doctype html><title>Local storage probe</title>Local fixture. No network or credentials."));
    }
    ~BrowserRuntime()
    {
        delete frame;
        active.reset(); loop.reset();
        wxTheApp->OnExit();
        wxEntryCleanup();
        // WebView2 may still release its process asynchronously. This unique
        // temporary directory contains only the local probe, never user data.
        wxFileName::Rmdir(data, wxPATH_RMDIR_RECURSIVE);
    }
    wxWebView* open(const HA::Config& config = {})
    {
        auto options = Profile::configuration(data, config); REQUIRE(options.has_value());
        auto* browser = wxWebView::New(*options); REQUIRE(browser != nullptr);
        auto loaded = std::make_shared<bool>(false);
        browser->Bind(wxEVT_WEBVIEW_LOADED, [loaded](wxWebViewEvent&) { *loaded = true; });
        REQUIRE(browser->Create(frame, wxID_ANY, wxFileSystem::FileNameToURL(data + "/probe.html")));
        REQUIRE(until([loaded] { return *loaded; }));
        REQUIRE(Profile::prepare(browser));
        return browser;
    }
    void close(wxWebView* browser)
    {
        browser->Destroy();
        wxTheApp->ProcessPendingEvents(); wxTheApp->ProcessIdle();
    }
};
wxString marker(wxWebView* browser)
{
    wxString result;
    REQUIRE(browser->RunScript("localStorage.getItem('orca_honor_profile_probe') || ''", &result));
    return result;
}
}

TEST_CASE("Honor browser storage survives view recreation and explicit reset removes it", "[HonorBrowserProfile]")
{
    BrowserRuntime runtime;
    auto* browser = runtime.open();
    REQUIRE(browser->RunScript("localStorage.setItem('orca_honor_profile_probe', 'retained-marker')"));
    REQUIRE(marker(browser).Contains("retained-marker"));
    runtime.close(browser);
    browser = runtime.open();
    REQUIRE(marker(browser).Contains("retained-marker"));

    HA::Config other; other.client_id += "2";
    auto* isolated = runtime.open(other);
    REQUIRE_FALSE(marker(isolated).Contains("retained-marker"));
    runtime.close(isolated);

    const auto cleared = Profile::reset(browser);
    REQUIRE(until([cleared] { return cleared->done.load(); }));
    REQUIRE(cleared->success);
    REQUIRE_FALSE(marker(browser).Contains("retained-marker"));
    runtime.close(browser);
    browser = runtime.open();
    REQUIRE_FALSE(marker(browser).Contains("retained-marker"));
    runtime.close(browser);
}

TEST_CASE("A cancelled Honor reset completes with its controller hidden before disposal", "[HonorBrowserProfile]")
{
    BrowserRuntime runtime;
    auto* browser = runtime.open();
    REQUIRE(browser->RunScript("localStorage.setItem('orca_honor_profile_probe', 'cancelled-marker')"));
    const auto reset = Profile::reset(browser);
    // Immediate controller destruction drops the WebView2 completion callback.
    // The account page hides the cancelled view until reset completes instead.
    browser->Stop(); browser->Hide();
    REQUIRE(until([reset] { return reset->done.load(); }));
    REQUIRE(reset->success);
    runtime.close(browser);
    browser = runtime.open();
    REQUIRE_FALSE(marker(browser).Contains("cancelled-marker"));
    runtime.close(browser);
}
#endif
