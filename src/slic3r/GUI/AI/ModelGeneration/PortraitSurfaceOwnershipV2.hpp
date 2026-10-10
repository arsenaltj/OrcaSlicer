#pragma once

#include "slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp"

namespace Slic3r::GUI {

// Offline cell attribution; the existing v1 leaf workbench remains independent.
struct PortraitSurfaceCellOwnership {
    using Json = nlohmann::json;
    Json document;

    static std::string root_id(const Json& partition, size_t face) {
        return AI::beauty_leaf_digest(Json{{"schema","orca.surface-root-cell/v1"},
            {"geometry_id",partition.at("geometry_id")},{"source_sha256",partition.at("source_sha256")},
            {"source_face_id",face}}.dump());
    }

    static bool safe_reference(const Json& ref) {
        const auto hash=ref.value("sha256",std::string());
        return AI::ShapeLockSet::sha256(hash) && ref.value("schema",std::string())==
            "orca.portrait-surface-ownership-reference/v2" && ref.value("path",std::string())==
            "portrait-ownership/"+hash+".json";
    }

    static PortraitSurfaceCellOwnership decode(const Json& value, const Json& partition,
                                               const Json& locks, const std::string& frozen_hash) {
        namespace SP=AI::SurfacePartition;
        SP::require(value.at("schema")=="orca.portrait-surface-ownership/v2", "Unsupported cell ownership.");
        for (const auto* field:{"geometry_id","source_sha256","face_count","evidence_sha256"})
            SP::require(value.at(field)==partition.at(field),"Cell ownership source identity drift.");
        SP::require(value.at("partition_sha256")==partition.at("partition_sha256") &&
            value.at("partition_ref")==locks.at("partition_ref") && value.at("detail_freeze_sha256")==frozen_hash &&
            SP::hash(frozen_hash) && value.at("policy_sha256")==AI::beauty_leaf_digest(value.at("policy").dump()),
            "Cell ownership boundary, frozen details or policy drift.");
        std::map<std::string,std::pair<size_t,Json>> cells;
        std::set<size_t> explicit_roots;
        for (const auto& face:partition.at("faces")) {
            const size_t root=face.at("source_face_id"); explicit_roots.insert(root);
            for (const auto& cell:face.at("cells")) cells.emplace(cell.at("id"),std::make_pair(root,cell));
        }
        std::set<std::string> frozen,subjects,claimed,regions;
        for (const auto& lock:locks.at("locks")) {
            subjects.insert(lock.at("subject_id").get<std::string>());
            for (const auto& id:lock.at("locked_cells")) frozen.insert(id.get<std::string>());
            for (const auto& id:lock.value("periocular_cells",Json::array())) frozen.insert(id.get<std::string>());
        }
        const size_t count=partition.at("face_count");
        for (const auto& region:value.at("regions")) {
            const auto id=region.at("id").get<std::string>(),subject=region.at("subject_id").get<std::string>();
            const auto parent=region.at("parent_label").get<std::string>();
            const auto views=region.at("view_ids").get<std::vector<std::string>>();
            SP::require(!id.empty() && regions.insert(id).second && subjects.count(subject) &&
                (parent=="skin" || parent=="hair" || parent=="cloth") &&
                (region.at("status")=="CONFIRMED_PARENT" || region.at("status")=="SUPPORTED_PARENT_PROPOSAL") &&
                views.size()>=2 && std::is_sorted(views.begin(),views.end()) &&
                std::adjacent_find(views.begin(),views.end())==views.end() &&
                std::find(views.begin(),views.end(),std::string())==views.end() && !region.at("units").empty(),
                "Invalid or correlated parent component.");
            for (const auto& unit:region.at("units")) {
                const auto key=unit.at("id").get<std::string>();
                SP::require(unit.at("source_face_id").is_number_unsigned() ||
                    (unit.at("source_face_id").is_number_integer() && unit.at("source_face_id").get<int64_t>()>=0),
                    "Invalid parent source face.");
                const size_t face=unit.at("source_face_id");
                SP::require(face<count && SP::hash(key) && claimed.insert(key).second && !frozen.count(key),
                    "Parent unit is duplicate, frozen or outside the source.");
                if (explicit_roots.count(face)) {
                    SP::require(cells.count(key) && cells.at(key).first==face,"Mixed-parent cell mapping drift.");
                    const auto& cell=cells.at(key).second;
                    const auto label=cell.at("label").get<std::string>();
                    SP::require(cell.at("subject_id")==subject &&
                        (label=="face" || label=="R6" || label=="skin" || label=="hair" || label=="cloth"),
                        "Parent ownership crosses a feature or subject.");
                } else SP::require(unit.value("implicit_root",false) && key==root_id(partition,face),
                    "Implicit parent source mapping drift.");
                if (unit.contains("view_ids")) {
                    const auto support=unit.at("view_ids").get<std::vector<std::string>>();
                    SP::require(support.size()>=2 && std::is_sorted(support.begin(),support.end()) &&
                        std::adjacent_find(support.begin(),support.end())==support.end(),"Single-view parent unit.");
                }
            }
        }
        return {value};
    }
};
} // namespace Slic3r::GUI
