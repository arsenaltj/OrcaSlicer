#include "LibraryWorkspaceService.hpp"
#include "ModelLibraryLoaderFactory.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
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
using DownloadPhase = LibraryDownloadRequest::Phase;
const char* phase_name(DownloadPhase phase)
{
    switch (phase) {
    case DownloadPhase::Checking: return "checking";
    case DownloadPhase::Transfer: return "transfer";
    case DownloadPhase::Verify: return "verify";
    case DownloadPhase::WaitingForAccount: return "refreshing";
    case DownloadPhase::Registering: return "registering";
    case DownloadPhase::Complete: return "complete";
    case DownloadPhase::Failed: return "failed";
    case DownloadPhase::Cancelled: return "cancelled";
    }
    return "";
}
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
    m_account->subscribe(this,[this]{account_changed(true);});
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
void LibraryWorkspaceService::invalidate_remote() {
    ++m_epoch;m_worker.cancel();m_image_requests.clear();m_mutating=false;m_geometry_pending=false;m_geometry.reset();m_geometry_offset=0;
}
void LibraryWorkspaceService::invalidate() {
    invalidate_remote();
    m_resume_geometry = false;
    m_download.reset();
    m_native_import.reset();
    if (m_import_running && !m_import_publishing) m_import_cancelled = true;
}
void LibraryWorkspaceService::account_changed(bool publish) {
    const auto& session = m_account->session();
    const auto& snapshot = session.snapshot();
    const auto lease = session.access();
    const std::string& subject = lease ? lease->subject : snapshot.identity.subject;
    const auto logout = m_account->logout_revision();
    const bool identity_changed = m_api != session.config().api_url || logout != m_logout_revision ||
        (!subject.empty() && subject != m_subject);
    const bool refreshing = snapshot.state == HonorAccount::State::Checking ||
        (snapshot.state == HonorAccount::State::SignedIn && !lease);
    const auto lifetime = lease ? lease->lifetime : PL::Cancellation{};
    const bool credentials_changed = lifetime != m_access_lifetime || refreshing != m_account_refreshing;
    m_access_lifetime = lifetime;
    m_account_refreshing = refreshing;
    if (identity_changed) {
        // Keep guest selections when signing in for the first time. An account
        // switch or explicit logout drops all selections belonging to that account.
        m_resume_detail = m_subject.empty() && logout == m_logout_revision && m_api == session.config().api_url &&
            !m_detail_key.empty();
        if (!m_resume_detail) {
            m_detail_key.clear();
            m_detail_selection = Json();
        }
        m_subject = subject;
        m_api = session.config().api_url;
        m_logout_revision = logout;
        ++m_account_generation;
        invalidate();
        m_models.clear();m_detail=Json();m_personal=Json::object();m_comment_body.clear();m_comment_token.clear();
        m_query["offset"]="0";
        publish_account();
        if (m_visible && m_ready && !refreshing) list();
        return;
    }
    if (!credentials_changed) {
        if (publish) publish_account();
        return;
    }

    m_resume_geometry = m_resume_geometry || m_geometry_pending || bool(m_geometry);
    invalidate_remote();
    if (m_download) m_download->credentials_changed(bool(lease) || refreshing);
    if (lease && m_download && m_download->login_required) {
        m_download->login_required = false;
        m_download->message = "登录已恢复，请重试下载。";
    }
    publish_account();
    if (!m_visible || !m_ready) return;
    if (lease && m_download && m_download->resume()) queue_download();
    else publish_download();
    if (m_detail_key.empty() || m_resume_detail) {
        if (!refreshing) {m_query["offset"]="0";list();}
    } else if (m_detail.is_null()) {
        if (!refreshing) request_detail();
    } else if (lease) {
        request_detail_content(true);
        if (m_resume_geometry) request_geometry(false);
        m_resume_geometry = false;
    }
}
void LibraryWorkspaceService::publish_account() {
    const auto& snapshot=m_account->session().snapshot();
    emit({{"type","account"},{"signedIn",bool(m_account->session().access())},
        {"name",snapshot.identity.name},{"accountTag",HonorAccount::digest(m_subject)},
        {"avatarData",m_account->avatar().data},
        {"accountGeneration",m_account_generation},{"refreshing",m_account_refreshing},
        {"loginRequired",!m_subject.empty() && !m_account_refreshing && !m_access_lifetime},
        {"hasDetail",!m_detail_key.empty()},{"projectAvailable",m_project}});
}
void LibraryWorkspaceService::visible(bool value) {
    if(m_visible==value) return;
    if(!value) emit({{"type","inactive"}});
    m_visible=value;
    if(!value) {
        auto terminal_download = !download_active() ? std::move(m_download) : std::optional<LibraryDownloadRequest>{};
        invalidate();m_download = std::move(terminal_download);
        ++m_scan;m_loader->cancel();m_local_scan_running=m_local_rescan_pending=false;return;
    }
    if (m_upload_pending) m_query["offset"]="0";
    m_upload_pending=false;m_personal=Json::object();m_resume_detail=!m_detail_key.empty();m_account->start();publish_account();refresh_local();
    if (!m_ready) return;
    if (m_resume_detail && m_detail.is_object()) {
        // The same document survives an open into the workbench. Reuse its
        // registered recommendations and leave the list's page state intact.
        m_resume_detail = false;
        publish_file_actions();
        publish_download();
        request_geometry(false);
    } else list();
}
void LibraryWorkspaceService::ready(bool value) {
    if(!value) {m_ready=false;invalidate();++m_scan;m_loader->cancel();m_local_scan_running=m_local_rescan_pending=false;return;}
    if(m_ready) return;
    m_resume_detail = !m_detail_key.empty();
    m_ready=true;publish_account();if(m_visible) {refresh_local();list();}
}
void LibraryWorkspaceService::activated()
{
    if (!m_visible || !m_ready || !m_upload_pending || !m_detail_key.empty()) return;
    m_upload_pending = false;
    m_personal = Json::object();
    m_query["offset"] = "0";
    list();
}
bool LibraryWorkspaceService::require_login() {
    if(m_account->session().access()) return true;
    if (m_account_refreshing) {
        emit({{"type","error"},{"message","正在刷新登录状态，请稍候。"}});
        return false;
    }
    if(m_ports.login) m_ports.login();return false;
}
void LibraryWorkspaceService::refresh_local() {
    if(!m_local) return;
    if(m_local_scan_running) {m_local_rescan_pending=true;return;}
    auto request=std::make_shared<ModelLibraryLoader::Request>();request->revision=++m_scan;
    request->root=m_local->root();request->scan=true;
    m_scan_removal_revision=m_local->removal_revision();
    m_scan_commit_revision=m_local->commit_revision();
    m_local_scan_running=m_loader->submit(std::move(request));
    if(!m_local_scan_running) {
        m_local_scan_completed = true;
        const bool registering = m_download && m_download->phase == DownloadPhase::Registering;
        finish_registration(false);
        publish_file_actions();
        if (!registering) emit({{"type","error"},{"message","本地模型读取队列暂不可用，已有模型已保留，请重试。"}});
    }
}
void LibraryWorkspaceService::local_cards() {
    if(!m_local) return;
    m_seen_local_revision=m_local->revision();
    Json cards=Json::array();
    for(const auto& e:m_local->entries()) {
        cards.push_back({{"key",e.job_id},{"title",utf8(e.title)},{"details",utf8(e.details)},
            {"local",true},{"designOnly",e.design_only},{"canDirectImport",m_ports.native_import && e.can_direct_import},
            {"generatedAt",e.generated_at},
            {"printFeedback",e.print_feedback},{"imported",e.imported_at>0},
            {"modelId",e.website_model_id},{"versionId",e.website_version_id},{"sha256",e.website_sha256},
            {"sourceVerified",e.website_original_verified},{"extension",e.model_path.extension().string()}});
    }
    emit({{"type","local"},{"revision",m_local->revision()},{"items",cards}});
    publish_file_actions();
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
    const auto it=m_models.find(selection);
    if(it==m_models.end()) {emit({{"type","error"},{"message","这条模型记录已更新，请重新选择。"}});return;}
    m_comment_body.clear();m_comment_token.clear();
    m_detail_key=selection;m_detail=Json();m_detail_selection=it->second;invalidate();
    request_detail();
}
void LibraryWorkspaceService::request_detail() {
    emit({{"type","loading"},{"active",true},{"stage","正在读取模型详情…"}});
    if (m_account_refreshing) return;
    const auto c=client();const auto visitor=m_visitor;const auto item=m_detail_selection;
    if(!m_worker.submit(m_epoch,"detail",m_detail_key,[c,item,visitor](PL::Cancellation cancel) {
        return Value{c.presentation_detail(item.at("id").get<std::string>(),item.at("version").get<int>(),visitor,cancel),{}};
    },true)) emit({{"type","error"},{"message","模型详情队列暂不可用，请重试。"}});
}
void LibraryWorkspaceService::start_download(const Json& command) {
    const auto file = command.value("file", "");
    const auto request = command.value("requestGeneration", std::uint64_t(0));
    if (!request || !PL::uuid(file) || download_active() || !m_detail.is_object() || !m_local ||
        command.value("key", "") != m_detail_key) return;
    m_download = LibraryDownloadRequest{m_detail_key, file, request};
    for (const auto& item : m_detail.at("files")) {
        if (item.value("id", "") != file) continue;
        m_download->total = std::size_t(PL::DownloadSpec::listed_size(item));
        break;
    }
    if (!m_local_scan_completed) m_download->fail("正在读取本地下载记录，请稍候重试。");
    else if (registered_file(file)) {
        m_download->phase = DownloadPhase::Complete;
        m_download->done = m_download->total;
    } else if (m_account_refreshing) m_download->phase = DownloadPhase::WaitingForAccount;
    else if (!m_access_lifetime) m_download->fail("登录后可下载模型文件。", true);
    else {queue_download();return;}
    publish_download();
}
void LibraryWorkspaceService::queue_download()
{
    const auto c = client();
    const auto model = m_detail.at("id").get<std::string>();
    const auto version = m_detail.at("version").at("version").get<int>();
    const auto version_id = m_detail.at("version").at("id").get<std::string>();
    const auto root = m_local->root();
    const auto file = m_download->file;
    const auto request = std::to_string(m_download->request);
    const auto epoch = m_epoch;
    // Re-read the selected immutable version with the current account before requesting bytes.
    if (!m_worker.submit(epoch,"download",request,[this,c,model,version,version_id,file,root,request,epoch](PL::Cancellation cancel) {
        const auto spec = PL::DownloadSpec::from_detail(c.detail(model,version,cancel),file);
        if (spec.model != model || spec.version != version_id || spec.version_number != version)
            throw PL::Error(0, "平台返回了不同的模型版本，请重新读取详情。");
        const auto progress = [this,epoch,request,cancel](PL::StagedDownload::Phase phase, std::size_t done, std::size_t total) {
            m_worker.publish(epoch,"download",request,Value{{
                {"verify",phase == PL::StagedDownload::Phase::Verify},{"done",done},{"total",total}},{}},cancel);
        };
        auto staged = PL::StagedDownload::prepare(c,spec,root,cancel,progress);
        return Value{{{"total",spec.size}},std::move(staged)};
    },true))
        m_download->fail("下载队列暂不可用，请重试。");
    publish_download();
}
void LibraryWorkspaceService::publish_download()
{
    if (!m_download || m_download->key != m_detail_key) return;
    auto& download = *m_download;
    Json value = {{"type","downloadState"},{"key",download.key},{"file",download.file},
        {"downloadRequest",download.request},{"phase",phase_name(download.phase)},
        {"downloadUpdate",++download.update},
        {"done",download.done},{"total",download.total},{"message",download.message},
        {"loginRequired",download.login_required}};
    if (download.phase == DownloadPhase::Complete) {
        const auto* entry = registered_file(download.file);
        if (!entry) return;
        value["actions"] = file_actions(*entry);
    }
    emit(std::move(value));
}
void LibraryWorkspaceService::download_result(PL::Worker::Result& result)
{
    if (!m_download || !m_download->in_worker() || result.key != std::to_string(m_download->request)) return;
    auto& download = *m_download;
    if (!result.error.empty()) {
        if (result.status == 401 && !download.retried) {
            auto& session = m_account->session();
            if (session.refresh_access(session.generation())) {account_changed();return;}
        }
        download.fail(result.error, result.status == 401);
        publish_download();
        return;
    }
    if (!result.value.download) {
        download.done = result.value.json.at("done").get<std::size_t>();
        download.total = result.value.json.at("total").get<std::size_t>();
        download.phase = result.value.json.at("verify").get<bool>() ? DownloadPhase::Verify : DownloadPhase::Transfer;
        publish_download();
        return;
    }
    // The current epoch and lease have already been checked on the GUI thread.
    download.phase = DownloadPhase::Registering;
    download.done = download.total = result.value.json.at("total").get<std::size_t>();
    publish_download();
    try {
        result.value.download->commit(std::make_shared<std::atomic_bool>(false));
        download.registration_scan = m_scan + 1;
        refresh_local();
    } catch (const std::exception&) {
        download.fail("本地登记失败，请检查存储目录后重试。");
        publish_download();
    }
}
const ModelLibraryEntry* LibraryWorkspaceService::registered_file(const std::string& file) const
{
    if (!m_local || !PL::uuid(file) || !m_detail.is_object()) return nullptr;
    const auto model = m_detail.value("id", "");
    const auto version = m_detail.at("version").value("id", "");
    for (const auto& candidate : m_detail.at("files")) {
        if (candidate.value("id", "") != file) continue;
        const auto digest = candidate.find("sha256");
        if (digest == candidate.end() || !digest->is_string() || digest->get_ref<const std::string&>().size() != 64)
            return nullptr;
        const auto& entries = m_local->entries();
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const ModelLibraryEntry& local) {
            return local.website_original_verified && local.website_model_id == model && local.website_version_id == version &&
                local.website_file_id == file && local.website_sha256 == digest->get_ref<const std::string&>();
        });
        if (entry == entries.end()) return nullptr;
        boost::system::error_code error;
        return boost::filesystem::is_regular_file(entry->model_path, error) && !error ? &*entry : nullptr;
    }
    return nullptr;
}

void LibraryWorkspaceService::publish_file_actions()
{
    if (!m_detail.is_object() || !m_local_scan_completed || !m_local) return;
    Json files = Json::object();
    Json missing = Json::array();
    for (const auto& item : m_detail.at("files")) {
        const auto file = item.value("id", "");
        if (download_active() && file == m_download->file) continue;
        if (const auto* entry = registered_file(file)) {
            files[file] = file_actions(*entry);
            continue;
        }
        const auto digest = item.find("sha256");
        if (digest == item.end() || !digest->is_string()) continue;
        try {
            if (PL::DownloadSpec::from_detail(m_detail, file).registered_original_missing(m_local->root()))
                missing.push_back(file);
        } catch (const PL::Error&) {
            // Some published files do not have download access or a usable format.
        }
    }
    emit({{"type","fileActions"},{"key",m_detail_key},{"files",std::move(files)},{"missing",std::move(missing)}});
}
LibraryWorkspaceService::Json LibraryWorkspaceService::file_actions(const ModelLibraryEntry& entry) const
{
    const bool workbench = AI::is_model_artifact(entry.model_path);
    return {{"open", workbench}, {"import", workbench || (m_ports.native_import && entry.can_direct_import)}};
}

void LibraryWorkspaceService::finish_registration(bool scanned)
{
    if (!m_download || m_download->phase != DownloadPhase::Registering) return;
    if (scanned && registered_file(m_download->file)) m_download->phase = DownloadPhase::Complete;
    else m_download->fail("文件已保存，但本地登记尚未完成。请重试登记。");
    publish_download();
}

void LibraryWorkspaceService::detail_action(const Json& command)
{
    const auto action = command.value("action", "");
    if (!one_of(action, {"open", "import"})) return;
    const auto* entry = registered_file(command.value("file", ""));
    if (!entry) {
        publish_file_actions();
        refresh_local();
        emit({{"type","error"},{"message","本地模型文件不可用，请重新读取或下载。"},{"downloadError",true}});
        return;
    }
    const bool workbench = AI::is_model_artifact(entry->model_path);
    if (workbench || (action == "import" && entry->can_direct_import && m_ports.native_import))
        local_action({{"key",entry->job_id},{"revision",m_local->revision()},{"action",action}});
}

void LibraryWorkspaceService::local_action(const Json& command) {
    if(!m_local) return;
    const auto id=command.value("key","");const auto revision=command.value("revision",std::uint64_t(0));
    const ModelLibraryActionToken token{revision,id};const auto* entry=m_local->find(token);
    if(!entry) {local_cards();emit({{"type","error"},{"message","本地模型记录已更新，请重试。"}});return;}
    const auto action=command.value("action","");auto copy=*entry;
    const bool native_model = !copy.design_only && AI::model_artifact_format(copy.model_path).empty();
    if((action=="open" || action=="import") && native_model) {
        if (!copy.can_direct_import || !m_ports.native_import)
            emit({{"type","error"},{"message","当前模型暂不能导入切片。"}});
        else {
            m_native_import = token;
            emit({{"type","loading"},{"active",true},{"stage","正在导入切片…"},{"importPercent",0},{"cancelable",true}});
        }
        return;
    }
    static const std::map<std::string,ModelLibraryCommand> actions={
        {"open",ModelLibraryCommand::OpenModel},{"image",ModelLibraryCommand::OpenImage},
        {"import",ModelLibraryCommand::Import},{"retexture",ModelLibraryCommand::Retexture},
        {"remove",ModelLibraryCommand::Remove},{"colors",ModelLibraryCommand::MatchColors},
        {"export",ModelLibraryCommand::Export},{"print",ModelLibraryCommand::RecordPrintResult}};
    const auto found=actions.find(action);if(found==actions.end()) return;
    if(native_model && (action=="colors" || action=="retexture")) return;
    if(!m_local->execute(copy.design_only && action=="open"?ModelLibraryCommand::OpenImage:found->second,token))
        emit({{"type","error"},{"message","当前模型正在处理或此操作暂不可用"}});
    if(found->second==ModelLibraryCommand::Remove) local_cards();else refresh_local();
}

void LibraryWorkspaceService::run_native_import()
{
    const auto token = *m_native_import;
    m_native_import.reset();
    m_import_running = true;
    m_import_cancelled = m_import_publishing = false;
    bool imported = false;
    try {
        if (m_local->can_replace()) {
            if (const auto* entry = m_local->find(token)) {
                const auto path = entry->model_path;
                int last_percent = 0;
                imported = m_ports.native_import(path, [&](int percent) {
                    if (m_import_cancelled || !m_visible) return false;
                    if (percent > last_percent) {
                        last_percent = percent;
                        m_import_publishing = percent >= 99;
                        emit({{"type","loading"},{"active",true},{"importPercent",percent},
                            {"stage",m_import_publishing ? "正在放置到打印盘…" : "正在导入切片…"},
                            {"cancelable",!m_import_publishing}});
                    }
                    return true;
                });
            }
        }
    } catch (const std::exception&) {
        // The loader reports its detailed diagnostic. Release the page so the action can be retried.
    }
    m_import_running = m_import_publishing = false;
    emit({{"type","loading"},{"active",false}});
    if (!imported) {
        if (m_import_cancelled) emit({{"type","toast"},{"message","已取消导入"}});
        else emit({{"type","error"},{"message","导入未完成，可重试。"}});
    }
}

void LibraryWorkspaceService::request_community(const std::string& kind, std::size_t offset, bool recovery)
{
    if (!m_detail.is_object() || !m_detail.value("publicInteraction", false)) return;
    const auto model = m_detail.value("id", "");
    const auto author = m_detail.at("author").value("id", "");
    std::string path;
    if (kind == "comments" && PL::uuid(model) && offset <= 100000)
        path = "/v1/models/" + model + "/comments?offset=" + std::to_string(offset);
    else if (kind == "recommendations" && PL::uuid(model))
        path = "/v1/library/models/" + model + "/recommendations";
    else if (kind == "author" && PL::uuid(author))
        path = "/v1/library/authors/" + author;
    if (path.empty()) return;
    const auto c = client();
    if (!m_worker.submit(m_epoch, kind, m_detail_key, [c, path, kind, offset, recovery](PL::Cancellation cancel) {
        auto value = c.query(path, cancel);
        if (kind == "comments") value = Json{{"items", std::move(value)}, {"offset", offset}, {"recovery", recovery}};
        if (kind == "recommendations") value["recovery"] = recovery;
        return Value{std::move(value), {}};
    })) emit({{"type","communityError"},{"kind",kind},{"key",m_detail_key},{"message","暂时无法加载，请重试。"}});
}

void LibraryWorkspaceService::change_community(const std::string& kind, const Json& command)
{
    if (!m_detail.is_object() || !m_detail.value("publicInteraction", false)) return;
    if (m_mutating) {
        emit({{"type","error"},{"message","正在保存，请稍候重试。"}});
        return;
    }
    const auto c = client();
    std::string path;
    Json payload;
    if (kind == "follow") {
        const auto author = m_detail.at("author").value("id", "");
        if (!PL::uuid(author) || author == m_subject) return;
        path = "/v1/library/authors/" + author + "/follow";
        payload = {{"active", command.value("active", true)}};
    } else {
        const auto model = m_detail.value("id", "");
        const auto body = wxString::FromUTF8(command.value("comment", "")).Trim().Trim(false);
        if (!PL::uuid(model) || body.empty() || body.length() > 2000) return;
        const auto comment = utf8(body);
        if (m_comment_body != comment) {m_comment_body = comment; m_comment_token = HonorAccount::random_value();}
        path = "/v1/models/" + model + "/comments";
        payload = {{"body",comment},{"idempotencyKey",m_comment_token}};
    }
    if (!require_login()) return;
    m_mutating = m_worker.submit(m_epoch, kind, m_detail_key, [c, path, payload, kind](PL::Cancellation cancel) {
        auto result = c.query(path, cancel, "POST", payload);
        if (kind == "follow") result["following"] = payload.at("active");
        return Value{std::move(result), {}};
    }, true);
    if (!m_mutating) emit({{"type","error"},{"message","操作队列暂不可用，请重试。"}});
}

void LibraryWorkspaceService::request_geometry(bool interactive)
{
    if (!m_detail.is_object() || m_geometry_pending || m_geometry) return;
    if (!m_account->session().access()) {
        if (m_account_refreshing) {m_resume_geometry = true;return;}
        if (interactive) require_login();
        return;
    }
    if (!m_detail.value("access", Json::object()).value("allowed", false)) {
        if (interactive) emit({{"type","error"},{"message","当前账号没有交互预览资格"}});
        return;
    }
    const auto c = client();
    const auto path = "/v1/library/models/" + m_detail.at("id").get<std::string>() +
        "/geometry?version=" + std::to_string(m_detail.at("version").at("version").get<int>());
    m_geometry_pending = m_worker.submit(m_epoch,"geometry",m_detail_key,[c,path](PL::Cancellation cancel) {
        auto response=c.bytes(path,cancel,67108864);
        return Value{Json(),{},std::make_shared<const std::string>(std::move(response.bytes))};
    }, interactive);
    if (!m_geometry_pending && interactive)
        emit({{"type","error"},{"message","暂时无法读取三维预览，请重试。"}});
}

void LibraryWorkspaceService::request_detail_content(bool recovery)
{
    const auto c = client();
    auto media = m_detail.value("media", Json::array());
    media.insert(media.begin(), Json{{"url",m_detail.value("previewUrl","")}});
    for (const auto& image : media) {
        const auto path = image.value("url", "");
        if (PL::preview_path(path) && m_image_requests.insert(path).second)
            m_worker.submit(m_epoch,"detailImage",m_detail_key,[c,path](PL::Cancellation cancel){return Value{c.image(path,cancel),{}};});
    }
    request_community("comments",0,recovery);
    request_community("author");
    request_community("recommendations",0,recovery);
}

void LibraryWorkspaceService::command(const Json& message) {
    if(!m_visible || !message.is_object()) return;
    try {
        account_changed();
        if (message.value("accountGeneration",std::uint64_t(0)) != m_account_generation) return;
        const auto cmd=message.value("command","");
        if (m_native_import || m_import_running) {
            if (cmd == "cancel" && !m_import_publishing) {
                m_import_cancelled = true;
                if (m_native_import) {
                    m_native_import.reset();
                    emit({{"type","loading"},{"active",false}});
                    emit({{"type","toast"},{"message","已取消导入"}});
                }
            }
            return;
        }
        if(cmd=="query") {
            const auto scope=message.value("scope","library"),filter=message.value("filter","all");
            if(!one_of(scope,{"library","mine"}) || !one_of(filter,{"all","models","likes","favorites","downloads","prints","views"})) return;
            if(m_scope!=scope && scope=="mine") m_personal=Json::object();
            m_scope=scope;m_filter=filter;m_query=message.value("query",Json::object());m_detail_key.clear();m_detail=Json();m_detail_selection=Json();m_resume_detail=false;list();
        } else if(cmd=="detail") show_detail(message.value("key",""));
        else if(cmd=="back") {
            m_detail_key.clear();m_detail=Json();
            m_detail_selection=Json();m_resume_detail=false;invalidate();local_cards();
            Json cards=Json::array();for(const auto& item:m_models) cards.push_back(item.second);images(cards);
        }
        else if(cmd=="login") {if (!m_account_refreshing && m_ports.login) m_ports.login();}
        else if(cmd=="refresh") {m_personal=Json::object();refresh_local();if (m_detail_key.empty()) list();}
        else if(cmd=="local") local_action(message);
        else if(cmd=="fileAction" && !download_active()) detail_action(message);
        else if(cmd=="upload") {
            if (m_ports.upload && m_ports.upload()) m_upload_pending = true;
            else emit({{"type","error"},{"message","无法打开模型上传页，请重试。"}});
        } else if(cmd=="share" && m_detail.is_object() && m_detail.value("publicInteraction",false)) {
            const auto model=m_detail.value("id","");
            if(PL::uuid(model) && m_ports.share && m_ports.share(model))
                emit({{"type","toast"},{"message","已复制模型链接"}});
            else emit({{"type","error"},{"message","复制链接失败，请重试。"}});
        } else if(cmd=="comments" || cmd=="recommendations" || cmd=="author")
            request_community(cmd,message.value("query",Json::object()).value("offset",std::size_t(0)));
        else if(cmd=="follow" || cmd=="postComment") change_community(cmd,message);
        else if(cmd=="chooseLocal") {if(m_local) m_local->choose_local();refresh_local();}
        else if(cmd=="storage") {if(m_ports.storage) m_ports.storage();}
        else if(cmd=="saveProject") {if(m_project && m_ports.save_project) m_ports.save_project();}
        else if(cmd=="cancelDownload" && m_download &&
                message.value("key","") == m_download->key && message.value("file","") == m_download->file &&
                message.value("downloadRequest",std::uint64_t(0)) == m_download->request) {
            if (m_download->cancelable()) {
                m_worker.cancel("download",std::to_string(m_download->request));
                m_download->phase = DownloadPhase::Cancelled;
            }
            publish_download();
        }
        else if(cmd=="download") start_download(message);
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
        } else if(cmd=="geometry") request_geometry(true);
    } catch(const PL::Error& e) {emit({{"type","error"},{"message",e.what()}});}
      catch(...) {emit({{"type","error"},{"message","模型库操作无效"}});}
}
void LibraryWorkspaceService::poll() {
    if (m_import_running) return;
    try {
    account_changed();
    if (m_native_import) {run_native_import();return;}
    if(m_local && m_seen_removal_revision!=m_local->removal_revision()) {
        m_seen_removal_revision=m_local->removal_revision();++m_scan;m_loader->cancel();
        m_local_scan_running=m_local_rescan_pending=false;
        if(m_visible && !m_local->removal_active()) refresh_local();
    }
    if(m_visible && m_local && m_seen_local_revision!=m_local->revision()) {local_cards();refresh_local();}
    std::optional<ModelLibraryLoader::Snapshot> snapshot;std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
    m_loader->take(snapshot,thumbnails,4);
    if(snapshot && snapshot->revision==m_scan) {
        m_local_scan_running=false;
        m_local_scan_completed=true;
    }
    if(m_visible && snapshot && snapshot->revision==m_scan && snapshot->failed) {
        const bool registering = m_download && m_download->phase == DownloadPhase::Registering;
        if (registering && snapshot->revision >= m_download->registration_scan) finish_registration(false);
        publish_file_actions();
        if (!registering) emit({{"type","error"},{"message","本地模型读取失败，已有模型与原文件已保留，请刷新重试。"}});
    }
    if(m_visible && snapshot && snapshot->revision==m_scan && !snapshot->failed && m_local) {
        m_local->replace(std::move(snapshot->entries),m_scan_removal_revision,m_scan_commit_revision);
        if (m_download && m_download->phase == DownloadPhase::Registering &&
            snapshot->revision >= m_download->registration_scan &&
            m_scan_removal_revision == m_local->removal_revision() && !m_local->removal_active())
            finish_registration(true);
        local_cards();
        auto request=std::make_shared<ModelLibraryLoader::Request>();request->revision=m_scan;request->root=m_local->root();
        request->entries=m_local->entries();request->edge=420;
        m_thumbnail_local_revision=m_local->revision();m_loader->submit(std::move(request));
    }
    for(const auto& thumbnail:thumbnails) if(m_visible && thumbnail.revision==m_scan && m_local &&
        m_thumbnail_local_revision==m_local->revision() && thumbnail.index<m_local->entries().size()) {
        emit({{"type","image"},{"key",m_local->entries()[thumbnail.index].job_id},{"data",thumbnail_data(thumbnail)}});
    }
    if(m_visible && !m_local_scan_running && m_local_rescan_pending) {m_local_rescan_pending=false;refresh_local();}
    for(auto& result:m_worker.take()) {
        if(result.epoch!=m_epoch || !m_visible) continue;
        if (result.kind == "download") {download_result(result);continue;}
        if(!result.error.empty()) {
            if(result.kind=="image" || result.kind=="detailImage" || result.kind=="taxonomy") continue;
            if(one_of(result.kind,{"comments","recommendations","author"})) {
                emit({{"type","communityError"},{"kind",result.kind},{"key",result.key},{"message",result.error}});
                continue;
            }
            if(one_of(result.kind,{"toggle","follow","postComment"})) m_mutating=false;
            if(result.kind=="geometry") m_geometry_pending=false;
            emit({{"type","loading"},{"active",false}});
            if(result.status==401 && m_access_lifetime) {
                auto& session = m_account->session();
                session.refresh_access(session.generation());
                account_changed();
            }
            emit({{"type","error"},{"message",result.error},{"loginRequired",result.status==401}});continue;
        }
        if(result.kind=="taxonomy") emit({{"type","taxonomy"},{"value",result.value.json}});
        else if(result.kind=="catalog") {
            if(result.value.json.contains("cache")) {m_personal=std::move(result.value.json["cache"]);result.value.json.erase("cache");}
            for(const auto& item:result.value.json.at("items")) m_models[key(item)]=item;
            auto payload=result.value.json;payload["type"]="catalog";payload["replace"]=m_query.value("offset","0")=="0";emit(payload);images(payload.at("items"));
            if(result.value.json.value("reauthRequired",false) && m_access_lifetime) {
                auto& session = m_account->session();
                session.refresh_access(session.generation());
                account_changed();
                if (result.epoch != m_epoch) continue;
            }
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
            publish_file_actions();
            request_detail_content();
            request_geometry(false);
        } else if(result.kind=="geometry") {
            if(result.key!=m_detail_key) continue;
            m_geometry_pending=false;
            m_geometry=std::move(result.value.geometry);m_geometry_offset=0;
            if(m_geometry) emit({{"type","geometryBegin"},{"key",m_detail_key},{"epoch",m_epoch},{"total",m_geometry->size()}});
        } else if(result.kind=="image" || result.kind=="detailImage")
            emit({{"type",result.kind},{"key",result.key},{"data",result.value.json}});
        else if(result.kind=="toggle") {m_mutating=false;if(result.key==m_detail_key) emit({{"type","statistics"},{"value",result.value.json}});}
        else if (one_of(result.kind,{"comments","recommendations","author","follow","postComment"})) {
            if (result.key != m_detail_key) continue;
            if (result.kind == "recommendations") {
                auto& cards = result.value.json.at("items");
                for (const auto& model : cards)
                    if (PL::uuid(model.value("id","")) && PL::uuid(model.value("versionId",""))) m_models[key(model)] = model;
                images(cards);
            }
            if (result.kind == "follow" || result.kind == "postComment") m_mutating = false;
            emit({{"type",result.kind},{"key",result.key},{"value",std::move(result.value.json)}});
            if (result.kind == "postComment") {
                m_comment_body.clear();m_comment_token.clear();
                request_community("comments");
            }
        }
    }
    geometry_chunks();
    } catch(const std::exception&) {
        invalidate_remote();
        if (download_active()) {
            m_download->fail("模型库响应无效，请重试下载。");
            publish_download();
        } else emit({{"type","error"},{"message","模型库响应无效，请刷新后重试"}});
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
