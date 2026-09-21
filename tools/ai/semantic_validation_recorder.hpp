#pragma once
// Developer-only decorators: persist exactly the images and prompts consumed
// by the native pipeline. Identity and inference are delegated unchanged.
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace Slic3r::AI::SemanticColoring {
class SemanticValidationRecorder {
    std::filesystem::path m_root;
    size_t m_index {0};
    std::string m_current_render;
    template<class T> static void raw(const std::filesystem::path& path,const std::vector<T>& values) {
        std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));
        if(!out)throw std::runtime_error("Cannot write model validation evidence.");
    }
    static void json(const std::filesystem::path& path,const nlohmann::json& document) {std::ofstream out(path);out<<document.dump(2);if(!out)throw std::runtime_error("Cannot write model validation metadata.");}
    std::filesystem::path image(const RGBImage& input,const std::string& type) {
        const auto dir=m_root/(std::to_string(m_index++)+"-"+type);std::filesystem::create_directories(dir);
        std::ofstream out(dir/"input.ppm",std::ios::binary);out<<"P6\n"<<input.width<<" "<<input.height<<"\n255\n";out.write(reinterpret_cast<const char*>(input.pixels.data()),std::streamsize(input.pixels.size()));
        if(!out)throw std::runtime_error("Cannot write validation input.");return dir;
    }
public:
    explicit SemanticValidationRecorder(std::filesystem::path root):m_root(std::move(root)){std::filesystem::create_directories(m_root);}
    void rendered(const RenderedView& view,int view_index,const ViewRegion& region,bool face_crop) {
        const auto dir=image(view.image,face_crop?"face-render":"body-render");
        m_current_render=dir.filename().string();
        raw(dir/"face_ids.u32",view.face_ids);raw(dir/"depth.f32",view.depth);
        raw(dir/"barycentric.f32",view.barycentric);
        nlohmann::json hashes=nlohmann::json::object();
        for(const char* name:{"input.ppm","face_ids.u32","depth.f32","barycentric.f32"}) {
            const auto hash=Slic3r::AI::model_artifact_sha256((dir/name).string());
            if(hash.size()!=64)throw std::runtime_error("Cannot hash rendered validation evidence.");
            hashes[name]=hash;
        }
        json(dir/"result.json",{{"schema","orca.semantic-render-evidence/v1"},{"render_id",m_current_render},
            {"width",view.image.width},{"height",view.image.height},{"view_index",view_index},
            {"yaw_degrees",view_index*45.},{"camera_elevation_ratio",.1},{"face_crop",face_crop},
            {"normalized_region",{region.left,region.top,region.width,region.height}},
            {"region_convention","left,top,width,height in full-view camera; letterboxed without distortion"},
            {"background_face_id",UINT32_MAX},{"depth_convention","larger is closer to camera; background is negative infinity"},
            {"raw_format","little-endian; row-major; original face IDs; barycentric 3xf32"},{"sha256",hashes}});
    }
    void prediction(const RGBImage& input,const Prediction& output,const std::string& type,const std::string& identity) {
        const auto dir=image(input,type);raw(dir/"labels.u8",output.labels);raw(dir/"confidence.f32",output.confidence);
        nlohmann::json regions=nlohmann::json::array();
        for(const auto& hint:output.regions)regions.push_back({{"person_id",hint.person_id},{"part",int(hint.part)},{"side",int(hint.side)},{"box",hint.box},{"support_polygon",hint.support_polygon}});
        std::map<int,size_t> counts;for(auto label:output.labels)++counts[int(label)];
        nlohmann::json labels=nlohmann::json::object();for(auto pair:counts)labels[std::to_string(pair.first)]=pair.second;
        json(dir/"result.json",{{"schema","orca.semantic-recognizer-evidence/v1"},{"width",input.width},{"height",input.height},{"type",type},{"render_id",m_current_render},{"identity",identity},{"person_detected",output.person_detected},{"face_detected",output.face_detected},{"canceled",output.canceled},{"error",output.error},{"regions",regions},{"label_pixels",labels}});
    }
    void boundary(const RGBImage& input,const BoundaryRefinementRequest& request,const BoundaryRefinement& output,const std::string& identity) {
        const auto dir=image(input,"boundary");raw(dir/"probability.f32",output.foreground_probability);raw(dir/"confidence.f32",output.confidence);
        nlohmann::json regions=nlohmann::json::array();size_t index=0;
        for(const auto& target:request.targets){nlohmann::json pos=nlohmann::json::array(),neg=nlohmann::json::array();for(const auto& point:target.prompts)(point.positive?pos:neg).push_back({point.x,point.y});const auto& b=target.region;
            regions.push_back({{"id","person"+std::to_string(target.person_id)+"-part"+std::to_string(int(target.part))+"-side"+std::to_string(int(target.side))+"-"+std::to_string(index++)},{"box",{b.left,b.top,b.right,b.bottom}},{"positive",pos},{"negative",neg},{"foreground",int(target.foreground)},{"background",int(target.background)},{"person_id",target.person_id},{"part",int(target.part)},{"side",int(target.side)},{"support_polygon",target.support_polygon}});
        }
        json(dir/"prompts.json",{{"schema","orcaslicer.semantic-boundary-prompts/v1"},{"case_id",dir.filename().string()},{"prompt_kind","actual-native-automatic"},{"regions",regions}});
        json(dir/"result.json",{{"schema","orca.semantic-boundary-evidence/v1"},{"render_id",m_current_render},{"width",input.width},{"height",input.height},{"identity",identity},{"model_score",output.model_score},{"rejected",output.rejected},{"rejection_reason",output.rejection_reason},{"loading_ms",output.loading_ms},{"encoding_ms",output.encoding_ms},{"decoding_ms",output.decoding_ms},{"canceled",output.canceled},{"error",output.error}});
    }
};
class RecordingBodyRecognizer final:public IBodyRegionRecognizer {
    IBodyRegionRecognizer& m_next;std::shared_ptr<SemanticValidationRecorder> m_record;
public:
    RecordingBodyRecognizer(IBodyRegionRecognizer& next,std::shared_ptr<SemanticValidationRecorder> record):m_next(next),m_record(std::move(record)){}
    std::string identity()const override{return m_next.identity();}
    Prediction predict(const RGBImage& input,const Cancel& cancel)override{auto output=m_next.predict(input,cancel);m_record->prediction(input,output,"body",identity());return output;}
};
class RecordingFaceRecognizer final:public IFaceRegionRecognizer {
    IFaceRegionRecognizer& m_next;std::shared_ptr<SemanticValidationRecorder> m_record;
public:
    RecordingFaceRecognizer(IFaceRegionRecognizer& next,std::shared_ptr<SemanticValidationRecorder> record):m_next(next),m_record(std::move(record)){}
    std::string identity()const override{return m_next.identity();}
    Prediction predict(const RGBImage& input,const Cancel& cancel)override{auto output=m_next.predict(input,cancel);m_record->prediction(input,output,"face",identity());return output;}
};
class RecordingBoundaryRefiner final:public IBoundaryRefiner {
    IBoundaryRefiner& m_next;std::shared_ptr<SemanticValidationRecorder> m_record;
public:
    RecordingBoundaryRefiner(IBoundaryRefiner& next,std::shared_ptr<SemanticValidationRecorder> record):m_next(next),m_record(std::move(record)){}
    std::string identity()const override{return m_next.identity();}
    BoundaryRefinement refine(const RGBImage& input,const Prediction& coarse,const BoundaryRefinementRequest& request,const Cancel& cancel)override{auto output=m_next.refine(input,coarse,request,cancel);m_record->boundary(input,request,output,identity());return output;}
};
}
