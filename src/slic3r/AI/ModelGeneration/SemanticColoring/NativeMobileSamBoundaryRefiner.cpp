#include "NativeMobileSamBoundaryRefiner.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#if defined(_WIN32) && defined(ORCA_ENABLE_MOBILESAM_NATIVE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include "onnxruntime_c_api.h"
#endif

namespace Slic3r::AI::SemanticColoring {
namespace {
struct Cleanup { std::function<void()> fn; ~Cleanup() { if (fn) fn(); } };
// Pillow's separable RGB8 bilinear resize uses 22-bit normalized coefficients
// and rounds each uint8 pass. Matching it matters for small ear/eye crops.
struct Filter { int first; std::vector<int> weights; };
std::vector<Filter> filters(int input, int output)
{
    std::vector<Filter> result; result.reserve(output);
    const double scale = double(input) / output, support = std::max(1., scale);
    for (int x = 0; x < output; ++x) {
        const double center = (x + .5) * scale;
        const int first = std::max(0, int(center - support + .5));
        const int last = std::min(input, int(center + support + .5));
        std::vector<double> coeff; double sum = 0.;
        for (int i = first; i < last; ++i) { const double v = std::max(0., 1. - std::abs((i - center + .5) / support)); coeff.push_back(v); sum += v; }
        Filter f {first, {}};
        for (double v : coeff) f.weights.push_back(int(v / sum * (1 << 22) + .5));
        result.push_back(std::move(f));
    }
    return result;
}
std::vector<uint8_t> resize_rgb(const RGBImage& input, int width, int height)
{
    const auto horizontal = filters(input.width, width), vertical = filters(input.height, height);
    std::vector<uint8_t> intermediate(size_t(width) * input.height * 3), result(size_t(width) * height * 3);
    for (int y = 0; y < input.height; ++y) for (int x = 0; x < width; ++x) for (int c = 0; c < 3; ++c) {
        const auto& f = horizontal[x]; int value = 1 << 21;
        for (size_t i = 0; i < f.weights.size(); ++i) value += input.pixels[(size_t(y) * input.width + f.first + i) * 3 + c] * f.weights[i];
        intermediate[(size_t(y) * width + x) * 3 + c] = uint8_t(std::clamp(value >> 22, 0, 255));
    }
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) for (int c = 0; c < 3; ++c) {
        const auto& f = vertical[y]; int value = 1 << 21;
        for (size_t i = 0; i < f.weights.size(); ++i) value += intermediate[(size_t(f.first + i) * width + x) * 3 + c] * f.weights[i];
        result[(size_t(y) * width + x) * 3 + c] = uint8_t(std::clamp(value >> 22, 0, 255));
    }
    return result;
}
}

std::vector<float> mobile_sam_image_tensor(const RGBImage& crop, int& width, int& height)
{
    if (!crop.valid()) throw std::runtime_error("Invalid MobileSAM crop.");
    const double scale = 1024. / std::max(crop.width, crop.height);
    width = std::max(1, int(crop.width * scale + .5)); height = std::max(1, int(crop.height * scale + .5));
    const auto resized = resize_rgb(crop, width, height);
    std::vector<float> tensor(3 * 1024 * 1024, 0.f);
    constexpr float mean[3] {123.675f, 116.28f, 103.53f}, stddev[3] {58.395f, 57.12f, 57.375f};
    for (int c = 0; c < 3; ++c) for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        tensor[size_t(c) * 1024 * 1024 + size_t(y) * 1024 + x] = (resized[(size_t(y) * width + x) * 3 + c] - mean[c]) / stddev[c];
    return tensor;
}

#if defined(_WIN32) && defined(ORCA_ENABLE_MOBILESAM_NATIVE)
namespace {
constexpr const char* encoder_hash = "83398d336e1e95140df654a7be83ddb35b4c78221dd984f4f62d2a78d95ace32";
constexpr const char* decoder_hash = "43c7655a81b62c0f2b0e31d2bc91d3811b48d7f736ecf93b3a4a1254318241cf";
constexpr const char* runtime_hash = "c7151fd9844ad7c7d18525f1177e9ef62d91e4a6ac3583d0be700554a2b2b1d6";
void verify(const std::filesystem::path& path, const char* expected)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Missing boundary resource: " + path.filename().u8string());
    const auto length = stream.tellg();
    if (length <= 0 || length > 128 * 1024 * 1024) throw std::runtime_error("Invalid boundary resource size.");
    std::vector<unsigned char> bytes(static_cast<size_t>(length)); stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) throw std::runtime_error("Cannot read boundary resource.");
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("Cannot verify boundary SHA256.");
    Cleanup close_alg {[&] { BCryptCloseAlgorithmProvider(alg, 0); }};
    DWORD length_object = 0, written = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length_object), sizeof(length_object), &written, 0) < 0) throw std::runtime_error("Cannot create boundary SHA256.");
    std::vector<unsigned char> object(length_object); unsigned char digest[32] {};
    if (BCryptCreateHash(alg, &hash, object.data(), length_object, nullptr, 0, 0) < 0) throw std::runtime_error("Cannot create boundary SHA256.");
    Cleanup close_hash {[&] { BCryptDestroyHash(hash); }};
    if (BCryptHashData(hash, bytes.data(), ULONG(bytes.size()), 0) < 0 || BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("Cannot finish boundary SHA256.");
    std::ostringstream hex; for (auto c : digest) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    if (hex.str() != expected) throw std::runtime_error("Boundary checksum mismatch: " + path.filename().u8string());
}
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

class NativeMobileSam final : public IBoundaryRefiner {
    std::filesystem::path m_root;
    HMODULE m_dll {nullptr};
    const OrtApi* m_api {nullptr};
    OrtEnv* m_env {nullptr}; OrtSession* m_encoder {nullptr}; OrtSession* m_decoder {nullptr};
    OrtMemoryInfo* m_memory {nullptr};
    std::mutex m_mutex;
    RGBImage m_previous_crop;
    OrtValue* m_embedding {nullptr};
    void check(OrtStatus* status) const {
        if (!status) return;
        const std::string error = m_api->GetErrorMessage(status); m_api->ReleaseStatus(status); throw std::runtime_error(error);
    }
    void clear() {
        if (m_api) {
            if (m_embedding) m_api->ReleaseValue(m_embedding);
            if (m_encoder) m_api->ReleaseSession(m_encoder);
            if (m_decoder) m_api->ReleaseSession(m_decoder);
            if (m_memory) m_api->ReleaseMemoryInfo(m_memory);
            if (m_env) m_api->ReleaseEnv(m_env);
        }
        m_embedding = nullptr; m_encoder = nullptr; m_decoder = nullptr; m_memory = nullptr; m_env = nullptr;
        if (m_dll) FreeLibrary(m_dll); m_dll = nullptr;
        m_api = nullptr;
    }
    void initialize() {
        if (m_encoder && m_decoder) return;
        try {
            m_dll = LoadLibraryExW((m_root / "onnxruntime.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!m_dll) throw std::runtime_error("Cannot load boundary ONNX Runtime.");
            const auto entry = reinterpret_cast<decltype(&OrtGetApiBase)>(GetProcAddress(m_dll, "OrtGetApiBase"));
            if (!entry || std::string(entry()->GetVersionString()) != "1.22.1") throw std::runtime_error("Boundary ONNX Runtime version mismatch.");
            m_api = entry()->GetApi(ORT_API_VERSION);
            if (!m_api) throw std::runtime_error("Boundary ONNX C API is unavailable.");
            check(m_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "orca.local.boundary", &m_env));
            check(m_api->DisableTelemetryEvents(m_env));
            OrtSessionOptions* options = nullptr; check(m_api->CreateSessionOptions(&options));
            Cleanup release_options {[&] { m_api->ReleaseSessionOptions(options); }};
            check(m_api->SetIntraOpNumThreads(options, 4)); check(m_api->SetInterOpNumThreads(options, 1));
            check(m_api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
            check(m_api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
            check(m_api->CreateSession(m_env, (m_root / "mobile_sam_encoder.onnx").c_str(), options, &m_encoder));
            check(m_api->CreateSession(m_env, (m_root / "mobile_sam_decoder.onnx").c_str(), options, &m_decoder));
            check(m_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_memory));
        } catch (...) { clear(); throw; }
    }
    OrtValue* tensor(std::vector<float>& values, const std::vector<int64_t>& shape) {
        OrtValue* value = nullptr;
        check(m_api->CreateTensorWithDataAsOrtValue(m_memory, values.data(), values.size() * sizeof(float), shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value));
        return value;
    }
    void run(OrtSession* session, const char* const* names, const OrtValue* const* values, size_t count,
             const char* const* outputs, size_t output_count, OrtValue** result, const Cancel& cancel) {
        OrtRunOptions* options = nullptr; check(m_api->CreateRunOptions(&options));
        Cleanup release {[&] { m_api->ReleaseRunOptions(options); }};
        std::atomic<bool> done {false};
        std::thread watchdog([&] {
            while (!done.load()) {
                if (cancel && cancel()) { if (auto status = m_api->RunOptionsSetTerminate(options)) m_api->ReleaseStatus(status); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        Cleanup join {[&] { done.store(true); watchdog.join(); }};
        check(m_api->Run(session, options, names, values, count, outputs, output_count, result));
    }
    std::vector<int64_t> shape(OrtValue* value) {
        OrtTensorTypeAndShapeInfo* info = nullptr; check(m_api->GetTensorTypeAndShape(value, &info));
        Cleanup release {[&] { m_api->ReleaseTensorTypeAndShapeInfo(info); }};
        size_t count = 0; check(m_api->GetDimensionsCount(info, &count));
        std::vector<int64_t> dimensions(count); check(m_api->GetDimensions(info, dimensions.data(), count)); return dimensions;
    }
public:
    explicit NativeMobileSam(const std::filesystem::path& root) : m_root(std::filesystem::absolute(root / "boundary")) {
        verify(m_root / "onnxruntime.dll", runtime_hash); verify(m_root / "mobile_sam_encoder.onnx", encoder_hash); verify(m_root / "mobile_sam_decoder.onnx", decoder_hash);
    }
    ~NativeMobileSam() override { clear(); }
    std::string identity() const override {
        return std::string(mobile_sam_provider_id) + "/ort1.22.1/" + encoder_hash + "/" + decoder_hash + "/" + mobile_sam_preprocess_version + "/prompt-score-v2-rejection";
    }
    BoundaryRefinement refine(const RGBImage& image, const Prediction&, const BoundaryRefinementRequest& request, const Cancel& cancel) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        BoundaryRefinement out; out.width = image.width; out.height = image.height;
        if (!image.valid()) { out.error = "Invalid MobileSAM input image."; return out; }
        out.foreground_probability.assign(size_t(image.width) * image.height, std::numeric_limits<float>::quiet_NaN()); out.confidence.assign(out.foreground_probability.size(), 0.f);
        try {
            if (cancel && cancel()) { out.canceled = true; return out; }
            if (!image.valid() || request.regions.size() != 1 || !request.regions[0].valid_for(image)) throw std::runtime_error("MobileSAM requires one valid local ROI per request.");
            const auto& box = request.regions[0]; RGBImage crop; crop.width = box.right - box.left; crop.height = box.bottom - box.top;
            crop.pixels.resize(size_t(crop.width) * crop.height * 3);
            for (int y = 0; y < crop.height; ++y) std::copy_n(image.pixels.data() + (size_t(y + box.top) * image.width + box.left) * 3, size_t(crop.width) * 3, crop.pixels.data() + size_t(y) * crop.width * 3);
            const auto load_started=Clock::now(); initialize(); out.loading_ms=milliseconds(load_started); int resized_w = 0, resized_h = 0;
            const bool cached = m_embedding && crop.width == m_previous_crop.width && crop.height == m_previous_crop.height && crop.pixels == m_previous_crop.pixels;
            if (!cached) {
                auto pixels = mobile_sam_image_tensor(crop, resized_w, resized_h); OrtValue* input = tensor(pixels, {1, 3, 1024, 1024});
                Cleanup release {[&] { m_api->ReleaseValue(input); }};
                OrtValue* pending_embedding=nullptr;
                Cleanup release_pending {[&] { if(pending_embedding)m_api->ReleaseValue(pending_embedding); }};
                const char* in[] {"images"}; const char* output[] {"image_embeddings"}; const OrtValue* values[] {input};
                const auto started = Clock::now(); run(m_encoder, in, values, 1, output, 1, &pending_embedding, cancel); out.encoding_ms = milliseconds(started);
                if (cancel && cancel()) { out.canceled=true; return out; }
                // Publish the key and value together only after successful Run.
                // An interrupted encoder must not poison the old crop cache.
                m_previous_crop = crop;
                if(m_embedding)m_api->ReleaseValue(m_embedding);
                m_embedding=pending_embedding;pending_embedding=nullptr;
            } else {
                const double scale = 1024. / std::max(crop.width, crop.height); resized_w = int(crop.width * scale + .5); resized_h = int(crop.height * scale + .5);
            }
            std::vector<float> points, labels;
            bool positive = false, negative = false;
            for (const auto& p : request.prompts) {
                if (p.x < box.left || p.x >= box.right || p.y < box.top || p.y >= box.bottom) throw std::runtime_error("Boundary prompt is outside its ROI.");
                points.push_back(float(p.x - box.left) * resized_w / crop.width); points.push_back(float(p.y - box.top) * resized_h / crop.height);
                labels.push_back(p.positive ? 1.f : 0.f); positive |= p.positive; negative |= !p.positive;
            }
            if (!positive || !negative) throw std::runtime_error("Boundary requires foreground and background seeds.");
            points.insert(points.end(), {0.f, 0.f}); labels.push_back(-1.f);
            std::vector<float> mask(256 * 256, 0.f), has_mask {0.f}, original {float(crop.height), float(crop.width)};
            std::vector<OrtValue*> owned;
            Cleanup release {[&] { for (auto value : owned) if (value) m_api->ReleaseValue(value); }};
            owned.push_back(tensor(points, {1, int64_t(labels.size()), 2})); owned.push_back(tensor(labels, {1, int64_t(labels.size())}));
            owned.push_back(tensor(mask, {1, 1, 256, 256})); owned.push_back(tensor(has_mask, {1})); owned.push_back(tensor(original, {2}));
            const char* names[] {"image_embeddings", "point_coords", "point_labels", "mask_input", "has_mask_input", "orig_im_size"};
            const OrtValue* values[] {m_embedding, owned[0], owned[1], owned[2], owned[3], owned[4]};
            const char* names_out[] {"masks", "iou_predictions"}; OrtValue* output[2] {};
            Cleanup release_output {[&] { for (auto value : output) if (value) m_api->ReleaseValue(value); }};
            const auto started = Clock::now(); run(m_decoder, names, values, 6, names_out, 2, output, cancel); out.decoding_ms = milliseconds(started);
            if (cancel && cancel()) { out.canceled = true; return out; }
            const auto dims = shape(output[0]), scores_dims = shape(output[1]);
            if (dims.size() != 4 || dims[0] != 1 || dims[1] < 1 || dims[1] > 4 || dims[2] != crop.height || dims[3] != crop.width || scores_dims.size() != 2 || scores_dims[1] != dims[1]) throw std::runtime_error("Unexpected MobileSAM mask shape.");
            float *logits = nullptr, *scores = nullptr; check(m_api->GetTensorMutableData(output[0], reinterpret_cast<void**>(&logits))); check(m_api->GetTensorMutableData(output[1], reinterpret_cast<void**>(&scores)));
            int selected = -1; float best = -std::numeric_limits<float>::infinity();
            const size_t area = size_t(crop.width) * crop.height;
            for (int candidate = 0; candidate < dims[1]; ++candidate) {
                bool valid = std::isfinite(scores[candidate]);
                for (const auto& p : request.prompts) {
                    const float value = logits[size_t(candidate) * area + size_t(p.y - box.top) * crop.width + p.x - box.left];
                    valid &= std::isfinite(value) && ((value >= 0.f) == p.positive);
                }
                if (valid && scores[candidate] > best) { selected = candidate; best = scores[candidate]; }
            }
            if (selected < 0) {
                out.rejected=true;out.rejection_reason="No MobileSAM mask satisfies the supplied prompts.";
                for(int candidate=0;candidate<dims[1];++candidate)if(std::isfinite(scores[candidate]))
                    out.model_score=std::max(out.model_score,std::clamp(scores[candidate],0.f,1.f));
                return out;
            }
            out.model_score = std::clamp(best, 0.f, 1.f);
            for (int y = 0; y < crop.height; ++y) for (int x = 0; x < crop.width; ++x) {
                const float value = logits[size_t(selected) * area + size_t(y) * crop.width + x];
                if (!std::isfinite(value)) throw std::runtime_error("Non-finite MobileSAM output.");
                const size_t pixel = size_t(y + box.top) * image.width + x + box.left;
                out.foreground_probability[pixel] = 1.f / (1.f + std::exp(-std::clamp(value, -30.f, 30.f)));
                out.confidence[pixel] = out.model_score;
            }
        } catch (const std::exception& error) { if (cancel && cancel()) out.canceled = true; else out.error = error.what(); }
        return out;
    }
};
}
#endif

std::unique_ptr<IBoundaryRefiner> create_mobile_sam_refiner(const std::filesystem::path& root)
{
#if defined(_WIN32) && defined(ORCA_ENABLE_MOBILESAM_NATIVE)
    return std::make_unique<NativeMobileSam>(root);
#else
    (void)root; throw std::runtime_error("Native MobileSAM is not enabled in this build.");
#endif
}
}
