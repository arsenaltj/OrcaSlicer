#include "slic3r/Utils/HonorAccount/HonorSession.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <chrono>
#include <condition_variable>
#include <thread>

using namespace Slic3r::HonorAccount;
namespace {
using Json=nlohmann::json;
std::string b64(const std::string& bytes)
{
    std::string out((bytes.size()+2)/3*4,'\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),reinterpret_cast<const unsigned char*>(bytes.data()),int(bytes.size()));
    while(!out.empty()&&out.back()=='=')out.pop_back();
    for(auto& c:out) {if(c=='+')c='-';if(c=='/')c='_';}return out;
}
Config configuration()
{
    Config c;c.environment="test";c.client_id="314159265";c.client_secret="offline-fixture-only";c.redirect_uri="https://client.example/callback?app=orca";
    c.authorize_url="https://fixture-test.cloud.honor.com/oauth2/v3/authorize";c.token_url="https://fixture-test.cloud.honor.com/oauth2/v3/token";
    c.jwks_url="https://fixture-test.cloud.honor.com/oauth2/v3/certs";c.issuer="https://fixture-test.cloud.honor.com";return c;
}
struct Signer {
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key {nullptr,EVP_PKEY_free};
    std::string jwks;
    Signer()
    {
        std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);
        REQUIRE(ctx);REQUIRE(EVP_PKEY_keygen_init(ctx.get())==1);REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(),2048)==1);
        EVP_PKEY* raw=nullptr;REQUIRE(EVP_PKEY_keygen(ctx.get(),&raw)==1);key.reset(raw);
        const auto* rsa=EVP_PKEY_get0_RSA(key.get());const BIGNUM *n,*e;RSA_get0_key(rsa,&n,&e,nullptr);
        auto bytes=[](const BIGNUM* number) {std::string s(BN_num_bytes(number),'\0');BN_bn2bin(number,reinterpret_cast<unsigned char*>(&s[0]));return s;};
        jwks=Json{{"keys",Json::array({Json{{"kty","RSA"},{"kid","fixture"},{"alg","RS256"},{"use","sig"},{"n",b64(bytes(n))},{"e",b64(bytes(e))}}})}}.dump();
    }
    Json claims(const std::string& nonce="nonce")
    {
        return {{"iss",configuration().issuer},{"aud",configuration().client_id},{"sub","fixture-union-id"},{"nonce",nonce},
            {"iat",now_seconds()-10},{"exp",now_seconds()+3600},{"display_name","测试用户"},{"picture",""}};
    }
    std::string sign(const Json& claims,const std::string& algorithm="RS256")
    {
        auto input=b64(Json{{"alg",algorithm},{"kid","fixture"}}.dump())+"."+b64(claims.dump());
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        if(EVP_DigestSignInit(ctx.get(),nullptr,EVP_sha256(),nullptr,key.get())!=1)throw std::runtime_error("fixture signing init");
        size_t size=0;if(EVP_DigestSign(ctx.get(),nullptr,&size,reinterpret_cast<const unsigned char*>(input.data()),input.size())!=1)throw std::runtime_error("fixture signing size");
        std::string sig(size,'\0');if(EVP_DigestSign(ctx.get(),reinterpret_cast<unsigned char*>(&sig[0]),&size,reinterpret_cast<const unsigned char*>(input.data()),input.size())!=1)throw std::runtime_error("fixture signing");
        sig.resize(size);return input+"."+b64(sig);
    }
    Credentials credentials()
    {
        Credentials c;c.access_token="fixture-at";c.refresh_token="fixture-rt";c.id_token=sign(claims());c.expires_at=now_seconds()+3600;
        c.identity=verify_id_token(c.id_token,jwks,configuration(),"nonce",now_seconds());return c;
    }
    Response response(const std::string& nonce="nonce",const std::string& subject="fixture-union-id")
    {
        auto payload=claims(nonce);payload["sub"]=subject;
        return {200,Json{{"access_token","rotated-at"},{"refresh_token","rotated-rt"},{"id_token",sign(payload)},{"expires_in",3600},{"token_type","Bearer"}}.dump()};
    }
};
struct Memory { std::map<std::string,std::string> values;bool writable {true}, readable {true}, erasable {true}; };
class MemoryStore final:public Store {
public:
    explicit MemoryStore(std::shared_ptr<Memory> memory):memory(memory) {}
    std::optional<std::string> load(const std::string& key) override {if(!memory->readable)throw std::runtime_error("fixture read failure");auto i=memory->values.find(key);return i==memory->values.end()?std::nullopt:std::optional<std::string>(i->second);}
    bool save(const std::string& key,const std::string& value) override {if(!memory->writable)return false;memory->values[key]=value;return true;}
    bool erase(const std::string& key) override {if(!memory->writable||!memory->erasable)return false;memory->values.erase(key);return true;}
    std::shared_ptr<Memory> memory;
};
std::shared_ptr<Memory> saved(const Credentials& credentials)
{
    auto memory=std::make_shared<Memory>();memory->values["configuration"]=serialize_config(configuration());
    memory->values["session-"+configuration().key()]=serialize_credentials(credentials);return memory;
}
void finish(Session& session)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(session.snapshot().busy()&&std::chrono::steady_clock::now()<deadline) {session.poll();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    REQUIRE_FALSE(session.snapshot().busy());
}
std::string parameter(const std::string& url,const std::string& key)
{
    auto begin=url.find(key+"=");REQUIRE(begin!=std::string::npos);begin+=key.size()+1;return url.substr(begin,url.find('&',begin)-begin);
}
}

TEST_CASE("HONOR callback rejects changed origins paths fixed parameters and states", "[HonorAccount]")
{
    auto c=configuration();auto callback=c.redirect_uri+"&state=expected&code=a%2Bb%2Fc%3D";
    REQUIRE(authorization_code(callback,c.redirect_uri,"expected")=="a+b/c=");
    REQUIRE_THROWS(authorization_code(callback,c.redirect_uri,"other"));
    REQUIRE_FALSE(is_callback("https://client.example.evil/callback?code=a",c.redirect_uri));
    REQUIRE_FALSE(is_callback("https://client.example/callback/extra?code=a",c.redirect_uri));
    REQUIRE_THROWS(authorization_code("https://client.example/callback?app=other&state=expected&code=a",c.redirect_uri,"expected"));
    REQUIRE_THROWS(authorization_code(callback+"&state=expected",c.redirect_uri,"expected"));
    REQUIRE_THROWS(authorization_code(callback+"#fragment",c.redirect_uri,"expected"));
    REQUIRE_THROWS(authorization_code(c.redirect_uri+"&state=expected&error=access_denied",c.redirect_uri,"expected"));
    REQUIRE_THROWS(authorization_code(c.redirect_uri+"&state=expected&code=%xx",c.redirect_uri,"expected"));
}
TEST_CASE("HONOR test provisioning fails closed before credentials reach production", "[HonorAccount]")
{
    auto c=configuration();REQUIRE(c.validate().empty());
    auto url=authorization_url(c,"state","nonce");REQUIRE(url.find("access_type=offline")!=std::string::npos);
    REQUIRE(url.find("client_secret")==std::string::npos);REQUIRE(url.find("loginChannel")==std::string::npos);
    c.token_url="https://hnoauth-login.cloud.honor.com/oauth2/v3/token";REQUIRE_FALSE(c.validate().empty());
    c=configuration();c.jwks_url="http://fixture-test.cloud.honor.com/certs";REQUIRE_FALSE(c.validate().empty());
    c=configuration();c.authorize_url="https://honor.com.evil/authorize";REQUIRE_FALSE(c.validate().empty());
    c=configuration();c.redirect_uri+="&state=fixed";REQUIRE_FALSE(c.validate().empty());
    c=configuration();auto original=c.key();c.issuer="https://other.cloud.honor.com";REQUIRE(c.key()!=original);
}
TEST_CASE("HONOR preconfigured test login exchanges and refreshes only through the test services", "[HonorAccount]")
{
    Config config;config.client_secret="offline-fixture-only";
    REQUIRE(config.validate().empty());
    REQUIRE(config.environment=="test");
    const std::string origin="https://hnoauth-login-test-drcn.cloud.honor.com";
    REQUIRE(config.authorize_url==origin+"/oauth2/v3/authorize");
    REQUIRE(config.token_url==origin+"/oauth2/v3/token");
    REQUIRE(config.jwks_url==origin+"/oauth2/v3/certs");
    auto memory=std::make_shared<Memory>();
    memory->values["configuration"]=serialize_config(config);
    auto signer=std::make_shared<Signer>();
    std::string nonce,posted_form,posted_url,keys_url;
    Transport transport=[&](const std::string& url,const std::string& form,Cancellation) {
        if(form.empty()) {keys_url=url;return Response{200,signer->jwks};}
        posted_url=url;posted_form=form;
        auto payload=signer->claims(nonce);payload["iss"]=config.issuer;payload["aud"]=config.client_id;
        return Response{200,Json{{"access_token","fixture-at"},{"refresh_token","fixture-rt"},
            {"id_token",signer->sign(payload)},{"expires_in",3600},{"token_type","Bearer"}}.dump()};
    };
    Session session(std::make_unique<MemoryStore>(memory),transport);
    session.restore();REQUIRE(session.snapshot().state==State::SignedOut);
    auto url=session.login();nonce=parameter(url,"nonce");auto state=parameter(url,"state");
    REQUIRE(url.find(config.authorize_url+"?")==0);
    REQUIRE(parameter(url,"redirect_uri")==encode("honorid://redirect_uri"));
    REQUIRE(url.find(config.client_secret)==std::string::npos);
    REQUIRE_FALSE(session.callback("honorid://redirect_url?state="+state+"&code=fixture"));
    REQUIRE(session.snapshot().state==State::Authorizing);
    REQUIRE_FALSE(is_callback("honorid://redirect_uri.evil?code=fixture",config.redirect_uri));
    REQUIRE_THROWS(authorization_code(config.redirect_uri+"?code=fixture",config.redirect_uri,state));
    REQUIRE(session.callback(config.redirect_uri+"?state="+state+"&code=a%2Bb%2Fc%3D"));
    finish(session);
    REQUIRE(session.snapshot().state==State::SignedIn);
    REQUIRE(posted_url==config.token_url);
    REQUIRE(keys_url==config.jwks_url);
    REQUIRE(parameter(posted_form,"redirect_uri")==encode(config.redirect_uri));
    REQUIRE(parameter(posted_form,"code")=="a%2Bb%2Fc%3D");
    Session restored(std::make_unique<MemoryStore>(memory),transport);
    restored.restore();finish(restored);
    REQUIRE(restored.snapshot().state==State::SignedIn);
    REQUIRE(posted_url==config.token_url);
    REQUIRE(parameter(posted_form,"grant_type")=="refresh_token");
    REQUIRE(keys_url==config.jwks_url);
    auto changed=config;changed.token_url=configuration().token_url;
    REQUIRE_FALSE(changed.validate().empty());
    changed=config;changed.client_secret.clear();REQUIRE_FALSE(changed.validate().empty());
}
TEST_CASE("HONOR test App ID rejects legacy labels and mixed production endpoints before any request", "[HonorAccount]")
{
    Config config;config.client_secret="offline-fixture-only";
    const auto test_key=config.key();
    SECTION("legacy official label") {config.environment="official-web";}
    SECTION("production label") {config.environment="production";}
    SECTION("production authorization") {config.authorize_url="https://hnoauth-login.cloud.honor.com/oauth2/v3/authorize";}
    SECTION("production token") {config.token_url="https://hnoauth-login.cloud.honor.com/oauth2/v3/token";}
    SECTION("production public keys") {config.jwks_url="https://hnoauth-login.cloud.honor.com/oauth2/v3/certs";}
    SECTION("legacy production keys") {config.jwks_url="https://hnoauth-login.cloud.hihonor.com/oauth2/v3/certs";}
    SECTION("unverified issuer") {config.issuer="https://honorid-test.hihonor.com";}
    REQUIRE_FALSE(config.validate().empty());
    REQUIRE(config.key()!=test_key);
    unsigned requests=0;
    Client client([&](const std::string&,const std::string&,Cancellation){++requests;return Response{};});
    auto cancel=std::make_shared<std::atomic_bool>(false);
    REQUIRE_THROWS(authorization_url(config,"state","nonce"));
    REQUIRE_THROWS(client.exchange(config,"fixture-code","nonce",cancel));
    Credentials previous;previous.refresh_token="fixture-rt";
    REQUIRE_THROWS(client.restore(config,previous,cancel));
    previous.refresh_token.clear();previous.expires_at=now_seconds()+3600;previous.identity.expires_at=previous.expires_at;
    REQUIRE_THROWS(client.restore(config,previous,cancel));
    REQUIRE(requests==0);
}
TEST_CASE("HONOR verifies RSA signature issuer audience nonce and validity before exposing identity", "[HonorAccount]")
{
    Signer signer;auto c=configuration();auto payload=signer.claims();
    auto identity=verify_id_token(signer.sign(payload),signer.jwks,c,"nonce",now_seconds());
    REQUIRE(identity.subject=="fixture-union-id");REQUIRE(identity.name=="测试用户");
    SECTION("nonce") {REQUIRE_THROWS(verify_id_token(signer.sign(payload),signer.jwks,c,"wrong",now_seconds()));}
    SECTION("issuer") {payload["iss"]="https://wrong.cloud.honor.com";REQUIRE_THROWS(verify_id_token(signer.sign(payload),signer.jwks,c,"nonce",now_seconds()));}
    SECTION("audience") {payload["aud"]="other-app";REQUIRE_THROWS(verify_id_token(signer.sign(payload),signer.jwks,c,"nonce",now_seconds()));}
    SECTION("multiple audiences") {payload["aud"]=Json::array({c.client_id,"other"});REQUIRE_THROWS(verify_id_token(signer.sign(payload),signer.jwks,c,"nonce",now_seconds()));}
    SECTION("expiry") {payload["exp"]=now_seconds()-1;REQUIRE_THROWS(verify_id_token(signer.sign(payload),signer.jwks,c,"nonce",now_seconds()));}
    SECTION("algorithm confusion") {REQUIRE_THROWS(verify_id_token(signer.sign(payload,"HS256"),signer.jwks,c,"nonce",now_seconds()));}
    SECTION("signature tampering") {auto jwt=signer.sign(payload);auto index=jwt.rfind('.')+1;jwt[index]=jwt[index]=='A'?'B':'A';REQUIRE_THROWS(verify_id_token(jwt,signer.jwks,c,"nonce",now_seconds()));}
    SECTION("unknown key") {auto keys=Json::parse(signer.jwks);keys["keys"][0]["kid"]="rotated";REQUIRE_THROWS(verify_id_token(signer.sign(payload),keys.dump(),c,"nonce",now_seconds()));}
}
TEST_CASE("HONOR refresh errors preserve credentials except documented expiry and blacklist", "[HonorAccount]")
{
    REQUIRE(oauth_failure(400,"{\"sub_error\":11205}",true)==Failure::InvalidCredentials);
    REQUIRE(oauth_failure(200,"{\"sub_error\":31204}",true)==Failure::InvalidCredentials);
    REQUIRE(oauth_failure(400,"{\"sub_error\":12304}",true)==Failure::Configuration);
    REQUIRE(oauth_failure(400,"{\"sub_error\":31202}",true)==Failure::Configuration);
    REQUIRE(oauth_failure(503,"{\"sub_error\":11205}",true)==Failure::Temporary);
    REQUIRE(oauth_failure(0,"",true)==Failure::Temporary);
}
TEST_CASE("HONOR authorization is exchanged once and protected persistence restores across sessions", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto memory=std::make_shared<Memory>();std::string nonce;std::atomic_int posts{0};
    Transport transport=[&](const std::string& url,const std::string& form,Cancellation)->Response {
        if(form.empty())return {200,signer->jwks};++posts;
        if(url!=configuration().token_url)throw std::runtime_error("unexpected fixture URL");
        return signer->response(nonce);
    };
    {
        Session session(std::make_unique<MemoryStore>(memory),transport);REQUIRE(session.configure(configuration()));session.restore();
        REQUIRE(session.snapshot().state==State::SignedOut);
        auto url=session.login();nonce=parameter(url,"nonce");auto state=parameter(url,"state");
        REQUIRE(session.login().empty());
        auto callback=configuration().redirect_uri+"&state="+state+"&code=one";
        REQUIRE(session.callback(callback));REQUIRE(session.callback(callback));finish(session);
        REQUIRE(posts==1);REQUIRE(session.snapshot().state==State::SignedIn);REQUIRE(session.snapshot().persistent);
    }
    Session restored(std::make_unique<MemoryStore>(memory),transport);restored.restore();restored.restore();finish(restored);
    REQUIRE(posts==2);REQUIRE(restored.snapshot().state==State::SignedIn);
    restored.logout();REQUIRE(restored.snapshot().state==State::SignedOut);
    Session reopened(std::make_unique<MemoryStore>(memory),transport);reopened.restore();REQUIRE(reopened.snapshot().state==State::SignedOut);
}
TEST_CASE("HONOR restore distinguishes temporary network loss revoked tokens and bad configuration", "[HonorAccount]")
{
    Signer signer;auto memory=saved(signer.credentials());Response response;
    SECTION("network") {response={0,""};}
    SECTION("revoked") {response={400,"{\"sub_error\":31204}"};}
    SECTION("configuration") {response={400,"{\"sub_error\":12304}"};}
    Session session(std::make_unique<MemoryStore>(memory),[response](const std::string&,const std::string&,Cancellation){return response;});
    session.restore();finish(session);
    if(response.status==0) {REQUIRE(session.snapshot().state==State::Offline);REQUIRE(session.snapshot().has_identity());}
    else if(response.body.find("31204")!=std::string::npos) {REQUIRE(session.snapshot().state==State::SignedOut);REQUIRE_FALSE(session.snapshot().has_identity());}
    else REQUIRE(session.snapshot().state==State::Failed);
    REQUIRE(memory->values.count("session-"+configuration().key())==(session.snapshot().state==State::SignedOut?0:1));
}
TEST_CASE("HONOR logout isolates late refresh results without saving or publishing them", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto memory=saved(signer->credentials());
    std::mutex mutex;std::condition_variable cv;bool entered=false,release=false,returned=false;
    Session session(std::make_unique<MemoryStore>(memory),[&](const std::string&,const std::string&,Cancellation){
        std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return release;});returned=true;cv.notify_all();return signer->response();
    });
    session.restore();
    {std::unique_lock<std::mutex> lock(mutex);REQUIRE(cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;}));}
    session.logout();
    {std::unique_lock<std::mutex> lock(mutex);release=true;cv.notify_all();REQUIRE(cv.wait_for(lock,std::chrono::seconds(2),[&]{return returned;}));}
    session.poll();REQUIRE(session.snapshot().state==State::SignedOut);
    REQUIRE(memory->values.count("session-"+configuration().key())==0);
}
TEST_CASE("HONOR missing system persistence never falls back to plaintext", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto memory=saved(signer->credentials());memory->writable=false;
    Session session(std::make_unique<MemoryStore>(memory),[signer](const std::string&,const std::string& form,Cancellation){return form.empty()?Response{200,signer->jwks}:signer->response();});
    session.restore();finish(session);REQUIRE(session.snapshot().state==State::SignedIn);REQUIRE_FALSE(session.snapshot().persistent);
}
TEST_CASE("HONOR rejects a refresh that switches the subject", "[HonorAccount]")
{
    Signer signer;Client client([&](const std::string&,const std::string& form,Cancellation){return form.empty()?Response{200,signer.jwks}:signer.response("nonce","other-user");});
    REQUIRE_THROWS(client.restore(configuration(),signer.credentials(),std::make_shared<std::atomic_bool>(false)));
}
TEST_CASE("HONOR unchanged settings keep the session and failed logout remains retryable", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto memory=saved(signer->credentials());
    Session session(std::make_unique<MemoryStore>(memory),[signer](const std::string&,const std::string& form,Cancellation){return form.empty()?Response{200,signer->jwks}:signer->response();});
    session.restore();finish(session);REQUIRE(session.configure(configuration()));REQUIRE(session.snapshot().state==State::SignedIn);
    memory->writable=false;session.logout();REQUIRE(session.snapshot().state==State::Failed);REQUIRE(session.snapshot().has_identity());
    memory->writable=true;session.logout();REQUIRE(session.snapshot().state==State::SignedOut);REQUIRE_FALSE(session.snapshot().has_identity());
}
TEST_CASE("HONOR without a refresh token waits for expiry instead of repeating startup validation", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto credentials=signer->credentials();credentials.refresh_token.clear();credentials.expires_at=now_seconds()+30;
    auto memory=saved(credentials);
    Session session(std::make_unique<MemoryStore>(memory),[signer](const std::string&,const std::string&,Cancellation){return Response{200,signer->jwks};});
    session.restore();finish(session);REQUIRE(session.snapshot().state==State::SignedIn);REQUIRE_FALSE(session.poll());
    credentials.expires_at=now_seconds()-1;
    Client client;REQUIRE_THROWS(client.restore(configuration(),credentials,std::make_shared<std::atomic_bool>(false)));
}
TEST_CASE("HONOR ordinary logout retains provisioning without requesting a browser reset", "[HonorAccount]")
{
    Signer signer;auto memory=saved(signer.credentials());
    Session session(std::make_unique<MemoryStore>(memory));
    REQUIRE(session.logout());
    REQUIRE_FALSE(session.browser_reset_pending());
    REQUIRE_FALSE(session.snapshot().has_identity());
    REQUIRE(memory->values.count("session-"+configuration().key())==0);
    REQUIRE(memory->values.at("configuration")==serialize_config(configuration()));
}
TEST_CASE("HONOR full logout keeps its browser reset pending across process restarts", "[HonorAccount]")
{
    Signer signer;auto memory=saved(signer.credentials());
    auto application=std::make_shared<Memory>();application->values["configuration"]=serialize_config(configuration());
    {
        Session session(std::make_unique<MemoryStore>(memory),https_request,std::make_unique<MemoryStore>(application));
        REQUIRE(session.logout(true));
        REQUIRE(session.browser_reset_pending());
        REQUIRE_FALSE(session.snapshot().has_identity());
        REQUIRE(memory->values.count("session-"+configuration().key())==0);
    }
    unsigned requests=0;
    Session reopened(std::make_unique<MemoryStore>(memory),[&](const std::string&,const std::string&,Cancellation) {
        ++requests;return Response{};
    });
    reopened.restore();
    REQUIRE(reopened.snapshot().state==State::SignedOut);
    REQUIRE(reopened.browser_reset_pending());
    REQUIRE(requests==0);
    REQUIRE(reopened.complete_browser_reset());
    REQUIRE_FALSE(reopened.browser_reset_pending());
    REQUIRE(memory->values.at("configuration")==serialize_config(configuration()));
    REQUIRE(application->values.size()==1);
}
TEST_CASE("HONOR interrupted full logout cannot restore credentials before finishing cleanup", "[HonorAccount]")
{
    Signer signer;auto memory=saved(signer.credentials());memory->erasable=false;
    unsigned requests=0;
    auto transport=[&](const std::string&,const std::string&,Cancellation) {++requests;return Response{};};
    {
        Session session(std::make_unique<MemoryStore>(memory),transport);
        REQUIRE_FALSE(session.logout(true));
        REQUIRE(session.browser_reset_pending());
        REQUIRE(memory->values.count("session-"+configuration().key())==1);
    }
    Session reopened(std::make_unique<MemoryStore>(memory),transport);
    reopened.restore();
    REQUIRE(reopened.snapshot().state==State::Failed);
    REQUIRE_FALSE(reopened.snapshot().has_identity());
    REQUIRE_FALSE(reopened.complete_browser_reset());
    REQUIRE(reopened.browser_reset_pending());
    memory->erasable=true;reopened.restore();
    REQUIRE(reopened.snapshot().state==State::SignedOut);
    REQUIRE(reopened.browser_reset_pending());
    REQUIRE(memory->values.count("session-"+configuration().key())==0);
    REQUIRE(requests==0);
    REQUIRE(reopened.complete_browser_reset());
    REQUIRE_FALSE(reopened.browser_reset_pending());
}
TEST_CASE("HONOR cannot report full logout when the cleanup request cannot be persisted", "[HonorAccount]")
{
    auto signer=std::make_shared<Signer>();auto memory=saved(signer->credentials());
    Session session(std::make_unique<MemoryStore>(memory),[signer](const std::string&,const std::string& form,Cancellation) {
        return form.empty()?Response{200,signer->jwks}:signer->response();
    });
    session.restore();finish(session);REQUIRE(session.snapshot().has_identity());
    memory->writable=false;
    REQUIRE_FALSE(session.logout(true));
    REQUIRE(session.snapshot().has_identity());
    REQUIRE(memory->values.count("session-"+configuration().key())==1);
    memory->writable=true;memory->readable=false;
    REQUIRE(session.browser_reset_pending());
    REQUIRE_FALSE(session.complete_browser_reset());
    memory->readable=true;
    REQUIRE(session.logout(true));
    REQUIRE(session.browser_reset_pending());
    REQUIRE_FALSE(session.snapshot().has_identity());
}
TEST_CASE("HONOR unreadable cleanup state blocks restoration without deleting a valid saved session", "[HonorAccount]")
{
    Signer signer;auto memory=saved(signer.credentials());
    unsigned requests=0;
    Session session(std::make_unique<MemoryStore>(memory),[&](const std::string&,const std::string&,Cancellation) {
        ++requests;return Response{};
    });
    memory->readable=false;session.restore();
    REQUIRE(session.snapshot().state==State::Failed);
    REQUIRE_FALSE(session.snapshot().has_identity());
    REQUIRE(memory->values.count("session-"+configuration().key())==1);
    REQUIRE(requests==0);
}
TEST_CASE("HONOR launch profiles share provisioning without sharing account sessions", "[HonorAccount]")
{
    auto app=std::make_shared<Memory>();auto original=std::make_shared<Memory>();
    original->values["configuration"]=serialize_config(configuration());
    original->values["session-"+configuration().key()]="profile-only-credential";
    Transport no_network=[](const std::string&,const std::string&,Cancellation)->Response {throw std::runtime_error("unexpected network");};
    Session trial(std::make_unique<MemoryStore>(original),no_network,std::make_unique<MemoryStore>(app));
    REQUIRE(app->values.size()==1);
    REQUIRE(app->values.at("configuration")==original->values.at("configuration"));
    auto direct_profile=std::make_shared<Memory>();
    Session direct(std::make_unique<MemoryStore>(direct_profile),no_network,std::make_unique<MemoryStore>(app));
    REQUIRE(direct.config().key()==trial.config().key());
    direct.restore();REQUIRE(direct.snapshot().state==State::SignedOut);
    REQUIRE_FALSE(direct.snapshot().has_identity());REQUIRE_FALSE(direct.snapshot().persistent);
    REQUIRE_FALSE(direct.login().empty());direct.cancel();direct.logout();
    REQUIRE(direct_profile->values.empty());
    REQUIRE(original->values.at("session-"+configuration().key())=="profile-only-credential");
    REQUIRE(app->values.size()==1);
}
TEST_CASE("HONOR profile provisioning keeps precedence and never replaces existing application configuration", "[HonorAccount]")
{
    auto profile=std::make_shared<Memory>();auto app=std::make_shared<Memory>();auto other=configuration();other.client_id="271828182";
    profile->values["configuration"]=serialize_config(configuration());
    app->values["configuration"]=serialize_config(other);
    Session session(std::make_unique<MemoryStore>(profile),https_request,std::make_unique<MemoryStore>(app));
    REQUIRE(session.config().client_id==configuration().client_id);
    REQUIRE(app->values.at("configuration")==serialize_config(other));
    auto changed=configuration();changed.client_id="161803398";
    REQUIRE(session.configure(changed));REQUIRE(session.config().client_id==changed.client_id);
    REQUIRE(app->values.at("configuration")==serialize_config(other));
}
TEST_CASE("HONOR unreadable or corrupt configuration never falls through to another account configuration", "[HonorAccount]")
{
    auto profile=std::make_shared<Memory>();auto app=std::make_shared<Memory>();
    app->values["configuration"]=serialize_config(configuration());
    SECTION("profile corrupt") {profile->values["configuration"]="{";}
    SECTION("profile unreadable") {profile->readable=false;}
    SECTION("application corrupt") {app->values["configuration"]="{";}
    SECTION("application unreadable") {app->readable=false;}
    SECTION("profile invalid") {auto invalid=configuration();invalid.client_secret.clear();profile->values["configuration"]=serialize_config(invalid);}
    Session session(std::make_unique<MemoryStore>(profile),https_request,std::make_unique<MemoryStore>(app));
    REQUIRE(session.login().empty());REQUIRE_FALSE(session.snapshot().has_identity());
}
TEST_CASE("HONOR configuration migration failure preserves the profile and configuration rollback", "[HonorAccount]")
{
    auto profile=std::make_shared<Memory>();auto app=std::make_shared<Memory>();
    profile->values["configuration"]=serialize_config(configuration());app->writable=false;
    Session session(std::make_unique<MemoryStore>(profile),https_request,std::make_unique<MemoryStore>(app));
    REQUIRE(session.config().key()==configuration().key());REQUIRE(app->values.empty());
    REQUIRE_FALSE(session.login().empty());session.cancel();
    profile->erasable=false;auto changed=configuration();changed.client_id="271828182";
    REQUIRE_FALSE(session.configure(changed));
    REQUIRE(profile->values.at("configuration")==serialize_config(configuration()));
    REQUIRE(session.config().key()==configuration().key());REQUIRE(app->values.empty());
    profile->erasable=true;app->writable=true;
    REQUIRE(session.configure(changed));REQUIRE(app->values.at("configuration")==serialize_config(changed));
}
#ifdef __WXMSW__
TEST_CASE("HONOR Windows protected storage atomically rotates long UTF8 credentials and tombstones logout", "[HonorAccount]")
{
    auto profile="offline-test-"+random_value();auto store=system_store(profile);
    std::string value;for(int i=0;i<2000;++i)value+="测试";
    REQUIRE(store->save("long",value));REQUIRE(store->load("long")==value);
    REQUIRE(store->save("long",value+"rotated"));REQUIRE(store->load("long")==value+"rotated");
    REQUIRE(store->erase("long"));REQUIRE_FALSE(store->load("long"));
}
#endif
