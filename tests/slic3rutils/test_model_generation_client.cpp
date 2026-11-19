#include <catch2/catch_test_macros.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "slic3r/GUI/AIModelGenerationClient.hpp"

using Slic3r::GUI::AIModelGenerationClient;

namespace {

// An in-process loopback fixture exercises the production HTTP callback. It is
// never installed or used for desktop/provider acceptance.
class CatalogReplyServer
{
public:
    CatalogReplyServer(std::string body, bool disconnect = false)
        : m_acceptor(m_io, {boost::asio::ip::make_address("127.0.0.1"), 0})
    {
        using Socket = boost::asio::ip::tcp::socket;
        m_acceptor.async_accept([this, body = std::move(body), disconnect](boost::system::error_code ec, Socket socket) {
            if (ec) return;
            auto connection = std::make_shared<Socket>(std::move(socket));
            auto buffer = std::make_shared<boost::asio::streambuf>();
            boost::asio::async_read_until(*connection, *buffer, "\r\n\r\n",
                [connection, buffer, body, disconnect](boost::system::error_code read_error, size_t) {
                    if (read_error || disconnect) return;
                    auto response = std::make_shared<std::string>(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body);
                    boost::asio::async_write(*connection, boost::asio::buffer(*response),
                        [connection, response](boost::system::error_code, size_t) {});
                });
        });
        m_thread = std::thread([this] { m_io.run(); });
    }

    ~CatalogReplyServer() { m_io.stop(); m_thread.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(m_acceptor.local_endpoint().port()); }

private:
    boost::asio::io_context m_io;
    boost::asio::ip::tcp::acceptor m_acceptor;
    std::thread m_thread;
};

struct CatalogCallbacks
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned completed {0};
    unsigned failed {0};
    std::string error;
    bool ready {false};

};

std::shared_ptr<CatalogCallbacks> observe_catalog(AIModelGenerationClient& client)
{
    // Shared ownership protects the timeout path from a late HTTP callback
    // addressing a destroyed test stack frame.
    auto calls = std::make_shared<CatalogCallbacks>();
    client.get_provider_catalog("geometry", "yoyoclaw",
        [calls](AIModelGenerationClient::ProviderCatalog catalog) {
            {
                std::lock_guard<std::mutex> lock(calls->mutex);
                ++calls->completed;
                calls->ready = catalog.ready();
            }
            calls->changed.notify_all();
        },
        [calls](std::string error) {
            {
                std::lock_guard<std::mutex> lock(calls->mutex);
                ++calls->failed;
                calls->error = std::move(error);
            }
            calls->changed.notify_all();
        });
    return calls;
}

} // namespace

TEST_CASE("Invalid successful provider responses complete through exactly one error callback",
          "[ModelGenerationClient][ProviderCatalogCallback]")
{
    const nlohmann::json valid {{"role", "geometry"}, {"provider", "yoyoclaw"}, {"state", "empty"},
                               {"models", nlohmann::json::array()}, {"error", nullptr}};
    std::vector<std::string> bodies {"{"};
    for (const std::string field : {"role", "provider", "state", "models", "error", "default_model", "connection_state"}) {
        auto malformed = valid;
        malformed[field] = 42;
        bodies.push_back(malformed.dump());
    }
    auto malformed = valid;
    malformed["models"] = nlohmann::json::array({{{"id", "model"}, {"name", "model"}, {"available", "yes"}}});
    bodies.push_back(malformed.dump());
    for (const auto& body : bodies) {
        DYNAMIC_SECTION(body) {
            CatalogReplyServer server(body);
            AIModelGenerationClient client(server.url());
            const auto calls = observe_catalog(client);
            std::unique_lock<std::mutex> lock(calls->mutex);
            REQUIRE(calls->changed.wait_for(lock, std::chrono::seconds(3), [&] { return calls->failed + calls->completed > 0; }));
            CHECK(calls->failed == 1);
            CHECK(calls->completed == 0);
            CHECK_FALSE(calls->error.empty());
        }
    }
}

TEST_CASE("Provider catalog transport failure invokes the error callback without success",
          "[ModelGenerationClient][ProviderCatalogCallback]")
{
    CatalogReplyServer server("", true);
    AIModelGenerationClient client(server.url());
    const auto calls = observe_catalog(client);
    std::unique_lock<std::mutex> lock(calls->mutex);
    REQUIRE(calls->changed.wait_for(lock, std::chrono::seconds(3), [&] { return calls->failed + calls->completed > 0; }));
    CHECK(calls->failed == 1);
    CHECK(calls->completed == 0);
}

TEST_CASE("Legacy connected empty catalogs finish successfully without enabling generation",
          "[ModelGenerationClient][ProviderCatalogCallback]")
{
    CatalogReplyServer server(R"({"role":"geometry","provider":"yoyoclaw","state":"empty","models":[],"error":null})");
    AIModelGenerationClient client(server.url());
    const auto calls = observe_catalog(client);
    std::unique_lock<std::mutex> lock(calls->mutex);
    REQUIRE(calls->changed.wait_for(lock, std::chrono::seconds(3), [&] { return calls->failed + calls->completed > 0; }));
    CHECK(calls->failed == 0);
    CHECK(calls->completed == 1);
    CHECK_FALSE(calls->ready);
}

TEST_CASE("Provider catalog decoding rejects wrong types without throwing", "[ModelGenerationClient]")
{
    const auto parse = [](const nlohmann::json& body) {
        return AIModelGenerationClient::parse_provider_catalog(body.dump(), "geometry", "yoyoclaw");
    };
    const nlohmann::json valid {{"role", "geometry"}, {"provider", "yoyoclaw"}, {"state", "empty"},
                               {"models", nlohmann::json::array()}, {"error", nullptr}};
    REQUIRE(parse(valid));
    CHECK_FALSE(parse(valid)->ready());
    CHECK_FALSE(parse(valid)->single_image);
    auto capable = valid;
    capable["single_image"] = true;
    capable["named_four_view"] = true;
    CHECK(parse(capable)->single_image);
    CHECK(parse(capable)->named_four_view);
    CHECK_FALSE(AIModelGenerationClient::parse_provider_catalog("{", "geometry", "yoyoclaw"));
    for (const std::string field : {"role", "provider", "state", "default_model", "connection_state", "models", "error", "single_image", "named_four_view"}) {
        DYNAMIC_SECTION(field) {
            auto malformed = valid;
            malformed[field] = 42;
            CHECK_FALSE(parse(malformed));
        }
    }
    auto malformed = valid;
    malformed["models"] = nlohmann::json::array({{{"id", "model"}, {"name", "model"}, {"available", "yes"}}});
    CHECK_FALSE(parse(malformed));
}

TEST_CASE("Legacy Tripo choices restore the same standard face cap without editing saved history",
          "[ModelGenerationClient][Regression]")
{
    for (const int faces : {300000, 1000000, 2000000}) {
        for (const bool null_geometry : {false, true}) {
            DYNAMIC_SECTION("faces=" << faces << " null=" << null_geometry) {
                nlohmann::json saved = {{"face_limit", faces}, {"output_format", "obj"}};
                if (null_geometry) saved["geometry_quality"] = nullptr;
                const auto original = saved;
                for (int restore = 0; restore < 2; ++restore) {
                    const auto options = AIModelGenerationClient::restore_generation_options(saved);
                    CHECK(options.provider == "tripo");
                    CHECK(options.face_limit == (faces == 2000000 ? 1000000 : faces));
                    CHECK(options.geometry_quality == "standard");
                    CHECK(options.texture_quality == "standard");
                    CHECK(options.output_format == "obj");
                    CHECK(saved == original);
                }
            }
        }
    }
}

TEST_CASE("YOYOClaw default and historical face choices remain distinct when reading saved jobs",
          "[ModelGenerationClient][Regression]")
{
    for (const int faces : {0, 300000, 1000000}) {
        const nlohmann::json saved {{"provider", "yoyoclaw"}, {"face_limit", faces},
                                    {"geometry_quality", "standard"}};
        const auto restored = AIModelGenerationClient::restore_generation_options(saved);
        CHECK(restored.provider == "yoyoclaw");
        CHECK(restored.face_limit == faces);
    }
}

TEST_CASE("Explicit model choices survive restoration for review without changing cost or face target",
          "[ModelGenerationClient][Regression]")
{
    for (const std::string provider : {"tripo", "yoyoclaw"}) {
        for (const std::string geometry : {"standard", "detailed"}) {
            DYNAMIC_SECTION(provider << " " << geometry) {
                const nlohmann::json saved = {{"provider", provider}, {"face_limit", 2000000},
                    {"geometry_quality", geometry}, {"texture_quality", "extreme"}, {"output_format", "obj"}};
                const auto options = AIModelGenerationClient::restore_generation_options(saved);
                CHECK(options.provider == provider);
                CHECK(options.face_limit == 2000000);
                CHECK(options.geometry_quality == geometry);
                CHECK(options.texture_quality == "extreme");
                CHECK(options.output_format == "obj");
            }
        }
    }
}

TEST_CASE("Tripo legacy restoration does not change YOYOClaw choices",
          "[ModelGenerationClient][Regression]")
{
    const auto options = AIModelGenerationClient::restore_generation_options(
        {{"provider", "yoyoclaw"}, {"face_limit", 1000000}, {"geometry_quality", nullptr}});
    CHECK(options.provider == "yoyoclaw");
    CHECK(options.face_limit == 1000000);
    CHECK(options.geometry_quality == "standard");
}

TEST_CASE("Unsupported saved providers fall back to Tripo",
          "[ModelGenerationClient][Regression]")
{
    const auto options = AIModelGenerationClient::restore_generation_options(
        {{"provider", "removed-provider"}, {"face_limit", 2000000}, {"geometry_quality", "detailed"}});
    CHECK(options.provider == "tripo");
    CHECK(options.face_limit == 2000000);
    CHECK(options.geometry_quality == "detailed");
}

TEST_CASE("Optional provider details preserve terminal classification and legacy records", "[ModelGenerationClient]")
{
    auto job = nlohmann::json{{"id", "failed-job"}, {"source", "image"}, {"state", "failed"},
        {"provider_failure", {{"code", "provider_task_failed"}, {"category", "provider"},
            {"retryable", false}, {"ambiguous", false}}}};
    auto legacy = AIModelGenerationClient::parse_job(job);
    REQUIRE(legacy);
    CHECK(legacy->provider_detail_code.empty());
    CHECK(legacy->provider_detail_message.empty());
    for (const auto& detail : {nlohmann::json("FailedOperation.InnerError"), nlohmann::json::array(), nlohmann::json(nullptr)}) {
        DYNAMIC_SECTION(detail.dump()) {
            job["provider_failure"]["provider_code"] = detail;
            job["provider_failure"]["provider_message"] = "输入图片生成几何失败，请更换图片或重试";
            const auto status = AIModelGenerationClient::parse_job(job);
            REQUIRE(status);
            CHECK(status->state == "failed");
            CHECK(status->provider_error_code == "provider_task_failed");
            CHECK_FALSE(status->provider_error_retryable);
            CHECK_FALSE(status->provider_error_ambiguous);
            CHECK(status->provider_detail_message == "输入图片生成几何失败，请更换图片或重试");
            CHECK(status->provider_detail_code == (detail.is_string() ? detail.get<std::string>() : ""));
        }
    }
    job["provider_failure"]["provider_code"] = "invalid\ncode";
    job["provider_failure"]["provider_message"] = nlohmann::json::object();
    auto invalid = AIModelGenerationClient::parse_job(job);
    REQUIRE(invalid);
    CHECK(invalid->provider_detail_code.empty());
    CHECK(invalid->provider_detail_message.empty());
    job["provider_failure"]["provider_message"] = std::string(600, 'a') + "末尾";
    auto bounded = AIModelGenerationClient::parse_job(job);
    REQUIRE(bounded);
    CHECK(bounded->provider_detail_message.size() == 512);
}

TEST_CASE("Malformed optional quality reports do not hide a valid job or rewrite its report", "[ModelGenerationClient]")
{
    const auto valid = nlohmann::json{{"id", "design-ready"}, {"source", "image"},
        {"state", "awaiting_confirmation"}, {"preview", {{"ready", true}}},
        {"image_metrics", {{"score", 42.0}, {"palette_quality_ok", false},
            {"model_input_quality", {{"score", 19.65}, {"model_input_eligible", false},
                {"blockers", {"subject_cropped"}}}}}}};
    for (const auto& field : {"score", "mean_color_error", "small_region_ratio_after", "changed_pixel_ratio",
             "boundary_complexity", "minimum_feature_px", "meaningful_palette_count", "meaningful_subject_color_count",
             "printable_subject_area_ratio", "largest_subject_component_ratio", "largest_detached_subject_diagonal_ratio",
             "design_reference", "palette_quality_ok", "material_fragmentation_ok"}) {
        for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json::object(), nlohmann::json::array()}) {
            DYNAMIC_SECTION(field << "=" << malformed.dump()) {
                auto job = valid;
                job["image_metrics"][field] = malformed;
                const auto original = job;
                const auto status = AIModelGenerationClient::parse_job(job);
                REQUIRE(status);
                CHECK(status->id == "design-ready");
                CHECK(status->preview_ready);
                CHECK_FALSE(status->model_input_eligible);
                CHECK(status->model_input_blockers == std::vector<std::string>{"subject_cropped"});
                CHECK(job == original);
            }
        }
    }
    for (const auto& report : {"model_input_quality", "generation_input_quality"}) {
        for (const auto& field : {"score", "model_input_eligible", "blockers", "warnings"}) {
            DYNAMIC_SECTION(report << "." << field) {
                auto job = valid;
                job["image_metrics"][report] = {{"score", 19.65}, {"model_input_eligible", false},
                    {"blockers", {"subject_cropped"}}, {"warnings", {"review_recommended"}}};
                job["image_metrics"][report][field] = nlohmann::json::object();
                const auto original = job;
                const auto status = AIModelGenerationClient::parse_job(job);
                REQUIRE(status);
                CHECK(status->state == "awaiting_confirmation");
                CHECK(status->preview_ready);
                CHECK_FALSE(status->palette_quality_ok);
                if (std::string(field) != "model_input_eligible") CHECK_FALSE(status->model_input_eligible);
                CHECK(job == original);
            }
        }
    }
}
