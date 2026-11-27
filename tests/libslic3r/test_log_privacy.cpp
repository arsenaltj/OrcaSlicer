#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/LogPrivacy.hpp"

using Slic3r::redact_log_user_paths;

TEST_CASE("Log messages hide absolute user identities and preserve asset context", "[LogPrivacy]")
{
    const auto path = GENERATE(
        std::string(R"(C:\Users\sample-user\AppData\Local\Temp\model.glb)"),
        std::string(R"(c:/uSeRs/sample-user/Documents/model.glb)"),
        std::string(R"(D:\Documents and Settings\sample-user\Temp\model.glb)"),
        std::string(R"(C:\Users\sample-user with spaces\Temp\model.glb)"),
        std::string("file:///C:/Users/sample-user/Temp/model.glb"),
        std::string("/home/sample-user/cache/model.glb"),
        std::string("/Users/sample-user/Library/model.glb"));
    const std::string message = "job=job-123 request=request-456 path=\"" + path + "\" failed=read";
    const auto clean = redact_log_user_paths(message);
    CHECK(clean.find("sample-user") == std::string::npos);
    CHECK(clean.find("<user-home>") != std::string::npos);
    CHECK(clean.find("model.glb") != std::string::npos);
    CHECK(clean.find("job=job-123 request=request-456") != std::string::npos);
    CHECK(clean.find("failed=read") != std::string::npos);
    CHECK(redact_log_user_paths(clean) == clean);
}

TEST_CASE("Log redaction hides Unicode profile names and multiple unquoted paths", "[LogPrivacy]")
{
    const std::string message = u8"source=C:/Users/测试用户/Temp/source.glb target=/home/sample-user/result.glb job=123";
    const auto clean = redact_log_user_paths(message);
    CHECK(clean.find(u8"测试用户") == std::string::npos);
    CHECK(clean.find("sample-user") == std::string::npos);
    CHECK(clean.find("source.glb") != std::string::npos);
    CHECK(clean.find("result.glb") != std::string::npos);
    CHECK(clean.find("job=123") != std::string::npos);
}

TEST_CASE("Log redaction hides Windows profiles concatenated directly to diagnostic labels", "[LogPrivacy]")
{
    const auto message = GENERATE(
        std::string(R"(create missing sub_pathC:\Users\sample-user\AppData\Local\Temp/model/Auxiliaries)"),
        std::string(R"(Failed to create missing sub_pathD:\Users\sample-user with spaces\Temp\model: access denied)"),
        std::string(R"(sourceC:\\Users\\sample-user\\Temp\\model.glb targetD:/Users/sample-other/Temp/result.3mf)"));
    const auto clean = redact_log_user_paths(message);
    CHECK(clean.find("sample-") == std::string::npos);
    CHECK(clean.find("<user-home>") != std::string::npos);
    CHECK(clean.find("model") != std::string::npos);
    CHECK(redact_log_user_paths(clean) == clean);
}

TEST_CASE("Log redaction keeps JSON valid and removes each serialized user root", "[LogPrivacy]")
{
    const nlohmann::json record = {
        {"job_id", "job-123"}, {"task_id", "task-456"},
        {"source", R"(C:\Users\sample-a\Temp\source.glb)"},
        {"target", R"(D:\Users\sample-b with spaces\Temp\result.3mf)"},
        {"nested", {{"source", "/home/sample-c/work/model.obj"}}}
    };
    const auto clean = redact_log_user_paths(record.dump());
    const auto parsed = nlohmann::json::parse(clean);
    CHECK(parsed.at("job_id") == "job-123");
    CHECK(parsed.at("task_id") == "task-456");
    CHECK(clean.find("sample-") == std::string::npos);
    CHECK(parsed.at("source").get<std::string>().find("source.glb") != std::string::npos);
    CHECK(parsed.at("target").get<std::string>().find("result.3mf") != std::string::npos);
    CHECK(parsed.at("nested").at("source") == "<user-home>/work/model.obj");
}

TEST_CASE("Log redaction preserves public paths and unrelated messages", "[LogPrivacy]")
{
    const auto message = GENERATE(
        std::string(),
        std::string("job=123 phase=ready elapsed_ms=45"),
        std::string(R"(D:\workspace\models\result.glb)"),
        std::string("/tmp/orcaslicer_1000/model.glb"),
        std::string("resources/users/icons/model.png"),
        std::string("https://example.invalid/Users/catalog/model"));
    CHECK(redact_log_user_paths(message) == message);
}
