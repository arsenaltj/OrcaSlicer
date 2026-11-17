#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/Redesign/GenerationParticleModel.hpp"
#include "slic3r/GUI/Redesign/GenerationParticleImage.hpp"
#include "slic3r/GUI/Redesign/GenerationForeground.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedModelTaskFlow.hpp"
#include "../test_utils.hpp"
#include <wx/init.h>
#include <wx/mstream.h>
#include <atomic>
#include <thread>

using namespace Slic3r::GUI;
namespace GP = Slic3r::GUI::GenerationParticles;
namespace {
GP::Image picture(bool transparent = false) {
    GP::Image im; im.width=100; im.height=100; im.rgba.resize(40000,255);
    for (int y=0;y<100;++y) for (int x=0;x<100;++x) {
        const bool subject=x>=25 && x<75 && y>=15 && y<85;
        for (int c=0;c<3;++c) im.rgba[4*(y*100+x)+c]=subject?60:255;
        im.rgba[4*(y*100+x)+3]=transparent && !subject?0:255;
    }
    return im;
}
std::string digest(const std::string& bytes) {
    unsigned char value[32]; unsigned int size=0;
    EVP_Digest(bytes.data(),bytes.size(),value,&size,EVP_sha256(),nullptr);
    std::string result; const char* hex="0123456789abcdef";
    for (auto c:value) { result+=hex[c>>4]; result+=hex[c&15]; }
    return result;
}
std::string png_bytes(bool transparent = true) {
    wxImage image(100,100); const auto im=picture(transparent); image.InitAlpha();
    for(int i=0;i<10000;++i) {
        for(int c=0;c<3;++c) image.GetData()[3*i+c]=im.rgba[4*i+c];
        image.GetAlpha()[i]=im.rgba[4*i+3];
    }
    wxMemoryOutputStream stream; wxPNGHandler handler;
    if (!handler.SaveFile(&image,stream)) return {};
    std::string bytes(stream.GetSize(),'\0'); stream.CopyTo(bytes.data(),bytes.size()); return bytes;
}
void write_bytes(const boost::filesystem::path& path,const std::string& bytes) {
    boost::filesystem::ofstream stream(path,std::ios::binary); stream.write(bytes.data(),bytes.size());
}
}

TEST_CASE("A subject has filled particles and bounded proportions for opaque and transparent references", "[GenerationParticles]") {
    const bool alpha=GENERATE(false,true);
    const auto shape=GP::prepare(picture(alpha)); REQUIRE(shape);
    CHECK(shape->points.size()==8000);
    int interior=0;
    for (const auto& p:shape->points) {
        CHECK(std::isfinite(p.x)); CHECK(std::isfinite(p.y));
        CHECK(std::abs(p.x)<0.37f); CHECK(std::abs(p.y)<0.51f);
        interior+=std::abs(p.x)<0.1f && std::abs(p.y)<0.1f;
    }
    CHECK(interior>100);
}
TEST_CASE("Light subjects on a contrasting background retain interior points", "[GenerationParticles]") {
    auto im=picture();
    for (int i=0;i<10000;++i) for (int c=0;c<3;++c) im.rgba[4*i+c]=255-im.rgba[4*i+c];
    REQUIRE(GP::prepare(im));
}
TEST_CASE("Empty flat and rectangular foregrounds decline a misleading particle shape", "[GenerationParticles]") {
    auto im=picture(); const auto value=GENERATE(0,255);
    for (int i=0;i<10000;++i) for (int c=0;c<3;++c) im.rgba[4*i+c]=value;
    CHECK_FALSE(GP::prepare(im));
    CHECK_FALSE(GP::prepare({}));
    GP::Image empty_logo; empty_logo.width=empty_logo.height=8; empty_logo.rgba.resize(8*8*4,0);
    CHECK_FALSE(GP::prepare(empty_logo,true));
}
TEST_CASE("Rounded corner transparency does not authorize the image rectangle as a subject", "[GenerationParticles]") {
    auto im=picture();
    for (int y=0;y<5;++y) for(int x=0;x<5;++x) im.rgba[4*(y*100+x)+3]=0;
    REQUIRE(GP::prepare(im));
}
TEST_CASE("Uncertain colour backgrounds decline while reliable alpha retains separated parts", "[GenerationParticles]") {
    auto im=picture();
    for(int x=0;x<100;++x) for(int c=0;c<3;++c) im.rgba[4*x+c]=x%2?0:255;
    CHECK_FALSE(GP::prepare(im));
    im=picture(true);
    for(int y=15;y<85;++y) for(int x=47;x<53;++x) im.rgba[4*(y*100+x)+3]=0;
    // A trustworthy alpha may contain disconnected parts of one subject.
    CHECK(GP::prepare(im));
    REQUIRE(GP::prepare(im,true));
}
TEST_CASE("A clipped edge with several foreground tones is not a background texture veto", "[GenerationParticles]") {
    auto im=picture();
    for(int y=60;y<100;++y) for(int x=25;x<75;++x)
        for(int c=0;c<3;++c) im.rgba[4*(y*100+x)+c]=((x-25)%20<10)?60:255;
    REQUIRE(GP::prepare(im));
}
TEST_CASE("Explicit masks preserve holes and disconnected cropped parts across categories", "[GenerationParticles]") {
    auto im=picture(); GP::SubjectMask mask{100,100,std::vector<unsigned char>(10000,0)};
    for(int y=0;y<100;++y) for(int x=15;x<85;++x)
        if(x<40 || x>60) mask.alpha[y*100+x]=255;
    const auto shape=GP::prepare(im,false,{},8000,&mask); REQUIRE(shape);
    for(const auto& p:shape->points) CHECK(std::abs(p.x)>0.09f);
    mask.width=99; CHECK_FALSE(GP::prepare(im,false,{},8000,&mask));
    mask.width=100; mask.alpha.resize(1); CHECK_FALSE(GP::prepare(im,false,{},8000,&mask));
    mask.alpha.assign(10000,255); CHECK_FALSE(GP::prepare(im,false,{},8000,&mask));
    mask.alpha.assign(10000,0); CHECK_FALSE(GP::prepare(im,false,{},8000,&mask));
}
TEST_CASE("Verified alpha avoids inference while opaque images share the foreground provider", "[GenerationParticles]") {
    ScopedTemporaryFile file(".png");
    ModelGenerationVisualReference source{"job","v",file.string(),{},true};
    int calls=0; GP::PreparationDetails report;
    const GP::ForegroundProvider provider=[&](const GP::Image& image,const auto&) {
        ++calls; GP::MaskResult result; result.reason="ready";
        result.mask={image.width,image.height,std::vector<unsigned char>(image.width*image.height,0)};
        for(int y=20;y<80;++y) for(int x=20;x<80;++x) result.mask.alpha[y*100+x]=255;
        return result;
    };
    write_bytes(file.path(),png_bytes());
    REQUIRE(GP::load(source,false,{},provider,&report)); CHECK(calls==0); CHECK(report.method=="image_alpha");
    write_bytes(file.path(),png_bytes(false));
    REQUIRE(GP::load(source,false,{},provider,&report)); CHECK(calls==1); CHECK(report.method=="local_foreground");
    const GP::ForegroundProvider invalid=[](const auto&,const auto&) {
        return GP::MaskResult{{100,100,std::vector<unsigned char>(10000,255)},"ready"};
    };
    CHECK_FALSE(GP::load(source,false,{},invalid,&report)); CHECK(report.reason=="invalid_mask");
    const GP::ForegroundProvider missing=[](const auto&,const auto&) { return GP::MaskResult{{},"runtime_unavailable"}; };
    REQUIRE(GP::load(source,false,{},missing,&report)); CHECK(report.method=="plain_background");
    bool canceled=false;
    const GP::ForegroundProvider late=[&](const auto&,const auto&) { canceled=true; return GP::MaskResult{}; };
    CHECK_FALSE(GP::load(source,false,[&] { return canceled; },late,&report)); CHECK(report.reason=="canceled");
}
TEST_CASE("Foreground inference declines missing resources invalid input and canceled work", "[GenerationParticles]") {
    ScopedTemporaryDir directory("foreground-missing"); const auto im=picture();
    CHECK(GP::infer_foreground(im,directory.path(),{}).mask.alpha.empty());
    CHECK(GP::infer_foreground({},directory.path(),{}).reason=="invalid_image");
    CHECK(GP::infer_foreground(im,directory.path(),[]{return true;}).reason=="canceled");
}
#ifdef ORCA_TEST_GENERATION_FOREGROUND_RUNTIME
TEST_CASE("Foreground runtime corruption cannot load an unverified native library", "[GenerationParticles][GenerationForeground]") {
    ScopedTemporaryDir directory("foreground-corrupt");
    const auto runtime=boost::filesystem::path(ORCA_TEST_GENERATION_FOREGROUND_RUNTIME);
    boost::filesystem::copy_file(runtime/"manifest.json",directory.path()/"manifest.json");
    write_bytes(directory.path()/"u2netp.onnx","corrupt model");
    CHECK(GP::infer_foreground(picture(),directory.path(),{}).reason=="runtime_integrity_or_missing");
    boost::filesystem::copy_file(runtime/"u2netp.onnx",directory.path()/"u2netp.onnx",boost::filesystem::copy_options::overwrite_existing);
    write_bytes(directory.path()/"onnxruntime.dll","corrupt native runtime");
    CHECK(GP::infer_foreground(picture(),directory.path(),{}).reason=="runtime_integrity_or_missing");
    write_bytes(directory.path()/"manifest.json","{}");
    CHECK(GP::infer_foreground(picture(),directory.path(),{}).reason=="runtime_integrity_or_missing");
}
TEST_CASE("Cancellation during native preparation leaves no usable partial foreground", "[GenerationParticles][GenerationForeground]") {
    // Warm the installed session first, so cancellation exercises an active
    // Run and its per-call termination flag rather than just model startup.
    REQUIRE(GP::infer_foreground(picture(),ORCA_TEST_GENERATION_FOREGROUND_RUNTIME,{}).reason=="ready");
    std::atomic<bool> canceled{false};
    std::thread canceler([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); canceled=true; });
    const auto result=GP::infer_foreground(picture(),ORCA_TEST_GENERATION_FOREGROUND_RUNTIME,[&] { return canceled.load(); });
    canceler.join();
    CHECK(result.mask.alpha.empty());
    CHECK(result.reason.find("canceled")!=std::string::npos);
    CHECK(GP::infer_foreground(picture(),ORCA_TEST_GENERATION_FOREGROUND_RUNTIME,{}).reason=="ready");
}
TEST_CASE("The shipped CPU foreground model supplies particles for different object categories", "[GenerationParticles][GenerationForeground]") {
    const auto category=GENERATE("sculpture","realistic","cartoon","diorama","relief","low_poly");
    const auto root=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path();
    ModelGenerationVisualReference source{"job","v",(root/"resources/images/smart_creation"/(std::string(category)+".png")).string(),{},true};
    GP::PreparationDetails report;
    const auto shape=GP::load(source,false,{},[](const auto& image,const auto& canceled) {
        return GP::infer_foreground(image,ORCA_TEST_GENERATION_FOREGROUND_RUNTIME,canceled);
    },&report);
    INFO(category); INFO(report.method); INFO(report.reason); REQUIRE(shape);
    CHECK(report.method=="local_foreground"); CHECK(shape->points.size()==8000);
}
TEST_CASE("A native foreground probe exports the actual mask and sampled points", "[.GenerationForegroundProbe]") {
    const auto* source_path=std::getenv("ORCA_FOREGROUND_PROBE_IMAGE");
    const auto* output=std::getenv("ORCA_FOREGROUND_PROBE_OUTPUT");
    REQUIRE(source_path); REQUIRE(output);
    ModelGenerationVisualReference source{"probe","v",source_path,{},true};
    GP::PreparationDetails report;
    double inference_ms=0;
    const auto shape=GP::load(source,false,{},[&](const auto& image,const auto& canceled) {
        const auto begin=std::chrono::steady_clock::now();
        auto result=GP::infer_foreground(image,ORCA_TEST_GENERATION_FOREGROUND_RUNTIME,canceled);
        inference_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        if (!result.mask.alpha.empty()) {
            wxImage mask(image.width,image.height);
            for(std::size_t i=0;i<result.mask.alpha.size();++i)
                for(int c=0;c<3;++c) mask.GetData()[3*i+c]=result.mask.alpha[i];
            wxPNGHandler handler; wxFileOutputStream out(wxString::FromUTF8(output)+".png");
            handler.SaveFile(&mask,out);
        }
        return result;
    },&report);
    nlohmann::json json{{"method",report.method},{"reason",report.reason},{"inference_ms",inference_ms},{"points",nlohmann::json::array()}};
    if (shape) for(const auto& p:shape->points) json["points"].push_back({p.x,p.y,p.z,p.intensity});
    write_bytes(boost::filesystem::path(wxString::FromUTF8(output).ToStdWstring()+L".json"),json.dump());
    INFO(report.method); INFO(report.reason); REQUIRE(shape);
}
#endif
TEST_CASE("A trusted logo retains its holes and disconnected marks", "[GenerationParticles]") {
    auto im=picture(true);
    for(int y=35;y<65;++y) for(int x=40;x<60;++x) im.rgba[4*(y*100+x)+3]=0;
    const auto shape=GP::prepare(im,true); REQUIRE(shape);
    for(const auto& p:shape->points) {
        const bool in_hole=std::abs(p.x)<0.12f && std::abs(p.y)<0.19f;
        CHECK_FALSE(in_hole);
    }
}
TEST_CASE("Sampling is bounded cancelable and stable across progress updates", "[GenerationParticles]") {
    const auto im=picture(true);
    const auto a=GP::prepare(im,false,{},50000), b=GP::prepare(im,false,{},50000);
    REQUIRE(a); REQUIRE(b); REQUIRE(a->points.size()==12000);
    for(std::size_t i=0;i<a->points.size();++i) {
        CHECK_THAT(a->points[i].x, Catch::Matchers::WithinAbs(b->points[i].x, 1e-7));
        CHECK_THAT(a->points[i].y, Catch::Matchers::WithinAbs(b->points[i].y, 1e-7));
    }
    int checks=0;
    CHECK_FALSE(GP::prepare(im,false,[&]{return ++checks>2;}));
}
TEST_CASE("Particle motion preserves a recognizable shape at long presentation times", "[GenerationParticles]") {
    const auto shape=GP::prepare(picture()); REQUIRE(shape);
    for(const auto& p:shape->points) {
        const auto moved=GP::sample(p,100000);
        CHECK(std::abs(moved.x-p.x)<0.020f); CHECK(std::abs(moved.y-p.y)<0.015f);
        CHECK(std::isfinite(moved.intensity));
    }
}
TEST_CASE("A cropped subject retains a plain background without including the image rectangle", "[GenerationParticles]") {
    auto im=picture();
    for(int y=0;y<100;++y) for(int x=0;x<100;++x) {
        const bool subject=y>=15 && x>=20 && x<80;
        for(int c=0;c<3;++c) im.rgba[4*(y*100+x)+c]=subject ? (c==0?210:160) : 124+y/25;
    }
    const auto shape=GP::prepare(im); REQUIRE(shape);
    for(const auto& p:shape->points) {
        CHECK(std::abs(p.x)<0.36f);
        CHECK(std::abs(p.y)<0.51f);
    }
    // An image filled to several edges is still uncertain, even with a
    // dominant background colour. It must not become a rectangular point set.
    for(int y=0;y<100;++y) for(int x=25;x<75;++x)
        for(int c=0;c<3;++c) im.rgba[4*(y*100+x)+c]=60;
    CHECK_FALSE(GP::prepare(im));
}
TEST_CASE("Particles circulate visibly within a stable silhouette", "[GenerationParticles]") {
    const auto shape=GP::prepare(picture()); REQUIRE(shape);
    int moving=0, shimmering=0;
    for(const auto& p:shape->points) {
        const auto a=GP::sample(p,0), b=GP::sample(p,1);
        moving+=std::hypot(a.x-b.x,a.y-b.y)>0.006f;
        shimmering+=std::abs(a.intensity-b.intensity)>p.intensity*0.15f;
    }
    CHECK(moving>int(shape->points.size()/2));
    CHECK(shimmering>int(shape->points.size()/3));
}
TEST_CASE("Business scheduling delays cannot switch working GL particles to CPU or static", "[GenerationParticles]") {
    GP::QualityPolicy quality;
    CHECK_FALSE(quality.observe(85,1));
    CHECK_FALSE(quality.observe(33,1));
    CHECK(quality.quality()==GP::Quality::Normal);
    CHECK_FALSE(quality.observe(70,1));
    CHECK(quality.observe(70,1));
    CHECK(quality.quality()==GP::Quality::Low);
    for(int i=0;i<20;++i) CHECK_FALSE(quality.observe(100,0.1));
    CHECK(quality.quality()==GP::Quality::Low);
    for(int i=0;i<3;++i) CHECK_FALSE(quality.observe(33,0.1));
    CHECK(quality.observe(33,0.1));
    CHECK(quality.quality()==GP::Quality::Normal);
}
TEST_CASE("Only sustained particle drawing cost can select static quality", "[GenerationParticles]") {
    GP::QualityPolicy quality;
    CHECK_FALSE(quality.observe(33,12));
    CHECK(quality.observe(33,12));
    CHECK(quality.quality()==GP::Quality::Low);
    CHECK_FALSE(quality.observe(33,12));
    CHECK(quality.observe(33,12));
    CHECK(quality.quality()==GP::Quality::Static);
    quality.require_cpu();
    CHECK(quality.quality()==GP::Quality::Static);
    CHECK_FALSE(quality.observe(100,100));
}
TEST_CASE("A new logical task resets rendering quality without retrying a failed GL backend", "[GenerationParticles]") {
    GP::QualityPolicy quality;
    quality.require_cpu();
    CHECK_FALSE(quality.observe(100,12));
    CHECK(quality.observe(100,12));
    REQUIRE(quality.quality()==GP::Quality::Static);
    quality.reset(true);
    CHECK(quality.quality()==GP::Quality::Cpu);
    quality.reset();
    CHECK(quality.quality()==GP::Quality::Normal);
}
TEST_CASE("A particle GL failure selects CPU rendering without repeated creation attempts", "[GenerationParticles]") {
    GP::QualityPolicy quality;
    quality.require_cpu();
    CHECK(quality.quality()==GP::Quality::Cpu);
    for (int i=0;i<5;++i) CHECK_FALSE(quality.observe(33,2));
    CHECK(quality.quality()==GP::Quality::Cpu);
}
TEST_CASE("Visual input availability participates in snapshot content equality", "[GenerationParticles]") {
    ModelGenerationUIState state; auto other=state;
    other.visual_reference={"job","v1","front.image","hash",false};
    CHECK_FALSE(state.same_content(other));
    state=other; other.visual_reference.ready=true; CHECK_FALSE(state.same_content(other));
    state=other; other.revision++; CHECK(state.same_content(other));
}
TEST_CASE("Only the current flow or job authorizes the waiting image", "[GenerationParticles]") {
    ModelGenerationUIState state; state.smart_mode=true; state.smart_flow_id="flow"; state.job_id="design";
    state.visual_reference={"old-flow","v","reference.png",{},true};
    CHECK_FALSE(GP::usable_reference(state));
    state.visual_reference.owner_id="flow"; CHECK(GP::usable_reference(state));
    state.visual_reference.ready=false; CHECK_FALSE(GP::usable_reference(state));
}
TEST_CASE("Model download and preview generation cannot restart the logical particle task", "[GenerationParticles]") {
    ModelGenerationUIState state; state.smart_mode=true; state.smart_flow_id="flow"; state.job_id="design";
    state.model_generation_session=1;
    GenerationBinding binding{CreationSource::Smart,"",0};
    const auto key=GP::task_key(binding,state);
    state.job_id="model"; state.stage=ModelGenerationUIStage::LoadingModel; state.revision+=8;
    CHECK(GP::task_key(binding,state)==key);
    ++state.model_generation_session; CHECK(GP::task_key(binding,state)==key);
    --state.model_generation_session; ++binding.epoch; CHECK(GP::task_key(binding,state)!=key);
    --binding.epoch; state.smart_flow_id="another-flow"; CHECK(GP::task_key(binding,state)!=key);
}
TEST_CASE("An advanced input becomes ready without restarting the observed job", "[GenerationParticles]") {
    int queries=0,cancels=0;
    AdvancedModelTaskFlow::Ports ports;
    ports.status=[&](const auto&,auto,auto){++queries;};
    ports.cancel=[&]{++cancels;}; ports.schedule=[](int){};
    AdvancedModelTaskFlow flow(std::move(ports)); flow.set_available(true);
    ModelGenerationVisualReference source{"job","front-v1","front.image","hash",false};
    flow.observe("job","front.image",false,source);
    const auto session=flow.snapshot().model_generation_session;
    const auto initial_cancels=cancels;
    source.ready=true; flow.observe("job","front.image",false,source);
    CHECK(flow.snapshot().visual_reference.ready);
    CHECK(flow.snapshot().model_generation_session==session); CHECK(queries==1); CHECK(cancels==initial_cancels);
    source.version="front-v2"; source.path="another.image";
    flow.observe("job","another.image",false,source);
    CHECK(flow.snapshot().visual_reference.version=="front-v1");
    CHECK(flow.snapshot().visual_reference.path=="front.image");
}
TEST_CASE("A verified cached image is decoded by content and refuses a changed digest", "[GenerationParticles]") {
    wxInitializer initialized; REQUIRE(initialized.IsOk());
    ScopedTemporaryFile file(".image");
    wxImage image(100,100); auto im=picture(true); image.InitAlpha();
    for(int i=0;i<10000;++i) { for(int c=0;c<3;++c) image.GetData()[3*i+c]=im.rgba[4*i+c]; image.GetAlpha()[i]=im.rgba[4*i+3]; }
    wxPNGHandler handler; wxFileOutputStream stream(file.path().wstring());
    REQUIRE(handler.SaveFile(&image,stream)); stream.Close();
    ModelGenerationVisualReference source{"job","v",file.path().string(),{},true};
    REQUIRE(GP::load(source,false,{}));
    source.sha256=std::string(64,'0'); CHECK_FALSE(GP::load(source,false,{}));
    source.sha256.clear(); CHECK_FALSE(GP::load(source,false,[]{return true;}));
}
TEST_CASE("A frozen advanced front is verified against its original job receipt", "[GenerationParticles]") {
    wxInitializer initialized; REQUIRE(initialized.IsOk());
    ScopedTemporaryDir directory("particle-confirmed-front");
    boost::filesystem::create_directories(directory.path()/"inputs");
    const auto path=directory.path()/"inputs/front.png", manifest=directory.path()/"job.json";
    const auto bytes=png_bytes(); REQUIRE_FALSE(bytes.empty()); write_bytes(path,bytes);
    nlohmann::json job={{"id","job-a"},{"creation_kind","advanced_multiview_v1"},
        {"approved_multiview_snapshot",{{"schema","approved-multiview-input-v1"},{"snapshot_id","job-a"},
            {"views",{{"front",{{"version_id","frozen-v1"},{"path","inputs/front.png"},{"sha256",digest(bytes)}}}}}}}};
    write_bytes(manifest,job.dump());
    ModelGenerationVisualReference source{"job-a","confirmed-front:job-a",path.string(),{},true};
    source.verification_manifest=manifest.string();
    REQUIRE(GP::load(source,false,{}));
    source.owner_id="job-b"; CHECK_FALSE(GP::load(source,false,{}));
    source.owner_id="job-a"; job["approved_multiview_snapshot"]["views"]["front"]["sha256"]=std::string(64,'0');
    write_bytes(manifest,job.dump()); CHECK_FALSE(GP::load(source,false,{}));
    job["approved_multiview_snapshot"]["views"]["front"]["sha256"]=digest(bytes);
    job["approved_multiview_snapshot"]["views"]["front"]["path"]="../another/front.png";
    write_bytes(manifest,job.dump()); CHECK_FALSE(GP::load(source,false,{}));
}
TEST_CASE("Missing corrupt and oversized image sources decline without writing them", "[GenerationParticles]") {
    ScopedTemporaryFile file(".image");
    ModelGenerationVisualReference source{"job","v",file.string(),{},true};
    CHECK_FALSE(GP::load(source,false,{}));
    write_bytes(file.path(),"invalid image"); CHECK_FALSE(GP::load(source,false,{}));
    boost::filesystem::resize_file(file.path(),MAX_INPUT_IMAGE_BYTES+1);
    CHECK_FALSE(GP::load(source,false,{}));
    CHECK(boost::filesystem::file_size(file.path())==MAX_INPUT_IMAGE_BYTES+1);
}
TEST_CASE("The bundled logo keeps its ring hole when sampled as white particles", "[GenerationParticles]") {
    const auto path=boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path()/"resources/images/generation_wait_logo.png";
    ModelGenerationVisualReference source; source.path=wxString(path.wstring()).ToUTF8().data();
    const auto shape=GP::load(source,true,{}); REQUIRE(shape);
    CHECK(shape->points.size()==8000);
    for(const auto& p:shape->points) CHECK(p.x*p.x+p.y*p.y>0.25f*0.25f);
}
TEST_CASE("JPEG particles honor camera orientation and Unicode paths without rewriting bytes", "[GenerationParticles]") {
    ScopedTemporaryDir directory("particle-jpeg");
    const auto path=directory.path()/boost::filesystem::path(L"中文 空格.jpg");
    wxImage image(100,100); const auto im=picture();
    for(int i=0;i<10000;++i) for(int c=0;c<3;++c) image.GetData()[3*i+c]=im.rgba[4*i+c];
    wxMemoryOutputStream stream; wxJPEGHandler handler; REQUIRE(handler.SaveFile(&image,stream));
    std::string bytes(stream.GetSize(),'\0'); stream.CopyTo(bytes.data(),bytes.size());
    const unsigned char exif[]={255,225,0,34,'E','x','i','f',0,0,'I','I',42,0,8,0,0,0,1,0,18,1,3,0,1,0,0,0,6,0,0,0,0,0,0,0};
    bytes.insert(2,reinterpret_cast<const char*>(exif),sizeof(exif)); write_bytes(path,bytes);
    ModelGenerationVisualReference source{"job","v",wxString(path.wstring()).ToUTF8().data(),digest(bytes),true};
    const auto shape=GP::load(source,false,{}); REQUIRE(shape);
    float x=0,y=0; for(const auto& p:shape->points) { x=std::max(x,std::abs(p.x)); y=std::max(y,std::abs(p.y)); }
    CHECK(x>y);
    boost::filesystem::ifstream after(path,std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(after)),{})==bytes); after.close();
    bytes.resize(bytes.size()-2); write_bytes(path,bytes); source.sha256=digest(bytes);
    CHECK_FALSE(GP::load(source,false,{}));
}
