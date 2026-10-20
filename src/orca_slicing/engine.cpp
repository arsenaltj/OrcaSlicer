#include "engine.hpp"
#include "project_support.hpp"
#include "recommendation_support.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace stdfs = std::filesystem;
namespace orca::slicing {
namespace {

void emit(const EventSink& sink, const std::string& job, const std::string& attempt,
          const std::string& phase, int progress, const std::string& message,
          const Json& extra = Json::object())
{
    if (!sink) return;
    Json event = extra;
    event["type"] = "progress";
    event["jobId"] = job;
    event["attemptId"] = attempt;
    event["phase"] = phase;
    if (progress >= 0) event["progress"] = progress / 100.0;
    else event["progress"] = nullptr;
    event["message"] = message;
    event["timestampMs"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sink(event);
}

Json result_error(const std::string& code, const std::string& message)
{
    return Json{{"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

std::string value_or(const Json& j, const char* key, const std::string& fallback = {})
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

Json slice_one(const Json& request, const EventSink& sink, std::atomic<bool>& canceled, const std::string& workspace)
{
    const std::string job = value_or(request, "jobId", "native-job");
    const std::string attempt = value_or(request, "attemptId", "native-attempt");
    const std::string input = value_or(request, "inputPath");
    if (input.empty() || !stdfs::is_regular_file(input))
        throw std::runtime_error("INPUT_NOT_FOUND");
    const std::string quality = value_or(request, "quality", "standard");
    if (quality != "standard" && quality != "fine" && quality != "fast" && quality != "smart")
        throw std::runtime_error("QUALITY_UNSUPPORTED");
    const Json overrides = request.value("config", Json::object());
    emit(sink, job, attempt, "importing", 2, "读取模型和配置");
    if (canceled.load()) throw std::runtime_error("SLICING_CANCELED");
    LoadedProject project = load_project(input, overrides, request.value("transforms", Json::array()));
    emit(sink, job, attempt, "analyzing", 12, "模型解析完成", describe_project(project));
    if (canceled.load()) throw std::runtime_error("SLICING_CANCELED");

    Slic3r::DynamicPrintConfig config = quality_config(project.config, quality == "smart" ? "standard" : quality);
    const auto outputRoot = stdfs::path(value_or(request, "outputDir", (stdstdfs::path(workspace) / "results").string()));
    stdfs::create_directories(outputRoot);
    const auto temp = outputRoot / (job + "-" + attempt + ".gcode.partial");
    const auto final = outputRoot / (job + "-" + attempt + ".gcode");
    const auto scene = outputRoot / (job + "-" + attempt + ".scene");
    write_scene(project, scene.string());
    emit(sink, job, attempt, "validating", 20, "校验打印配置和模型");

    Slic3r::Print print;
    print.set_status_silent();
    print.set_cancel_callback([&canceled] { if (canceled.load()) throw Slic3r::CanceledException(); });
    print.set_status_callback([&](const Slic3r::PrintBase::SlicingStatus& status) {
        if (canceled.load()) print.cancel();
        emit(sink, job, attempt, "slicing", std::clamp(status.percent, 0, 100), status.text);
    });
    print.apply(project.model, std::move(config));
    std::vector<Slic3r::StringObjectException> warnings;
    const auto validation = print.validate(&warnings);
    if (!validation.string.empty()) throw std::runtime_error("VALIDATION_FAILED:" + validation.string);
    emit(sink, job, attempt, "slicing", 30, "开始原生切片");
    print.process();
    if (canceled.load()) throw std::runtime_error("SLICING_CANCELED");
    Slic3r::GCodeProcessorResult gcode;
    const std::string generated = print.export_gcode(temp.string(), &gcode, nullptr);
    if (generated.empty() || !stdfs::is_regular_file(generated)) throw std::runtime_error("GCODE_NOT_CREATED");
    if (canceled.load()) throw std::runtime_error("SLICING_CANCELED");
    std::error_code ec;
    stdfs::rename(generated, final, ec);
    if (ec) {
        stdfs::copy_file(generated, final, stdfs::copy_options::overwrite_existing, ec);
        stdfs::remove(generated, ec);
    }
    if (!stdfs::is_regular_file(final)) throw std::runtime_error("GCODE_COMMIT_FAILED");
    emit(sink, job, attempt, "committed", 100, "切片产物已提交");

    double filament = 0.0;
    for (const auto& [_, volume] : gcode.print_statistics.total_volumes_per_extruder) filament += volume;
    unsigned int maxLayer = 0;
    for (const auto& move : gcode.moves) maxLayer = std::max(maxLayer, move.layer_id);
    Json out{{"jobId",job},{"attemptId",attempt},{"state","succeeded"},{"gcodePath",final.string()},
             {"scenePath",scene.string()},{"layerCount",maxLayer + 1},{"filamentVolumeMm3",filament},
             {"printTimeSeconds",gcode.print_statistics.modes[static_cast<size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time},
             {"warnings",Json::array()},{"quality",quality},{"project",describe_project(project)}};
    for (const auto& warning : warnings) out["warnings"].push_back(warning.string);
    return out;
}

} // namespace

Engine::Engine(std::string workspace) : workspace_(std::move(workspace)) {}
Engine::~Engine() { cancel(); }
void Engine::checkCanceled() const { if (canceled_.load()) throw std::runtime_error("SLICING_CANCELED"); }
void Engine::cancel() noexcept { canceled_.store(true); std::lock_guard<std::mutex> lock(printMutex_); if (activePrint_) activePrint_->cancel(); }
Json Engine::execute(const Json& request, const EventSink& sink)
{
    canceled_.store(false);
    const std::string operation = value_or(request, "operation", "slice");
    if (operation == "capabilities") return Json{{"available",true},{"nativeSlicing",true},{"nativeRecommendation",true},{"remoteAI",false},{"qualities",Json::array({"standard","fine","fast","smart"})}};
    if (operation == "slice" || operation == "validate") {
        std::lock_guard<std::mutex> lock(printMutex_);
        return slice_one(request, sink, canceled_, workspace_);
    }
    if (operation == "recommend") {
        const std::string input = value_or(request, "inputPath");
        LoadedProject project = load_project(input, request.value("config", Json::object()), request.value("transforms", Json::array()));
        Json out{{"jobId",value_or(request,"jobId")},{"attemptId",value_or(request,"attemptId")},{"state","succeeded"},{"candidates",Json::array()}};
        for (const auto& candidate : recommendation_candidates(project)) out["candidates"].push_back(describe_candidate(candidate));
        emit(sink, value_or(request,"jobId"), value_or(request,"attemptId"), "recommendation", 100, "本地推荐候选已生成", out);
        return out;
    }
    if (operation == "import" || operation == "analyze") {
        LoadedProject project = load_project(value_or(request,"inputPath"), request.value("config", Json::object()), request.value("transforms", Json::array()));
        return describe_project(project);
    }
    throw std::runtime_error("OPERATION_UNSUPPORTED");
}
} // namespace orca::slicing
