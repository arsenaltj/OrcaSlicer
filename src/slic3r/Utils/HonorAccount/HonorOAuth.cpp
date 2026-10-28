#include "HonorOAuth.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <regex>
#include <vector>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

namespace Slic3r::HonorAccount {
namespace {
using Json = nlohmann::json;
[[noreturn]] void protocol(const char* message) { throw Error(Failure::Protocol, message); }
struct Uri { std::string base, query, fragment, host; };
Uri uri(const std::string& value)
{
    if (value.empty() || value.find_first_of("\r\n\t \\") != std::string::npos)
        protocol("地址格式无效");
    static const std::regex pattern(R"(^([A-Za-z][A-Za-z0-9+.-]*):(?://([^/?#]*))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$)");
    std::smatch m;
    if (!std::regex_match(value, m, pattern)) protocol("地址格式无效");
    std::string scheme = m[1], host = m[2];
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    if (host.find('@') != std::string::npos) protocol("地址不能包含用户凭据");
    return {scheme + ":" + (m[2].matched ? "//" + host : "") + m[3].str(), m[4], m[5], host};
}
bool honor_https(const std::string& value)
{
    try {
        auto u = uri(value);
        const auto suffix = [&](const std::string& s) {
            return u.host.size() > s.size() && u.host.compare(u.host.size()-s.size(), s.size(), s) == 0;
        };
        return u.base.compare(0, 8, "https://") == 0 && u.query.empty() && u.fragment.empty() &&
               (suffix(".honor.com") || suffix(".hihonor.com"));
    } catch (...) { return false; }
}
std::string decode(const std::string& value)
{
    std::string out;
    auto hex = [](char c)->int { if (c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
    for (size_t i=0; i<value.size(); ++i) {
        if (value[i]=='%') {
            if (i+2>=value.size() || hex(value[i+1])<0 || hex(value[i+2])<0) protocol("回调参数编码无效");
            const char c = char(hex(value[i+1])*16 + hex(value[i+2]));
            if (!c || c=='\r' || c=='\n') protocol("回调参数无效");
            out += c; i += 2;
        } else out += value[i]=='+' ? ' ' : value[i];
    }
    return out;
}
std::map<std::string,std::string> query(const std::string& value)
{
    std::map<std::string,std::string> result;
    size_t begin=0;
    while (begin<value.size()) {
        auto end=value.find('&',begin); if(end==std::string::npos) end=value.size();
        auto field=value.substr(begin,end-begin); auto equal=field.find('=');
        if(equal==std::string::npos) protocol("回调参数无效");
        auto name=decode(field.substr(0,equal));
        if(name.empty() || !result.emplace(name,decode(field.substr(equal+1))).second) protocol("回调含重复参数");
        begin=end+1;
    }
    return result;
}
std::string unbase64(const std::string& value)
{
    if(value.empty() || value.size()>256*1024 || value.size()%4==1) protocol("身份令牌编码无效");
    std::string padded=value;
    for(char& c:padded) { if(c=='-')c='+'; else if(c=='_')c='/'; else if(!std::isalnum(static_cast<unsigned char>(c)))protocol("身份令牌编码无效"); }
    while(padded.size()%4) padded+='=';
    std::string out(padded.size()/4*3,'\0');
    auto size=EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]), reinterpret_cast<const unsigned char*>(padded.data()),int(padded.size()));
    if(size<0)protocol("身份令牌编码无效");
    if(padded.back()=='=')--size;
    if(padded.size()>1 && padded[padded.size()-2]=='=')--size;
    out.resize(size); return out;
}
std::string field(const Json& j,const char* name) { return j.contains(name)&&j[name].is_string()?j[name].get<std::string>():std::string(); }
}

std::int64_t now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string digest(const std::string& value)
{
    unsigned char hash[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>(value.data()),value.size(),hash);
    static const char* hex="0123456789abcdef"; std::string out;
    for(auto c:hash) { out+=hex[c>>4]; out+=hex[c&15]; } return out;
}
std::string random_value()
{
    unsigned char bytes[32]; if(RAND_bytes(bytes,sizeof(bytes))!=1)protocol("无法生成安全授权请求");
    return digest(std::string(reinterpret_cast<char*>(bytes),sizeof(bytes)));
}
std::string encode(const std::string& value)
{
    const char* hex="0123456789ABCDEF"; std::string out;
    for(unsigned char c:value) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=c;
        else { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    } return out;
}
std::string Config::validate() const
{
    if(environment!="test" && environment!="production")return "账号服务环境无效";
    if(client_id.empty()||client_secret.empty()||redirect_uri.empty()||authorize_url.empty()||token_url.empty()||jwks_url.empty()||issuer.empty())
        return "荣耀账号接入尚未就绪，请联系软件提供方";
    const Config trial;
    if(client_id==trial.client_id) {
        // This App ID belongs to the user's China test environment. Neither a
        // legacy label nor a partial endpoint override may send its secret to
        // production. Its session fingerprint changes with the whole profile.
        if(environment!=trial.environment||authorize_url!=trial.authorize_url||token_url!=trial.token_url||
           jwks_url!=trial.jwks_url||issuer!=trial.issuer)
            return "此测试 App ID 必须使用配套的荣耀测试账号服务";
    }
    if(!std::all_of(client_id.begin(),client_id.end(),[](char c){return c>='0'&&c<='9';}))return "App ID 格式无效";
    if(!honor_https(authorize_url)||!honor_https(token_url)||!honor_https(jwks_url)||!honor_https(issuer))
        return "授权、令牌、公钥及签发者地址须使用荣耀域名下的 HTTPS 地址";
    try {
        auto r=uri(redirect_uri); if(redirect_uri.find('#')!=std::string::npos)return "回调地址不能带片段";
        auto fixed=query(r.query);
        if(fixed.count("state")||fixed.count("code")||fixed.count("error"))return "回调固定参数不能占用 OAuth 参数名";
    }
    catch(...) { return "已登记回调地址格式无效"; }
    if(environment=="test") {
        for(const auto& endpoint:{authorize_url,token_url,jwks_url}) {
            const auto host=uri(endpoint).host;
            if(host=="hnoauth-login.cloud.honor.com"||host=="hnoauth-login.cloud.hihonor.com")
                return "测试配置不能使用生产授权、令牌或公钥地址";
        }
    }
    return {};
}
std::string Config::key() const
{
    return digest(environment+"\n"+client_id+"\n"+redirect_uri+"\n"+authorize_url+"\n"+token_url+"\n"+jwks_url+"\n"+issuer);
}
std::string authorization_url(const Config& c,const std::string& state,const std::string& nonce)
{
    if(auto error=c.validate();!error.empty())throw Error(Failure::Configuration,error);
    return c.authorize_url+"?response_type=code&client_id="+encode(c.client_id)+"&redirect_uri="+encode(c.redirect_uri)+
        "&scope=openid%20profile&access_type=offline&state="+encode(state)+"&nonce="+encode(nonce)+"&themeName=dark";
}
bool is_callback(const std::string& url,const std::string& redirect)
{
    try { return uri(url).base==uri(redirect).base; } catch(...) { return false; }
}
std::string authorization_code(const std::string& url,const std::string& redirect,const std::string& state)
{
    if(!is_callback(url,redirect))protocol("授权回调地址不匹配");
    auto actual=uri(url); if(!actual.fragment.empty())protocol("授权回调格式无效");
    auto params=query(actual.query); auto fixed=query(uri(redirect).query);
    for(const auto& item:fixed)if(params[item.first]!=item.second)protocol("授权回调参数不匹配");
    if(state.empty()||params["state"]!=state)protocol("授权校验失败，请重新登录");
    if(params.count("error"))throw Error(Failure::Cancelled,"授权未完成，可重新登录");
    if(params["code"].empty()||params["code"].size()>16384)protocol("授权回调缺少有效授权码");
    return params["code"];
}
Identity verify_id_token(const std::string& jwt,const std::string& jwks,const Config& c,const std::string& nonce,std::int64_t now)
{
    try {
        auto first=jwt.find('.'),last=jwt.rfind('.');
        if(first==std::string::npos||first==last||jwt.size()>256*1024)protocol("身份令牌格式无效");
        auto header=Json::parse(unbase64(jwt.substr(0,first)));
        auto claims=Json::parse(unbase64(jwt.substr(first+1,last-first-1)));
        if(field(header,"alg")!="RS256"||field(header,"kid").empty()||header.contains("crit"))protocol("身份令牌算法无效");
        auto keys=Json::parse(jwks).at("keys");
        Json key;
        for(const auto& item:keys)if(field(item,"kid")==field(header,"kid")) { if(!key.is_null())protocol("身份公钥不唯一"); key=item; }
        if(key.is_null())protocol("身份公钥尚未更新，请重试");
        if(field(key,"kty")!="RSA"||(!field(key,"use").empty()&&field(key,"use")!="sig")||
            (!field(key,"alg").empty()&&field(key,"alg")!="RS256"))protocol("身份公钥无效");
        const auto n=unbase64(key.at("n")), e=unbase64(key.at("e"));
        std::unique_ptr<BIGNUM,decltype(&BN_free)> bn(BN_bin2bn(reinterpret_cast<const unsigned char*>(n.data()),int(n.size()),nullptr),BN_free);
        std::unique_ptr<BIGNUM,decltype(&BN_free)> be(BN_bin2bn(reinterpret_cast<const unsigned char*>(e.data()),int(e.size()),nullptr),BN_free);
        std::unique_ptr<RSA,decltype(&RSA_free)> rsa(RSA_new(),RSA_free);
        if(!bn||!be||!rsa||BN_num_bits(bn.get())<2048||RSA_set0_key(rsa.get(),bn.get(),be.get(),nullptr)!=1)protocol("身份公钥无效");
        bn.release();be.release();
        std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> pkey(EVP_PKEY_new(),EVP_PKEY_free);
        if(!pkey||EVP_PKEY_set1_RSA(pkey.get(),rsa.get())!=1)protocol("身份公钥无效");
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        auto signature=unbase64(jwt.substr(last+1));
        if(!ctx||EVP_DigestVerifyInit(ctx.get(),nullptr,EVP_sha256(),nullptr,pkey.get())!=1||
            EVP_DigestVerify(ctx.get(),reinterpret_cast<const unsigned char*>(signature.data()),signature.size(),
                reinterpret_cast<const unsigned char*>(jwt.data()),last)!=1)protocol("身份签名校验失败");
        if(field(claims,"iss")!=c.issuer)protocol("身份签发者不匹配");
        bool audience=false;
        if(claims.at("aud").is_string())audience=claims["aud"]==c.client_id;
        else if(claims["aud"].is_array()) {
            for(const auto& a:claims["aud"])if(a==c.client_id)audience=true;
            if(claims["aud"].size()>1 && field(claims,"azp")!=c.client_id)audience=false;
        }
        if(!audience||(!field(claims,"azp").empty()&&field(claims,"azp")!=c.client_id))protocol("身份受众不匹配");
        if(!claims.at("exp").is_number_integer()||!claims.at("iat").is_number_integer())protocol("身份有效期无效");
        auto expires=claims["exp"].get<std::int64_t>(), issued=claims["iat"].get<std::int64_t>();
        if(expires<=now||issued>now+60||issued>=expires)protocol("身份令牌已过期或设备时间不正确");
        if(claims.contains("nbf")&&(!claims["nbf"].is_number_integer()||claims["nbf"].get<std::int64_t>()>now+60))protocol("身份令牌尚未生效");
        if(!nonce.empty()&&field(claims,"nonce")!=nonce)protocol("身份随机校验失败");
        auto subject=field(claims,"sub");if(subject.empty())protocol("身份令牌缺少用户标识");
        return {subject,field(claims,"display_name"),field(claims,"picture"),expires};
    } catch(const Error&) { throw; } catch(...) { protocol("身份令牌或公钥响应无效"); }
}
Failure oauth_failure(unsigned status,const std::string& body,bool refreshing)
{
    if(status==0||status==408||status==429||status>=500)return Failure::Temporary;
    auto j=Json::parse(body,nullptr,false);
    if(j.is_object()) {
        const auto err=field(j,"error");
        if(refreshing && err=="invalid_grant")return Failure::InvalidCredentials;
        // HONOR's documented refresh-token invalid/expired/revoked sub-errors.
        if(refreshing && j.contains("sub_error") && j["sub_error"].is_number_integer()) {
            const auto sub=j["sub_error"].get<int>();
            if(sub==11205 || sub==31204)return Failure::InvalidCredentials;
            return Failure::Configuration;
        }
        if(err=="invalid_client"||err=="unauthorized_client")return Failure::Configuration;
    }
    return Failure::Protocol;
}
std::string serialize_config(const Config& c) { return Json{{"environment",c.environment},{"client_id",c.client_id},{"client_secret",c.client_secret},{"redirect_uri",c.redirect_uri},{"authorize_url",c.authorize_url},{"token_url",c.token_url},{"jwks_url",c.jwks_url},{"issuer",c.issuer},{"login_channel",c.login_channel},{"req_client_type",c.req_client_type}}.dump(); }
Config parse_config(const std::string& s) { auto j=Json::parse(s); Config c; c.environment=j.at("environment");c.client_id=j.at("client_id");c.client_secret=j.at("client_secret");c.redirect_uri=j.at("redirect_uri");c.authorize_url=j.at("authorize_url");c.token_url=j.at("token_url");c.jwks_url=j.at("jwks_url");c.issuer=j.at("issuer");c.login_channel=j.value("login_channel","99013101");c.req_client_type=j.value("req_client_type","131");return c; }
std::string serialize_credentials(const Credentials& c) { return Json{{"access_token",c.access_token},{"refresh_token",c.refresh_token},{"id_token",c.id_token},{"expires_at",c.expires_at},{"subject",c.identity.subject},{"name",c.identity.name},{"picture",c.identity.picture},{"identity_expires_at",c.identity.expires_at}}.dump(); }
Credentials parse_credentials(const std::string& s) { auto j=Json::parse(s);Credentials c;c.access_token=j.at("access_token");c.refresh_token=j.at("refresh_token");c.id_token=j.at("id_token");c.expires_at=j.at("expires_at");c.identity={j.at("subject"),j.at("name"),j.at("picture"),j.at("identity_expires_at")};if(c.identity.subject.empty()||c.id_token.empty()||c.access_token.empty())protocol("本机账号凭据不完整，请重新登录");return c; }
} // namespace Slic3r::HonorAccount
