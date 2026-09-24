#pragma once

#include "LocalPrintColorMatching.hpp"
#include "LocalPrintColorQuality.hpp"
#include "LocalPrintColorPatchRefinement.hpp"
#include "../Model/SurfaceSelectionState.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <numeric>

namespace Slic3r::GUI::LocalPrintColorBoundaryRefinement {
namespace detail {
inline bool same_regions(const std::vector<AI::PrintColorRegion>& a, const std::vector<AI::PrintColorRegion>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].subject_id != b[i].subject_id || a[i].label != b[i].label ||
            a[i].confidence != b[i].confidence || a[i].user_protected != b[i].user_protected ||
            a[i].protect_color != b[i].protect_color || a[i].locked_physical_slot != b[i].locked_physical_slot ||
            a[i].faces != b[i].faces) return false;
    return true;
}
inline bool same_contrasts(const std::vector<AI::PrintColorContrast>& a, const std::vector<AI::PrintColorContrast>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].first_region != b[i].first_region || a[i].second_region != b[i].second_region ||
            a[i].weight != b[i].weight || a[i].minimum_output_delta_e != b[i].minimum_output_delta_e ||
            a[i].hard != b[i].hard) return false;
    return true;
}
struct EdgeIncidence { uint32_t a, b, face; double length; };
struct Edge { uint32_t first, second; double length; };
} // namespace detail

// A display-color refinement, not mesh repair or a printability test. Versioning
// belongs to the caller. A kept baseline remains intact apart from its notice.
inline LocalPrintColorMatching::Computation compute_refined(
    const LocalPrintColorMatching::Input& input, const indexed_triangle_set& mesh,
    LocalPrintColorMatching::Computation baseline, bool run_patches=true, bool run_local_rounds=true)
{
    namespace Matching = LocalPrintColorMatching;
    if (!baseline.ok()) return baseline;
    auto cancelled = [&] { return input.cancelled && input.cancelled(); };
    auto stop = [&] { baseline.cancelled = true; return baseline; };
    auto keep = [&](const std::string& reason) {
        baseline.result.notices.push_back("Boundary refinement kept the baseline: " + reason);
        return baseline;
    };
    if (cancelled()) return stop();
    const auto& original = baseline.result;
    std::string error;
    if (!original.valid(error)) {
        baseline.error = "Invalid boundary-refinement baseline: " + error;
        return baseline;
    }
    if (original.confirmed) return keep("confirmed versions are not revised in place.");
    if (original.targets.size() > AI::kMaxLocalPrintTargetColors)
        return keep("the target count exceeds the bounded refinement budget.");
    const auto& identity = input.identity;
    const auto& contrasts = input.contrasts.empty() ? identity.contrasts : input.contrasts;
    if (original.source_sha256 != identity.source_sha256 || original.geometry_id != identity.geometry_id ||
        original.face_count != identity.face_count || original.requested_color_count != identity.requested_color_count ||
        original.material_fingerprint != identity.material_fingerprint || original.process_fingerprint != identity.process_fingerprint ||
        original.color_tolerance != input.tolerance || original.important_area_floor != input.important_area_floor ||
        original.user_overrides != identity.user_overrides ||
        !detail::same_regions(original.regions, identity.regions) || !detail::same_contrasts(original.contrasts, contrasts) ||
        original.physical_channels.size() != identity.physical_channels.size())
        return keep("the input and computed result no longer describe the same request.");
    for (size_t i = 0; i < original.physical_channels.size(); ++i) {
        const auto& a = original.physical_channels[i]; const auto& b = identity.physical_channels[i];
        if (a.slot != b.slot || a.display_color != b.display_color || a.material_type != b.material_type || a.compatible != b.compatible)
            return keep("the physical material snapshot changed.");
    }
    for (const auto& target : original.targets)
        if (!target.executable || !target.physical_slot || target.recipe)
            return keep("some targets lack a direct physical assignment.");
    const size_t count = input.faces.size(), target_count = original.targets.size();
    // Several layered-mode source groups may use one real tool. Boundaries
    // and connected components concern that physical output, not artificial
    // boundaries between logical groups with identical printing assignments.
    std::vector<size_t> surface_group(target_count);
    std::map<size_t, size_t> slot_group;
    for (size_t t = 0; t < target_count; ++t)
        surface_group[t] = slot_group.emplace(*original.targets[t].physical_slot, t).first->second;
    if (count == 0 || count != mesh.indices.size() || count != original.face_count || mesh.vertices.empty() ||
        count > std::numeric_limits<uint32_t>::max() / 3 || mesh.vertices.size() > std::numeric_limits<uint32_t>::max())
        return keep("native topology is absent or does not cover the source faces.");

    auto samples = input.faces;
    std::vector<uint8_t> frozen(count, 0);
    for (const auto& item : original.user_overrides) { samples[item.first].color = item.second; frozen[item.first] = 1; }
    std::set<std::string> contrast_regions;
    for (const auto& contrast : original.contrasts) {
        contrast_regions.insert(contrast.first_region); contrast_regions.insert(contrast.second_region);
    }
    for (const auto& region : original.regions)
        if (region.user_protected || region.locked_physical_slot || contrast_regions.count(region.id))
            for (size_t f : region.faces) frozen[f] = 1;
    double total_area = 0;
    for (size_t f = 0; f < count; ++f) {
        if (f % 4096 == 0 && cancelled()) return stop();
        if (!AI::valid_print_rgb(input.faces[f].color) || !AI::valid_print_rgb(samples[f].color) ||
            !std::isfinite(samples[f].area) || samples[f].area <= 0)
            return keep("source colors or face areas are invalid.");
        total_area += samples[f].area;
    }
    if (!std::isfinite(total_area)) return keep("surface area overflow.");

    // Exact finite float32 positions only; no radius weld can bridge a narrow gap.
    // Duplicated seam vertices are aliases for adjacency, never modified geometry.
    std::vector<uint32_t> aliases(mesh.vertices.size());
    {
        std::map<std::array<float, 3>, uint32_t> positions;
        for (size_t v = 0; v < mesh.vertices.size(); ++v) {
            if (v % 4096 == 0 && cancelled()) return stop();
            if (!mesh.vertices[v].allFinite()) return keep("native geometry contains a non-finite coordinate.");
            const auto& p = mesh.vertices[v];
            const auto inserted = positions.emplace(std::array<float, 3>{p.x(), p.y(), p.z()}, uint32_t(positions.size()));
            aliases[v] = inserted.first->second;
        }
    }
    std::vector<detail::EdgeIncidence> incidences;
    incidences.reserve(count * 3);
    for (size_t f = 0; f < count; ++f) {
        if (f % 4096 == 0 && cancelled()) return stop();
        const auto& face = mesh.indices[f];
        for (int c = 0; c < 3; ++c)
            if (face[c] < 0 || size_t(face[c]) >= mesh.vertices.size()) return keep("native face indices are invalid.");
        const auto a = mesh.vertices[face[0]].cast<double>().eval();
        const auto b = mesh.vertices[face[1]].cast<double>().eval();
        const auto c = mesh.vertices[face[2]].cast<double>().eval();
        const double twice_area = (b-a).cross(c-a).norm();
        if (!std::isfinite(twice_area) || twice_area <= 0) return keep("native geometry has a degenerate face.");
        for (int i = 0; i < 3; ++i) {
            const int j = (i+1)%3;
            const auto u = aliases[face[i]], v = aliases[face[j]];
            if (u == v) return keep("coordinate welding collapses a native face.");
            const double length = (mesh.vertices[face[i]].cast<double>() - mesh.vertices[face[j]].cast<double>()).norm();
            incidences.push_back({std::min(u,v), std::max(u,v), uint32_t(f), length});
        }
    }
    if (cancelled()) return stop();
    if (AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh) != original.geometry_id)
        return keep("the native geometry fingerprint does not match the source.");
    if (cancelled()) return stop();
    std::sort(incidences.begin(), incidences.end(), [](const auto& a, const auto& b) {
        return a.a < b.a || (a.a == b.a && (a.b < b.b || (a.b == b.b && a.face < b.face)));
    });
    if (cancelled()) return stop();
    std::vector<detail::Edge> edges;
    edges.reserve(incidences.size()/2);
    std::vector<std::array<uint32_t, 3>> incident_edges(count);
    std::vector<uint8_t> degree(count, 0);
    for (size_t begin = 0; begin < incidences.size();) {
        if (begin % 4096 == 0 && cancelled()) return stop();
        size_t end = begin+1;
        while (end < incidences.size() && incidences[end].a == incidences[begin].a && incidences[end].b == incidences[begin].b) ++end;
        if (end-begin != 2 || incidences[begin].face == incidences[begin+1].face)
            return keep("the exact-coordinate surface is not closed with two faces per edge.");
        const auto a = incidences[begin].face, b = incidences[begin+1].face;
        if (degree[a] >= 3 || degree[b] >= 3) return keep("native full-edge adjacency is inconsistent.");
        const uint32_t edge = uint32_t(edges.size());
        edges.push_back({a,b,incidences[begin].length});
        incident_edges[a][degree[a]++] = edge; incident_edges[b][degree[b]++] = edge;
        begin = end;
    }
    if (std::any_of(degree.begin(), degree.end(), [](uint8_t n) { return n != 3; }))
        return keep("native full-edge adjacency is incomplete.");
    incidences.clear(); incidences.shrink_to_fit(); aliases.clear(); aliases.shrink_to_fit();

    auto topology = [&](const std::vector<size_t>& labels, std::vector<size_t>& components, double& boundary) {
        std::vector<uint32_t> parent(count); std::iota(parent.begin(), parent.end(), uint32_t(0));
        std::vector<uint8_t> rank(count, 0);
        auto root = [&](uint32_t v) {
            while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
            return v;
        };
        boundary = 0;
        for (size_t i = 0; i < edges.size(); ++i) {
            if (i % 4096 == 0 && cancelled()) return false;
            const auto& edge = edges[i];
            if (surface_group[labels[edge.first]] != surface_group[labels[edge.second]]) { boundary += edge.length; continue; }
            auto a = root(edge.first), b = root(edge.second);
            if (a == b) continue;
            if (rank[a] < rank[b]) std::swap(a,b);
            parent[b] = a; if (rank[a] == rank[b]) ++rank[a];
        }
        components.assign(target_count, 0);
        for (size_t f = 0; f < count; ++f) {
            if (f % 4096 == 0 && cancelled()) return false;
            if (root(uint32_t(f)) == f) ++components[surface_group[labels[f]]];
        }
        return true;
    };
    // Reuse native adjacency across rounds. Every accepted move strictly reduces
    // that face's error; every round retains all groups and the original region
    // decisions. A rejected later round must not discard earlier valid progress.
    auto accepted = original.face_targets;
    std::vector<size_t> before, after;
    double boundary_before = 0, boundary_after = 0;
    if (!topology(accepted, before, boundary_before)) return stop();
    const double initial_boundary = boundary_before;
    const size_t round_limit = run_local_rounds ? (original.mode == AI::PrintColorMode::Layered ? 8 : 1) : 0;
    size_t accepted_rounds = 0;
    std::string stop_reason=run_local_rounds ? "" : "only coherent patches were requested.";
    for (size_t round = 0; round < round_limit; ++round) {
        auto assignment = accepted;
        size_t moves = 0;
        for (size_t f = 0; f < count; ++f) {
            if (f % 4096 == 0 && cancelled()) return stop();
            if (frozen[f]) continue;
            std::array<bool, AI::kMaxLocalPrintTargetColors> allowed {};
            std::array<double, AI::kMaxLocalPrintTargetColors> same_length {};
            for (const auto index : incident_edges[f]) {
                const auto& edge = edges[index];
                const size_t neighbor = edge.first == f ? edge.second : edge.first;
                const size_t t = accepted[neighbor];
                allowed[t] = true; same_length[surface_group[t]] += edge.length;
            }
            const size_t original_target = accepted[f];
            double best = Matching::delta_e(samples[f].color, original.targets[original_target].output);
            for (size_t t = 0; t < target_count; ++t) {
                if (!allowed[t] || same_length[surface_group[t]] + 1e-9 < same_length[surface_group[original_target]]) continue;
                const double error = Matching::delta_e(samples[f].color, original.targets[t].output);
                if (error < best - 1e-9) { best = error; assignment[f] = t; }
            }
            if (assignment[f] != original_target) ++moves;
        }
        if (moves == 0) {
            stop_reason = "no eligible move improves source color error without increasing its local boundary.";
            break;
        }
        std::vector<size_t> population(target_count, 0);
        for (size_t t : assignment) ++population[t];
        if (std::find(population.begin(), population.end(), 0) != population.end()) {
            stop_reason = "the simultaneous round would empty an existing target.";
            break;
        }
        if (original.mode == AI::PrintColorMode::Layered) {
            // Automatic separation was evaluated on the region's dominant output.
            // Pointwise improvements must not erase that decision in a small face,
            // lip or eye region. Keep the whole round if its dominant tool changes.
            for (const auto& region : original.regions) {
                if (region.id.compare(0, 14, "auto-semantic:") != 0 || region.confidence < .5 || region.faces.empty()) continue;
                std::vector<double> before(target_count, 0), after(target_count, 0);
                for (size_t f : region.faces) {
                    before[surface_group[original.face_targets[f]]] += samples[f].area;
                    after[surface_group[assignment[f]]] += samples[f].area;
                }
                if (std::max_element(before.begin(), before.end()) - before.begin() !=
                    std::max_element(after.begin(), after.end()) - after.begin())
                    stop_reason = "the round would change a reliable semantic region's dominant physical output.";
            }
        }
        if (!stop_reason.empty()) break;
        if (!topology(assignment, after, boundary_after)) return stop();
        if (boundary_after > boundary_before + 1e-9) {
            stop_reason = "the simultaneous round would increase total color-boundary length.";
            break;
        }
        for (size_t t = 0; t < target_count; ++t)
            if (after[t] > before[t]) stop_reason = "the simultaneous round would increase a target's connected-component count.";
        if (!stop_reason.empty()) break;
        accepted = std::move(assignment); before = std::move(after); boundary_before = boundary_after;
        ++accepted_rounds;
    }
    size_t island_patches=0, detail_patches=0;
    if(run_patches && original.mode==AI::PrintColorMode::Layered) {
        const auto patch_components=before;
        const double patch_boundary=boundary_before;
        bool patch_cancelled=false;
        auto patch_gate=[&](const std::vector<size_t>& labels) {
            std::vector<size_t> components; double boundary=0;
            if(!topology(labels,components,boundary)) {patch_cancelled=true;return false;}
            if(boundary>patch_boundary+1e-9) return false;
            for(size_t t=0;t<target_count;++t) if(components[t]>patch_components[t]) return false;
            return true;
        };
        auto patches=LocalPrintColorPatchRefinement::refine(samples,original,accepted,frozen,surface_group,
            edges,incident_edges,input.cancelled,patch_gate);
        if(patch_cancelled || patches.cancelled || cancelled()) return stop();
        island_patches=patches.islands; detail_patches=patches.detail_patches;
        accepted=std::move(patches.assignment);
        if(!topology(accepted,before,boundary_before)) return stop();
    }
    if (accepted_rounds == 0 && island_patches == 0 && detail_patches == 0) return keep(stop_reason);
    size_t changed = 0;
    for (size_t f = 0; f < count; ++f) changed += accepted[f] != original.face_targets[f];

    auto refined = original;
    refined.face_targets = std::move(accepted);
    std::vector<double> areas(target_count, 0), residuals(target_count, 0), worst(target_count, 0);
    std::vector<std::array<double, 3>> sources(target_count);
    for (size_t f = 0; f < count; ++f) areas[refined.face_targets[f]] += samples[f].area;
    for (size_t f = 0; f < count; ++f) {
        if (f % 4096 == 0 && cancelled()) return stop();
        const size_t t = refined.face_targets[f]; const double weight = samples[f].area / areas[t];
        const double error = Matching::delta_e(samples[f].color, refined.targets[t].output);
        residuals[t] += weight * error; worst[t] = std::max(worst[t], error);
        for (size_t c = 0; c < 3; ++c) sources[t][c] += weight * samples[f].color[c];
    }
    for (size_t t = 0; t < target_count; ++t) {
        auto& target = refined.targets[t]; target.area = areas[t]; target.delta_e00 = residuals[t];
        // The former source representative was a quantization seed. After a
        // partition change, report its actual area-weighted source RGB mean.
        for (size_t c = 0; c < 3; ++c) target.source[c] = float(std::clamp(sources[t][c], 0.0, 1.0));
        target.evidence = AI::ColorEvidence::Estimated;
        target.within_tolerance = (refined.mode == AI::PrintColorMode::Layered ? worst[t] : target.delta_e00) <= refined.color_tolerance;
        target.unresolved_reason = target.within_tolerance ? "" : "Display-color estimate exceeds the requested tolerance.";
    }
    // Those algorithm-owned notices contain stale regional numbers or describe
    // the pre-refinement partition. Preserve unrelated notices and remeasure AI.
    refined.notices.erase(std::remove_if(refined.notices.begin(), refined.notices.end(), [](const auto& notice) {
        return notice.rfind("AI color preference [", 0) == 0 || notice.rfind("The AI preference candidate gave no further improvement;", 0) == 0;
    }), refined.notices.end());
    refined.notices.push_back((island_patches || detail_patches ? "Bounded local rounds and coherent patch refinement changed " :
        std::to_string(accepted_rounds) + " bounded display-color boundary rounds changed ") + std::to_string(changed) +
        " faces; total boundary length " + std::to_string(initial_boundary) + " -> " + std::to_string(boundary_before) +
        " mm, with no target component-count increase. This is not a printability or measured-color guarantee.");
    if (original.mode == AI::PrintColorMode::Layered)
        refined.notices.push_back("Boundary refinement stopped: " + (stop_reason.empty() ?
            "the eight-round computation limit was reached; convergence is not claimed." : stop_reason));
    if(island_patches || detail_patches)
        refined.notices.push_back("Coherent patch refinement accepted " + std::to_string(island_patches) +
            " whole physical islands and " + std::to_string(detail_patches) +
            " protected-region patches. Every changed face improved its display error; reliable-region dominant outputs were preserved and regional fragmentation did not increase, including complete block recolors. Candidate checks are bounded; this is not a global optimum or measured print-color guarantee.");
    for (const auto& region : refined.regions) {
        if (region.user_protected || !region.protect_color || region.confidence < .5 || region.faces.empty()) continue;
        double area = 0, weighted = 0, worst = 0;
        for (size_t f : region.faces) area += samples[f].area;
        for (size_t i = 0; i < region.faces.size(); ++i) {
            if (i % 4096 == 0 && cancelled()) return stop();
            const size_t f = region.faces[i];
            const double error = Matching::delta_e(samples[f].color, refined.targets[refined.face_targets[f]].output);
            weighted += (samples[f].area / area) * error; worst = std::max(worst, error);
        }
        refined.notices.push_back("AI color preference [" + region.id + "] is approximate: covered mean DeltaE00=" +
            std::to_string(weighted) + ", worst DeltaE00=" + std::to_string(worst) + "; unassigned area fraction=0" +
            (worst > refined.color_tolerance ? "; requested color tolerance was not preserved." : "; estimated colors meet the requested tolerance."));
    }
    if (cancelled()) return stop();
    if (!refined.valid(error)) return keep("the refined result failed its contract check.");
    baseline.result = std::move(refined);
    return baseline;
}

// Compare the final outputs: a soft preference must not buy an improvement in
// one region by degrading overall quality or another protected region. Both
// candidates contain exactly the same manual edits, locks and contrast rules.
inline bool preserves_quality(const LocalPrintColorQuality::Quality& candidate,
                              const LocalPrintColorQuality::Quality& baseline,
                              const std::vector<AI::PrintColorRegion>& regions)
{
    if (!candidate.error.empty() || !baseline.error.empty()) return false;
    auto no_larger = [](double a, double b, double epsilon) {
        return std::isfinite(a) && std::isfinite(b) && a <= b + epsilon;
    };
    if (!no_larger(candidate.unresolved_area_fraction, baseline.unresolved_area_fraction, 1e-12) ||
        !no_larger(candidate.over_tolerance_area_fraction, baseline.over_tolerance_area_fraction, 1e-12)) return false;
    if (baseline.has_covered_samples && (!candidate.has_covered_samples ||
        !no_larger(candidate.mean_delta_e, baseline.mean_delta_e, 1e-9) ||
        !no_larger(candidate.p95_delta_e, baseline.p95_delta_e, 1e-9) ||
        !no_larger(candidate.worst_delta_e, baseline.worst_delta_e, 1e-9))) return false;
    for (const auto& region : regions) {
        if ((!region.user_protected && (!region.protect_color || region.confidence < .5)) || region.faces.empty()) continue;
        auto find = [&](const auto& quality) {
            return std::find_if(quality.regions.begin(), quality.regions.end(), [&](const auto& q) { return q.id == region.id; });
        };
        const auto a = find(candidate), b = find(baseline);
        if (a == candidate.regions.end() || b == baseline.regions.end() ||
            !no_larger(a->unresolved_area / a->area, b->unresolved_area / b->area, 1e-12)) return false;
        if (b->area > b->unresolved_area &&
            (!no_larger(a->mean_delta_e, b->mean_delta_e, 1e-9) ||
             !no_larger(a->worst_delta_e, b->worst_delta_e, 1e-9))) return false;
    }
    return true;
}

inline LocalPrintColorMatching::Computation compute_quality_guarded(
    const LocalPrintColorMatching::Input& input, const indexed_triangle_set& mesh)
{
    namespace Matching = LocalPrintColorMatching;
    // Choose the ordinary/preference baseline before patch refinement. Otherwise
    // enabling detail patches changes the candidate competition and can replace
    // faces unrelated to any accepted patch, defeating pointwise preservation.
    auto finish=[&](Matching::Computation chosen) {
        if(chosen.ok() && chosen.result.mode==AI::PrintColorMode::Layered)
            chosen=compute_refined(input,mesh,std::move(chosen),true,false);
        return chosen;
    };
    auto candidate = compute_refined(input, mesh, Matching::compute(input), false);
    if (!candidate.ok()) return candidate;
    candidate.result.algorithm_version = candidate.result.mode == AI::PrintColorMode::Layered ?
        "region-layered-v2-boundary-v2-patches-v3-quality-v1" : "region-direct-v5-boundary-v1-quality-v1";
    bool has_preference = false;
    auto baseline_input = input;
    for (auto& region : baseline_input.identity.regions) {
        if (region.user_protected) continue;
        has_preference |= region.protect_color && region.confidence >= .5 && !region.faces.empty() && !region.locked_physical_slot;
        region.protect_color = false;
    }
    if (!has_preference) return finish(std::move(candidate));
    auto baseline = compute_refined(baseline_input, mesh, Matching::compute(baseline_input), false);
    if (!baseline.ok()) return baseline; // A cancelled comparison cannot publish a candidate.
    baseline.result.algorithm_version = candidate.result.algorithm_version;
    baseline.result.regions = input.identity.regions; // Keep evidence and manual intent when retaining its output.
    auto cancelled = [&] { return input.cancelled && input.cancelled(); };
    if (cancelled()) { candidate.cancelled = true; return candidate; }
    const auto candidate_quality = LocalPrintColorQuality::evaluate(input.faces, candidate.result);
    const auto baseline_quality = LocalPrintColorQuality::evaluate(input.faces, baseline.result);
    if (cancelled()) { candidate.cancelled = true; return candidate; }
    if (!candidate_quality.error.empty() || !baseline_quality.error.empty()) {
        candidate.error = "Unable to verify final semantic color quality.";
        return candidate;
    }
    if (!preserves_quality(candidate_quality, baseline_quality, input.identity.regions) ||
        preserves_quality(baseline_quality, candidate_quality, input.identity.regions)) {
        baseline.result.notices.push_back("Final semantic quality gate retained ordinary matching: the preference did not improve overall and protected-region quality without regression after boundary refinement.");
        return finish(std::move(baseline));
    }
    candidate.result.notices.push_back("Final semantic quality gate accepted the preference: overall coverage, mean, P95, worst error and protected-region quality did not regress after boundary refinement.");
    return finish(std::move(candidate));
}

// A local material lock may share the existing group using that material. Do
// not reserve another quantization seed and displace unrelated dark surfaces
// merely because the locked patch has a different source color.
inline LocalPrintColorMatching::Computation compute_guarded(
    const LocalPrintColorMatching::Input& input, const indexed_triangle_set& mesh)
{
    namespace Matching = LocalPrintColorMatching;
    auto baseline = compute_quality_guarded(input, mesh);
    if (!baseline.ok()) return baseline; // Preserve input validation and conflicts.
    baseline.result.algorithm_version = baseline.result.mode == AI::PrintColorMode::Layered ?
        "region-layered-v2-boundary-v2-patches-v3-quality-v1" : "region-direct-v5-boundary-v1-quality-v1-locks-v1";
    auto unlocked = input;
    std::map<size_t, size_t> locks;
    for (auto& region : unlocked.identity.regions) {
        if (!region.locked_physical_slot) continue;
        for (size_t f : region.faces) locks.emplace(f, *region.locked_physical_slot);
        region.locked_physical_slot.reset();
        // This region represents a material choice, not an additional request
        // to reserve its original color. Other explicit protections stay intact.
        region.user_protected = false;
        region.protect_color = false;
    }
    if (locks.empty() || baseline.result.mode != AI::PrintColorMode::Direct) return baseline;
    auto alternative = compute_quality_guarded(unlocked, mesh);
    if (alternative.cancelled) return alternative;
    if (!alternative.ok()) return baseline;
    auto& candidate = alternative.result;
    std::map<size_t, size_t> slot_targets;
    for (size_t t = 0; t < candidate.targets.size(); ++t) {
        const auto& target = candidate.targets[t];
        if (!target.executable || !target.physical_slot || target.recipe ||
            !slot_targets.emplace(*target.physical_slot, t).second) return baseline;
    }
    for (const auto& lock : locks) {
        const auto found = slot_targets.find(lock.second);
        if (found == slot_targets.end()) return baseline; // No invented or duplicate slot.
        candidate.face_targets[lock.first] = found->second;
    }
    candidate.regions = input.identity.regions;
    candidate.algorithm_version = baseline.result.algorithm_version;
    auto cancelled = [&] { return input.cancelled && input.cancelled(); };
    auto stop = [&] { alternative.cancelled = true; return alternative; };
    auto samples = input.faces;
    for (const auto& edit : candidate.user_overrides) samples[edit.first].color = edit.second;
    std::vector<double> areas(candidate.targets.size(), 0), residuals(candidate.targets.size(), 0);
    std::vector<std::array<double, 3>> sources(candidate.targets.size());
    for (size_t f = 0; f < samples.size(); ++f) areas[candidate.face_targets[f]] += samples[f].area;
    for (size_t f = 0; f < samples.size(); ++f) {
        if (f % 4096 == 0 && cancelled()) return stop();
        const size_t t = candidate.face_targets[f];
        const double fraction = samples[f].area / areas[t];
        residuals[t] += fraction * Matching::delta_e(samples[f].color, candidate.targets[t].output);
        for (size_t c = 0; c < 3; ++c) sources[t][c] += fraction * samples[f].color[c];
    }
    std::vector<size_t> remap(candidate.targets.size(), size_t(-1));
    std::vector<AI::PrintColorTarget> targets;
    for (size_t t = 0; t < candidate.targets.size(); ++t) {
        if (areas[t] == 0) continue;
        auto target = candidate.targets[t];
        target.area = areas[t]; target.delta_e00 = residuals[t];
        for (size_t c = 0; c < 3; ++c) target.source[c] = float(std::clamp(sources[t][c], 0.0, 1.0));
        target.evidence = AI::ColorEvidence::Estimated;
        target.within_tolerance = target.delta_e00 <= candidate.color_tolerance;
        target.unresolved_reason = target.within_tolerance ? "" : "Display-color estimate exceeds the requested tolerance.";
        remap[t] = targets.size(); targets.push_back(std::move(target));
    }
    for (auto& t : candidate.face_targets) t = remap[t];
    candidate.targets = std::move(targets);
    // Recheck contrast on the final partition using the same area-majority
    // region definition as matching. Contract validation alone does not enforce
    // hard contrast. Soft contrast may not regress either.
    auto region_output = [&](const auto& result, const std::string& id) {
        const auto region = std::find_if(result.regions.begin(), result.regions.end(),
            [&](const auto& item) { return item.id == id; });
        std::vector<double> mass(result.targets.size(), 0);
        for (size_t f : region->faces) mass[result.face_targets[f]] += samples[f].area;
        return result.targets[size_t(std::max_element(mass.begin(), mass.end()) - mass.begin())].output;
    };
    for (const auto& constraint : candidate.contrasts) {
        const double next = Matching::delta_e(region_output(candidate, constraint.first_region), region_output(candidate, constraint.second_region));
        const double prior = Matching::delta_e(region_output(baseline.result, constraint.first_region), region_output(baseline.result, constraint.second_region));
        if ((constraint.hard && next < constraint.minimum_output_delta_e) || next + 1e-9 < prior) return baseline;
    }
    std::string error;
    if (!candidate.valid(error)) return baseline;
    if (cancelled()) return stop();
    const auto next = LocalPrintColorQuality::evaluate(input.faces, candidate);
    const auto prior = LocalPrintColorQuality::evaluate(input.faces, baseline.result);
    if (cancelled()) return stop();
    if (!preserves_quality(next, prior, input.identity.regions) ||
        preserves_quality(prior, next, input.identity.regions)) return baseline;
    // Earlier notices describe the pre-lock partition and have stale residuals.
    candidate.notices.clear();
    candidate.notices.push_back("Local material locks reused existing physical groups: overall and protected-region quality did not regress; unselected faces retain the unlocked matching outputs.");
    return alternative;
}
} // namespace Slic3r::GUI::LocalPrintColorBoundaryRefinement
