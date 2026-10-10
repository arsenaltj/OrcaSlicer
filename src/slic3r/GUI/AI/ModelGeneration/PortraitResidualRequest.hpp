#pragma once

#include "PortraitShapeDetails.hpp"
#include "ModelPreviewPalette.hpp"
#include "PortraitResidualProposal.hpp"

namespace Slic3r::GUI::PortraitResidual {

inline nlohmann::json parent_units(const PortraitShapeDetails& details,
    const std::vector<std::array<float,3>>& palette, const std::vector<std::array<float,3>>& card,
    const std::vector<std::pair<size_t,std::array<float,3>>>& current,
    const std::set<size_t>& blocked, const std::vector<std::vector<uint32_t>>& adjacency) {
    using Json=nlohmann::json;
    if(adjacency.size()!=details.base_colors.size())
        throw std::invalid_argument("verified_parent_topology_unavailable");
    std::map<size_t,std::array<float,3>> colors(current.begin(),current.end());
    std::map<std::string,Json> groups;
    std::vector<const Json*> samples(details.base_colors.size(),nullptr);
    const auto nearest=[&](const std::array<float,3>& source,bool skin) {
        size_t selected=palette.size();float best=std::numeric_limits<float>::max();
        const auto reference=PreviewPalette::to_lab(skin && card.size()==6 ? card[0] : source);
        for(size_t i=0;i<palette.size();++i) {
            const auto candidate=PreviewPalette::to_lab(palette[i]);
            const bool red=candidate[1]>.045f && candidate[1]>candidate[2]*1.25f+.01f;
            if(skin && (red || candidate[0]<.55f)) continue;
            float score=0;for(size_t c=0;c<3;++c) score+=(candidate[c]-reference[c])*(candidate[c]-reference[c]);
            if(score<best) {best=score;selected=i;}
        }
        return selected;
    };
    for(const auto& sample:details.parent_samples) {
        const size_t face=sample.at(0).get<size_t>();
        if(face>=details.base_colors.size() || blocked.count(face)) continue;
        if(samples[face]) throw std::invalid_argument("duplicate_parent_observation");
        samples[face]=&sample;
        const auto subject=sample.at(1).get<std::string>(),label=sample.at(2).get<std::string>();
        const std::string parent=label=="face" || label=="neck" ? "skin" : label;
        const auto& rgb=details.base_colors[face];
        const auto slot=nearest({rgb[0],rgb[1],rgb[2]},parent=="skin");
        if(slot>=palette.size()) continue;
        const auto found=colors.find(face);
        if(found!=colors.end() && found->second==palette[slot]) continue;
        const std::string id=subject+":"+parent+":"+std::to_string(slot)+
            (sample.at(5).get<size_t>()<2 ? ":single" : ":multi")+
            (sample.at(3).get<double>()<.85 ? ":uncertain" : ":confident");
        auto& unit=groups[id];
        if(unit.is_null()) unit={{"unit_id",id},{"subject_id",subject},{"parent_region",parent},{"proposal",parent},
            {"face_ids",Json::array()},{"confidence",1.0},{"view_support",16},{"pixel_support",size_t(0)},
            {"evidence_source","verified-local-semantics"},{"target_rgb",palette[slot]},{"target_slot",slot}};
        unit["face_ids"].push_back(face);
        unit["confidence"]=std::min(unit["confidence"].get<double>(),sample.at(3).get<double>());
        unit["view_support"]=std::min(unit["view_support"].get<size_t>(),sample.at(5).get<size_t>());
        unit["pixel_support"]=unit["pixel_support"].get<size_t>()+sample.at(4).get<size_t>();
    }
    Json result=Json::array();
    for(const auto& group:groups) {
        const auto faces=group.second.at("face_ids").get<std::vector<size_t>>();
        std::set<size_t> remaining(faces.begin(),faces.end());
        while(!remaining.empty()) {
            const size_t seed=*remaining.begin();remaining.erase(seed);
            std::vector<size_t> component{seed};
            for(size_t cursor=0;cursor<component.size();++cursor)
                for(const auto neighbor:adjacency.at(component[cursor])) {
                    if(neighbor>=adjacency.size()) throw std::invalid_argument("parent_topology_out_of_range");
                    if(remaining.erase(neighbor)) component.push_back(neighbor);
                }
            std::sort(component.begin(),component.end());
            auto unit=group.second;
            unit["unit_id"]=group.first+":"+std::to_string(seed);
            unit["face_ids"]=component;unit["confidence"]=1.0;unit["view_support"]=16;unit["pixel_support"]=size_t(0);
            for(const auto face:component) {
                const auto& sample=*samples.at(face);
                unit["confidence"]=std::min(unit["confidence"].get<double>(),sample.at(3).get<double>());
                unit["view_support"]=std::min(unit["view_support"].get<size_t>(),sample.at(5).get<size_t>());
                unit["pixel_support"]=unit["pixel_support"].get<size_t>()+sample.at(4).get<size_t>();
            }
            result.push_back(std::move(unit));
        }
    }
    return result;
}

} // namespace Slic3r::GUI::PortraitResidual
