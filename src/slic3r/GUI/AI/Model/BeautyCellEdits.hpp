#pragma once

#include "BeautyLeafEditing.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

namespace Slic3r::AI {
struct BeautyCellEdits {
    std::map<std::string,SemanticColoring::Color> colors;
    SurfaceSelectionPersistence::SelectionState selection;

    static nlohmann::json encode(const BeautyLeafEditing& editing,
        const std::map<std::string,SemanticColoring::Color>& colors,
        SurfaceSelectionPersistence::SelectionState state) {
        if(!editing.cells) throw std::invalid_argument("Cell edits require a contour surface.");
        const auto& partition=editing.cells->partition;
        nlohmann::json value={{"schema","orca.beauty-cell-edit/v1"},{"geometry_id",partition.at("geometry_id")},
            {"source_sha256",partition.at("source_sha256")},{"mapping_sha256",editing.mapping_fingerprint()},
            {"boundary_sha256",editing.shape_boundary_sha256},{"triangle_count",editing.size()},
            {"colors",nlohmann::json::array()}};
        for(const auto& entry:colors) {
            editing.cells->indices(entry.first);
            for(float c:entry.second) if(!std::isfinite(c) || c<0 || c>1) throw std::invalid_argument("Invalid cell color.");
            value["colors"].push_back({entry.first,entry.second});
        }
        const std::array<const char*,4> names{"selected","protected","foreground","domain"};
        const std::array<std::vector<uint8_t>*,4> masks{&state.selected,&state.protected_faces,&state.foreground,&state.domain};
        for(size_t k=0;k<names.size();++k) {
            if(masks[k]->empty()) masks[k]->assign(editing.size(),0);
            if(masks[k]->size()!=editing.size()) throw std::invalid_argument("Cell selection mapping changed.");
            value[names[k]]=nlohmann::json::array();
            for(size_t i=0;i<editing.size();++i) {
                if((*masks[k])[i]>1) throw std::invalid_argument("Invalid cell selection mask.");
                if((*masks[k])[i]) value[names[k]].push_back(i);
            }
        }
        return value;
    }
    static BeautyCellEdits decode(const nlohmann::json& value,const BeautyLeafEditing& editing) {
        if(!editing.cells || !value.is_object() || value.size()!=11 || value.at("schema")!="orca.beauty-cell-edit/v1" ||
            value.at("geometry_id")!=editing.cells->partition.at("geometry_id") ||
            value.at("source_sha256")!=editing.cells->partition.at("source_sha256") ||
            value.at("mapping_sha256")!=editing.mapping_fingerprint() ||
            value.at("boundary_sha256")!=editing.shape_boundary_sha256 || value.at("triangle_count")!=editing.size() ||
            !value.at("colors").is_array() || value.at("colors").size()>editing.size())
            throw std::invalid_argument("Cell draft identity changed.");
        BeautyCellEdits result;
        std::string previous;
        for(const auto& row:value.at("colors")) {
            if(!row.is_array() || row.size()!=2 || !row[0].is_string()) throw std::invalid_argument("Invalid cell draft color.");
            const auto id=row[0].get<std::string>();
            if(!previous.empty() && previous>=id) throw std::invalid_argument("Cell draft colors are not unique and sorted.");
            previous=id;editing.cells->indices(id);
            const auto color=row[1].get<SemanticColoring::Color>();
            for(float c:color) if(!std::isfinite(c) || c<0 || c>1) throw std::invalid_argument("Invalid cell draft color.");
            result.colors.emplace(id,color);
        }
        const std::array<const char*,4> names{"selected","protected","foreground","domain"};
        const std::array<std::vector<uint8_t>*,4> masks{&result.selection.selected,&result.selection.protected_faces,
                                                     &result.selection.foreground,&result.selection.domain};
        for(size_t k=0;k<names.size();++k) {
            masks[k]->assign(editing.size(),0);
            for(auto i:ShapeLockSet::faces(value.at(names[k]),editing.size())) (*masks[k])[i]=1;
        }
        return result;
    }
};
} // namespace Slic3r::AI
