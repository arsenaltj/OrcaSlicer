#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "test_utils.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalSemanticWorkerClient.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalSemanticCachePublication.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace W = Slic3r::GUI::LocalSemanticWorker;
namespace Publication = Slic3r::GUI::LocalSemanticCachePublication;
namespace fs = boost::filesystem;
using Json = nlohmann::json;

TEST_CASE("cache publication never overwrites another directory or crosses its parent", "[LocalSemanticWorkerClient]")
{
    ScopedTemporaryDir temporary;
    const auto pending=temporary.path()/"pending",destination=temporary.path()/"published";
    fs::create_directory(pending);fs::create_directory(destination);
    {fs::ofstream file(destination/"keep.txt");file<<"unrelated contents";}
    std::atomic<bool> cancelled{false};
    const auto existing=Publication::publish(pending,destination,cancelled,[]{return true;});
    CHECK(existing.status==Publication::Status::InvalidOwnership);
    CHECK(existing.attempts==0);
    CHECK(fs::exists(destination/"keep.txt"));
    const auto outside=Publication::publish(pending,destination/"nested",cancelled,[]{return true;});
    CHECK(outside.status==Publication::Status::InvalidOwnership);
    CHECK(outside.attempts==0);
    const auto rejected=Publication::publish(pending,temporary.path()/"unused",cancelled,[]{return false;});
    CHECK(rejected.status==Publication::Status::InvalidOwnership);
    CHECK(fs::exists(pending));
}

#ifdef _WIN32
TEST_CASE("cache publication recovers a held Windows directory without repeating inference", "[LocalSemanticWorkerClient]")
{
    ScopedTemporaryDir temporary;
    const auto pending=temporary.path()/"pending",destination=temporary.path()/"published";
    fs::create_directory(pending);
    {fs::ofstream file(pending/"evidence.json",std::ios::binary);file<<"complete validated bytes";}
    const std::string handle_kind=GENERATE("directory","file");
    const auto held_path=handle_kind=="directory" ? pending : pending/"evidence.json";
    HANDLE held=CreateFileW(held_path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_EXISTING,handle_kind=="directory" ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE(held!=INVALID_HANDLE_VALUE);
    const std::string mode=GENERATE("release","persistent","cancel","ownership");
    std::atomic<bool> cancelled{false};
    std::thread release;
    if(mode=="release" || mode=="cancel") release=std::thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(mode=="cancel") cancelled=true;
        // Keep the obstruction until publish observes cancellation. Releasing
        // it here races an already-started rename and tests a different outcome.
        if(mode=="release") CloseHandle(held);
    });
    unsigned validations=0;
    const auto start=std::chrono::steady_clock::now();
    const auto result=Publication::publish(pending,destination,cancelled,[&] {
        ++validations;
        return fs::exists(pending/"evidence.json") && (mode!="ownership" || validations==1);
    });
    if(release.joinable()) release.join();
    if(mode!="release") CloseHandle(held);
    CAPTURE(handle_kind,mode,result.native_error,result.attempts,validations);
    CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(3));
    CHECK(result.native_error!=0);
    if(mode=="release") {
        REQUIRE(result.status==Publication::Status::Stored);
        CHECK(result.attempts>1);
        CHECK(result.attempts<=20);
        CHECK(validations==result.attempts);
        CHECK_FALSE(fs::exists(pending));
        fs::ifstream file(destination/"evidence.json",std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        CHECK(contents=="complete validated bytes");
    } else {
        CHECK_FALSE(fs::exists(destination));
        CHECK(fs::exists(pending/"evidence.json"));
        if(mode=="persistent") {CHECK(result.status==Publication::Status::Failed);CHECK(result.attempts==20);}
        if(mode=="cancel") {CHECK(result.status==Publication::Status::Cancelled);CHECK(result.attempts<20);}
        if(mode=="ownership") {CHECK(result.status==Publication::Status::InvalidOwnership);CHECK(result.attempts==1);}
    }
}
#endif

namespace {
std::string u8path(const fs::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}
fs::path path8(const std::string& text)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(text));
#else
    return fs::path(text);
#endif
}
Json config_json(const fs::path& root)
{
    return {{"schema","orcaslicer.local-semantic-runtime.v1"},{"enabled",true},
        {"python_executable",u8path(root/"python.exe")},{"weights_directory",u8path(root/"weights")},
        {"cpu_threads",4},{"timeout_seconds",120},{"cache_bytes",1024}};
}
void write(const fs::path& file, const std::string& contents)
{
    fs::ofstream stream(file, std::ios::binary); stream << contents;
}
#ifdef _WIN32
std::uint64_t filetime_value(const FILETIME& time)
{
    return (std::uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
}
// Retain a handle acquired while this fixture's child is alive. Cleanup never
// reopens a numeric PID, so PID reuse cannot terminate an unrelated process.
struct FixtureProcess {
    HANDLE handle=nullptr;
    fs::path owned_directory;
    ~FixtureProcess() { cleanup(); if(handle) CloseHandle(handle); }
    bool exited() const { return handle && WaitForSingleObject(handle,0)==WAIT_OBJECT_0; }
    bool cleanup() {
        if(!handle || exited()) return true;
        // A process already terminating can reject TerminateProcess before its
        // handle signals. Failure cleanup waits that exact acquired handle too.
        TerminateProcess(handle,91);
        return WaitForSingleObject(handle,2000)==WAIT_OBJECT_0;
    }
    bool acquire(const fs::path& root,const std::string& token,std::uint64_t earliest) {
        try {
            const auto ready=root/"grandchild-ready.json",spawn=root/"grandchild-spawn.json";
            if(!fs::exists(ready) || !fs::exists(spawn)) return false;
            if(fs::file_size(ready)>4096 || fs::file_size(spawn)>4096) return false;
            fs::ifstream ready_stream(ready),spawn_stream(spawn);
            const auto record=Json::parse(ready_stream),created=Json::parse(spawn_stream);
            if(record.at("token")!=token || created.at("token")!=token || record.at("pid")!=created.at("pid")) return false;
            const auto pid=record.at("pid").get<std::uint64_t>();
            const auto birth=record.at("creation_time").get<std::uint64_t>();
            const auto owned=path8(record.at("owned").get<std::string>());
            if(owned.parent_path()!=root/"requests" || owned.filename().string().find("mesh-")!=0) return false;
            if(pid==0 || pid>MAXDWORD || pid==GetCurrentProcessId() || birth<earliest) return false;
            HANDLE candidate=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,DWORD(pid));
            if(!candidate) return false;
            FILETIME actual{},exit{},kernel{},user{};
            if(!GetProcessTimes(candidate,&actual,&exit,&kernel,&user) || filetime_value(actual)!=birth) {
                CloseHandle(candidate);return false;
            }
            owned_directory=owned;handle=candidate;return true;
        } catch(...) { return false; }
    }
};
#endif
}

TEST_CASE("Semantic configuration validates explicit absolute paths and limits", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary;
    const auto file=temporary.path()/"config.json";
    const auto config=config_json(temporary.path()/path8(u8"颜色 路径"));
    write(file,config.dump());
    W::Configuration value; std::string error;
    REQUIRE(W::read_configuration(file,value,error));
    CHECK(value.enabled); CHECK(value.cpu_threads==4); CHECK(value.timeout_seconds==120);
    CHECK(u8path(value.python_executable)==config["python_executable"].get<std::string>());
}

TEST_CASE("Installed beauty runtime is relocatable and respects explicit opt out", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary;
    const auto root=temporary.path()/"installed";
    fs::create_directories(root/"python"); fs::create_directories(root/"weights");
    write(root/"python"/"python.exe","fixture");
    for(const char* name:{"mobilenet0.25_Final.pth","face_parsing.farl.celebm.main_ema_181500_jit.pt","face_landmarker.task"})
        write(root/"weights"/name,"fixture");
    const auto file=temporary.path()/"config.json";
    W::Configuration value;std::string reason;
    REQUIRE(W::read_runtime_configuration(file,root,value,reason));
    CHECK(value.enabled);CHECK(value.python_executable==root/"python"/"python.exe");
    auto config=config_json(root);config["enabled"]=false;write(file,config.dump());
    REQUIRE(W::read_runtime_configuration(file,root,value,reason));CHECK_FALSE(value.enabled);
    write(file,"broken");CHECK_FALSE(W::read_runtime_configuration(file,root,value,reason));
}

TEST_CASE("request cleanup is idempotent and cannot remove outside its disposable parent", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary;
    const auto parent=temporary.path()/"requests", owned=parent/"page-owned", outside=temporary.path()/"original";
    fs::create_directories(owned/"mesh");fs::create_directory(outside);
    write(owned/"mesh"/"native.bin","temporary mesh packet");write(outside/"original.glb","preserved original");
    std::string reason;
    CHECK_FALSE(W::cleanup_request(outside,parent,reason));
    CHECK_FALSE(W::cleanup_request(parent,parent,reason));
    CHECK_FALSE(W::cleanup_request(owned/"mesh",parent,reason));
    CHECK(fs::exists(outside/"original.glb"));CHECK(fs::exists(owned/"mesh"/"native.bin"));
    REQUIRE(W::cleanup_request(owned,parent,reason));CHECK(reason.empty());CHECK_FALSE(fs::exists(owned));
    REQUIRE(W::cleanup_request(owned,parent,reason));CHECK(reason.empty());
    CHECK(fs::exists(parent));CHECK(fs::exists(outside/"original.glb"));
}

#ifdef _WIN32
TEST_CASE("request cleanup reports a held file and succeeds after its owner releases it", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary;
    const auto parent=temporary.path()/"requests",owned=parent/"page-held";
    fs::create_directories(owned);
    const auto path=owned/"held.bin";
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE(file!=INVALID_HANDLE_VALUE);
    std::string reason;
    const bool removed_while_held=W::cleanup_request(owned,parent,reason);
    const bool explained=!reason.empty();
    CloseHandle(file); // Release before assertions or a second cleanup attempt.
    CHECK_FALSE(removed_while_held);CHECK(explained);
    REQUIRE(W::cleanup_request(owned,parent,reason));CHECK(reason.empty());CHECK_FALSE(fs::exists(owned));
}
#endif

TEST_CASE("Malformed semantic configuration cannot alter the previous settings", "[LocalSemanticWorkerClient][AI]")
{
    const int variant=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12);
    ScopedTemporaryDir temporary; const auto file=temporary.path()/"config.json";
    auto j=config_json(temporary.path()); std::string raw;
    switch(variant) {
    case 0:j["cpu_threads"]=true; break;
    case 1:j["cpu_threads"]=9; break;
    case 2:j["timeout_seconds"]=0; break;
    case 3:j["cache_bytes"]=-1; break;
    case 4:j["cache_bytes"]=4ULL*1024*1024*1024+1; break;
    case 5:j["python_executable"]="python"; break;
    case 6:j["weights_directory"]="relative"; break;
    case 7:j["command"]="no shell commands"; break;
    case 8:j.erase("schema"); break;
    case 9:raw="{\"schema\":1,\"schema\":2}"; break;
    case 10:raw=std::string(16385,' '); break;
    case 11:raw="{\"x\":NaN}"; break;
    case 12:j["enabled"]=1; break;
    }
    write(file,raw.empty()?j.dump():raw);
    W::Configuration value; value.cpu_threads=7; value.enabled=true;
    std::string error;
    CHECK_FALSE(W::read_configuration(file,value,error));
    CHECK(value.cpu_threads==7); CHECK(value.enabled); CHECK_FALSE(error.empty());
}

TEST_CASE("Disabled and pre-cancelled semantics start no process or request directory", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary; std::atomic<bool> cancelled {false}; W::Configuration config;
    auto result=W::probe(config,temporary.path()/"missing.py",temporary.path()/"requests",cancelled);
    CHECK(result.status==W::Status::Disabled); CHECK_FALSE(fs::exists(temporary.path()/"requests"));
    cancelled=true; config.enabled=true;
    result=W::probe(config,temporary.path()/"missing.py",temporary.path()/"requests",cancelled);
    CHECK(result.status==W::Status::Cancelled); CHECK_FALSE(fs::exists(temporary.path()/"requests"));
}

TEST_CASE("Missing local runtimes leave ordinary operation available", "[LocalSemanticWorkerClient][AI]")
{
    ScopedTemporaryDir temporary; W::Configuration config; config.enabled=true;
    config.python_executable=temporary.path()/"missing.exe"; config.weights_directory=temporary.path();
    std::atomic<bool> cancelled {false};
    const auto result=W::probe(config,temporary.path()/"missing.py",temporary.path()/"requests",cancelled);
    CHECK(result.status==W::Status::Unavailable); CHECK(result.reason=="local_runtime_missing");
    CHECK_FALSE(fs::exists(temporary.path()/"requests"));
}

TEST_CASE("Owned semantic workers handle protocol failure cancellation and timeout", "[LocalSemanticWorkerClient][AI]")
{
    // Explicit stdlib interpreter opt-in. This never imports torch or providers.
    const char* configured_value=boost::nowide::getenv("ORCA_LOCAL_SEMANTIC_TEST_PYTHON");
    if (!configured_value || !*configured_value) SKIP("Set ORCA_LOCAL_SEMANTIC_TEST_PYTHON for owned-child integration tests.");
    const std::string configured(configured_value);
    const auto scenario=GENERATE("ready","failure","malformed","oversize","crash","cancel","timeout","stale","incomplete","wronglabels","wrongweights","fingerprint");
    ScopedTemporaryDir temporary;
    const auto root=temporary.path()/path8(u8"本地 worker 路径"); fs::create_directory(root);
    const auto script=root/"fake worker.py";
    std::string source=R"PY(import argparse, hashlib, json, os, pathlib, sys, time
p=argparse.ArgumentParser(); p.add_argument('--probe',action='store_true'); p.add_argument('--config'); p.add_argument('--output'); a=p.parse_args()
allowed={'SYSTEMROOT','WINDIR','TEMP','TMP','TMPDIR','PYTHONNOUSERSITE','HF_HUB_OFFLINE','TRANSFORMERS_OFFLINE','HF_HUB_DISABLE_TELEMETRY','OMP_NUM_THREADS','MKL_NUM_THREADS','LC_CTYPE'}
assert not (set(os.environ)-allowed)
config=json.loads(pathlib.Path(a.config).read_text(encoding='utf-8')); assert pathlib.Path(config['python_executable']).is_absolute()
mode=MODE
if mode in ('cancel','timeout'): time.sleep(30)
if mode=='crash': sys.exit(9)
out=pathlib.Path(a.output)
if mode=='oversize': out.write_text('x'*70000); time.sleep(30)
if mode=='malformed': out.write_text('{broken'); sys.exit(0)
r={'schema':'orcaslicer.local-semantic-worker.v1','worker_version':'local-semantic-cpu-v1','status':'ok','capability_ready':True,
 'identity':{'worker_version':'local-semantic-cpu-v1','worker_sha256':hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),'python_executable_sha256':hashlib.sha256(pathlib.Path(sys.executable).read_bytes()).hexdigest(),'device':'cpu','bits':64,
 'python':'3.12.14','packages':dict.fromkeys(['torch','torchvision','pyfacer','numpy','Pillow'],'mock-only'),
 'weights':{'mobilenet0.25_Final.pth':'2979b33ffafda5d74b6948cd7a5b9a7a62f62b949cef24e95fd15d2883a65220','face_parsing.farl.celebm.main_ema_181500_jit.pt':'bbc1f0e9f68c80eb83a0b23f33850d1e10f2ec1eda96884112d111c2c1f15c79'}},
 'label_schema':'farl-celebm-19-v1','label_names':['background','neck','face','cloth','rr','lr','rb','lb','re','le','nose','imouth','llip','ulip','hair','eyeg','hat','earr','neck_l'],
 'supported_labels':['neck','face','rr','lr','rb','lb','re','le','nose','imouth','llip','ulip','hair','cloth'],
 'unsupported':['teeth','pupil','eye_white','body_instance_segmentation'],
 'network_policy':'python-audit-hook-and-offline-flags; not-native-OS-sandbox','mesh_requests_ready':False,'probe_seconds':0}
r['runtime_fingerprint']=hashlib.sha256(json.dumps(r['identity'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
if mode=='failure': r.update(status='unavailable',capability_ready=False,error_code='local_probe_failed')
if mode=='stale': r['identity']['worker_sha256']='0'*64
if mode=='incomplete': del r['identity']['packages']
if mode=='wronglabels': r['supported_labels']=['teeth']
if mode=='wrongweights': r['identity']['weights']['mobilenet0.25_Final.pth']='0'*64
if mode=='fingerprint': r['runtime_fingerprint']='0'*64
out.write_text(json.dumps(r))
sys.exit(2 if mode=='failure' else 0)
)PY";
    source.replace(source.find("MODE"),4,Json(scenario).dump()); write(script,source);
    W::Configuration config; config.enabled=true; config.python_executable=path8(configured);
    config.weights_directory=root; config.timeout_seconds=10;
    std::atomic<bool> cancelled {false};
    std::thread cancellation;
    if (std::string(scenario)=="cancel") cancellation=std::thread([&cancelled] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250)); cancelled=true;
    });
    const auto started=std::chrono::steady_clock::now();
    const auto result=W::probe(config,script,root/"requests",cancelled);
    if(cancellation.joinable()) cancellation.join();
    const auto elapsed=std::chrono::steady_clock::now()-started;
    CAPTURE(scenario,result.reason,result.exit_code);
    if (std::string(scenario)=="ready") CHECK(result.status==W::Status::Ready);
    else if (std::string(scenario)=="cancel") CHECK(result.status==W::Status::Cancelled);
    else if (std::string(scenario)=="timeout") CHECK(result.status==W::Status::TimedOut);
    else CHECK(result.status==W::Status::Unavailable);
    const std::map<std::string,std::string> reasons {{"ready",""},{"failure","semantic_probe_unavailable"},
        {"malformed","invalid_semantic_response"},{"oversize","semantic_output_too_large"},
        {"crash","local_worker_response_missing_or_large"},{"cancel","cancelled"},{"timeout","semantic_timeout"},
        {"stale","invalid_semantic_response"},{"incomplete","invalid_semantic_response"},
        {"wronglabels","invalid_semantic_response"},{"wrongweights","invalid_semantic_response"},{"fingerprint","invalid_semantic_response"}};
    CHECK(result.reason==reasons.at(scenario));
    CHECK(elapsed<std::chrono::seconds(20));
    CHECK(fs::is_directory(result.request_directory));
}

TEST_CASE("Configured local CPU models execute through the desktop owned worker", "[.][LocalSemanticWorkerRealProbe]")
{
    // Opt-in local model execution only; no providers or user model input.
    const auto environment=[](const char* key) { const char* p=boost::nowide::getenv(key); return p ? std::string(p) : std::string(); };
    const auto config_file=environment("ORCA_LOCAL_SEMANTIC_RUNTIME_CONFIG");
    const auto worker=environment("ORCA_LOCAL_SEMANTIC_WORKER_SCRIPT");
    const auto evidence=environment("ORCA_LOCAL_SEMANTIC_PROBE_EVIDENCE");
    if (config_file.empty() || worker.empty() || evidence.empty()) SKIP("Explicit local configuration, installed worker and evidence directory required.");
    W::Configuration config; std::string error;
    REQUIRE(W::read_configuration(path8(config_file),config,error));
    std::atomic<bool> cancelled {false};
    const auto result=W::probe(config,path8(worker),path8(evidence),cancelled);
    CAPTURE(result.reason,result.exit_code,u8path(result.request_directory));
    REQUIRE(result.status==W::Status::Ready);
    const auto report=Json::parse(result.response_json);
    CHECK(report.at("capability_ready")==true);
    CHECK(report.at("mesh_requests_ready")==false);
    CHECK(report.at("identity").at("device")=="cpu");
    CHECK(report.at("supported_labels").size()==14);
}

TEST_CASE("local mesh requests preserve disabled cancelled and invalid path outcomes without launching", "[LocalSemanticWorkerClient]")
{
    W::Configuration config;std::atomic<bool> cancelled{false};indexed_triangle_set mesh;
    auto result=W::analyze(config,{}, {}, {},mesh,cancelled);
    CHECK(result.process.status==W::Status::Disabled);CHECK(result.evidence.regions.empty());
    cancelled=true;result=W::analyze(config,{}, {}, {},mesh,cancelled);
    CHECK(result.process.status==W::Status::Cancelled);CHECK(result.evidence.regions.empty());
    cancelled=false;config.enabled=true;result=W::analyze(config,"relative",{}, {},mesh,cancelled);
    CHECK(result.process.status==W::Status::Unavailable);CHECK(result.process.reason=="invalid_semantic_input_paths");
    CHECK(result.process.request_directory.empty());
}

TEST_CASE("configured local mesh analysis reaches native face proof through the owned request process", "[.][LocalSemanticWorkerRealMesh]")
{
    const auto environment=[](const char* key) {const char* value=boost::nowide::getenv(key);return value?std::string(value):std::string();};
    const auto config_file=environment("ORCA_LOCAL_SEMANTIC_RUNTIME_CONFIG");
    const auto directory=environment("ORCA_LOCAL_SEMANTIC_PACKAGE_DIRECTORY");
    const auto source=environment("ORCA_LOCAL_SEMANTIC_MESH_SOURCE");
    const auto evidence=environment("ORCA_LOCAL_SEMANTIC_REQUEST_EVIDENCE");
    if(config_file.empty() || directory.empty() || source.empty() || evidence.empty())
        SKIP("Explicit configured local model, installed module directory and fresh evidence path required.");
    W::Configuration config;std::string error;
    REQUIRE(W::read_configuration(path8(config_file),config,error));
    Slic3r::TriangleMesh native;Slic3r::ObjInfo colors;
    REQUIRE(Slic3r::AI::load_model_artifact(path8(source),native,colors,error));
    REQUIRE_FALSE(fs::exists(path8(evidence)));
    std::atomic<bool> cancelled{false};
    const auto result=W::analyze(config,path8(directory),path8(evidence),path8(source),native.its,cancelled);
    CAPTURE(result.process.reason,result.process.exit_code,u8path(result.process.request_directory));
    REQUIRE(result.process.status==W::Status::Ready);
    CHECK(result.evidence.face_regions.size()==native.its.indices.size());
    CHECK(result.evidence.known_faces+result.evidence.unknown_faces==native.its.indices.size());
    CHECK(result.evidence.known_faces>0);
    for(const auto& region:result.evidence.regions) {
        CHECK_FALSE(region.user_protected);CHECK_FALSE(region.locked_physical_slot.has_value());
    }
    write(path8(evidence)/"host-acceptance.json",Json({{"request_directory",u8path(result.process.request_directory)},
        {"source_sha256",result.evidence.identity.source_sha256},{"geometry_id",result.evidence.identity.geometry_id},
        {"known_faces",result.evidence.known_faces},{"unknown_faces",result.evidence.unknown_faces},
        {"regions",result.evidence.regions.size()},{"runtime_sha256",result.evidence.identity.runtime_sha256},
        {"policy_sha256",result.evidence.identity.policy_sha256},{"scope","actual native owned request and evidence proof; not GUI/printing"}}).dump(2));
}

TEST_CASE("owned mesh requests reject invalid payloads and stop mesh stage cancellation and timeout", "[LocalSemanticWorkerClient][AI]")
{
    // All nine modules are local stdlib-only fixtures. The source is synthetic;
    // the actual host packet codec/proof/evidence decoder are still exercised.
    const char* configured_value=boost::nowide::getenv("ORCA_LOCAL_SEMANTIC_TEST_PYTHON");
    if(!configured_value || !*configured_value) SKIP("Set ORCA_LOCAL_SEMANTIC_TEST_PYTHON for owned mesh process tests.");
    const std::string configured(configured_value);
    const auto scenario=GENERATE("ready","ready-eyes","identity-eyes","negative-stat","huge-views","wrong-count","oversize-partial",
                                 "reordered","manifest","cancel","timeout","ready-orphan",
                                 "identity-status","identity-capability","identity-fingerprint","identity-weights",
                                 "cache-hit","cache-source","cache-geometry","cache-module","cache-threads",
                                 "cache-corrupt","cache-proof","cache-statistics","cache-zero","cache-tiny",
                                 "cache-blocked","cache-disable","cache-evict","cache-cancel",
                                 "cache-pending","cache-hardlink","cache-foreign");
    const std::string mode(scenario);
    ScopedTemporaryDir temporary;
    const auto root=temporary.path()/path8(u8"本地 mesh 模块");fs::create_directory(root);
    const auto source=root/"source.glb";
    write(source,"synthetic source bytes; no GLB parsing or models in this fixture");
    const auto fixture_token=fs::unique_path("owned-%%%%-%%%%-%%%%").string();
    std::string script=R"PY(import argparse, hashlib, json, os, pathlib, struct, subprocess, sys, time
p=argparse.ArgumentParser();p.add_argument('--probe',action='store_true');p.add_argument('--identity-only',action='store_true');p.add_argument('--request');p.add_argument('--config');p.add_argument('--output');a=p.parse_args()
mode=MODE
token=FIXTURE_TOKEN
root=pathlib.Path(__file__).parent
allowed={'SYSTEMROOT','WINDIR','TEMP','TMP','TMPDIR','PYTHONNOUSERSITE','HF_HUB_OFFLINE','TRANSFORMERS_OFFLINE','HF_HUB_DISABLE_TELEMETRY','OMP_NUM_THREADS','MKL_NUM_THREADS','LC_CTYPE'}
assert not (set(os.environ)-allowed)
config=json.loads(pathlib.Path(a.config).read_text(encoding='utf-8'))
assert pathlib.Path(config['python_executable']).is_absolute()
def canonical(value):return json.dumps(value,sort_keys=True,separators=(',',':')).encode()
def sha(raw):return hashlib.sha256(raw).hexdigest()
weights={'mobilenet0.25_Final.pth':'2979b33ffafda5d74b6948cd7a5b9a7a62f62b949cef24e95fd15d2883a65220','face_parsing.farl.celebm.main_ema_181500_jit.pt':'bbc1f0e9f68c80eb83a0b23f33850d1e10f2ec1eda96884112d111c2c1f15c79'}
probe_id={'worker_version':'local-semantic-cpu-v1','worker_sha256':sha((root/'local_semantic_worker.py').read_bytes()),
 'python':sys.version.split()[0],'python_executable_sha256':sha(pathlib.Path(sys.executable).read_bytes()),
 'bits':64,'packages':dict.fromkeys(['torch','torchvision','pyfacer','numpy','Pillow'],'mock-only'),'weights':weights,'device':'cpu'}
if mode in ('ready-eyes','identity-eyes'):
 weights['face_landmarker.task']='64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff'
 probe_id['packages']['mediapipe']='1.0.1' if mode=='ready-eyes' else 'incompatible'
if a.probe:
 assert a.identity_only, 'Mesh analysis must not run the synthetic model probe'
 (root/'identity-inspected').write_text('metadata only',encoding='utf-8')
 r={'schema':'orcaslicer.local-semantic-worker.v1','worker_version':'local-semantic-cpu-v1','status':'identity','capability_ready':False,
  'identity':probe_id,'runtime_fingerprint':sha(canonical(probe_id)),'label_schema':'farl-celebm-19-v1',
  'label_names':['background','neck','face','cloth','rr','lr','rb','lb','re','le','nose','imouth','llip','ulip','hair','eyeg','hat','earr','neck_l'],
  'supported_labels':['neck','face','rr','lr','rb','lb','re','le','nose','imouth','llip','ulip','hair','cloth'],
  'unsupported':['teeth','pupil','eye_white','body_instance_segmentation'],
  'network_policy':'python-audit-hook-and-offline-flags; not-native-OS-sandbox','mesh_requests_ready':False,'probe_seconds':0}
 if mode=='identity-status':r['status']='ok'
 if mode=='identity-capability':r['capability_ready']=True
 if mode=='identity-fingerprint':r['runtime_fingerprint']='0'*64
 if mode=='identity-weights':r['identity']['weights']['mobilenet0.25_Final.pth']='0'*64;r['runtime_fingerprint']=sha(canonical(r['identity']))
 pathlib.Path(a.output).write_bytes(canonical(r));sys.exit(0)
with (root/'mesh-started').open('ab') as marker:marker.write(b'mesh\n')
request_path=pathlib.Path(a.request);owned=request_path.parent
if os.name=='nt' and mode in ('cancel','timeout','ready-orphan'):
 child_code=r'''import ctypes, json, os, pathlib, sys, time
from ctypes import wintypes
root=pathlib.Path(sys.argv[1]);owned=pathlib.Path(sys.argv[2]);token=sys.argv[3]
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.GetCurrentProcess.restype=wintypes.HANDLE
k.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
birth=wintypes.FILETIME();end=wintypes.FILETIME();kernel=wintypes.FILETIME();user=wintypes.FILETIME()
assert k.GetProcessTimes(k.GetCurrentProcess(),ctypes.byref(birth),ctypes.byref(end),ctypes.byref(kernel),ctypes.byref(user))
# A real open file prevents Windows directory cleanup until this process exits.
with (owned/'grandchild-held.bin').open('wb') as held:
 held.write(b'owned fixture only');held.flush()
 record={'pid':os.getpid(),'creation_time':(birth.dwHighDateTime<<32)|birth.dwLowDateTime,'token':token,'owned':str(owned)}
 partial=root/'grandchild-ready.partial';partial.write_text(json.dumps(record),encoding='utf-8');partial.replace(root/'grandchild-ready.json')
 time.sleep(30)
'''
 child=subprocess.Popen([getattr(sys,'_base_executable',sys.executable),'-I','-c',child_code,str(root),str(owned),token],
                        stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                        creationflags=subprocess.CREATE_NO_WINDOW)
 partial=root/'grandchild-spawn.partial';partial.write_text(json.dumps({'pid':child.pid,'token':token}),encoding='utf-8');partial.replace(root/'grandchild-spawn.json')
 # The C++ observer opens an identity-checked process handle before allowing
 # this worker to exit, so even the normal-return orphan is deterministically observed.
 deadline=time.monotonic()+5
 while not (root/'grandchild-observed').exists():
  if time.monotonic()>deadline:
   child.terminate();child.wait(timeout=2);raise RuntimeError('fixture observer did not acknowledge child')
  time.sleep(.01)
if mode in ('cancel','timeout'):
 if os.name=='nt':child.wait(timeout=30)
 else:time.sleep(30)
q=json.loads(request_path.read_text(encoding='utf-8'))
names=['glb_artifact.py','local_semantic_worker.py','local_semantic_geometry.py','local_semantic_render.py','local_semantic_transform.py','local_semantic_views.py','local_semantic_projection.py','local_semantic_pipeline.py','local_semantic_request.py','local_eye_landmarks.py','local_face_landmarks.py','local_body_regions.py']
modules={name:sha((root/name).read_bytes()) for name in names}
identity={'probe_identity':probe_id,'modules_sha256':modules}
runtime=sha(canonical(identity))
policy=sha(canonical({'version':'visible-face-semantic-v5-body-supplement','label_schema':'farl-celebm-face19-subset-v1',
 'modules_sha256':{name:modules[name] for name in names if name in ['local_semantic_render.py','local_semantic_transform.py','local_semantic_views.py','local_semantic_projection.py','local_semantic_pipeline.py','local_eye_landmarks.py','local_face_landmarks.py','local_body_regions.py']}}))
assert runtime==q['runtime_fingerprint'] and policy==q['policy_sha256']
assert sha(pathlib.Path(q['source_path']).read_bytes())==q['source_sha256']
native=(owned/'native.bin').read_bytes()
magic,nv,nf,source_sha,geom=struct.unpack_from('<8sQQ64s64s',native)
assert magic==b'ORCASG01' and nf==q['face_count'] and geom.decode()==q['geometry_id']
assert sha(native[:-32])==native[-32:].hex()
rendered=native;render_geometry=geom.decode()
if mode=='reordered':
 vertices=[struct.unpack_from('<3f',native,152+i*12) for i in range(nv)]
 faces=[struct.unpack_from('<3I',native,152+nv*12+i*12) for i in range(nf)]
 faces[0],faces[1]=faces[1],faces[0]
 fingerprint=hashlib.sha256(b'orca.surface-selection.geometry/v1\0'+struct.pack('<Q',nf))
 for face in faces:
  for v in face:fingerprint.update(struct.pack('<3f',*[0. if x==0 else x for x in vertices[v]]))
 render_geometry=fingerprint.hexdigest()
 data=struct.pack('<8sQQ64s64s',magic,nv,nf,source_sha,render_geometry.encode())+native[152:152+nv*12]+b''.join(struct.pack('<3I',*face) for face in faces)
 rendered=data+hashlib.sha256(data).digest()
evidence={'schema':'orcaslicer.local-semantic-evidence.v1','label_schema':'farl-celebm-face19-subset-v1',
 'request_id':q['request_id'],'source_sha256':q['source_sha256'],'geometry_id':q['geometry_id'],'render_geometry_id':render_geometry,
 'face_count':nf,'weights_sha256':sha(canonical(weights)),'runtime_sha256':runtime,'policy_sha256':policy,
 'subjects':['surface-fixture'],'regions':[{'subject_id':'surface-fixture','label':'face','samples':[[0,.99,1.,2,1]]}]}
payloads={'rendered.bin':rendered,'evidence.json':canonical(evidence)}
files={}
for name,raw in payloads.items():
 (owned/name).write_bytes(raw);files[name]={'bytes':len(raw),'sha256':sha(raw)}
statistics={'face_count':nf,'observations':1,'views':1,'view_families':1,'raw_pixels':2,
 'visible_faces':1,'unseen_faces':nf-1,'associated_components':1,'ambiguous_components':0,
 'ambiguous_faces':0,'cross_subject_faces':0,'below_threshold_faces':0,'known_faces':1,'unknown_faces':nf-1,
 'render_visible_faces':1,'render_unseen_faces':nf-1}
if mode=='negative-stat':statistics['raw_pixels']=-1
if mode=='huge-views':statistics['views']=2**64-1
if mode=='wrong-count':statistics['known_faces']=0
if mode=='manifest':files['rendered.bin']['sha256']='0'*64
response={'schema':'orcaslicer.local-semantic-response.v1','worker_version':'local-semantic-request-cpu-v1',
 'request_id':q['request_id'],'status':'ok','identity':identity,'runtime_fingerprint':runtime,'policy_sha256':policy,
 'files':files,'statistics':statistics}
pathlib.Path(a.output).write_bytes(canonical(response))
if mode=='oversize-partial':(owned/'result.json.partial').write_bytes(b'x'*65537)
# Exit immediately: no artificial sleep gives the monitor a guaranteed poll.
os._exit(0)
)PY";
    script.replace(script.find("MODE"),4,Json(scenario).dump());
    script.replace(script.find("FIXTURE_TOKEN"),13,Json(fixture_token).dump());
    for(const char* name:{"glb_artifact.py","local_semantic_geometry.py","local_semantic_render.py",
                          "local_semantic_transform.py","local_semantic_views.py","local_semantic_projection.py","local_semantic_pipeline.py","local_eye_landmarks.py","local_face_landmarks.py","local_body_regions.py"})
        write(root/name,"# inert fixture module; no third-party dependencies\n");
    write(root/"local_semantic_worker.py",script);write(root/"local_semantic_request.py",script);
    indexed_triangle_set native;
    native.vertices.emplace_back(0.f,0.f,0.f);native.vertices.emplace_back(1.f,0.f,0.f);
    native.vertices.emplace_back(0.f,1.f,0.f);native.vertices.emplace_back(0.f,0.f,1.f);
    native.indices.emplace_back(0,2,1);native.indices.emplace_back(0,1,3);
    native.indices.emplace_back(1,2,3);native.indices.emplace_back(2,0,3);
    W::Configuration config;config.enabled=true;config.python_executable=path8(configured);
    config.weights_directory=root;config.timeout_seconds=10;
    const auto cache=root/path8(u8"识别缓存");
    if(mode=="cache-zero") config.cache_bytes=0;
    if(mode=="cache-tiny") config.cache_bytes=1;
    if(mode=="cache-blocked") write(cache,"An existing ordinary file must remain untouched.");
    std::atomic<bool> cancelled{false},mesh_observed{false},observer_done{false};
    std::thread cancellation;
#ifdef _WIN32
    const bool expects_descendant=std::string(scenario)=="cancel" || std::string(scenario)=="timeout" || std::string(scenario)=="ready-orphan";
    FixtureProcess descendant;
    FILETIME earliest{};GetSystemTimeAsFileTime(&earliest);
    if(expects_descendant) cancellation=std::thread([&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!observer_done && std::chrono::steady_clock::now()<deadline) {
            if(descendant.acquire(root,fixture_token,filetime_value(earliest))) {
                write(root/"grandchild-observed","identity-checked owned child handle acquired");
                mesh_observed=true;
                if(std::string(scenario)=="cancel") cancelled=true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
#else
    if(std::string(scenario)=="cancel") cancellation=std::thread([&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(!observer_done && std::chrono::steady_clock::now()<deadline) {
            boost::system::error_code error;
            if(fs::exists(root/"mesh-started",error)) {mesh_observed=true;cancelled=true;return;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
#endif
    const auto started=std::chrono::steady_clock::now();
    const auto result=[&] {
        try { return W::analyze(config,root,root/"requests",source,native,cancelled,cache); }
        catch(...) {
            observer_done=true;
            if(cancellation.joinable()) cancellation.join();
            throw;
        }
    }();
    observer_done=true;
    if(cancellation.joinable()) cancellation.join();
    const auto elapsed=std::chrono::steady_clock::now()-started;
    CAPTURE(scenario,result.process.reason,result.process.exit_code,
        result.cache_write.stored,result.cache_write.operation,result.cache_write.native_error,result.cache_write.publish_attempts);
#ifdef _WIN32
    if(expects_descendant) {
        // Observe before cleanup: cleanup must not make a broken host pass.
        const bool stopped_by_host=descendant.exited();
        const bool cleanup_complete=descendant.cleanup();
        CHECK(mesh_observed.load());
        CHECK(stopped_by_host);
        CHECK(cleanup_complete);
        // Process exit has released its held file; check the
        // actual request artifacts can be removed without touching other roots.
        if(descendant.handle) {
            CHECK(result.process.request_directory==descendant.owned_directory);
            boost::system::error_code remove_error;
            fs::remove(descendant.owned_directory/"grandchild-held.bin",remove_error);
            CHECK_FALSE(bool(remove_error));
        }
    }
#endif
    REQUIRE(fs::exists(root/"identity-inspected"));
    if(std::string(scenario).find("identity-")==0) {
        CHECK_FALSE(fs::exists(root/"mesh-started"));
        CHECK(result.process.status==W::Status::Unavailable);
        CHECK(result.process.reason=="invalid_semantic_response");
        CHECK(result.evidence.regions.empty());
        return;
    }
    REQUIRE(fs::exists(root/"mesh-started"));
    CHECK(result.process.request_directory.filename().string().find("mesh-")==0);
    CHECK(elapsed<std::chrono::seconds(20));
    if(mode=="ready" || mode=="ready-eyes" || mode=="ready-orphan" || mode.find("cache-")==0) {
        REQUIRE(result.process.status==W::Status::Ready);
        CHECK(result.evidence.known_faces==1);CHECK(result.evidence.unknown_faces==3);
        REQUIRE(result.evidence.regions.size()==1);
        REQUIRE(result.evidence.regions[0].faces.size()==1);
        CHECK(result.evidence.regions[0].faces[0]==0);
        CHECK_FALSE(result.evidence.regions[0].user_protected);
        CHECK_FALSE(result.evidence.regions[0].locked_physical_slot.has_value());
        CHECK_FALSE(result.cache_hit);
        if(mode.find("cache-")==0) {
            const auto read=[](const fs::path& file) { fs::ifstream input(file,std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()); };
            const auto entries=[&] {
                std::vector<fs::path> paths;
                if(fs::is_directory(cache)) for(const auto& entry:fs::directory_iterator(cache))
                    if(fs::is_directory(entry.path())) paths.push_back(entry.path());
                return paths;
            };
            const auto disk_bytes=[&] {
                unsigned long long size=0;
                if(fs::is_directory(cache)) for(const auto& entry:fs::recursive_directory_iterator(cache))
                    if(fs::is_regular_file(entry.path())) size+=fs::file_size(entry.path());
                return size;
            };
            const bool no_storage=mode=="cache-zero" || mode=="cache-tiny" || mode=="cache-blocked";
            if(no_storage) {
                CHECK(entries().empty());CHECK_FALSE(result.cache_write.stored);
            } else {
                REQUIRE(result.cache_write.stored);
                CHECK(result.cache_write.operation=="stored");
                CHECK(result.cache_write.publish_attempts>=1);
                REQUIRE(entries().size()==1);
            }
            if(mode=="cache-source" || mode=="cache-evict") write(source,read(source)+" changed source");
            if(mode=="cache-geometry") native.vertices[0].x()=.25f;
            if(mode=="cache-module") write(root/"local_semantic_projection.py","# changed local projection module\n");
            if(mode=="cache-threads") config.cpu_threads=2;
            if(mode=="cache-disable") config.cache_bytes=0;
            if(mode=="cache-evict") config.cache_bytes=disk_bytes()+128;
            if(mode=="cache-corrupt") write(entries()[0]/"evidence.json","truncated cached evidence");
            if(mode=="cache-pending") {
                const auto key=entries()[0].filename().string();
                const auto pending=cache/("pending-"+key+"-interrupted");fs::create_directory(pending);
                write(pending/"owner.json",read(entries()[0]/"owner.json"));
                write(pending/"rendered.bin","interrupted owned write");
            }
            if(mode=="cache-hardlink") {
                const auto file=entries()[0]/"evidence.json";
                fs::create_hard_link(file,root/"external-evidence.json");
            }
            if(mode=="cache-foreign") write(cache/"user-note.txt","Preserve unrelated local content.");
            if(mode=="cache-proof" || mode=="cache-statistics") {
                const auto entry=entries()[0];
                auto response=Json::parse(read(entry/"result.json"));
                if(mode=="cache-proof") {
                    auto reordered=native;std::swap(reordered.indices[0],reordered.indices[1]);
                    std::string packet,error;
                    REQUIRE(Slic3r::GUI::LocalSemanticGeometry::encode(reordered,
                        Slic3r::AI::model_artifact_sha256(source),packet,error));
                    write(entry/"rendered.bin",packet);
                    auto evidence=Json::parse(read(entry/"evidence.json"));
                    evidence["render_geometry_id"]=Slic3r::AI::SurfaceSelectionPersistence::geometry_fingerprint(reordered);
                    write(entry/"evidence.json",evidence.dump());
                } else response["statistics"]["known_faces"]=0;
                // Recompute every manifest hash: even an internally consistent
                // cache still has to pass native proof and semantic statistics.
                for(const char* name:{"rendered.bin","evidence.json"})
                    response["files"][name]={{"bytes",fs::file_size(entry/name)},
                        {"sha256",Slic3r::AI::model_artifact_sha256(entry/name)}};
                write(entry/"result.json",response.dump());
                auto manifest=Json::parse(read(entry/"manifest.json"));
                for(const char* name:{"rendered.bin","evidence.json","result.json"})
                    manifest["files"][name]={{"bytes",fs::file_size(entry/name)},
                        {"sha256",Slic3r::AI::model_artifact_sha256(entry/name)}};
                write(entry/"manifest.json",manifest.dump());
            }
            std::string cleanup_error;
            REQUIRE(W::cleanup_request(root/"requests",root,cleanup_error));
            if(mode=="cache-cancel") cancelled=true;
            const auto again=W::analyze(config,root,root/"second-requests",source,native,cancelled,cache);
            CAPTURE(again.process.reason);
            if(mode=="cache-cancel") {
                CHECK(again.process.status==W::Status::Cancelled);CHECK_FALSE(again.cache_hit);
                CHECK_FALSE(fs::exists(root/"second-requests"));CHECK(read(root/"mesh-started")=="mesh\n");
            } else {
                REQUIRE(again.process.status==W::Status::Ready);
                const bool expected_hit=mode=="cache-hit" || mode=="cache-pending";
                CHECK(again.cache_hit==expected_hit);
                CHECK(read(root/"mesh-started")== (expected_hit ? "mesh\n" : "mesh\nmesh\n"));
                CHECK(again.evidence.known_faces==result.evidence.known_faces);
                CHECK(again.evidence.regions[0].faces==result.evidence.regions[0].faces);
                if(mode=="cache-hit") {
                    CHECK(again.process.response_json==result.process.response_json);
                    CHECK_FALSE(fs::exists(again.process.request_directory/"rendered.bin"));
                }
            }
            if(mode=="cache-evict") {CHECK(entries().size()==1);CHECK(disk_bytes()<=config.cache_bytes);}
            if(mode=="cache-pending") CHECK(entries().size()==1);
            if(mode=="cache-hardlink") CHECK(read(root/"external-evidence.json")==read(entries()[0]/"evidence.json"));
            if(mode=="cache-foreign") CHECK(read(cache/"user-note.txt")=="Preserve unrelated local content.");
            if(no_storage) CHECK(entries().empty());
            if(mode=="cache-blocked") CHECK(read(cache)=="An existing ordinary file must remain untouched.");
        }
    } else {
        CHECK(result.evidence.regions.empty());CHECK(result.evidence.face_regions.empty());
        CHECK_FALSE(result.cache_hit);
        if(fs::is_directory(cache)) for(const auto& entry:fs::directory_iterator(cache))
            CHECK_FALSE(fs::is_directory(entry.path())); // No failed or cancelled result is promoted.
        if(std::string(scenario)=="cancel") {
            CHECK(mesh_observed.load());CHECK(result.process.status==W::Status::Cancelled);
            CHECK(result.process.reason=="cancelled");
        } else if(std::string(scenario)=="timeout") {
            CHECK(result.process.status==W::Status::TimedOut);CHECK(result.process.reason=="semantic_timeout");
        } else {
            CHECK(result.process.status==W::Status::Unavailable);
            if(std::string(scenario)=="oversize-partial") CHECK(result.process.reason=="semantic_output_too_large");
            else if(std::string(scenario)=="reordered" || std::string(scenario)=="manifest")
                CHECK(result.process.reason=="semantic_native_face_proof_failed");
            else CHECK(result.process.reason=="invalid_semantic_evidence");
        }
    }
}
