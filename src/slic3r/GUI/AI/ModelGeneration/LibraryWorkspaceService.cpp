#include "LibraryWorkspaceService.hpp"
#include "ModelLibraryLoaderFactory.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "slic3r/GUI/Redesign/PlatformAccountService.hpp"
#include "slic3r/Utils/HonorAccount/HonorOAuth.hpp"
#include <wx/base64.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <algorithm>
#include <set>

namespace Slic3r::GUI {
namespace PL=PlatformLibrary;
namespace {
std::string utf8(const wxString& text) {return std::string(text.utf8_string());}
std::string thumbnail_data(const ModelLibraryLoader::Thumbnail& thumbnail) {
    if(thumbnail.width<=0 || thumbnail.height<=0 || thumbnail.rgb.empty()) return {};
    wxImage image(thumbnail.width,thumbnail.height);std::copy(thumbnail.rgb.begin(),thumbnail.rgb.end(),image.GetData());
    if(!thumbnail.alpha.empty()) {image.InitAlpha();std::copy(thumbnail.alpha.begin(),thumbnail.alpha.end(),image.GetAlpha());}
    wxMemoryOutputStream stream;if(!image.SaveFile(stream,wxBITMAP_TYPE_PNG)) return {};
    const auto size=stream.GetSize();std::vector<char> data(size);stream.CopyTo(data.data(),size);
    return "data:image/png;base64,"+utf8(wxBase64Encode(data.data(),size));
}
bool one_of(const std::string& value,std::initializer_list<const char*> values) {
    return std::any_of(values.begin(),values.end(),[&](const char* allowed){return value==allowed;});
}
}
LibraryWorkspaceService::LibraryWorkspaceService(std::shared_ptr<PlatformAccountService> account,
    std::shared_ptr<ModelLibraryApplication> local,Ports ports)
    :m_account(std::move(account)),m_local(std::move(local)),m_ports(std::move(ports)),
     m_loader(make_model_library_loader(true)),m_timer(this)
{
    // Visitor identity has no account credential and is scoped to this application session.
    const auto random=HonorAccount::random_value();
    const auto hex=HonorAccount::digest(random);
    m_visitor=hex.substr(0,8)+"-"+hex.substr(8,4)+"-4"+hex.substr(13,3)+"-8"+hex.substr(17,3)+"-"+hex.substr(20,12);
    m_account->subscribe(this,[this]{account_changed();});
    Bind(wxEVT_TIMER,[this](wxTimerEvent&){poll();},m_timer.GetId());m_timer.Start(100);
    account_changed();
    Presentation::background_tasks().on_shutdown(this, [this] {
        m_timer.Stop(); m_emit={}; m_account->unsubscribe(this); invalidate();
        auto remote = m_worker.begin_shutdown();
        auto local = m_loader->begin_shutdown();
        Presentation::background_tasks().retire(remote);
        Presentation::background_tasks().retire(local);
    });
}
LibraryWorkspaceService::~LibraryWorkspaceService() {
    Presentation::background_tasks().remove_shutdown_hook(this);
    m_account->unsubscribe(this);m_timer.Stop();m_emit={};m_worker.stop();m_loader->stop();
}
void LibraryWorkspaceService::emit(Json value) {if(m_visible && m_ready && m_emit) m_emit(value);}
std::string LibraryWorkspaceService::key(const Json& item) {
    return item.value("id","")+":"+item.value("versionId","");
}
LibraryWorkspaceService::Client LibraryWorkspaceService::client() const {
    const auto& session=m_account->session();PL::Identity identity;
    identity.api=session.config().api_url;identity.generation=session.generation();
    if(const auto lease=session.access()) {identity.bearer=lease->access_token;identity.subject=lease->subject;}
    return Client(std::move(identity));
}
void LibraryWorkspaceService::invalidate() {
    ++m_epoch;m_worker.cancel();m_image_requests.clear();m_downloading=false;m_mutating=false;m_geometry_pending=false;m_geometry.reset();m_geometry_offset=0;
}
void LibraryWorkspaceService::account_changed() {
    const auto lease=m_account->session().access();const auto generation=m_account->session().generation();
    const std::string subject=lease?lease->subject:"";
    if(generation!=m_generation || subject!=m_subject) {
        if(!m_subject.empty() && subject!=m_subject) {
            m_pending_download_file.clear();m_pending_download_key.clear();
        }
        m_resume_detail=!m_detail_key.empty();invalidate();m_generation=generation;m_subject=subject;
        m_models.clear();m_detail=Json();m_personal=Json::object();
        publish_account();
        if(m_visible && m_ready) list();
    } else publish_account();
}
void LibraryWorkspaceService::publish_account() {
    const auto& snapshot=m_account->session().snapshot();
    emit({{"type","account"},{"signedIn",bool(m_account->session().access())},
        {"name",snapshot.identity.name},{"accountTag",HonorAccount::digest(m_subject)},
        {"accountGeneration",m_generation},{"projectAvailable",m_project}});
}
void LibraryWorkspaceService::visible(bool value) {
    if(m_visible==value) return;
    if(!value) emit({{"type","inactive"}});
    m_visible=value;
    if(!value) {invalidate();++m_scan;m_loader->cancel();return;}
    m_personal=Json::object();m_resume_detail=!m_detail_key.empty();m_account->start();publish_account();refresh_local();
    if(m_ready) list();
}
void LibraryWorkspaceService::ready(bool value) {
    if(!value) {m_ready=false;invalidate();++m_scan;m_loader->cancel();return;}
    if(m_ready) return;
    m_ready=true;publish_account();if(m_visible) {refresh_local();list();}
}
bool LibraryWorkspaceService::require_login() {
    if(m_account->session().access()) return true;
    if(m_ports.login) m_ports.login();return false;
}
void LibraryWorkspaceService::refresh_local() {
    if(!m_local) return;
    auto request=std::make_shared<ModelLibraryLoader::Request>();request->revision=++m_scan;
    request->root=m_local->root();request->scan=true;
    if(!m_loader->submit(std::move(request)))
        emit({{"type","error"},{"message","本地模型读取队列暂不可用，已有模型已保留，请重试。"}});
}
void LibraryWorkspaceService::local_cards() {
    if(!m_local) return;
    Json cards=Json::array();
    for(const auto& e:m_local->entries()) {
        cards.push_back({{"key",e.job_id},{"title",utf8(e.title)},{"details",utf8(e.details)},
            {"local",true},{"designOnly",e.design_only},{"generatedAt",e.generated_at},
            {"printFeedback",e.print_feedback},{"imported",e.imported_at>0},
            {"modelId",e.website_model_id},{"versionId",e.website_version_id},{"sha256",e.website_sha256},
            {"sourceVerified",e.website_original_verified},{"extension",e.model_path.extension().string()}});
    }
    emit({{"type","local"},{"revision",m_local->revision()},{"items",cards}});
}
void LibraryWorkspaceService::list() {
    if(!m_visible || !m_ready) return;
    invalidate();if(m_query.value("offset","0")=="0") m_models.clear();
    emit({{"type","loading"},{"active",true},{"stage","正在查询模型…"}});
    const auto c=client();const auto filters=m_query;const auto scope=m_scope,filter=m_filter;
    if(scope=="mine" && filter=="prints") {
        local_cards();emit({{"type","catalog"},{"items",Json::array()},{"hasMore",false},{"replace",true}});return;
    }
    if(scope=="mine" && filter!="all" && filter!="models" && c.identity().bearer.empty()) {
        emit({{"type","catalog"},{"items",Json::array()},{"hasMore",false},{"replace",true},{"loginRequired",true}});return;
    }
    const auto personal=m_personal;
    const auto epoch=m_epoch;
    const bool accepted=m_worker.submit(epoch,"catalog","",[this,c,filters,scope,filter,personal,epoch](PL::Cancellation cancel) {
        Json result;
        if(scope=="library") result=c.list(filters,cancel);
        else if(filter=="all" || filter=="models") result=c.personal(filter,filters,cancel,personal,
            [this,epoch,cancel](const Json& partial) {m_worker.publish(epoch,"catalog","partial",Value{partial,{}},cancel);});
        else result=c.activity(filter,filters,cancel);
        if(!result.contains("hasMore")) result["hasMore"]=result.value("offset",0)+result.value("limit",24)<result.value("total",0);
        return Value{std::move(result),{}};
    },true);
    if(!accepted) emit({{"type","error"},{"message","模型查询队列暂不可用，请重试。"}});
    else m_worker.submit(epoch,"taxonomy","",[c](PL::Cancellation cancel) {
        return Value{c.query("/v1/library/taxonomy",cancel),{}};
    });
    local_cards();
}
void LibraryWorkspaceService::images(const Json& items) {
    const auto c=client();
    for(const auto& item:items) {
        const auto path=item.value("coverUrl","");const auto id=key(item);
        if(!PL::preview_path(path)) continue;
        if(!m_image_requests.insert(id).second) continue;
        if(!m_worker.submit(m_epoch,"image",id,[c,path](PL::Cancellation cancel){return Value{c.image(path,cancel),{}};}))
            m_image_requests.erase(id);
    }
}
void LibraryWorkspaceService::show_detail(const std::string& selection) {
    const auto it=m_models.find(selection);if(it==m_models.end()) return;
    m_detail_key=selection;m_detail=Json();const auto item=it->second;m_detail_selection=item;invalidate();
    emit({{"type","loading"},{"active",true},{"stage","正在读取模型详情…"}});
    const auto c=client();const auto visitor=m_visitor;
    if(!m_worker.submit(m_epoch,"detail",selection,[c,item,visitor](PL::Cancellation cancel) {
        return Value{c.presentation_detail(item.at("id").get<std::string>(),item.at("version").get<int>(),visitor,cancel),{}};
    },true)) emit({{"type","error"},{"message","模型详情队列暂不可用，请重试。"}});
}
void LibraryWorkspaceService::start_download(const std::string& file, bool resumed) {
    if(m_downloading || m_detail.is_null() || !m_local) return;
    if(!PL::uuid(file)) {emit({{"type","error"},{"message","请选择有效的模型文件"}});return;}
    if(!resumed) m_download_reauth_attempted=false;
    m_pending_download_file=file;m_pending_download_key=m_detail_key;
    if(!require_login()) return;
    const auto c=client();const auto model=m_detail.value("id","");
    const auto version=m_detail.at("version").at("version").get<int>();
    // Re-read the selected immutable version with the current account before requesting bytes.
    invalidate();
    const auto root=m_local->root();
    const bool accepted=m_worker.submit(m_epoch,"download",file,[this,c,model,version,file,root](PL::Cancellation cancel) {
        const auto detail=c.detail(model,version,cancel);const auto spec=PL::DownloadSpec::from_detail(detail,file);
        auto staged=PL::StagedDownload::prepare(c,spec,root,cancel,
            [this,cancel](std::size_t done,std::size_t total){m_worker.progress(done,total,cancel);});
        return Value{Json::object(),std::move(staged)};
    },true);
    m_downloading=accepted;
    if(accepted) emit({{"type","loading"},{"active",true},{"stage","正在验证下载资格…"},{"cancelable",true}});
    else emit({{"type","error"},{"message","下载队列暂不可用，请重试。"}});
}
void LibraryWorkspaceService::local_action(const Json& command) {
    if(!m_local) return;
    const auto id=command.value("key","");const auto revision=command.value("revision",std::uint64_t(0));
    const ModelLibraryActionToken token{revision,id};const auto* entry=m_local->find(token);if(!entry) return;
    const auto action=command.value("action","");auto copy=*entry;
    if((action=="open" || action=="import") && !copy.design_only &&
        copy.model_path.extension()!=".glb" && copy.model_path.extension()!=".obj") {
        if(m_local->can_replace() && m_ports.native_import) m_ports.native_import(copy.model_path);return;
    }
    static const std::map<std::string,ModelLibraryCommand> actions={
        {"open",ModelLibraryCommand::OpenModel},{"image",ModelLibraryCommand::OpenImage},
        {"import",ModelLibraryCommand::Import},{"retexture",ModelLibraryCommand::Retexture},
        {"remove",ModelLibraryCommand::Remove},{"colors",ModelLibraryCommand::MatchColors},
        {"export",ModelLibraryCommand::Export},{"print",ModelLibraryCommand::RecordPrintResult}};
    const auto found=actions.find(action);if(found==actions.end()) return;
    if(!copy.design_only && copy.model_path.extension()!=".glb" && copy.model_path.extension()!=".obj" &&
       (action=="colors" || action=="retexture")) return;
    if(!m_local->execute(copy.design_only && action=="open"?ModelLibraryCommand::OpenImage:found->second,token))
        emit({{"type","error"},{"message","当前模型正在处理或此操作暂不可用"}});
    refresh_local();
}
void LibraryWorkspaceService::command(const Json& message) {
    if(!m_visible || !message.is_object()) return;
    try {
        const auto cmd=message.value("command","");
        if(m_downloading && cmd!="cancel") return;
        if(cmd=="query") {
            const auto scope=message.value("scope","library"),filter=message.value("filter","all");
            if(!one_of(scope,{"library","mine"}) || !one_of(filter,{"all","models","likes","favorites","downloads","prints","views"})) return;
            if(m_scope!=scope && scope=="mine") m_personal=Json::object();
            m_pending_download_file.clear();m_pending_download_key.clear();
            m_scope=scope;m_filter=filter;m_query=message.value("query",Json::object());m_detail_key.clear();m_detail=Json();m_detail_selection=Json();m_resume_detail=false;list();
        } else if(cmd=="detail") show_detail(message.value("key",""));
        else if(cmd=="back") {m_pending_download_file.clear();m_pending_download_key.clear();m_detail_key.clear();m_detail=Json();list();}
        else if(cmd=="login") require_login();
        else if(cmd=="refresh") {m_personal=Json::object();refresh_local();list();}
        else if(cmd=="local") local_action(message);
        else if(cmd=="chooseLocal") {if(m_local) m_local->choose_local();refresh_local();}
        else if(cmd=="storage") {if(m_ports.storage) m_ports.storage();}
        else if(cmd=="saveProject") {if(m_project && m_ports.save_project) m_ports.save_project();}
        else if(cmd=="cancel") {m_pending_download_file.clear();m_pending_download_key.clear();invalidate();emit({{"type","loading"},{"active",false}});}
        else if(cmd=="download") start_download(message.value("file",""));
        else if(cmd=="toggle") {
            if(m_detail.is_null() || !m_detail.value("publicInteraction",false) || !require_login() || m_mutating) return;
            const auto kind=message.value("kind","");if(!one_of(kind,{"like","favorite"})) return;
            const auto c=client();const auto id=m_detail.at("id").get<std::string>();const bool active=message.value("active",true);
            m_mutating=true;
            if(!m_worker.submit(m_epoch,"toggle",m_detail_key,[c,id,kind,active](PL::Cancellation cancel) {
                c.toggle(id,kind,active,cancel);auto stats=c.query("/v1/library/models/"+id+"/statistics",cancel);
                const auto engagement=c.query("/v1/models/"+id+"/engagement",cancel);stats["liked"]=engagement.value("liked",false);
                return Value{stats,{}};
            },true)) {m_mutating=false;emit({{"type","error"},{"message","操作队列暂不可用，请重试。"}});}
        } else if(cmd=="geometry") {
            if(m_detail.is_null() || !require_login() || m_downloading || m_geometry_pending || m_geometry) return;
            if(!m_detail.value("access",Json::object()).value("allowed",false)) {emit({{"type","error"},{"message","当前账号没有交互预览资格"}});return;}
            const auto c=client();const auto id=m_detail.at("id").get<std::string>();const auto version=m_detail.at("version").at("version").get<int>();
            m_geometry_pending=true;
            if(!m_worker.submit(m_epoch,"geometry",m_detail_key,[c,id,version](PL::Cancellation cancel) {
                auto response=c.bytes("/v1/library/models/"+id+"/geometry?version="+std::to_string(version),cancel,67108864);
                return Value{Json(),{},std::make_shared<const std::string>(std::move(response.bytes))};
            },true)) m_geometry_pending=false;
        }
    } catch(const PL::Error& e) {emit({{"type","error"},{"message",e.what()}});}
      catch(...) {emit({{"type","error"},{"message","模型库操作无效"}});}
}
void LibraryWorkspaceService::poll() {
    try {
    const auto lease=m_account->session().access();
    if(m_generation!=m_account->session().generation() || m_subject!=(lease?lease->subject:"")) account_changed();
    std::optional<ModelLibraryLoader::Snapshot> snapshot;std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
    m_loader->take(snapshot,thumbnails,4);
    if(m_visible && snapshot && snapshot->revision==m_scan && snapshot->failed)
        emit({{"type","error"},{"message","本地模型读取失败，已有模型与原文件已保留，请刷新重试。"}});
    if(m_visible && snapshot && snapshot->revision==m_scan && !snapshot->failed && m_local) {
        m_local->replace(std::move(snapshot->entries));local_cards();
        auto request=std::make_shared<ModelLibraryLoader::Request>();request->revision=m_scan;request->root=m_local->root();
        request->entries=m_local->entries();request->edge=420;m_loader->submit(std::move(request));
    }
    for(const auto& thumbnail:thumbnails) if(m_visible && thumbnail.revision==m_scan && m_local && thumbnail.index<m_local->entries().size()) {
        emit({{"type","image"},{"key",m_local->entries()[thumbnail.index].job_id},{"data",thumbnail_data(thumbnail)}});
    }
    if(m_downloading) {const auto bytes=m_worker.bytes();emit({{"type","loading"},{"active",true},
        {"stage",bytes.first?"正在下载并校验模型…":"正在验证下载资格…"},{"done",bytes.first},{"total",bytes.second},{"cancelable",true}});}
    for(auto& result:m_worker.take()) {
        if(result.epoch!=m_epoch || !m_visible) continue;
        if(!result.error.empty()) {
            if(result.kind=="image" || result.kind=="detailImage" || result.kind=="taxonomy") continue;
            if(result.kind=="download") {
                m_downloading=false;
                if(result.status!=401 || m_download_reauth_attempted) {m_pending_download_file.clear();m_pending_download_key.clear();}
                else m_download_reauth_attempted=true;
            }
            if(result.kind=="toggle") m_mutating=false;
            if(result.kind=="geometry") m_geometry_pending=false;
            emit({{"type","loading"},{"active",false}});
            if(result.status==401 && m_account->session().access()) m_account->session().refresh_access(m_generation);
            emit({{"type","error"},{"message",result.error},{"loginRequired",result.status==401}});continue;
        }
        if(result.kind=="taxonomy") emit({{"type","taxonomy"},{"value",result.value.json}});
        else if(result.kind=="catalog") {
            if(result.value.json.contains("cache")) {m_personal=std::move(result.value.json["cache"]);result.value.json.erase("cache");}
            if(result.value.json.value("reauthRequired",false) && lease) m_account->session().refresh_access(m_generation);
            for(const auto& item:result.value.json.at("items")) m_models[key(item)]=item;
            auto payload=result.value.json;payload["type"]="catalog";payload["replace"]=m_query.value("offset","0")=="0";emit(payload);images(payload.at("items"));
            if(m_resume_detail && !m_detail_key.empty()) {
                m_resume_detail=false;
                if(!m_detail_selection.is_null()) {m_models[m_detail_key]=m_detail_selection;show_detail(m_detail_key);}
                else {emit({{"type","error"},{"message","原模型在当前账号或筛选中已不可用"}});m_detail_key.clear();}
            }
        } else if(result.kind=="detail") {
            if(result.key!=m_detail_key) continue;m_detail=result.value.json;
            for(const auto& version:m_detail.value("versions",Json::array())) if(m_detail.value("owned",false) || version.value("status","")=="published") {
                Json entry={{"id",m_detail.at("id")},{"versionId",version.at("id")},{"version",version.at("version")}};
                m_models[key(entry)]=entry;
            }
            emit({{"type","detail"},{"value",m_detail},{"key",result.key}});
            const auto c=client();Json media=m_detail.value("media",Json::array());
            media.push_back({{"url",m_detail.value("previewUrl","")}});
            for(const auto& image:media) {const auto path=image.value("url","");if(!PL::preview_path(path)) continue;
                m_worker.submit(m_epoch,"detailImage",path,[c,path](PL::Cancellation cancel){return Value{c.image(path,cancel),{}};});}
            if(!m_pending_download_file.empty() && m_pending_download_key==result.key && lease) {
                const auto file=m_pending_download_file;start_download(file,true);
            }
        } else if(result.kind=="geometry") {
            if(result.key!=m_detail_key) continue;
            m_geometry_pending=false;
            m_geometry=std::move(result.value.geometry);m_geometry_offset=0;
            if(m_geometry) emit({{"type","geometryBegin"},{"key",m_detail_key},{"epoch",m_epoch},{"total",m_geometry->size()}});
        } else if(result.kind=="image" || result.kind=="detailImage")
            emit({{"type",result.kind},{"key",result.key},{"data",result.value.json}});
        else if(result.kind=="toggle") {m_mutating=false;if(result.key==m_detail_key) emit({{"type","statistics"},{"value",result.value.json}});}
        else if(result.kind=="download") {
            m_downloading=false;
            m_pending_download_file.clear();m_pending_download_key.clear();
            try {
                // No GUI yield between lease validation and atomic publication.
                if(!m_account->session().access() || m_generation!=m_account->session().generation()) continue;
                const auto id=result.value.download->commit(std::make_shared<std::atomic_bool>(false));
                emit({{"type","downloaded"},{"key",id}});refresh_local();
            } catch(const std::exception& e) {emit({{"type","error"},{"message",e.what()}});}
        }
    }
    geometry_chunks();
    } catch(const std::exception&) {
        invalidate();emit({{"type","error"},{"message","模型库响应无效，请刷新后重试"}});
    }
}
void LibraryWorkspaceService::geometry_chunks() {
    if(!m_geometry || !m_visible) return;
    for(int i=0;i<2 && m_geometry_offset<m_geometry->size();++i) {
        const auto size=std::min<std::size_t>(192*1024,m_geometry->size()-m_geometry_offset);
        emit({{"type","geometryChunk"},{"key",m_detail_key},{"epoch",m_epoch},{"offset",m_geometry_offset},
            {"data",utf8(wxBase64Encode(m_geometry->data()+m_geometry_offset,size))}});
        m_geometry_offset+=size;
    }
    if(m_geometry_offset==m_geometry->size()) {
        emit({{"type","geometryEnd"},{"key",m_detail_key},{"epoch",m_epoch}});m_geometry.reset();m_geometry_offset=0;
    }
}
}
