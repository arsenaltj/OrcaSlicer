#pragma once
#include "slic3r/Utils/HonorAccount/HonorClient.hpp"
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace Slic3r::PlatformLibrary {
using Json = nlohmann::json;
using Cancellation = HonorAccount::Cancellation;
struct Identity { std::string api, bearer, subject; std::uint64_t generation {0}; };
struct Error : std::runtime_error {
    unsigned status;
    Error(unsigned value, const std::string& message) : std::runtime_error(message), status(value) {}
};
bool uuid(const std::string&);
bool supported_format(const std::string&);
std::string file_digest(const boost::filesystem::path&, const std::atomic<bool>* cancel = nullptr);
std::string escaped(const std::string&);
// Only platform-issued relative media/preview paths can cross this seam.
bool preview_path(const std::string&);
struct Binary { unsigned status {0}; std::string bytes, mime; };
Binary transfer(const Identity&, const std::string& path, const std::string& method,
    const std::string& json, Cancellation, std::size_t limit,
    std::function<bool(const char*, std::size_t)> sink = {},
    std::function<void(std::size_t, std::size_t)> progress = {});
using BinaryTransport=std::function<Binary(const Identity&,const std::string&,const std::string&,
    const std::string&,Cancellation,std::size_t,std::function<bool(const char*,std::size_t)>,
    std::function<void(std::size_t,std::size_t)>)>;
class Client {
public:
    explicit Client(Identity identity, BinaryTransport transport=transfer) : m_identity(std::move(identity)),m_transfer(std::move(transport)) {}
    Json query(const std::string& path, Cancellation, const std::string& method="GET", const Json& body=nullptr) const;
    Json list(const Json& filters, Cancellation) const;
    Json detail(const std::string& id, int version, Cancellation) const;
    Json activity(const std::string& kind, const Json& filters, Cancellation) const;
    Json owned_models(Cancellation) const;
    // Account-scoped, in-memory source collections; filtering and paging follow normalization.
    Json personal(const std::string& filter, const Json& filters, Cancellation, Json cache=Json::object()) const;
    Json presentation_detail(const std::string& id, int version, const std::string& visitor, Cancellation) const;
    Json toggle(const std::string& id, const std::string& kind, bool active, Cancellation) const;
    std::string image(const std::string& relative, Cancellation) const;
    Binary bytes(const std::string& path,Cancellation cancel,std::size_t limit,
        std::function<bool(const char*,std::size_t)> sink={},std::function<void(std::size_t,std::size_t)> progress={}) const {
        return m_transfer(m_identity,path,"GET","",std::move(cancel),limit,std::move(sink),std::move(progress));
    }
    const Identity& identity() const { return m_identity; }
private:
    Identity m_identity;
    BinaryTransport m_transfer;
};
struct DownloadSpec {
    std::string model, version, file, sha256, filename, title;
    int version_number {0};
    std::uint64_t size {0};
    static DownloadSpec from_detail(const Json&, const std::string& file);
    void validate() const;
    std::string extension() const;
    std::string key() const;
};
// Worker prepares bytes only. The GUI commits after rechecking account/request identity.
class StagedDownload {
public:
    ~StagedDownload();
    StagedDownload(const StagedDownload&)=delete;
    StagedDownload& operator=(const StagedDownload&)=delete;
    static std::shared_ptr<StagedDownload> prepare(const Client&, const DownloadSpec&,
        const boost::filesystem::path& root, Cancellation,
        std::function<void(std::size_t,std::size_t)> progress = {},
        std::function<std::uintmax_t(const boost::filesystem::path&)> available_bytes = {});
    std::string commit(Cancellation);
private:
    StagedDownload()=default;
    DownloadSpec m_spec;
    boost::filesystem::path m_root, m_staging, m_final;
    bool m_reused {false}, m_committed {false};
};
}
