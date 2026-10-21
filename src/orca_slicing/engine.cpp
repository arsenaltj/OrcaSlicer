#include "engine.hpp"
#include "project_support.hpp"
#include "recommendation_support.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <tbb/global_control.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#if defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif
#ifndef ORCA_SLICING_SOURCE_COMMIT
#define ORCA_SLICING_SOURCE_COMMIT "unknown"
#endif
namespace stdfs=std::filesystem;
namespace orca::slicing {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex workerMutex;
std::string text(const Json& j,const char* key,const std::string& fallback={})
{ return j.contains(key)&&j[key].is_string()?j[key].get<std::string>():fallback; }
void emit(const EventSink& sink,const Json& request,const std::string& phase,int progress,
          const std::string& message,const Json& extra=Json::object())
{
    if (!sink) return;
    Json event=extra;
    event["type"]="progress"; event["schemaVersion"]=2;
    event["jobId"]=text(request,"jobId"); event["attemptId"]=text(request,"attemptId");
    event["phase"]=phase; event["progress"]=progress/100.0; event["message"]=message;
    event["timestampMs"]=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sink(event);
}

// A single monitor owns the Print pointer under a SHORT lock. No callback enters
// this lock. Cancellation, budget and timeout remain effective between callbacks.
class Budget {
    std::atomic<bool>& canceled;
    Clock::time_point deadline;
    uint64_t memoryLimit;
    std::mutex mutex;
    std::condition_variable wake;
    bool done=false;
    Slic3r::Print* active=nullptr;
    std::atomic<int> reason{0}; // 1 timeout, 2 memory, 3 explicit cancel
    std::thread monitor;
    static uint64_t resident_bytes() {
#if defined(__linux__) || defined(__ANDROID__)
        uint64_t total=0,resident=0;
        std::ifstream stat("/proc/self/statm");
        if (stat>>total>>resident) return resident*static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
        return 0;
    }
public:
    Budget(std::atomic<bool>& flag,int64_t maxMs,uint64_t maxBytes):canceled(flag),
        deadline(maxMs>0?Clock::now()+std::chrono::milliseconds(maxMs):Clock::time_point::max()),memoryLimit(maxBytes) {
        monitor=std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!done) {
                int why=canceled.load()?3:0;
                if (!why && Clock::now()>=deadline) why=1;
                if (!why && memoryLimit && resident_bytes()>memoryLimit) why=2;
                if (why) { int expected=0; reason.compare_exchange_strong(expected,why); if (active) active->cancel(); }
                wake.wait_for(lock,std::chrono::milliseconds(25),[this]{return done;});
            }
        });
    }
    ~Budget() { {std::lock_guard<std::mutex> lock(mutex);done=true;} wake.notify_one();monitor.join(); }
    void attach(Slic3r::Print* print) { std::lock_guard<std::mutex> lock(mutex);active=print; }
    void check() const {
        const int why=reason.load();
        if (why==1) throw std::runtime_error("RECOMMENDATION_TIMEOUT");
        if (why==2) throw std::runtime_error("MEMORY_LIMIT");
        if (why==3 || canceled.load()) throw std::runtime_error("SLICING_CANCELED");
        if (Clock::now()>=deadline) throw std::runtime_error("RECOMMENDATION_TIMEOUT");
    }
};
struct PrintScope {
    Budget& budget;
    PrintScope(Budget& b,Slic3r::Print& print):budget(b) {budget.attach(&print);}
    ~PrintScope() {budget.attach(nullptr);}
};

void commit_file(const stdfs::path& source,const stdfs::path& target)
{
    // Same-directory rename is the commit boundary. Never delete a failed copy
    // or announce a result before every artifact has been closed and hashed.
    std::error_code error;
    stdfs::rename(source,target,error);
    if (error) {
        if (stdfs::is_regular_file(target) && file_sha256(source.u8string())==file_sha256(target.u8string())) {
            stdfs::remove(source); return;
        }
        throw std::runtime_error("ARTIFACT_COMMIT_FAILED:"+error.message());
    }
}
void write_json(const stdfs::path& path,const Json& value)
{
    auto partial=path; partial+=".partial";
    std::ofstream output(partial,std::ios::binary|std::ios::trunc);
    output<<value.dump(2); output.flush();
    if (!output) throw std::runtime_error("SNAPSHOT_WRITE_FAILED");
    output.close(); commit_file(partial,path);
}
Json request_snapshot(const Json& request)
{
    Json effective=Json::object();
    if (request.contains("snapshotPath")) {
        std::ifstream input(stdfs::u8path(request.at("snapshotPath").get<std::string>()));
        if (!input) throw std::runtime_error("PROJECT_SNAPSHOT_NOT_FOUND");
        input>>effective;
        if (!effective.is_object() || effective.value("schemaVersion",0)!=2) throw std::runtime_error("INVALID_PROJECT_SNAPSHOT");
    }
    effective.update(request);
    return effective;
}
LoadedProject load(const Json& request,const std::string& workspace)
{
    LoadedProject project=load_project(text(request,"inputPath"),request.value("config",Json::object()),
        request.value("transforms",Json::array()),workspace,request.value("projectCommands",Json::array()));
    if (request.contains("inputSha256") && request["inputSha256"]!=project.inputSha256) throw std::runtime_error("INPUT_IDENTITY_MISMATCH");
    project.metadata["projectId"]=request.value("projectId",Json());
    project.metadata["revision"]=request.value("projectRevision",request.value("revision",Json(1)));
    project.metadata["quality"]=text(request,"quality","standard");
    return project;
}
stdfs::path root(const Json& request,const std::string& workspace)
{
    auto path=stdfs::u8path(text(request,"outputDir",(stdfs::u8path(workspace)/"results").u8string()));
    stdfs::create_directories(path); return path;
}
std::string artifact_key(const Json& request,const LoadedProject& project)
{
    return json_sha256(Json{{"job",text(request,"jobId")},{"attempt",text(request,"attemptId")},
        {"project",project.metadata},{"input",project.inputSha256},{"config",config_json(project.config)}}).substr(0,32);
}
Json scene_and_snapshot(LoadedProject& project,const stdfs::path& prefix)
{
    auto scene=prefix; scene+=".scene";
    auto partial=scene; partial+=".partial";
    const auto sceneInfo=write_scene(project,partial.u8string());
    commit_file(partial,scene);
    Json description=describe_project(project);
    description["scenePath"]=scene.u8string();
    description["sceneSha256"]=file_sha256(scene.u8string());
    description["sceneSize"]=stdfs::file_size(scene);
    auto snapshot=prefix; snapshot+=".project.json";
    description["snapshotPath"]=snapshot.u8string();
    description["sourceCommit"]=ORCA_SLICING_SOURCE_COMMIT;
    description["engineVersion"]="orca-slicing-c/1.0.0";
    write_json(snapshot,description);
    return description;
}

Json slice_one(LoadedProject& project,const Json& request,const EventSink& sink,Budget& budget,
               const std::string& workspace)
{
    budget.check();
    const auto started=Clock::now();
    validate_output_capabilities(project);
    validate_project_bounds(project);
    const Json requestedConfig=config_json(project.config);
    emit(sink,request,"validating",20,"校验实际生效配置与模型");
    Slic3r::Print print;
    PrintScope registration(budget,print);
    print.set_status_silent();
    print.set_status_callback([&](const Slic3r::PrintBase::SlicingStatus& status) {
        budget.check(); emit(sink,request,"slicing",std::clamp(status.percent,0,100),status.text);
    });
    auto model=slicing_model(project);
    print.apply(model,project.config);
    // Print::apply normalizes a by-value copy (e.g. vase walls/top/infill).
    // Identity, snapshots and confirmed candidate configs must describe that
    // actual normalized config, not the caller's requested values.
    project.config=print.full_print_config();
    validate_output_capabilities(project);
    const auto prefix=root(request,workspace)/artifact_key(request,project);
    Json description=scene_and_snapshot(project,prefix);
    const auto gcodePartial=stdfs::u8path(prefix.u8string()+".gcode.partial");
    const auto gcodeFinal=stdfs::u8path(prefix.u8string()+".gcode");
    const auto previewPartial=stdfs::u8path(prefix.u8string()+".orcapv.partial");
    const auto previewFinal=stdfs::u8path(prefix.u8string()+".orcapv");
    std::vector<Slic3r::StringObjectException> warnings;
    const auto validation=print.validate(&warnings);
    if (!validation.string.empty()) throw std::runtime_error("VALIDATION_FAILED:"+validation.string);
    budget.check();
    try { print.process(); }
    catch (const Slic3r::CanceledException&) { budget.check(); throw std::runtime_error("SLICING_CANCELED"); }
    budget.check();
    Slic3r::GCodeProcessorResult gcode;
    std::string generated;
    try { generated=print.export_gcode(gcodePartial.u8string(),&gcode,nullptr); }
    catch (const Slic3r::CanceledException&) {budget.check();throw std::runtime_error("SLICING_CANCELED");}
    if (generated.empty() || !stdfs::is_regular_file(stdfs::u8path(generated))) throw std::runtime_error("GCODE_NOT_CREATED");
    budget.check();
    Json preview=write_preview(gcode,previewPartial.u8string());
    budget.check();
    commit_file(stdfs::u8path(generated),gcodeFinal);
    commit_file(previewPartial,previewFinal);
    preview["previewPath"]=previewFinal.u8string();
    double volume=0,grams=0;
    const auto* densities=project.config.option<Slic3r::ConfigOptionFloats>("filament_density");
    for (const auto& usage:gcode.print_statistics.total_volumes_per_extruder) {
        volume+=usage.second;
        if (!densities || usage.first>=densities->values.size() || densities->values[usage.first]<=0)
            throw std::runtime_error("FILAMENT_DENSITY_REQUIRED_FOR_METRICS");
        grams+=usage.second*densities->values[usage.first]/1000.0;
    }
    Json out=preview;
    out.update(Json{{"schemaVersion",2},{"projectId",request.value("projectId",Json())},
        {"projectRevision",project.metadata.at("revision")},{"inputSha256",project.inputSha256},
        {"configSha256",json_sha256(config_json(project.config))},{"effectiveConfig",config_json(project.config)},
        {"requestedConfig",requestedConfig},{"requestedConfigSha256",json_sha256(requestedConfig)},
        {"jobId",text(request,"jobId")},{"attemptId",text(request,"attemptId")},{"state","succeeded"},
        {"quality",text(request,"quality","standard")},{"qualityPresetVersion","p1-v2"},
        {"outputMode",project.metadata.value("outputMode",Json("native"))},
        {"singleMaterialFilamentIndex",project.metadata.value("singleMaterialFilamentIndex",Json())},
        {"sourceColorCount",description["model"]["colors"].size()},
        {"engineVersion","orca-slicing-c/1.0.0"},{"sourceCommit",ORCA_SLICING_SOURCE_COMMIT},
        {"sourceIdentityValidated",std::string(ORCA_SLICING_SOURCE_COMMIT)!="unknown"},
        {"gcodePath",gcodeFinal.u8string()},{"gcodeSize",stdfs::file_size(gcodeFinal)},
        {"gcodeSha256",file_sha256(gcodeFinal.u8string())},{"scenePath",description.at("scenePath")},
        {"sceneSha256",description.at("sceneSha256")},{"sceneSize",description.at("sceneSize")},
        {"snapshotPath",description.at("snapshotPath")},{"colors",description["model"]["colors"]},{"palette",description["model"]["colors"]},
        {"filamentVolumeMm3",volume},{"filamentGrams",grams},
        {"printTimeSeconds",gcode.print_statistics.modes[size_t(Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time},
        {"calculationElapsedMs",std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()},
        {"warnings",Json::array()},{"project",description}});
    for (const auto& warning:warnings) out["warnings"].push_back(warning.string);
    budget.check();
    auto manifest=prefix; manifest+=".result.json";
    out["manifestPath"]=manifest.u8string();
    write_json(manifest,out);
    emit(sink,request,"committed",100,"原生切片与预览产物已提交");
    return out;
}
} // namespace

Engine::Engine(std::string workspace):workspace_(std::move(workspace)) {}
Engine::~Engine() {cancel();}
void Engine::cancel() noexcept {canceled_.store(true);}
Json Engine::execute(const Json& raw,const EventSink& sink)
{
    std::unique_lock<std::mutex> worker(workerMutex,std::try_to_lock);
    if (!worker.owns_lock()) throw std::runtime_error("BUSY:Only one native worker may execute");
    canceled_.store(false);
    const Json request=request_snapshot(raw);
    const std::string operation=text(request,"operation","slice");
    if (operation=="capabilities") return Json{{"schemaVersion",2},{"available",true},{"nativeSlicing",true},
        {"nativeRecommendation",true},{"remoteAI",false},{"nativeCheckpointResume",false},
        {"maxWorkers",1},{"maxThreads",4},{"qualities",Json::array({"standard","fine","fast","smart"})}};
    const int maxThreads=std::clamp(request.value("maxThreads",4),1,4);
    tbb::global_control threads(tbb::global_control::max_allowed_parallelism,size_t(maxThreads));
    const int64_t recommendationMs=std::clamp<int64_t>(request.value("maxRecommendationMs",int64_t(600000)),1,600000);
    const uint64_t memory=std::clamp<uint64_t>(request.value("maxMemoryBytes",uint64_t(2147483648)),1,2147483648ULL);
    Budget budget(canceled_,operation=="recommend"?recommendationMs:0,memory);
    emit(sink,request,"importing",2,"读取工程快照");
    LoadedProject project=load(request,workspace_);
    budget.check();
    if (operation=="import" || operation=="analyze" || operation=="update" || operation=="projectCommand") {
        // update callers submit the full durable command history; an optional
        // single command is appended once to that history before replay.
        if (request.contains("command")) {
            Json commands=request.value("projectCommands",Json::array()); commands.push_back(request["command"]);
            project=load_project(text(request,"inputPath"),request.value("config",Json::object()),
                request.value("transforms",Json::array()),workspace_,commands);
            project.metadata["projectId"]=request.value("projectId",Json());
            project.metadata["revision"]=request.value("projectRevision",request.value("revision",Json(1)));
            project.metadata["quality"]=text(request,"quality","standard");
        }
        // Import may expose an off-bed source for preparation, but an update
        // cannot commit invalid placement and slicing always enforces the gate.
        if (operation=="update" || operation=="projectCommand") validate_project_bounds(project);
        Json description=scene_and_snapshot(project,root(request,workspace_)/artifact_key(request,project));
        budget.check();
        return Json{{"schemaVersion",2},{"state","succeeded"},{"project",description},
            {"scenePath",description.at("scenePath")},{"inputSha256",project.inputSha256}};
    }
    if (operation=="slice" || operation=="validate") {
        const std::string quality=text(request,"quality","standard");
        if (quality!="standard" && quality!="fine" && quality!="fast" && quality!="smart") throw std::invalid_argument("QUALITY_UNSUPPORTED");
        if (quality=="smart" && !request.value("executeEffectiveConfig",false))
            throw std::invalid_argument("SMART_REQUIRES_CONFIRMED_EFFECTIVE_CONFIG");
        if (!request.value("executeEffectiveConfig",false)) project.config=quality_config(project.config,quality);
        return slice_one(project,request,sink,budget,workspace_);
    }
    if (operation=="recommend") {
        const Json baselineConfig=config_json(project.config);
        const auto baselineConfigSha=json_sha256(baselineConfig);
        Json out{{"schemaVersion",2},{"jobId",text(request,"jobId")},{"attemptId",text(request,"attemptId")},
            {"state","succeeded"},{"recommendationState","proposed"},{"projectId",request.value("projectId",Json())},
            {"sourceRevision",project.metadata.at("revision")},{"inputSha256",project.inputSha256},
            {"sourceConfigSha256",baselineConfigSha},{"candidates",Json::array()}};
        const auto candidates=recommendation_candidates(project);
        for (const auto& plan:candidates) {
            budget.check();
            Json trial=describe_candidate(plan);
            trial["sourceRevision"]=project.metadata.at("revision"); trial["sourceConfigSha256"]=baselineConfigSha;
            trial["inputSha256"]=project.inputSha256; trial["configSha256"]=json_sha256(config_json(plan.config));
            trial["schemaVersion"]=2; trial["sourceCommit"]=ORCA_SLICING_SOURCE_COMMIT;
            Json trialRequest=request;
            trialRequest["attemptId"]=text(request,"attemptId")+"-"+plan.candidateId;
            trialRequest["quality"]=plan.quality; trialRequest["executeEffectiveConfig"]=true;
            project.config=plan.config;
            emit(sink,request,"trial",int(out["candidates"].size()*25),"真实试切："+plan.candidateId,Json{{"candidateId",plan.candidateId}});
            try {
                const Json result=slice_one(project,trialRequest,EventSink{},budget,workspace_);
                trial["validated"]=true; trial["state"]="succeeded"; trial["result"]=result;
                trial["requestedConfig"]=trial["config"];
                trial["config"]=result.at("effectiveConfig"); trial["configSha256"]=result.at("configSha256");
                trial["configDiff"]=Json::object();
                for (auto option=trial["config"].begin();option!=trial["config"].end();++option)
                    if (!baselineConfig.contains(option.key()) || baselineConfig[option.key()]!=option.value())
                        trial["configDiff"][option.key()]=Json{{"before",baselineConfig.value(option.key(),Json())},{"after",option.value()}};
                for (const char* key:{"gcodePath","gcodeSize","gcodeSha256","scenePath","sceneSha256","previewPath","previewSha256","layerCount","layerHeights","manifestPath"}) trial[key]=result.at(key);
                trial["metrics"]=Json{{"printTimeSeconds",result["printTimeSeconds"]},{"filamentVolumeMm3",result["filamentVolumeMm3"]},
                    {"filamentGrams",result["filamentGrams"]},{"calculationElapsedMs",result["calculationElapsedMs"]}};
                for (const auto& warning:result["warnings"]) trial["warnings"].push_back(warning);
            } catch (const std::exception& error) {
                budget.check(); // Timeout, memory or user cancellation ends the operation.
                trial["validated"]=false; trial["state"]="failed"; trial["error"]=error.what(); trial["warnings"].push_back(error.what());
            }
            out["candidates"].push_back(std::move(trial));
        }
        try {out.update(select_recommendation(out["candidates"],text(request,"preference","balanced")));}
        catch (const std::exception&) {throw std::runtime_error("NO_VALIDATED_RECOMMENDATION:"+out.dump());}
        emit(sink,request,"recommendation",100,"真实试切候选已完成",out);
        return out;
    }
    throw std::invalid_argument("OPERATION_UNSUPPORTED");
}
} // namespace orca::slicing
