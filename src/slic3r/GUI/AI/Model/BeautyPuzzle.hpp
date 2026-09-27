#pragma once

#include "BeautySurface.hpp"
#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Slic3r::AI {

// A mutually exclusive partition of existing triangles. IDs belong to pieces,
// not palette slots; missing entries in colors mean "keep the source texture".
// Mutations are transactions and never change mesh topology or source pixels.
struct BeautyPuzzle {
    std::string geometry_id;
    std::vector<uint32_t> face_piece;
    std::map<uint32_t, std::array<float, 4>> colors;
    // Desired appearance before quantization. Missing on legacy records and
    // explicit material paint: keep their saved appearance as the fallback.
    std::map<uint32_t, std::array<float, 4>> target_colors;
    // Palette identity and physical assignments survive equal-RGB filaments.
    // Empty retains the legacy full-color document and its custom paint.
    std::vector<PhysicalFilamentChannel> palette;
    std::map<uint32_t, size_t> filament_slots;
    std::vector<MixedColorRecipe> mixed_recipes;
    uint32_t next_id {1};

    static constexpr size_t max_faces = 2000000;
    static constexpr uint32_t max_id = 1000000000;

    static std::array<float, 4> filament_color(const PhysicalFilamentChannel& channel) {
        require(is_rgb_hex_color(channel.display_color), "Invalid filament color.");
        std::array<float, 4> rgb {0,0,0,1};
        for (size_t c=0;c<3;++c) rgb[c]=float(std::stoul(channel.display_color.substr(1+2*c,2),nullptr,16))/255.f;
        return rgb;
    }
    bool same_palette(const std::vector<PhysicalFilamentChannel>& other) const {
        if(palette.size()!=other.size())return false;
        for(size_t i=0;i<palette.size();++i) {
            const auto& a=palette[i];const auto& b=other[i];
            if(a.slot!=b.slot || a.display_color!=b.display_color || a.material_type!=b.material_type || a.compatible!=b.compatible)return false;
        }
        return true;
    }
    bool same_edit(const BeautyPuzzle& other) const {
        return face_piece==other.face_piece && colors==other.colors && target_colors==other.target_colors && filament_slots==other.filament_slots && same_palette(other.palette) && same_mixed_palette(other.mixed_recipes);
    }
    bool same_mixed_palette(const std::vector<MixedColorRecipe>& other) const {
        return mixed_recipes.size()==other.size() && std::equal(mixed_recipes.begin(),mixed_recipes.end(),other.begin(),same_native_mixed_recipe);
    }
    size_t nearest_filament(const std::array<float,4>& color,
                            const std::vector<MixedColorRecipe>& candidates = {}) const {
        double best=std::numeric_limits<double>::infinity();size_t slot=SIZE_MAX;
        const auto consider=[&](size_t candidate,const std::array<float,4>& rgb) {
            // A semantic skin label is not evidence of illumination. Keep the
            // full perceptual lightness term, including for dark skin and lips.
            const double d=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
                {color[0],color[1],color[2]},{rgb[0],rgb[1],rgb[2]});
            if(d<best){best=d;slot=candidate;}
        };
        for(const auto& channel:palette) if(channel.compatible) {
            consider(channel.slot,filament_color(channel));
        }
        require(slot!=SIZE_MAX,"No compatible physical filament is available.");
        // Physical slots win exact ties. Only existing, validated native
        // recipes participate; matching never creates project configuration.
        if(valid_native_mixed_palette(palette,candidates))
            for(const auto& recipe:candidates)if(recipe.uniform_color)
                consider(*recipe.existing_virtual_slot,filament_color({*recipe.existing_virtual_slot,recipe.target_color,{},true}));
        return slot;
    }
    void paint_filament(uint32_t id,size_t slot) {
        require_piece(id);
        const auto it=std::find_if(palette.begin(),palette.end(),[&](const auto& c){return c.slot==slot && c.compatible;});
        if(it!=palette.end())colors[id]=filament_color(*it);
        else {
            const auto recipe=std::find_if(mixed_recipes.begin(),mixed_recipes.end(),[&](const auto& r){return r.existing_virtual_slot==slot;});
            require(recipe!=mixed_recipes.end(),"The selected filament is unavailable.");
            colors[id]=filament_color({slot,recipe->target_color,{},true});
        }
        filament_slots[id]=slot;
        target_colors.erase(id);
    }
    void paint_mixed(uint32_t id,const MixedColorRecipe& recipe) {
        require(valid_native_mixed_palette(palette,{recipe}),"The selected native mixed filament is unavailable.");
        auto found=std::find_if(mixed_recipes.begin(),mixed_recipes.end(),[&](const auto& r){return r.existing_virtual_slot==recipe.existing_virtual_slot;});
        if(found==mixed_recipes.end())mixed_recipes.push_back(recipe);else *found=recipe;
        paint_filament(id,*recipe.existing_virtual_slot);
    }
    // One area-weighted source color per independently editable piece. No
    // nearest-color classification is performed per triangle or texture pixel.
    void match_filaments(const BeautySurface& surface,const std::vector<PhysicalFilamentChannel>& channels,
                         const std::vector<MixedColorRecipe>& available_mixed={},
                         const std::vector<RGBA>& source_faces={}) {
        check_surface(surface);
        require(source_faces.empty() || source_faces.size()==face_piece.size(),"Source colors belong to different geometry.");
        if(!is_valid_physical_channel_set(channels) || std::none_of(channels.begin(),channels.end(),[](const auto& c){return c.compatible;}))return;
        const bool unchanged=same_palette(channels);
        const bool previously_matched=!palette.empty();
        // Keep only used recipes with an identical native definition. A changed
        // or removed project slot must never silently acquire another recipe.
        mixed_recipes.erase(std::remove_if(mixed_recipes.begin(),mixed_recipes.end(),[&](const auto& recipe){
            return !unchanged || std::none_of(available_mixed.begin(),available_mixed.end(),[&](const auto& r){return same_native_mixed_recipe(r,recipe);});
        }),mixed_recipes.end());
        if(!unchanged) {
            for(auto it=filament_slots.begin();it!=filament_slots.end();) {
                // Explicit physical paint survives unrelated palette changes,
                // even when another material has identical RGB. Automatic and
                // custom RGB targets are rematched against the new palette.
                const auto old=std::find_if(palette.begin(),palette.end(),[&](const auto& c){return c.slot==it->second;});
                const bool retain=!target_colors.count(it->first) && old!=palette.end() &&
                    std::any_of(channels.begin(),channels.end(),[&](const auto& c){return c.slot==old->slot &&
                        c.display_color==old->display_color && c.material_type==old->material_type && c.compatible;});
                if(retain)++it;else it=filament_slots.erase(it);
            }
        }
        for(auto it=filament_slots.begin();it!=filament_slots.end();)
            if(!native_palette_has_slot(channels,mixed_recipes,it->second))it=filament_slots.erase(it);else ++it;
        palette=channels;
        std::unordered_set<uint32_t> assigned;
        for(const auto& item:filament_slots)assigned.insert(item.first);
        if(unchanged && std::all_of(face_piece.begin(),face_piece.end(),[&](uint32_t id){return assigned.count(id)!=0;}))return;
        struct Mean {std::array<double,3> rgb {};double area=0;};
        std::map<uint32_t,Mean> means;
        for(size_t f=0;f<face_piece.size();++f) {
            if(assigned.count(face_piece[f]))continue;
            auto& mean=means[face_piece[f]];const double area=std::max(surface.areas[f],1e-15);mean.area+=area;
            for(size_t c=0;c<3;++c)mean.rgb[c]+=area*(source_faces.empty()?surface.patches[surface.face_patch[f]].mean_color[c]:source_faces[f][c]);
        }
        for(const auto& entry:means) {
            if(unchanged && filament_slots.count(entry.first))continue;
            const auto painted=colors.find(entry.first);const auto& mean=entry.second;
            const auto target=target_colors.find(entry.first);
            const bool retain_target=target!=target_colors.end() || painted==colors.end() || !previously_matched;
            const auto color=target!=target_colors.end()?target->second:painted==colors.end()?std::array<float,4>{float(mean.rgb[0]/mean.area),float(mean.rgb[1]/mean.area),float(mean.rgb[2]/mean.area),1}:painted->second;
            // Saved/manual paint (including a removed recipe) is never rebound
            // to a newly changed virtual slot. New automatic pieces may reuse
            // the project's native mixtures and save their exact definitions.
            const auto slot=nearest_filament(color,painted==colors.end()?available_mixed:std::vector<MixedColorRecipe>{});
            const auto recipe=std::find_if(available_mixed.begin(),available_mixed.end(),[&](const auto& r){return r.existing_virtual_slot==slot &&
                std::none_of(palette.begin(),palette.end(),[&](const auto& c){return c.slot==slot;});});
            if(recipe!=available_mixed.end())paint_mixed(entry.first,*recipe);
            else paint_filament(entry.first,slot);
            if(retain_target)target_colors[entry.first]=color;
        }
    }
    void restore_source_color(uint32_t id,const BeautySurface& surface) {
        auto original=*this;original.clear_color(id);
        const auto color=original.representative_color(id,surface);
        if(palette.empty())clear_color(id);else {paint_filament(id,nearest_filament(color));target_colors[id]=color;}
    }

    static BeautyPuzzle create(const BeautySurface& surface, size_t target_count = 180,
                              const std::function<bool()>& canceled = {})
    {
        check_surface_data(surface);
        require(target_count > 0 && target_count <= 10000, "Choose between 1 and 10000 puzzle pieces.");
        auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
        };
        checkpoint();
        // Start from connected micro-patches. Decomposing here also handles a
        // saved BeautySurface partition whose IDs cover disconnected islands.
        struct Node {
            double area {0};
            Vec3d center {Vec3d::Zero()}, normal {Vec3d::Zero()};
            std::array<double, 3> lab {};
            std::set<uint32_t> neighbors;
            uint32_t parent {0}, revision {0};
            bool alive {true};
        };
        const size_t count = surface.face_patch.size();
        const uint32_t missing = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> atoms(count, missing);
        std::vector<std::array<double, 3>> patch_lab;
        patch_lab.reserve(surface.patches.size());
        for (const auto& patch : surface.patches) patch_lab.push_back(to_lab(patch.mean_color));
        std::vector<Node> nodes;
        nodes.reserve(surface.patches.size());
        std::vector<size_t> queue;
        double total_area = 0;
        for (size_t seed = 0; seed < count; ++seed) {
            if ((seed & 4095) == 0) checkpoint();
            if (atoms[seed] != missing) continue;
            const uint32_t id = uint32_t(nodes.size());
            nodes.emplace_back();
            auto& node = nodes.back();
            node.parent = id;
            queue.assign(1, seed);
            atoms[seed] = id;
            for (size_t at = 0; at < queue.size(); ++at) {
                if ((at & 4095) == 0) checkpoint();
                const size_t f = queue[at];
                const double weight = std::max(surface.areas[f], 1e-15);
                node.area += weight;
                node.center += surface.centers[f] * weight;
                node.normal += surface.normals[f] * weight;
                for (size_t ch = 0; ch < 3; ++ch)
                    node.lab[ch] += patch_lab[surface.face_patch[f]][ch] * weight;
                for (const int32_t n : surface.face_neighbors[f]) {
                    if (n >= 0 && atoms[n] == missing && surface.face_patch[n] == surface.face_patch[f]) {
                        atoms[n] = id;
                        queue.push_back(size_t(n));
                    }
                }
            }
            node.center /= node.area;
            for (double& ch : node.lab) ch /= node.area;
            total_area += node.area;
        }
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            for (const int32_t n : surface.face_neighbors[f])
                if (n >= 0 && atoms[f] != atoms[n]) nodes[atoms[f]].neighbors.insert(atoms[n]);
        }
        const double target_area = total_area / double(std::min(target_count, count));
        struct Edge { double cost; uint32_t a, b, va, vb; };
        struct Later {
            bool operator()(const Edge& a, const Edge& b) const {
                if (a.cost != b.cost) return a.cost > b.cost;
                if (a.a != b.a) return a.a > b.a;
                return a.b > b.b;
            }
        };
        std::priority_queue<Edge, std::vector<Edge>, Later> edges;
        auto enqueue = [&](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            const auto& x = nodes[a];
            const auto& y = nodes[b];
            double color_distance = 0;
            for (size_t ch = 0; ch < 3; ++ch) color_distance += std::pow(x.lab[ch] - y.lab[ch], 2) / 1600.;
            const double nx = x.normal.norm(), ny = y.normal.norm();
            const double bend = nx > 1e-15 && ny > 1e-15
                ? 1. - std::clamp(x.normal.dot(y.normal) / (nx * ny), -1., 1.) : 0.;
            const double spatial = (x.center - y.center).squaredNorm() / target_area;
            const double size = (x.area + y.area) / target_area;
            const double ward = (x.area / (x.area + y.area)) * (y.area / target_area);
            // Ward-like adjacent-region merging suppresses tiny noisy pieces;
            // surface area and compactness avoid tessellation-dependent strips.
            const double cost = ward * (.03 + 4. * color_distance + .5 * bend + .15 * spatial)
                + .012 * size * size;
            edges.push({cost, a, b, x.revision, y.revision});
        };
        auto rebuild_edges = [&] {
            edges = std::priority_queue<Edge, std::vector<Edge>, Later>();
            for (uint32_t a = 0; a < nodes.size(); ++a) {
                if ((a & 255) == 0) checkpoint();
                if (nodes[a].alive)
                    for (const uint32_t b : nodes[a].neighbors) if (a < b) enqueue(a, b);
            }
        };
        rebuild_edges();
        size_t pieces = nodes.size(), attempts = 0;
        while (pieces > target_count && !edges.empty()) {
            if ((attempts++ & 255) == 0) checkpoint();
            const Edge edge = edges.top();
            edges.pop();
            auto& a = nodes[edge.a];
            auto& b = nodes[edge.b];
            if (!a.alive || !b.alive || a.revision != edge.va || b.revision != edge.vb) continue;
            const double merged_area = a.area + b.area;
            a.center = (a.center * a.area + b.center * b.area) / merged_area;
            for (size_t ch = 0; ch < 3; ++ch) a.lab[ch] = (a.lab[ch] * a.area + b.lab[ch] * b.area) / merged_area;
            a.normal += b.normal;
            a.area = merged_area;
            ++a.revision;
            b.alive = false;
            b.parent = edge.a;
            a.neighbors.erase(edge.b);
            for (const uint32_t neighbor : b.neighbors) {
                if (neighbor == edge.a) continue;
                nodes[neighbor].neighbors.erase(edge.b);
                nodes[neighbor].neighbors.insert(edge.a);
                a.neighbors.insert(neighbor);
            }
            b.neighbors.clear();
            --pieces;
            for (const uint32_t neighbor : a.neighbors) enqueue(edge.a, neighbor);
            // Do not retain an unbounded heap of obsolete boundary scores.
            if (edges.size() > nodes.size() * 24 + 1024) rebuild_edges();
        }
        BeautyPuzzle result;
        result.geometry_id = surface.geometry_id;
        result.face_piece.resize(count);
        std::vector<uint32_t> piece_id(nodes.size(), 0);
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            uint32_t root = atoms[f];
            while (nodes[root].parent != root) root = nodes[root].parent;
            uint32_t current = atoms[f];
            while (nodes[current].parent != current) {
                const uint32_t parent = nodes[current].parent;
                nodes[current].parent = root;
                current = parent;
            }
            if (piece_id[root] == 0) piece_id[root] = result.next_id++;
            result.face_piece[f] = piece_id[root];
        }
        checkpoint();
        return result;
    }

    // A coarse editing partition, distinct from the legacy micro-piece preview.
    // The caller must bind semantic labels to this exact surface before passing
    // them in; -1 is unknown, nonnegative values identify semantic regions.
    static BeautyPuzzle create_regions(const BeautySurface& surface,
        const std::vector<int32_t>& semantic_labels = {}, const std::function<bool()>& canceled = {},
        const std::vector<std::string>& semantic_names = {})
    {
        require(semantic_labels.empty() || semantic_labels.size() == surface.face_patch.size(),
                "Semantic hints belong to different geometry.");
        for (const int32_t label : semantic_labels) require(label >= -1, "Invalid semantic region hint.");
        auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
        };
        BeautyPuzzle result = create(surface, 28, canceled);
        result.regularize_boundaries(surface, canceled);
        result.smooth_partition(surface, {}, {}, canceled);
        if (semantic_labels.empty() || std::all_of(semantic_labels.begin(), semantic_labels.end(),
                                                  [](int32_t label) { return label < 0; })) return result;

        const size_t count = result.face_piece.size();
        const auto coarse = result.face_piece;
        std::vector<double> coarse_area(result.next_id, 0.);
        std::vector<std::array<double, 3>> patch_lab;
        for (const auto& patch : surface.patches) patch_lab.push_back(to_lab(patch.mean_color));
        double total_area = 0., edge_sum = 0.;
        size_t edge_count = 0;
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            coarse_area[coarse[f]] += std::max(surface.areas[f], 1e-15);
            total_area += std::max(surface.areas[f], 1e-15);
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0 && size_t(n) > f) {
                edge_sum += (surface.centers[f] - surface.centers[n]).norm();
                ++edge_count;
            }
        }
        const double mean_step = edge_count ? edge_sum / double(edge_count) : std::sqrt(total_area / double(count));
        struct Seed {
            uint32_t coarse_id; int32_t semantic_id;
            bool crease_guard {false};
            double area {0.}, radius {0.};
            std::array<double, 3> lab {};
            Vec3d normal {Vec3d::Zero()};
        };
        std::map<std::pair<uint32_t, int32_t>, uint32_t> group_ids;
        std::vector<Seed> seeds;
        std::vector<int32_t> group(count, -1);
        for (size_t f = 0; f < count; ++f) if (semantic_labels[f] >= 0) {
            if ((f & 4095) == 0) checkpoint();
            const auto key = std::make_pair(coarse[f], semantic_labels[f]);
            const auto inserted = group_ids.emplace(key, uint32_t(seeds.size()));
            if (inserted.second) seeds.push_back({coarse[f], semantic_labels[f]});
            auto& seed = seeds[inserted.first->second];
            group[f] = int32_t(inserted.first->second);
            const double area = std::max(surface.areas[f], 1e-15);
            seed.area += area;
            seed.normal += surface.normals[f] * area;
            for (size_t ch = 0; ch < 3; ++ch) seed.lab[ch] += patch_lab[surface.face_patch[f]][ch] * area;
        }
        for (auto& seed : seeds) {
            if(size_t(seed.semantic_id)<semantic_names.size()) {
                const auto& name=semantic_names[size_t(seed.semantic_id)];
                seed.crease_guard=name=="nose" || name=="le" || name=="re" || name=="iris" ||
                    name=="ulip" || name=="llip" || name=="imouth";
            }
            for (double& ch : seed.lab) ch /= seed.area;
            if (seed.normal.norm() > 1e-15) seed.normal.normalize();
            // Sparse eye/nose samples may fill local holes, never an entire
            // unobserved cheek or garment. The coarse boundary is also hard.
            seed.radius = std::max(2.5 * mean_step,
                std::min(.24 * std::sqrt(coarse_area[seed.coarse_id]), 2. * std::sqrt(seed.area)));
        }
        using Entry = std::pair<double, size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        std::vector<uint8_t> settled(count, 0);
        for (size_t f = 0; f < count; ++f) if (group[f] >= 0) { distance[f] = 0.; settled[f] = 1; }
        auto offer = [&](size_t from, size_t to, double d, int32_t owner) {
            if (settled[to]) return;
            const auto& seed = seeds[size_t(owner)];
            if (coarse[to] != seed.coarse_id) return;
            // Unknown faces are reached along the surface, not just by distance.
            // Penalize pronounced local folds, not accumulated gentle curvature:
            // the latter shrinks cheeks and can strand reliable feature seeds.
            // This is a conservative crease guard, not anatomical inference.
            // Radius scaling preserves behavior under a change of model units.
            // Apply only to named facial features. Hair/skin/clothing and legacy
            // unnamed hints retain their established propagation behavior.
            const double turn = seed.crease_guard ? std::max(0., std::acos(std::clamp(surface.normals[from].dot(surface.normals[to]), -1., 1.)) - .35) : 0.;
            const double next = d + std::max((surface.centers[from] - surface.centers[to]).norm(), 1e-12)
                + .35 * seed.radius * turn;
            if (next > seed.radius || next >= distance[to]) return;
            double color_distance = 0.;
            for (size_t ch = 0; ch < 3; ++ch)
                color_distance += std::pow(patch_lab[surface.face_patch[to]][ch] - seed.lab[ch], 2);
            if (color_distance > 24. * 24. || seed.normal.dot(surface.normals[to]) < .25) return;
            distance[to] = next; group[to] = owner;
            pending.emplace(next, to);
        };
        for (size_t f = 0; f < count; ++f) if (semantic_labels[f] >= 0)
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0) offer(f, size_t(n), 0., group[f]);
        size_t attempts = 0;
        while (!pending.empty()) {
            if ((attempts++ & 4095) == 0) checkpoint();
            const auto [d, f] = pending.top(); pending.pop();
            if (settled[f] || d != distance[f]) continue;
            settled[f] = 1;
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0) offer(f, size_t(n), d, group[f]);
        }
        // Semantics identify a body part, not a single pigment. Preserve strong
        // source-color divisions already found by the coarse partition (e.g.
        // beard versus cheek) instead of averaging the entire "face" label.
        struct Pigment {std::array<double,3> lab;uint32_t id;};
        std::map<int32_t,std::vector<Pigment>> pigments;
        std::map<int32_t,double> largest_seed;
        std::vector<size_t> seed_order(seeds.size());std::iota(seed_order.begin(),seed_order.end(),0);
        for(const auto& seed:seeds)largest_seed[seed.semantic_id]=std::max(largest_seed[seed.semantic_id],seed.area);
        std::sort(seed_order.begin(),seed_order.end(),[&](size_t a,size_t b){return seeds[a].area==seeds[b].area?a<b:seeds[a].area>seeds[b].area;});
        std::vector<uint32_t> pigment_ids(seeds.size());
        for(size_t i:seed_order) {
            const auto& seed=seeds[i];auto& choices=pigments[seed.semantic_id];
            double best=std::numeric_limits<double>::infinity();uint32_t id=0;
            for(const auto& pigment:choices) {
                const double d=tex2color::color_utils::calc_lab_color_difference_by_ciede2000(seed.lab,pigment.lab);
                if(d<best){best=d;id=pigment.id;}
            }
            // Small/noisy seeds join the nearest substantial pigment. A new
            // pigment must occupy at least 2% of this part's largest coarse seed.
            if(choices.empty() || (best>12. && seed.area>=.02*largest_seed[seed.semantic_id])) {
                id=result.allocate_id();choices.push_back({seed.lab,id});
            }
            pigment_ids[i]=id;
        }
        for (size_t f = 0; f < count; ++f) if (group[f] >= 0) {
            result.face_piece[f] = pigment_ids[size_t(group[f])];
        }
        // A triangle tooth can be removed without disconnecting its donor:
        // two neighboring faces already belong to its recipient. Actual seed
        // faces and strong source-color edges stay fixed.
        result.trim_boundary_teeth(surface, semantic_labels, canceled, &coarse);
        const std::set<uint32_t> affected(result.face_piece.begin(), result.face_piece.end());
        result.split_islands(affected, surface);
        result.absorb_small_unrecognized(surface, semantic_labels, coarse, total_area / 28. * .015, canceled);
        result.smooth_partition(surface, {}, semantic_labels, canceled, &coarse);
        checkpoint();
        return result;
    }

    size_t piece_count() const {
        return std::unordered_set<uint32_t>(face_piece.begin(), face_piece.end()).size();
    }

    std::vector<size_t> faces(uint32_t id) const {
        std::vector<size_t> result;
        for (size_t f = 0; f < face_piece.size(); ++f) if (face_piece[f] == id) result.push_back(f);
        return result;
    }

    std::array<float, 4> representative_color(uint32_t id, const BeautySurface& surface) const {
        check_surface(surface);
        require_piece(id);
        const auto found = colors.find(id);
        if (found != colors.end()) return found->second;
        std::array<double, 3> sum {};
        double weight_sum = 0;
        for (size_t f = 0; f < face_piece.size(); ++f) if (face_piece[f] == id) {
            const double weight = std::max(surface.areas[f], 1e-15);
            weight_sum += weight;
            for (size_t ch = 0; ch < 3; ++ch) sum[ch] += surface.patches[surface.face_patch[f]].mean_color[ch] * weight;
        }
        return {float(sum[0] / weight_sum), float(sum[1] / weight_sum), float(sum[2] / weight_sum), 1.f};
    }

    std::array<float,4> source_region_color(uint32_t id,const BeautySurface& surface,
                                           const std::vector<RGBA>& source) const {
        check_surface(surface);require_piece(id);
        require(source.size()==face_piece.size(),"Source colors belong to different geometry.");
        std::array<double,3> sum{};double total=0;
        for(size_t f=0;f<source.size();++f)if(face_piece[f]==id) {
            const double weight=std::max(surface.areas[f],1e-15);total+=weight;
            for(size_t c=0;c<3;++c)sum[c]+=weight*source[f][c];
        }
        return {float(sum[0]/total),float(sum[1]/total),float(sum[2]/total),1.f};
    }

    void paint(uint32_t id, const std::array<float, 4>& color) {
        check_state();
        require_piece(id);
        check_color(color);
        if(!palette.empty()){paint_filament(id,nearest_filament(color));target_colors[id]=color;}
        else colors[id] = color;
    }

    void clear_color(uint32_t id) {
        require_piece(id);
        colors.erase(id);
        filament_slots.erase(id);
        target_colors.erase(id);
    }

    // Visible strokes can contain isolated silhouette samples. A boundary drag
    // only transfers sampled components connected to the shared boundary.
    void resize_boundary(uint32_t id, const std::vector<size_t>& selected,
                         bool outward, const BeautySurface& surface) {
        check_surface(surface);
        require_piece(id);
        const auto indices = checked_selection(selected);
        std::vector<uint8_t> allowed(face_piece.size(), 0), visited(face_piece.size(), 0);
        for (size_t f : indices)
            if ((face_piece[f] != id) == outward) allowed[f] = 1;
        std::vector<size_t> connected;
        for (size_t f : indices) if (allowed[f]) {
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && ((face_piece[n] == id) == outward)) {
                    visited[f] = 1; connected.push_back(f); break;
                }
        }
        for (size_t i = 0; i < connected.size(); ++i)
            for (int32_t n : surface.face_neighbors[connected[i]])
                if (n >= 0 && allowed[n] && !visited[n]) {
                    visited[n] = 1; connected.push_back(size_t(n));
                }
        require(!connected.empty(), "请从选中区域的边界开始拖动。");
        if (outward) grow(id, connected, surface);
        else shrink(id, connected, surface);
    }

    // A drag is one transaction, even when different parts of its boundary move
    // in opposite directions. Screen sampling may miss a single triangle; close
    // those gaps before retaining only samples reached from the old boundary.
    // Unconnected silhouette samples are ignored, never made into new islands.
    void reshape_region(uint32_t id, const std::vector<size_t>& added,
                        const std::vector<size_t>& removed, const BeautySurface& surface,
                        const std::vector<int32_t>& semantic_labels = {},bool preserve_shape = false) {
        check_surface(surface);
        require(semantic_labels.empty() || semantic_labels.size()==face_piece.size(),"Face recognition belongs to another surface.");
        require_piece(id);
        if (added.empty() && removed.empty()) return;
        const size_t count = face_piece.size();
        auto boundary_samples = [&](const std::vector<size_t>& selected, bool outward) {
            std::vector<uint8_t> mask(count, 0), reached(count, 0);
            if (selected.empty()) return reached;
            const auto indices = checked_selection(selected);
            for (size_t f : indices) if ((face_piece[f] != id) == outward) mask[f] = 1;
            std::vector<size_t> gaps;
            for (size_t f : indices) if (mask[f])
                for (int32_t n : surface.face_neighbors[f])
                    if (n >= 0 && !mask[n] && ((face_piece[n] != id) == outward)) {
                        unsigned support = 0;
                        for (int32_t around : surface.face_neighbors[n])
                            if (around >= 0 && mask[around] && face_piece[around] == face_piece[n]) ++support;
                        if (support >= 2) gaps.push_back(size_t(n));
                    }
            if (!preserve_shape) for (size_t f : gaps) mask[f] = 1;
            std::vector<size_t> queue;
            for (size_t f = 0; f < count; ++f) if (mask[f])
                for (int32_t n : surface.face_neighbors[f])
                    if (n >= 0 && ((face_piece[n] == id) == outward)) {
                        reached[f] = 1; queue.push_back(f); break;
                    }
            for (size_t at = 0; at < queue.size(); ++at)
                for (int32_t n : surface.face_neighbors[queue[at]])
                    if (n >= 0 && mask[n] && !reached[n]) {
                        reached[n] = 1; queue.push_back(size_t(n));
                    }
            return reached;
        };
        const auto grow_mask = boundary_samples(added, true);
        const auto shrink_mask = boundary_samples(removed, false);
        BeautyPuzzle candidate = *this;
        std::set<uint32_t> affected {id};
        std::vector<uint8_t> changed(count, 0);
        for (size_t f = 0; f < count; ++f) if (grow_mask[f]) {
            affected.insert(face_piece[f]);
            candidate.face_piece[f] = id; changed[f] = 1;
        }
        using Entry = std::pair<double, size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        std::vector<uint32_t> recipient(count, 0);
        auto step = [&](size_t a, size_t b) {
            return std::max((surface.centers[a] - surface.centers[b]).norm(), 1e-12);
        };
        auto offer = [&](size_t f, double d, uint32_t owner) {
            if (d < distance[f] || (d == distance[f] && owner < recipient[f])) {
                distance[f] = d; recipient[f] = owner; pending.emplace(d, f);
            }
        };
        for (size_t f = 0; f < count; ++f) if (shrink_mask[f])
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && candidate.face_piece[n] != id)
                    offer(f, step(f, size_t(n)), candidate.face_piece[n]);
        while (!pending.empty()) {
            const auto [d, f] = pending.top(); pending.pop();
            if (d != distance[f]) continue;
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && shrink_mask[n]) offer(size_t(n), d + step(f, size_t(n)), recipient[f]);
        }
        for (size_t f = 0; f < count; ++f) if (recipient[f]) {
            candidate.face_piece[f] = recipient[f]; changed[f] = 1;
            affected.insert(recipient[f]);
        }
        if (std::none_of(changed.begin(), changed.end(), [](uint8_t value) { return value != 0; })) return;
        candidate.absorb_drag_fragments(*this, changed, surface);
        candidate.split_islands(affected, surface);
        candidate.prune_colors();
        // Keep the center of each transferred component: a small deliberate
        // movement must not disappear when its new edge is rounded afterward.
        std::vector<int32_t> anchors(count, -1);
        std::vector<uint8_t> visited(count, 0);
        std::vector<size_t> component;
        for (size_t seed = 0; seed < count; ++seed) if (changed[seed] && !visited[seed]) {
            component.assign(1, seed); visited[seed] = 1;
            Vec3d center = Vec3d::Zero();
            double component_area = 0.;
            for (size_t at = 0; at < component.size(); ++at) {
                const size_t f = component[at];
                const double area = std::max(surface.areas[f], 1e-15);
                center += surface.centers[f] * area; component_area += area;
                for (int32_t n : surface.face_neighbors[f])
                    if (n >= 0 && changed[n] && !visited[n] && candidate.face_piece[n] == candidate.face_piece[seed]) {
                        visited[n] = 1; component.push_back(size_t(n));
                    }
            }
            center /= component_area;
            const auto anchor = std::min_element(component.begin(), component.end(), [&](size_t a, size_t b) {
                return (surface.centers[a] - center).squaredNorm() < (surface.centers[b] - center).squaredNorm();
            });
            anchors[*anchor] = 0;
        }
        // Only the touched boundary and its immediate neighborhood are faired;
        // a drag must not unexpectedly edit another side of the model.
        // Explicit drag samples override recognition. Nearby automatic fairing
        // follows the recognized contour; an unknown face carries no guidance.
        const auto deliberate=changed;
        for (unsigned ring = 0; ring < 3; ++ring) {
            auto expanded = changed;
            for (size_t f = 0; f < count; ++f) if (changed[f])
                for (int32_t n : surface.face_neighbors[f]) if (n >= 0) expanded[n] = 1;
            changed.swap(expanded);
        }
        if(!preserve_shape)candidate.smooth_partition(surface, changed, anchors, {}, nullptr,
            semantic_labels.empty()?nullptr:&semantic_labels,&deliberate);
        *this = std::move(candidate);
    }

    // Also works on a restored v1 partition. IDs and color intent stay exact;
    // smoothing neither merges pieces nor changes source texture or geometry.
    void smooth_boundaries(const BeautySurface& surface) {
        check_surface(surface);
        BeautyPuzzle candidate = *this;
        candidate.smooth_partition(surface, {}, {}, {});
        *this = std::move(candidate);
    }

    // Grow transfers only the supplied faces. A remote selection is rejected;
    // removing a donor's neck creates independent pieces with inherited colors.
    void grow(uint32_t id, const std::vector<size_t>& selected, const BeautySurface& surface) {
        check_surface(surface);
        require_piece(id);
        const auto indices = checked_selection(selected);
        BeautyPuzzle candidate = *this;
        std::set<uint32_t> affected;
        for (size_t f : indices) {
            if (face_piece[f] != id) affected.insert(face_piece[f]);
            candidate.face_piece[f] = id;
        }
        require(candidate.connected(id, surface), "The added area must touch the selected piece.");
        candidate.split_islands(affected, surface);
        candidate.prune_colors();
        *this = std::move(candidate);
    }

    // Shrink is a multi-source geodesic transfer from adjacent other pieces.
    // Every removed component needs a recipient boundary; enclosed holes fail.
    void shrink(uint32_t id, const std::vector<size_t>& selected, const BeautySurface& surface) {
        check_surface(surface);
        require_piece(id);
        const auto indices = checked_selection(selected);
        std::vector<uint8_t> removed(face_piece.size(), 0);
        size_t removed_count = 0;
        for (size_t f : indices) if (face_piece[f] == id) { removed[f] = 1; ++removed_count; }
        require(removed_count != 0, "Select part of this piece to shrink it.");
        using Entry = std::pair<double, size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
        std::vector<double> distance(face_piece.size(), std::numeric_limits<double>::infinity());
        std::vector<uint32_t> recipient(face_piece.size(), 0);
        auto offer = [&](size_t f, double d, uint32_t target) {
            if (d < distance[f] || (d == distance[f] && target < recipient[f])) {
                distance[f] = d;
                recipient[f] = target;
                pending.emplace(d, f);
            }
        };
        auto step = [&](size_t a, size_t b) {
            return std::max((surface.centers[a] - surface.centers[b]).norm(), 1e-12);
        };
        for (size_t f : indices) if (removed[f])
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && face_piece[n] != id) offer(f, step(f, size_t(n)), face_piece[n]);
        while (!pending.empty()) {
            const auto [d, f] = pending.top();
            pending.pop();
            if (d != distance[f]) continue;
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && removed[n]) offer(size_t(n), d + step(f, size_t(n)), recipient[f]);
        }
        BeautyPuzzle candidate = *this;
        std::set<uint32_t> affected {id};
        for (size_t f : indices) if (removed[f]) {
            require(recipient[f] != 0, "This area has no adjacent piece to receive it. Split it instead.");
            candidate.face_piece[f] = recipient[f];
            affected.insert(recipient[f]);
        }
        candidate.split_islands(affected, surface);
        candidate.prune_colors();
        *this = std::move(candidate);
    }

    // The merged piece follows the target's color intent. An unpainted target
    // keeps the source texture; the removed source ID's override is discarded.
    void merge(uint32_t target, uint32_t other, const BeautySurface& surface) {
        check_surface(surface);
        require_piece(target);
        require_piece(other);
        require(target != other, "Select a different neighboring piece to merge.");
        bool adjacent = false;
        for (size_t f = 0; f < face_piece.size() && !adjacent; ++f) if (face_piece[f] == target)
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0 && face_piece[n] == other) adjacent = true;
        require(adjacent, "Only neighboring pieces can be merged.");
        BeautyPuzzle candidate = *this;
        for (auto& id : candidate.face_piece) if (id == other) id = target;
        require(candidate.connected(target, surface), "The merged piece must be connected.");
        candidate.colors.erase(other);
        candidate.filament_slots.erase(other);
        candidate.target_colors.erase(other);
        *this = std::move(candidate);
    }

    uint32_t split(uint32_t id, const std::vector<size_t>& selected, const BeautySurface& surface) {
        check_surface(surface);
        require_piece(id);
        const auto indices = checked_selection(selected);
        size_t taken = 0;
        for (size_t f : indices) if (face_piece[f] == id) ++taken;
        const size_t total = size_t(std::count(face_piece.begin(), face_piece.end(), id));
        require(taken > 0 && taken < total, "Select only part of this piece to split it.");
        BeautyPuzzle candidate = *this;
        const uint32_t created = candidate.allocate_id();
        const auto color = colors.find(id);
        if (color != colors.end()) candidate.colors[created] = color->second;
        if(filament_slots.count(id))candidate.filament_slots[created]=filament_slots.at(id);
        if(target_colors.count(id))candidate.target_colors[created]=target_colors.at(id);
        for (size_t f : indices) if (face_piece[f] == id) candidate.face_piece[f] = created;
        candidate.split_islands({id, created}, surface);
        *this = std::move(candidate);
        return created;
    }

    // An editing group can cross several print-color pieces. Cut only the
    // selected faces at each old piece boundary, preserving every donor's
    // color and exact filament slot until the explicit paint operation.
    // All selected components are painted in one transaction so undo can
    // restore the original partition and assignments together.
    void paint_faces_filament(const std::vector<size_t>& selected, const BeautySurface& surface, size_t slot) {
        require(native_palette_has_slot(palette, mixed_recipes, slot), "The selected filament is unavailable.");
        paint_faces(selected, surface, slot, {});
    }

    // Paint just this edit region before the user has chosen whole-model
    // filament matching. Other regions keep the source texture unchanged.
    void paint_faces_color(const std::vector<size_t>& selected, const BeautySurface& surface,
                           const std::array<float, 4>& color) {
        require(palette.empty(), "Use a filament for a matched puzzle.");
        check_color(color);
        paint_faces(selected, surface, std::nullopt, color);
    }

    void paint_faces_target(const std::vector<size_t>& selected, const BeautySurface& surface,
                            const std::array<float,4>& color) {
        check_color(color);
        if(palette.empty()){paint_faces_color(selected,surface,color);return;}
        auto candidate=*this;
        candidate.paint_faces_filament(selected,surface,nearest_filament(color));
        for(size_t f:selected)candidate.target_colors[candidate.face_piece[f]]=color;
        *this=std::move(candidate);
    }

    void paint_faces(const std::vector<size_t>& selected, const BeautySurface& surface,
                     std::optional<size_t> slot, const std::array<float, 4>& color) {
        check_surface(surface);
        const auto indices = checked_selection(selected);
        BeautyPuzzle candidate = *this;
        std::vector<uint8_t> chosen(face_piece.size(), 0);
        for (size_t f : indices) chosen[f] = 1;
        struct Component { uint32_t owner, id; double area; bool chosen; };
        std::vector<Component> components;
        std::vector<uint32_t> component_of(face_piece.size(), UINT32_MAX);
        std::unordered_map<uint32_t, uint32_t> largest;
        std::vector<size_t> queue;
        for (size_t seed = 0; seed < face_piece.size(); ++seed) {
            if (component_of[seed] != UINT32_MAX) continue;
            const uint32_t index = uint32_t(components.size());
            components.push_back({chosen[seed] ? 0 : face_piece[seed], 0, 0., chosen[seed] != 0});
            component_of[seed] = index;
            queue.assign(1, seed);
            for (size_t at = 0; at < queue.size(); ++at) {
                const size_t f = queue[at];
                components[index].area += std::max(surface.areas[f], 1e-15);
                for (int32_t neighbor : surface.face_neighbors[f])
                    if (neighbor >= 0 && component_of[neighbor] == UINT32_MAX &&
                        chosen[neighbor] == chosen[seed] &&
                        (chosen[seed] || face_piece[neighbor] == face_piece[seed])) {
                        component_of[neighbor] = index;
                        queue.push_back(size_t(neighbor));
                    }
            }
            if (!chosen[seed]) {
                const auto found = largest.find(face_piece[seed]);
                if (found == largest.end() || components[found->second].area < components[index].area)
                    largest[face_piece[seed]] = index;
            }
        }
        for (uint32_t index = 0; index < components.size(); ++index) {
            auto& component = components[index];
            component.id = !component.chosen && largest.at(component.owner) == index ? component.owner : candidate.allocate_id();
            if (!component.chosen && component.id != component.owner) {
                const auto color = colors.find(component.owner);
                if (color != colors.end()) candidate.colors[component.id] = color->second;
                const auto filament = filament_slots.find(component.owner);
                if (filament != filament_slots.end()) candidate.filament_slots[component.id] = filament->second;
                if(target_colors.count(component.owner))candidate.target_colors[component.id]=target_colors.at(component.owner);
            }
        }
        for (size_t f = 0; f < face_piece.size(); ++f)
            candidate.face_piece[f] = components[component_of[f]].id;
        for (const auto& component : components)
            if (component.chosen) {
                if (slot) candidate.paint_filament(component.id, *slot);
                else candidate.paint(component.id, color);
            }
        candidate.prune_colors();
        candidate.validate(surface);
        *this = std::move(candidate);
    }

    // A user-drawn region may span several old pieces. Every selected connected
    // component gets an independent ID with original-texture intent; the largest
    // selected component keeps the returned ID. Donor islands retain their paint.
    uint32_t assign_region(const std::vector<size_t>& selected, const BeautySurface& surface,
                           bool clean_new_fragments = false) {
        check_surface(surface);
        const auto indices = checked_selection(selected);
        BeautyPuzzle candidate = *this;
        const uint32_t created = candidate.allocate_id();
        std::set<uint32_t> affected {created};
        std::vector<uint8_t> changed(face_piece.size(), 0);
        for (size_t f : indices) {
            affected.insert(candidate.face_piece[f]);
            candidate.face_piece[f] = created;
            changed[f] = 1;
        }
        if (clean_new_fragments) candidate.absorb_drag_fragments(*this, changed, surface);
        candidate.split_islands(affected, surface);
        candidate.prune_colors();
        *this = std::move(candidate);
        return created;
    }

    // Refine a recognized detail in place when it substantially overlaps an
    // existing small piece. Re-cutting it would leave colored slivers behind.
    // Large surrounding face/garment regions cannot become the detail owner.
    uint32_t refine_detail_region(const std::vector<size_t>& selected, const BeautySurface& surface) {
        check_surface(surface);
        const auto indices = checked_selection(selected);
        std::map<uint32_t, double> overlap, total;
        double area = 0.;
        for (size_t f : indices) { overlap[face_piece[f]] += surface.areas[f]; area += surface.areas[f]; }
        for (size_t f = 0; f < face_piece.size(); ++f)
            if (overlap.count(face_piece[f])) total[face_piece[f]] += surface.areas[f];
        uint32_t owner = 0; double best = 0.;
        for (const auto& entry : overlap)
            if (entry.second >= total[entry.first] * .5 && total[entry.first] <= area * 2. && entry.second > best) {
                owner = entry.first; best = entry.second;
            }
        if (!owner) return assign_region(indices, surface, true);
        std::vector<uint8_t> mask(face_piece.size(), 0);
        for (size_t f : indices) mask[f] = 1;
        std::vector<size_t> removed;
        for (size_t f = 0; f < face_piece.size(); ++f)
            if (face_piece[f] == owner && !mask[f]) removed.push_back(f);
        reshape_region(owner, indices, removed, surface, {}, true);
        return owner;
    }

    void validate(const BeautySurface& surface) const {
        check_surface(surface);
        std::vector<uint8_t> visited(face_piece.size(), 0);
        std::unordered_set<uint32_t> seen;
        std::vector<size_t> queue;
        for (size_t f = 0; f < face_piece.size(); ++f) if (!visited[f]) {
            require(seen.insert(face_piece[f]).second, "A saved puzzle piece has disconnected islands.");
            visit_component(f, surface, visited, queue);
        }
    }

    nlohmann::json encode() const {
        check_state();
        using Json = nlohmann::json;
        Json runs = Json::array(), saved_colors = Json::array();
        for (size_t start = 0; start < face_piece.size();) {
            size_t end = start + 1;
            while (end < face_piece.size() && face_piece[end] == face_piece[start]) ++end;
            runs.push_back({face_piece[start], end - start});
            start = end;
        }
        for (const auto& item : colors) saved_colors.push_back({{"id", item.first}, {"rgba", item.second}});
        Json result={{"schema", !target_colors.empty()?"orca.beauty-puzzle/v4":palette.empty()?"orca.beauty-puzzle/v1":mixed_recipes.empty()?"orca.beauty-puzzle/v2":"orca.beauty-puzzle/v3"}, {"geometry_id", geometry_id},
                {"face_count", face_piece.size()}, {"next_id", next_id},
                {"piece_runs", std::move(runs)}, {"colors", std::move(saved_colors)}};
        if(!palette.empty()) {
            result["palette"]=Json::array();result["filament_slots"]=Json::array();
            for(const auto& channel:palette)result["palette"].push_back({{"slot",channel.slot},{"color",channel.display_color},{"material",channel.material_type},{"compatible",channel.compatible}});
            for(const auto& item:filament_slots)result["filament_slots"].push_back({item.first,item.second});
        }
        if(!mixed_recipes.empty() || !target_colors.empty()) {
            result["mixed_recipes"]=Json::array();
            for(const auto& recipe:mixed_recipes) {
                Json components=Json::array();for(const auto& c:recipe.components)components.push_back({c.slot,c.ratio});
                result["mixed_recipes"].push_back({{"slot",*recipe.existing_virtual_slot},{"color",recipe.target_color},
                    {"components",std::move(components)},{"settings",recipe.native_settings_fingerprint}});
            }
        }
        if(!target_colors.empty()) {
            result["target_colors"]=Json::array();
            for(const auto& item:target_colors)result["target_colors"].push_back({{"id",item.first},{"rgba",item.second}});
        }
        return result;
    }

    static BeautyPuzzle decode(const nlohmann::json& json, const std::string& geometry, size_t face_count) {
        require(face_count > 0 && face_count <= max_faces && !geometry.empty() && geometry.size() <= 256,
                "Invalid puzzle surface identity.");
        const auto schema=json.is_object()?json.value("schema",std::string{}):std::string{};
        const bool targets=schema=="orca.beauty-puzzle/v4";
        const bool mixed=targets || schema=="orca.beauty-puzzle/v3",matched=mixed || schema=="orca.beauty-puzzle/v2";
        require(json.is_object() && ((json.size()==6 && schema=="orca.beauty-puzzle/v1") || (matched && json.size()==(targets?10:mixed?9:8))),
                "Unsupported puzzle record.");
        require(json.at("geometry_id") == geometry && integer(json.at("face_count"), max_faces) == face_count,
                "Puzzle record belongs to different geometry.");
        BeautyPuzzle result;
        result.geometry_id = geometry;
        result.next_id = uint32_t(integer(json.at("next_id"), max_id));
        const auto& runs = json.at("piece_runs");
        require(runs.is_array() && !runs.empty() && runs.size() <= face_count, "Invalid puzzle partition.");
        result.face_piece.reserve(face_count);
        for (const auto& run : runs) {
            require(run.is_array() && run.size() == 2, "Invalid puzzle partition run.");
            const uint32_t id = uint32_t(integer(run[0], max_id - 1));
            const size_t length = size_t(integer(run[1], face_count));
            require(id > 0 && length > 0 && length <= face_count - result.face_piece.size(),
                    "Puzzle partition run exceeds surface.");
            result.face_piece.insert(result.face_piece.end(), length, id);
        }
        require(result.face_piece.size() == face_count, "Puzzle partition does not cover the surface.");
        const auto& items = json.at("colors");
        require(items.is_array() && items.size() <= face_count, "Too many puzzle colors.");
        for (const auto& item : items) {
            require(item.is_object() && item.size() == 2, "Invalid puzzle color record.");
            const uint32_t id = uint32_t(integer(item.at("id"), max_id - 1));
            const auto& rgba = item.at("rgba");
            require(rgba.is_array() && rgba.size() == 4, "Invalid puzzle color channels.");
            std::array<float, 4> color;
            for (size_t ch = 0; ch < 4; ++ch) {
                require(rgba[ch].is_number(), "Invalid puzzle color value.");
                const double value = rgba[ch].get<double>();
                require(std::isfinite(value) && value >= 0. && value <= 1., "Puzzle color must be between zero and one.");
                color[ch] = float(value);
            }
            require(result.colors.emplace(id, color).second, "Repeated puzzle color identity.");
        }
        if(matched) {
            const auto& channels=json.at("palette");const auto& slots=json.at("filament_slots");
            require(channels.is_array() && channels.size()<=kMaxPhysicalColorChannels && slots.is_array() && slots.size()<=face_count,"Invalid puzzle palette.");
            for(const auto& c:channels) {
                require(c.is_object() && c.size()==4 && c.at("compatible").is_boolean(),"Invalid physical filament.");
                result.palette.push_back({size_t(integer(c.at("slot"),255)),c.at("color").get<std::string>(),c.at("material").get<std::string>(),c.at("compatible").get<bool>()});
            }
            require(is_valid_physical_channel_set(result.palette),"Invalid physical filament palette.");
            for(const auto& item:slots) {
                require(item.is_array() && item.size()==2,"Invalid puzzle filament assignment.");
                require(result.filament_slots.emplace(uint32_t(integer(item[0],max_id-1)),size_t(integer(item[1],255))).second,"Repeated puzzle filament assignment.");
            }
        }
        if(mixed) {
            const auto& recipes=json.at("mixed_recipes");require(recipes.is_array() && recipes.size()<=254,"Invalid mixed palette.");
            for(const auto& r:recipes) {
                require(r.is_object() && r.size()==4,"Invalid mixed recipe.");
                MixedColorRecipe recipe;recipe.existing_virtual_slot=size_t(integer(r.at("slot"),254));
                recipe.target_color=r.at("color").get<std::string>();recipe.native_settings_fingerprint=r.at("settings").get<std::string>();
                const auto& components=r.at("components");require(components.is_array() && components.size()<=kMaxMixedColorComponents,"Invalid mixed components.");
                for(const auto& c:components) {
                    require(c.is_array() && c.size()==2 && c[1].is_number(),"Invalid mixed component.");
                    recipe.components.push_back({size_t(integer(c[0],254)),c[1].get<double>()});
                }
                result.mixed_recipes.push_back(std::move(recipe));
            }
        }
        if(targets) {
            const auto& items=json.at("target_colors");
            require(items.is_array() && !items.empty() && items.size()<=face_count,"Invalid puzzle targets.");
            for(const auto& item:items) {
                require(item.is_object() && item.size()==2,"Invalid puzzle target record.");
                const auto id=uint32_t(integer(item.at("id"),max_id-1));
                const auto& rgba=item.at("rgba");
                require(rgba.is_array() && rgba.size()==4,"Invalid puzzle target channels.");
                std::array<float,4> color;
                for(size_t ch=0;ch<4;++ch) {
                    require(rgba[ch].is_number(),"Invalid puzzle target value.");
                    const double value=rgba[ch].get<double>();
                    require(std::isfinite(value) && value>=0. && value<=1.,"Puzzle target must be between zero and one.");
                    color[ch]=float(value);
                }
                require(result.target_colors.emplace(id,color).second,"Repeated puzzle target identity.");
            }
        }
        result.check_state();
        return result;
    }

private:
    // Missed samples on folds can leave tiny donor islands inside the moved
    // boundary. Repair only newly disconnected remnants of a region that lost
    // faces in this drag; an existing eye/lip piece is never a candidate merely
    // because it is small. Large pieces still use the ordinary split behavior.
    void absorb_drag_fragments(const BeautyPuzzle& original, std::vector<uint8_t>& changed,
                               const BeautySurface& surface) {
        const size_t count = face_piece.size();
        std::map<uint32_t, double> removed_area, original_area;
        for (size_t f = 0; f < count; ++f)
            if (original.face_piece[f] != face_piece[f])
                removed_area[original.face_piece[f]] += std::max(surface.areas[f], 1e-15);
        std::map<uint32_t, std::vector<size_t>> members;
        for (size_t f = 0; f < count; ++f) {
            if (removed_area.count(original.face_piece[f])) original_area[original.face_piece[f]] += std::max(surface.areas[f], 1e-15);
            if (removed_area.count(face_piece[f])) members[face_piece[f]].push_back(f);
        }
        struct Component { std::vector<size_t> faces; double area {0.}; };
        std::vector<uint32_t> visited(count, 0);
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        uint32_t stamp = 0;
        using Entry = std::pair<double, size_t>;
        for (const auto& donor : removed_area) {
            const uint32_t owner = donor.first;
            ++stamp;
            std::vector<Component> parts;
            for (size_t seed : members[owner]) {
                if (face_piece[seed] != owner || visited[seed] == stamp) continue;
                Component part;
                part.faces.push_back(seed); visited[seed] = stamp;
                for (size_t at = 0; at < part.faces.size(); ++at) {
                    const size_t f = part.faces[at];
                    part.area += std::max(surface.areas[f], 1e-15);
                    for (int32_t n : surface.face_neighbors[f])
                        if (n >= 0 && face_piece[n] == owner && visited[n] != stamp) {
                            visited[n] = stamp; part.faces.push_back(size_t(n));
                        }
                }
                parts.push_back(std::move(part));
            }
            if (parts.size() < 2) continue;
            std::sort(parts.begin(), parts.end(), [](const Component& a, const Component& b) { return a.area > b.area; });
            const double limit = original_area[owner] * .025;
            double budget = original_area[owner] * .05;
            // Bound cleanup to the scale of the actual transferred area. A
            // distant thin appendage cut at its root must remain independent.
            const double radius = 2. * std::sqrt(donor.second);
            for (size_t index = 1; index < parts.size(); ++index) {
                const auto& part = parts[index];
                if (part.area > limit || part.area > budget) continue;
                std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
                std::map<uint32_t, double> contacts;
                for (size_t f : part.faces) distance[f] = std::numeric_limits<double>::infinity();
                for (size_t f : part.faces)
                    for (int32_t n : surface.face_neighbors[f])
                        if (n >= 0 && face_piece[n] != owner && changed[n] && original.face_piece[n] == owner) {
                            contacts[face_piece[n]] += std::sqrt(std::max(surface.areas[f], 1e-15));
                            if (distance[f] != 0.) { distance[f] = 0.; pending.emplace(0., f); }
                        }
                if (contacts.empty()) continue;
                size_t reached = 0;
                while (!pending.empty()) {
                    const auto [d, f] = pending.top(); pending.pop();
                    if (d != distance[f]) continue;
                    if (d > radius) break;
                    ++reached;
                    for (int32_t n : surface.face_neighbors[f]) if (n >= 0 && face_piece[n] == owner) {
                        const double next = d + std::max((surface.centers[f] - surface.centers[n]).norm(), 1e-12);
                        if (next < distance[n]) { distance[n] = next; pending.emplace(next, size_t(n)); }
                    }
                }
                if (reached != part.faces.size()) continue;
                const auto target = std::max_element(contacts.begin(), contacts.end(), [](const auto& a, const auto& b) {
                    return a.second < b.second;
                })->first;
                for (size_t f : part.faces) {
                    face_piece[f] = target; changed[f] = 1;
                    if (removed_area.count(target)) members[target].push_back(f);
                }
                budget -= part.area;
            }
        }
    }

    struct BoundaryField {
        std::vector<uint32_t> target;
        std::vector<float> confidence;
    };

    // Diffuse region indicators in a physical-width geodesic band. Solving
    // (area + sigma^2 * surface-Laplacian) u = area * indicator suppresses
    // multi-triangle stairs at a scale tied to surface area, rather than to a
    // fixed number of triangle rings. Only scalar bands are resident at once.
    BoundaryField boundary_field(const BeautySurface& surface, const std::vector<uint8_t>& scope,
                                 const std::vector<int32_t>& protected_labels,
                                 const std::unordered_map<uint32_t, size_t>& slot,
                                 const std::vector<double>& area, const std::vector<size_t>& members,
                                 const std::function<bool()>& canceled,
                                 const std::vector<uint32_t>* barriers,
                                 const std::vector<int32_t>* semantics,
                                 const std::vector<uint8_t>* deliberate) const {
        const size_t count = face_piece.size();
        auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
        };
        std::map<uint32_t, std::vector<size_t>> seeds;
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            for (int32_t n : surface.face_neighbors[f]) {
                if (n < 0 || size_t(n) < f || face_piece[f] == face_piece[n]) continue;
                if (!scope.empty() && !scope[f] && !scope[n]) continue;
                if (!protected_labels.empty() && protected_labels[f] >= 0 && protected_labels[n] >= 0) continue;
                if (barriers && (*barriers)[f] != (*barriers)[n]) continue;
                seeds[face_piece[f]].push_back(f); seeds[face_piece[f]].push_back(size_t(n));
                seeds[face_piece[n]].push_back(f); seeds[face_piece[n]].push_back(size_t(n));
            }
        }
        double total_area = 0.;
        for (double value : area) total_area += value;
        BoundaryField result {std::vector<uint32_t>(count, 0), std::vector<float>(count, 0.f)};
        std::vector<float> own(count, 1.f);
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        std::vector<int32_t> local(count, -1);
        using Entry = std::pair<double, size_t>;
        for (const auto& item : seeds) {
            checkpoint();
            const uint32_t owner = item.first;
            const size_t owner_slot = slot.at(owner);
            if (members[owner_slot] <= 12) continue;
            const double sigma = std::max(1.5 * std::sqrt(area[owner_slot] / double(members[owner_slot])),
                std::min(.12 * std::sqrt(area[owner_slot]), .03 * std::sqrt(total_area)));
            const double radius = 2.5 * sigma, time = sigma * sigma;
            std::vector<size_t> band;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
            auto offer = [&](size_t f, double d) {
                if (d >= radius || d >= distance[f]) return;
                if (!std::isfinite(distance[f])) band.push_back(f);
                distance[f] = d; pending.emplace(d, f);
            };
            for (size_t f : item.second) offer(f, 0.);
            size_t work = 0;
            while (!pending.empty()) {
                if ((++work & 4095) == 0) checkpoint();
                const auto [d, f] = pending.top(); pending.pop();
                if (d != distance[f]) continue;
                for (int32_t n : surface.face_neighbors[f]) {
                    if (n < 0 || (barriers && (*barriers)[f] != (*barriers)[n])) continue;
                    offer(size_t(n), d + std::max((surface.centers[f] - surface.centers[n]).norm(), 1e-12));
                }
            }
            for (size_t i = 0; i < band.size(); ++i) local[band[i]] = int32_t(i);
            const size_t size = band.size();
            std::vector<std::array<int32_t, 3>> neighbors(size);
            std::vector<std::array<double, 3>> weights(size);
            std::vector<double> diagonal(size), rhs(size), value(size), residual(size), direction(size), product(size);
            for (size_t i = 0; i < size; ++i) {
                if ((i & 4095) == 0) checkpoint();
                const size_t f = band[i];
                const double mass = std::max(surface.areas[f], 1e-15);
                diagonal[i] = mass;
                value[i] = face_piece[f] == owner ? 1. : 0.;
                rhs[i] = mass * value[i];
                neighbors[i].fill(-1); weights[i].fill(0.);
                for (size_t edge = 0; edge < 3; ++edge) {
                    const int32_t n = surface.face_neighbors[f][edge];
                    if (n < 0 || (barriers && (*barriers)[f] != (*barriers)[n])) continue;
                    const double length2 = std::max((surface.centers[f] - surface.centers[n]).squaredNorm(), 1e-24);
                    const double alignment = std::clamp(surface.normals[f].dot(surface.normals[n]), .1, 1.);
                    const bool guided=semantics && (*semantics)[f]>=0 && (*semantics)[n]>=0 && (*semantics)[f]!=(*semantics)[n] &&
                        (!deliberate || (!(*deliberate)[f] && !(*deliberate)[n]));
                    const double conductance = time * .5 * (mass + std::max(surface.areas[n], 1e-15)) / length2 * alignment * alignment * (guided?.12:1.);
                    diagonal[i] += conductance;
                    if (local[n] >= 0) { neighbors[i][edge] = local[n]; weights[i][edge] = conductance; }
                    else if (face_piece[n] == owner) rhs[i] += conductance;
                }
            }
            auto multiply = [&](const std::vector<double>& x, std::vector<double>& y) {
                for (size_t i = 0; i < size; ++i) {
                    double sum = diagonal[i] * x[i];
                    for (size_t edge = 0; edge < 3; ++edge)
                        if (neighbors[i][edge] >= 0) sum -= weights[i][edge] * x[size_t(neighbors[i][edge])];
                    y[i] = sum;
                }
            };
            multiply(value, product);
            double rz = 0.;
            for (size_t i = 0; i < size; ++i) {
                residual[i] = rhs[i] - product[i];
                direction[i] = residual[i] / diagonal[i];
                rz += residual[i] * direction[i];
            }
            const double initial_rz = rz;
            // Diagonally preconditioned CG has a fixed ceiling, while the
            // requested physical smoothing width does not depend on that cap.
            for (unsigned iteration = 0; iteration < 80 && rz > initial_rz * 1e-10 && rz > 1e-30; ++iteration) {
                checkpoint();
                multiply(direction, product);
                double denominator = 0.;
                for (size_t i = 0; i < size; ++i) denominator += direction[i] * product[i];
                if (!(denominator > 0.) || !std::isfinite(denominator)) break;
                const double alpha = rz / denominator;
                double next_rz = 0.;
                for (size_t i = 0; i < size; ++i) {
                    value[i] += alpha * direction[i];
                    residual[i] -= alpha * product[i];
                    next_rz += residual[i] * residual[i] / diagonal[i];
                }
                const double beta = next_rz / rz;
                for (size_t i = 0; i < size; ++i) direction[i] = residual[i] / diagonal[i] + beta * direction[i];
                rz = next_rz;
            }
            for (size_t i = 0; i < size; ++i) {
                const size_t f = band[i];
                const float score = std::isfinite(value[i]) ? float(std::clamp(value[i], 0., 1.)) : (face_piece[f] == owner ? 1.f : 0.f);
                if (face_piece[f] == owner) own[f] = score;
                else if (score > result.confidence[f]) { result.confidence[f] = score; result.target[f] = owner; }
                distance[f] = std::numeric_limits<double>::infinity(); local[f] = -1;
            }
        }
        for (size_t f = 0; f < count; ++f) {
            result.confidence[f] -= own[f];
            if (result.confidence[f] <= .025f) result.target[f] = 0;
        }
        return result;
    }

    void smooth_partition(const BeautySurface& surface, const std::vector<uint8_t>& scope,
                          const std::vector<int32_t>& protected_labels,
                          const std::function<bool()>& canceled,
                          const std::vector<uint32_t>* barriers = nullptr,
                          const std::vector<int32_t>* semantics = nullptr,
                          const std::vector<uint8_t>* deliberate = nullptr) {
        const size_t count = face_piece.size();
        auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
        };
        // Saved IDs can be sparse and very large; storage scales with pieces,
        // never next_id. Area budgets protect thin lips and small eye pieces.
        std::unordered_map<uint32_t, size_t> slot;
        std::vector<double> area, minimum, maximum;
        std::vector<size_t> members;
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            const auto entry = slot.emplace(face_piece[f], slot.size());
            if (entry.second) { area.push_back(0.); members.push_back(0); }
            area[entry.first->second] += std::max(surface.areas[f], 1e-15);
            ++members[entry.first->second];
        }
        minimum.resize(area.size()); maximum.resize(area.size());
        for (size_t i = 0; i < area.size(); ++i) {
            minimum[i] = area[i] * (members[i] <= 12 ? 1. : members[i] <= 80 ? .9 : .82);
            maximum[i] = area[i] * (members[i] <= 12 ? 1. : 1.18);
        }
        std::vector<std::array<double, 3>> lab;
        for (const auto& patch : surface.patches) lab.push_back(to_lab(patch.mean_color));
        auto weight = [&](size_t a, size_t b) {
            // Face area provides a tessellation-aware perimeter proxy without
            // requiring a second mesh or modifying the surface contract.
            return std::sqrt(std::max(surface.areas[a], 1e-15)) + std::sqrt(std::max(surface.areas[b], 1e-15));
        };
        double perimeter = 0.;
        for (size_t f = 0; f < count; ++f)
            for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && size_t(n) > f && face_piece[f] != face_piece[n]) perimeter += weight(f, size_t(n));

        std::vector<uint32_t> visited(count, 0);
        uint32_t stamp = 0;
        auto new_visit = [&] {
            if (++stamp == 0) { std::fill(visited.begin(), visited.end(), 0); ++stamp; }
        };
        std::vector<size_t> queue;
        queue.reserve(128);
        auto donor_stays_connected = [&](size_t removed, uint32_t owner) {
            std::array<size_t, 3> same {};
            size_t same_count = 0;
            for (int32_t n : surface.face_neighbors[removed])
                if (n >= 0 && face_piece[n] == owner) same[same_count++] = size_t(n);
            if (same_count == 0) return false;
            if (same_count == 1) return true;
            new_visit();
            queue.assign(1, same[0]); visited[same[0]] = stamp;
            size_t reached = 1;
            for (size_t at = 0; at < queue.size() && queue.size() < 128; ++at) {
                for (int32_t n : surface.face_neighbors[queue[at]])
                    if (n >= 0 && size_t(n) != removed && visited[n] != stamp && face_piece[n] == owner) {
                        visited[n] = stamp; queue.push_back(size_t(n));
                        for (size_t i = 1; i < same_count; ++i) if (same[i] == size_t(n)) ++reached;
                        if (reached == same_count) return true;
                    }
            }
            // Not proving the local connection means retaining the face. This
            // conservative rule never cuts a neck or splits a saved region.
            return false;
        };
        const auto field = boundary_field(surface, scope, protected_labels, slot, area, members, canceled, barriers, semantics, deliberate);
        auto crosses_recognized_contour=[&](size_t f,size_t n) {
            return semantics && (*semantics)[f]>=0 && (*semantics)[n]>=0 && (*semantics)[f]!=(*semantics)[n] &&
                (!deliberate || (!(*deliberate)[f] && !(*deliberate)[n]));
        };
        std::vector<uint32_t> fragment_of(count, 0);
        std::vector<uint8_t> queued(count, 0);
        uint32_t fragment_id = 0;
        auto eligible = [&](size_t f) {
            return field.target[f] && field.target[f] != face_piece[f] &&
                (scope.empty() || scope[f]) && (protected_labels.empty() || protected_labels[f] < 0);
        };
        for (size_t seed = 0; seed < count; ++seed) {
            if ((seed & 4095) == 0) checkpoint();
            if (fragment_of[seed] || !eligible(seed)) continue;
            ++fragment_id;
            std::vector<size_t> fragment {seed};
            fragment_of[seed] = fragment_id;
            for (size_t at = 0; at < fragment.size(); ++at)
                for (int32_t n : surface.face_neighbors[fragment[at]])
                    if (n >= 0 && !fragment_of[n] && eligible(size_t(n))) {
                        fragment_of[n] = fragment_id; fragment.push_back(size_t(n));
                    }
            std::vector<uint32_t> before;
            before.reserve(fragment.size());
            for (size_t f : fragment) before.push_back(face_piece[f]);
            const auto old_area = area;
            const double old_perimeter = perimeter;
            std::priority_queue<std::pair<float, size_t>> pending;
            auto enqueue = [&](size_t f) {
                if (fragment_of[f] == fragment_id && !queued[f] && field.target[f] != face_piece[f]) {
                    queued[f] = 1; pending.emplace(field.confidence[f], f);
                }
            };
            for (size_t f : fragment) enqueue(f);
            size_t work = 0;
            while (!pending.empty()) {
                if ((++work & 4095) == 0) checkpoint();
                const size_t f = pending.top().second; pending.pop(); queued[f] = 0;
                const uint32_t owner = face_piece[f], target = field.target[f];
                if (owner == target) continue;
                const size_t from = slot.at(owner), to = slot.at(target);
                const double mass = std::max(surface.areas[f], 1e-15);
                if (area[from] - mass < minimum[from] || area[to] + mass > maximum[to]) continue;
                bool touches_target = false;
                for (int32_t n : surface.face_neighbors[f]) {
                    if (n < 0 || face_piece[n] != target || (!scope.empty() && !scope[n])) continue;
                    if (barriers && (*barriers)[f] != (*barriers)[n]) continue;
                    double difference = 0.;
                    for (size_t ch = 0; ch < 3; ++ch)
                        difference += std::pow(lab[surface.face_patch[f]][ch] - lab[surface.face_patch[n]][ch], 2);
                    if (difference <= 24. * 24. && (!crosses_recognized_contour(f,size_t(n)) || field.confidence[f]>.3f)) { touches_target = true; break; }
                }
                if (!touches_target || !donor_stays_connected(f, owner)) continue;
                for (int32_t n : surface.face_neighbors[f]) if (n >= 0) {
                    const double cost = weight(f, size_t(n));
                    if (face_piece[n] != owner) perimeter -= cost;
                    if (face_piece[n] != target) perimeter += cost;
                }
                face_piece[f] = target; area[from] -= mass; area[to] += mass;
                for (int32_t n : surface.face_neighbors[f]) if (n >= 0) enqueue(size_t(n));
            }
            // Judge each independent boundary fragment against its own start.
            // A bad proposal on a fold must not roll back a useful smoothing on
            // the cheek or garment elsewhere. Each transaction is topology-safe
            // in the current partition; reversing it cannot undo another one.
            if (perimeter > old_perimeter + std::max(1., old_perimeter) * 1e-10) {
                for (size_t i = 0; i < fragment.size(); ++i) face_piece[fragment[i]] = before[i];
                area = old_area; perimeter = old_perimeter;
            }
        }
        for (unsigned pass = 0; pass < 8; ++pass) {
            checkpoint();
            const auto before = face_piece;
            const auto old_area = area;
            const double old_perimeter = perimeter;
            size_t changed = 0;
            for (size_t at = 0; at < count; ++at) {
                if ((at & 4095) == 0) checkpoint();
                const size_t f = pass % 2 ? count - at - 1 : at;
                if ((!scope.empty() && !scope[f]) || (!protected_labels.empty() && protected_labels[f] >= 0)) continue;
                const uint32_t owner = face_piece[f];
                const size_t owner_slot = slot.at(owner);
                const double face_area = std::max(surface.areas[f], 1e-15);
                if (area[owner_slot] - face_area < minimum[owner_slot]) continue;
                std::array<uint32_t, 3> targets {};
                std::array<unsigned, 3> contacts {};
                size_t target_count = 0;
                for (int32_t n : surface.face_neighbors[f]) {
                    if (n < 0 || face_piece[n] == owner || (!scope.empty() && !scope[n])) continue;
                    if (barriers && (*barriers)[f] != (*barriers)[n]) continue;
                    const uint32_t other = face_piece[n];
                    if (area[slot.at(other)] + face_area > maximum[slot.at(other)]) continue;
                    double difference = 0.;
                    for (size_t ch = 0; ch < 3; ++ch)
                        difference += std::pow(lab[surface.face_patch[f]][ch] - lab[surface.face_patch[n]][ch], 2);
                    if (difference > 24. * 24.) continue;
                    if(crosses_recognized_contour(f,size_t(n)))continue;
                    size_t index = 0;
                    while (index < target_count && targets[index] != other) ++index;
                    if (index == target_count) targets[target_count++] = other;
                    ++contacts[index];
                }
                if (!target_count) continue;
                uint32_t target = 0;
                for (size_t i = 0; i < target_count; ++i)
                    if (contacts[i] >= 2) { target = targets[i]; break; }
                if (!target || !donor_stays_connected(f, owner)) continue;
                for (int32_t n : surface.face_neighbors[f]) if (n >= 0) {
                    const double cost = weight(f, size_t(n));
                    if (face_piece[n] != owner) perimeter -= cost;
                    if (face_piece[n] != target) perimeter += cost;
                }
                face_piece[f] = target;
                area[owner_slot] -= face_area; area[slot.at(target)] += face_area;
                ++changed;
            }
            if (perimeter > old_perimeter + std::max(1., old_perimeter) * 1e-10) {
                face_piece = before; area = old_area; perimeter = old_perimeter;
                break;
            } else if (!changed) break;
        }
        checkpoint();
    }

    void regularize_boundaries(const BeautySurface& surface, const std::function<bool()>& canceled) {
        auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
        };
        const size_t count = face_piece.size();
        const auto original = face_piece;
        double total_area = 0.;
        std::vector<double> piece_area(next_id, 0.);
        std::vector<std::array<double, 3>> piece_lab(next_id), patch_lab;
        for (const auto& patch : surface.patches) patch_lab.push_back(to_lab(patch.mean_color));
        for (size_t f = 0; f < count; ++f) {
            const double area = std::max(surface.areas[f], 1e-15);
            total_area += area; piece_area[original[f]] += area;
            for (size_t ch = 0; ch < 3; ++ch)
                piece_lab[original[f]][ch] += patch_lab[surface.face_patch[f]][ch] * area;
        }
        for (uint32_t id = 1; id < next_id; ++id)
            for (double& ch : piece_lab[id]) ch /= std::max(piece_area[id], 1e-15);
        const double radius = .12 * std::sqrt(total_area / double(piece_count()));
        using Entry = std::pair<double, size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        auto length = [&](size_t a, size_t b) {
            return std::max((surface.centers[a] - surface.centers[b]).norm(), 1e-12);
        };
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0 && original[f] != original[n])
                distance[f] = std::min(distance[f], .5 * length(f, size_t(n)));
            if (distance[f] < radius) pending.emplace(distance[f], f);
        }
        size_t attempts = 0;
        while (!pending.empty()) {
            if ((attempts++ & 4095) == 0) checkpoint();
            const auto [d, f] = pending.top(); pending.pop();
            if (d != distance[f]) continue;
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0 && original[f] == original[n]) {
                const double next = d + length(f, size_t(n));
                if (next < radius && next < distance[n]) { distance[n] = next; pending.emplace(next, size_t(n)); }
            }
        }
        // Keep one connected interior per region. Disconnected remnants after
        // erosion are part of the boundary band, not extra mandatory seeds.
        std::vector<uint32_t> component(count, 0), best_component(next_id, 0);
        std::vector<double> best_area(next_id, 0.);
        std::vector<size_t> deepest(next_id, count), queue;
        uint32_t next_component = 1;
        for (size_t f = 0; f < count; ++f) {
            if ((f & 4095) == 0) checkpoint();
            const uint32_t id = original[f];
            if (deepest[id] == count || distance[f] > distance[deepest[id]]) deepest[id] = f;
            if (component[f] != 0 || distance[f] < radius) continue;
            const uint32_t current = next_component++;
            component[f] = current; queue.assign(1, f);
            double area = 0.;
            for (size_t at = 0; at < queue.size(); ++at) {
                if ((at & 4095) == 0) checkpoint();
                area += std::max(surface.areas[queue[at]], 1e-15);
                for (int32_t n : surface.face_neighbors[queue[at]])
                    if (n >= 0 && component[n] == 0 && original[n] == id && distance[n] >= radius) {
                        component[n] = current; queue.push_back(size_t(n));
                    }
            }
            if (area > best_area[id]) { best_area[id] = area; best_component[id] = current; }
        }
        std::vector<uint8_t> settled(count, 0);
        for (size_t f = 0; f < count; ++f)
            if ((component[f] != 0 && component[f] == best_component[original[f]]) ||
                (best_component[original[f]] == 0 && f == deepest[original[f]])) settled[f] = 1;
        std::fill(distance.begin(), distance.end(), std::numeric_limits<double>::infinity());
        auto offer = [&](size_t from, size_t to, double d) {
            if (settled[to]) return;
            const uint32_t owner = face_piece[from];
            double color_distance = 0.;
            for (size_t ch = 0; ch < 3; ++ch)
                color_distance += std::pow(patch_lab[surface.face_patch[to]][ch] - piece_lab[owner][ch], 2);
            const double bend = 1. - std::clamp(surface.normals[from].dot(surface.normals[to]), -1., 1.);
            const double next = d + length(from, to) * (1. + .10 * std::sqrt(color_distance) + .8 * bend);
            if (next < distance[to]) {
                distance[to] = next; face_piece[to] = owner;
                pending.emplace(next, to);
            }
        };
        for (size_t f = 0; f < count; ++f) if (settled[f])
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0) offer(f, size_t(n), 0.);
        while (!pending.empty()) {
            if ((attempts++ & 4095) == 0) checkpoint();
            const auto [d, f] = pending.top(); pending.pop();
            if (settled[f] || d != distance[f]) continue;
            settled[f] = 1;
            for (int32_t n : surface.face_neighbors[f]) if (n >= 0) offer(f, size_t(n), d);
        }
        trim_boundary_teeth(surface, {}, canceled);
        prune_colors();
        checkpoint();
    }

    void trim_boundary_teeth(const BeautySurface& surface, const std::vector<int32_t>& protected_labels,
                             const std::function<bool()>& canceled, const std::vector<uint32_t>* barriers = nullptr) {
        std::vector<std::array<double, 3>> patch_lab;
        for (const auto& patch : surface.patches) patch_lab.push_back(to_lab(patch.mean_color));
        for (unsigned pass = 0; pass < 6; ++pass) {
            size_t changed = 0;
            for (size_t at = 0; at < face_piece.size(); ++at) {
                if ((at & 4095) == 0 && canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
                const size_t f = pass % 2 ? face_piece.size() - at - 1 : at;
                if (!protected_labels.empty() && protected_labels[f] >= 0) continue;
                const auto& neighbors = surface.face_neighbors[f];
                for (size_t a = 0; a < neighbors.size(); ++a) {
                    const int32_t n = neighbors[a];
                    if (n < 0 || face_piece[n] == face_piece[f]) continue;
                    if (barriers && (*barriers)[f] != (*barriers)[n]) continue;
                    bool majority = false;
                    for (size_t b = a + 1; b < neighbors.size(); ++b)
                        if (neighbors[b] >= 0 && neighbors[b] != n && face_piece[neighbors[b]] == face_piece[n]) majority = true;
                    if (!majority) continue;
                    double color_distance = 0.;
                    for (size_t ch = 0; ch < 3; ++ch)
                        color_distance += std::pow(patch_lab[surface.face_patch[f]][ch] - patch_lab[surface.face_patch[n]][ch], 2);
                    if (color_distance > 12. * 12.) continue;
                    face_piece[f] = face_piece[n]; ++changed; break;
                }
            }
            if (!changed) break;
        }
    }

    void absorb_small_unrecognized(const BeautySurface& surface, const std::vector<int32_t>& semantic_labels,
                                   const std::vector<uint32_t>& coarse, double minimum_area,
                                   const std::function<bool()>& canceled) {
        std::vector<double> area(next_id, 0.);
        std::vector<uint8_t> recognized(next_id, 0);
        std::vector<uint32_t> coarse_min(next_id, std::numeric_limits<uint32_t>::max()), coarse_max(next_id, 0);
        std::vector<std::vector<size_t>> members(next_id);
        for (size_t f = 0; f < face_piece.size(); ++f) {
            const uint32_t id = face_piece[f];
            area[id] += std::max(surface.areas[f], 1e-15);
            coarse_min[id] = std::min(coarse_min[id], coarse[f]);
            coarse_max[id] = std::max(coarse_max[id], coarse[f]);
            members[id].push_back(f);
            if (semantic_labels[f] >= 0) recognized[id] = 1;
        }
        std::vector<uint32_t> order;
        for (uint32_t id = 1; id < next_id; ++id) if (!recognized[id] && area[id] < minimum_area) order.push_back(id);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return area[a] != area[b] ? area[a] < area[b] : a < b;
        });
        for (const uint32_t id : order) {
            if (canceled && canceled()) throw std::runtime_error("Puzzle preparation cancelled.");
            if (members[id].empty() || area[id] >= minimum_area) continue;
            std::map<uint32_t, double> shared_boundary;
            for (size_t f : members[id]) for (int32_t n : surface.face_neighbors[f])
                if (n >= 0 && face_piece[n] != id && (!recognized[face_piece[n]] ||
                    (coarse_min[id] == coarse_max[id] && coarse[f] == coarse[n])))
                    shared_boundary[face_piece[n]] += std::sqrt(std::max(surface.areas[f], 1e-15));
            uint32_t target = 0;
            double best = -1.;
            for (const auto& neighbor : shared_boundary) {
                const double preference = neighbor.second * (recognized[neighbor.first] ? 1. : 2.);
                if (preference > best) { best = preference; target = neighbor.first; }
            }
            if (target == 0) continue; // A disconnected shell remains independent.
            for (size_t f : members[id]) { face_piece[f] = target; members[target].push_back(f); }
            area[target] += area[id]; area[id] = 0.; members[id].clear();
            coarse_min[target] = std::min(coarse_min[target], coarse_min[id]);
            coarse_max[target] = std::max(coarse_max[target], coarse_max[id]);
        }
    }

    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    static uint64_t integer(const nlohmann::json& value, uint64_t limit) {
        require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0),
                "Invalid puzzle integer.");
        const uint64_t result = value.get<uint64_t>();
        require(result <= limit, "Puzzle integer exceeds limit.");
        return result;
    }
    static void check_color(const std::array<float, 4>& color) {
        for (float channel : color)
            require(std::isfinite(channel) && channel >= 0 && channel <= 1, "Puzzle color must be between zero and one.");
    }
    static std::array<double, 3> to_lab(const std::array<double, 3>& rgb) {
        std::array<double, 3> linear;
        for (size_t ch = 0; ch < 3; ++ch) linear[ch] = rgb[ch] <= .04045 ? rgb[ch] / 12.92 : std::pow((rgb[ch] + .055) / 1.055, 2.4);
        auto transform = [](double t) { return t > .008856451679 ? std::cbrt(t) : t * 7.787037037 + 16. / 116.; };
        const double x = transform((.4124564 * linear[0] + .3575761 * linear[1] + .1804375 * linear[2]) / .95047);
        const double y = transform(.2126729 * linear[0] + .7151522 * linear[1] + .0721750 * linear[2]);
        const double z = transform((.0193339 * linear[0] + .1191920 * linear[1] + .9503041 * linear[2]) / 1.08883);
        return {116. * y - 16., 500. * (x - y), 200. * (y - z)};
    }
    static void check_surface_data(const BeautySurface& surface) {
        const size_t count = surface.face_patch.size();
        require(count > 0 && count <= max_faces && !surface.geometry_id.empty() && surface.geometry_id.size() <= 256,
                "Invalid puzzle surface.");
        require(surface.face_neighbors.size() == count && surface.centers.size() == count &&
                surface.normals.size() == count && surface.areas.size() == count && surface.patches.size() <= count,
                "Incomplete puzzle surface data.");
        for (const auto& patch : surface.patches)
            for (double color : patch.mean_color)
                require(std::isfinite(color) && color >= 0 && color <= 1, "Invalid puzzle source color.");
        for (size_t f = 0; f < count; ++f) {
            require(surface.face_patch[f] < surface.patches.size() && surface.centers[f].allFinite() &&
                    surface.normals[f].allFinite() && std::isfinite(surface.areas[f]) && surface.areas[f] >= 0,
                    "Invalid puzzle face data.");
            for (int32_t n : surface.face_neighbors[f]) {
                require(n == -1 || (n >= 0 && size_t(n) < count && size_t(n) != f), "Invalid puzzle adjacency.");
                if (n >= 0)
                    require(std::find(surface.face_neighbors[n].begin(), surface.face_neighbors[n].end(), int32_t(f)) !=
                            surface.face_neighbors[n].end(), "Puzzle adjacency must be symmetric.");
            }
        }
    }
    void check_state() const {
        require(!geometry_id.empty() && geometry_id.size() <= 256 && !face_piece.empty() && face_piece.size() <= max_faces,
                "Invalid puzzle identity or capacity.");
        require(next_id > 1 && next_id <= max_id, "Invalid next puzzle identity.");
        std::unordered_set<uint32_t> ids;
        for (uint32_t id : face_piece) {
            require(id > 0 && id < next_id, "Puzzle partition contains an invalid identity.");
            ids.insert(id);
        }
        require(colors.size() <= ids.size(), "Too many puzzle color overrides.");
        for (const auto& item : colors) {
            require(ids.count(item.first) != 0, "Puzzle color refers to a missing piece.");
            check_color(item.second);
        }
        for(const auto& item:target_colors) {
            require(colors.count(item.first) && filament_slots.count(item.first),"Puzzle target refers to an unmatched piece.");
            check_color(item.second);
        }
        require(palette.empty()?filament_slots.empty():is_valid_physical_channel_set(palette),"Invalid puzzle palette state.");
        require(valid_native_mixed_palette(palette,mixed_recipes),"Invalid native mixed palette.");
        for(const auto& item:filament_slots) {
            const auto channel=std::find_if(palette.begin(),palette.end(),[&](const auto& c){return c.slot==item.second && c.compatible;});
            require(ids.count(item.first) && colors.count(item.first) && native_palette_has_slot(palette,mixed_recipes,item.second),"Puzzle filament refers to a missing region or material.");
            auto expected=channel!=palette.end()?filament_color(*channel):std::array<float,4>{};
            if(channel==palette.end())for(const auto& r:mixed_recipes)if(r.existing_virtual_slot==item.second)expected=filament_color({item.second,r.target_color,{},true});
            require(colors.at(item.first)==expected,"Puzzle color differs from its assigned filament.");
        }
    }
    void check_surface(const BeautySurface& surface) const {
        check_state();
        require(geometry_id == surface.geometry_id && face_piece.size() == surface.face_patch.size(),
                "Puzzle belongs to different geometry.");
        check_surface_data(surface);
    }
    void require_piece(uint32_t id) const {
        require(id > 0 && std::find(face_piece.begin(), face_piece.end(), id) != face_piece.end(),
                "The selected puzzle piece no longer exists.");
    }
    std::vector<size_t> checked_selection(std::vector<size_t> selected) const {
        require(!selected.empty() && selected.size() <= max_faces, "Select a surface area first.");
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        require(selected.back() < face_piece.size(), "Selected area belongs to different geometry.");
        return selected;
    }
    uint32_t allocate_id() {
        require(next_id < max_id, "Puzzle identity limit reached.");
        return next_id++;
    }
    void visit_component(size_t seed, const BeautySurface& surface, std::vector<uint8_t>& visited,
                         std::vector<size_t>& queue) const {
        queue.assign(1, seed);
        visited[seed] = 1;
        for (size_t at = 0; at < queue.size(); ++at)
            for (int32_t n : surface.face_neighbors[queue[at]])
                if (n >= 0 && !visited[n] && face_piece[n] == face_piece[seed]) {
                    visited[n] = 1;
                    queue.push_back(size_t(n));
                }
    }
    bool connected(uint32_t id, const BeautySurface& surface) const {
        const auto seed = std::find(face_piece.begin(), face_piece.end(), id);
        if (seed == face_piece.end()) return false;
        std::vector<uint8_t> visited(face_piece.size(), 0);
        std::vector<size_t> queue;
        visit_component(size_t(seed - face_piece.begin()), surface, visited, queue);
        return queue.size() == size_t(std::count(face_piece.begin(), face_piece.end(), id));
    }
    void split_islands(const std::set<uint32_t>& affected, const BeautySurface& surface) {
        struct Component { std::vector<size_t> faces; double area {0}; };
        std::map<uint32_t, std::vector<Component>> components;
        std::vector<uint8_t> visited(face_piece.size(), 0);
        std::vector<size_t> queue;
        for (size_t f = 0; f < face_piece.size(); ++f) {
            if (visited[f] || affected.count(face_piece[f]) == 0) continue;
            visit_component(f, surface, visited, queue);
            Component component;
            for (size_t index : queue) component.area += std::max(surface.areas[index], 1e-15);
            component.faces = queue;
            components[face_piece[f]].push_back(std::move(component));
        }
        for (auto& item : components) {
            auto& parts = item.second;
            if (parts.size() < 2) continue;
            const auto largest = std::max_element(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.area < b.area; });
            const auto original_color = colors.find(item.first);
            for (auto part = parts.begin(); part != parts.end(); ++part) if (part != largest) {
                const uint32_t created = allocate_id();
                if (original_color != colors.end()) colors[created] = original_color->second;
                if(filament_slots.count(item.first))filament_slots[created]=filament_slots.at(item.first);
                if(target_colors.count(item.first))target_colors[created]=target_colors.at(item.first);
                for (size_t f : part->faces) face_piece[f] = created;
            }
        }
    }
    void prune_colors() {
        const std::unordered_set<uint32_t> ids(face_piece.begin(), face_piece.end());
        for (auto color = colors.begin(); color != colors.end();) {
            if (ids.count(color->first) == 0) color = colors.erase(color);
            else ++color;
        }
        for(auto slot=filament_slots.begin();slot!=filament_slots.end();) {
            if(!ids.count(slot->first))slot=filament_slots.erase(slot);else ++slot;
        }
        for(auto target=target_colors.begin();target!=target_colors.end();) {
            if(!ids.count(target->first))target=target_colors.erase(target);else ++target;
        }
    }
};

} // namespace Slic3r::AI
