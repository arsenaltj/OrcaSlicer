#include "DiagnosticLogs.hpp"
#include "ZipArchive.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <nlohmann/json.hpp>
#include <wx/datetime.h>
#include <algorithm>
#include <array>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r::Diagnostics {
namespace {
using Json=nlohmann::json;
const std::string field=R"((?:hy3d[_-]?api|openai[_-]?pro[_-]?api|[A-Za-z0-9_.-]*(?:api[_-]?key|token|secret|password|passwd|pwd|authorization(?:[_-]?header)?|cookies?(?:[_-]?header)?|credentials?|private[_-]?key|session[_-]?proof|nonce)))";
bool sensitive(std::string key) {
    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
    static const std::regex pattern("^"+field+"$",std::regex::icase);return std::regex_match(key,pattern);
}
std::string redact_plain(std::string text) {
    static const std::regex quoted("(\""+field+"\"\\s*:\\s*)\"(?:\\\\.|[^\"\\\\])*\"",std::regex::icase);
    static const std::regex single("('"+field+"'\\s*:\\s*)'(?:\\\\.|[^'\\\\])*'",std::regex::icase);
    static const std::regex bare("([\"']"+field+"[\"']\\s*:\\s*)(?![\"'])[^{\\[\\s,}\\]]+",std::regex::icase);
    static const std::regex authorization(R"((authorization\s*[:=]\s*(?:(?:bearer|basic)\s+)?)[^\s,;}\]]+)",std::regex::icase);
    static const std::regex bearer(R"(((?:bearer|basic)\s+)[^\s,;"'}\]]+)",std::regex::icase);
    // Matching the known suffix also handles accessToken/provider_api_key without
    // unbounded backtracking across ordinary long diagnostic messages.
    static const std::regex assignment(R"(((?:hy3d[_-]?api|openai[_-]?pro[_-]?api|api[_-]?key|token|secret|password|passwd|pwd|authorization|cookies?|credentials?|private[_-]?key|session[_-]?proof|nonce)\s*[:=]\s*)[^\s,;}\]]+)",std::regex::icase);
    static const std::regex secret(R"(\bsk-[A-Za-z0-9_-]{8,}\b)");
    static const std::regex url(R"(https?://[^\s"'<>]+)",std::regex::icase);
    // Remove URL credentials and request parameters before other patterns can split them.
    std::string urls;std::size_t pos=0;
    for(std::sregex_iterator it(text.begin(),text.end(),url),end;it!=end;++it) {
        urls.append(text,pos,std::size_t(it->position())-pos);auto value=it->str();
        const auto scheme=value.find("://")+3,slash=value.find_first_of("/?#",scheme);
        const auto at=value.rfind('@',slash==std::string::npos?value.size():slash);
        if(at!=std::string::npos && at>=scheme) value.erase(scheme,at-scheme+1);
        const auto query=value.find_first_of("?#");if(query!=std::string::npos) value.erase(query);
        urls+=value;pos=std::size_t(it->position()+it->length());
    }
    urls+=text.substr(pos);text=std::move(urls);
    text=std::regex_replace(text,quoted,"$1\"<redacted>\"");
    text=std::regex_replace(text,single,"$1'<redacted>'");
    text=std::regex_replace(text,bare,"$1<redacted>");
    text=std::regex_replace(text,authorization,"$1<redacted>");
    text=std::regex_replace(text,bearer,"$1<redacted>");
    text=std::regex_replace(text,assignment,"$1<redacted>");
    return std::regex_replace(text,secret,"<redacted>");
}
void redact_json(Json& value) {
    if(value.is_object()) for(auto it=value.begin();it!=value.end();++it) {
        if(sensitive(it.key())) it.value()="<redacted>";else redact_json(it.value());
    } else if(value.is_array()) for(auto& item:value) redact_json(item);
    else if(value.is_string()) value=redact_plain(value.get<std::string>());
}
struct Cancelled {};
struct ReadError : std::runtime_error {using std::runtime_error::runtime_error;};
void check(const std::atomic_bool& cancel) {if(cancel) throw Cancelled{};}
struct Temporary {
    boost::filesystem::path path;
    ~Temporary(){boost::system::error_code error;if(!path.empty()) boost::filesystem::remove_all(path,error);}
};
bool is_log(const boost::filesystem::path& path) {
    auto name=path.filename().string();std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    static const std::regex allowed(R"(.*(?:\.log(?:\.[0-9]+)?|\.jsonl|\.ndjson|\.txt)$)");
    return std::regex_match(name,allowed);
}
wxString wx_path(const boost::filesystem::path& path) {
#ifdef _WIN32
    return wxString(path.wstring());
#else
    return wxString::FromUTF8(path.string());
#endif
}
std::string relative_name(const boost::filesystem::path& path) {
#ifdef _WIN32
    return std::string(wxString(path.generic_wstring()).utf8_string());
#else
    return path.generic_string();
#endif
}
void snapshot(const boost::filesystem::path& source,const boost::filesystem::path& dest,const std::atomic_bool& cancel) {
    boost::filesystem::ifstream input(source,std::ios::binary);if(!input) throw ReadError("无法读取／文件被占用");
    boost::system::error_code error;const auto size=boost::filesystem::file_size(source,error);
    if(error) throw ReadError("日志已轮转／无法读取大小");
    boost::filesystem::create_directories(dest.parent_path());boost::filesystem::ofstream output(dest,std::ios::binary|std::ios::trunc);
    if(!output) throw std::runtime_error("无法写入日志快照");
    std::array<char,65536> buffer;std::string pending;std::uintmax_t remaining=size;
    while(remaining) {
        check(cancel);const auto count=std::min<std::uintmax_t>(buffer.size(),remaining);input.read(buffer.data(),std::streamsize(count));
        if(std::uintmax_t(input.gcount())!=count) throw ReadError("日志已轮转／读取不完整");
        remaining-=count;pending.append(buffer.data(),std::size_t(count));std::size_t begin=0,end;
        while((end=pending.find('\n',begin))!=std::string::npos) {
            const auto clean=redact_log_text(pending.substr(begin,end-begin));output<<clean<<'\n';begin=end+1;
        }
        pending.erase(0,begin);if(!output) throw std::runtime_error("日志快照写入失败");
    }
    if(!pending.empty()) output<<redact_log_text(pending);
    output.close();if(!output) throw std::runtime_error("日志快照写入失败");
}
void publish(const boost::filesystem::path& source,const boost::filesystem::path& target) {
#ifdef _WIN32
    if(!::MoveFileExW(source.wstring().c_str(),target.wstring().c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("无法替换目标文件，请检查磁盘、权限或文件占用；原目标已保留");
#else
    if(boost::nowide::rename(source.string().c_str(),target.string().c_str())!=0)
        throw std::runtime_error("无法替换目标文件，请检查磁盘、权限或文件占用；原目标已保留");
#endif
}
}
std::string redact_log_text(const std::string& text) {
    // Locate complete JSON payloads after native [timestamp] prefixes, retaining
    // suffix text and handling multiple payloads without exposing credential objects.
    std::string result;std::size_t copied=0,search=0;
    while((search=text.find_first_of("{[",search))!=std::string::npos) {
        std::size_t end=search,depth=0;bool quoted=false,escaped=false;
        for(;end<text.size();++end) {
            const char c=text[end];
            if(quoted) {if(escaped) escaped=false;else if(c=='\\') escaped=true;else if(c=='"') quoted=false;continue;}
            if(c=='"') quoted=true;
            else if(c=='{' || c=='[') ++depth;
            else if(c=='}' || c==']') {if(--depth==0) {++end;break;}}
        }
        auto parsed=depth==0?Json::parse(text.substr(search,end-search),nullptr,false):Json(Json::value_t::discarded);
        if(!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
            result+=redact_plain(text.substr(copied,search-copied));redact_json(parsed);
            result+=parsed.dump(-1,' ',false,Json::error_handler_t::replace);copied=end;search=end;
        } else ++search;
    }
    return result+redact_plain(text.substr(copied));
}
LogExportResult export_logs(const boost::filesystem::path& data,const boost::filesystem::path& target,
    const std::string& version,const std::atomic_bool& cancel,std::function<void(const std::string&)> progress) {
    namespace fs=boost::filesystem;LogExportResult result;Temporary temporary;
    try {
        check(cancel);const auto root=data/"log";
        if(!fs::exists(root)) {result.empty=true;return result;}
        if(fs::is_symlink(root) || !fs::is_directory(root)) throw std::runtime_error("日志目录无效");
        const auto destination=fs::absolute(target);
        if(!fs::is_directory(destination.parent_path()) || fs::is_symlink(destination) || fs::is_directory(destination))
            throw std::runtime_error("导出目标不可写，原目标已保留");
        const auto staging=destination.parent_path()/fs::unique_path(".honor-logs-%%%%%%%%-%%%%%%%%");
        if(!fs::create_directory(staging)) throw std::runtime_error("无法创建导出快照");temporary.path=staging;
        Json files=Json::array(),omissions=Json::array();
        boost::system::error_code error;fs::recursive_directory_iterator it(root,error),end;
        if(error) throw std::runtime_error("无法读取日志目录");
        while(it!=end) {
            check(cancel);const auto source=it->path();
            const auto status=it->symlink_status(error);
            if(error) {result.omitted.push_back(relative_name(source.lexically_relative(root)));omissions.push_back({{"path",result.omitted.back()},{"reason","日志已轮转或状态不可读"}});}
            else if(fs::is_symlink(status)) it.disable_recursion_pending();
            else if(fs::is_regular_file(status) && is_log(source)) {
                const auto relative=source.lexically_relative(root);const auto name=relative_name(relative);
                const auto dest=temporary.path/"logs"/relative;
                if(progress) progress("正在整理并脱敏日志…（"+name+"）");
                try {
                    snapshot(source,dest,cancel);result.files.push_back(name);
                    files.push_back({{"path","logs/"+name},{"bytes",fs::file_size(dest)}});
                } catch(const Cancelled&) {throw;}
                  catch(const ReadError&) {
                    fs::remove(dest,error);result.omitted.push_back(name);omissions.push_back({{"path",name},{"reason","无法完整读取，可能已轮转或占用"}});
                }
            }
            it.increment(error);if(error) {result.omitted.push_back("部分日志目录");omissions.push_back({{"path","log/"},{"reason","部分目录无法读取"}});break;}
        }
        if(result.files.empty()) {result.empty=result.omitted.empty();if(!result.empty) result.error="日志均无法读取，请检查文件占用或磁盘权限";return result;}
        Json manifest={{"applicationVersion",version},{"exportedAt",wxDateTime::UNow().ToUTC().FormatISOCombined('T').ToStdString()+"Z"},
            {"files",std::move(files)},{"omitted",std::move(omissions)},{"scope","当前配置目录/log；不包含模型、历史、账号存储或完整配置"}};
        {fs::ofstream out(temporary.path/"manifest.json",std::ios::binary);out<<manifest.dump(2);out.close();if(!out) throw std::runtime_error("无法写入日志清单");}
        check(cancel);if(progress) progress("正在生成日志 ZIP…");
        const auto archive=temporary.path/"logs.zip";
        if(!save_zip_sources({wx_path(temporary.path/"logs"),wx_path(temporary.path/"manifest.json")},
            wx_path(archive),[&]{return cancel.load();})) {check(cancel);throw std::runtime_error("ZIP 写入失败，请检查磁盘空间和权限；原目标已保留");}
        check(cancel);if(progress) progress("正在完成日志导出…");check(cancel);
        publish(archive,destination);result.success=true;
    } catch(const Cancelled&) {result.cancelled=true;}
      catch(const std::exception&) {result.error="日志导出失败，请检查磁盘空间、写入权限或文件占用；原目标已保留";}
    return result;
}
}
