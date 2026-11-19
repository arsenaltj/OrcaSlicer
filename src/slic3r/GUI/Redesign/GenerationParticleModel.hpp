#pragma once

#include "InlineModelGenerationState.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

namespace Slic3r::GUI::GenerationParticles {

struct Image {
    int width {0}, height {0};
    std::vector<unsigned char> rgba;
};
struct SubjectMask {
    int width {0}, height {0};
    std::vector<unsigned char> alpha;
};
struct MaskResult {
    SubjectMask mask;
    std::string reason;
};
using ForegroundProvider = std::function<MaskResult(const Image&, const std::function<bool()>&)>;
struct PreparationDetails {
    std::string method {"none"}, reason {"unavailable"};
};
struct Point { float x, y, z, phase, intensity; };
struct Shape { std::vector<Point> points; };
struct Frame {
    std::shared_ptr<const Shape> logo, subject;
    double seconds {0}, subject_weight {0};
    std::string task;
};

enum class Quality { Normal, Low, Cpu, Static };

// Completed-frame latency includes business work and GUI queueing. It can
// reduce point count, but only our own drawing cost can freeze the animation.
// CPU drawing is reserved for an actual GL failure, not a slow GUI queue.
class QualityPolicy {
public:
    Quality quality() const { return m_quality; }
    void reset(bool cpu_required = false) {
        m_quality = cpu_required ? Quality::Cpu : Quality::Normal;
        m_slow_batches = m_fast_batches = 0;
    }
    void require_cpu() {
        if (m_quality < Quality::Cpu) m_quality = Quality::Cpu;
        m_slow_batches = m_fast_batches = 0;
    }
    bool observe(double interval_p95_ms, double cpu_p95_ms) {
        if (m_quality == Quality::Static) return false;
        const bool expensive = cpu_p95_ms > 8.0;
        const bool delayed = interval_p95_ms > 50.0;
        if (!expensive && !(delayed && m_quality == Quality::Normal)) {
            m_slow_batches = 0;
            if (m_quality == Quality::Low && !delayed && ++m_fast_batches >= 4) {
                reset();
                return true;
            }
            if (delayed) m_fast_batches = 0;
            return false;
        }
        m_fast_batches = 0;
        if (++m_slow_batches < 2) return false;
        m_slow_batches = 0;
        m_quality = m_quality == Quality::Normal ? Quality::Low : Quality::Static;
        return true;
    }
private:
    Quality m_quality {Quality::Normal};
    unsigned m_slow_batches {0};
    unsigned m_fast_batches {0};
};

// Kept separate from preview generation: installing the artifact or changing
// its palette must not restart the waiting animation.
inline std::string task_key(const GenerationBinding& binding, const ModelGenerationUIState& state) {
    return std::to_string(int(binding.source)) + "/" + binding.context + "/" +
        std::to_string(binding.epoch) + "/" + inline_generation_task(state);
}
inline bool usable_reference(const ModelGenerationUIState& state) {
    const auto& ref = state.visual_reference;
    return ref.ready && !ref.path.empty() && !ref.version.empty() &&
        !inline_generation_task(state).empty() && ref.owner_id == inline_generation_task(state);
}
inline bool same_image(const ModelGenerationVisualReference& a, const ModelGenerationVisualReference& b) {
    return a.owner_id == b.owner_id && a.version == b.version && a.path == b.path && a.sha256 == b.sha256 &&
        a.verification_manifest == b.verification_manifest;
}

inline bool has_subject_alpha(const Image& image) {
    const auto count=image.rgba.size()/4;
    std::size_t transparent=0, opaque=0;
    for (std::size_t i=0;i<count;++i) {
        transparent+=image.rgba[4*i+3]<32; opaque+=image.rgba[4*i+3]>192;
    }
    return count && transparent>count/25 && opaque>count/100 && opaque<count*94/100;
}

// Shared, bounded sampler for trusted alpha, local segmentation and the
// conservative plain-background fallback. No category-specific decisions.
inline std::shared_ptr<const Shape> prepare(const Image& image, bool logo = false,
    std::function<bool()> canceled = {}, std::size_t requested = 8000, const SubjectMask* supplied = nullptr)
{
    const int w = image.width, h = image.height;
    if (w < 8 || h < 8 || w > 512 || h > 512 || image.rgba.size() != std::size_t(w) * h * 4) return {};
    const auto stopped = [&] { return canceled && canceled(); };
    if (stopped()) return {};
    const int n = w * h;
    std::vector<unsigned char> mask(n);
    const bool alpha = has_subject_alpha(image);
    const bool explicit_mask = supplied != nullptr;
    if (explicit_mask) {
        if (supplied->width!=w || supplied->height!=h || supplied->alpha.size()!=std::size_t(n)) return {};
        for (int i=0;i<n;++i) mask[i]=supplied->alpha[i]>96;
    } else if (alpha || logo) {
        for (int i = 0; i < n; ++i) mask[i] = image.rgba[4*i+3] > 96;
    } else {
        // A clipped portrait can occupy one edge while the other three still
        // provide a reliable studio background. Use their colour consensus;
        // averaging skin/hair into the background used to reject these images.
        std::array<std::vector<int>, 4> edges;
        std::array<std::vector<unsigned char>, 3> channels;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            if (x == 0 || y == 0 || x == w-1 || y == h-1) {
                const int p = y*w+x;
                for (int c = 0; c < 3; ++c) channels[c].push_back(image.rgba[4*p+c]);
                if (y == 0) edges[0].push_back(p);
                if (y == h-1) edges[1].push_back(p);
                if (x == 0) edges[2].push_back(p);
                if (x == w-1) edges[3].push_back(p);
            }
        double median[3] {}, mean[3] {};
        for (int c = 0; c < 3; ++c) {
            auto& values = channels[c];
            std::nth_element(values.begin(), values.begin()+values.size()/2, values.end());
            median[c] = values[values.size()/2];
        }
        const auto agrees = [&](int p) {
            double distance = 0;
            for (int c = 0; c < 3; ++c) {
                const double d = image.rgba[4*p+c] - median[c]; distance += d*d;
            }
            return distance <= 3 * 24.0 * 24.0;
        };
        int reliable_edges = 0, inliers = 0, border = 0;
        for (const auto& edge : edges) {
            int agreed = 0, runs = 0; bool previous = true;
            for (const int p : edge) {
                const bool match = agrees(p);
                if (!match && previous) ++runs;
                previous = match;
                if (match) {
                    ++agreed; ++inliers;
                    for (int c = 0; c < 3; ++c) mean[c] += image.rgba[4*p+c];
                }
            }
            // Stripes, scattered edge texture and several uncertain edges
            // are not evidence of a single foreground on a plain background.
            const bool reliable=agreed * 10 >= int(edge.size()) * 9;
            // A subject-clipped edge can contain any number of foreground
            // tones. Texture is a veto only on a claimed background edge.
            if ((reliable && runs > 2) || (!reliable && runs > std::max(3,int(edge.size())/24))) return {};
            reliable_edges += reliable;
            border += int(edge.size());
        }
        if (reliable_edges < 3 || inliers * 4 < border * 3) return {};
        for (auto& c : mean) c /= inliers;
        std::vector<int> queue;
        queue.reserve(n);
        std::vector<unsigned char> background(n);
        const auto admit = [&](int p) {
            if (background[p]) return;
            double distance = 0;
            for (int c = 0; c < 3; ++c) {
                const auto d = image.rgba[4*p+c] - mean[c]; distance += d*d;
            }
            if (distance <= 3 * 34.0 * 34.0) { background[p] = 1; queue.push_back(p); }
        };
        for (int x = 0; x < w; ++x) { admit(x); admit((h-1)*w+x); }
        for (int y = 0; y < h; ++y) { admit(y*w); admit(y*w+w-1); }
        for (std::size_t q = 0; q < queue.size(); ++q) {
            if ((q & 4095) == 0 && stopped()) return {};
            const int p = queue[q], x = p % w, y = p / w;
            if (x) admit(p-1); if (x+1 < w) admit(p+1);
            if (y) admit(p-w); if (y+1 < h) admit(p+w);
        }
        for (int p = 0; p < n; ++p) mask[p] = !background[p];
    }
    if (stopped()) return {};
    int count = std::accumulate(mask.begin(), mask.end(), 0);
    if (count == 0 || count < n / 100 || count > n * 94 / 100) return {};
    // Preserve disconnected parts of a supplied mask/alpha (ears, separate
    // ornaments, buildings). Only the colour heuristic guesses a single body.
    if (!logo) {
        std::vector<unsigned char> visited(n);
        std::vector<int> largest, queue;
        std::vector<unsigned char> cleaned(n);
        int second = 0;
        for (int p = 0; p < n; ++p) if (mask[p] && !visited[p]) {
            queue.clear(); queue.push_back(p); visited[p] = 1;
            for (std::size_t q = 0; q < queue.size(); ++q) {
                if ((q & 4095) == 0 && stopped()) return {};
                const int v = queue[q], x = v % w, y = v / w;
                const auto add = [&](int v2) { if (mask[v2] && !visited[v2]) { visited[v2] = 1; queue.push_back(v2); } };
                if (x) add(v-1); if (x+1 < w) add(v+1);
                if (y) add(v-w); if (y+1 < h) add(v+w);
            }
            if (queue.size() >= std::size_t(std::max(4,n/4000)))
                for (const int v:queue) cleaned[v]=1;
            if (queue.size() > largest.size()) { second = int(largest.size()); largest = queue; }
            else second = std::max(second, int(queue.size()));
        }
        if (largest.empty()) return {};
        if (explicit_mask || alpha) mask=std::move(cleaned);
        else {
            if (second > int(largest.size()) / 5) return {};
            std::fill(mask.begin(), mask.end(), 0);
            for (const int p : largest) mask[p] = 1;
        }
        count=std::accumulate(mask.begin(),mask.end(),0);
        if (count<n/100) return {};
    }
    int minx = w, maxx = 0, miny = h, maxy = 0;
    std::array<bool, 4> touching {};
    std::vector<int> candidates;
    candidates.reserve(count * 2);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (mask[y*w+x]) {
        const int p = y*w+x;
        minx = std::min(minx, x); maxx = std::max(maxx, x);
        miny = std::min(miny, y); maxy = std::max(maxy, y);
        touching[0] = touching[0] || x == 0; touching[1] = touching[1] || x == w-1;
        touching[2] = touching[2] || y == 0; touching[3] = touching[3] || y == h-1;
        candidates.push_back(p);
        const bool edge = x && y && x+1 < w && y+1 < h &&
            (!mask[p-1] || !mask[p+1] || !mask[p-w] || !mask[p+w]);
        const int dx = x+1 < w ? std::abs(int(image.rgba[4*p])-image.rgba[4*(p+1)]) : 0;
        if (edge || dx > 32) candidates.push_back(p);
    }
    if (!logo && !alpha && !explicit_mask && std::count(touching.begin(), touching.end(), true) > 1) return {};
    auto result = std::make_shared<Shape>();
    requested = std::clamp<std::size_t>(requested, 1, 12000);
    result->points.reserve(requested);
    std::uint32_t seed = 0x9368ab45;
    const auto random = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
    const float side = float(std::max(maxx-minx+1, maxy-miny+1));
    for (std::size_t i = 0; i < requested; ++i) {
        if ((i & 1023) == 0 && stopped()) return {};
        const auto index = std::min(candidates.size()-1, std::size_t(random() * candidates.size()));
        const int p = candidates[index];
        const float luminance = (0.2126f*image.rgba[4*p] + 0.7152f*image.rgba[4*p+1] + 0.0722f*image.rgba[4*p+2])/255.f;
        const float tone = logo ? 1.f : 0.35f+0.65f*std::sqrt(luminance);
        result->points.push_back({(p%w - (minx+maxx)*0.5f + (random()-0.5f)*0.7f) / side,
            (p/w - (miny+maxy)*0.5f + (random()-0.5f)*0.7f) / side,
            (random()-0.5f)*0.14f, random()*6.2831853f, tone*(0.75f+random()*0.25f)});
    }
    return result;
}
inline Point sample(Point p, double seconds) {
    const float t = float(std::isfinite(seconds) ? std::fmod(std::max(0.0, seconds), 10000.0) : 0.0);
    // Keep in sync with the vertex shader. Local circulation and a travelling
    // light wave remain visible without rotating or dissolving the silhouette.
    const float wave = std::sin(t*2.2f-p.y*7.f+p.phase*0.4f);
    p.x += 0.010f*std::sin(t*1.4f+p.phase)+0.003f*std::sin(t*2.5f+p.phase*1.7f)+p.z*0.08f*std::sin(t*0.55f);
    p.y += 0.011f*std::cos(t*1.2f+p.phase)+0.003f*std::sin(t*2.1f+p.phase*1.3f);
    p.z += 0.025f*std::sin(t*0.9f+p.phase);
    p.intensity *= 0.55f+0.45f*(0.5f+0.5f*wave);
    return p;
}
} // namespace Slic3r::GUI::GenerationParticles
