#pragma once

#include "HonorClient.hpp"

#include <chrono>
#include <nlohmann/json_fwd.hpp>

namespace Slic3r::HonorAccount {

struct PointEntry {
    std::string id, kind, reason, created_at;
    std::int64_t amount;
};

struct PointBalance {
    std::int64_t balance;
    std::vector<PointEntry> entries;
};

struct StorageSnapshot {
    std::int64_t cloud_limit, cloud_used, cloud_reserved, cloud_available;
    bool cloud_over_limit;
    std::int64_t library_used, derived_used, drafts_used, drafts_limit;
    bool drafts_can_create;
};

struct FileMetadata {
    std::string id, filename, mime, sha256, status, created_at;
    std::optional<std::string> failure_reason;
    std::int64_t size;
    std::string purpose;
    std::vector<std::string> spaces;
    struct Actions {
        bool download_original, download_preview, save_to_cloud, share_to_library;
    };
    struct Preview {
        std::string state;
        std::optional<std::string> sha256;
        std::optional<std::int64_t> size;
    };
    std::optional<Actions> actions;
    std::optional<Preview> preview;
};

struct CloudFilePage {
    std::vector<FileMetadata> files;
    std::optional<unsigned> next_offset;
};

struct UploadInput {
    std::string filename, mime;
    std::shared_ptr<const std::string> bytes;
    std::string purpose {"model"};
};

struct CloudSave { FileMetadata file; StorageSnapshot storage; };
struct CloudRemoval { bool removed; StorageSnapshot storage; };
struct OriginalFile { FileMetadata file; std::string bytes; };

class Services;
class UploadSession {
public:
    const std::string& file_id() const { return m_id; }
private:
    friend class Services;
    std::string m_id, m_url, m_sha256;
    Headers m_headers;
    size_t m_size {0};
    std::chrono::steady_clock::time_point m_expires;
    Cancellation m_lifetime;
};

class DownloadTicket {
public:
    const FileMetadata& file() const { return m_file; }
private:
    friend class Services;
    FileMetadata m_file;
    std::string m_url;
    std::chrono::steady_clock::time_point m_expires;
    Cancellation m_lifetime;
};

class Services {
public:
    Services(SessionAccess, Transport = https_request);
    PointBalance points(Cancellation = {});
    StorageSnapshot storage(Cancellation = {});
    CloudFilePage list_cloud(unsigned offset = 0, Cancellation = {});
    FileMetadata file(const std::string& id, Cancellation = {});
    UploadSession create_upload(const UploadInput&, Cancellation = {});
    void put_upload(const UploadSession&, const std::shared_ptr<const std::string>& bytes, Cancellation = {});
    FileMetadata complete_upload(const std::string& id, Cancellation = {});
    void cancel_upload(const std::string& id, Cancellation = {});
    std::string preview_glb(const std::string& id, Cancellation = {});
    void retry_preview(const std::string& id, Cancellation = {});
    DownloadTicket download_ticket(const std::string& id, Cancellation = {});
    OriginalFile download(const DownloadTicket&, Cancellation = {});
    CloudSave save_to_cloud(const std::string& id, Cancellation = {});
    CloudRemoval remove_cloud_reference(const std::string& id, Cancellation = {});

private:
    void check(const Cancellation&) const;
    nlohmann::json json(Request::Method, const std::string& path, const Cancellation&, const std::string& body = {});
    Response transfer(Request, const Cancellation&);
    SessionAccess m_access;
    Transport m_transport;
    Config m_config;
};

} // namespace Slic3r::HonorAccount
