#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticBoundaryRefinement.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/NativeMobileSamBoundaryRefiner.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

using namespace Slic3r::AI::SemanticColoring;

namespace {
RGBImage image()
{
    return {32, 32, std::vector<uint8_t>(32 * 32 * 3, 128)};
}

Prediction prediction(Label fill = Label::Unknown)
{
    Prediction result;
    result.labels.assign(32 * 32, fill);
    result.confidence.assign(32 * 32, fill == Label::Unknown ? 0.f : .95f);
    return result;
}

size_t pixel(int x, int y) { return size_t(y) * 32 + x; }

class FakeRefiner final : public IBoundaryRefiner {
public:
    bool invalid {false};
    std::string identity() const override { return "test.boundary/v1"; }
    BoundaryRefinement refine(const RGBImage& input, const Prediction&,
                              const BoundaryRefinementRequest&, const Cancel& cancel) override {
        BoundaryRefinement result;
        result.width = input.width; result.height = input.height;
        if (cancel && cancel()) { result.canceled = true; return result; }
        const float nan = std::numeric_limits<float>::quiet_NaN();
        result.foreground_probability.assign(32 * 32, nan);
        result.confidence.assign(32 * 32, 0.f);
        for (int y = 6; y < 25; ++y) for (int x = 3; x < 17; ++x) {
            result.foreground_probability[pixel(x, y)] = .9f;
            result.confidence[pixel(x, y)] = .9f;
        }
        // A confident disconnected island must not be accepted.
        result.foreground_probability[pixel(30, 30)] = .99f;
        result.confidence[pixel(30, 30)] = .99f;
        if (invalid) result.confidence.pop_back();
        return result;
    }
};

void add_exposed_ear(Prediction& body, Prediction& face)
{
    face.face_detected = true;
    for (int y = 10; y <= 21; ++y) for (int x = 4; x <= 7; ++x) {
        face.labels[pixel(x, y)] = Label::FaceSkin;
        face.confidence[pixel(x, y)] = .9f;
        body.labels[pixel(x, y)] = Label::FaceSkin;
        body.confidence[pixel(x, y)] = .9f;
    }
    for (int y = 8; y <= 23; ++y) for (int x = 8; x <= 14; ++x) {
        body.labels[pixel(x, y)] = Label::Hair;
        body.confidence[pixel(x, y)] = .92f;
    }
}
}

TEST_CASE("Ear boundary prompts require both reliable skin and adjacent hair", "[SemanticBoundaryRefinement]")
{
    const auto input = image();
    auto body = prediction(Label::FaceSkin), face = prediction();
    face.face_detected = true;
    CHECK(ear_hair_boundary_request(input, body, face).regions.empty());
    add_exposed_ear(body, face);
    const auto request = ear_hair_boundary_request(input, body, face);
    REQUIRE(request.regions.size() == 1);
    CHECK(request.foreground == Label::FaceSkin);
    CHECK(request.background == Label::Hair);
    CHECK(std::any_of(request.prompts.begin(), request.prompts.end(),
        [](const BoundaryPrompt& item) { return item.positive; }));
    CHECK(std::any_of(request.prompts.begin(), request.prompts.end(),
        [](const BoundaryPrompt& item) { return !item.positive; }));
}

TEST_CASE("Boundary refinement only restores seed-connected ear skin inside its ROI", "[SemanticBoundaryRefinement]")
{
    const auto input = image();
    auto body = prediction(), face = prediction();
    add_exposed_ear(body, face);
    face.labels[pixel(10, 15)] = Label::Eyebrow;
    face.confidence[pixel(10, 15)] = .95f;
    FakeRefiner refiner;
    const auto result = apply_ear_hair_boundary(input, body, face, refiner);
    REQUIRE(result.prediction.valid_for(input));
    REQUIRE(result.refinement.valid_for(input));
    REQUIRE(result.accepted.size() == 32 * 32);
    CHECK(result.prediction.labels[pixel(9, 14)] == Label::FaceSkin);
    CHECK(result.prediction.labels[pixel(10, 15)] == Label::Eyebrow);
    CHECK(result.prediction.labels[pixel(30, 30)] == Label::Unknown);
    CHECK(result.accepted[pixel(9, 14)] == 1);
    CHECK(result.accepted[pixel(30, 30)] == 0);
}

TEST_CASE("Hairline refinement requires independent skin and hair seeds", "[SemanticBoundaryRefinement]")
{
    const auto input = image();
    auto body = prediction(), face = prediction();
    face.face_detected = true;
    face.regions.push_back({0,BoundaryPart::Hairline,BoundarySide::Unspecified,
                            {4,4,28,18},{{5,5},{27,5},{27,17},{5,17}}});
    auto missing = facial_boundary_request(input, body, face);
    REQUIRE(missing.targets.size() == 1);
    CHECK(missing.targets.front().prompts.empty());

    for (int y = 8; y < 15; ++y) for (int x = 7; x < 15; ++x) {
        face.labels[pixel(x,y)] = Label::FaceSkin;
        face.confidence[pixel(x,y)] = .92f;
    }
    missing = facial_boundary_request(input, body, face);
    CHECK(missing.targets.front().prompts.empty());

    for (int y = 5; y < 11; ++y) for (int x = 18; x < 26; ++x) {
        body.labels[pixel(x,y)] = Label::Hair;
        body.confidence[pixel(x,y)] = .93f;
    }
    const auto valid = facial_boundary_request(input, body, face);
    REQUIRE(valid.targets.front().prompts.size() == 2);
    CHECK(valid.targets.front().foreground == Label::Hair);
    CHECK(valid.targets.front().background == Label::FaceSkin);
}

TEST_CASE("Ear hair proposals preserve reliable skin while recovering uncertain hair", "[SemanticBoundaryRefinement]")
{
    const auto input = image();
    auto body = prediction(), face = prediction();
    add_exposed_ear(body, face);
    for (int y = 10; y <= 21; ++y) for (int x = 8; x <= 10; ++x) {
        face.labels[pixel(x, y)] = Label::FaceSkin;
        face.confidence[pixel(x, y)] = .88f;
    }
    face.labels[pixel(11, 14)] = Label::FaceSkin;
    face.confidence[pixel(11, 14)] = .55f;
    class HairProposal final : public IBoundaryRefiner {
    public:
        std::string identity() const override { return "test.hair-proposal"; }
        BoundaryRefinement refine(const RGBImage& input, const Prediction&,
                                  const BoundaryRefinementRequest&, const Cancel&) override {
            BoundaryRefinement result;
            result.width = input.width; result.height = input.height;
            result.foreground_probability.assign(size_t(input.width) * input.height,
                                                  std::numeric_limits<float>::quiet_NaN());
            result.confidence.assign(size_t(input.width) * input.height, 0.f);
            for (int y = 8; y <= 23; ++y) for (int x = 3; x <= 14; ++x) {
                result.foreground_probability[pixel(x, y)] = x <= 7 ? .9f : .1f;
                result.confidence[pixel(x, y)] = .9f;
            }
            return result;
        }
    } refiner;
    const auto request = ear_hair_boundary_request(input, body, face);
    REQUIRE(request.targets.size() == 1);
    REQUIRE(request.targets[0].prompts.size() == 2);
    const auto result = apply_ear_hair_boundary(input, body, face, refiner);
    CHECK(result.prediction.labels[pixel(9, 14)] == Label::FaceSkin);
    CHECK(result.accepted[pixel(9, 14)] == 0);
    CHECK(result.prediction.labels[pixel(11, 14)] == Label::Hair);
}

TEST_CASE("Invalid or canceled boundary output leaves no stale recolor", "[SemanticBoundaryRefinement]")
{
    const auto input = image();
    auto body = prediction(), face = prediction();
    add_exposed_ear(body, face);
    FakeRefiner refiner;
    refiner.invalid = true;
    const auto fallback = refine_ear_hair_boundary(input, body, face, refiner);
    CHECK(fallback.labels == face.labels);
    refiner.invalid = false;
    const auto canceled = refine_ear_hair_boundary(input, body, face, refiner, [] { return true; });
    CHECK(canceled.canceled);
    CHECK(canceled.labels == face.labels);
}

TEST_CASE("Boundary providers register independently from body and face recognizers", "[SemanticBoundaryRefinement]")
{
    const std::string id = "test.boundary.factory.v1";
    CHECK(register_boundary_refiner_factory(id, [](const std::filesystem::path&) {
        return std::make_unique<FakeRefiner>();
    }));
    CHECK_FALSE(register_boundary_refiner_factory(id, [](const std::filesystem::path&) {
        return std::make_unique<FakeRefiner>();
    }));
    auto providers = create_region_recognizers("missing-body", "missing-face", id, {});
    REQUIRE(providers.boundary);
    CHECK(providers.boundary->identity() == "test.boundary/v1");

    auto fallback = create_region_recognizers("missing-body", "missing-face", "missing-boundary", {});
    CHECK_FALSE(fallback.boundary);
    CHECK_FALSE(fallback.boundary_error.empty());
    CHECK(fallback.error.find("boundary") == std::string::npos);
}

TEST_CASE("Boundary prompts stay on material interiors and inside the owning person ROI", "[SemanticBoundaryRefinement]")
{
    auto input = image(); auto body = prediction(), face = prediction(); add_exposed_ear(body, face);
    // A hollow skin region's centroid is not a valid skin prompt.
    for(int y=11;y<21;++y)for(int x=5;x<7;++x) { face.labels[pixel(x,y)]=Label::Unknown; face.confidence[pixel(x,y)]=0.f; }
    face.regions.push_back({7,BoundaryPart::Ear,BoundarySide::Left,{0,5,16,27},{}});
    face.regions.push_back({9,BoundaryPart::Ear,BoundarySide::Right,{20,5,32,27},{}});
    const auto request=facial_boundary_request(input,body,face);
    REQUIRE(request.targets.size()==2);
    CHECK(request.targets[0].person_id==7);
    REQUIRE(request.targets[0].prompts.size()==2);
    for(const auto& p:request.targets[0].prompts){
        CHECK(p.x<16);
        if(p.positive){const bool skin=(face.labels[pixel(p.x,p.y)]==Label::FaceSkin)||(body.labels[pixel(p.x,p.y)]==Label::FaceSkin);CHECK(skin);}
        else CHECK(body.labels[pixel(p.x,p.y)]==Label::Hair);
    }
    CHECK(request.targets[1].prompts.empty());
}

TEST_CASE("Failure in one boundary ROI does not erase another accepted ROI", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(),face=prediction();add_exposed_ear(body,face);
    face.regions.push_back({0,BoundaryPart::Ear,BoundarySide::Left,{0,5,16,27},{}});
    face.regions.push_back({1,BoundaryPart::Ear,BoundarySide::Right,{17,5,32,27},{}});
    for(int y=10;y<=21;++y)for(int x=20;x<24;++x){face.labels[pixel(x,y)]=Label::FaceSkin;face.confidence[pixel(x,y)]=.9f;}
    for(int y=8;y<=23;++y)for(int x=24;x<29;++x){body.labels[pixel(x,y)]=Label::Hair;body.confidence[pixel(x,y)]=.92f;}
    class PartialFailure final:public IBoundaryRefiner{public:
        FakeRefiner first; std::string identity()const override{return "test.partial";}
        BoundaryRefinement refine(const RGBImage& i,const Prediction& p,const BoundaryRefinementRequest& r,const Cancel& c)override{
            if(r.targets[0].person_id==1)throw std::runtime_error("One ROI failed");return first.refine(i,p,r,c);
        }
    } refiner;
    auto result=apply_facial_boundaries(input,body,face,refiner);
    CHECK(result.prediction.labels[pixel(9,14)]==Label::FaceSkin);
    REQUIRE(result.diagnostics.size()==2);
    CHECK(result.diagnostics[0].status=="accepted");CHECK(result.diagnostics[1].status=="error");
    CHECK(result.prediction.labels[pixel(25,14)]==face.labels[pixel(25,14)]);
}

TEST_CASE("Reliable face details cannot also become negative coarse hair prompts", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(Label::Hair),face=prediction(Label::FaceSkin);
    face.face_detected=true;
    face.regions.push_back({0,BoundaryPart::Ear,BoundarySide::Left,{0,0,32,32},{}});
    auto blocked=facial_boundary_request(input,body,face);
    REQUIRE(blocked.targets.size()==1);CHECK(blocked.targets[0].prompts.empty());
    // Expose reliable hair beside the face, then require separate prompt pixels.
    for(int y=0;y<32;++y)for(int x=24;x<32;++x){face.labels[pixel(x,y)]=Label::Unknown;face.confidence[pixel(x,y)]=0.f;}
    auto valid=facial_boundary_request(input,body,face);
    REQUIRE(valid.targets[0].prompts.size()==2);
    const auto& positive=valid.targets[0].prompts[0];const auto& negative=valid.targets[0].prompts[1];
    CHECK((positive.x!=negative.x || positive.y!=negative.y));CHECK(negative.x>=24);
}

TEST_CASE("Overlapping person hints cannot borrow or recolor each other's seeds", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(),face=prediction();add_exposed_ear(body,face);
    face.regions.push_back({3,BoundaryPart::Ear,BoundarySide::Left,{0,5,16,27},{}});
    // All reliable skin in the first ROI belongs to an overlapping person.
    face.regions.push_back({4,BoundaryPart::Ear,BoundarySide::Right,{3,9,8,23},{}});
    auto request=facial_boundary_request(input,body,face);
    REQUIRE(request.targets.size()==2);
    CHECK(request.targets[0].prompts.empty());CHECK(request.targets[1].prompts.empty());
    FakeRefiner refiner;auto result=apply_facial_boundaries(input,body,face,refiner);
    CHECK(result.prediction.labels==face.labels);
    CHECK(std::none_of(result.accepted.begin(),result.accepted.end(),[](uint8_t item){return item!=0;}));
}

TEST_CASE("Hidden ear proposals covering an eye cannot invoke boundary inference", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(),face=prediction();add_exposed_ear(body,face);
    face.regions.push_back({0,BoundaryPart::Ear,BoundarySide::Left,{0,5,16,27},{{1,6},{15,6},{15,26},{1,26}}});
    face.regions.push_back({0,BoundaryPart::Eye,BoundarySide::Right,{6,11,13,19},{{7,12},{12,12},{12,18},{7,18}}});
    auto request=ear_hair_boundary_request(input,body,face);REQUIRE(request.targets.size()==1);CHECK(request.targets[0].prompts.empty());
    class NoCall final:public IBoundaryRefiner{public:int calls=0;std::string identity()const override{return "test.visibility";}
        BoundaryRefinement refine(const RGBImage&,const Prediction&,const BoundaryRefinementRequest&,const Cancel&)override{++calls;return {};}
    } refiner;
    auto result=apply_ear_hair_boundary(input,body,face,refiner);CHECK(refiner.calls==0);CHECK(result.prediction.labels==face.labels);
    REQUIRE(result.diagnostics.size()==1);CHECK(result.diagnostics[0].status=="rejected");
    // The visible ear remains eligible when the eye is outside its support.
    face.regions[1].box={20,11,28,19};face.regions[1].support_polygon={{21,12},{27,12},{27,18},{21,18}};
    request=ear_hair_boundary_request(input,body,face);REQUIRE(request.targets.size()==1);CHECK(request.targets[0].prompts.size()==2);
}

TEST_CASE("A padded ear window may refine a partial eye overlap while preserving eye detail", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(),face=prediction();add_exposed_ear(body,face);
    face.regions.push_back({0,BoundaryPart::Ear,BoundarySide::Left,{0,5,20,27},{{1,6},{19,6},{19,26},{1,26}}});
    // The eye center is inside the padded ear window, but only one eye corner
    // is covered. This is a visible ear edge, not a hidden ear proposal.
    face.regions.push_back({0,BoundaryPart::Eye,BoundarySide::Right,{14,11,28,19},{{14,12},{20,12},{20,18},{14,18}}});
    const auto request=ear_hair_boundary_request(input,body,face);
    REQUIRE(request.targets.size()==1);
    CHECK(request.targets[0].prompts.size()==2);
}

TEST_CASE("A successful model may reject its candidate without becoming a runtime failure", "[SemanticBoundaryRefinement]")
{
    auto input=image();auto body=prediction(),face=prediction();add_exposed_ear(body,face);
    class Declined final:public IBoundaryRefiner{public:std::string identity()const override{return "test.declined";}
        BoundaryRefinement refine(const RGBImage&,const Prediction&,const BoundaryRefinementRequest&,const Cancel&)override{
            BoundaryRefinement out;out.rejected=true;out.rejection_reason="Inconsistent automatic prompts";return out;
        }
    } refiner;
    auto result=apply_ear_hair_boundary(input,body,face,refiner);
    CHECK(result.prediction.labels==face.labels);REQUIRE(result.diagnostics.size()==1);
    CHECK(result.diagnostics[0].status=="rejected");CHECK(result.diagnostics[0].reason=="Inconsistent automatic prompts");
    CHECK(result.prediction.error.empty());
}

TEST_CASE("MobileSAM pads normalized image tensors with zero", "[SemanticBoundaryRefinement]")
{
    RGBImage crop {3,2,std::vector<uint8_t>(18,128)};int w=0,h=0;
    auto tensor=mobile_sam_image_tensor(crop,w,h);
    REQUIRE(tensor.size()==size_t(3*1024*1024));CHECK(w==1024);CHECK(h==683);
    CHECK(std::abs(tensor[0]-(128.f-123.675f)/58.395f)<1e-6f);
    for(int c=0;c<3;++c)CHECK(tensor[size_t(c)*1024*1024+size_t(h)*1024]==0.f);
}

TEST_CASE("Native MobileSAM matches the fixed Python crop and cancels in flight", "[.][NativeMobileSam]")
{
    const char* fixture=std::getenv("ORCA_BOUNDARY_PARITY_CASE");REQUIRE(fixture!=nullptr);
    std::ifstream stream(std::filesystem::u8path(fixture));nlohmann::json doc;stream>>doc;
    auto refiner=create_mobile_sam_refiner(std::filesystem::u8path(doc.at("runtime").get<std::string>()));
    RGBImage input;input.width=doc.at("width");input.height=doc.at("height");input.pixels.resize(size_t(input.width)*input.height*3);
    std::ifstream rgb(std::filesystem::u8path(doc.at("rgb").get<std::string>()),std::ios::binary);REQUIRE(bool(rgb.read(reinterpret_cast<char*>(input.pixels.data()),std::streamsize(input.pixels.size()))));
    BoundaryRefinementRequest request;request.regions.push_back({0,0,input.width,input.height});
    for(const auto& p:doc.at("positive"))request.prompts.push_back({p[0],p[1],true});for(const auto& p:doc.at("negative"))request.prompts.push_back({p[0],p[1],false});
    std::vector<float> expected(size_t(input.width)*input.height);std::ifstream mask(std::filesystem::u8path(doc.at("probability").get<std::string>()),std::ios::binary);REQUIRE(bool(mask.read(reinterpret_cast<char*>(expected.data()),std::streamsize(expected.size()*sizeof(float)))));
    auto output=refiner->refine(input,{},request,{});INFO(output.error);REQUIRE(output.valid_for(input));
    float maximum=0.f;size_t mismatch=0;for(size_t p=0;p<expected.size();++p){maximum=std::max(maximum,std::abs(expected[p]-output.foreground_probability[p]));mismatch+=((expected[p]>=.5f)!=(output.foreground_probability[p]>=.5f));}
    INFO("maximum probability delta "<<maximum<<"; changed mask pixels "<<mismatch);
    CHECK(maximum<.0002f);CHECK(mismatch==0);
    auto inconsistent=request;inconsistent.prompts.push_back({request.prompts.front().x,request.prompts.front().y,false});
    auto declined=refiner->refine(input,{},inconsistent,{});CHECK(declined.rejected);CHECK(declined.error.empty());CHECK_FALSE(declined.valid_for(input));
    auto second=refiner->refine(input,{},request,{});REQUIRE(second.valid_for(input));CHECK(second.encoding_ms==0.);
    auto canceled=refiner->refine(input,{},request,[]{return true;});CHECK(canceled.canceled);
    // Cancellation becomes true during decoder Run, after entry validation.
    std::atomic<int> polls {0};auto interrupted=refiner->refine(input,{},request,[&]{return ++polls>2;});CHECK(interrupted.canceled);
    auto recovered=refiner->refine(input,{},request,{});CHECK(recovered.valid_for(input));
    // Cancel a new crop during encoder Run, then return to the previous crop.
    // The last successful embedding remains reusable with its original key.
    auto different=input;different.pixels.back()^=uint8_t(1);polls=0;
    auto encoder_canceled=refiner->refine(different,{},request,[&]{return ++polls>2;});CHECK(encoder_canceled.canceled);
    auto old_crop=refiner->refine(input,{},request,{});REQUIRE(old_crop.valid_for(input));CHECK(old_crop.encoding_ms==0.);
    CHECK(old_crop.foreground_probability==output.foreground_probability);
    nlohmann::json report {{"max_probability_delta",maximum},{"mask_mismatch_pixels",mismatch},{"loading_ms",output.loading_ms},{"encoding_ms",output.encoding_ms},{"decoding_ms",output.decoding_ms},{"score",output.model_score},{"identity",refiner->identity()}};
    if(doc.contains("report")){std::ofstream out(std::filesystem::u8path(doc.at("report").get<std::string>()));out<<report.dump(2);}
}
