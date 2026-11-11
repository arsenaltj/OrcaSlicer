#include "LibraryClient.hpp"
#include "slic3r/Utils/HonorAccount/HonorOAuth.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <regex>
#include <ctime>
#include <cctype>

namespace Slic3r::PlatformLibrary {
namespace {
void check(Cancellation cancel) { if (cancel && *cancel) throw Error(0,"操作已取消"); }
std::string hex(const unsigned char* bytes, std::size_t count) {
    const char* digits="0123456789abcdef"; std::string out;
    for(std::size_t i=0;i<count;++i) { out+=digits[bytes[i]>>4]; out+=digits[bytes[i]&15]; } return out;
}
std::string base64(const std::string& bytes) {
    const char* chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; unsigned accumulator=0;int bits=-6;
    for(unsigned char c:bytes) { accumulator=(accumulator<<8)|c;bits+=8;
        while(bits>=0) {out+=chars[(accumulator>>bits)&63];bits-=6;} }
    if(bits>-6) out+=chars[((accumulator<<8)>>(bits+8))&63];
    while(out.size()%4) out+='=';return out;
}
}
std::string file_digest(const boost::filesystem::path& path, const std::atomic<bool>* cancel) {
    boost::filesystem::ifstream input(path,std::ios::binary);
    if(!input) throw Error(0,"无法读取下载文件");
    SHA256_CTX ctx; SHA256_Init(&ctx); std::array<char,65536> buffer;
    while(input) { if(cancel && *cancel) throw Error(0,"操作已取消"); input.read(buffer.data(),buffer.size());
        SHA256_Update(&ctx,buffer.data(),std::size_t(input.gcount())); }
    if(!input.eof()) throw Error(0,"无法完成文件校验");
    unsigned char bytes[SHA256_DIGEST_LENGTH]; SHA256_Final(bytes,&ctx); return hex(bytes,sizeof(bytes));
}
namespace {
void directory(const boost::filesystem::path& root,const char* name) {
    const auto path=root/name;boost::filesystem::create_directories(path);
    if(boost::filesystem::canonical(path)!=path) throw Error(0,"模型目录指向了其他位置");
}
struct Receiver {
    Cancellation cancel; Binary value; std::size_t limit, received {0};
    std::function<bool(const char*,std::size_t)> sink;
    std::function<void(std::size_t,std::size_t)> progress;
};
size_t receive(char* data,size_t size,size_t count,void* opaque) noexcept {
    auto& r=*static_cast<Receiver*>(opaque); const auto n=size*count;
    try { if((r.cancel && *r.cancel) || n>r.limit-r.received) return 0;
        if(r.sink) { if(!r.sink(data,n)) return 0; } else r.value.bytes.append(data,n);
        r.received+=n; return n;
    } catch(...) { return 0; }
}
int progress(void* opaque,curl_off_t total,curl_off_t done,curl_off_t,curl_off_t) noexcept {
    auto& r=*static_cast<Receiver*>(opaque);
    if(r.cancel && *r.cancel) return 1;
    try { if(r.progress) r.progress(std::size_t(std::max<curl_off_t>(0,done)),std::size_t(std::max<curl_off_t>(0,total))); }
    catch(...) { return 1; } return 0;
}
std::string filters(const Json& q) {
    if(!q.is_object()) throw Error(0,"查询条件无效");
    std::string result="?limit=24";
    for(const char* key:{"q","category","sort","offset"}) {
        const auto it=q.find(key); if(it==q.end()) continue;
        if(!it->is_string() || it->get_ref<const std::string&>().size()>160) throw Error(0,"查询条件无效");
        result+="&"+std::string(key)+"="+escaped(it->get<std::string>());
    } return result;
}
}
bool uuid(const std::string& s) {
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(s,pattern);
}
bool supported_format(const std::string& ext) {
    auto lower=ext;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
    return lower=="glb" || lower=="obj" || lower=="3mf" || lower=="stl" || lower=="step" || lower=="stp";
}
std::string escaped(const std::string& value) {
    std::string out; const char* h="0123456789ABCDEF";
    for(unsigned char c:value) if(std::isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out+=char(c);
        else {out+='%';out+=h[c>>4];out+=h[c&15];} return out;
}
bool preview_path(const std::string& value) {
    static const std::regex pattern(R"(^/v1/library/(media/[0-9a-fA-F-]{36}|models/[0-9a-fA-F-]{36}/preview\?version=[0-9]{1,8})$)");
    return std::regex_match(value,pattern);
}
Binary transfer(const Identity& identity,const std::string& path,const std::string& method,
    const std::string& json,Cancellation cancel,std::size_t limit,
    std::function<bool(const char*,std::size_t)> sink,std::function<void(std::size_t,std::size_t)> notify) {
    check(cancel); HonorAccount::Config config; config.api_url=identity.api;
    if(!config.validate().empty() || path.rfind("/v1/",0)!=0 || path.find("..")!=std::string::npos || path.find('#')!=std::string::npos)
        throw Error(0,"平台服务地址不可用");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if(!curl) throw Error(0,"无法初始化网络连接");
    Receiver r {cancel,{},limit,0,std::move(sink),std::move(notify)};
    const auto url=identity.api+path;
    curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L); curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl.get(),CURLOPT_SSL_OPTIONS,long(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS,CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,8L); curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,limit<=4*1024*1024?20L:180L);
    curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_LIMIT,64L); curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_TIME,20L);
    curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,receive); curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&r);
    curl_easy_setopt(curl.get(),CURLOPT_NOPROGRESS,0L); curl_easy_setopt(curl.get(),CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl.get(),CURLOPT_XFERINFODATA,&r);
    curl_slist* list=nullptr; list=curl_slist_append(list,"Content-Type: application/json");
    if(!identity.bearer.empty()) list=curl_slist_append(list,("Authorization: Bearer "+identity.bearer).c_str());
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(list,curl_slist_free_all);
    curl_easy_setopt(curl.get(),CURLOPT_HTTPHEADER,list);
    if(method!="GET") {curl_easy_setopt(curl.get(),CURLOPT_CUSTOMREQUEST,method.c_str());
        curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDS,json.data()); curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDSIZE,long(json.size()));}
    const auto code=curl_easy_perform(curl.get()); check(cancel);
    long status=0; curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    r.value.status=unsigned(status); char* mime=nullptr;curl_easy_getinfo(curl.get(),CURLINFO_CONTENT_TYPE,&mime);
    if(mime) r.value.mime=mime;
    if(code!=CURLE_OK) throw Error(unsigned(status),r.received>=limit?"文件超出允许大小":"网络中断或无法写入文件，请重试");
    if(status<200 || status>=300) throw Error(unsigned(status),status==401?"登录已失效，请重新登录":status==403?"当前账号没有此操作资格":status==404?"模型或文件已不可用":"平台暂时无法完成请求");
    return std::move(r.value);
}
Json Client::query(const std::string& path,Cancellation cancel,const std::string& method,const Json& body) const {
    check(cancel);
    const auto r=m_transfer(m_identity,path,method,body.is_null()?"":body.dump(),cancel,4*1024*1024,{},{});
    check(cancel);
    auto value=Json::parse(r.bytes,nullptr,false); if(value.is_discarded()) throw Error(0,"平台响应格式无效"); return value;
}
Json Client::list(const Json& q,Cancellation c) const {return query("/v1/library/models"+filters(q),c);}
Json Client::activity(const std::string& kind,const Json& q,Cancellation c) const {
    if(kind!="likes" && kind!="favorites" && kind!="downloads" && kind!="views") throw Error(0,"个人筛选无效");
    if(m_identity.bearer.empty()) throw Error(401,"请先登录");
    return query("/v1/library/me/"+kind+filters(q),c);
}
Json Client::detail(const std::string& id,int version,Cancellation c) const {
    if(!uuid(id) || version<1) throw Error(0,"模型版本无效");
    return query("/v1/library/models/"+id+"?version="+std::to_string(version),c);
}
Json Client::toggle(const std::string& id,const std::string& kind,bool active,Cancellation c) const {
    if(!uuid(id) || (kind!="like" && kind!="favorite")) throw Error(0,"操作无效");
    if(m_identity.bearer.empty()) throw Error(401,"请先登录");
    if(kind=="like") return query("/v1/models/"+id+"/like",c,active?"POST":"DELETE",Json::object());
    return query("/v1/library/models/"+id+"/favorite",c,"POST",{{"active",active}});
}
std::string Client::image(const std::string& path,Cancellation c) const {
    if(!preview_path(path)) throw Error(0,"预览资源无效");
    const auto r=bytes(path,c,20*1024*1024);
    std::string mime=r.mime.substr(0,r.mime.find(';'));
    if(mime!="image/png" && mime!="image/jpeg" && mime!="image/webp" && mime!="image/svg+xml") throw Error(0,"预览类型无效");
    // SVG is rendered as an image document under a restrictive local CSP, never as HTML.
    return "data:"+mime+";base64,"+base64(r.bytes);
}
DownloadSpec DownloadSpec::from_detail(const Json& detail,const std::string& file) {
    if(!detail.is_object() || !uuid(file) || !detail.contains("version") || !detail.value("access",Json::object()).value("allowed",false))
        throw Error(403,"当前账号没有下载资格");
    DownloadSpec s; s.model=detail.value("id",""); s.version=detail["version"].value("id","");
    s.version_number=detail["version"].value("version",0); s.file=file; s.title=detail.value("title","");
    for(const auto& f:detail.at("files")) if(f.value("id","")==file) {
        if(f.value("status","")!="ready") throw Error(0,"模型文件尚未处理完成");
        s.filename=f.value("filename","");s.sha256=f.value("sha256","");
        const auto size=f.find("size");
        if(size!=f.end()) {
            if(size->is_string() && std::regex_match(size->get<std::string>(),std::regex("^[0-9]{1,12}$"))) s.size=std::stoull(size->get<std::string>());
            else if(size->is_number_unsigned()) s.size=size->get<std::uint64_t>();
        } break;}
    s.validate();
    return s;
}
std::string DownloadSpec::extension() const {
    const auto dot=filename.find_last_of('.');auto ext=dot==std::string::npos?"":filename.substr(dot);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});return ext;
}
void DownloadSpec::validate() const {
    const auto ext=extension();
    if(!uuid(model) || !uuid(version) || !uuid(file) || version_number<1 || ext.empty() || !supported_format(ext.substr(1)) ||
       size==0 || size>104857600 || !std::regex_match(sha256,std::regex("^[0-9a-f]{64}$")))
        throw Error(0,"下载文件清单无效或格式暂不支持");
}
std::string DownloadSpec::key() const {return "web-"+model+"-"+version+"-"+file;}
StagedDownload::~StagedDownload() {if(!m_staging.empty()) {boost::system::error_code ec;boost::filesystem::remove_all(m_staging,ec);}}
std::shared_ptr<StagedDownload> StagedDownload::prepare(const Client& client,const DownloadSpec& spec,
    const boost::filesystem::path& root,Cancellation cancel,std::function<void(std::size_t,std::size_t)> notify,
    std::function<std::uintmax_t(const boost::filesystem::path&)> available_bytes) {
    check(cancel);spec.validate();auto staged=std::shared_ptr<StagedDownload>(new StagedDownload);
    boost::filesystem::create_directories(root);const auto canonical_root=boost::filesystem::canonical(root);
    staged->m_spec=spec;staged->m_root=canonical_root;staged->m_final=canonical_root/"website"/spec.key();
    directory(canonical_root,"website");directory(canonical_root,"downloads");
    const auto ext=spec.extension();
    const auto file=staged->m_final/("model"+ext);
    if(boost::filesystem::exists(staged->m_final)) {
        if(boost::filesystem::canonical(staged->m_final)!=staged->m_final || boost::filesystem::canonical(file)!=file ||
           !boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file)!=spec.size || file_digest(file,cancel.get())!=spec.sha256)
            throw Error(0,"已有下载内容变化，请保留该文件后重试");
        staged->m_reused=true;return staged;
    }
    const auto available=available_bytes?available_bytes(canonical_root):boost::filesystem::space(canonical_root).available;
    if(available<spec.size+1048576) throw Error(0,"磁盘空间不足");
    staged->m_staging=canonical_root/"website"/boost::filesystem::unique_path(".download-%%%%%%%%-%%%%%%%%");
    boost::filesystem::create_directory(staged->m_staging);
    boost::filesystem::ofstream out(staged->m_staging/("model"+ext),std::ios::binary);
    if(!out) throw Error(0,"无法创建下载临时文件");
    client.bytes("/v1/library/models/"+spec.model+"/files/"+spec.file+"/download?version="+std::to_string(spec.version_number),
        cancel,std::size_t(spec.size),[&](const char* b,std::size_t n) {out.write(b,n);return bool(out);},std::move(notify));
    out.close();if(!out) throw Error(0,"下载文件写入失败");check(cancel);
    const auto part=staged->m_staging/("model"+ext);
    if(boost::filesystem::file_size(part)!=spec.size || file_digest(part,cancel.get())!=spec.sha256) throw Error(0,"文件大小或 SHA-256 校验失败");
    return staged;
}
std::string StagedDownload::commit(Cancellation cancel) {
    check(cancel);if(m_committed) return m_spec.key();
    if(boost::filesystem::canonical(m_root)!=m_root) throw Error(0,"模型目录已经变化");
    directory(m_root,"website");directory(m_root,"downloads");
    const auto marker=m_root/"downloads"/("orcaslicer-ai-"+m_spec.key()+".json");
    const auto ext=m_spec.extension();
    if(boost::filesystem::exists(marker)) {
        boost::filesystem::ifstream input(marker);const auto record=Json::parse(input,nullptr,false);
        if(!m_reused || boost::filesystem::canonical(marker)!=marker || record.is_discarded() ||
           record.value("source","")!="website_download" || record.value("job_id","")!=m_spec.key() ||
           record.value("website_sha256","")!=m_spec.sha256 || record.value("website_model_id","")!=m_spec.model ||
           record.value("website_version_id","")!=m_spec.version || record.value("website_file_id","")!=m_spec.file ||
           record.value("model_path","")!=(m_final/("model"+ext)).lexically_relative(m_root).generic_string())
            throw Error(0,"已有登记与下载版本不一致，已保留原件");
        m_committed=true;return m_spec.key();
    }
    const auto pending=marker.parent_path()/boost::filesystem::unique_path(".register-%%%%%%%%.tmp");
    Json metadata={{"source","website_download"},{"job_id",m_spec.key()},{"prompt",m_spec.title},
        {"model_path",(m_final/("model"+ext)).lexically_relative(m_root).generic_string()},
        {"generated_at",std::time(nullptr)},{"website_model_id",m_spec.model},{"website_version_id",m_spec.version},
        {"website_file_id",m_spec.file},{"website_sha256",m_spec.sha256}};
    try {
        boost::filesystem::ofstream output(pending,std::ios::binary);output<<metadata.dump(2);output.close();
        if(!output) throw Error(0,"无法登记下载模型");check(cancel);
        if(!m_reused) {boost::filesystem::rename(m_staging,m_final);m_staging.clear();}
        boost::filesystem::rename(pending,marker);m_committed=true;return m_spec.key();
    } catch(...) {
        boost::system::error_code ec;boost::filesystem::remove(pending,ec);
        if(!m_reused && m_staging.empty()) boost::filesystem::remove_all(m_final,ec);throw;
    }
}
}
