#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "PortraitResidualProposalClient.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#endif
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

namespace Slic3r::GUI::PortraitResidual {
namespace {
namespace fs = boost::filesystem;
namespace process = boost::process;

std::string read_bounded(const fs::path& path, size_t limit)
{
    if (!fs::is_regular_file(path) || fs::file_size(path) > limit) throw std::runtime_error("output");
    fs::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_json(const fs::path& path, const nlohmann::json& value)
{
    fs::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("request");
    stream << value.dump(); stream.flush();
    if (!stream) throw std::runtime_error("request");
}

} // namespace

ClientResult run(const LocalSemanticWorker::Configuration& config,
                 const boost::filesystem::path& installed_script,
                 const boost::filesystem::path& request_root,
                 const boost::filesystem::path& cache_root,
                 const nlohmann::json& request,
                 const std::atomic<bool>& cancelled)
{
    ClientResult result;
    if (cancelled.load()) { result.status = ClientStatus::Cancelled; result.reason = "cancelled"; return result; }
    if (!config.enabled) { result.status = ClientStatus::Disabled; result.reason = "not_configured"; return result; }
    try {
        if (!config.python_executable.is_absolute() || !fs::is_regular_file(config.python_executable) ||
            !installed_script.is_absolute() || !fs::is_regular_file(installed_script) || !request_root.is_absolute())
            throw std::runtime_error("runtime_missing");
        if (request.value("schema", std::string()) != "orca.portrait-residual-request/v1")
            throw std::runtime_error("request_schema");
        fs::create_directories(request_root);
        result.request_directory = request_root / fs::unique_path("residual-%%%%-%%%%-%%%%");
        if (!fs::create_directory(result.request_directory)) throw std::runtime_error("request_directory");
        const auto request_path = result.request_directory / "request.json";
        const auto output_path = result.request_directory / "proposal.json";
        write_json(request_path, request);
        process::environment env; env.clear();
        for (const char* key : {"SYSTEMROOT", "WINDIR", "TEMP", "TMP", "TMPDIR"})
            if (const char* value = boost::nowide::getenv(key)) env[key] = value;
        env["PYTHONNOUSERSITE"] = "1";
        env["HF_HUB_OFFLINE"] = "1";
        env["TRANSFORMERS_OFFLINE"] = "1";
        env["HF_HUB_DISABLE_TELEMETRY"] = "1";
        env["NO_PROXY"] = "*";
        env["no_proxy"] = "*";
        std::unique_ptr<process::child> child;
#ifdef _WIN32
        process::wenvironment wide_env(env);
        child = std::make_unique<process::child>(config.python_executable.wstring(),
            process::args(std::vector<std::wstring>{L"-I", installed_script.wstring(), L"--request", request_path.wstring(),
                L"--output", output_path.wstring(), L"--cache-root", cache_root.wstring()}), wide_env,
            process::start_dir(result.request_directory.wstring()), process::std_out > process::null,
            process::std_err > process::null, process::windows::create_no_window);
#else
        child = std::make_unique<process::child>(config.python_executable.string(),
            process::args(std::vector<std::string>{"-I", installed_script.string(), "--request", request_path.string(),
                "--output", output_path.string(), "--cache-root", cache_root.string()}), env,
            process::start_dir(result.request_directory.string()), process::std_out > process::null,
            process::std_err > process::null);
#endif
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(10u, config.timeout_seconds));
        while (child->running()) {
            if (cancelled.load()) { child->terminate(); result.status = ClientStatus::Cancelled; result.reason = "cancelled"; return result; }
            if (std::chrono::steady_clock::now() >= deadline) { child->terminate(); result.status = ClientStatus::TimedOut; result.reason = "residual_timeout"; return result; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        child->wait();
        const auto response = nlohmann::json::parse(read_bounded(output_path, 32 * 1024 * 1024));
        if (response.value("status", std::string()) == "CANCELLED") {
            result.status = ClientStatus::Cancelled; result.reason = "cancelled"; return result;
        }
        if (response.value("status", std::string()) == "UNAVAILABLE" || child->exit_code() != 0) {
            result.status = ClientStatus::Unavailable; result.reason = "local_residual_proposal_unavailable"; return result;
        }
        result.document = Document::decode(response);
        result.document.bind_to_request(request,AI::model_artifact_sha256(request_path));
        result.status = ClientStatus::Ready;
        result.reason.clear();
        return result;
    } catch (...) {
        result.status = cancelled.load() ? ClientStatus::Cancelled : ClientStatus::Unavailable;
        result.reason = cancelled.load() ? "cancelled" : "local_residual_proposal_unavailable";
        return result;
    }
}

} // namespace Slic3r::GUI::PortraitResidual
