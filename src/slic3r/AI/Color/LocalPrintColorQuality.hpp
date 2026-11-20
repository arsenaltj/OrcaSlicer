#pragma once
#include "LocalPrintColorMatching.hpp"

namespace Slic3r::GUI::LocalPrintColorQuality {
struct RegionQuality {
    std::string id;
    std::string label;
    double area {0};
    double mean_delta_e {0};
    double worst_delta_e {0};
    double unresolved_area {0};
};
struct Quality {
    bool has_covered_samples {false};
    double mean_delta_e {0};
    double p95_delta_e {0};
    double worst_delta_e {0};
    double unresolved_area_fraction {0};
    double over_tolerance_area_fraction {0};
    std::vector<RegionQuality> regions;
    std::string error;
};

// Evaluate original surface samples, not only quantized cluster centers. This
// makes quantization loss visible even when a centroid exactly matches a spool.
inline Quality evaluate(const std::vector<LocalPrintColorMatching::FaceSample>& original,
                        const AI::LocalPrintColorResult& result)
{
    Quality quality;
    if (!result.valid(quality.error)) return quality;
    if (original.size() != result.face_count) { quality.error = "Quality samples do not match the surface."; return quality; }
    std::vector<std::pair<double, double>> residuals;
    std::vector<double> face_errors(original.size());
    std::map<size_t, AI::PrintRgb> overrides(result.user_overrides.begin(), result.user_overrides.end());
    double total = 0, unresolved = 0;
    for (size_t f = 0; f < original.size(); ++f) {
        const auto& sample = original[f];
        if (!AI::valid_print_rgb(sample.color) || !std::isfinite(sample.area) || sample.area <= 0) {
            quality.error = "Invalid quality sample."; return quality;
        }
        const auto& target = result.targets[result.face_targets[f]];
        const auto edit = overrides.find(f);
        const auto& source = edit == overrides.end() ? sample.color : edit->second;
        const double delta = LocalPrintColorMatching::delta_e(source, target.output);
        face_errors[f] = delta;
        total += sample.area;
        if (!std::isfinite(total)) { quality.error = "Quality sample area overflow."; return quality; }
        if (!target.executable) unresolved += sample.area;
        // Unassigned target.output is only an intent preview, not an attainable
        // color. Exclude it from residual aggregates and report coverage.
        if (!target.executable) continue;
        if (delta > result.color_tolerance) quality.over_tolerance_area_fraction += sample.area;
        residuals.emplace_back(delta, sample.area);
        quality.mean_delta_e += delta * sample.area;
        quality.worst_delta_e = std::max(quality.worst_delta_e, delta);
    }
    quality.unresolved_area_fraction = unresolved / total;
    quality.over_tolerance_area_fraction /= total;
    const double covered = total - unresolved;
    if (covered > 0) {
        quality.has_covered_samples = true;
        quality.mean_delta_e /= covered;
        std::sort(residuals.begin(), residuals.end());
        double cumulative = 0;
        for (const auto& r : residuals) {
            cumulative += r.second;
            quality.p95_delta_e = r.first;
            if (cumulative >= covered * .95) break;
        }
    }
    for (const auto& region : result.regions) {
        RegionQuality q; q.id = region.id; q.label = region.label;
        for (size_t f : region.faces) {
            q.area += original[f].area;
            if (!result.targets[result.face_targets[f]].executable) { q.unresolved_area += original[f].area; continue; }
            q.mean_delta_e += original[f].area * face_errors[f];
            q.worst_delta_e = std::max(q.worst_delta_e, face_errors[f]);
        }
        if (q.area > q.unresolved_area) q.mean_delta_e /= q.area - q.unresolved_area;
        quality.regions.push_back(std::move(q));
    }
    return quality;
}
} // namespace Slic3r::GUI::LocalPrintColorQuality
