#pragma once

#include "BeautySurface.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace Slic3r::AI {

struct BeautyGroup {
    uint32_t id {0};
    std::string name;
    std::vector<size_t> faces;
    bool locked {false};
    bool preserve_color {true};
};

// Portable editing intent, separate from both texture pixels and printer
// colors. Face ordinals always refer to the exact geometry_id in this record.
struct BeautyDocument {
    std::string geometry_id;
    size_t face_count {0};
    uint32_t next_group_id {1};
    std::vector<uint32_t> face_patch;
    std::vector<BeautyGroup> groups;
    nlohmann::json edits = nlohmann::json::array();

    static std::vector<size_t> checked_faces(std::vector<size_t> faces,size_t count) {
        if(faces.empty())throw std::runtime_error("Select a region first.");
        std::sort(faces.begin(),faces.end());faces.erase(std::unique(faces.begin(),faces.end()),faces.end());
        if(faces.back()>=count)throw std::runtime_error("Region belongs to a different surface.");
        return faces;
    }
    uint32_t add_group(std::string name,std::vector<size_t> faces) {
        if(name.empty() || name.size()>240 || groups.size()>=128)throw std::runtime_error("Use a region name up to 240 bytes and at most 128 groups.");
        faces=checked_faces(std::move(faces),face_count);
        size_t count=faces.size();for(const auto& g:groups)count+=g.faces.size();
        if(count>4000000 || next_group_id>=1000000)throw std::runtime_error("The saved region limit has been reached.");
        for(const auto& g:groups)if(g.name==name)throw std::runtime_error("A region already uses this name.");
        const auto id=next_group_id++;groups.push_back({id,std::move(name),std::move(faces),false,true});return id;
    }
    BeautyGroup& group(uint32_t id) {
        auto found=std::find_if(groups.begin(),groups.end(),[id](const auto& g){return g.id==id;});
        if(found==groups.end())throw std::runtime_error("The selected region no longer exists.");return *found;
    }
    // Partition the selected group's exact face mask. Underlying patches and
    // geometry remain fixed, so even a partial-patch brush boundary survives.
    uint32_t split_group(uint32_t id,const std::vector<size_t>& selected,std::string name) {
        const auto cut=checked_faces(selected,face_count);
        auto original=group(id);
        if(original.locked)throw std::runtime_error("Unlock this region before changing its boundary.");
        std::vector<size_t> taken,left;
        std::set_intersection(original.faces.begin(),original.faces.end(),cut.begin(),cut.end(),std::back_inserter(taken));
        std::set_difference(original.faces.begin(),original.faces.end(),cut.begin(),cut.end(),std::back_inserter(left));
        if(taken.empty() || left.empty())throw std::runtime_error("Select part of this region to split it.");
        const auto created=add_group(std::move(name),std::move(taken));
        group(id).faces=std::move(left);group(created).preserve_color=original.preserve_color;return created;
    }
    void merge_selection(uint32_t id,const std::vector<size_t>& selected) {
        auto faces=checked_faces(selected,face_count);auto& g=group(id);
        if(g.locked)throw std::runtime_error("Unlock this region before changing its boundary.");
        faces.insert(faces.end(),g.faces.begin(),g.faces.end());faces=checked_faces(std::move(faces),face_count);
        size_t count=faces.size();for(const auto& other:groups)if(other.id!=id)count+=other.faces.size();
        if(count>4000000)throw std::runtime_error("The saved region limit has been reached.");g.faces=std::move(faces);
    }
    std::vector<uint8_t> protection() const {
        std::vector<uint8_t> mask(face_count,0);
        for(const auto& g:groups)if(g.locked)for(size_t f:g.faces)mask[f]=1;
        return mask;
    }
    nlohmann::json encode() const {
        using Json=nlohmann::json;
        Json runs=Json::array();
        for(size_t start=0;start<face_patch.size();) {
            size_t end=start+1;while(end<face_patch.size() && face_patch[end]==face_patch[start])++end;
            runs.push_back({face_patch[start],end-start});start=end;
        }
        Json saved_groups=Json::array();
        for(const auto& g:groups)saved_groups.push_back({{"id",g.id},{"name",g.name},{"faces",g.faces},
            {"locked",g.locked},{"preserve_color",g.preserve_color}});
        return {{"schema","orca.beauty-workbench/v1"},{"geometry_id",geometry_id},{"face_count",face_count},
            {"patch_algorithm",BeautySurface::algorithm_version},{"patch_runs",std::move(runs)},
            {"next_group_id",next_group_id},{"groups",std::move(saved_groups)},{"edits",edits}};
    }
    static BeautyDocument decode(const nlohmann::json& json,const std::string& geometry,size_t faces) {
        auto require=[](bool v,const char* m){if(!v)throw std::runtime_error(m);};
        auto integer=[&](const nlohmann::json& v,size_t limit) {
            require(v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>()>=0),"Invalid beauty record integer.");
            const uint64_t value=v.get<uint64_t>();require(value<=limit,"Beauty record integer exceeds limit.");return size_t(value);
        };
        require(json.is_object() && json.value("schema","")=="orca.beauty-workbench/v1","Unsupported beauty record.");
        require(json.at("geometry_id")==geometry && integer(json.at("face_count"),2000000)==faces,"Beauty record belongs to different geometry.");
        require(integer(json.at("patch_algorithm"),100)==BeautySurface::algorithm_version,"Unsupported beauty patch version.");
        BeautyDocument result;result.geometry_id=geometry;result.face_count=faces;
        const auto& runs=json.at("patch_runs");require(runs.is_array() && runs.size()<=faces,"Invalid patch cache.");
        for(const auto& run:runs) {
            require(run.is_array() && run.size()==2,"Invalid patch run.");
            const size_t id=integer(run[0],faces?faces-1:0),n=integer(run[1],faces);
            require(n>0 && n<=faces-result.face_patch.size(),"Patch run exceeds surface.");
            result.face_patch.insert(result.face_patch.end(),n,uint32_t(id));
        }
        require(result.face_patch.empty() || result.face_patch.size()==faces,"Incomplete patch cache.");
        const auto& groups=json.at("groups");require(groups.is_array() && groups.size()<=128,"Too many saved beauty regions.");
        size_t saved_faces=0;
        for(const auto& value:groups) {
            const auto id=uint32_t(integer(value.at("id"),1000000));
            const auto name=value.at("name").get<std::string>();
            require(id>0 && !name.empty() && name.size()<=240,"Invalid saved region.");
            for(const auto& other:result.groups)require(id!=other.id && name!=other.name,"Repeated region identity.");
            const auto& items=value.at("faces");require(items.is_array() && items.size()<=faces,"Region face list exceeds limit.");
            saved_faces+=items.size();require(saved_faces<=4000000,"Saved region masks exceed the memory limit.");
            std::vector<size_t> indices;indices.reserve(items.size());
            for(const auto& index:items)indices.push_back(integer(index,faces?faces-1:0));
            require(std::is_sorted(indices.begin(),indices.end()) && std::adjacent_find(indices.begin(),indices.end())==indices.end(),"Region faces must be sorted and unique.");
            result.groups.push_back({id,name,checked_faces(std::move(indices),faces),value.at("locked").get<bool>(),value.at("preserve_color").get<bool>()});
            result.next_group_id=std::max(result.next_group_id,id+1);
        }
        result.next_group_id=std::max(result.next_group_id,uint32_t(integer(json.at("next_group_id"),1000000)));
        const auto& edits=json.at("edits");require(edits.is_array() && edits.size()<=128,"Too many beauty operations in this asset.");
        result.edits=edits;return result;
    }
};

// Bounded group-edit history; patch arrays are immutable and intentionally not
// duplicated for each click. Model-version undo stays with the existing host.
class BeautyGroupHistory {
    struct State {std::vector<BeautyGroup> groups;uint32_t next_id;};
    std::vector<State> past,future;
    static State snapshot(const BeautyDocument& d){return {d.groups,d.next_group_id};}
    static void restore(BeautyDocument& d,const State& s){d.groups=s.groups;d.next_group_id=s.next_id;}
    void trim() {
        auto bytes=[](const State& s){size_t n=0;for(const auto& g:s.groups)n+=g.faces.size()*sizeof(size_t)+g.name.size();return n;};
        size_t size=0;for(const auto& s:past)size+=bytes(s);
        while(past.size()>1 && (past.size()>20 || size>64*1024*1024)) {size-=bytes(past.front());past.erase(past.begin());}
    }
public:
    void clear(){past.clear();future.clear();}
    void record(const BeautyDocument& before){past.push_back(snapshot(before));future.clear();trim();}
    bool undo(BeautyDocument& d){if(past.empty())return false;future.push_back(snapshot(d));restore(d,past.back());past.pop_back();return true;}
    bool redo(BeautyDocument& d){if(future.empty())return false;past.push_back(snapshot(d));restore(d,future.back());future.pop_back();return true;}
};
} // namespace Slic3r::AI
