#include "HonorSession.hpp"
#include <wx/secretstore.h>
#include <nlohmann/json.hpp>

namespace Slic3r::HonorAccount {
namespace {
class SystemStore final : public Store {
public:
    explicit SystemStore(const std::string& profile):m_prefix("OrcaSlicer/HONOR/"+digest(profile)+"/") {}
    std::optional<std::string> load(const std::string& key) override
    {
        auto index=read(key);if(!index)return {};
        auto j=nlohmann::json::parse(*index);const auto count=j.at("chunks").get<unsigned>();
        if(count>64)throw Error(Failure::Protocol,"本机安全凭据不完整");
        if(!count)return {};
        const auto generation=j.at("generation").get<std::string>();
        if(generation.size()!=64)throw Error(Failure::Protocol,"本机安全凭据不完整");
        std::string value;
        for(unsigned i=0;i<count;++i) {
            auto part=read(key+"/"+generation+"/"+std::to_string(i));
            if(!part)throw Error(Failure::Protocol,"本机安全凭据不完整");
            value+=*part;
        }
        return value;
    }
    bool save(const std::string& key,const std::string& value) override
    {
        // Windows Credential Manager caps each blob. Split UTF-8 as opaque
        // bytes, then atomically replace the small index only after all chunks
        // were saved. A failed rotation keeps the previous complete credential.
        const auto count=(value.size()+1799)/1800;
        if(count>64)return false;
        const auto previous=read(key);const auto generation=random_value();
        for(size_t i=0;i<count;++i)if(!write(key+"/"+generation+"/"+std::to_string(i),value.substr(i*1800,1800))) {
            cleanup(key,nlohmann::json{{"chunks",i},{"generation",generation}}.dump());return false;
        }
        const auto index=nlohmann::json{{"chunks",count},{"generation",generation}}.dump();
        if(!write(key,index)) {cleanup(key,index);return false;}
        if(previous)cleanup(key,*previous);
        return true;
    }
    bool erase(const std::string& key) override
    {
        auto previous=read(key);
        if(!previous)return wxSecretStore::GetDefault().IsOk();
        // Tombstone first: chunk deletion can fail without restoring an account.
        if(!write(key,"{\"chunks\":0,\"generation\":\"\"}"))return false;
        cleanup(key,*previous);return true;
    }
private:
    std::string m_prefix;
    std::optional<std::string> read(const std::string& key)
    {
        auto store=wxSecretStore::GetDefault();if(!store.IsOk())return {};
        wxString user;wxSecretValue secret;
        if(!store.Load(wxString::FromUTF8(m_prefix+key),user,secret))return {};
        return std::string(static_cast<const char*>(secret.GetData()),secret.GetSize());
    }
    bool write(const std::string& key,const std::string& value)
    {
        auto store=wxSecretStore::GetDefault();
        return store.IsOk()&&store.Save(wxString::FromUTF8(m_prefix+key),"HONOR",wxSecretValue(value.size(),value.data()));
    }
    void cleanup(const std::string& key,const std::string& index)
    {
        try {
            auto j=nlohmann::json::parse(index);const auto count=j.at("chunks").get<unsigned>();
            const auto generation=j.at("generation").get<std::string>();if(count>64||generation.size()!=64)return;
            auto store=wxSecretStore::GetDefault();
            for(unsigned i=0;i<count;++i)store.Delete(wxString::FromUTF8(m_prefix+key+"/"+generation+"/"+std::to_string(i)));
        } catch(...) {} // The active index was already committed.
    }
};
class ConfigurationStore final : public Store {
public:
    std::optional<std::string> load(const std::string& key) override
    { return key=="configuration"?m_store.load(key):std::nullopt; }
    bool save(const std::string& key,const std::string& value) override
    { return key=="configuration" && m_store.save(key,value); }
    bool erase(const std::string& key) override
    { return key=="configuration" && m_store.erase(key); }
private:
    // Per OS user, independent of datadir spelling or launch arguments. Keep
    // the old profile namespaces unchanged so existing sessions remain intact.
    SystemStore m_store {"application-configuration/v1"};
};
}
std::unique_ptr<Store> system_store(const std::string& profile) { return std::make_unique<SystemStore>(profile); }
std::unique_ptr<Store> application_configuration_store() { return std::make_unique<ConfigurationStore>(); }
} // namespace Slic3r::HonorAccount
