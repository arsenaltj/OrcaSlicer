#include "HonorServices.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Slic3r::HonorAccount {
namespace {

using Json = nlohmann::json;
using Method = Request::Method;
using Clock = std::chrono::steady_clock;
constexpr size_t max_file_bytes = 100 * 1024 * 1024;
constexpr unsigned max_offset = 9999999;
constexpr unsigned page_size = 100;

[[noreturn]] void invalid()
{
    throw Error(Failure::Protocol, "平台服务响应格式无效");
}

bool file_id(const std::string& value)
{
    static const std::regex pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return value.size() == 36 && std::regex_match(value, pattern);
}

bool checksum(const std::string& value)
{
    static const std::regex pattern("^[0-9a-f]{64}$");
    return value.size() == 64 && std::regex_match(value, pattern);
}

std::string path_for(const std::string& id)
{
    if (!file_id(id)) throw Error(Failure::Protocol, "文件标识无效");
    return "/v1/files/" + id;
}

const Json& field(const Json& value, const char* key)
{
    if (!value.is_object()) invalid();
    const auto found = value.find(key);
    if (found == value.end()) invalid();
    return *found;
}

std::string string(const Json& value)
{
    if (!value.is_string()) invalid();
    return value.get<std::string>();
}

std::string string(const Json& value, const char* key) { return string(field(value, key)); }

std::int64_t integer(const Json& value)
{
    if (!value.is_number_integer() || (value.is_number_unsigned() &&
            value.get<std::uint64_t>() > std::uint64_t((std::numeric_limits<std::int64_t>::max)()))) invalid();
    return value.get<std::int64_t>();
}

std::int64_t nonnegative(const Json& value, const char* key)
{
    const auto number = integer(field(value, key));
    if (number < 0) invalid();
    return number;
}

bool boolean(const Json& value, const char* key)
{
    const auto& item = field(value, key);
    if (!item.is_boolean()) invalid();
    return item.get<bool>();
}

StorageSnapshot read_storage(const Json& value)
{
    const auto& cloud = field(value, "cloud");
    const auto& drafts = field(value, "drafts");
    return {nonnegative(cloud, "limitBytes"), nonnegative(cloud, "usedBytes"), nonnegative(cloud, "reservedBytes"),
            nonnegative(cloud, "availableBytes"), boolean(cloud, "overLimit"),
            nonnegative(field(value, "library"), "usedBytes"), nonnegative(field(value, "derived"), "usedBytes"),
            nonnegative(drafts, "used"), nonnegative(drafts, "limit"), boolean(drafts, "canCreate")};
}

FileMetadata read_file(const Json& value, const std::string& expected = {})
{
    FileMetadata result;
    result.id = string(value, "id");
    result.filename = string(value, "filename");
    result.mime = string(value, "mime");
    result.sha256 = string(value, "sha256");
    result.status = string(value, "status");
    result.created_at = string(value, "createdAt");
    result.size = nonnegative(value, "size");
    if (!file_id(result.id) || (!expected.empty() && expected != result.id) ||
        !checksum(result.sha256) || result.size == 0 ||
        (result.status != "pending" && result.status != "ready" && result.status != "failed" && result.status != "deleted")) invalid();
    const auto& failure = field(value, "failureReason");
    if (!failure.is_null()) result.failure_reason = string(failure);
    if (value.contains("purpose")) result.purpose = string(value, "purpose");
    if (value.contains("spaces")) {
        const auto& spaces = field(value, "spaces");
        if (!spaces.is_array()) invalid();
        for (const auto& space : spaces) result.spaces.push_back(string(space));
    }
    if (value.contains("actions")) {
        const auto& actions = field(value, "actions");
        result.actions = FileMetadata::Actions{boolean(actions, "downloadOriginal"), boolean(actions, "downloadPreview"),
                                              boolean(actions, "saveToCloud"), boolean(actions, "shareToLibrary")};
    }
    if (value.contains("preview")) {
        const auto& preview = field(value, "preview");
        FileMetadata::Preview details{string(preview, "state"), {}, {}};
        const auto& sha = field(preview, "sha256");
        const auto& size = field(preview, "sizeBytes");
        if (!sha.is_null()) details.sha256 = string(sha);
        if (!size.is_null()) details.size = nonnegative(preview, "sizeBytes");
        result.preview = std::move(details);
    }
    return result;
}

void expect_success(const Json& value)
{
    if (!boolean(value, "ok")) invalid();
}

bool header_value(const std::string& value)
{
    return !value.empty() && value.size() < 256 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch < 0x7f;
    });
}

std::string ticket_url(const std::string& api, const std::string& value, const std::string& path, bool allow_external)
{
    if (value.empty() || value.find('#') != std::string::npos) invalid();
    if (value.front() == '/') {
        const auto prefix = path + "?ticket=";
        static const std::regex token("^[A-Za-z0-9_-]{43}$");
        if (value.size() != prefix.size() + 43 || value.compare(0, prefix.size(), prefix) != 0 ||
            !std::regex_match(value.substr(prefix.size()), token)) invalid();
        return api + value;
    }
    if (!allow_external || !valid_authorization_url(value)) invalid();
    return value;
}

Clock::time_point expiration(const Json& value, Clock::time_point start, std::int64_t maximum)
{
    const auto seconds = nonnegative(value, "expiresIn");
    if (seconds == 0 || seconds > maximum) invalid();
    return start + std::chrono::seconds(seconds);
}

void check_ticket(const Cancellation& expected, const Cancellation& provided, Clock::time_point expires)
{
    if (provided != expected || !provided || provided->load())
        throw Error(Failure::Cancelled, "票据不属于当前登录会话");
    if (Clock::now() >= expires)
        throw Error(Failure::Protocol, "文件票据已过期", 0, "TICKET_EXPIRED");
}

} // namespace

Services::Services(SessionAccess access, Transport transport)
    : m_access(std::move(access)), m_transport(std::move(transport))
{
    m_config.api_url = m_access.api_url;
}

void Services::check(const Cancellation& cancel) const
{
    if (m_access.subject.empty() || m_access.access_token.empty() || !m_access.lifetime)
        throw Error(Failure::InvalidCredentials, "请先登录平台账号");
    if (m_access.lifetime->load() || (cancel && cancel->load()))
        throw Error(Failure::Cancelled, "请求已取消或账号已切换");
}

Json Services::json(Method method, const std::string& path, const Cancellation& cancel, const std::string& body)
{
    check(cancel);
    const auto response = Client(m_transport).request(m_config, method, path, body, m_access.access_token, cancel, m_access.lifetime);
    if (response.status != (method == Method::Post ? 201u : 200u)) response_error(response.status, response.body);
    auto parsed = Json::parse(response.body, nullptr, false);
    if (parsed.is_discarded()) invalid();
    return parsed;
}

Response Services::transfer(Request request, const Cancellation& cancel)
{
    check(cancel);
    request.account_lifetime = m_access.lifetime;
    auto response = m_transport(request, cancel ? cancel : std::make_shared<std::atomic_bool>(false));
    check(cancel);
    return response;
}

PointBalance Services::points(Cancellation cancel)
{
    const auto response = json(Method::Get, "/v1/me/points", cancel);
    PointBalance result{integer(field(response, "balance")), {}};
    const auto& entries = field(response, "entries");
    if (!entries.is_array()) invalid();
    for (const auto& entry : entries)
        result.entries.push_back({string(entry, "id"), string(entry, "kind"), string(entry, "reason"),
                                  string(entry, "created_at"), integer(field(entry, "amount"))});
    return result;
}

StorageSnapshot Services::storage(Cancellation cancel)
{
    return read_storage(json(Method::Get, "/v1/me/storage", cancel));
}

CloudFilePage Services::list_cloud(unsigned offset, Cancellation cancel)
{
    if (offset > max_offset) throw Error(Failure::Protocol, "云文件列表偏移量无效");
    const auto response = json(Method::Get, "/v1/files?space=cloud&offset=" + std::to_string(offset), cancel);
    if (!response.is_array() || response.size() > page_size) invalid();
    CloudFilePage page;
    for (const auto& item : response) page.files.push_back(read_file(item));
    if (page.files.size() == page_size && offset <= max_offset - page_size) page.next_offset = offset + page_size;
    return page;
}

FileMetadata Services::file(const std::string& id, Cancellation cancel)
{
    return read_file(json(Method::Get, path_for(id), cancel), id);
}

UploadSession Services::create_upload(const UploadInput& input, Cancellation cancel)
{
    check(cancel);
    if (!input.bytes || input.bytes->empty() || input.bytes->size() > max_file_bytes ||
        input.filename.empty() || input.filename.size() > 180 || !header_value(input.mime) ||
        (input.purpose != "model" && input.purpose != "image" && input.purpose != "attachment" && input.purpose != "print"))
        throw Error(Failure::Protocol, "云文件上传参数无效");
    UploadSession result;
    result.m_size = input.bytes->size();
    result.m_sha256 = digest(*input.bytes);
    const auto start = Clock::now();
    const auto response = json(Method::Post, "/v1/files/upload-sessions", cancel,
        Json{{"targetSpace", "cloud"}, {"purpose", input.purpose}, {"filename", input.filename},
             {"mime", input.mime}, {"size", result.m_size}, {"sha256", result.m_sha256}}.dump());
    result.m_id = string(response, "id");
    result.m_url = ticket_url(m_config.api_url, string(response, "uploadUrl"), path_for(result.m_id) + "/upload", true);
    const auto& headers = field(response, "headers");
    if (string(response, "method") != "PUT" || !headers.is_object()) invalid();
    for (auto header = headers.begin(); header != headers.end(); ++header) {
        auto name = header.key();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        auto value = string(*header);
        if (name != "content-type" || !header_value(value) || !result.m_headers.empty()) invalid();
        result.m_headers.emplace_back(std::move(name), std::move(value));
    }
    if (result.m_headers.empty()) invalid();
    result.m_lifetime = m_access.lifetime;
    result.m_expires = expiration(response, start, 600);
    return result;
}

void Services::put_upload(const UploadSession& session, const std::shared_ptr<const std::string>& bytes, Cancellation cancel)
{
    check(cancel);
    check_ticket(m_access.lifetime, session.m_lifetime, session.m_expires);
    if (!bytes || bytes->size() != session.m_size || digest(*bytes) != session.m_sha256)
        throw Error(Failure::Protocol, "待上传文件已变化，请创建新的上传会话");
    Request request;
    request.method = Method::Put;
    request.url = session.m_url;
    request.headers = session.m_headers;
    request.upload = bytes;
    const auto response = transfer(std::move(request), cancel);
    if (response.status < 200 || response.status >= 300) response_error(response.status, response.body);
}

FileMetadata Services::complete_upload(const std::string& id, Cancellation cancel)
{
    return read_file(json(Method::Post, path_for(id) + "/complete", cancel), id);
}

void Services::cancel_upload(const std::string& id, Cancellation cancel)
{
    expect_success(json(Method::Post, path_for(id) + "/cancel-upload", cancel));
}

std::string Services::preview_glb(const std::string& id, Cancellation cancel)
{
    check(cancel);
    auto response = Client(m_transport).request(m_config, Method::Get, path_for(id) + "/preview.glb", {},
        m_access.access_token, cancel, m_access.lifetime, max_file_bytes, "model/gltf-binary");
    if (response.status != 200) response_error(response.status, response.body);
    if (response.body.size() < 12 || response.body.compare(0, 8, "glTF\x02\0\0\0", 8) != 0) invalid();
    const auto* length = reinterpret_cast<const unsigned char*>(response.body.data() + 8);
    const auto size = std::uint32_t(length[0]) | (std::uint32_t(length[1]) << 8) |
                      (std::uint32_t(length[2]) << 16) | (std::uint32_t(length[3]) << 24);
    if (size != response.body.size()) invalid();
    return std::move(response.body);
}

void Services::retry_preview(const std::string& id, Cancellation cancel)
{
    expect_success(json(Method::Post, path_for(id) + "/preview/retry", cancel));
}

DownloadTicket Services::download_ticket(const std::string& id, Cancellation cancel)
{
    DownloadTicket ticket;
    ticket.m_file = file(id, cancel);
    if (ticket.m_file.status != "ready" || std::uint64_t(ticket.m_file.size) > max_file_bytes)
        throw Error(Failure::Protocol, "原件未就绪或超过文件大小限制");
    const auto start = Clock::now();
    const auto path = path_for(id);
    const auto response = json(Method::Get, path + "/download-ticket", cancel);
    ticket.m_url = ticket_url(m_config.api_url, string(response, "url"), path + "/download", false);
    ticket.m_expires = expiration(response, start, 120);
    ticket.m_lifetime = m_access.lifetime;
    return ticket;
}

OriginalFile Services::download(const DownloadTicket& ticket, Cancellation cancel)
{
    check(cancel);
    check_ticket(m_access.lifetime, ticket.m_lifetime, ticket.m_expires);
    Request request;
    request.url = ticket.m_url;
    request.accept = "*/*";
    request.response_limit = std::max(size_t(ticket.m_file.size), size_t(65536));
    auto response = transfer(std::move(request), cancel);
    if (response.status != 200) response_error(response.status, response.body);
    if (response.body.size() != size_t(ticket.m_file.size) || digest(response.body) != ticket.m_file.sha256)
        throw Error(Failure::Protocol, "下载原件的大小或 SHA-256 校验失败", 200, "DOWNLOAD_INTEGRITY_FAILED");
    check(cancel);
    return {ticket.m_file, std::move(response.body)};
}

CloudSave Services::save_to_cloud(const std::string& id, Cancellation cancel)
{
    const auto response = json(Method::Post, path_for(id) + "/save-to-cloud", cancel);
    return {read_file(field(response, "file"), id), read_storage(field(response, "storage"))};
}

CloudRemoval Services::remove_cloud_reference(const std::string& id, Cancellation cancel)
{
    path_for(id);
    const auto response = json(Method::Delete, "/v1/cloud/files/" + id, cancel);
    expect_success(response);
    return {boolean(response, "removed"), read_storage(field(response, "storage"))};
}

} // namespace Slic3r::HonorAccount
