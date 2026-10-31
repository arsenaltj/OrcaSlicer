#include "AccountWorkspace.hpp"
#include "RedesignWidgets.hpp"
#include "RedesignTheme.hpp"
#include "HonorAccountWebTheme.hpp"
#include "HonorBrandMark.hpp"
#include "AccountAvatar.hpp"
#include "HonorBrowserProfile.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <wx/access.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/dcclient.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/webview.h>

namespace Slic3r::GUI {
using namespace RedesignTheme;
namespace HA = HonorAccount;
namespace {
wxString text(const char* s) { return wxString::FromUTF8(s); }
wxString text(const std::string& s) { return wxString::FromUTF8(s); }
RoundedActionButton* action(wxWindow* parent,const char* label,bool primary=false,int width=160)
{
    auto* button=new RoundedActionButton(parent,text(label),primary,44);
    button->SetMinSize(parent->FromDIP(wxSize(width,44)));
    // wxPanel's native dialog navigation can consume Enter before KEY_DOWN.
    button->Bind(wxEVT_CHAR_HOOK,[button](wxKeyEvent& event) {
        if(button->IsEnabled()&&event.GetModifiers()==wxMOD_NONE&&
           (event.GetKeyCode()==WXK_RETURN||event.GetKeyCode()==WXK_NUMPAD_ENTER)) {
            wxCommandEvent command(wxEVT_BUTTON,button->GetId());
            command.SetEventObject(button);button->ProcessWindowEvent(command);
            return;
        }
        event.Skip();
    });
    button->SetCursor(wxCursor(wxCURSOR_HAND));return button;
}
#if wxUSE_ACCESSIBILITY
class CheckboxAccessible final : public wxAccessible {
public:
    explicit CheckboxAccessible(wxWindow* window) : wxAccessible(window) {}
    wxAccStatus GetName(int child, wxString* name) override
    { if (child != wxACC_SELF) return wxACC_NOT_IMPLEMENTED; *name = GetWindow()->GetName(); return wxACC_OK; }
};
class AvatarAccessible final : public wxAccessible {
public:
    explicit AvatarAccessible(wxWindow* window) : wxAccessible(window) {}
    wxAccStatus GetRole(int child, wxAccRole* role) override
    { if (child != wxACC_SELF) return wxACC_NOT_IMPLEMENTED; *role = wxROLE_SYSTEM_PUSHBUTTON; return wxACC_OK; }
    wxAccStatus GetName(int child, wxString* name) override
    { if (child != wxACC_SELF) return wxACC_NOT_IMPLEMENTED; *name = GetWindow()->GetName(); return wxACC_OK; }
    wxAccStatus GetDefaultAction(int child, wxString* name) override
    { if (child != wxACC_SELF) return wxACC_NOT_IMPLEMENTED; *name = text("更换头像"); return wxACC_OK; }
    wxAccStatus DoDefaultAction(int child) override
    {
        if (child != wxACC_SELF || !GetWindow()->IsEnabled()) return wxACC_NOT_SUPPORTED;
        auto* window = GetWindow();
        wxCommandEvent event(wxEVT_BUTTON, window->GetId());
        event.SetEventObject(window);wxPostEvent(window,event);
        return wxACC_OK;
    }
};
#endif
}
AccountWorkspace::AccountWorkspace(wxWindow* parent,Changed changed,std::function<void()> enter)
    :wxPanel(parent,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxTAB_TRAVERSAL),
     m_session(HA::system_store(Slic3r::data_dir()),HA::https_request,HA::application_configuration_store()),m_changed(std::move(changed)),m_enter_workspace(std::move(enter)),m_timer(this)
{
    SetName(text("我的账号"));SetBackgroundColour(background_colour());
    auto* outer=new wxBoxSizer(wxVERTICAL);SetSizer(outer);
    auto* header=new wxBoxSizer(wxHORIZONTAL);
    header->SetMinSize(0,FromDIP(44));
    auto* title=new wxStaticText(this,wxID_ANY,text("我的账号"));style_text(title,primary_text_colour(),24,true);
    header->Add(title,0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(20));
    m_browser_status=new wxStaticText(this,wxID_ANY,text("请在荣耀官方页面完成登录或注册"),wxDefaultPosition,wxDefaultSize,wxST_ELLIPSIZE_END);
    style_text(m_browser_status,primary_text_colour(),11);m_browser_status->SetMinSize(wxSize(0,-1));
    header->Add(m_browser_status,1,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(16));
    m_cancel_login=action(this,"取消登录",false,110);header->Add(m_cancel_login,0,wxALIGN_CENTER_VERTICAL);
    m_browser_status->Hide();m_cancel_login->Hide();
    outer->Add(header,0,wxEXPAND|wxALL,FromDIP(28));

    m_summary=new RoundedPanel(this,wxDefaultSize,panel_colour(),background_colour(),16);
    m_summary->SetBackgroundColour(panel_colour());
    auto* card=new wxBoxSizer(wxVERTICAL);m_summary->SetSizer(card);
    card->AddStretchSpacer(2);
    auto* brand=new wxStaticText(m_summary,wxID_ANY,"HONOR");style_text(brand,primary_text_colour(),28,true);
    card->Add(brand,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(24));
    m_default_avatar=AccountAvatar::read(text(Slic3r::resources_dir()+"/images/honor_account_avatar.png"));
    m_guest_avatar=AccountAvatar::read(text(Slic3r::resources_dir()+"/images/account_guest_avatar.png"));
    // Match the accepted summary avatar's on-screen diameter. The former
    // wxStaticBitmap implicitly doubled its untagged bitmap on this Windows UI.
    m_avatar=action(m_summary,"",false,144);
    m_avatar->SetMinSize(FromDIP(wxSize(144,144)));m_avatar->SetMaxSize(FromDIP(wxSize(144,144)));
    m_avatar->set_secondary_face(panel_colour());
    m_avatar->SetBackgroundColour(panel_colour());
#if wxUSE_ACCESSIBILITY
    m_avatar->SetAccessible(new AvatarAccessible(m_avatar));
#endif
    m_avatar->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){edit_avatar();});
    card->Add(m_avatar,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(20));
    m_name=new wxStaticText(m_summary,wxID_ANY,text("未登录"));style_text(m_name,primary_text_colour(),20,true);
    card->Add(m_name,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(12));
    auto* description=new wxStaticText(m_summary,wxID_ANY,text("使用荣耀账号登录，开启你的创作"));style_text(description,secondary_text_colour(),12);
    card->Add(description,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(20));
    m_status=new wxStaticText(m_summary,wxID_ANY,wxEmptyString,wxDefaultPosition,wxDefaultSize,wxALIGN_CENTER);
    style_text(m_status,secondary_text_colour(),10);m_status->SetMinSize(FromDIP(wxSize(420,42)));
    card->Add(m_status,0,wxALIGN_CENTER|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(24));
    const auto clear_caption=text("清除登录状态和验证身份记录，您下次登录时可能需要重新验证身份");
    m_clear_login_row=new wxPanel(m_summary,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxTAB_TRAVERSAL);
    m_clear_login_row->SetBackgroundColour(panel_colour());
    auto* clear_row=new wxBoxSizer(wxHORIZONTAL);m_clear_login_row->SetSizer(clear_row);
    m_clear_login=new wxCheckBox(m_clear_login_row,wxID_ANY,wxEmptyString);
    style_text(m_clear_login,secondary_text_colour(),10);m_clear_login->SetBackgroundColour(panel_colour());
    m_clear_login->SetName(clear_caption);m_clear_login->SetValue(false);
#if wxUSE_ACCESSIBILITY
    m_clear_login->SetAccessible(new CheckboxAccessible(m_clear_login));
#endif
    // Native static text wraps CJK at the available width, without forcing a
    // sentence break. Keep the checkbox aligned with the first text line.
    auto* clear_label=new wxStaticText(m_clear_login_row,wxID_ANY,clear_caption,
        wxDefaultPosition,wxDefaultSize,wxST_WRAP|wxST_NO_AUTORESIZE);
    style_text(clear_label,secondary_text_colour(),10);
    const wxSize clear_text_size(FromDIP(230),2*clear_label->GetCharHeight());
    clear_label->SetMinSize(clear_text_size);clear_label->SetMaxSize(clear_text_size);
    clear_label->Bind(wxEVT_LEFT_UP,[this](wxMouseEvent&) {
        m_clear_login->SetValue(!m_clear_login->GetValue());m_clear_login->SetFocus();
    });
    clear_row->Add(m_clear_login,0,wxALIGN_TOP|wxRIGHT,FromDIP(6));
    clear_row->Add(clear_label);
    card->Add(m_clear_login_row,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(14));
    auto* buttons=new wxBoxSizer(wxHORIZONTAL);
    m_login=action(m_summary,"荣耀账号登录 / 注册",true,230);buttons->Add(m_login);
    m_logout=action(m_summary,"退出登录",false,230);buttons->Add(m_logout);
    m_logout->SetToolTip(text("退出本软件；勾选上方选项可同时清除本机荣耀网页登录记录"));
    card->Add(buttons,0,wxALIGN_CENTER);
    auto* guest=action(m_summary,"进入创作工作台",false,230);
    card->Add(guest,0,wxALIGN_CENTER|wxTOP,FromDIP(14));
    // Keep the summary above the actions anchored while reducing their gap.
    wxClientDC measure(m_summary);auto hint_font=m_status->GetFont();hint_font.SetPointSize(9);measure.SetFont(hint_font);
    m_hint_space=card->AddSpacer(measure.GetMultiLineTextExtent(text("国\n国")).y+FromDIP(12));
    card->AddStretchSpacer(3);
    auto* note=new wxStaticText(m_summary,wxID_ANY,text("登录和注册由荣耀官方页面提供"));style_text(note,secondary_text_colour(),9);
    card->Add(note,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(28));
    outer->Add(m_summary,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(28));
    m_browser_host=new wxPanel(this);m_browser_host->SetBackgroundColour(panel_colour());
    auto* browser_sizer=new wxBoxSizer(wxVERTICAL);m_browser_host->SetSizer(browser_sizer);
    outer->Add(m_browser_host,1,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(28));m_browser_host->Hide();
    m_login->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){login();});
    m_logout->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){logout();});
    guest->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){m_enter_workspace();});
    m_cancel_login->Bind(wxEVT_BUTTON,[this](wxCommandEvent&){m_session.cancel();close_browser();render();});
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){poll();},m_timer.GetId());
    // start() runs after the shell's page table has been assembled.
}
AccountWorkspace::~AccountWorkspace()
{
    m_timer.Stop();
    m_changed={};m_session.cancel();
}
void AccountWorkspace::start() { m_session.restore();render();m_timer.Start(150); }
void AccountWorkspace::render()
{
    const auto& s=m_session.snapshot();
    m_name->SetLabel(text(s.has_identity()?(s.identity.name.empty()?"荣耀用户":s.identity.name):"未登录"));
    const auto message=!s.has_identity()&&!m_logout_message.empty()?m_logout_message:s.message;
    m_status->SetLabel(text(message.empty()?"登录后将在本机安全保持会话":message));m_status->Wrap(FromDIP(520));
    m_login->Show(!s.has_identity());
    if(m_login->Enable(!s.busy()&&!m_clear_only))m_login->Refresh();
    m_hint_space->Show(!s.has_identity());
    m_clear_login_row->Show(s.has_identity());
    if(!s.has_identity())m_clear_login->SetValue(false);
    m_logout->Show(s.has_identity());
    m_avatar->Enable(s.has_identity());
    m_avatar->SetName(text(s.has_identity()?"更换头像":"未登录头像"));
    m_avatar->SetToolTip(text(s.has_identity()?"更换头像":"登录后可更换头像"));
    m_avatar->SetCursor(wxCursor(s.has_identity()?wxCURSOR_HAND:wxCURSOR_ARROW));
    if(m_browser)m_browser_status->SetLabel(text(m_browser_initializing?"正在打开荣耀登录页…":s.message));
    m_browser_status->Show(m_browser!=nullptr&&!m_clear_only);m_cancel_login->Show(m_browser!=nullptr&&!m_clear_only);
    load_avatar();m_summary->Layout();Layout();
    if(m_changed)m_changed(s,m_avatar_bitmap);
}
void AccountWorkspace::poll()
{
    bool changed=m_session.poll();
    if(m_browser_reset&&m_browser_reset->done.load()) {
        const bool cleared=m_browser_reset->success&&m_session.complete_browser_reset();
        m_browser_reset.reset();
        BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=browser_reset result=" << (cleared?"completed":"failed");
        if(!cleared) {
            m_browser_initializing=false;
            browser_failed("登录记录未能完全清除，下次登录会重试清理；若仍失败，请重新打开软件");
            close_browser();
        } else if(m_clear_only) {
            m_logout_message="本机荣耀登录状态和验证记录已清除";
            close_browser();
        } else navigate_authorization();
        changed=true;
    }
    if(m_browser&&m_browser_initializing&&std::chrono::steady_clock::now()>=m_browser_deadline) {
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=browser_prepare result=timeout";
        m_browser_initializing=false;
        browser_failed("荣耀登录页面准备超时，请重试；若仍在清理账号记录，请重新打开软件");
        close_browser();changed=true;
    }
    const auto state=m_session.snapshot().state;
    if(m_browser && !m_clear_only && state!=HA::State::Authorizing && state!=HA::State::Exchanging) {close_browser();changed=true;}
    if(m_browser && m_clear_only && !m_browser_initializing && !m_browser_reset) {close_browser();changed=true;}
    if(changed)render();
}
void AccountWorkspace::load_avatar()
{
    const auto& snapshot=m_session.snapshot();
    const auto path=AccountAvatar::account_path(text(Slic3r::data_dir()),m_session.config(),snapshot.identity.subject);
    if(m_avatar_loaded&&path==m_avatar_path)return;
    m_avatar_loaded=true;m_avatar_path=path;
    m_avatar_image=path.empty()?m_guest_avatar:AccountAvatar::read(path);
    if(!m_avatar_image.IsOk())m_avatar_image=m_default_avatar;
    m_avatar_bitmap=wxBitmap(AccountAvatar::circle(m_avatar_image,FromDIP(144)));
    m_avatar->set_icon(m_avatar_bitmap);
}
void AccountWorkspace::edit_avatar()
{
    if(!m_session.snapshot().has_identity())return;
    const auto path=m_avatar_path;
    wxDialog dialog(this,wxID_ANY,text("更换头像"),wxDefaultPosition,wxDefaultSize,wxBORDER_SIMPLE);
    dialog.SetBackgroundColour(panel_colour());
    auto* layout=new wxBoxSizer(wxVERTICAL);dialog.SetSizer(layout);
    auto* title=new wxStaticText(&dialog,wxID_ANY,text("更换头像"));style_text(title,primary_text_colour(),18,true);
    auto* header=new wxBoxSizer(wxHORIZONTAL);
    header->Add(new HonorBrandMark(&dialog),0,wxALIGN_CENTER_VERTICAL|wxRIGHT,FromDIP(10));
    header->Add(title,1,wxALIGN_CENTER_VERTICAL);
    layout->Add(header,0,wxEXPAND|wxALL,FromDIP(24));
    wxImage candidate=m_avatar_image;
    auto preview_bitmap=[&] {
        wxBitmap bitmap(AccountAvatar::circle(candidate,dialog.FromDIP(128)));
        bitmap.SetScaleFactor(dialog.GetDPIScaleFactor());return bitmap;
    };
    auto* preview=new wxStaticBitmap(&dialog,wxID_ANY,preview_bitmap());
    layout->Add(preview,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(16));
    auto* hint=new wxStaticText(&dialog,wxID_ANY,text("头像仅在本机使用，支持 PNG / JPG\n不超过 10MB，图片宽高不超过 4096 像素"),wxDefaultPosition,wxDefaultSize,wxALIGN_CENTER);
    style_text(hint,secondary_text_colour(),10);layout->Add(hint,0,wxALIGN_CENTER|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(24));
    auto* choices=new wxBoxSizer(wxHORIZONTAL);
    auto* choose=action(&dialog,"选择图片",false,150);choices->Add(choose,0,wxRIGHT,FromDIP(12));
    auto* reset=action(&dialog,"恢复默认头像",false,150);choices->Add(reset);
    layout->Add(choices,0,wxALIGN_CENTER|wxBOTTOM,FromDIP(18));
    auto* status=new wxStaticText(&dialog,wxID_ANY,wxEmptyString,wxDefaultPosition,FromDIP(wxSize(390,48)),wxALIGN_CENTER);
    style_text(status,secondary_text_colour(),10);layout->Add(status,0,wxALIGN_CENTER|wxLEFT|wxRIGHT,FromDIP(24));
    auto* footer=new wxBoxSizer(wxHORIZONTAL);
    auto* cancel=action(&dialog,"取消",false,150);footer->Add(cancel,0,wxRIGHT,FromDIP(12));
    auto* save=action(&dialog,"保存头像",true,150);save->Enable(false);footer->Add(save);
    layout->Add(footer,0,wxALIGN_CENTER|wxALL,FromDIP(24));
    bool use_default=false;
    auto update=[&] {
        preview->SetBitmap(preview_bitmap());
        status->SetLabel(wxEmptyString);save->Enable(true);save->Refresh();dialog.Layout();
    };
    choose->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        wxString selected;
        {
            wxFileDialog picker(&dialog,text("选择头像图片"),wxEmptyString,wxEmptyString,
                "PNG / JPG (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
            if(picker.ShowModal()!=wxID_OK)return;
            selected=picker.GetPath();
        } // Native picker restores its parent's enabled state before painting.
        const auto source=AccountAvatar::read(selected);
        if(!source.IsOk()) {
            status->SetLabel(text("无法读取图片，请选择有效的 PNG / JPG\n文件不超过 10MB，宽高不超过 4096 像素"));return;
        }
        candidate=AccountAvatar::square(source);use_default=false;update();
    });
    reset->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {candidate=m_default_avatar;use_default=true;update();});
    cancel->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {dialog.EndModal(wxID_CANCEL);});
    dialog.Bind(wxEVT_CHAR_HOOK,[&](wxKeyEvent& event) {
        if(event.GetKeyCode()==WXK_ESCAPE)dialog.EndModal(wxID_CANCEL);else event.Skip();
    });
    save->Bind(wxEVT_BUTTON,[&](wxCommandEvent&) {
        // Session polling continues inside modal loops: never apply a pending
        // image to an account that has logged out or changed meanwhile.
        if(!m_session.snapshot().has_identity()||path!=m_avatar_path) {dialog.EndModal(wxID_CANCEL);return;}
        const bool ok=use_default?AccountAvatar::reset(path):AccountAvatar::save(path,candidate);
        if(!ok) {status->SetLabel(text("头像保存失败，原头像已保留\n请检查用户数据目录是否可写后重试"));return;}
        m_avatar_loaded=false;render();dialog.EndModal(wxID_OK);
    });
    dialog.Fit();dialog.CentreOnParent();choose->SetFocus();dialog.ShowModal();
}
void AccountWorkspace::close_browser()
{
    if(m_browser) {
        if(m_browser_reset&&!m_browser_reset->done.load()) {
            // Native completion can be lost if the controller is destroyed.
            // Keep it hidden until the timer finishes; shutdown is safe because
            // the durable request forces another reset before future login.
            m_clear_only=true;m_pending_authorization.clear();
            m_browser->Stop();m_browser->Hide();
            m_browser_status->Hide();m_cancel_login->Hide();
            m_browser_host->Hide();m_summary->Show();Layout();return;
        }
        // Keep the dedicated site's trusted-browser/session data across normal
        // success, cancellation, failure and restarts. No legacy native bridge.
        m_browser->Stop();
        m_browser_host->GetSizer()->Detach(m_browser);m_browser->Hide();
        m_browser->Destroy();
        m_browser=nullptr;
    }
    m_browser_initializing=false;m_clear_only=false;m_browser_reset.reset();m_pending_authorization.clear();
    m_browser_status->Hide();m_cancel_login->Hide();
    m_browser_host->Hide();m_summary->Show();Layout();
}
void AccountWorkspace::browser_failed(const char* message)
{
    if(m_clear_only)m_logout_message=message;
    else m_session.fail_authorization(message);
}
bool AccountWorkspace::open_browser(const std::string& url,bool clear_only)
{
    auto config=HonorBrowserProfile::configuration(text(Slic3r::data_dir()),m_session.config());
    if(!config)return false;
    m_browser=wxWebView::New(*config);
    if(!m_browser)return false;
    if(!m_browser->Create(m_browser_host,wxID_ANY,"about:blank",wxDefaultPosition,wxDefaultSize,wxBORDER_NONE)) {
        m_browser->Destroy();m_browser=nullptr;return false;
    }
    m_browser_initializing=true;m_clear_only=clear_only;m_pending_authorization=url;
    m_browser_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    m_browser->EnableAccessToDevTools(false);m_browser->EnableContextMenu(false);m_browser->EnableHistory(false);
    m_browser->SetBackgroundColour(panel_colour());
    const auto theme_script=honor_account_theme_script();
    m_browser->AddUserScript(theme_script,wxWEBVIEW_INJECT_AT_DOCUMENT_START);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED,[this,theme_script](wxWebViewEvent& event) {
        if(m_browser_initializing&&event.GetURL()=="about:blank") {
            // Start outside the native callback; cancellation can close a view
            // before this queued preparation runs.
            auto* browser=m_browser;
            CallAfter([this,browser]{if(m_browser==browser&&m_browser_initializing)prepare_browser();});
            return;
        }
        // Reapply idempotently if a backend couldn't inject before first paint.
        if(m_browser)m_browser->RunScriptAsync(theme_script);
    });
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING,[this](wxWebViewEvent& event) {
        const auto url=event.GetURL().ToStdString(wxConvUTF8);
        if(m_session.callback(url)) {
            event.Veto();
            // Destroying the native controller from its own navigation callback
            // is unsafe. The timer closes it after verification or failure.
            render();return;
        }
        if(url!="about:blank"&&url.compare(0,8,"https://")!=0)event.Veto();
    });
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW,[this](wxWebViewEvent& event) {
        event.Veto();auto url=event.GetURL().ToStdString(wxConvUTF8);
        if(m_session.callback(url))render();
        else if(url.compare(0,8,"https://")==0 && m_browser)m_browser->LoadURL(text(url));
    });
    m_browser->Bind(wxEVT_WEBVIEW_ERROR,[this](wxWebViewEvent& event) {
        // Do not display WebView's error text: it may contain the full URL.
        if(m_clear_only||m_session.snapshot().state==HA::State::Authorizing) {
            BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] Authorization page failed; WebView category=" << event.GetInt();
            m_browser_initializing=false;
            browser_failed("荣耀授权页面未能打开，请稍后重试或联系软件提供方");render();
        }
    });
    m_browser_host->GetSizer()->Add(m_browser,1,wxEXPAND);
    if(clear_only)m_browser->Hide();
    else {m_summary->Hide();m_browser_host->Show();}
    Layout();
    m_browser->LoadURL("about:blank");return true;
}
void AccountWorkspace::prepare_browser()
{
    if(m_browser_reset)return; // Multiple about:blank events share one reset.
    if(!HonorBrowserProfile::prepare(m_browser)) {
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=browser_prepare result=failed";
        m_browser_initializing=false;
        browser_failed("荣耀登录页面无法准备，请检查 WebView2 运行时后重试；待清理记录将保留清理请求");return;
    }
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=browser_prepare result=ready persistent=1";
    if(m_session.browser_reset_pending()) {
        BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=browser_reset result=start";
        m_browser_reset=HonorBrowserProfile::reset(m_browser);return;
    }
    if(m_clear_only) {
        m_browser_initializing=false;m_logout_message="本机荣耀登录状态和验证记录已清除";return;
    }
    navigate_authorization();
}
void AccountWorkspace::navigate_authorization()
{
    if(!m_browser||!m_browser_initializing||m_session.snapshot().state!=HA::State::Authorizing)return;
    const auto url=std::move(m_pending_authorization);m_pending_authorization.clear();
    m_browser_initializing=false;
    m_browser->LoadURL(text(url));render();
}
void AccountWorkspace::login()
{
    if(m_clear_only||m_browser)return;
    m_logout_message.clear();
    auto url=m_session.login();
    if(!url.empty()&&!open_browser(url))m_session.fail_authorization("登录页面需要可用的 Microsoft Edge WebView2 运行时及可写的用户数据目录");
    render();
}
void AccountWorkspace::logout()
{
    const bool clear=m_clear_login->GetValue();
    if(!m_session.logout(clear)) {render();return;}
    m_logout_message.clear();close_browser();
    if(clear) {
        m_logout_message="已退出，正在清除本机荣耀登录和验证记录…";
        if(!open_browser({},true))m_logout_message="已退出，登录记录尚未清除；下次登录会先重试清理";
    }
    render();
}
} // namespace Slic3r::GUI
