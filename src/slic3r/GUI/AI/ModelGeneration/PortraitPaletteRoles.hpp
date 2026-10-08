#pragma once

#include "ModelPreviewPalette.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <map>
#include <set>

namespace Slic3r::GUI {
inline constexpr std::array<const char*,6> portrait_role_names {
    "portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};

inline bool valid_portrait_roles(const std::vector<std::string>& roles,size_t count) {
    if(roles.empty()) return true;
    if(roles.size()!=count) return false;
    std::set<std::string> seen;
    for(const auto& role:roles) if(!role.empty() &&
        (std::find(portrait_role_names.begin(),portrait_role_names.end(),role)==portrait_role_names.end() || !seen.insert(role).second)) return false;
    return true;
}
inline std::vector<std::string> portrait_roles_for_card(const std::vector<PreviewPalette::Color>& targets,
                                                       const std::vector<PreviewPalette::Color>& card) {
    if(card.size()!=6) return {};
    const std::vector<PreviewPalette::Color> inherited_card{{247.f/255,226.f/255,218.f/255},
        {40.f/255,38.f/255,41.f/255}, {246.f/255,247.f/255,249.f/255},
        {234.f/255,154.f/255,146.f/255}, {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const PreviewPalette::Color approved_skin{236.f/255,195.f/255,178.f/255};
    // The approved project card changed only its skin slot. Recognize this
    // explicit preset variant without changing physical colors or guessing custom roles.
    const bool inherited = card == inherited_card;
    std::vector<std::string> result(targets.size());
    for(size_t slot=0;slot<targets.size();++slot) for(size_t role=0;role<card.size();++role) {
        bool equal=true;
        for(size_t k=0;k<3;++k) equal=equal && std::abs(targets[slot][k]-card[role][k])<=.5f/255.f;
        if(!equal && role==0 && inherited) {
            equal=true;
            for(size_t k=0;k<3;++k) equal=equal && std::abs(targets[slot][k]-approved_skin[k])<=.5f/255.f;
        }
        if(equal) {result[slot]=portrait_role_names[role];break;}
    }
    return valid_portrait_roles(result,targets.size()) ? result : std::vector<std::string>{};
}
// Physical slots retain their identities even when several source appearance
// groups would match the same role. Histogram-group targets are not a color card.
inline PreviewPalette::ColorTrialMapping portrait_card_slot_mapping(
    const std::vector<PreviewPalette::Color>& targets, const std::vector<PreviewPalette::Color>& card) {
    if (targets.size() < 3 || targets.size() > 6) return {};
    const auto roles = portrait_roles_for_card(targets, card);
    if (roles.empty() || std::find(roles.begin(), roles.end(), std::string()) != roles.end()) return {};
    return {true, targets, targets};
}
inline std::string portrait_detail_role(const std::string& label,size_t count) {
    if(label=="lb" || label=="rb" || label=="iris" || label.compare(0,5,"iris-")==0 || label.compare(0,11,"periocular-")==0)
        return "portrait-dark";
    if(label=="le" || label=="re" || label=="teeth") return "portrait-light";
    if(label=="ulip" || label=="llip" || label=="lip-line-corner") return count==3 ? "portrait-dark" : "portrait-lips";
    return {};
}
inline std::map<std::string,AI::SemanticColoring::Color> portrait_contour_colors(const nlohmann::json& partition,
    const std::vector<std::string>& roles,const std::vector<AI::SemanticColoring::Color>& palette,
    std::set<std::string>* missing=nullptr) {
    if(!valid_portrait_roles(roles,palette.size())) throw std::invalid_argument("Portrait palette roles changed.");
    std::map<std::string,AI::SemanticColoring::Color> result;
    for(const auto& face:partition.at("faces")) for(const auto& cell:face.at("cells")) {
        const auto role=portrait_detail_role(cell.at("label"),palette.size());
        if(role.empty()) continue;
        const auto found=std::find(roles.begin(),roles.end(),role);
        if(found!=roles.end()) result.emplace(cell.at("id"),palette.at(size_t(found-roles.begin())));
        else if(missing) missing->insert(role);
    }
    return result;
}
} // namespace Slic3r::GUI
