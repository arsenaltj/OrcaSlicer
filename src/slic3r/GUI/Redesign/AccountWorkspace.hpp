#pragma once
#include "../../Utils/HonorAccount/HonorSession.hpp"
#include <functional>
#include <chrono>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

class wxWebView;
class wxStaticText;
class wxSizerItem;
class wxCheckBox;
namespace Slic3r::GUI {
namespace HonorBrowserProfile { struct Reset; }
class RoundedActionButton;
class AccountWorkspace final : public wxPanel {
public:
    using Changed = std::function<void(const HonorAccount::Snapshot&, const wxBitmap&)>;
    AccountWorkspace(wxWindow*, Changed, std::function<void()> enter_workspace);
    ~AccountWorkspace() override;
    void start();
private:
    void render();
    void login();
    void logout();
    void browser_failed(const char* message);
    void close_browser();
    bool open_browser(const std::string& url, bool clear_only = false);
    void prepare_browser();
    void navigate_authorization();
    void poll();
    void load_avatar();
    void edit_avatar();
    HonorAccount::Session m_session;
    Changed m_changed;
    std::function<void()> m_enter_workspace;
    wxTimer m_timer;
    wxPanel* m_summary {nullptr};
    wxPanel* m_clear_login_row {nullptr};
    wxPanel* m_browser_host {nullptr};
    wxWebView* m_browser {nullptr};
    wxStaticText* m_name {nullptr};
    wxStaticText* m_status {nullptr};
    wxStaticText* m_browser_status {nullptr};
    RoundedActionButton* m_avatar {nullptr};
    RoundedActionButton* m_login {nullptr};
    RoundedActionButton* m_logout {nullptr};
    wxCheckBox* m_clear_login {nullptr};
    RoundedActionButton* m_cancel_login {nullptr};
    wxSizerItem* m_hint_space {nullptr};
    wxImage m_default_avatar, m_guest_avatar, m_avatar_image;
    wxBitmap m_avatar_bitmap;
    wxString m_avatar_path;
    bool m_avatar_loaded {false};
    bool m_browser_initializing {false};
    bool m_clear_only {false};
    std::shared_ptr<HonorBrowserProfile::Reset> m_browser_reset;
    std::string m_logout_message;
    std::string m_pending_authorization;
    std::chrono::steady_clock::time_point m_browser_deadline;
};
} // namespace Slic3r::GUI
