#pragma once

#include "LocalPrintColorMatching.hpp"
#include <array>
#include <unordered_map>

namespace Slic3r::GUI::LocalPrintColorPatchRefinement {
struct Outcome {
    std::vector<size_t> assignment;
    bool cancelled {false};
    size_t islands {0}, detail_patches {0};
};

// Internal adapter: the caller validates the result, samples and closed native
// adjacency, and supplies a final physical topology/boundary acceptance gate.
template<class Edges, class Gate>
Outcome refine(const std::vector<LocalPrintColorMatching::FaceSample>& samples,
               const AI::LocalPrintColorResult& result, const std::vector<size_t>& initial,
               const std::vector<uint8_t>& frozen, const std::vector<size_t>& physical_group,
               const Edges& edges, const std::vector<std::array<uint32_t,3>>& incident,
               const std::function<bool()>& cancelled, Gate&& global_gate)
{
    namespace Matching = LocalPrintColorMatching;
    Outcome out; out.assignment=initial;
    const size_t count=initial.size(), target_count=result.targets.size();
    auto stop=[&] { out.cancelled=true; return out; };
    auto is_cancelled=[&] {return cancelled && cancelled();};
    auto neighbor=[&](size_t f,uint32_t edge) {return size_t(edges[edge].first==f ? edges[edge].second : edges[edge].first);};
    auto error=[&](size_t f,size_t target) {return Matching::delta_e(samples[f].color,result.targets[target].output);};
    struct Region {
        const AI::PrintColorRegion* source;
        std::vector<std::array<int32_t,3>> neighbors;
        std::vector<size_t> components;
        size_t dominant;
    };
    std::vector<Region> regions;
    std::vector<uint8_t> critical(count,0);
    auto dominant=[&](const Region& r) {
        std::vector<double> mass(target_count,0);
        for(size_t f:r.source->faces) mass[physical_group[out.assignment[f]]]+=samples[f].area;
        return size_t(std::max_element(mass.begin(),mass.end())-mass.begin());
    };
    auto components=[&](const Region& r) {
        const auto& faces=r.source->faces;
        std::vector<uint8_t> seen(faces.size(),0);
        std::vector<size_t> counts(target_count,0), queue;
        for(size_t start=0;start<faces.size();++start) {
            if(seen[start]) continue;
            const size_t group=physical_group[out.assignment[faces[start]]]; ++counts[group];
            seen[start]=1; queue={start};
            for(size_t at=0;at<queue.size();++at)
                for(int32_t n:r.neighbors[queue[at]])
                    if(n>=0 && !seen[size_t(n)] && physical_group[out.assignment[faces[size_t(n)]]]==group) {
                        seen[size_t(n)]=1; queue.push_back(size_t(n));
                    }
        }
        return counts;
    };
    for(const auto& source:result.regions) {
        if(is_cancelled()) return stop();
        if(source.id.compare(0,14,"auto-semantic:")!=0 || source.confidence<.5 || source.faces.empty()) continue;
        Region r; r.source=&source; r.neighbors.resize(source.faces.size());
        std::unordered_map<size_t,int32_t> local;
        for(size_t i=0;i<source.faces.size();++i) local.emplace(source.faces[i],int32_t(i));
        for(size_t i=0;i<source.faces.size();++i) {
            const size_t f=source.faces[i];
            for(size_t c=0;c<3;++c) {
                const auto found=local.find(neighbor(f,incident[f][c]));
                r.neighbors[i][c]=found==local.end() ? -1 : found->second;
            }
            if(source.protect_color && !frozen[f]) critical[f]=1;
        }
        r.dominant=dominant(r); r.components=components(r); regions.push_back(std::move(r));
    }
    std::vector<size_t> population(target_count,0);
    for(size_t t:out.assignment) ++population[t];
    struct Patch { std::vector<size_t> faces; size_t target; double gain; };
    auto prioritize=[](std::vector<Patch>& patches) {
        std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b) {
            if(a.gain!=b.gain) return a.gain>b.gain;
            return a.faces.front()<b.faces.front();
        });
        // Bound expensive regional checks even for highly fragmented surfaces.
        if(patches.size()>256) patches.resize(256);
    };
    auto apply=[&](const Patch& patch) {
        std::vector<size_t> old, removed(target_count,0); old.reserve(patch.faces.size());
        std::unordered_map<size_t,size_t> prior_labels;
        prior_labels.reserve(patch.faces.size());
        bool touches_destination=false;
        for(size_t f:patch.faces) {
            if(frozen[f] || error(f,patch.target)>=error(f,out.assignment[f])-1e-9) return false;
            old.push_back(out.assignment[f]); ++removed[out.assignment[f]];
            prior_labels.emplace(f,out.assignment[f]);
            for(uint32_t edge:incident[f])
                touches_destination |= physical_group[out.assignment[neighbor(f,edge)]]==physical_group[patch.target];
        }
        if(!touches_destination) return false;
        auto next=population;
        for(size_t t=0;t<target_count;++t) next[t]-=removed[t];
        next[patch.target]+=patch.faces.size();
        if(std::find(next.begin(),next.end(),0)!=next.end()) return false;
        for(size_t f:patch.faces) out.assignment[f]=patch.target;
        auto whole_region_blocks=[&](const Region& r) {
            // A semantic mask can exclude the neighboring destination color.
            // Recoloring an entire old component is not splitting that region.
            // Inspect the state immediately before this patch so earlier island
            // removals cannot hide a partial split behind a lower total count.
            const auto& faces=r.source->faces;
            auto previous=[&](size_t i) {
                const auto found=prior_labels.find(faces[i]);
                return physical_group[found==prior_labels.end() ? out.assignment[faces[i]] : found->second];
            };
            std::vector<uint8_t> visited(faces.size(),0);
            std::vector<size_t> queue;
            for(size_t start=0;start<faces.size();++start) {
                if(start%4096==0 && is_cancelled()) {out.cancelled=true;return false;}
                const size_t before=previous(start), destination=physical_group[out.assignment[faces[start]]];
                if(visited[start] || before==destination) continue;
                visited[start]=1;queue={start};
                for(size_t at=0;at<queue.size();++at) {
                    if(at%4096==0 && is_cancelled()) {out.cancelled=true;return false;}
                    const size_t i=queue[at];
                    if(physical_group[out.assignment[faces[i]]]!=destination) return false;
                    for(int32_t n:r.neighbors[i])
                        if(n>=0 && !visited[size_t(n)] && previous(size_t(n))==before) {
                            visited[size_t(n)]=1;queue.push_back(size_t(n));
                        }
                }
            }
            return true;
        };
        bool valid=true;
        for(const auto& r:regions) {
            if(is_cancelled()) {out.cancelled=true;valid=false;break;}
            if(dominant(r)!=r.dominant) {valid=false;break;}
            const auto after=components(r);
            bool color_count_increased=false;
            for(size_t t=0;t<target_count;++t) color_count_increased |= after[t]>r.components[t];
            if(color_count_increased &&
               (std::accumulate(after.begin(),after.end(),size_t(0))>
                    std::accumulate(r.components.begin(),r.components.end(),size_t(0)) ||
                !whole_region_blocks(r))) valid=false;
            if(!valid) break;
        }
        if(!valid) {
            for(size_t i=0;i<patch.faces.size();++i) out.assignment[patch.faces[i]]=old[i];
            return false;
        }
        population=std::move(next); return true;
    };

    // Whole physical islands can move together even when no single triangle
    // can cross the local boundary barrier. Every face must improve.
    std::vector<uint8_t> seen(count,0);
    std::vector<Patch> patches;
    for(size_t start=0;start<count;++start) {
        if(start%4096==0 && is_cancelled()) return stop();
        if(seen[start]) continue;
        const size_t group=physical_group[initial[start]];
        std::vector<size_t> faces{start}; seen[start]=1;
        std::set<size_t> adjacent; bool locked=false;
        for(size_t at=0;at<faces.size();++at) {
            if(at%4096==0 && is_cancelled()) return stop();
            const size_t f=faces[at]; locked |= frozen[f]!=0;
            for(uint32_t edge:incident[f]) {
                const size_t n=neighbor(f,edge);
                if(physical_group[initial[n]]!=group) {adjacent.insert(initial[n]);continue;}
                if(!seen[n]) {seen[n]=1;faces.push_back(n);}
            }
        }
        if(locked) continue;
        double best=0; size_t destination=0;
        std::set<size_t> evaluated;
        for(size_t t:adjacent) {
            if(!evaluated.insert(physical_group[t]).second) continue;
            double gain=0; bool improves=true;
            for(size_t i=0;i<faces.size();++i) {
                if(i%4096==0 && is_cancelled()) return stop();
                const size_t f=faces[i]; const double difference=error(f,initial[f])-error(f,t);
                if(difference<=1e-9) {improves=false;break;}
                gain+=samples[f].area*difference;
            }
            if(improves && gain>best) {best=gain;destination=t;}
        }
        if(best>0) patches.push_back({std::move(faces),destination,best});
    }
    prioritize(patches);
    for(const auto& patch:patches) {
        if(out.cancelled || is_cancelled()) return stop();
        out.islands+=apply(patch);
    }
    if(out.cancelled || is_cancelled()) return stop();
    if(!global_gate(out.assignment)) {
        out.assignment=initial;out.islands=0;
        std::fill(population.begin(),population.end(),0);for(size_t t:out.assignment) ++population[t];
    }
    if(is_cancelled()) return stop();

    for(size_t round=0;round<8;++round) {
        auto desired=out.assignment;
        for(size_t f=0;f<count;++f) {
            if(f%4096==0 && is_cancelled()) return stop();
            if(!critical[f]) continue;
            double best=error(f,desired[f]);
            std::set<size_t> candidates;
            for(uint32_t edge:incident[f]) candidates.insert(out.assignment[neighbor(f,edge)]);
            for(size_t t:candidates) {
                const double residual=error(f,t);
                if(residual<best-1e-9) {best=residual;desired[f]=t;}
            }
        }
        std::fill(seen.begin(),seen.end(),0); patches.clear();
        for(size_t start=0;start<count;++start) {
            if(start%4096==0 && is_cancelled()) return stop();
            if(seen[start] || desired[start]==out.assignment[start]) continue;
            Patch patch{{start},desired[start],0};seen[start]=1;
            for(size_t at=0;at<patch.faces.size();++at) {
                if(at%4096==0 && is_cancelled()) return stop();
                const size_t f=patch.faces[at];
                patch.gain+=samples[f].area*(error(f,out.assignment[f])-error(f,patch.target));
                patch.target=std::min(patch.target,desired[f]);
                for(uint32_t edge:incident[f]) {
                    const size_t n=neighbor(f,edge);
                    if(!seen[n] && desired[n]!=out.assignment[n] &&
                       physical_group[out.assignment[n]]==physical_group[out.assignment[start]] &&
                       physical_group[desired[n]]==physical_group[desired[start]]) {seen[n]=1;patch.faces.push_back(n);}
                }
            }
            patches.push_back(std::move(patch));
        }
        prioritize(patches);
        const auto prior=out.assignment; const auto prior_population=population;
        size_t applied=0;
        for(const auto& patch:patches) {
            if(out.cancelled || is_cancelled()) return stop();
            applied+=apply(patch);
        }
        if(out.cancelled || is_cancelled()) return stop();
        if(applied==0) break;
        if(!global_gate(out.assignment)) {out.assignment=prior;population=prior_population;break;}
        if(is_cancelled()) return stop();
        out.detail_patches+=applied;
    }
    return out;
}
} // namespace Slic3r::GUI::LocalPrintColorPatchRefinement
