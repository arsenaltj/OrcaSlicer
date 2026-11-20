#include "slic3r/Utils/HonorAccount/HonorServices.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r::HonorAccount;

namespace {

using Json = nlohmann::json;
using Method = Request::Method;
const std::string origin = "https://platform.example.test";
const std::string id = "22222222-2222-4222-8222-222222222222";
const std::string file_path = "/v1/files/" + id;
const std::string ticket_query = "?ticket=" + std::string(43, 'T');
const std::string payload = "original file payload";
const std::string storage_payload = R"({"cloud":{"limitBytes":100,"usedBytes":20,"reservedBytes":5,"availableBytes":75,"overLimit":false},"library":{"usedBytes":500},"derived":{"usedBytes":40},"drafts":{"used":1,"limit":10,"canCreate":true}})";

SessionAccess lease(const std::string& subject = "account-one")
{
    return {origin, "fixture-token", subject, 1, std::make_shared<std::atomic_bool>(false)};
}

Json file_response(const std::string& status = "ready", Json failure = nullptr)
{
    return {{"id", id}, {"filename", "model.stl"}, {"mime", "model/stl"}, {"size", payload.size()},
            {"sha256", digest(payload)}, {"status", status}, {"createdAt", "2026-11-20T00:00:00Z"},
            {"failureReason", std::move(failure)}, {"purpose", "model"}, {"metadata", Json::object()}};
}

Response download_ticket_response()
{
    return {200, Json{{"url", file_path + "/download" + ticket_query}, {"expiresIn", 120}}.dump()};
}

Response upload_response(const std::string& url)
{
    return {201, Json{{"id", id}, {"uploadUrl", url}, {"method", "PUT"},
                      {"headers", {{"content-type", "model/stl"}}}, {"expiresIn", 600}}.dump()};
}

} // namespace

TEST_CASE("Points distinguish zero balances from failed and malformed responses", "[HonorServices]")
{
    Response reply{200, R"({"balance":0,"entries":[{"id":"entry","amount":-25,"kind":"spend","reason":"fixture","created_at":"2026-11-20T00:00:00Z"}]})"};
    Services service(lease(), [&](const Request& request, Cancellation) {
        REQUIRE(request.url == origin + "/v1/me/points");
        REQUIRE(request.bearer == "fixture-token");
        REQUIRE(request.headers == desktop_client_headers());
        return reply;
    });
    const auto points = service.points();
    REQUIRE(points.balance == 0);
    REQUIRE(points.entries.size() == 1);
    REQUIRE(points.entries.front().amount == -25);
    reply = {503, R"({"error":{"code":"SERVICE_UNAVAILABLE"}})"};
    REQUIRE_THROWS_AS(service.points(), Error);
    reply = {200, R"({"entries":[]})"};
    REQUIRE_THROWS_AS(service.points(), Error);
}

TEST_CASE("Cloud storage and file pages keep backend quota and pagination semantics", "[HonorServices]")
{
    Services service(lease(), [&](const Request& request, Cancellation) {
        if (request.url == origin + "/v1/me/storage") return Response{200, storage_payload};
        if (request.url == origin + "/v1/files?space=cloud&offset=13") {
            auto page = Json::array();
            auto item = file_response();
            item["spaces"] = {"cloud"};
            item["actions"] = {{"downloadOriginal", true}, {"downloadPreview", false},
                               {"saveToCloud", false}, {"shareToLibrary", true}};
            item["preview"] = {{"state", "pending"}, {"url", nullptr}, {"sha256", nullptr}, {"sizeBytes", nullptr}};
            for (unsigned index = 0; index < 100; ++index) page.push_back(item);
            return Response{200, page.dump()};
        }
        REQUIRE(request.url == origin + "/v1/files?space=cloud&offset=113");
        return Response{200, "[]"};
    });
    const auto storage = service.storage();
    REQUIRE(storage.cloud_reserved == 5);
    REQUIRE(storage.cloud_available == 75);
    REQUIRE(storage.library_used == 500);
    const auto page = service.list_cloud(13);
    REQUIRE(page.files.size() == 100);
    REQUIRE(page.next_offset == 113);
    REQUIRE(page.files.front().spaces == std::vector<std::string>{"cloud"});
    REQUIRE(page.files.front().actions->download_original);
    REQUIRE_FALSE(page.files.front().actions->download_preview);
    REQUIRE(page.files.front().preview->state == "pending");
    REQUIRE_FALSE(service.list_cloud(*page.next_offset).next_offset);
}

TEST_CASE("Interrupted cloud uploads retain the original file id for reconciliation and cancellation", "[HonorServices]")
{
    const bool external = GENERATE(false, true);
    const auto upload_url = external ? "https://objects.example.test/staging?signature=fixture" : file_path + "/upload" + ticket_query;
    const auto bytes = std::make_shared<const std::string>(payload);
    bool interrupted = true, cancelled = false;
    unsigned created = 0, transferred = 0;
    const auto transport = [&](const Request& request, Cancellation) {
        if (request.url == origin + "/v1/files/upload-sessions") {
            ++created;
            REQUIRE(request.method == Method::Post);
            REQUIRE(request.bearer == "fixture-token");
            const auto body = Json::parse(request.json);
            REQUIRE(body.at("targetSpace") == "cloud");
            REQUIRE(body.at("sha256") == digest(payload));
            REQUIRE(body.at("size") == bytes->size());
            return upload_response(upload_url);
        }
        if (request.method == Method::Put) {
            ++transferred;
            REQUIRE(request.url == (external ? upload_url : origin + upload_url));
            REQUIRE(request.bearer.empty());
            REQUIRE((request.headers == Headers{{"content-type", "model/stl"}}));
            REQUIRE(*request.upload == *bytes);
            if (interrupted) throw Error(Failure::Temporary, "connection interrupted");
            return Response{external ? 204u : 200u, {}};
        }
        if (request.url == origin + file_path + "/complete") {
            REQUIRE(request.method == Method::Post);
            return Response{201, file_response().dump()};
        }
        if (request.url == origin + file_path + "/cancel-upload") {
            REQUIRE(request.method == Method::Post);
            cancelled = true;
            return Response{201, R"({"ok":true})"};
        }
        REQUIRE(request.url == origin + file_path);
        REQUIRE(request.method == Method::Get);
        return Response{200, cancelled ? file_response("failed", "UPLOAD_CANCELED").dump() : file_response("pending").dump()};
    };
    const auto account = lease();
    Services service(account, transport);
    Services other(lease("account-two"), transport);
    const auto upload = service.create_upload({"model.stl", "model/stl", bytes});
    REQUIRE_THROWS_AS(other.put_upload(upload, bytes), Error);
    REQUIRE_THROWS_AS(service.put_upload(upload, bytes), Error);
    REQUIRE(service.file(upload.file_id()).status == "pending");
    REQUIRE(created == 1);
    REQUIRE(transferred == 1);
    SECTION("reconciling an upload that reached the server") {
        REQUIRE(service.complete_upload(upload.file_id()).status == "ready");
    }
    SECTION("cancelling an unfinished upload") {
        const auto stop = std::make_shared<std::atomic_bool>(true);
        REQUIRE_THROWS_AS(service.put_upload(upload, bytes, stop), Error);
        service.cancel_upload(upload.file_id());
        REQUIRE(service.file(upload.file_id()).failure_reason == "UPLOAD_CANCELED");
    }
    SECTION("sending bytes with a valid reserved upload ticket") {
        interrupted = false;
        REQUIRE_NOTHROW(service.put_upload(upload, bytes));
        REQUIRE(transferred == 2);
    }
    account.lifetime->store(true);
    REQUIRE_THROWS_AS(service.file(upload.file_id()), Error);
}

TEST_CASE("Cloud tickets cannot redirect credentials or change the intended file", "[HonorServices]")
{
    const std::string target = GENERATE("http://objects.example.test/", "https://user@objects.example.test/",
        "//objects.example.test/", "/v1/files/wrong/upload?ticket=invalid");
    Services service(lease(), [&](const Request&, Cancellation) { return upload_response(target); });
    REQUIRE_THROWS_AS(service.create_upload({"model.stl", "model/stl", std::make_shared<const std::string>(payload)}), Error);
}

TEST_CASE("Original downloads expose bytes only after validating the ticket and file integrity", "[HonorServices]")
{
    const auto reply = GENERATE(200, 403);
    const auto actual_bytes = GENERATE(std::string(payload), std::string(payload.size(), 'X'), std::string("short"));
    unsigned downloads = 0;
    Services service(lease(), [&](const Request& request, Cancellation) {
        if (request.url == origin + file_path) return Response{200, file_response().dump()};
        if (request.url == origin + file_path + "/download-ticket") return download_ticket_response();
        REQUIRE(request.url == origin + file_path + "/download" + ticket_query);
        REQUIRE(request.bearer.empty());
        REQUIRE(request.headers.empty());
        ++downloads;
        return reply == 403 ? Response{403, R"({"error":{"code":"DOWNLOAD_TICKET_INVALID"}})"}
                            : Response{200, actual_bytes};
    });
    const auto ticket = service.download_ticket(id);
    Services other(lease("account-two"));
    REQUIRE_THROWS_AS(other.download(ticket), Error);
    REQUIRE(downloads == 0);
    if (reply == 200 && actual_bytes == payload) REQUIRE(service.download(ticket).bytes == payload);
    else REQUIRE_THROWS_AS(service.download(ticket), Error);
    REQUIRE(downloads == 1);
}

TEST_CASE("A cloud response is discarded when the active account changes during transport", "[HonorServices]")
{
    const auto account = lease();
    Services service(account, [&](const Request& request, Cancellation) {
        REQUIRE(request.account_lifetime == account.lifetime);
        account.lifetime->store(true);
        return Response{200, R"({"balance":0,"entries":[]})"};
    });
    REQUIRE_THROWS_AS(service.points(), Error);
}

TEST_CASE("Cloud preview and reference management use their dedicated routes", "[HonorServices]")
{
    const std::string glb("glTF\x02\0\0\0\x0c\0\0\0", 12);
    Services service(lease(), [&](const Request& request, Cancellation) {
        REQUIRE(request.bearer == "fixture-token");
        if (request.url == origin + file_path + "/preview.glb") {
            REQUIRE(request.method == Method::Get);
            return Response{200, glb};
        }
        if (request.url == origin + file_path + "/preview/retry") {
            REQUIRE(request.method == Method::Post);
            return Response{201, R"({"ok":true})"};
        }
        if (request.url == origin + file_path + "/save-to-cloud") {
            REQUIRE(request.method == Method::Post);
            return Response{201, Json{{"file", file_response()}, {"storage", Json::parse(storage_payload)}}.dump()};
        }
        REQUIRE(request.url == origin + "/v1/cloud/files/" + id);
        REQUIRE(request.method == Method::Delete);
        return Response{200, Json{{"ok", true}, {"removed", true}, {"storage", Json::parse(storage_payload)}}.dump()};
    });
    REQUIRE(service.preview_glb(id) == glb);
    REQUIRE_NOTHROW(service.retry_preview(id));
    REQUIRE(service.save_to_cloud(id).file.id == id);
    REQUIRE(service.remove_cloud_reference(id).removed);
}
