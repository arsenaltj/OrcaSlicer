#include "slic3r/Utils/PlatformLibrary/LibraryClient.hpp"
#include "slic3r/Utils/PlatformLibrary/LibraryWorker.hpp"
#include "slic3r/GUI/Redesign/LibraryPresentationChannel.hpp"
#include "slic3r/Utils/HonorAccount/HonorOAuth.hpp"
#include "slic3r/Utils/HonorAccount/HonorSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryRepository.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryExport.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <condition_variable>
#include <chrono>
#include <future>
#include <wx/init.h>

using namespace Slic3r::PlatformLibrary;

// Explicit local QA only. Read the existing access token without refreshing,
// saving or erasing the user's session; never log credentials or account data.
TEST_CASE("A free platform model downloads and reuses a verified consumable local asset", "[.PlatformLibraryLiveQA]") {
    const auto profile_value = boost::nowide::getenv("ORCA_LIBRARY_QA_PROFILE");
    if (!profile_value) SKIP("Set the existing credential profile.");
    // Own the first value before nowide reuses its environment string buffer.
    const std::string profile(profile_value);
    const auto output = boost::nowide::getenv("ORCA_LIBRARY_QA_OUTPUT");
    if (!output) SKIP("Set an independent QA output directory.");
    wxInitializer init; REQUIRE(init.IsOk());
    const boost::filesystem::path evidence(output);
    boost::filesystem::create_directories(evidence);
    Json report = {{"scope", "free_download_native_transaction"}, {"session_read_only", true},
                   {"session_refreshed", false}, {"main_window", false}, {"status", "pending"}};
    const auto save_report = [&] {
        boost::filesystem::ofstream stream(evidence / "live-download.json"); stream << report.dump(2);
    };
    auto store = Slic3r::HonorAccount::system_store(profile);
    Slic3r::HonorAccount::Config config;
    config.api_url = "https://api.cutelion.top";
    if (const auto raw = store->load("configuration")) config = Slic3r::HonorAccount::parse_config(*raw);
    REQUIRE(config.api_url == "https://api.cutelion.top");
    const auto saved = store->load("platform-session-" + config.key());
    if (!saved || Slic3r::HonorAccount::is_refresh_tombstone(*saved)) {
        report["status"] = "not_verified"; report["reason"] = "no_existing_access_session"; save_report();
        SKIP("No existing platform access session; the user's login is left unchanged.");
    }
    const auto session = Slic3r::HonorAccount::parse_session(*saved);
    if (session.access_expires_at <= Slic3r::HonorAccount::now_seconds()) {
        report["status"] = "not_verified"; report["reason"] = "existing_access_expired"; save_report();
        SKIP("Existing access expired; this QA never rotates or refreshes credentials.");
    }
    Client client({config.api_url, session.access_token, session.identity.subject, 1});
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    try {
        // Fixed small public GLB; refuse a changed version or any paid access.
        const auto selected = client.detail("2c8b7618-119e-4c72-a7a7-5f24cfd7b3c6", 1, cancel);
        REQUIRE(selected.at("version").value("access", "") == "free");
        REQUIRE(selected.at("access").value("allowed", false));
        const auto spec = DownloadSpec::from_detail(selected, "8cc05093-3714-4944-823c-3ed22f3a2fa5");
        REQUIRE(spec.size <= 1024 * 1024);
        const auto models = evidence / "下载模型" / "models";
        const auto started = std::chrono::steady_clock::now();
        auto download = StagedDownload::prepare(client, spec, models, cancel);
        REQUIRE(download->commit(cancel) == spec.key());
        const auto path = models / "website" / spec.key() / "model.glb";
        REQUIRE(file_digest(path) == spec.sha256);
        REQUIRE(boost::filesystem::file_size(path) == spec.size);
        std::size_t extra_network_calls = 0;
        Client reuse(client.identity(), [&](const Identity&, const std::string&, const std::string&, const std::string&,
            Cancellation, std::size_t, std::function<bool(const char*, std::size_t)>,
            std::function<void(std::size_t, std::size_t)>) -> Binary {
                ++extra_network_calls; throw Error(0, "Duplicate download must reuse the local bytes");
        });
        REQUIRE(StagedDownload::prepare(reuse, spec, models, cancel)->commit(cancel) == spec.key());
        REQUIRE(extra_network_calls == 0);
        Slic3r::GUI::ModelPreview3D::PreparedModel prepared;
        std::string error;
        REQUIRE(Slic3r::GUI::ModelPreview3D::prepare_model(path, prepared, error));
        REQUIRE(prepared.triangles > 0);
        const auto entries = Slic3r::GUI::ModelLibraryRepository::read(models, *cancel);
        REQUIRE(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.job_id == spec.key() && entry.model_path == path;
        }));
        report.update({{"status", "passed"}, {"model", spec.model}, {"version", spec.version},
            {"file", spec.file}, {"sha256", spec.sha256}, {"size", spec.size},
            {"triangles", prepared.triangles}, {"repeat_network_calls", extra_network_calls},
            {"milliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()}});
        save_report();
    } catch (const Error& error) {
        report["status"] = "not_verified"; report["http_status"] = error.status;
        report["reason"] = error.what(); save_report();
        if (error.status == 401) SKIP("The existing access token is no longer valid; no refresh is attempted.");
        throw;
    }
}

TEST_CASE("Canceling accumulated library results allows new account work to complete", "[PlatformLibrary][UiLifecycle]") {
    Worker worker;
    std::mutex mutex;
    std::condition_variable changed;
    int produced=0;
    for(int i=0;i<40;++i) REQUIRE(worker.submit(1,"old","",[&](Cancellation) {
        {std::lock_guard<std::mutex> lock(mutex);++produced;changed.notify_all();}
        return Worker::Value{Json::object(),{}};
    }));
    bool filled;
    {std::unique_lock<std::mutex> lock(mutex);
     filled=changed.wait_for(lock,std::chrono::seconds(3),[&]{return produced>=16;});}
    worker.cancel();
    REQUIRE(worker.submit(2,"new","",[](Cancellation){return Worker::Value{Json::object(),{}};}));
    std::deque<Worker::Result> results;
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(results.empty() && std::chrono::steady_clock::now()<end) {results=worker.take(1);std::this_thread::yield();}
    worker.stop();
    REQUIRE(filled);
    REQUIRE(results.size()==1);
    REQUIRE(results.front().epoch==2);
    REQUIRE(results.front().kind=="new");
}

namespace {
constexpr const char* model="11111111-1111-4111-8111-111111111111";
constexpr const char* version="22222222-2222-4222-8222-222222222222";
constexpr const char* file="33333333-3333-4333-8333-333333333333";
struct Directory {
    boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("library-%%%%%%%%");
    Directory(){boost::filesystem::create_directories(path);}
    ~Directory(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}
};
Json detail(const std::string& bytes,const std::string& filename="model.glb") {
    return {{"id",model},{"title","Test source"},{"version",{{"id",version},{"version",2}}},
        {"access",{{"allowed",true}}},{"files",Json::array({{{"id",file},{"filename",filename},
            {"size",std::to_string(bytes.size())},{"status","ready"},{"sha256",Slic3r::HonorAccount::digest(bytes)}}})}};
}
Client fixture_client(std::string bytes, std::function<void(Cancellation)> effect={}) {
    return Client({"https://platform.example.test","native-only-token","account",4},
        [bytes=std::move(bytes),effect](const Identity&,const std::string&,const std::string&,const std::string&,
            Cancellation cancel,std::size_t,std::function<bool(const char*,std::size_t)> sink,std::function<void(std::size_t,std::size_t)>) {
            if(effect) effect(cancel);
            if(sink && !sink(bytes.data(),bytes.size())) throw Error(0,"write failed");
            return Binary{200,sink?"":bytes,"application/octet-stream"};
        });
}
Cancellation cancellation(){return std::make_shared<std::atomic_bool>(false);}
}
TEST_CASE("Download specifications bind the exact authorized version and file digest", "[PlatformLibrary]") {
    const auto good=detail("complete");const auto s=DownloadSpec::from_detail(good,file);
    REQUIRE(s.version_number==2);REQUIRE(s.size==8);REQUIRE(s.sha256==Slic3r::HonorAccount::digest("complete"));
    auto denied=good;denied["access"]["allowed"]=false;REQUIRE_THROWS(DownloadSpec::from_detail(denied,file));
    REQUIRE_THROWS(DownloadSpec::from_detail(good,model));
    auto bad=good;bad["files"][0]["sha256"]="";REQUIRE_THROWS(DownloadSpec::from_detail(bad,file));
    bad=good;bad["files"][0]["size"]="104857601";REQUIRE_THROWS(DownloadSpec::from_detail(bad,file));
    bad=good;bad["files"][0]["filename"]="model.exe";REQUIRE_THROWS(DownloadSpec::from_detail(bad,file));
    REQUIRE(DownloadSpec::from_detail(detail("complete","MODEL.GLB"),file).extension()==".glb");
    auto invalid=s;invalid.file="../../other";
    Directory root;REQUIRE_THROWS(StagedDownload::prepare(fixture_client("complete"),invalid,root.path,cancellation()));
    REQUIRE(boost::filesystem::is_empty(root.path));
}
TEST_CASE("Only issued platform preview resources can reach the image adapter", "[PlatformLibrary]") {
    REQUIRE(preview_path(std::string("/v1/library/models/")+model+"/preview?version=2"));
    REQUIRE(preview_path(std::string("/v1/library/media/")+file));
    for(const auto& path:{"https://other.test/image.png","file:///C:/secret","/v1/library/media/../../file","/v1/library/media/not-a-uuid?url=secret"})
        REQUIRE_FALSE(preview_path(path));
    REQUIRE(escaped("q /&?")=="q%20%2F%26%3F");
}
TEST_CASE("Uncommitted verified downloads remain invisible and clean their temporary bytes", "[PlatformLibrary]") {
    Directory root;const std::string bytes="complete";
    auto staged=StagedDownload::prepare(fixture_client(bytes),DownloadSpec::from_detail(detail(bytes),file),root.path,cancellation());
    REQUIRE(boost::filesystem::is_empty(root.path/"downloads"));
    REQUIRE(boost::filesystem::is_empty(root.path/"website")==false);
    staged.reset();REQUIRE(boost::filesystem::is_empty(root.path/"website"));
}
TEST_CASE("A verified download atomically registers without overwriting source or repair history", "[PlatformLibrary]") {
    Directory root;const std::string bytes="complete";boost::filesystem::create_directories(root.path/"downloads");
    {boost::filesystem::ofstream original(root.path/"downloads"/"original.json");original<<"untouched";}
    auto spec=DownloadSpec::from_detail(detail(bytes,"model.3mf"),file);
    auto staged=StagedDownload::prepare(fixture_client(bytes),spec,root.path,cancellation());
    const auto id=staged->commit(cancellation());REQUIRE(id==spec.key());
    const auto metadata=root.path/"downloads"/("orcaslicer-ai-"+id+".json");REQUIRE(boost::filesystem::is_regular_file(metadata));
    boost::filesystem::ifstream input(metadata);Json record;input>>record;
    REQUIRE(record.at("website_sha256")==Slic3r::HonorAccount::digest(bytes));
    REQUIRE_FALSE(record.contains("check"));REQUIRE_FALSE(record.contains("access_token"));
    REQUIRE(boost::filesystem::is_regular_file(root.path/record.at("model_path").get<std::string>()));
    REQUIRE(Slic3r::GUI::ModelLibraryRepository::read(root.path,*cancellation()).empty());
    auto entries=Slic3r::GUI::ModelLibraryRepository::read(root.path,*cancellation(),true);
    REQUIRE(entries.size()==1);REQUIRE(entries.front().website_model_id==model);REQUIRE(entries.front().model_path.extension()==".3mf");
    REQUIRE(entries.front().website_original_verified);
    boost::filesystem::path destination;std::string error;
    Slic3r::GUI::ModelLibraryExportRequest export_request;export_request.model=entries.front().model_path;export_request.id=id;
    REQUIRE(Slic3r::GUI::export_model_library_copy(export_request,root.path,destination,error));
    REQUIRE(file_digest(destination/"model.3mf")==Slic3r::HonorAccount::digest(bytes));
    REQUIRE_FALSE(boost::filesystem::exists(destination/"model.glb"));
    boost::filesystem::ifstream original(root.path/"downloads"/"original.json");std::string text;original>>text;REQUIRE(text=="untouched");
}
TEST_CASE("Size or digest mismatches and cancellation leave no registered asset", "[PlatformLibrary]") {
    Directory root;const auto spec=DownloadSpec::from_detail(detail("complete"),file);
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("wrong"),spec,root.path,cancellation()));
    REQUIRE(boost::filesystem::is_empty(root.path/"downloads"));REQUIRE(boost::filesystem::is_empty(root.path/"website"));
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("badbytes"),spec,root.path,cancellation()));
    REQUIRE(boost::filesystem::is_empty(root.path/"website"));
    auto cancel=cancellation();
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("complete",[](Cancellation c){*c=true;}),spec,root.path,cancel));
    REQUIRE(boost::filesystem::is_empty(root.path/"downloads"));REQUIRE(boost::filesystem::is_empty(root.path/"website"));
    cancel=cancellation();auto staged=StagedDownload::prepare(fixture_client("complete"),spec,root.path,cancel);*cancel=true;
    REQUIRE_THROWS(staged->commit(cancel));staged.reset();REQUIRE(boost::filesystem::is_empty(root.path/"website"));
}
TEST_CASE("Repeated content reuses the registered version and refuses modified local bytes", "[PlatformLibrary]") {
    Directory root;const auto spec=DownloadSpec::from_detail(detail("complete"),file);
    auto first=StagedDownload::prepare(fixture_client("complete"),spec,root.path,cancellation());first->commit(cancellation());
    int calls=0;auto reused=StagedDownload::prepare(fixture_client("unused",[&](Cancellation){++calls;}),spec,root.path,cancellation());
    REQUIRE(reused->commit(cancellation())==spec.key());REQUIRE(calls==0);
    {boost::filesystem::ofstream edit(root.path/"website"/spec.key()/"model.glb");edit<<"useredit";}
    auto entries=Slic3r::GUI::ModelLibraryRepository::read(root.path,*cancellation());
    REQUIRE(entries.size()==1);REQUIRE_FALSE(entries.front().website_original_verified);
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("complete"),spec,root.path,cancellation()));
    REQUIRE(boost::filesystem::is_regular_file(root.path/"website"/spec.key()/"model.glb"));
}
TEST_CASE("Changed registration cannot overwrite an existing download history", "[PlatformLibrary]") {
    Directory root;const auto spec=DownloadSpec::from_detail(detail("complete"),file);
    StagedDownload::prepare(fixture_client("complete"),spec,root.path,cancellation())->commit(cancellation());
    const auto marker=root.path/"downloads"/("orcaslicer-ai-"+spec.key()+".json");
    {boost::filesystem::ofstream out(marker);out<<"{\"source\":\"user-edited\"}";}
    auto staged=StagedDownload::prepare(fixture_client("complete"),spec,root.path,cancellation());
    REQUIRE_THROWS(staged->commit(cancellation()));
    REQUIRE(file_digest(root.path/"website"/spec.key()/"model.glb")==spec.sha256);
    boost::filesystem::ifstream input(marker);Json record;input>>record;REQUIRE(record.at("source")=="user-edited");
}
TEST_CASE("A new website version keeps an independently edited older download", "[PlatformLibrary]") {
    Directory root;const auto old_spec=DownloadSpec::from_detail(detail("complete"),file);
    StagedDownload::prepare(fixture_client("complete"),old_spec,root.path,cancellation())->commit(cancellation());
    const auto original=root.path/"website"/old_spec.key()/"model.glb";
    {boost::filesystem::ofstream edit(original);edit<<"useredit";}
    auto newer=detail("newbytes");
    newer["version"]={{"id","44444444-4444-4444-8444-444444444444"},{"version",3}};
    const auto new_spec=DownloadSpec::from_detail(newer,file);
    const auto id=StagedDownload::prepare(fixture_client("newbytes"),new_spec,root.path,cancellation())->commit(cancellation());
    REQUIRE(id!=old_spec.key());
    REQUIRE(file_digest(original)==Slic3r::HonorAccount::digest("useredit"));
    REQUIRE(file_digest(root.path/"website"/id/"model.glb")==new_spec.sha256);
    const auto entries=Slic3r::GUI::ModelLibraryRepository::read(root.path,*cancellation());
    REQUIRE(entries.size()==2);
}
TEST_CASE("Network and disk failures never register a partial model", "[PlatformLibrary]") {
    Directory root;const auto spec=DownloadSpec::from_detail(detail("complete"),file);
    int calls=0;
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("complete",[&](Cancellation){++calls;}),spec,root.path,cancellation(),{},
        [](const boost::filesystem::path&){return 0;}));
    REQUIRE(calls==0);REQUIRE(boost::filesystem::is_empty(root.path/"website"));
    REQUIRE_THROWS(StagedDownload::prepare(fixture_client("complete",[](Cancellation){throw Error(0,"connection lost");}),spec,root.path,cancellation()));
    REQUIRE(boost::filesystem::is_empty(root.path/"website"));REQUIRE(boost::filesystem::is_empty(root.path/"downloads"));
}
TEST_CASE("An invalidated asynchronous query cannot publish into a later request", "[PlatformLibrary]") {
    Worker worker;std::mutex mutex;std::condition_variable ready;bool entered=false,release=false;
    worker.submit(1,"old","",[&](Cancellation){std::unique_lock<std::mutex> lock(mutex);entered=true;ready.notify_all();ready.wait(lock,[&]{return release;});return Worker::Value{1,{}};});
    bool started;
    {std::unique_lock<std::mutex> lock(mutex);started=ready.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});
        if(!started) release=true;}
    ready.notify_all();REQUIRE(started);
    worker.cancel();worker.submit(2,"new","",[](Cancellation){return Worker::Value{2,{}};});
    {std::lock_guard<std::mutex> lock(mutex);release=true;}ready.notify_all();
    std::deque<Worker::Result> results;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(results.empty() && std::chrono::steady_clock::now()<end){results=worker.take();std::this_thread::yield();}
    REQUIRE(results.size()==1);REQUIRE(results.front().epoch==2);REQUIRE(results.front().value.json==2);
}
namespace {
Client catalog_client(std::function<Json(const std::string&,Cancellation)> response,std::string account="account",std::uint64_t generation=4) {
    return Client({"https://platform.example.test","native-only-token",std::move(account),generation},
        [response](const Identity&,const std::string& path,const std::string&,const std::string&,
            Cancellation cancel,std::size_t,std::function<bool(const char*,std::size_t)>,std::function<void(std::size_t,std::size_t)>) {
            return Binary{200,response(path,cancel).dump(),"application/json"};
        });
}
Json owned_record(int n) {
    const auto id=std::string("11111111-1111-4111-8111-")+std::string(12-std::to_string(n).size(),'0')+std::to_string(n);
    return {{"id",id},{"version_id",version},{"version",2},{"title","作品 "+std::to_string(n)},
        {"created_at","2026-11-10T00:00:00Z"},{"visibility",n%2?"private":"public"},
        {"status",n%3?"published":"draft"},{"withdrawn_at",n==2?Json("2026-11-11"):Json()}};
}
}
TEST_CASE("Owned uploads read every hundred-item page and preserve all publication states", "[PlatformLibrary]") {
    std::vector<std::string> calls;
    auto c=catalog_client([&](const std::string& path,Cancellation) {
        calls.push_back(path);const auto offset=std::stoul(path.substr(path.find('=')+1));
        Json batch=Json::array();for(auto n=offset;n<std::min<unsigned long>(offset+100,205);++n) batch.push_back(owned_record(int(n)));
        return batch;
    });
    const auto items=c.owned_models(cancellation());REQUIRE(items.size()==205);
    REQUIRE(calls==std::vector<std::string>{"/v1/me/models?offset=0","/v1/me/models?offset=100","/v1/me/models?offset=200"});
    REQUIRE(items[0].at("status")=="draft");REQUIRE(items[1].at("visibility")=="private");
    REQUIRE(items[2].at("withdrawn")==true);REQUIRE(items[0].at("versionId")==version);
    REQUIRE_FALSE(items[0].contains("viewCount"));
}
TEST_CASE("Owned uploads normalize before search sort and pagination and isolate their account cache", "[PlatformLibrary]") {
    int calls=0;auto c=catalog_client([&](const std::string&,Cancellation) {
        ++calls;Json batch=Json::array();for(int i=0;i<30;++i) batch.push_back(owned_record(i));return batch;
    });
    const auto first=c.personal("models",{{"sort","title"},{"offset","0"}},cancellation());
    REQUIRE(first.at("items").size()==24);REQUIRE(first.at("hasMore")==true);REQUIRE(first.at("total")==30);
    auto second=c.personal("models",{{"sort","title"},{"offset","24"}},cancellation(),first.at("cache"));
    REQUIRE(second.at("items").size()==6);REQUIRE(second.at("hasMore")==false);REQUIRE(calls==1);
    const auto search=c.personal("models",{{"q","作品 12"}},cancellation(),first.at("cache"));
    REQUIRE(search.at("items").size()==1);REQUIRE(search.at("items")[0].at("title")=="作品 12");
    auto other=catalog_client([&](const std::string&,Cancellation){++calls;return Json::array();},"other");
    REQUIRE(other.personal("models",Json::object(),cancellation(),first.at("cache")).at("items").empty());REQUIRE(calls==2);
    auto refreshed=catalog_client([&](const std::string&,Cancellation){++calls;return Json::array();},"account",5);
    REQUIRE(refreshed.personal("models",Json::object(),cancellation(),first.at("cache")).at("items").empty());REQUIRE(calls==3);
    REQUIRE(c.personal("models",Json::object(),cancellation()).at("total")==30);REQUIRE(calls==4);
}
TEST_CASE("Partial account endpoints preserve uploaded models and successful activity without invented counts", "[PlatformLibrary]") {
    auto record=owned_record(1);
    const auto c=catalog_client([&](const std::string& path,Cancellation) {
        if(path.find("/me/models?")!=std::string::npos) return Json::array({record});
        if(path.find("/likes?")!=std::string::npos) return Json{{"items",Json::array({{{"id",record.at("id")},
            {"versionId",version},{"version",2},{"title","public title"},{"likeCount",7}}})},{"total",1}};
        throw Error(404,"not deployed");
    });
    const auto all=c.personal("all",Json::object(),cancellation());
    REQUIRE(all.at("items").size()==1);REQUIRE(all.at("items")[0].at("visibility")=="private");
    REQUIRE(all.at("items")[0].at("likeCount")==7);REQUIRE_FALSE(all.at("items")[0].contains("viewCount"));
    REQUIRE(all.at("unavailable").size()==3);
    auto offline=catalog_client([](const std::string&,Cancellation)->Json {throw Error(0,"offline");});
    REQUIRE(offline.personal("all",Json::object(),cancellation()).at("items").empty());
    REQUIRE(offline.personal("all",Json::object(),cancellation()).at("unavailable").size()==5);
}
TEST_CASE("Cancelled or nonadvancing owned pages never become a reusable collection", "[PlatformLibrary]") {
    int calls=0;auto stop=cancellation();
    auto cancelled=catalog_client([&](const std::string&,Cancellation c) {++calls;*c=true;return Json::array({owned_record(1)});});
    REQUIRE_THROWS(cancelled.personal("models",Json::object(),stop));REQUIRE(calls==1);
    auto repeats=catalog_client([&](const std::string&,Cancellation) {++calls;Json batch=Json::array();
        for(int i=0;i<100;++i) batch.push_back(owned_record(i));return batch;});
    REQUIRE_THROWS(repeats.owned_models(cancellation()));REQUIRE(calls==3);
}
TEST_CASE("Details remain usable when optional statistics or engagement fail and restrict private interactions", "[PlatformLibrary]") {
    auto value=detail("complete");value["author"]={{"id","account"}};value["visibility"]="public";value["withdrawn"]=false;
    value["versions"]=Json::array({{{"id",version},{"version",2},{"status","published"}}});
    int optional=0;auto c=catalog_client([&](const std::string& path,Cancellation) {
        if(path.find("?version=")!=std::string::npos) return value;
        ++optional;throw Error(503,"unavailable");return Json();
    });
    auto result=c.presentation_detail(model,2,"visitor",cancellation());
    REQUIRE(result.at("id")==model);REQUIRE(result.at("owned")==true);REQUIRE(result.at("statistics").empty());
    REQUIRE(result.at("publicInteraction")==true);REQUIRE(optional==3);
    value["visibility"]="private";value["versions"][0]["status"]="draft";
    result=c.presentation_detail(model,2,"visitor",cancellation());
    REQUIRE(result.at("publicInteraction")==false);REQUIRE(result.at("version").at("status")=="draft");REQUIRE(optional==3);
}
TEST_CASE("A malformed account source cannot discard other successful personal collections", "[PlatformLibrary]") {
    const auto c=catalog_client([](const std::string& path,Cancellation) {
        if(path.find("/me/models?")!=std::string::npos) return Json::array({owned_record(1)});
        if(path.find("/likes?")!=std::string::npos) return Json{{"items",Json::array()},{"total","invalid"}};
        throw Error(503,"unavailable");return Json();
    });
    const auto result=c.personal("all",Json::object(),cancellation());
    REQUIRE(result.at("items").size()==1);REQUIRE(result.at("unavailable").size()==4);
}

TEST_CASE("The installed library handshake survives account publication and rejects obsolete documents", "[PlatformLibrary][LibraryBridge]") {
    using Channel=Slic3r::GUI::LibraryPresentationChannel;
    Channel channel;
    auto init=channel.begin("document-one");
    channel.push({{"type","account"},{"accountTag","first"},{"accountGeneration",4u}});
    REQUIRE(channel.take().empty());
    REQUIRE(init.at("type")=="init");
    Json ready={{"command","ready"},{"nonce","document-one"},{"documentGeneration",init.at("documentGeneration")}};
    REQUIRE(channel.accept(ready)==Channel::Acceptance::Ready);
    REQUIRE(channel.accept(ready)==Channel::Acceptance::Rejected);
    channel.push({{"type","account"},{"accountTag","first"},{"accountGeneration",4u}});
    channel.push({{"type","catalog"},{"items",Json::array({owned_record(1)})}});
    auto batch=channel.take();REQUIRE(batch.size()==2);
    REQUIRE(batch[1].at("accountTag")=="first");REQUIRE(batch[1].at("requestGeneration")==0);
    Json command={{"command","query"},{"nonce","document-one"},{"documentGeneration",init.at("documentGeneration")},
        {"accountTag","first"},{"accountGeneration",4u},{"requestGeneration",1u}};
    REQUIRE(channel.accept(command)==Channel::Acceptance::Command);
    REQUIRE(channel.accept(command)==Channel::Acceptance::Rejected);
    channel.push({{"type","catalog"},{"items",Json::array({owned_record(1)})}});
    auto old=channel.take();
    channel.push({{"type","account"},{"accountTag","second"},{"accountGeneration",5u}});
    channel.retry(std::move(old));
    batch=channel.take();REQUIRE(batch.size()==1);REQUIRE(batch[0].at("accountTag")=="second");
    command["requestGeneration"]=2u;REQUIRE(channel.accept(command)==Channel::Acceptance::Rejected);
    const auto newer=channel.begin("document-two");
    REQUIRE(newer.at("documentGeneration")!=init.at("documentGeneration"));
    REQUIRE(channel.accept(ready)==Channel::Acceptance::Rejected);
    REQUIRE(channel.take().empty());
}

TEST_CASE("Library presentation coalesces progress and retries only current snapshots", "[PlatformLibrary][LibraryBridge]") {
    using Channel=Slic3r::GUI::LibraryPresentationChannel;
    Channel channel;const auto init=channel.begin("nonce");
    REQUIRE(channel.accept({{"command","ready"},{"nonce","nonce"},
        {"documentGeneration",init.at("documentGeneration")}})==Channel::Acceptance::Ready);
    channel.push({{"type","account"},{"accountTag","account"},{"accountGeneration",4u}});
    for(unsigned i=0;i<1000;++i) channel.push({{"type","loading"},{"done",i}});
    auto batch=channel.take();REQUIRE(batch.size()==2);REQUIRE(batch.back().at("done")==999);
    channel.retry(batch);REQUIRE(channel.take()==batch);
    channel.push({{"type","loading"},{"active",true}});
    REQUIRE(channel.accept({{"command","cancel"},{"nonce","nonce"},{"documentGeneration",init.at("documentGeneration")},
        {"accountTag","account"},{"accountGeneration",4u},{"requestGeneration",1u}})==Channel::Acceptance::Command);
    REQUIRE(channel.take().empty());
    channel.retry(batch);REQUIRE(channel.take().empty());
    channel.push({{"type","loading"},{"active",false}});
    REQUIRE(channel.take()[0].at("requestGeneration")==1);
}

TEST_CASE("Personal uploads are presented before optional activity requests finish", "[PlatformLibrary][LibraryBridge]") {
    int snapshots=0;bool got_uploads=false;
    const auto c=catalog_client([&](const std::string& path,Cancellation) {
        if(path.find("/me/models?")!=std::string::npos) return Json::array({owned_record(1)});
        REQUIRE(got_uploads);
        throw Error(503,"unavailable");return Json();
    });
    const auto result=c.personal("all",Json::object(),cancellation(),Json::object(),[&](const Json& value) {
        ++snapshots;REQUIRE(value.at("items").size()==1);got_uploads=true;
    });
    REQUIRE(snapshots==5);REQUIRE(result.at("items").size()==1);
    REQUIRE(result.at("unavailable").size()==4);
}

TEST_CASE("An invalid owned upload cannot hide the remaining valid uploads", "[PlatformLibrary][LibraryBridge]") {
    auto invalid=owned_record(2);invalid["version_id"]=nullptr;
    const auto c=catalog_client([&](const std::string&,Cancellation) {return Json::array({owned_record(1),invalid,owned_record(3)});});
    const auto result=c.personal("models",Json::object(),cancellation());
    REQUIRE(result.at("items").size()==2);REQUIRE(result.at("unavailable")==Json::array({"models"}));
    const auto cached=c.personal("models",Json::object(),cancellation(),result.at("cache"));
    REQUIRE(cached.at("items").size()==2);REQUIRE(cached.at("unavailable")==result.at("unavailable"));
}

TEST_CASE("A rejected library queue never executes its download command", "[PlatformLibrary][LibraryDownload]") {
    Worker worker;worker.stop();bool executed=false;
    REQUIRE_FALSE(worker.submit(1,"download",file,[&](Cancellation){executed=true;return Worker::Value{};},true));
    REQUIRE_FALSE(executed);REQUIRE(worker.take().empty());
}

TEST_CASE("Cancelled library progress and partial results cannot contaminate the next request", "[PlatformLibrary][LibraryDownload]") {
    Worker worker;std::promise<void> entered,release;const auto released=release.get_future().share();
    Cancellation old;
    REQUIRE(worker.submit(1,"old","",[&](Cancellation cancel) {
        old=cancel;worker.progress(10,100,cancel);entered.set_value();released.wait();
        worker.progress(99,100,cancel);worker.publish(1,"catalog","partial",Worker::Value{1,{}},cancel);
        return Worker::Value{};
    }));
    const auto started=entered.get_future().wait_for(std::chrono::seconds(2));
    if(started!=std::future_status::ready) release.set_value();
    REQUIRE(started==std::future_status::ready);
    REQUIRE(worker.bytes().first==10);worker.cancel();release.set_value();
    REQUIRE(worker.submit(2,"new","",[&](Cancellation cancel) {worker.progress(5,50,cancel);return Worker::Value{2,{}};}));
    std::deque<Worker::Result> results;const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(results.empty() && std::chrono::steady_clock::now()<end) {results=worker.take();std::this_thread::yield();}
    REQUIRE(results.size()==1);REQUIRE(results.front().epoch==2);REQUIRE(worker.bytes().first==5);
    REQUIRE_FALSE(worker.publish(1,"catalog","partial",Worker::Value{1,{}},old));
}

TEST_CASE("Download failures leave Chinese-path storage clean and keep existing files", "[PlatformLibrary][LibraryDownload]") {
    Directory temporary;const auto root=temporary.path/boost::filesystem::path(L"中文模型目录");
    const auto spec=DownloadSpec::from_detail(detail("complete"),file);
    for(const unsigned status:{401u,403u,404u}) {
        auto client=fixture_client("complete",[status](Cancellation){throw Error(status,"下载请求未完成");});
        try {StagedDownload::prepare(client,spec,root,cancellation());FAIL("Expected rejected download");}
        catch(const Error& error) {REQUIRE(error.status==status);}
        REQUIRE(boost::filesystem::is_empty(root/"website"));REQUIRE(boost::filesystem::is_empty(root/"downloads"));
    }
    REQUIRE(StagedDownload::prepare(fixture_client("complete"),spec,root,cancellation())->commit(cancellation())==spec.key());
    REQUIRE(file_digest(root/"website"/spec.key()/"model.glb")==spec.sha256);
}
