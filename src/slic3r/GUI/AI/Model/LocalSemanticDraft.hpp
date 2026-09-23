#pragma once

#include "LocalSemanticEvidence.hpp"

namespace Slic3r::GUI::LocalSemanticDraft {

// Evidence must already have passed the owned client's actual native/render
// proof and strict decode. This merge cannot establish proof from echoed IDs.
// It updates only automatic draft regions, never confirmation, painting,
// physical channels, overrides, contrast rules or version lineage.
inline bool merge_regions(const AI::LocalPrintColorResult& draft, const LocalSemanticEvidence::Evidence& evidence,
    std::vector<AI::PrintColorRegion>& destination, std::string& error)
{
    error.clear();
    try {
        auto require=[](bool condition,const char* reason) {
            if(!condition) throw std::invalid_argument(reason);
        };
        require(AI::is_lowercase_sha256(draft.source_sha256) && AI::is_lowercase_sha256(draft.geometry_id) &&
            draft.face_count>0 && draft.face_count<=LocalSemanticEvidence::max_faces &&
            evidence.identity.source_sha256==draft.source_sha256 && evidence.identity.geometry_id==draft.geometry_id &&
            evidence.identity.face_count==draft.face_count,"Semantic evidence does not match the draft surface.");
        require(evidence.subjects.size()<=LocalSemanticEvidence::max_subjects &&
            evidence.regions.size()<=LocalSemanticEvidence::max_regions && evidence.face_regions.size()==draft.face_count,
            "Invalid semantic evidence collections.");
        std::set<std::string> subjects;
        for(const auto& subject:evidence.subjects)
            require(LocalSemanticEvidence::detail::identifier(subject) && subjects.insert(subject).second,
                    "Invalid semantic subject identity.");
        std::set<std::string> draft_ids, referenced;
        std::vector<size_t> visited(draft.face_count,std::numeric_limits<size_t>::max());
        for(size_t i=0;i<draft.regions.size();++i) {
            const auto& region=draft.regions[i];
            require(!region.id.empty() && draft_ids.insert(region.id).second,"Invalid draft region identity.");
            for(size_t face:region.faces) {
                require(face<draft.face_count && visited[face]!=i,"Invalid draft region face.");
                visited[face]=i;
            }
        }
        for(const auto& contrast:draft.contrasts) {
            require(draft_ids.count(contrast.first_region)!=0 && draft_ids.count(contrast.second_region)!=0,
                    "A draft contrast refers to a missing region.");
            referenced.insert(contrast.first_region); referenced.insert(contrast.second_region);
        }
        std::vector<uint8_t> blocked(draft.face_count,0), overridden(draft.face_count,0), claimed(draft.face_count,0);
        for(const auto& edit:draft.user_overrides) {
            require(edit.first<draft.face_count && !overridden[edit.first] && AI::valid_print_rgb(edit.second),
                    "Invalid draft color override.");
            overridden[edit.first]=1; blocked[edit.first]=1;
        }
        std::vector<AI::PrintColorRegion> merged;
        std::set<std::string> retained_ids;
        for(const auto& region:draft.regions) {
            const bool frozen=region.user_protected || region.locked_physical_slot.has_value() || referenced.count(region.id)!=0;
            const bool replaceable=region.id.compare(0,14,"auto-semantic:")==0 && !frozen;
            if(!replaceable) {merged.push_back(region);retained_ids.insert(region.id);}
            if(frozen) for(size_t face:region.faces) blocked[face]=1;
        }
        std::set<std::string> evidence_ids;
        size_t known=0;
        for(size_t i=0;i<evidence.regions.size();++i) {
            const auto& region=evidence.regions[i];
            require(subjects.count(region.subject_id)!=0 && LocalSemanticEvidence::supported_label(region.label) &&
                region.id=="auto-semantic:"+region.subject_id+":"+region.label && evidence_ids.insert(region.id).second &&
                !region.user_protected && !region.locked_physical_slot.has_value() &&
                std::isfinite(region.confidence) && region.confidence>=0 && region.confidence<=1 && !region.faces.empty(),
                "Invalid automatic semantic region or authority.");
            size_t previous=0;bool first=true;
            for(size_t face:region.faces) {
                require(face<draft.face_count && (first || face>previous) && !claimed[face] &&
                    evidence.face_regions[face]==int32_t(i),"Invalid semantic face assignment.");
                first=false;previous=face;claimed[face]=1;++known;
            }
            // Validate even a skipped record; an ID collision is not permission
            // to accept malformed or authority-bearing incoming data.
            if(retained_ids.count(region.id)!=0) continue;
            auto value=region;
            value.faces.erase(std::remove_if(value.faces.begin(),value.faces.end(),
                [&](size_t face){return blocked[face]!=0;}),value.faces.end());
            if(!value.faces.empty()) merged.push_back(std::move(value));
        }
        for(size_t face=0;face<draft.face_count;++face)
            require(claimed[face] || evidence.face_regions[face]==-1,"Unknown semantic face has an owner.");
        require(evidence.known_faces==known && evidence.unknown_faces==draft.face_count-known,
                "Semantic evidence counts do not match its regions.");
        destination=std::move(merged);
        return true;
    } catch(const std::exception& exception) {
        error=exception.what();
        return false;
    }
}

} // namespace Slic3r::GUI::LocalSemanticDraft
