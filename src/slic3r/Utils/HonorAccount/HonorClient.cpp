#include "HonorClient.hpp"
#include "../Http.hpp"
#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

namespace Slic3r::HonorAccount {
namespace {
struct Transfer { std::string body; Cancellation cancel; };
size_t receive(char* data,size_t size,size_t count,void* context)
{
    auto& t=*static_cast<Transfer*>(context);
    if(t.cancel->load() || size*count>1024*1024-t.body.size())return 0;
    t.body.append(data,size*count);return size*count;
}
int progress(void* context,curl_off_t,curl_off_t,curl_off_t,curl_off_t) { return static_cast<Transfer*>(context)->cancel->load()?1:0; }
std::string safe_code(unsigned status,const std::string& body)
{
    std::string message="账号服务请求未完成（HTTP "+std::to_string(status)+"）";
    const auto j=nlohmann::json::parse(body,nullptr,false);
    if(j.is_object()&&j.contains("sub_error")&&j["sub_error"].is_number_integer())message+="，荣耀错误码 "+std::to_string(j["sub_error"].get<int>());
    return message; // Never use remote error descriptions: they can echo secrets.
}
}
Response https_request(const std::string& url,const std::string& form,Cancellation cancel)
{
    Http::tls_global_init();
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if(!curl)throw Error(Failure::Temporary,"无法创建账号网络请求");
    Transfer transfer{{},cancel};
    curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
#ifdef _WIN32
    // This build uses OpenSSL libcurl; use Windows roots as Http::tls_verify does.
    curl_easy_setopt(curl.get(),CURLOPT_SSL_OPTIONS,long(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS,CURLPROTO_HTTPS);
    // In particular, never replay an APP Secret to a redirected host.
    curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,8L);
    curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,20L);
    curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&transfer);
    curl_easy_setopt(curl.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl.get(),CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl.get(),CURLOPT_XFERINFODATA,&transfer);
    if(!form.empty()) {
        curl_easy_setopt(curl.get(),CURLOPT_POST,1L);
        curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDS,form.data());
        curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDSIZE,long(form.size()));
    }
    // libcurl's form POST defaults to application/x-www-form-urlencoded.
    const auto result=curl_easy_perform(curl.get());
    if(cancel->load())throw Error(Failure::Cancelled,"登录已取消");
    if(result!=CURLE_OK)throw Error(Failure::Temporary,"账号服务暂时无法连接，请检查网络后重试");
    long status=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    return {unsigned(status),std::move(transfer.body)};
}
std::string Client::keys(const Config& c,Cancellation cancel,bool force)
{
    {
        std::lock_guard<std::mutex> lock(m_cache->mutex);
        if(!force&&m_cache->url==c.jwks_url&&!m_cache->json.empty()&&now_seconds()-m_cache->fetched<3600)return m_cache->json;
    }
    auto response=m_transport(c.jwks_url,{},cancel);
    if(response.status!=200)throw Error(Failure::Temporary,"无法获取账号身份公钥，请稍后重试");
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    m_cache->url=c.jwks_url;m_cache->json=std::move(response.body);m_cache->fetched=now_seconds();return m_cache->json;
}
Credentials Client::tokens(const Config& c,const std::string& form,const std::string& nonce,const Credentials* previous,Cancellation cancel)
{
    if(auto error=c.validate();!error.empty())throw Error(Failure::Configuration,error);
    auto response=m_transport(c.token_url,form,cancel);
    if(cancel->load())throw Error(Failure::Cancelled,"登录已取消");
    auto j=nlohmann::json::parse(response.body,nullptr,false);
    if(response.status!=200||!j.is_object()||j.contains("error"))
        throw Error(oauth_failure(response.status,response.body,previous!=nullptr),safe_code(response.status,response.body));
    try {
        Credentials result;
        result.access_token=j.at("access_token").get<std::string>();
        result.refresh_token=j.value("refresh_token",previous?previous->refresh_token:std::string());
        result.id_token=j.at("id_token").get<std::string>();
        if(result.access_token.empty()||j.at("token_type")!="Bearer"||!j.at("expires_in").is_number_integer())throw std::runtime_error("token");
        auto lifetime=j["expires_in"].get<std::int64_t>();
        if(lifetime<=0||lifetime>86400)throw std::runtime_error("expiry");
        result.expires_at=now_seconds()+lifetime;
        auto jwks=keys(c,cancel);
        try { result.identity=verify_id_token(result.id_token,jwks,c,nonce,now_seconds()); }
        catch(const Error&) { // A rotated signing key may have a new kid; fetch once.
            result.identity=verify_id_token(result.id_token,keys(c,cancel,true),c,nonce,now_seconds());
        }
        if(previous && result.identity.subject!=previous->identity.subject)throw Error(Failure::Protocol,"恢复的账号与本机身份不一致，请重新登录");
        if(cancel->load())throw Error(Failure::Cancelled,"登录已取消");
        return result;
    } catch(const Error&) { throw; } catch(...) { throw Error(Failure::Protocol,"账号令牌响应不完整，请重新登录"); }
}
Credentials Client::exchange(const Config& c,const std::string& code,const std::string& nonce,Cancellation cancel)
{
    return tokens(c,"grant_type=authorization_code&client_id="+encode(c.client_id)+"&client_secret="+encode(c.client_secret)+
        "&redirect_uri="+encode(c.redirect_uri)+"&code="+encode(code),nonce,nullptr,cancel);
}
Credentials Client::restore(const Config& c,const Credentials& previous,Cancellation cancel)
{
    if(auto error=c.validate();!error.empty())throw Error(Failure::Configuration,error);
    if(!previous.refresh_token.empty())return tokens(c,"grant_type=refresh_token&client_id="+encode(c.client_id)+"&client_secret="+
        encode(c.client_secret)+"&refresh_token="+encode(previous.refresh_token),{},&previous,cancel);
    Credentials result=previous;
    if(std::min(result.expires_at,result.identity.expires_at)<=now_seconds())throw Error(Failure::InvalidCredentials,"登录已过期，请重新登录");
    result.identity=verify_id_token(result.id_token,keys(c,cancel),c,{},now_seconds());
    return result;
}
} // namespace Slic3r::HonorAccount
