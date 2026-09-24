#pragma once

#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stdexcept>

namespace Slic3r::GUI::LocalPrintRecipeProofState {
using Json=nlohmann::json;
inline std::string digest(const Json& value)
{
    const auto bytes=value.dump();
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int size=0;
    if(EVP_Digest(bytes.data(),bytes.size(),hash,&size,EVP_sha256(),nullptr)!=1 || size!=32)
        throw std::runtime_error("Cannot identify the local recipe evidence.");
    const char* hex="0123456789abcdef"; std::string result; result.reserve(64);
    for(unsigned int i=0;i<size;++i) {result+=hex[hash[i]>>4];result+=hex[hash[i]&15];}
    return result;
}
inline std::string canonical_color(std::string value)
{
    for(auto& c:value) if(c>='a' && c<='f') c=char(c-'a'+'A');
    return value;
}
inline AI::PrintRgb estimate_color(const std::vector<AI::PrintSublayerMaterial>& materials,const AI::MixedColorRecipe& recipe)
{
    unsigned char r=0,g=0,b=0;int total=0;
    for(const auto& c:recipe.components) {
        const auto m=std::find_if(materials.begin(),materials.end(),[&](const auto& v){return v.channel.slot==c.slot;});
        if(m==materials.end()) throw std::invalid_argument("Estimate references an absent material.");
        const auto& hex=m->channel.display_color;
        const auto cr=(unsigned char)std::stoul(hex.substr(1,2),nullptr,16);
        const auto cg=(unsigned char)std::stoul(hex.substr(3,2),nullptr,16);
        const auto cb=(unsigned char)std::stoul(hex.substr(5,2),nullptr,16);
        const int weight=int(std::lround(c.ratio*100));
        if(total==0) {r=cr;g=cg;b=cb;}
        else filament_mixer_lerp(r,g,b,cr,cg,cb,float(weight)/float(total+weight),&r,&g,&b);
        total+=weight;
    }
    return {float(r)/255,float(g)/255,float(b)/255};
}
// Shared with enumeration: keep the existing candidate identity stable.
inline Json context(const std::vector<AI::PrintSublayerMaterial>& materials,const AI::PrintSublayerProcess& p)
{
    Json j={{"schema","orcaslicer.local-sublayer-catalog.v1"},
        {"material_fingerprint",p.material_fingerprint},{"process_fingerprint",p.process_fingerprint},
        {"surface_condition",p.surface_condition},{"measurement_condition",p.measurement_condition},
        {"layer_heights_mm",p.layer_heights_mm},{"z_resolution_mm",p.z_resolution_mm},
        {"region_width_mm",p.region_width_mm},{"region_height_mm",p.region_height_mm},
        {"first_layer_unsplit",p.first_layer_unsplit},{"order","physical-slot-ascending"},{"materials",Json::array()}};
    for(const auto& m:materials) j["materials"].push_back({{"slot",m.channel.slot},{"color",canonical_color(m.channel.display_color)},
        {"type",m.channel.material_type},{"compatible",m.channel.compatible},{"identity",m.identity},
        {"min_layer_mm",m.min_layer_mm},{"max_layer_mm",m.max_layer_mm},{"line_width_mm",m.line_width_mm},
        {"temperature_c",m.temperature_c},{"min_temperature_c",m.min_temperature_c},{"max_temperature_c",m.max_temperature_c}});
    return j;
}
inline Json payload(const AI::PrintColorTarget& target)
{
    const auto& p=target.recipe_proof.value(); const auto& recipe=target.recipe.value();
    Json components=Json::array();
    for(const auto& c:recipe.components) components.push_back({c.slot,c.ratio});
    return {{"schema",p.schema},{"scope","supplied-constraints-only"},{"context",context(p.materials,p.process)},
        {"sublayers_enabled",p.process.sublayers_enabled},{"sublayer_heights_mm",p.sublayer_heights_mm},
        {"evidence_source",p.evidence_source},{"evidence_sha256",p.evidence_sha256},
        {"uncertainty_delta_e",p.uncertainty_delta_e},{"candidate_id",target.candidate_id},
        {"components",components},{"output",target.output},{"target_color",recipe.target_color},{"evidence",int(target.evidence)}};
}
inline Json encode(const AI::PrintColorTarget& target)
{
    auto j=payload(target);j["checksum"]=target.recipe_proof->checksum;return j;
}
inline size_t index(const Json& j)
{
    if(!j.is_number_integer() || (!j.is_number_unsigned() && j.get<int64_t>()<0))
        throw std::invalid_argument("Recipe evidence requires a nonnegative integer slot.");
    const auto n=j.get<uint64_t>();
    if(n>std::numeric_limits<size_t>::max()) throw std::invalid_argument("Recipe evidence slot overflow.");
    return size_t(n);
}
inline AI::PrintColorRecipeProof decode(const Json& j)
{
    if(j.at("scope")!="supplied-constraints-only") throw std::invalid_argument("Unknown recipe evidence scope.");
    const auto& c=j.at("context");
    if(c.at("schema")!="orcaslicer.local-sublayer-catalog.v1" || c.at("order")!="physical-slot-ascending")
        throw std::invalid_argument("Unknown recipe context or sublayer order.");
    AI::PrintColorRecipeProof proof;proof.schema=j.at("schema").get<std::string>();auto& p=proof.process;
    p.material_fingerprint=c.at("material_fingerprint").get<std::string>();p.process_fingerprint=c.at("process_fingerprint").get<std::string>();
    p.surface_condition=c.at("surface_condition").get<std::string>();p.measurement_condition=c.at("measurement_condition").get<std::string>();
    p.first_layer_unsplit=c.at("first_layer_unsplit").get<bool>();p.sublayers_enabled=j.at("sublayers_enabled").get<bool>();
    p.layer_heights_mm=c.at("layer_heights_mm").get<std::vector<double>>();
    p.z_resolution_mm=c.at("z_resolution_mm").get<double>();p.region_width_mm=c.at("region_width_mm").get<double>();p.region_height_mm=c.at("region_height_mm").get<double>();
    if(!c.at("materials").is_array() || c.at("materials").size()>6 || p.layer_heights_mm.size()>4096)
        throw std::invalid_argument("Recipe evidence exceeds its material or layer-height budget.");
    for(const auto& v:c.at("materials")) {
        AI::PrintSublayerMaterial m;
        m.channel={index(v.at("slot")),v.at("color").get<std::string>(),v.at("type").get<std::string>(),v.at("compatible").get<bool>()};
        m.identity=v.at("identity").get<std::string>();m.min_layer_mm=v.at("min_layer_mm").get<double>();m.max_layer_mm=v.at("max_layer_mm").get<double>();
        m.line_width_mm=v.at("line_width_mm").get<double>();m.temperature_c=v.at("temperature_c").get<double>();
        m.min_temperature_c=v.at("min_temperature_c").get<double>();m.max_temperature_c=v.at("max_temperature_c").get<double>();
        proof.materials.push_back(std::move(m));
    }
    const auto& heights=j.at("sublayer_heights_mm");
    if(!heights.is_array() || heights.size()>4096) throw std::invalid_argument("Too many saved sublayer rows.");
    for(const auto& row:heights) if(!row.is_array() || row.size()>3) throw std::invalid_argument("Invalid saved sublayer row.");
    proof.sublayer_heights_mm=heights.get<std::vector<std::vector<double>>>();
    proof.evidence_source=j.at("evidence_source").get<std::string>();proof.evidence_sha256=j.at("evidence_sha256").get<std::string>();
    proof.uncertainty_delta_e=j.at("uncertainty_delta_e").get<double>();proof.checksum=j.at("checksum").get<std::string>();
    return proof;
}
inline bool valid(const AI::PrintColorTarget& target,const AI::LocalPrintColorResult& result,std::string& error)
{
    auto fail=[&](const char* s){error=s;return false;};
    if(!target.recipe || !target.recipe_proof) return fail("Recipe constraint evidence is missing.");
    const auto& proof=*target.recipe_proof;const auto& p=proof.process;const auto& recipe=*target.recipe;
    if(!AI::is_valid_mixed_color_recipe(recipe) || recipe.components.size()<2 || !AI::valid_print_rgb(target.output))
        return fail("Invalid recipe structure or output color.");
    if(proof.schema!="orcaslicer.local-recipe-proof.v1" || recipe.existing_virtual_slot ||
       !AI::is_lowercase_sha256(proof.checksum) || digest(payload(target))!=proof.checksum)
        return fail("Recipe evidence checksum or scope does not match its target.");
    if(p.material_fingerprint!=result.material_fingerprint || p.process_fingerprint!=result.process_fingerprint ||
       !p.sublayers_enabled || !p.first_layer_unsplit || p.surface_condition.empty() || p.measurement_condition.empty())
        return fail("Recipe evidence does not match the current process.");
    auto positive=[](double v){return std::isfinite(v) && v>0;};
    if(!positive(p.z_resolution_mm) || !positive(p.region_width_mm) || !positive(p.region_height_mm) ||
       p.layer_heights_mm.empty() || p.layer_heights_mm.size()>4096 || proof.sublayer_heights_mm.size()!=p.layer_heights_mm.size() ||
       proof.materials.size()!=result.physical_channels.size() || proof.materials.size()<2 || proof.materials.size()>6)
        return fail("Recipe evidence has incomplete process dimensions.");
    size_t previous=0;bool first=true;
    for(const auto& m:proof.materials) {
        if(!first && m.channel.slot<=previous) return fail("Recipe material order is invalid.");first=false;previous=m.channel.slot;
        const auto actual=std::find_if(result.physical_channels.begin(),result.physical_channels.end(),[&](const auto& c){return c.slot==m.channel.slot;});
        if(actual==result.physical_channels.end() || canonical_color(actual->display_color)!=canonical_color(m.channel.display_color) ||
           actual->material_type!=m.channel.material_type || actual->compatible!=m.channel.compatible || m.identity.empty() ||
           !positive(m.min_layer_mm) || !positive(m.max_layer_mm) || m.min_layer_mm>m.max_layer_mm || !positive(m.line_width_mm) ||
           !positive(m.temperature_c) || !positive(m.min_temperature_c) || !positive(m.max_temperature_c) || m.min_temperature_c>m.max_temperature_c)
            return fail("Recipe material identity or limits are invalid.");
    }
    auto identity=context(proof.materials,p);identity["components"]=Json::array();first=true;
    for(const auto& c:recipe.components) {
        const double percent=c.ratio*100;
        if(!std::isfinite(percent) || percent<=0 || percent>=100 || std::abs(percent-std::round(percent))>1e-7 ||
           (!first && c.slot<=previous)) return fail("Recipe proportions or sublayer order are invalid.");
        first=false;previous=c.slot;identity["components"].push_back({c.slot,int(std::lround(percent))});
    }
    if(digest(identity)!=target.candidate_id) return fail("Recipe candidate identity does not match its input constraints.");
    for(size_t row=0;row<p.layer_heights_mm.size();++row) {
        const double height=p.layer_heights_mm[row];const auto& sub=proof.sublayer_heights_mm[row];
        if(!positive(height) || height>p.region_height_mm+1e-9 || sub.size()!=recipe.components.size()) return fail("Invalid checked recipe layer height.");
        std::string type;
        for(size_t i=0;i<sub.size();++i) {
            const auto& c=recipe.components[i];const auto m=std::find_if(proof.materials.begin(),proof.materials.end(),[&](const auto& v){return v.channel.slot==c.slot;});
            if(m==proof.materials.end() || !m->channel.compatible || m->channel.material_type.empty()) return fail("Unavailable recipe material.");
            if(i==0) type=m->channel.material_type;
            const double units=sub[i]/p.z_resolution_mm;
            if(m->channel.material_type!=type || m->temperature_c<m->min_temperature_c || m->temperature_c>m->max_temperature_c ||
               p.region_width_mm+1e-9<m->line_width_mm || !positive(sub[i]) || std::abs(sub[i]-height*c.ratio)>1e-9 ||
               sub[i]+1e-9<m->min_layer_mm || sub[i]>m->max_layer_mm+1e-9 || !std::isfinite(units) || std::abs(units-std::round(units))>1e-7)
                return fail("Saved recipe violates its material or sublayer constraints.");
        }
    }
    if(!std::isfinite(proof.uncertainty_delta_e) || proof.uncertainty_delta_e<0) return fail("Invalid recipe uncertainty.");
    if(target.evidence==AI::ColorEvidence::Estimated) {
        if(proof.evidence_source!="local-polynomial-estimate-v1" || !proof.evidence_sha256.empty() || proof.uncertainty_delta_e!=0)
            return fail("Invalid local estimate provenance.");
        const auto expected=estimate_color(proof.materials,recipe);
        for(size_t c=0;c<3;++c) if(std::abs(expected[c]-target.output[c])>1e-6f)
            return fail("Saved estimate disagrees with the local forward model.");
    } else if(target.evidence==AI::ColorEvidence::Measured || target.evidence==AI::ColorEvidence::Interpolated) {
        if(proof.evidence_source.empty() || !AI::is_lowercase_sha256(proof.evidence_sha256)) return fail("Calibration reference is incomplete.");
    } else return fail("Unknown recipe color evidence.");
    const char* hex="0123456789ABCDEF";std::string output_hex="#";
    for(float c:target.output) {const auto b=unsigned(std::lround(c*255));output_hex+=hex[b>>4];output_hex+=hex[b&15];}
    if(canonical_color(recipe.target_color)!=output_hex) return fail("Recipe display color disagrees with its predicted output.");
    // The checksum detects changes; it does not authenticate a measurement or
    // certify the supplied region dimensions against a native sliced model.
    return true;
}
} // namespace Slic3r::GUI::LocalPrintRecipeProofState

