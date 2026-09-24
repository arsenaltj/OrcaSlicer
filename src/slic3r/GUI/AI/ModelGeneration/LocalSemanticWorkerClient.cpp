#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif
#include "LocalSemanticWorkerClient.hpp"
#include "LocalSemanticCachePublication.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/process.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#ifdef _WIN32
#include <boost/process/windows.hpp>
#include <boost/process/extend.hpp>
#endif
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <chrono>
#include <algorithm>
#include <ctime>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace Slic3r::GUI::LocalSemanticWorker {
namespace {
namespace fs = boost::filesystem;
namespace process = boost::process;
using Json = nlohmann::json;
constexpr size_t max_config_bytes = 16 * 1024;
constexpr size_t max_response_bytes = 64 * 1024;
constexpr const char* schema = "orcaslicer.local-semantic-worker.v1";

// Each launch owns a new group. In particular, a Windows venv launcher exiting
// does not imply that its worker or that worker's descendants have exited.
struct OwnedProcess {
    std::unique_ptr<process::group> group;
    std::unique_ptr<process::child> child;
    bool stopped=false;
    bool complete=true;
#ifdef _WIN32
    HANDLE primary_thread=nullptr; // Borrowed from child, never closed separately.
#endif
    ~OwnedProcess() { stop(); }
    void prepare() {
        // Called inside the caller's try, including the throwing group constructor.
        group=std::make_unique<process::group>();
#ifdef _WIN32
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        const auto job=group->native_handle();
        if(!QueryInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr))
            throw std::runtime_error("owned_job_query_failed");
        // Boost enables breakaway by default. This private request job must retain
        // descendants, and closing its final handle is a failure-path backstop.
        limits.BasicLimitInformation.LimitFlags&=~(JOB_OBJECT_LIMIT_BREAKAWAY_OK|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK);
        limits.BasicLimitInformation.LimitFlags|=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            throw std::runtime_error("owned_job_limits_failed");
#endif
    }
#ifdef _WIN32
    void resume() {
        std::error_code error;
        if(!primary_thread || !group->has(*child,error) || error)
            throw std::runtime_error("owned_job_assignment_failed");
        if(ResumeThread(primary_thread)==DWORD(-1)) throw std::runtime_error("owned_worker_resume_failed");
    }
#endif
    bool stop() noexcept {
        if(stopped) return complete;
        stopped=true;
        if(!group && !child) return true;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        std::error_code error;
#ifdef _WIN32
        // ActiveProcesses may reach zero before the process objects signal.
        // Retain handles to current, verified members before termination rather
        // than reopening PIDs after exit, when Windows can reuse them.
        struct MemberHandles {
            std::array<HANDLE,256> values{};
            size_t size=0;
            ~MemberHandles() { for(size_t i=0;i<size;++i) CloseHandle(values[i]); }
        } members;
        bool membership_verified=true;
        if(group && group->valid()) {
            struct ProcessIds { DWORD assigned, listed; ULONG_PTR ids[256]; } ids{};
            if(!QueryInformationJobObject(group->native_handle(),JobObjectBasicProcessIdList,&ids,sizeof(ids),nullptr) ||
                ids.listed>members.values.size()) membership_verified=false;
            else for(DWORD i=0;i<ids.listed;++i) {
                const auto handle=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(ids.ids[i]));
                if(!handle) {
                    if(GetLastError()!=ERROR_INVALID_PARAMETER) membership_verified=false;
                    continue; // Already exited members have no remaining process handle to acquire.
                }
                BOOL in_owned_job=FALSE;
                if(IsProcessInJob(handle,group->native_handle(),&in_owned_job) && in_owned_job)
                    members.values[members.size++]=handle;
                else { CloseHandle(handle); membership_verified=false; }
            }
        }
#endif
#ifndef _WIN32
        // Boost's POSIX terminate invalidates its stored pgid; retain it only for
        // bounded existence checks, never for a later second signal.
        const auto pgid=group && group->valid()?group->native_handle():-1;
#endif
        if(group && group->valid()) group->terminate(error);
        if(child && child->valid()) {
            std::error_code running_error;
            if(child->running(running_error)) child->terminate(running_error);
        }
        for(;;) {
            bool members_done=true;
#ifdef _WIN32
            if(group && group->valid()) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                // Windows Boost 1.84 terminate retains this handle. Its group
                // wait APIs can deadlock, so query the retained job directly.
                members_done=QueryInformationJobObject(group->native_handle(),JobObjectBasicAccountingInformation,
                    &accounting,sizeof(accounting),nullptr) && accounting.ActiveProcesses==0;
            }
            for(size_t i=0;i<members.size;++i)
                members_done=WaitForSingleObject(members.values[i],0)==WAIT_OBJECT_0 && members_done;
            members_done=membership_verified && members_done;
#else
            if(pgid>0) members_done=(::kill(-pgid,0)==-1 && errno==ESRCH);
#endif
            bool child_done=!child || !child->valid();
            if(!child_done) {
#ifdef _WIN32
                child_done=WaitForSingleObject(child->native_handle(),0)==WAIT_OBJECT_0;
#else
                std::error_code running_error;
                child_done=!child->running(running_error) && !running_error;
#endif
                if(child_done) {
                    // Reap only after observed exit: this is a single child wait,
                    // not an unbounded wait for an asynchronously terminating job.
                    std::error_code wait_error;child->wait(wait_error);
                    child_done=!wait_error;
                }
            }
            if(members_done && child_done) {
                if(group) group->detach(); // Avoid a later destructor termination.
                return true;
            }
            if(std::chrono::steady_clock::now()>=deadline) {complete=false;return false;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

std::string utf8(const fs::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}
fs::path from_utf8(const std::string& text)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(text));
#else
    return fs::path(text);
#endif
}
std::string bounded_read(const fs::path& file, size_t limit)
{
    if (!fs::is_regular_file(file) || fs::file_size(file) > limit) throw std::runtime_error("invalid_size");
    fs::ifstream stream(file, std::ios::binary);
    const auto initial_size=fs::file_size(file);
    if(initial_size>limit) throw std::runtime_error("invalid_size");
    std::string bytes(size_t(initial_size) + 1, '\0');
    stream.read(bytes.data(), std::streamsize(bytes.size()));
    bytes.resize(size_t(stream.gcount()));
    if (stream.bad() || bytes.size() > initial_size) throw std::runtime_error("invalid_read");
    return bytes;
}
Json strict_json(const std::string& bytes)
{
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&keys](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 20) throw std::runtime_error("json_depth");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::object_end) keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw std::runtime_error("duplicate_key");
        return true;
    });
}
bool valid(const Configuration& c)
{
    return c.python_executable.is_absolute() && c.weights_directory.is_absolute() &&
        c.cpu_threads >= 1 && c.cpu_threads <= 8 && c.timeout_seconds >= 10 && c.timeout_seconds <= 600 &&
        c.cache_bytes <= 4ULL * 1024 * 1024 * 1024;
}
unsigned long long integer(const Json& j, const char* key, unsigned long long low, unsigned long long high)
{
    const auto& value = j.at(key);
    if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<long long>() < 0))
        throw std::runtime_error("integer_required");
    const auto result = value.get<unsigned long long>();
    if (result < low || result > high) throw std::runtime_error("integer_range");
    return result;
}
bool oversized(const fs::path& path, size_t limit=max_response_bytes)
{
    boost::system::error_code error;
    const auto size = fs::file_size(path, error);
    return !error && size > limit;
}
Json encode(const Configuration& c)
{
    return {{"schema", "orcaslicer.local-semantic-runtime.v1"}, {"enabled", c.enabled},
        {"python_executable", utf8(c.python_executable)}, {"weights_directory", utf8(c.weights_directory)},
        {"cpu_threads", c.cpu_threads}, {"timeout_seconds", c.timeout_seconds}, {"cache_bytes", c.cache_bytes}};
}
bool exact_keys(const Json& value, const std::set<std::string>& keys)
{
    if (!value.is_object() || value.size()!=keys.size()) return false;
    for (auto it=value.begin();it!=value.end();++it) if (!keys.count(it.key())) return false;
    return true;
}
std::string bytes_hash(const std::string& text)
{
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length=0;
    if (EVP_Digest(text.data(),text.size(),bytes,&length,EVP_sha256(),nullptr)!=1 || length!=32)
        throw std::runtime_error("digest");
    constexpr char alphabet[]="0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned i=0;i<length;++i) { result+=alphabet[bytes[i]>>4]; result+=alphabet[bytes[i]&15]; }
    return result;
}
std::string canonical_hash(const Json& value) { return bytes_hash(value.dump()); }

const std::map<std::string,size_t> semantic_output_limits={{"result.json",max_response_bytes},
    {"rendered.bin",LocalSemanticGeometry::max_bytes},{"evidence.json",LocalSemanticEvidence::max_bytes}};
struct CachedEvidence {
    std::string request_id;
    std::map<std::string,std::string> files;
};

bool cache_plain_path(const fs::path& path)
{
    if(fs::is_symlink(fs::symlink_status(path))) return false;
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    return fs::exists(path) && fs::canonical(path)==path.lexically_normal();
}

// Both a process-local mutex and an OS file lock are needed: POSIX file locks
// alone do not exclude another thread in this process. Contention is a miss,
// never a reason to block or fail color matching. Unicode paths use native APIs.
std::mutex semantic_cache_mutex;
template<class Action> bool with_cache_lock(const fs::path& root, Action action, CacheWriteReport* report=nullptr)
{
    auto stage=[&](const char* value) {if(report) report->operation=value;};
    stage("process_lock");
    std::unique_lock<std::mutex> local(semantic_cache_mutex,std::try_to_lock);
    if(!local.owns_lock()) return false;
    try {
        stage("parent_validation");
        if(!root.is_absolute() || root.filename().empty() || !cache_plain_path(root.parent_path())) return false;
        stage("create_root");
        if(!fs::exists(root)) fs::create_directory(root);
        stage("root_validation");
        if(!fs::is_directory(root) || !cache_plain_path(root)) return false;
        const auto lock_path=root/".cache.lock";
        stage("lock_validation");
        if(fs::exists(lock_path) && (!cache_plain_path(lock_path) || !fs::is_regular_file(lock_path) || fs::hard_link_count(lock_path)!=1)) return false;
        stage("open_lock");
        { fs::ofstream file(lock_path,std::ios::binary|std::ios::app); if(!file) return false; }
        stage("acquire_lock");
        boost::interprocess::file_lock lock(lock_path.c_str());
        boost::interprocess::scoped_lock<boost::interprocess::file_lock> held(lock,boost::interprocess::try_to_lock);
        if(!held.owns()) return false;
        return action();
    } catch(const boost::system::system_error& error) {
        if(report) report->native_error=error.code().value(); return false;
    } catch(const boost::interprocess::interprocess_exception& error) {
        if(report) report->native_error=error.get_native_error(); return false;
    } catch(...) { return false; }
}

// An ownership marker and an exact, flat file set delimit the only directories
// the cache may evict. Unexpected or linked files are left untouched.
bool cache_owned_entry(const fs::path& root,const fs::path& path,unsigned long long& bytes)
{
    bytes=0;
    if(path.parent_path()!=root || !cache_plain_path(path) || !fs::is_directory(path)) return false;
    if(!cache_plain_path(path/"owner.json") || !fs::is_regular_file(path/"owner.json") ||
        fs::hard_link_count(path/"owner.json")!=1) return false;
    const auto owner=strict_json(bounded_read(path/"owner.json",1024));
    if(!exact_keys(owner,{"schema","key"}) || owner.at("schema")!="orcaslicer.semantic-cache-owner.v1" ||
        !owner.at("key").is_string() || !AI::is_lowercase_sha256(owner.at("key").get<std::string>())) return false;
    const auto key=owner.at("key").get<std::string>(),name=path.filename().string();
    if(name!=key && name.find("pending-"+key+"-")!=0) return false;
    for(const auto& entry:fs::directory_iterator(path)) {
        const auto file=entry.path();const auto name=file.filename().string();
        if((name!="owner.json" && name!="manifest.json" && !semantic_output_limits.count(name)) ||
            !cache_plain_path(file) || !fs::is_regular_file(file) || fs::hard_link_count(file)!=1) return false;
        bytes+=fs::file_size(file);
    }
    return true;
}

void cache_remove(const fs::path& root,const fs::path& entry)
{
    unsigned long long ignored=0;
    if(!cache_plain_path(root) || !cache_owned_entry(root,entry,ignored)) throw std::runtime_error("cache_ownership");
    fs::remove_all(entry);
}

bool cache_trim(const fs::path& root,unsigned long long budget,unsigned long long reserve)
{
    if(reserve>budget) return false;
    unsigned long long total=0;
    std::vector<std::pair<std::time_t,fs::path>> entries;
    for(const auto& entry:fs::directory_iterator(root)) {
        if(entry.path().filename()==".cache.lock") continue;
        unsigned long long bytes=0;
        if(!cache_owned_entry(root,entry.path(),bytes)) return false;
        if(entry.path().filename().string().find("pending-")==0) {
            cache_remove(root,entry.path()); // Exclusive lock: no live writer owns it.
            continue;
        }
        total+=bytes;
        entries.emplace_back(fs::last_write_time(entry.path()),entry.path());
    }
    std::sort(entries.begin(),entries.end());
    for(const auto& entry:entries) {
        if(total<=budget-reserve) break;
        unsigned long long bytes=0;
        if(!cache_owned_entry(root,entry.second,bytes)) return false;
        cache_remove(root,entry.second);total-=bytes;
    }
    return total<=budget-reserve;
}

std::optional<CachedEvidence> read_cache(const fs::path& root,const std::string& key,unsigned long long budget)
{
    std::optional<CachedEvidence> result;
    if(root.empty() || budget==0) return result;
    with_cache_lock(root,[&] {
        if(!cache_trim(root,budget,0)) return false;
        const auto entry=root/key;
        unsigned long long bytes=0;
        if(!fs::exists(entry) || !cache_owned_entry(root,entry,bytes) || bytes>budget) return false;
        const auto manifest=strict_json(bounded_read(entry/"manifest.json",max_response_bytes));
        if(!exact_keys(manifest,{"schema","key","request_id","files"}) ||
            manifest.at("schema")!="orcaslicer.semantic-cache.v1" || manifest.at("key")!=key ||
            !manifest.at("request_id").is_string()) return false;
        CachedEvidence cached;cached.request_id=manifest.at("request_id").get<std::string>();
        if(cached.request_id.empty() || cached.request_id.size()>96 ||
            cached.request_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos ||
            !exact_keys(manifest.at("files"),{"result.json","rendered.bin","evidence.json"})) return false;
        for(const auto& limit:semantic_output_limits) {
            const auto& metadata=manifest.at("files").at(limit.first);
            if(!exact_keys(metadata,{"bytes","sha256"})) return false;
            const auto size=integer(metadata,"bytes",1,limit.second);
            auto raw=bounded_read(entry/limit.first,limit.second);
            if(raw.size()!=size || bytes_hash(raw)!=metadata.at("sha256")) return false;
            cached.files.emplace(limit.first,std::move(raw));
        }
        result=std::move(cached);return true;
    });
    return result;
}

CacheWriteReport write_cache(const fs::path& root,const std::string& key,unsigned long long budget,
                 const CachedEvidence& cached,const std::atomic<bool>& cancelled)
{
    CacheWriteReport report;
    if(root.empty() || budget==0) {report.operation="disabled";return report;}
    if(cancelled.load()) {report.operation="cancelled";return report;}
    report.stored=with_cache_lock(root,[&] {
        report.operation="payload_validation";
        Json files=Json::object();unsigned long long size=0;
        for(const auto& limit:semantic_output_limits) {
            const auto& raw=cached.files.at(limit.first);
            if(raw.empty() || raw.size()>limit.second) return false;
            files[limit.first]={{"bytes",raw.size()},{"sha256",bytes_hash(raw)}};size+=raw.size();
        }
        const auto owner=Json({{"schema","orcaslicer.semantic-cache-owner.v1"},{"key",key}}).dump();
        const auto manifest=Json({{"schema","orcaslicer.semantic-cache.v1"},{"key",key},
            {"request_id",cached.request_id},{"files",files}}).dump();
        size+=owner.size()+manifest.size();
        report.operation="budget";
        if(size>budget) return false;
        const auto final=root/key;
        report.operation="replace_previous";
        if(fs::exists(final)) cache_remove(root,final);
        report.operation="trim";
        if(!cache_trim(root,budget,size) || cancelled.load()) return false;
        const auto pending=root/fs::unique_path("pending-"+key+"-%%%%-%%%%");
        report.operation="create_pending";
        if(!fs::create_directory(pending)) return false;
        auto write=[&](const char* name,const std::string& raw) {
            report.operation=std::string("write_")+name;
            fs::ofstream file(pending/name,std::ios::binary);file.write(raw.data(),std::streamsize(raw.size()));
            file.flush();if(!file) throw std::runtime_error("cache_write");
        };
        try {
            write("owner.json",owner);
            for(const auto& item:cached.files) write(item.first.c_str(),item.second);
            write("manifest.json",manifest);
            if(cancelled.load()) {report.operation="cancelled";cache_remove(root,pending);return false;}
            report.operation="publish";
            const auto publication=LocalSemanticCachePublication::publish(pending,final,cancelled,[&] {
                unsigned long long ignored=0;
                return cache_plain_path(root) && cache_owned_entry(root,pending,ignored);
            });
            report.publish_attempts=publication.attempts;report.native_error=publication.native_error;
            if(publication.status==LocalSemanticCachePublication::Status::Stored) {report.operation="stored";return true;}
            if(publication.status==LocalSemanticCachePublication::Status::Cancelled) report.operation="cancelled";
            if(publication.status==LocalSemanticCachePublication::Status::InvalidOwnership) report.operation="publish_ownership";
            try {cache_remove(root,pending);} catch(...) {}
            return false;
        } catch(const boost::system::system_error& error) {
            report.native_error=error.code().value();
            try {cache_remove(root,pending);} catch(...) {}
            return false;
        } catch(...) {
            try {cache_remove(root,pending);} catch(...) {}
            return false;
        }
    },&report);
    return report;
}

void touch_cache(const fs::path& root,const std::string& key)
{
    with_cache_lock(root,[&] {
        unsigned long long bytes=0;
        if(!cache_owned_entry(root,root/key,bytes)) return false;
        fs::last_write_time(root/key,std::time(nullptr));return true;
    });
}

bool complete_probe(const Json& response)
{
    if (!exact_keys(response,{"schema","worker_version","status","capability_ready","identity","runtime_fingerprint",
        "label_schema","label_names","supported_labels","unsupported","network_policy","mesh_requests_ready","probe_seconds"})) return false;
    const auto& id=response.at("identity");
    if (!exact_keys(id,{"worker_version","worker_sha256","python","python_executable_sha256","bits","packages","weights","device"}) ||
        id.at("worker_version")!="local-semantic-cpu-v1" || !id.at("bits").is_number_integer() ||
        !id.at("python").is_string() || id.at("python").get<std::string>().size()>64) return false;
    const auto& packages=id.at("packages");
    const bool eyes=packages.contains("mediapipe");
    if (eyes ? (!exact_keys(packages,{"torch","torchvision","pyfacer","numpy","Pillow","mediapipe"}) || packages.at("mediapipe")!="1.0.1") :
        !exact_keys(packages,{"torch","torchvision","pyfacer","numpy","Pillow"})) return false;
    for (const auto& version:packages) if (!version.is_string() || version.get<std::string>().empty() || version.get<std::string>().size()>128) return false;
    Json weights={{"mobilenet0.25_Final.pth","2979b33ffafda5d74b6948cd7a5b9a7a62f62b949cef24e95fd15d2883a65220"},
        {"face_parsing.farl.celebm.main_ema_181500_jit.pt","bbc1f0e9f68c80eb83a0b23f33850d1e10f2ec1eda96884112d111c2c1f15c79"}};
    if(eyes && id.at("weights").contains("face_landmarker.task"))
        weights["face_landmarker.task"]="64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff";
    if(eyes && id.at("weights").contains("selfie_multiclass_256x256.tflite"))
        weights["selfie_multiclass_256x256.tflite"]="c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0";
    if(eyes && weights.size()==2) return false;
    const Json labels={"background","neck","face","cloth","rr","lr","rb","lb","re","le","nose","imouth","llip","ulip","hair","eyeg","hat","earr","neck_l"};
    const Json supported={"neck","face","rr","lr","rb","lb","re","le","nose","imouth","llip","ulip","hair","cloth"};
    return id.at("weights")==weights && response.at("runtime_fingerprint")==canonical_hash(id) &&
        response.at("label_schema")=="farl-celebm-19-v1" && response.at("label_names")==labels &&
        response.at("supported_labels")==supported && response.at("unsupported")==Json({"teeth","pupil","eye_white","body_instance_segmentation"}) &&
        response.at("mesh_requests_ready").is_boolean() && response.at("mesh_requests_ready")==false &&
        response.at("network_policy")=="python-audit-hook-and-offline-flags; not-native-OS-sandbox" &&
        response.at("probe_seconds").is_number() && std::isfinite(response.at("probe_seconds").get<double>()) &&
        response.at("probe_seconds").get<double>()>=0 && response.at("probe_seconds").get<double>()<=600;
}
} // namespace

bool cleanup_request(const fs::path& owned, const fs::path& parent, std::string& reason)
{
    reason="local_semantic_cleanup_failed";
    try {
        if(!owned.is_absolute() || !parent.is_absolute() || owned.filename()=="." || owned.filename()=="..") return false;
        boost::system::error_code error;
        const auto canonical_parent=fs::canonical(parent,error);
        if(error) return false;
        const auto actual_parent=fs::canonical(owned.parent_path(),error);
        if(error || actual_parent!=canonical_parent) return false;
        const auto status=fs::symlink_status(owned,error);
        // Boost can return file_not_found together with ENOENT. The status is
        // authoritative: absence after removal is success, not a cleanup error.
        if(status.type()==fs::file_not_found) { reason.clear();return true; }
        if(error || !fs::is_directory(status) || fs::is_symlink(status)) return false;
        const auto actual=fs::canonical(owned,error);
        if(error || actual.parent_path()!=canonical_parent) return false;
        for(int attempt=0;attempt<20;++attempt) {
            fs::remove_all(actual,error);
            if(!error) { reason.clear();return true; }
            boost::system::error_code status_error;
            if(fs::symlink_status(actual,status_error).type()==fs::file_not_found) { reason.clear();return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    } catch(...) {}
    return false;
}

bool read_configuration(const fs::path& file, Configuration& destination, std::string& reason)
{
    try {
        const auto j = strict_json(bounded_read(file, max_config_bytes));
        if (!j.is_object() || j.size() != 7 || j.at("schema") != "orcaslicer.local-semantic-runtime.v1" ||
            !j.at("enabled").is_boolean()) throw std::runtime_error("schema");
        Configuration c;
        c.enabled = j.at("enabled").get<bool>();
        for (const char* key : {"python_executable", "weights_directory"}) {
            const auto text = j.at(key).get<std::string>();
            if (text.empty() || text.size() > 4096 || text.find('\0') != std::string::npos) throw std::runtime_error("path");
            (std::string(key) == "python_executable" ? c.python_executable : c.weights_directory) = from_utf8(text);
        }
        c.cpu_threads = unsigned(integer(j, "cpu_threads", 1, 8));
        c.timeout_seconds = unsigned(integer(j, "timeout_seconds", 10, 600));
        c.cache_bytes = integer(j, "cache_bytes", 0, 4ULL * 1024 * 1024 * 1024);
        if (!valid(c)) throw std::runtime_error("path");
        destination = std::move(c); reason.clear(); return true;
    } catch (...) { reason = "invalid_semantic_configuration"; return false; }
}

bool read_runtime_configuration(const fs::path& file, const fs::path& installed_runtime,
                                Configuration& destination, std::string& reason)
{
    try {
        if (fs::exists(file)) return read_configuration(file, destination, reason);
        Configuration c;
        c.python_executable = installed_runtime / "python" / "python.exe";
        c.weights_directory = installed_runtime / "weights";
        c.enabled = true;
        c.timeout_seconds = 300;
        if (!valid(c) || !fs::is_regular_file(c.python_executable)) throw std::runtime_error("missing");
        for (const char* name : {"mobilenet0.25_Final.pth", "face_parsing.farl.celebm.main_ema_181500_jit.pt", "face_landmarker.task"})
            if (!fs::is_regular_file(c.weights_directory / name)) throw std::runtime_error("missing");
        destination = std::move(c); reason.clear(); return true;
    } catch (...) { reason = "bundled_semantic_runtime_unavailable"; return false; }
}

static Result inspect_runtime(const Configuration& c, const fs::path& script, const fs::path& request_root,
                              const std::atomic<bool>& cancelled, bool identity_only)
{
    Result result;
    const auto finish = [&] {
        if (cancelled.load()) { result.status=Status::Cancelled; result.reason="cancelled"; result.response_json.clear(); }
        return result;
    };
    if (cancelled.load()) { result.status = Status::Cancelled; result.reason = "cancelled"; return finish(); }
    if (!c.enabled) { result.status = Status::Disabled; result.reason = "not_configured"; return finish(); }
    OwnedProcess owned;
    auto& child = owned.child;
    std::string failure_reason="local_runtime_preparation_failed";
    const auto stop = [&] { return owned.stop(); };
    try {
        if (!valid(c) || !script.is_absolute() || !request_root.is_absolute() ||
            !fs::is_regular_file(c.python_executable) || !fs::is_regular_file(script)) {
            result.reason = "local_runtime_missing"; return finish();
        }
        const auto worker_hash = AI::model_artifact_sha256(script);
        const auto interpreter_hash = AI::model_artifact_sha256(c.python_executable);
        if (worker_hash.size()!=64 || interpreter_hash.size()!=64) throw std::runtime_error("runtime_hash_failed");
        fs::create_directories(request_root);
        result.request_directory = request_root / fs::unique_path("probe-%%%%-%%%%-%%%%");
        if (!fs::create_directory(result.request_directory)) throw std::runtime_error("unique_directory");
        const auto config_path = result.request_directory / "config.json";
        const auto response_path = result.request_directory / "result.json";
        { fs::ofstream file(config_path, std::ios::binary); file << encode(c).dump();
          file.flush(); if (!file) throw std::runtime_error("write_config"); }
        process::environment env;
        env.clear();
        for (const char* name : {"SYSTEMROOT", "WINDIR", "TEMP", "TMP", "TMPDIR"})
            if (const char* value = boost::nowide::getenv(name)) env[name] = value;
        env["PYTHONNOUSERSITE"] = "1"; env["HF_HUB_OFFLINE"] = "1";
        env["TRANSFORMERS_OFFLINE"] = "1"; env["HF_HUB_DISABLE_TELEMETRY"] = "1";
        env["OMP_NUM_THREADS"] = std::to_string(c.cpu_threads); env["MKL_NUM_THREADS"] = std::to_string(c.cpu_threads);
        failure_reason="local_worker_launch_failed";
        owned.prepare();
#ifdef _WIN32
        process::wenvironment wide_env(env);
        std::vector<std::wstring> arguments{L"-I", script.wstring(), L"--probe", L"--config",
            config_path.wstring(), L"--output", response_path.wstring()};
        if(identity_only) arguments.emplace_back(L"--identity-only");
        child = std::make_unique<process::child>(c.python_executable.wstring(),
            process::args(arguments), wide_env,
            process::start_dir(result.request_directory.wstring()), process::std_out > process::null,
            process::std_err > process::null, process::windows::create_no_window, *owned.group,
            process::extend::on_setup = [](auto& exec) { exec.creation_flags |= CREATE_SUSPENDED; },
            process::extend::on_success = [&](auto& exec) { owned.primary_thread = exec.proc_info.hThread; });
        owned.resume();
#else
        std::vector<std::string> arguments{"-I", script.string(), "--probe", "--config",
            config_path.string(), "--output", response_path.string()};
        if(identity_only) arguments.emplace_back("--identity-only");
        child = std::make_unique<process::child>(c.python_executable.string(),
            process::args(arguments), env,
            process::start_dir(result.request_directory.string()), process::std_out > process::null,
            process::std_err > process::null, *owned.group);
#endif
        failure_reason="local_worker_wait_failed";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(c.timeout_seconds);
        while (child->running()) {
            if (cancelled.load()) { stop(); result.status = Status::Cancelled; result.reason = "cancelled"; return finish(); }
            if (std::chrono::steady_clock::now() >= deadline) { stop(); result.status = Status::TimedOut;
                result.reason = "semantic_timeout"; return finish(); }
            if (oversized(response_path) || oversized(result.request_directory / "result.json.partial")) {
                stop(); result.reason = "semantic_output_too_large"; return finish();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        child->wait();
        result.exit_code=child->exit_code();
        if (!stop()) throw std::runtime_error("owned_worker_shutdown_incomplete");
        if (cancelled.load()) { result.status = Status::Cancelled; result.reason = "cancelled"; return finish(); }
        failure_reason="local_worker_response_missing_or_large";
        result.response_json = bounded_read(response_path, max_response_bytes);
        failure_reason="invalid_semantic_response";
        const auto response = strict_json(result.response_json);
        if (response.at("schema") != schema || response.at("worker_version") != "local-semantic-cpu-v1" ||
            !response.at("capability_ready").is_boolean()) throw std::runtime_error("bad_response");
        if (response.at("status") == "unavailable" && response.at("capability_ready") == false && child->exit_code() == 2) {
            result.reason = "semantic_probe_unavailable"; return finish();
        }
        if (response.at("status") != (identity_only ? "identity" : "ok") ||
            response.at("capability_ready") != !identity_only || child->exit_code() != 0)
            throw std::runtime_error("bad_response");
        if (!complete_probe(response)) throw std::runtime_error("incomplete_probe");
        const auto& identity = response.at("identity");
        if (identity.at("worker_sha256") != worker_hash || identity.at("python_executable_sha256") != interpreter_hash ||
            identity.at("device") != "cpu" || identity.at("bits") != 64 ||
            AI::model_artifact_sha256(script) != worker_hash || AI::model_artifact_sha256(c.python_executable) != interpreter_hash)
            throw std::runtime_error("stale_runtime");
        result.status = Status::Ready; result.reason.clear();
    } catch (...) { stop(); result.status = Status::Unavailable; result.reason = failure_reason; result.response_json.clear(); }
    return finish();
}

Result probe(const Configuration& c, const fs::path& script, const fs::path& request_root,
             const std::atomic<bool>& cancelled)
{
    return inspect_runtime(c,script,request_root,cancelled,false);
}

MeshResult analyze(const Configuration& c, const fs::path& directory, const fs::path& request_root,
                   const fs::path& source, const indexed_triangle_set& native, const std::atomic<bool>& cancelled,
                   const fs::path& cache_root)
{
    MeshResult result;
    auto& state=result.process;
    const auto finish=[&]() -> MeshResult {
        if(cancelled.load()) {
            state.status=Status::Cancelled; state.reason="cancelled"; state.response_json.clear(); result.evidence={}; result.cache_hit=false;
        }
        return std::move(result);
    };
    if(cancelled.load()) return finish();
    if(!c.enabled) {state.status=Status::Disabled;state.reason="not_configured";return finish();}
    if(!directory.is_absolute() || !request_root.is_absolute() || !source.is_absolute()) {
        state.reason="invalid_semantic_input_paths";return finish();
    }
    // Metadata inspection does not execute models. The mesh request performs
    // inference once and still needs complete runtime and native surface proof.
    state=inspect_runtime(c,directory/"local_semantic_worker.py",request_root,cancelled,true);
    if(state.status!=Status::Ready) return finish();
    state.status=Status::Unavailable;
    OwnedProcess owned;
    auto& child = owned.child;
    const auto stop=[&] { return owned.stop(); };
    std::string failure="local_semantic_request_preparation_failed";
    try {
        const auto probe_identity=strict_json(state.response_json).at("identity");
        state.response_json.clear();
        if(cancelled.load()) return finish();
        std::vector<std::string> module_names={"glb_artifact.py","local_semantic_worker.py","local_semantic_geometry.py",
            "local_semantic_render.py","local_semantic_transform.py","local_semantic_views.py","local_semantic_projection.py",
            "local_semantic_pipeline.py","local_semantic_request.py","local_eye_landmarks.py","local_face_landmarks.py","local_body_regions.py"};
        // The exact native raster binary joins both identity maps only when
        // installed; an absent accelerator uses the pixel-identical Python path.
        if(fs::exists(directory/"local_semantic_raster.dll"))module_names.push_back("local_semantic_raster.dll");
        auto module_hashes=[&] {
            Json hashes=Json::object();
            for(const auto& name:module_names) {
                const auto path=directory/name;
                if(!fs::is_regular_file(path) || fs::is_symlink(path) || fs::file_size(path)>2*1024*1024) throw std::runtime_error("missing_module");
                hashes[name]=AI::model_artifact_sha256(path);
                if(!AI::is_lowercase_sha256(hashes[name].get<std::string>())) throw std::runtime_error("module_hash");
            }
            return hashes;
        };
        const auto modules=module_hashes();
        if(modules.at("local_semantic_worker.py")!=probe_identity.at("worker_sha256")) throw std::runtime_error("stale_probe");
        const Json identity={{"probe_identity",probe_identity},{"modules_sha256",modules}};
        const auto runtime_hash=canonical_hash(identity);
        Json policy_modules=Json::object();
        for(const char* name:{"local_semantic_render.py","local_semantic_transform.py","local_semantic_views.py",
                             "local_semantic_projection.py","local_semantic_pipeline.py","local_eye_landmarks.py","local_face_landmarks.py","local_body_regions.py"}) policy_modules[name]=modules.at(name);
        if(modules.contains("local_semantic_raster.dll"))policy_modules["local_semantic_raster.dll"]=modules.at("local_semantic_raster.dll");
        const Json policy={{"version","visible-face-semantic-v5-body-supplement"},{"label_schema","farl-celebm-face19-subset-v1"},
                           {"modules_sha256",policy_modules}};
        const auto policy_hash=canonical_hash(policy);
        if(!fs::is_regular_file(source) || fs::file_size(source)>512ULL*1024*1024) throw std::runtime_error("source_size");
        const auto source_hash=AI::model_artifact_sha256(source);
        const auto geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(native);
        std::string native_bytes,error;
        if(!LocalSemanticGeometry::encode(native,source_hash,native_bytes,error,&cancelled)) throw std::runtime_error("native_packet");
        state.request_directory=request_root/fs::unique_path("mesh-%%%%-%%%%-%%%%");
        if(!fs::create_directory(state.request_directory)) throw std::runtime_error("request_directory");
        const auto request_id=state.request_directory.filename().string();
        const Json request={{"schema","orcaslicer.local-semantic-request.v1"},{"request_id",request_id},
            {"source_path",utf8(source)},{"source_sha256",source_hash},{"geometry_id",geometry_id},{"face_count",native.indices.size()},
            {"runtime_fingerprint",runtime_hash},{"policy_sha256",policy_hash}};
        auto write_file=[&](const fs::path& path,const std::string& bytes) {
            fs::ofstream file(path,std::ios::binary);file.write(bytes.data(),std::streamsize(bytes.size()));
            file.flush();if(!file) throw std::runtime_error("request_write");
        };
        const auto config_path=state.request_directory/"config.json",request_path=state.request_directory/"request.json";
        const auto response_path=state.request_directory/"result.json",script=directory/"local_semantic_request.py";
        write_file(config_path,encode(c).dump());write_file(request_path,request.dump());
        write_file(state.request_directory/"native.bin",native_bytes);
        const auto native_hash=bytes_hash(native_bytes);
        native_bytes.clear();native_bytes.shrink_to_fit();
        if(cancelled.load()) return finish();
        std::map<std::string,std::string> validated_files;
        auto validate=[&](const std::string& raw_response,const std::string& validation_id,int response_exit,auto read_file) -> bool {
            failure="invalid_semantic_request_response";
            state.response_json=raw_response;state.exit_code=response_exit;validated_files.clear();
            const auto response=strict_json(state.response_json);
            if(response.at("schema")!="orcaslicer.local-semantic-response.v1" ||
               response.at("worker_version")!="local-semantic-request-cpu-v1" || response.at("request_id")!=validation_id)
                throw std::runtime_error("response_identity");
            if(state.exit_code==2 && response.at("status")=="unavailable" &&
                exact_keys(response,{"schema","worker_version","request_id","status","error_code"}) && response.at("error_code").is_string()) {
                state.reason="local_semantic_analysis_unavailable";return false;
            }
            if(state.exit_code!=0 || response.at("status")!="ok" || !exact_keys(response,
                {"schema","worker_version","request_id","status","identity","runtime_fingerprint","policy_sha256","files","statistics"}) ||
                response.at("identity")!=identity || response.at("runtime_fingerprint")!=runtime_hash || response.at("policy_sha256")!=policy_hash)
                throw std::runtime_error("response_contract");
            if(module_hashes()!=modules || AI::model_artifact_sha256(c.python_executable)!=probe_identity.at("python_executable_sha256") ||
                AI::model_artifact_sha256(source)!=source_hash || AI::model_artifact_sha256(state.request_directory/"native.bin")!=native_hash)
                throw std::runtime_error("stale_input_or_runtime");
            const auto& files=response.at("files");
            if(!exact_keys(files,{"rendered.bin","evidence.json"})) throw std::runtime_error("output_files");
            auto read_output=[&](const char* name,size_t limit) {
                const auto& metadata=files.at(name);
                if(!exact_keys(metadata,{"bytes","sha256"}) || !metadata.at("sha256").is_string() ||
                    !AI::is_lowercase_sha256(metadata.at("sha256").get<std::string>())) throw std::runtime_error("output_manifest");
                const auto size=integer(metadata,"bytes",1,limit);
                auto bytes=read_file(name,limit);
                if(bytes.size()!=size || bytes_hash(bytes)!=metadata.at("sha256")) throw std::runtime_error("output_hash");
                validated_files[name]=bytes;
                return bytes;
            };
            failure="semantic_native_face_proof_failed";
            LocalSemanticGeometry::Packet rendered;
            if(!LocalSemanticGeometry::decode(read_output("rendered.bin",LocalSemanticGeometry::max_bytes),source_hash,rendered,error,&cancelled))
                throw std::runtime_error("render_packet");
            LocalSemanticEvidence::VerifiedFaceBinding binding;
            if(!LocalSemanticEvidence::prove_ordered_faces(source_hash,native,rendered.mesh,binding,error)) throw std::runtime_error("ordered_surface");
            if(cancelled.load()) return false;
            LocalSemanticEvidence::ExpectedIdentity expected;
            expected.request_id=validation_id;expected.source_sha256=source_hash;expected.geometry_id=geometry_id;expected.face_count=native.indices.size();
            expected.weights_sha256=canonical_hash(probe_identity.at("weights"));expected.runtime_sha256=runtime_hash;expected.policy_sha256=policy_hash;
            failure="invalid_semantic_evidence";
            if(!LocalSemanticEvidence::decode(read_output("evidence.json",LocalSemanticEvidence::max_bytes),expected,binding,result.evidence,error))
                throw std::runtime_error("evidence_decode");
            const auto& statistics=response.at("statistics");
            std::map<std::string,unsigned long long> statistic_limits;
            for(const char* key:{"face_count","visible_faces","unseen_faces","ambiguous_faces","cross_subject_faces",
                "below_threshold_faces","known_faces","unknown_faces","render_visible_faces","render_unseen_faces"})
                statistic_limits[key]=native.indices.size();
            statistic_limits.insert({{"observations",128},{"views",16},{"view_families",16},{"raw_pixels",16*1024*1024},
                {"associated_components",32},{"ambiguous_components",32}});
            if(!statistics.is_object() || statistics.size()!=statistic_limits.size()) throw std::runtime_error("statistics");
            std::map<std::string,unsigned long long> counts;
            for(const auto& entry:statistic_limits) counts[entry.first]=integer(statistics,entry.first.c_str(),0,entry.second);
            if(counts.at("known_faces")!=result.evidence.known_faces || counts.at("unknown_faces")!=result.evidence.unknown_faces ||
                counts.at("face_count")!=native.indices.size() || counts.at("visible_faces")+counts.at("unseen_faces")!=native.indices.size() ||
                counts.at("render_visible_faces")+counts.at("render_unseen_faces")!=native.indices.size() ||
                counts.at("known_faces")>counts.at("visible_faces") || counts.at("visible_faces")>counts.at("render_visible_faces") ||
                counts.at("view_families")>counts.at("views") || counts.at("observations")>8*counts.at("views") ||
                counts.at("ambiguous_components")>counts.at("associated_components"))
                throw std::runtime_error("statistics_mismatch");
            if(AI::model_artifact_sha256(source)!=source_hash) throw std::runtime_error("source_changed_during_validation");
            state.status=Status::Ready;state.reason.clear();
            return true;
        };
        const auto cache_key=canonical_hash(Json({{"schema","orcaslicer.semantic-cache-key.v1"},
            {"source_sha256",source_hash},{"native_sha256",native_hash},{"geometry_id",geometry_id},
            {"runtime_sha256",runtime_hash},{"policy_sha256",policy_hash},{"cpu_threads",c.cpu_threads}}));
        if(auto cached=read_cache(cache_root,cache_key,c.cache_bytes)) {
            try {
                if(validate(cached->files.at("result.json"),cached->request_id,0,
                    [&](const char* name,size_t) {return cached->files.at(name);}) && !cancelled.load()) {
                    result.cache_hit=true;touch_cache(cache_root,cache_key);return finish();
                }
            } catch(...) {} // A bad cache is only a miss. Fresh outputs still need every proof.
            state.status=Status::Unavailable;state.reason.clear();state.response_json.clear();state.exit_code=-1;
            result.evidence={};validated_files.clear();
        }
        if(cancelled.load()) return finish();
        process::environment env;env.clear();
        for(const char* name:{"SYSTEMROOT","WINDIR","TEMP","TMP","TMPDIR"})
            if(const char* value=boost::nowide::getenv(name)) env[name]=value;
        env["PYTHONNOUSERSITE"]="1";env["HF_HUB_OFFLINE"]="1";env["TRANSFORMERS_OFFLINE"]="1";env["HF_HUB_DISABLE_TELEMETRY"]="1";
        env["OMP_NUM_THREADS"]=std::to_string(c.cpu_threads);env["MKL_NUM_THREADS"]=std::to_string(c.cpu_threads);
        failure="local_semantic_request_launch_failed";
        owned.prepare();
#ifdef _WIN32
        process::wenvironment wide_env(env);
        child=std::make_unique<process::child>(c.python_executable.wstring(),
            process::args(std::vector<std::wstring>{L"-I",script.wstring(),L"--request",request_path.wstring(),
                L"--config",config_path.wstring(),L"--output",response_path.wstring()}),wide_env,
            process::start_dir(state.request_directory.wstring()),process::std_out>process::null,
            process::std_err>process::null,process::windows::create_no_window,*owned.group,
            process::extend::on_setup = [](auto& exec) { exec.creation_flags |= CREATE_SUSPENDED; },
            process::extend::on_success = [&](auto& exec) { owned.primary_thread = exec.proc_info.hThread; });
        owned.resume();
#else
        child=std::make_unique<process::child>(c.python_executable.string(),
            process::args(std::vector<std::string>{"-I",script.string(),"--request",request_path.string(),
                "--config",config_path.string(),"--output",response_path.string()}),env,
            process::start_dir(state.request_directory.string()),process::std_out>process::null,process::std_err>process::null,*owned.group);
#endif
        const auto outputs_oversized=[&] {
            for(const auto& entry:semantic_output_limits) if(oversized(state.request_directory/entry.first,entry.second) ||
                oversized(state.request_directory/(entry.first+".partial"),entry.second)) return true;
            return false;
        };
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(c.timeout_seconds);
        failure="local_semantic_request_wait_failed";
        while(child->running()) {
            if(cancelled.load()) {stop();return finish();}
            if(std::chrono::steady_clock::now()>=deadline) {stop();state.status=Status::TimedOut;state.reason="semantic_timeout";return finish();}
            if(outputs_oversized()) {
                stop();state.reason="semantic_output_too_large";return finish();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        child->wait();state.exit_code=child->exit_code();
        if(!stop()) throw std::runtime_error("owned_worker_shutdown_incomplete");
        if(cancelled.load()) return finish();
        if(outputs_oversized()) {state.reason="semantic_output_too_large";return finish();}
        failure="invalid_semantic_request_response";
        if(!validate(bounded_read(response_path,max_response_bytes),request_id,state.exit_code,
            [&](const char* name,size_t limit) {return bounded_read(state.request_directory/name,limit);})) return finish();
        if(cancelled.load()) return finish();
        validated_files["result.json"]=state.response_json;
        result.cache_write=write_cache(cache_root,cache_key,c.cache_bytes,CachedEvidence{request_id,std::move(validated_files)},cancelled);
    } catch(...) {
        stop();state.status=Status::Unavailable;state.reason=failure;state.response_json.clear();result.evidence={};result.cache_hit=false;
    }
    return finish();
}
} // namespace Slic3r::GUI::LocalSemanticWorker
