#include "slic3r/Utils/DiagnosticLogs.hpp"
#include "slic3r/Utils/ZipArchive.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <wx/log.h>
#include <nlohmann/json.hpp>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs=boost::filesystem;
struct LogsDirectory {
    fs::path path=fs::temp_directory_path()/fs::unique_path("diagnostic-%%%%%%%%");
    LogsDirectory(){fs::create_directories(path/"log");}
    ~LogsDirectory(){boost::system::error_code error;fs::remove_all(path,error);}
    void put(const fs::path& name,const std::string& bytes) {fs::create_directories((path/name).parent_path());fs::ofstream out(path/name,std::ios::binary);out<<bytes;}
};
std::string read(const fs::path& path) {fs::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
std::map<std::string,std::string> unzip(const fs::path& path) {
    wxLogNull quiet;
#ifdef _WIN32
    wxFileInputStream file(wxString(path.wstring()));
#else
    wxFileInputStream file(wxString::FromUTF8(path.string()));
#endif
    REQUIRE(file.IsOk());std::map<std::string,std::string> result;wxZipInputStream zip(file);
    while(auto* raw=zip.GetNextEntry()) {
        std::unique_ptr<wxZipEntry> entry(raw);if(entry->IsDir()) continue;
        std::string bytes;char buffer[8192];
        while(!zip.Eof() && zip.IsOk()) {zip.Read(buffer,sizeof(buffer));bytes.append(buffer,zip.LastRead());}
        result[entry->GetInternalName().ToUTF8().data()]=std::move(bytes);
    }
    return result;
}
bool no_staging(const fs::path& parent) {
    for(const auto& p:fs::directory_iterator(parent)) if(p.path().filename().string().find(".honor-logs-")==0) return false;
    return true;
}
}
using namespace Slic3r::Diagnostics;
TEST_CASE("Diagnostic redaction preserves error context and removes credentials without truncation", "[DiagnosticLogs]") {
    const std::string input=R"({"time":"2026-11-11","stage":"repair","diagnostic_id":"diag-123","error":"network failed","accessToken":"TOKEN_VALUE","nested":{"credentials":{"name":"user","proof":"proof-value"},"sessionProof":"PROOF_VALUE","apiKey":"KEY_VALUE"},"message":"GET https://user:pass@example.test/v1/models?token=QUERY_VALUE#private"})";
    const auto clean=redact_log_text("[native] "+input);
    for(const char* secret:{"TOKEN_VALUE","proof-value","PROOF_VALUE","KEY_VALUE","QUERY_VALUE","user:pass"})
        REQUIRE(clean.find(secret)==std::string::npos);
    for(const char* context:{"2026-11-11","repair","diag-123","network failed","https://example.test/v1/models"})
        REQUIRE(clean.find(context)!=std::string::npos);
    const auto plain=redact_log_text("Authorization: Bearer AUTH_VALUE; password=PASSWORD_VALUE bearer BEARER_VALUE sk-abcdefghijklmnop");
    for(const char* secret:{"AUTH_VALUE","PASSWORD_VALUE","BEARER_VALUE","sk-abcdefghijklmnop"}) REQUIRE(plain.find(secret)==std::string::npos);
    const std::string long_error(5000,'x');REQUIRE(redact_log_text("error="+long_error)=="error="+long_error);
    const auto escaped=redact_log_text(R"(prefix {"refreshToken":"quoted\"TOKEN_VALUE", "nonce":123456, "stage":"saving"} trailing)");
    REQUIRE(escaped.find("TOKEN_VALUE")==std::string::npos);REQUIRE(escaped.find("123456")==std::string::npos);
    REQUIRE(escaped.find("saving")!=std::string::npos);
}
TEST_CASE("Log snapshots export only current diagnostic logs and a readable manifest to Chinese paths", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/debug.log.0","time=now diagnostic_id=diag-123 password=SECRET_VALUE\n");
    root.put("log/orca-ai-sidecar.log",R"({"stage":"model","access_token":"TOKEN_VALUE","diagnostic_id":"ai-456"})");
    root.put("log/diagnostics/errors.jsonl","error=connection failed\n");
    root.put(fs::path(L"log/服务.log"),"diagnostic_id=chinese-log\n");
    root.put("models/history.log","private model");root.put("accounts/store.json","credentials");root.put("log/account.json","private config");
    const auto target=root.path/fs::path(L"中文目录")/fs::path(L"中文日志.zip");fs::create_directory(target.parent_path());std::atomic_bool cancel {false};
    const auto result=export_logs(root.path,target,"test-version",cancel);
    REQUIRE(result.success);REQUIRE(result.omitted.empty());REQUIRE(result.files.size()==4);REQUIRE(no_staging(target.parent_path()));
    const auto archive=unzip(target);REQUIRE(archive.size()==5);REQUIRE(archive.count("manifest.json")==1);
    const auto manifest=nlohmann::json::parse(archive.at("manifest.json"));
    REQUIRE(manifest.at("applicationVersion")=="test-version");REQUIRE(manifest.at("files").size()==4);REQUIRE(manifest.at("omitted").empty());
    REQUIRE(archive.at("logs/服务.log")=="diagnostic_id=chinese-log\n");
    REQUIRE(archive.at("logs/debug.log.0").find("SECRET_VALUE")==std::string::npos);
    REQUIRE(archive.at("logs/orca-ai-sidecar.log").find("TOKEN_VALUE")==std::string::npos);
    REQUIRE(archive.at("logs/orca-ai-sidecar.log").find("ai-456")!=std::string::npos);
}
TEST_CASE("Running logs use a finite snapshot and late cancellation protects an existing archive", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/current.log","first line\n");root.put("target.zip","original archive");
    fs::ofstream running(root.path/"log/current.log",std::ios::binary|std::ios::app);running<<"running line\n";running.flush();
    std::atomic_bool cancel {false};auto result=export_logs(root.path,root.path/"target.zip","version",cancel);
    REQUIRE(result.success);REQUIRE(unzip(root.path/"target.zip").at("logs/current.log").find("running line")!=std::string::npos);
    const auto original=read(root.path/"target.zip");
    result=export_logs(root.path,root.path/"target.zip","version",cancel,[&](const std::string& stage){if(stage.find("完成")!=std::string::npos) cancel=true;});
    REQUIRE(result.cancelled);REQUIRE_FALSE(result.success);REQUIRE(read(root.path/"target.zip")==original);REQUIRE(no_staging(root.path));
}
TEST_CASE("Early cancellation invalid paths and empty logs leave no temporary or replacement files", "[DiagnosticLogs]") {
    LogsDirectory root;std::atomic_bool cancel {false};const auto target=root.path/"existing.zip";root.put("existing.zip","old bytes");
    REQUIRE(export_logs(root.path,target,"version",cancel).empty);REQUIRE(read(target)=="old bytes");
    root.put("log/active.log","error\n");
    const auto stopped=export_logs(root.path,target,"version",cancel,[&](const std::string&){cancel=true;});
    REQUIRE(stopped.cancelled);REQUIRE(read(target)=="old bytes");REQUIRE(no_staging(root.path));cancel=false;
    const auto bad=export_logs(root.path,root.path/"missing"/"target.zip","version",cancel);
    REQUIRE_FALSE(bad.success);REQUIRE_FALSE(bad.error.empty());REQUIRE_FALSE(fs::exists(root.path/"missing"));
    const auto directory=export_logs(root.path,root.path/"log","version",cancel);
    REQUIRE_FALSE(directory.success);REQUIRE(fs::exists(root.path/"log/active.log"));REQUIRE(no_staging(root.path));
}
TEST_CASE("Rotated or locked logs are reported as omissions while readable diagnostics survive", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/rotating.log","will rotate\n");root.put("log/stable.log","stable context\n");
    std::atomic_bool cancel {false};bool removed=false;
    const auto result=export_logs(root.path,root.path/"target.zip","version",cancel,[&](const std::string& stage) {
        if(!removed && stage.find("rotating.log")!=std::string::npos) {fs::remove(root.path/"log/rotating.log");removed=true;}
    });
    REQUIRE(result.success);REQUIRE(result.omitted.size()==1);REQUIRE(result.files.size()==1);
    const auto archive=unzip(root.path/"target.zip");
    REQUIRE(archive.at("logs/stable.log")=="stable context\n");
    REQUIRE(nlohmann::json::parse(archive.at("manifest.json")).at("omitted").size()==1);REQUIRE(no_staging(root.path));
}
#ifdef _WIN32
TEST_CASE("An exclusively locked destination preserves its original ZIP on publication failure", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/current.log","error\n");root.put("target.zip","original bytes");
    const auto target=root.path/"target.zip";std::atomic_bool cancel {false};
    HANDLE locked=CreateFileW(target.wstring().c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE(locked!=INVALID_HANDLE_VALUE);
    const auto result=export_logs(root.path,target,"version",cancel);CloseHandle(locked);
    REQUIRE_FALSE(result.success);REQUIRE_FALSE(result.error.empty());REQUIRE(read(target)=="original bytes");REQUIRE(no_staging(root.path));
}
TEST_CASE("An exclusively locked log is omitted without blocking other program diagnostics", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/locked.log","locked input\n");root.put("log/available.log","available context\n");
    const auto source=root.path/"log/locked.log";std::atomic_bool cancel {false};
    HANDLE locked=CreateFileW(source.wstring().c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE(locked!=INVALID_HANDLE_VALUE);
    const auto result=export_logs(root.path,root.path/"target.zip","version",cancel);CloseHandle(locked);
    REQUIRE(result.success);REQUIRE(result.omitted.size()==1);REQUIRE(result.files.size()==1);
    REQUIRE(unzip(root.path/"target.zip").at("logs/available.log")=="available context\n");
}
#endif
TEST_CASE("The shared troubleshooting ZIP writer retains text and directory exports", "[DiagnosticLogs]") {
    LogsDirectory root;root.put("log/legacy.log","legacy");
    const auto target=root.path/"legacy.zip";
    REQUIRE(save_zip_sources({wxString::FromUTF8((root.path/"log").string()),"TxtData:system.txt|system summary"},wxString::FromUTF8(target.string())));
    const auto archive=unzip(target);REQUIRE(archive.at("log/legacy.log")=="legacy");REQUIRE(archive.at("system.txt")=="system summary");
}
