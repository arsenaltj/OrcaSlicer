#pragma once
#include "LocalSemanticEvidence.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace Slic3r::AI {
// Saved editing aids are bound to the original texture and geometry, not the
// current inference version. Updating the recognizer never overwrites an edit.
struct BeautyGuidance {
    std::vector<int32_t> labels;
    std::vector<std::vector<size_t>> details;
    std::vector<GUI::LocalSemanticEvidence::EyeDetail> eyes;
    std::vector<std::string> names;
    bool completed(size_t count) const { return labels.size()==count; }
    bool has_features() const { return std::any_of(labels.begin(),labels.end(),[](int32_t id){return id>=0;}); }
    std::vector<uint8_t> skin_faces() const {
        std::vector<uint8_t> mask(labels.size(),0);
        for(size_t f=0;f<labels.size();++f) if(labels[f]>=0 && size_t(labels[f])<names.size()) {
            const auto& n=names[size_t(labels[f])];
            mask[f]=n=="face" || n=="nose" || n=="neck" || n=="lr" || n=="rr";
        }
        return mask;
    }
    nlohmann::json encode(const std::string& geometry,const std::string& source_hash) const {
        nlohmann::json runs=nlohmann::json::array(),saved_eyes=nlohmann::json::array();
        for(size_t at=0;at<labels.size();) {
            size_t end=at+1;while(end<labels.size() && labels[end]==labels[at])++end;
            runs.push_back({labels[at],end-at});at=end;
        }
        for(const auto& eye:eyes)saved_eyes.push_back({{"subject",eye.subject_id},{"label",eye.label},
            {"aperture",eye.aperture_faces},{"iris",eye.iris_faces}});
        return {{"schema","orca.beauty-guidance/v2"},{"geometry",geometry},{"source",source_hash},
            {"labels",std::move(runs)},{"details",details},{"eyes",std::move(saved_eyes)},{"names",names}};
    }
    static BeautyGuidance decode(const nlohmann::json& json,const std::string& geometry,
                                 const std::string& source_hash,size_t count) {
        auto require=[](bool ok){if(!ok)throw std::runtime_error("Invalid saved recognition aids.");};
        const bool v2=json.value("schema","")=="orca.beauty-guidance/v2";
        require(count>0 && count<=2000000 && json.is_object() && json.size()==(v2?7:6) &&
            (v2 || json.at("schema")=="orca.beauty-guidance/v1") && json.at("geometry")==geometry && json.at("source")==source_hash);
        auto integer=[&](const nlohmann::json& v,size_t max) {
            require(v.is_number_integer() && (!v.is_number_unsigned()?v.get<int64_t>()>=0:true));
            const auto n=v.get<uint64_t>();require(n<=max);return size_t(n);
        };
        BeautyGuidance result;
        if(v2) {
            require(json.at("names").is_array() && json.at("names").size()<=10001);
            for(const auto& name:json.at("names")) {
                const auto n=name.get<std::string>();
                require(GUI::LocalSemanticEvidence::supported_label(n) || n=="iris");result.names.push_back(n);
            }
        }
        const auto& runs=json.at("labels");require(runs.is_array() && runs.size()<=count);
        for(const auto& run:runs) {
            require(run.is_array() && run.size()==2 && run[0].is_number_integer());
            const auto label=run[0].get<int64_t>();require(label>=-1 && label<=10000);
            const size_t n=integer(run[1],count-result.labels.size());require(n>0);
            result.labels.insert(result.labels.end(),n,int32_t(label));
        }
        require(result.labels.empty() || result.labels.size()==count);
        if(!result.names.empty())for(int32_t id:result.labels)require(id<0 || size_t(id)<result.names.size());
        size_t total=0;
        auto faces=[&](const nlohmann::json& array) {
            require(array.is_array() && array.size()<=count);total+=array.size();require(total<=count*4);
            std::vector<size_t> indices;indices.reserve(array.size());
            for(const auto& v:array)indices.push_back(integer(v,count-1));
            require(std::is_sorted(indices.begin(),indices.end()) && std::adjacent_find(indices.begin(),indices.end())==indices.end());return indices;
        };
        const auto& details=json.at("details");require(details.is_array() && details.size()<=128);
        for(const auto& detail:details)result.details.push_back(faces(detail));
        const auto& eyes=json.at("eyes");require(eyes.is_array() && eyes.size()<=128);
        for(const auto& value:eyes) {
            require(value.is_object() && value.size()==4);
            GUI::LocalSemanticEvidence::EyeDetail eye;
            eye.subject_id=value.at("subject").get<std::string>();eye.label=value.at("label").get<std::string>();
            require(eye.subject_id.size()<=128 && (eye.label=="le" || eye.label=="re"));
            eye.aperture_faces=faces(value.at("aperture"));eye.iris_faces=faces(value.at("iris"));
            require(std::includes(eye.aperture_faces.begin(),eye.aperture_faces.end(),eye.iris_faces.begin(),eye.iris_faces.end()));
            result.eyes.push_back(std::move(eye));
        }
        return result;
    }
};
}
