#include "GenerationForeground.hpp"
#include <wx/image.h>
#include <wx/dynlib.h>
#include <wx/log.h>
#include <boost/filesystem/fstream.hpp>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef ORCA_GENERATION_FOREGROUND_NATIVE
#include <onnxruntime_c_api.h>
#endif

namespace Slic3r::GUI::GenerationParticles {
namespace {
#ifdef ORCA_GENERATION_FOREGROUND_NATIVE
using Clock=std::chrono::steady_clock;
std::string file_digest(const boost::filesystem::path& path) {
    boost::system::error_code error;
    const auto file_size=boost::filesystem::file_size(path,error);
    if (error || !file_size || file_size>64*1024*1024) return {};
    boost::filesystem::ifstream in(path,std::ios::binary);
    if (!in) return {};
    auto context=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1) return {};
    char buffer[65536];
    while (in.read(buffer,sizeof(buffer)) || in.gcount())
        if (EVP_DigestUpdate(context.get(),buffer,std::size_t(in.gcount()))!=1) return {};
    if (!in.eof()) return {};
    unsigned char hash[32]; unsigned int size=0;
    if (EVP_DigestFinal_ex(context.get(),hash,&size)!=1 || size!=32) return {};
    constexpr char hex[]="0123456789abcdef"; std::string result;
    for (const auto c:hash) { result+=hex[c>>4]; result+=hex[c&15]; }
    return result;
}
struct Runtime {
    wxDynamicLibrary library;
    const OrtApi* api {nullptr}; OrtEnv* env {nullptr}; OrtSession* session {nullptr};
    ~Runtime() {
        if (session) api->ReleaseSession(session);
        if (env) api->ReleaseEnv(env);
    }
    void check(OrtStatus* status) {
        if (!status) return;
        api->ReleaseStatus(status);
        throw std::runtime_error("foreground_inference_failed");
    }
    bool initialize(const boost::filesystem::path& root) {
        boost::system::error_code error;
        const auto manifest=root/"manifest.json";
        const auto size=boost::filesystem::file_size(manifest,error);
        if (error || !size || size>16384) return false;
        boost::filesystem::ifstream in(manifest); nlohmann::json receipt;
        try { in>>receipt; } catch (...) { return false; }
        if (receipt.value("schema","")!="generation-foreground-v1" ||
            receipt.value("model_sha256","")!="309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8") return false;
        const auto model=root/"u2netp.onnx";
        if (file_digest(model)!=receipt.value("model_sha256","")) return false;
        const auto native=root/"onnxruntime.dll";
        const auto native_hash=receipt.value("runtime_sha256","");
        if (native_hash!=ORCA_FOREGROUND_DLL_SHA256 || file_digest(native)!=native_hash) return false;
        wxLogNull quiet;
        if (!library.Load(wxString(native.wstring()),wxDL_NOW|wxDL_VERBATIM)) return false;
        using GetApi=const OrtApiBase* (ORT_API_CALL*)();
        const auto get=reinterpret_cast<GetApi>(library.GetSymbol("OrtGetApiBase"));
        if (!get || !(api=get()->GetApi(ORT_API_VERSION))) return false;
        check(api->CreateEnv(ORT_LOGGING_LEVEL_ERROR,"generation_foreground",&env));
        OrtSessionOptions* options=nullptr; check(api->CreateSessionOptions(&options));
        const auto release=[&] { api->ReleaseSessionOptions(options); };
        try {
            check(api->SetIntraOpNumThreads(options,2)); check(api->SetInterOpNumThreads(options,1));
            check(api->SetSessionExecutionMode(options,ORT_SEQUENTIAL));
            check(api->SetSessionGraphOptimizationLevel(options,ORT_ENABLE_ALL));
            check(api->CreateSession(env,model.c_str(),options,&session));
        } catch (...) { release(); throw; }
        release(); return true;
    }
};
std::mutex runtime_mutex;
std::shared_ptr<Runtime> installed_runtime;
boost::filesystem::path installed_root;
#endif
}
MaskResult infer_foreground(const Image& image, const boost::filesystem::path& root,
    const std::function<bool()>& canceled)
{
    MaskResult result;
    const auto stopped=[&] { return canceled && canceled(); };
    if (stopped()) { result.reason="canceled"; return result; }
    if (image.width<8 || image.height<8 || image.width>512 || image.height>512 ||
        image.rgba.size()!=std::size_t(image.width)*image.height*4) { result.reason="invalid_image"; return result; }
#ifndef ORCA_GENERATION_FOREGROUND_NATIVE
    result.reason="runtime_unavailable"; return result;
#else
    // One cached CPU session and one active inference. Waiting workers remain
    // cancelable and never stall the UI or duplicate model/thread pools.
    const auto deadline=Clock::now()+std::chrono::seconds(5);
    std::unique_lock<std::mutex> lock(runtime_mutex,std::defer_lock);
    while (!lock.try_lock()) {
        if (stopped() || Clock::now()>deadline) { result.reason="canceled_or_busy"; return result; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    try {
        if (!installed_runtime || installed_root!=root) {
            auto runtime=std::make_shared<Runtime>();
            if (!runtime->initialize(root)) { result.reason="runtime_integrity_or_missing"; return result; }
            installed_runtime=std::move(runtime); installed_root=root;
        }
        if (stopped()) { result.reason="canceled"; return result; }
        if (Clock::now()>deadline) { result.reason="canceled_or_timeout"; return result; }
        auto& runtime=*installed_runtime; const auto* api=runtime.api;
        wxImage rgb(image.width,image.height);
        unsigned char minimum=255,maximum=0;
        for (std::size_t i=0;i<image.rgba.size()/4;++i) for (int c=0;c<3;++c) {
            const auto v=image.rgba[4*i+c]; rgb.GetData()[3*i+c]=v;
            minimum=std::min(minimum,v); maximum=std::max(maximum,v);
        }
        if (maximum-minimum<8) { result.reason="flat_image"; return result; }
        rgb.Rescale(320,320,wxIMAGE_QUALITY_BILINEAR);
        std::vector<float> input(3*320*320);
        const float mean[]={0.485f,0.456f,0.406f},deviation[]={0.229f,0.224f,0.225f};
        maximum=1;
        for (int i=0;i<3*320*320;++i) maximum=std::max(maximum,rgb.GetData()[i]);
        for (int i=0;i<320*320;++i) for (int c=0;c<3;++c)
            input[c*320*320+i]=(rgb.GetData()[3*i+c]/float(maximum)-mean[c])/deviation[c];
        OrtMemoryInfo* memory=nullptr; OrtValue *tensor=nullptr,*output=nullptr; OrtRunOptions* options=nullptr;
        const auto release=[&] {
            if (output) api->ReleaseValue(output);
            if (tensor) api->ReleaseValue(tensor);
            if (memory) api->ReleaseMemoryInfo(memory);
            if (options) api->ReleaseRunOptions(options);
        };
        try {
            runtime.check(api->CreateCpuMemoryInfo(OrtArenaAllocator,OrtMemTypeDefault,&memory));
            const std::int64_t dimensions[]={1,3,320,320};
            runtime.check(api->CreateTensorWithDataAsOrtValue(memory,input.data(),input.size()*sizeof(float),
                dimensions,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&tensor));
            runtime.check(api->CreateRunOptions(&options));
            std::mutex watch_mutex; std::condition_variable wake; bool finished=false;
            std::atomic<bool> interrupted {false};
            std::thread watcher([&] {
                std::unique_lock<std::mutex> guard(watch_mutex);
                while (!wake.wait_for(guard,std::chrono::milliseconds(10),[&] { return finished; }))
                    if (stopped() || Clock::now()>deadline) {
                        interrupted=true;
                        if (auto* status=api->RunOptionsSetTerminate(options)) api->ReleaseStatus(status);
                        break;
                    }
            });
            const char* inputs[]={"input.1"}; const char* outputs[]={"1959"};
            const OrtValue* input_tensor=tensor;
            auto* status=api->Run(runtime.session,options,inputs,&input_tensor,1,outputs,1,&output);
            { std::lock_guard<std::mutex> guard(watch_mutex); finished=true; }
            wake.notify_one(); watcher.join();
            if (interrupted || stopped()) {
                if (status) api->ReleaseStatus(status);
                release(); result.reason="canceled_or_timeout"; return result;
            }
            runtime.check(status);
            OrtTensorTypeAndShapeInfo* shape=nullptr;
            runtime.check(api->GetTensorTypeAndShape(output,&shape));
            std::size_t elements=0; auto* shape_status=api->GetTensorShapeElementCount(shape,&elements);
            api->ReleaseTensorTypeAndShapeInfo(shape); runtime.check(shape_status);
            if (elements!=320*320) { release(); result.reason="invalid_output_shape"; return result; }
            float* values=nullptr; runtime.check(api->GetTensorMutableData(output,reinterpret_cast<void**>(&values)));
            float low=1.f,high=0.f;
            for (std::size_t i=0;i<elements;++i) {
                if (!std::isfinite(values[i])) { release(); result.reason="invalid_output_values"; return result; }
                low=std::min(low,values[i]); high=std::max(high,values[i]);
            }
            if (high-low<0.25f) { release(); result.reason="uncertain_output"; return result; }
            wxImage mask(320,320);
            for (std::size_t i=0;i<elements;++i) {
                const auto alpha=static_cast<unsigned char>(std::clamp((values[i]-low)/(high-low),0.f,1.f)*255.f);
                for (int c=0;c<3;++c) mask.GetData()[3*i+c]=alpha;
            }
            mask.Rescale(image.width,image.height,wxIMAGE_QUALITY_BILINEAR);
            result.mask.width=image.width; result.mask.height=image.height;
            result.mask.alpha.resize(std::size_t(image.width)*image.height);
            for (std::size_t i=0;i<result.mask.alpha.size();++i) result.mask.alpha[i]=mask.GetData()[3*i];
            release(); result.reason="ready"; return result;
        } catch (...) { release(); throw; }
    } catch (...) { result.reason="inference_failed"; return result; }
#endif
}
}
