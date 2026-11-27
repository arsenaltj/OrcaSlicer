#include "model_internal.hpp"
#define ORCA_PRINT_COLOR_DIRECT_ONLY
#include "slic3r/AI/Color/LocalPrintColorMatching.hpp"
#include "slic3r/AI/Color/LocalPrintColorQuality.hpp"

namespace orca_model_detail {
namespace {
namespace Palette = Slic3r::GUI::PreviewPalette;
std::string fingerprint(const J& r, const char* key)
{
    const auto value = r.at(key).get<std::string>();
    if (value.size()!=64 || value.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw Error("INVALID_COLOR_IDENTITY", "Supply lowercase SHA256 material and process fingerprints");
    return value;
}
double face_area(const Mesh& mesh, size_t face)
{
    const auto& t=mesh.faces[face];const auto& a=mesh.vertices[t[0]];const auto& b=mesh.vertices[t[1]];const auto& c=mesh.vertices[t[2]];
    V u{},v{},cross{};for(int i=0;i<3;++i){u[i]=b[i]-a[i];v[i]=c[i]-a[i];}
    for(int i=0;i<3;++i)cross[i]=u[(i+1)%3]*v[(i+2)%3]-u[(i+2)%3]*v[(i+1)%3];
    double area=std::sqrt(cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2])/2.;
    if(!std::isfinite(area)||area<=0)throw Error("INVALID_COLOR_GEOMETRY","Repair degenerate geometry before matching");
    return area;
}
Palette::Color color(const J& value)
{
    auto rgb=value.get<Palette::Color>();
    for(auto x:rgb)if(!std::isfinite(x)||x<0||x>1)throw Error("INVALID_COLOR","Expected normalized sRGB");
    return rgb;
}
}
J print_colors(Context& c,const J& r)
{
    expected(c,r);valid(c.doc.mesh);
    if(!r.contains("documentId")||r.at("documentId")!=c.doc.id)throw Error("STALE_DOCUMENT","Supply the current documentId");
    const auto op=r.at("operation").get<std::string>();
    if(op=="applyPrintColors"||op=="savePrintColorDraft"){
        if(c.candidate.id.empty()||c.candidate_base!=c.doc.revision||c.candidate_generation!=c.generation||
           c.candidate_geometry!=c.doc.geometry||c.candidate_document!=c.doc.id||
           r.at("candidateId")!=c.candidate_id||!c.candidate.appearance.value("printColorCandidate",false))
            throw Error("STALE_CANDIDATE","Print color candidate is no longer current");
        const auto& plan=c.candidate.appearance.at("printColorPlan");
        if(op=="savePrintColorDraft"){
            if(!plan.value("trialOnly",false))throw Error("COLOR_NOT_TRIAL","This operation accepts only trial candidates");
            auto next=c.candidate;next.mesh.materials=c.doc.mesh.materials;
            next.appearance["printColorDraft"]=plan;next.appearance.erase("printColorPlan");next.appearance.erase("printColorCandidate");
            return commit(c,std::move(next));
        }
        if(plan.value("trialOnly",false))throw Error("COLOR_TRIAL_ONLY","Trial candidates cannot be applied as manufacturing material plans");
        if(!r.value("confirmMaterialMapping",false)||fingerprint(r,"materialFingerprint")!=plan.at("materialFingerprint")||
           fingerprint(r,"processFingerprint")!=plan.at("processFingerprint"))
            throw Error("COLOR_PLAN_CHANGED","Confirm the same material and process snapshot");
        if(!plan.value("withinTolerance",false)&&!r.value("acceptColorError",false))throw Error("COLOR_ERROR_CONFIRMATION_REQUIRED","Explicitly accept the predicted color approximation");
        auto next=c.candidate;next.appearance.erase("printColorCandidate");next.appearance.erase("printColorDraft");
        check_cancel(c);return commit(c,std::move(next));
    }
    const int count=r.value("requestedColorCount",6);
    if(count<1||count>6)throw Error("UNSUPPORTED_COLOR_COUNT","Direct matching supports 1..6 targets; layered recipes are not enabled");
    const auto& source=c.doc.mesh;
    Palette::Histogram histogram;
    for(size_t f=0;f<c.doc.mesh.faces.size();++f){
        check_cancel(c);const auto& rgba=c.doc.mesh.colors[f];
        const uint32_t packed=(uint32_t(std::lround(rgba[0]*255))<<16)|(uint32_t(std::lround(rgba[1]*255))<<8)|uint32_t(std::lround(rgba[2]*255));
        histogram.add(packed,face_area(c.doc.mesh,f));
    }
    const auto targets=histogram.print_palette(size_t(count));check_cancel(c);
    if(op=="getPrintPalette")return J{{"documentId",c.doc.id},{"revision",c.doc.revision},{"geometryRevision",c.doc.geometry},
        {"colors",targets},{"requestedColorCount",count},{"scope","area-weighted-face-colors"}};
    const auto materials=r.at("palette");
    if(!materials.is_array()||materials.empty()||materials.size()>6)throw Error("INVALID_PALETTE","Direct matching supports 1..6 physical material entries");
    const auto material_fingerprint=fingerprint(r,"materialFingerprint"),process_fingerprint=fingerprint(r,"processFingerprint");
    std::vector<Palette::Color> physical,physical_lab,target_lab;std::vector<int> ids;std::set<int> unique_ids,slots;
    for(const auto& entry:materials){
        int id=entry.at("materialId").get<int>(),slot=entry.at("slot").get<int>();
        if(id<0||id>255||slot<0||slot>255||!unique_ids.insert(id).second||!slots.insert(slot).second)
            throw Error("INVALID_PALETTE","Material IDs and physical slots must be unique and in range");
        const auto rgb=color(entry.at("color"));physical.push_back(rgb);physical_lab.push_back(Palette::to_lab(rgb));ids.push_back(id);
    }
    std::map<size_t,size_t> locked_faces;
    if(r.contains("materialLocks"))for(const auto& lock:r.at("materialLocks")){
        const auto id=lock.at("materialId").get<int>();const auto found=std::find(ids.begin(),ids.end(),id);
        if(found==ids.end())throw Error("INVALID_MATERIAL_LOCK","Locked material is absent from the physical palette");
        const auto route=size_t(found-ids.begin());
        for(const auto& raw:lock.at("faces")){const auto face=raw.get<size_t>();if(face>=source.faces.size())throw Error("INVALID_FACE","Locked face is out of range");
            const auto old=locked_faces.emplace(face,route);if(!old.second&&old.first->second!=route)throw Error("COLOR_LOCK_CONFLICT","Conflicting material locks");}
    }
    Slic3r::GUI::LocalPrintColorMatching::Input input;
    input.identity.source_sha256=c.doc.input_sha;input.identity.geometry_id=std::to_string(c.doc.geometry);
    input.identity.material_fingerprint=material_fingerprint;input.identity.process_fingerprint=process_fingerprint;
    input.identity.requested_color_count=size_t(count);input.identity.face_count=c.doc.mesh.faces.size();
    input.cancelled=[&]{return c.cancel.load();};input.tolerance=r.value("colorTolerance",5.0);
    for(size_t i=0;i<physical.size();++i){
        const auto& rgb=physical[i];char hex[8];std::snprintf(hex,sizeof(hex),"#%02X%02X%02X",int(std::lround(rgb[0]*255)),int(std::lround(rgb[1]*255)),int(std::lround(rgb[2]*255)));
        input.identity.physical_channels.push_back({materials[i].at("slot").get<size_t>(),hex,materials[i].value("materialType",std::string("unspecified")),true});
    }
    for(size_t f=0;f<c.doc.mesh.faces.size();++f){check_cancel(c);const auto& rgba=c.doc.mesh.colors[f];input.faces.push_back({{float(rgba[0]),float(rgba[1]),float(rgba[2])},face_area(c.doc.mesh,f)});}
    if(r.contains("protectedFaces")){Slic3r::AI::PrintColorRegion region;region.id="manual-protection";region.confidence=1;region.user_protected=true;region.faces=r.at("protectedFaces").get<std::vector<size_t>>();input.identity.regions.push_back(region);}
    std::map<size_t,std::vector<size_t>> by_material;for(const auto& lock:locked_faces)by_material[lock.second].push_back(lock.first);
    for(const auto& item:by_material){Slic3r::AI::PrintColorRegion region;region.id="manual-lock-"+std::to_string(item.first);region.confidence=1;region.user_protected=true;region.faces=item.second;region.locked_physical_slot=materials[item.first].at("slot").get<size_t>();input.identity.regions.push_back(region);}
    auto calculation=Slic3r::GUI::LocalPrintColorMatching::compute(input);
    if(!calculation.ok()){if(calculation.cancelled)throw Error("CANCELED","Color matching canceled");throw Error(calculation.error.find("budget")!=std::string::npos?"COLOR_BUDGET_CONFLICT":"COLOR_MATCH_FAILED",calculation.error);}
    const auto& result=calculation.result;Document next=c.doc;double total_area=0,error_sum=0,worst=0;bool within=true;J groups=J::array();
    for(const auto& target:result.targets){
        if(!target.executable||!target.physical_slot||target.recipe)throw Error("COLOR_UNASSIGNED","Direct physical material assignment is incomplete");
        groups.push_back(J{{"sourceColor",target.source},{"outputColor",target.output},{"slot",*target.physical_slot},{"deltaE00",target.delta_e00},{"withinTolerance",target.within_tolerance}});within=within&&target.within_tolerance;
    }
    for(size_t f=0;f<next.mesh.faces.size();++f){check_cancel(c);const auto& target=result.targets.at(result.face_targets.at(f));
        const auto it=std::find_if(materials.begin(),materials.end(),[&](const auto& x){return x.at("slot")==*target.physical_slot;});
        next.mesh.materials[f]=it->at("materialId").template get<int>();const auto& rgb=target.output;next.mesh.colors[f]={rgb[0],rgb[1],rgb[2],c.doc.mesh.colors[f][3]};
        const double area=input.faces[f].area,error=Slic3r::GUI::LocalPrintColorMatching::delta_e(input.faces[f].color,rgb);total_area+=area;error_sum+=area*error;worst=std::max(worst,error);
    }
    const auto quality=Slic3r::GUI::LocalPrintColorQuality::evaluate(input.faces,result);
    if(!quality.error.empty())throw Error("COLOR_QUALITY_FAILED",quality.error);
    J plan{{"schemaVersion",1},{"scope","shared-desktop-direct-print-color-v1"},{"trialOnly",r.value("trialOnly",false)},{"materialFingerprint",material_fingerprint},
           {"processFingerprint",process_fingerprint},{"palette",materials},{"groups",groups},
           {"geometryRevision",c.doc.geometry},{"sourceRevision",c.doc.revision},{"routingVerified",false},{"withinTolerance",within},
           {"quality",{{"metric","CIEDE2000"},{"mean",quality.mean_delta_e},{"p95",quality.p95_delta_e},{"max",quality.worst_delta_e},{"unresolvedAreaFraction",quality.unresolved_area_fraction},{"overToleranceAreaFraction",quality.over_tolerance_area_fraction},{"tolerance",input.tolerance}}}};
    if(r.contains("suiteRef")){const auto ref=r.at("suiteRef");if(!ref.is_object()||ref.dump().size()>4096)throw Error("INVALID_SUITE_REF","Suite binding must be a small identity object");plan["suiteRef"]=ref;}
    next.appearance["printColorPlan"]=plan;next.appearance["printColorCandidate"]=true;
    auto descriptor=preview_document(c,next);check_cancel(c);
    const auto id=c.doc.id+":print-color:"+std::to_string(c.generation)+":"+std::to_string(c.doc.revision)+":"+sha256(plan.dump()).substr(0,24);
    c.candidate=std::move(next);c.candidate_base=c.doc.revision;c.candidate_generation=c.generation;c.candidate_geometry=c.doc.geometry;
    c.candidate_document=c.doc.id;c.candidate_id=id;
    return J{{"candidateId",id},{"sourceRevision",c.doc.revision},{"scene",descriptor},{"plan",plan}};
}
}
