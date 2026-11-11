#include "LibraryClient.hpp"
#include <wx/string.h>
#include <algorithm>
#include <map>

namespace Slic3r::PlatformLibrary {
namespace {
void check(const Cancellation& cancel) {if(cancel && *cancel) throw Error(0,"操作已取消");}
std::string text(const Json& item,const char* name) {
    const auto it=item.find(name);return it!=item.end() && it->is_string()?it->get<std::string>():"";
}
std::string key(const Json& item) {return text(item,"id")+":"+text(item,"versionId");}
wxString lower(const std::string& value) {return wxString::FromUTF8(value).Lower();}
Json owned_card(const Json& source,const Identity& identity) {
    const auto id=text(source,"id"),version_id=text(source,"version_id");
    if(!uuid(id) || !uuid(version_id) || !source.contains("version") || !source.at("version").is_number_integer())
        throw Error(0,"本人作品清单无效");
    const auto version=source.at("version").get<int>();if(version<1) throw Error(0,"本人作品版本无效");
    return {{"id",id},{"versionId",version_id},{"version",version},{"title",text(source,"title")},
        {"description",text(source,"description")},{"category",text(source,"category")},
        {"createdAt",text(source,"created_at")},{"visibility",text(source,"visibility")},
        {"status",text(source,"status")},{"withdrawn",source.contains("withdrawn_at") && !source.at("withdrawn_at").is_null()},
        {"owned",true},{"access",text(source,"access")},{"author",{{"id",identity.subject},{"displayName","我"}}},
        {"coverUrl","/v1/library/models/"+id+"/preview?version="+std::to_string(version)}};
}
Json page(Json items,const Json& filters) {
    const auto q=lower(text(filters,"q"));const auto category=text(filters,"category"),sort=text(filters,"sort");
    items.erase(std::remove_if(items.begin(),items.end(),[&](const Json& item) {
        return (!q.empty() && !lower(text(item,"title")+" "+text(item,"description")).Contains(q)) ||
            (!category.empty() && category!=text(item,"category"));
    }),items.end());
    const std::map<std::string,std::string> metrics={{"views","viewCount"},{"downloads","downloadCount"},
        {"likes","likeCount"},{"favorites","favoriteCount"}};
    const auto metric=metrics.find(sort);
    std::sort(items.begin(),items.end(),[&](const Json& a,const Json& b) {
        if(metric!=metrics.end()) {
            const auto value=[&](const Json& x){const auto it=x.find(metric->second);return it!=x.end() && it->is_number()?it->get<double>():-1.;};
            if(value(a)!=value(b)) return value(a)>value(b);
        } else if(sort=="title") {
            const auto aa=lower(text(a,"title")),bb=lower(text(b,"title"));if(aa!=bb) return aa<bb;
        } else if(text(a,"createdAt")!=text(b,"createdAt")) return text(a,"createdAt")>text(b,"createdAt");
        return key(a)<key(b);
    });
    const auto offset_text=text(filters,"offset");
    if(offset_text.size()>7 || offset_text.find_first_not_of("0123456789")!=std::string::npos) throw Error(0,"分页无效");
    const std::size_t offset=offset_text.empty()?0:std::stoul(offset_text);const auto total=items.size();
    Json selected=Json::array();for(auto i=std::min<std::size_t>(offset,total);i<std::min<std::size_t>(offset+24,total);++i) selected.push_back(std::move(items[i]));
    return {{"items",std::move(selected)},{"offset",offset},{"limit",24},{"total",total},{"hasMore",offset+24<total}};
}
}
Json Client::owned_models(Cancellation cancel) const {
    if(m_identity.bearer.empty()) throw Error(401,"请先登录");
    Json items=Json::array();std::map<std::string,bool> seen;
    for(std::size_t offset=0;;offset+=100) {
        check(cancel);if(offset>9999999) throw Error(0,"本人作品数量超出分页范围");
        const auto batch=query("/v1/me/models?offset="+std::to_string(offset),cancel);check(cancel);
        if(!batch.is_array() || batch.size()>100) throw Error(0,"本人作品分页无效");
        std::size_t added=0;
        for(const auto& source:batch) {
            auto card=owned_card(source,m_identity);
            if(seen.emplace(key(card),true).second) {items.push_back(std::move(card));++added;}
        }
        if(batch.size()<100) return items;
        if(!added) throw Error(0,"本人作品分页未推进，请刷新重试");
    }
}
Json Client::personal(const std::string& filter,const Json& filters,Cancellation cancel,Json cache) const {
    if(m_identity.bearer.empty()) return {{"items",Json::array()},{"hasMore",false},{"cache",Json::object()}};
    if(filter!="all" && filter!="models") throw Error(0,"个人筛选无效");
    if(!cache.is_object() || cache.value("account","")!=m_identity.subject ||
        cache.value("generation",std::uint64_t(0))!=m_identity.generation) cache=Json::object();
    cache["account"]=m_identity.subject;cache["generation"]=m_identity.generation;
    Json warnings=Json::array();bool reauth=false;
    const auto read=[&](const std::string& kind) {
        if(cache.contains(kind)) return;
        try {
            if(kind=="models") {cache[kind]=owned_models(cancel);return;}
            Json items=Json::array();std::size_t offset=0;
            for(;;) {
                check(cancel);const auto batch=activity(kind,{{"offset",std::to_string(offset)}},cancel);
                if(!batch.contains("items") || !batch.at("items").is_array()) throw Error(0,"个人记录分页无效");
                for(const auto& item:batch.at("items")) items.push_back(item);
                offset+=batch.at("items").size();
                if(offset>=batch.value("total",0u)) break;
                if(batch.at("items").empty() || offset>9999999) throw Error(0,"个人记录分页未推进");
            }
            cache[kind]=std::move(items);
        } catch(const Error& error) {check(cancel);warnings.push_back(kind);reauth=reauth || error.status==401;}
          catch(const std::exception&) {check(cancel);warnings.push_back(kind);}
    };
    read("models");if(filter=="all") for(const char* kind:{"likes","favorites","downloads","views"}) read(kind);
    check(cancel);std::map<std::string,Json> merged;
    const auto append=[&](const std::string& kind) {
        if(!cache.contains(kind)) return;
        for(const auto& item:cache.at(kind)) {
            const auto id=key(item);if(!uuid(text(item,"id")) || !uuid(text(item,"versionId"))) continue;
            const auto it=merged.find(id);
            if(it==merged.end()) merged[id]=item;
            else for(const char* field:{"viewCount","downloadCount","likeCount","favoriteCount"})
                if(item.contains(field) && item.at(field).is_number()) it->second[field]=item.at(field);
        }
    };
    append("models");if(filter=="all") for(const char* kind:{"likes","favorites","downloads","views"}) append(kind);
    Json items=Json::array();for(auto& pair:merged) items.push_back(std::move(pair.second));
    auto result=page(std::move(items),filters);result["cache"]=std::move(cache);result["unavailable"]=std::move(warnings);
    result["reauthRequired"]=reauth;return result;
}
Json Client::presentation_detail(const std::string& id,int version,const std::string& visitor,Cancellation cancel) const {
    auto result=detail(id,version,cancel);check(cancel);
    result["owned"]=!m_identity.subject.empty() && result.value("author",Json::object()).value("id","")==m_identity.subject;
    std::string status;
    for(const auto& v:result.value("versions",Json::array())) if(v.value("version",0)==version) status=v.value("status","");
    result["version"]["status"]=status;
    const bool public_model=result.value("visibility","")=="public" && !result.value("withdrawn",false) && status=="published";
    result["publicInteraction"]=public_model;result["statistics"]=Json::object();
    if(public_model) {
        try {query("/v1/library/models/"+id+"/view",cancel,"POST",{{"visitorId",visitor}});}catch(const Error&) {check(cancel);}
        try {
            auto statistics=query("/v1/library/models/"+id+"/statistics",cancel);
            if(statistics.is_object()) result["statistics"]=std::move(statistics);
        }catch(const Error&) {check(cancel);}
        if(!m_identity.bearer.empty()) try {
            const auto engagement=query("/v1/models/"+id+"/engagement",cancel);
            if(engagement.contains("liked")) result["statistics"]["liked"]=engagement.at("liked");
        }catch(const Error&) {check(cancel);}
    }
    check(cancel);return result;
}
}
