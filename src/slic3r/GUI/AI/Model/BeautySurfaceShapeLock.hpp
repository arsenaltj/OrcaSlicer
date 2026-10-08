#pragma once

#include "BeautyShapeLock.hpp"
#include "SurfacePartition.hpp"

namespace Slic3r::AI {
// v3 retains its own polygon identity; legacy face/leaf locks remain readable.
struct BeautySurfaceShapeLock {
    nlohmann::json document;

    std::string fingerprint(const nlohmann::json& partition) const {
        auto boundary=document;boundary.erase("partition_ref");
        boundary["partition_sha256"]=partition.at("partition_sha256");
        return beauty_leaf_digest(boundary.dump());
    }

    static bool safe_partition_path(const std::string& path, const std::string& hash) {
        return SurfacePartition::hash(hash) && path == "surface-partitions/" + hash + ".json";
    }

    static nlohmann::json identity(const nlohmann::json& partition) {
        nlohmann::json result;
        for(const auto* field:{"geometry_id","source_sha256","evidence_sha256","runtime_sha256",
                              "policy_sha256","face_count","baseline_sha256","boundary_policy_sha256"})
            result[field]=partition.at(field);
        return result;
    }

    static BeautySurfaceShapeLock from_partition(const nlohmann::json& partition,
        const std::vector<GUI::LocalSemanticEvidence::ShapeDetail>& shapes,const std::string& file_hash) {
        auto value=identity(partition);
        value.update({{"schema","orca.beauty-shape-lock/v3"},{"partition_ref",{
            {"schema","orca.surface-partition-reference/v1"},{"path","surface-partitions/"+file_hash+".json"},
            {"sha256",file_hash}}},{"locks",nlohmann::json::array()}});
        for(const auto& shape:shapes) {
            if(!ShapeLockSet::lockable_shape(shape) || shape.view_support<2) continue;
            std::vector<std::string> locked,nested,accessory;
            for(const auto& face:partition.at("faces")) for(const auto& cell:face.at("cells")) {
                if(cell.at("subject_id")!=shape.subject_id) continue;
                const auto label=cell.at("label").get<std::string>(), id=cell.at("id").get<std::string>();
                if(label==shape.label || label=="iris-"+shape.label) locked.push_back(id);
                if(label=="iris-"+shape.label) nested.push_back(id);
                if(label=="periocular-"+shape.label) accessory.push_back(id);
            }
            if(locked.empty()) continue;
            for(auto* ids:{&locked,&nested,&accessory}) std::sort(ids->begin(),ids->end());
            value["locks"].push_back({{"subject_id",shape.subject_id},{"label",shape.label},{"parent_label",shape.label},
                {"status",shape.status},{"view_support",shape.view_support},{"reasons",shape.reasons},
                {"locked_cells",locked},{"nested_cells",nested},{"periocular_cells",accessory}});
        }
        return decode(value,partition,identity(partition),file_hash);
    }

    static BeautySurfaceShapeLock decode(const nlohmann::json& value,
                                         const nlohmann::json& partition,
                                         const nlohmann::json& identity,
                                         const std::string& partition_file_hash) {
        namespace SP = SurfacePartition;
        SP::require(value.at("schema") == "orca.beauty-shape-lock/v3", "Unsupported contour lock schema.");
        SP::validate(partition, identity);
        for (auto it = identity.begin(); it != identity.end(); ++it)
            SP::require(value.at(it.key()) == it.value(), "Contour lock identity drift.");
        const auto& reference = value.at("partition_ref");
        SP::require(reference.at("schema") == "orca.surface-partition-reference/v1" &&
                    reference.at("sha256") == partition_file_hash &&
                    safe_partition_path(reference.at("path"), partition_file_hash), "Unsafe contour partition reference.");
        std::map<std::string,nlohmann::json> cells;
        for (const auto& face : partition.at("faces")) for (const auto& cell : face.at("cells"))
            cells.emplace(cell.at("id"), cell);
        std::set<std::string> details, claimed;
        for (const auto& lock : value.at("locks")) {
            const auto label = lock.at("label").get<std::string>();
            const auto subject = lock.at("subject_id").get<std::string>();
            SP::require(ShapeLockSet::supported_label(label) && !subject.empty() &&
                        details.insert(subject + ":" + label).second &&
                        ShapeLockSet::allowed_parent(label, lock.at("parent_label")), "Invalid contour lock owner.");
            SP::require((lock.at("status") == "VALID_SHAPE" || lock.at("status") == "PROTECTED_SHAPE_UNCERTAIN") &&
                        lock.at("view_support").get<size_t>() >= 2 &&
                        !ShapeLockSet::hard_conflict(lock.at("reasons").get<std::vector<std::string>>()), "Contour lock has a hard conflict.");
            const auto locked = lock.at("locked_cells").get<std::vector<std::string>>();
            SP::require(!locked.empty() && std::is_sorted(locked.begin(),locked.end()) &&
                        std::adjacent_find(locked.begin(),locked.end()) == locked.end(), "Invalid locked cell list.");
            for (const auto& id : locked) {
                SP::require(cells.count(id) && claimed.insert(id).second, "Contour cell overlap or missing cell.");
                const auto& cell = cells.at(id);
                SP::require(cell.at("subject_id") == subject &&
                            (cell.at("label") == label || cell.at("label") == "iris-" + label), "Contour lock crosses its parent or eye side.");
            }
            const auto nested = lock.at("nested_cells").get<std::vector<std::string>>();
            SP::require(std::is_sorted(nested.begin(),nested.end()) && std::adjacent_find(nested.begin(),nested.end()) == nested.end(), "Invalid nested contour cells.");
            for (const auto& id : nested)
                SP::require((label == "le" || label == "re") && std::binary_search(locked.begin(),locked.end(),id) &&
                            cells.at(id).at("label") == "iris-" + label, "Nested iris leaves its eye.");
            const auto accessory = lock.value("periocular_cells",std::vector<std::string>{});
            SP::require(std::is_sorted(accessory.begin(),accessory.end()) &&
                        std::adjacent_find(accessory.begin(),accessory.end())==accessory.end(), "Invalid eye accessory cell list.");
            for (const auto& id : accessory)
                SP::require((label == "le" || label == "re") && cells.count(id) && claimed.insert(id).second && cells.at(id).at("subject_id") == subject &&
                            cells.at(id).at("label") == "periocular-" + label, "Eye accessory crosses its eye.");
        }
        return {value};
    }

    static ShapeLockSet decode_legacy(const nlohmann::json& value, const std::string& geometry,
                                      const std::string& source, size_t count) {
        return ShapeLockSet::decode(value,geometry,source,count);
    }
};
} // namespace Slic3r::AI
