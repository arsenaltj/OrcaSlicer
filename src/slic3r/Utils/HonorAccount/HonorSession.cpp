#include "HonorSession.hpp"
#include <algorithm>
#include <boost/log/trivial.hpp>
#include <thread>

namespace Slic3r::HonorAccount {
Session::Session(std::unique_ptr<Store> store,Transport transport,std::unique_ptr<Store> application_configuration)
    :m_store(std::move(store)),m_application_configuration(std::move(application_configuration)),m_transport(std::move(transport))
{
    try {
        auto saved=m_store->load("configuration");
        const bool profile=saved.has_value();
        if(!saved && m_application_configuration)saved=m_application_configuration->load("configuration");
        if(saved) {
            m_config=parse_config(*saved);
            BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=config_read result="
                << (m_config.validate().empty()?"ready":"invalid") << " source=" << (profile?"profile":"application");
            if(profile)share_configuration();
        } else BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=config_read result=missing";
    }
    catch(...) { m_snapshot.message="本机账号服务无法读取，请联系软件提供方";
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=config_read result=failed"; }
}
void Session::share_configuration()
{
    if(!m_application_configuration || !m_config.validate().empty())return;
    try {
        // Migrate only known provisioning from this profile, never scan other
        // profiles, overwrite existing provisioning, or copy a session.
        if(m_application_configuration->load("configuration"))return;
        const auto value=serialize_config(m_config);
        const bool ok=m_application_configuration->save("configuration",value) &&
            m_application_configuration->load("configuration")==std::optional<std::string>(value);
        BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=config_share result=" << (ok?"ready":"failed");
    } catch(...) { BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=config_share result=failed"; }
}
Session::~Session() { cancel_task(); }
void Session::cancel_task() { if(m_task)m_task->cancel->store(true);m_task.reset();m_state.clear();m_nonce.clear(); }
bool Session::configure(const Config& config)
{
    if(!config.validate().empty())return false;
    if(serialize_config(config)==serialize_config(m_config))return true;
    // Write the new protected configuration before discarding a working one.
    try {
        if(!m_store->save("configuration",serialize_config(config)))return false;
        if(!m_store->erase("session-"+m_config.key())) {
            m_store->save("configuration",serialize_config(m_config));return false;
        }
    } catch(...) { return false; }
    cancel_task();m_retry_enabled=false;m_credentials.reset();m_config=config;m_snapshot={};share_configuration();return true;
}
void Session::start(std::function<Credentials(Cancellation)> operation)
{
    auto task=m_task=std::make_shared<Task>();
    std::thread([task,operation=std::move(operation)] {
        std::optional<Credentials> result;Failure failure=Failure::Temporary;std::string message;
        try { result=operation(task->cancel); }
        catch(const Error& e) { failure=e.kind;message=e.what(); }
        catch(...) { failure=Failure::Protocol;message="账号请求未完成，请重试"; }
        std::lock_guard<std::mutex> lock(task->mutex);
        if(!task->cancel->load()) { task->credentials=std::move(result);task->failure=failure;task->message=std::move(message);task->done=true; }
    }).detach();
}
void Session::restore()
{
    if(m_snapshot.busy())return;
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=restore result=start";
    cancel_task();m_retry_enabled=false;
    if(auto error=m_config.validate();!error.empty()) { m_snapshot.state=State::SignedOut;m_snapshot.message=error;
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=restore result=config_invalid";return; }
    try {
        if(m_store->load("browser-reset-"+m_config.key())) {
            // Finish an interrupted full logout before publishing identity.
            if(logout())m_snapshot.message="登录记录待清理，下次登录前将先完成清理";
            return;
        }
    } catch(...) {
        // Failure to read the request blocks restoration without deleting a
        // previously valid session that the user may not have chosen to clear.
        m_snapshot.state=State::Failed;m_snapshot.message="本机登录记录无法读取，请稍后重试";return;
    }
    if(!m_credentials) {
        try { if(auto saved=m_store->load("session-"+m_config.key())) {m_credentials=parse_credentials(*saved);m_snapshot.persistent=true;} }
        catch(...) { m_snapshot={};m_snapshot.message="本机凭据无法读取，请重新登录";
            BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=restore result=storage_failed";return; }
    }
    if(!m_credentials) { m_snapshot={};BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=restore result=no_session";return; }
    m_snapshot.state=State::Checking;m_snapshot.message="正在验证登录状态…";
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage="
        << (m_credentials->refresh_token.empty()?"restore_verify":"refresh") << " result=start";
    // Do not publish cached identity as signed in before checking the service.
    const auto config=m_config;const auto credentials=*m_credentials;auto transport=m_transport;auto keys=m_keys;
    start([config,credentials,transport,keys](Cancellation cancel)mutable { return Client(transport,keys).restore(config,credentials,cancel); });
}
std::string Session::login()
{
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=login result=requested";
    if(m_snapshot.busy())return {};
    cancel_task();m_retry_enabled=false;
    try {
        m_state=random_value();m_nonce=random_value();
        auto url=authorization_url(m_config,m_state,m_nonce);
        m_snapshot.state=State::Authorizing;m_snapshot.message="请在下方荣耀页面完成登录或注册";
        BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=authorize result=ready";return url;
    } catch(const Error& e) { m_snapshot.state=State::Failed;m_snapshot.message=e.what();m_state.clear();m_nonce.clear();
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=authorize result=rejected failure=" << int(e.kind);return {}; }
}
bool Session::callback(const std::string& url)
{
    if(!is_callback(url,m_config.redirect_uri))return false;
    if(m_snapshot.state!=State::Authorizing)return true;
    try {
        auto code=authorization_code(url,m_config.redirect_uri,m_state);const auto nonce=m_nonce;
        BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=callback result=validated";
        m_state.clear();m_nonce.clear();m_snapshot.state=State::Exchanging;m_snapshot.message="正在确认账号身份…";
        const auto config=m_config;auto transport=m_transport;auto keys=m_keys;
        start([config,code,nonce,transport,keys](Cancellation cancel)mutable { return Client(transport,keys).exchange(config,code,nonce,cancel); });
    } catch(const Error& e) { cancel_task();m_snapshot.state=State::Failed;m_snapshot.message=e.what();
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=callback result=rejected failure=" << int(e.kind); }
    return true;
}
void Session::cancel()
{
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=cancel result=requested";
    cancel_task();m_retry_enabled=false;
    m_snapshot.state=m_credentials?State::Offline:State::SignedOut;
    if(m_credentials)m_snapshot.identity=m_credentials->identity;
    m_snapshot.message="登录已取消";
}
void Session::fail_authorization(const std::string& message) { cancel();m_snapshot.state=State::Failed;m_snapshot.message=message; }
bool Session::browser_reset_pending() const
{
    try { return m_store->load("browser-reset-"+m_config.key()).has_value(); }
    catch(...) { return true; } // Unreadable state must not permit old SSO data.
}
bool Session::complete_browser_reset()
{
    try {
        const auto key="browser-reset-"+m_config.key();
        const auto session="session-"+m_config.key();
        // The previous logout may have stopped after writing its request but
        // before erasing credentials. Never remove the request ahead of them.
        return m_store->erase(session) && !m_store->load(session).has_value() &&
            m_store->erase(key) && !m_store->load(key).has_value();
    } catch(...) { return false; }
}
bool Session::logout(bool clear_browser)
{
    cancel_task();m_retry_enabled=false;
    if(clear_browser) {
        bool requested=false;
        try {
            const auto key="browser-reset-"+m_config.key();
            requested=m_store->save(key,"pending") && m_store->load(key)==std::optional<std::string>("pending");
        } catch(...) {}
        if(!requested) {
            m_snapshot.state=State::Failed;
            m_snapshot.message="无法保存登录记录清理请求，请重试退出";
            BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=browser_reset result=request_failed";
            return false;
        }
    }
    bool erased=false;
    try { erased=m_store->erase("session-"+m_config.key()); } catch(...) {}
    if(!erased) {
        BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=logout result=storage_failed";
        m_snapshot.state=State::Failed;
        if(m_credentials)m_snapshot.identity=m_credentials->identity;
        m_snapshot.message="系统未能清除已保存的凭据，请重试退出";return false;
    }
    m_credentials.reset();m_snapshot={};
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=logout result=completed";
    return true;
}
bool Session::poll()
{
    if(m_task) {
        auto task=m_task;
        std::lock_guard<std::mutex> lock(task->mutex);
        if(!task->done)return false;
        const bool refreshing=m_snapshot.state==State::Checking;
        m_task.reset();
        if(task->credentials) {
            m_credentials=std::move(task->credentials);
            m_snapshot.identity=m_credentials->identity;m_snapshot.state=State::SignedIn;
            try { m_snapshot.persistent=m_store->save("session-"+m_config.key(),serialize_credentials(*m_credentials)); }
            catch(...) { m_snapshot.persistent=false; }
            BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=" << (refreshing?"restore":"exchange")
                << " result=verified persistent=" << m_snapshot.persistent;
            m_snapshot.message=m_snapshot.persistent?"已登录，下次打开软件将自动恢复":"已登录，但系统安全存储不可用，仅保持本次会话";
            if(m_credentials->refresh_token.empty())m_snapshot.message="已登录；未获得刷新凭据，到期后需重新登录";
            m_retry_at=std::min(m_credentials->expires_at,m_credentials->identity.expires_at)-
                (m_credentials->refresh_token.empty()?0:60);m_retry_enabled=true;
        } else if(task->failure==Failure::InvalidCredentials) {
            BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=" << (refreshing?"restore":"exchange") << " result=invalid_credentials";
            logout();m_snapshot.message="登录已失效，请重新登录";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[HonorAccount] stage=" << (refreshing?"restore":"exchange")
                << " result=failed failure=" << int(task->failure);
            m_snapshot.state=m_credentials&&task->failure==Failure::Temporary?State::Offline:State::Failed;
            if(m_credentials)m_snapshot.identity=m_credentials->identity;
            m_snapshot.message=m_credentials?"待验证。"+task->message:task->message;
            if(m_snapshot.state==State::Offline)m_snapshot.message="离线，待验证。"+task->message;
            m_retry_enabled=m_credentials && task->failure==Failure::Temporary;
            m_retry_at=now_seconds()+60;
        }
        return true;
    }
    if(m_retry_enabled&&now_seconds()>=m_retry_at) { restore();return true; }
    return false;
}
} // namespace Slic3r::HonorAccount
