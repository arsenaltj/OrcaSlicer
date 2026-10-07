#pragma once

#include "PortraitColorPlan.hpp"
#include "PortraitShapeDetails.hpp"

namespace Slic3r::GUI {

inline PortraitColorPlan build_parent_repair_plan(const PortraitShapeDetails& details,
    const indexed_triangle_set& mesh, const AI::BeautySurface& surface,
    const std::vector<PortraitColorPlan::Slot>& palette, const AI::SemanticColoring::FaceColors& previous,
    const AI::SemanticColoring::SubfaceColors& children,
    const std::map<size_t,std::map<std::string,size_t>>& visibility) {
    using Key = AI::BeautyLeafKey;
    const auto& ownership = *details.surface_ownership; ownership.validate(details.locks);
    PortraitColorPlan plan;
    plan.geometry_id = details.locks.geometry_id; plan.source_sha256 = details.locks.source_sha256;
    plan.boundary_sha256 = AI::beauty_leaf_digest(details.locks.encode().dump());
    plan.face_count = details.locks.face_count; plan.palette = palette; plan.repair_domain = ownership.editing_domain;
    const auto hash = ownership.fingerprint();
    plan.ownership_reference = {{"schema","orca.portrait-surface-ownership-reference/v1"},
        {"sha256",hash},{"path","portrait-ownership/"+hash+".json"}};
    std::map<size_t,PortraitColorPlan::RGB> roots;
    for (const auto& entry : previous) if (entry.first >= plan.face_count || !roots.emplace(entry).second)
        throw std::invalid_argument("Duplicate or out-of-range preserved root color.");
    std::map<size_t,std::map<Key,PortraitColorPlan::RGB>> overrides;
    for (const auto& child : children) {
        const Key key{child.face_id,child.path.depth,child.path.value};
        if (!key.valid(plan.face_count) || !key.depth || !overrides[child.face_id].emplace(key,child.color).second)
            throw std::invalid_argument("Invalid preserved child color.");
    }
    const auto keys = ownership.editing_domain.all_leaves();
    const auto links = ownership.editing_domain.adjacency(mesh,surface);
    std::map<Key,const PortraitSurfaceOwnership::Region*> targets;
    for (const auto& region : ownership.regions) for (const auto& key : region.leaves) targets.emplace(key,&region);
    std::vector<int32_t> component_ids(keys.size(),-1);
    for (size_t first = 0; first < keys.size(); ++first) {
        const auto found = targets.find(keys[first]);
        if (found == targets.end() || component_ids[first] >= 0) continue;
        const auto& seed = *found->second;
        PortraitColorPlan::Component component;
        component.id = "parent-component-"+std::to_string(plan.components.size());
        component.label = seed.parent; component.region = seed.subject+":"+seed.parent;
        component.risks = seed.risks; component.color_sources = nlohmann::json::array();
        std::vector<size_t> queue{first}; component_ids[first] = int32_t(plan.components.size());
        std::array<double,3> total{}; std::map<size_t,double> votes; std::set<size_t> source_roots;
        for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
            const auto i = queue[cursor]; const auto& key = keys[i]; const auto f = key.source_face_id;
            const double area = std::max(surface.areas[f]*std::ldexp(1.,-2*key.depth),1e-15);
            component.leaves.push_back(key); component.area += area; source_roots.insert(f);
            for (size_t c = 0; c < 3; ++c) total[c] += area*details.base_colors[f][c];
            std::optional<PortraitColorPlan::RGB> old;
            std::string origin = "SOURCE_TEXTURE_OR_VERIFIED_BASE";
            const auto root = roots.find(f); if (root != roots.end()) { old = root->second; origin = "OLD_ROOT_SLOT"; }
            uint8_t depth = 0;
            const auto group = overrides.find(f);
            if (group != overrides.end()) for (const auto& entry : group->second)
                if (entry.first.contains(key) && entry.first.depth >= depth) {
                    old = entry.second; depth = entry.first.depth; origin = "OLD_SUBFACE_COLOR";
                }
            auto before = nlohmann::json{{"leaf",{f,key.depth,key.path}},{"source",origin},
                {"original_rgb",nullptr},{"original_uid",nullptr}};
            if (old) {
                before["original_rgb"] = *old;
                for (size_t s = 0; s < palette.size(); ++s) if (palette[s].rgb == *old) {
                    before["original_uid"] = palette[s].uid; votes[s] += area; break;
                }
            }
            component.color_sources.push_back(std::move(before));
            for (const auto n : links[i]) if (component_ids[n] < 0) {
                const auto other = targets.find(keys[n]);
                if (other != targets.end() && other->second->subject == seed.subject && other->second->parent == seed.parent) {
                    component_ids[n] = component_ids[first]; queue.push_back(n);
                }
            }
        }
        for (size_t c = 0; c < 3; ++c) component.source[c] = float(total[c]/component.area);
        component.original_slot = votes.empty() ? plan.slot(seed.parent == "face" ? "portrait-skin" : "portrait-light") :
            std::max_element(votes.begin(),votes.end(),[](const auto& a,const auto& b){return a.second < b.second;})->first;
        std::sort(component.leaves.begin(),component.leaves.end());
        for (const auto f : source_roots) {
            const auto views = visibility.find(f);
            if (views != visibility.end()) for (const auto& view : views->second) component.visible_pixels[view.first] += view.second;
        }
        plan.components.push_back(std::move(component));
    }
    std::vector<std::set<size_t>> adjacent(plan.components.size());
    for (size_t i = 0; i < keys.size(); ++i) if (component_ids[i] >= 0) for (const auto n : links[i])
        if (component_ids[n] >= 0 && component_ids[i] != component_ids[n]) {
            adjacent[component_ids[i]].insert(component_ids[n]); adjacent[component_ids[n]].insert(component_ids[i]);
        }
    for (size_t i = 0; i < plan.components.size(); ++i) plan.components[i].neighbors.assign(adjacent[i].begin(),adjacent[i].end());
    plan.optimize(); return plan;
}

inline PortraitColorPlan build_portrait_color_plan(const PortraitShapeDetails& details,
    const indexed_triangle_set& source_mesh, const AI::BeautySurface& surface, const AI::SemanticColoring::Analysis& analysis,
    const std::vector<PortraitColorPlan::Slot>& palette, const AI::SemanticColoring::FaceColors& previous,
    const AI::SemanticColoring::SubfaceColors& previous_children = {},
    const std::map<size_t, std::map<std::string,size_t>>& visibility = {}) {
    using Label = AI::SemanticColoring::Label;
    using Key = AI::BeautyLeafKey;
    const auto count = details.locks.face_count;
    if (!details.compatible(surface.geometry_id,count) || analysis.geometry_id != surface.geometry_id ||
        surface.areas.size() != count || analysis.face_labels.size() != count || analysis.face_confidence.size() != count)
        throw std::invalid_argument("Color plan source mapping changed.");
    if (details.surface_ownership)
        return build_parent_repair_plan(details,source_mesh,surface,palette,previous,previous_children,visibility);
    PortraitColorPlan plan;
    plan.geometry_id = details.locks.geometry_id; plan.source_sha256 = details.locks.source_sha256;
    plan.boundary_sha256 = AI::beauty_leaf_digest(details.locks.encode().dump());
    plan.face_count = count; plan.palette = palette;
    if (palette.size() < 3 || palette.size() > 6) throw std::invalid_argument("Unsupported portrait palette budget.");
    std::vector<size_t> original(count,palette.size());
    for (const auto& item : previous) {
        if (item.first >= count || original[item.first] != palette.size()) throw std::invalid_argument("Invalid previous face color.");
        for (size_t slot = 0; slot < palette.size(); ++slot) if (palette[slot].rgb == item.second) {
            original[item.first] = slot; break;
        }
        if (original[item.first] == palette.size()) throw std::invalid_argument("Previous assignment leaves the fixed palette.");
    }
    const auto nearest = [&](size_t face) {
        const auto& color = details.base_colors[face];
        const auto source = PortraitColorPlan::oklab({color[0],color[1],color[2]});
        size_t best = 0; double score = std::numeric_limits<double>::max();
        for (size_t i = 0; i < palette.size(); ++i) {
            const auto proposed = PortraitColorPlan::distance(source,PortraitColorPlan::oklab(palette[i].rgb));
            if (proposed < score) { best = i; score = proposed; }
        }
        return best;
    };
    const auto domain = details.locks.leaf_domain ? *details.locks.leaf_domain : AI::BeautyLeafDomain{surface.geometry_id,count,{}};
    const auto keys = domain.all_leaves();
    std::map<Key,std::pair<std::string,std::string>> features;
    std::vector<std::string> parents(count);
    std::set<std::string> subject_ids(details.subjects.begin(),details.subjects.end());
    if (subject_ids.size() != details.subjects.size()) throw std::invalid_argument("Duplicate portrait subject context.");
    for (const auto& subject : subject_ids)
        if (!LocalSemanticEvidence::detail::identifier(subject)) throw std::invalid_argument("Invalid portrait subject context.");
    std::map<std::string,std::vector<std::string>> risks;
    for (const auto& lock : details.locks.locks) {
        if (!subject_ids.empty() && !subject_ids.count(lock.subject_id))
            throw std::invalid_argument("Shape lock leaves the portrait subject context.");
        const auto accepted = details.locks.leaf_domain ? lock.locked_leaves : [&]() {
            std::vector<Key> result; for (const auto f : lock.locked_faces) result.push_back({f,0,0}); return result;
        }();
        const auto nested = details.locks.leaf_domain ? lock.nested_leaves : [&]() {
            std::vector<Key> result; for (const auto f : lock.nested_faces) result.push_back({f,0,0}); return result;
        }();
        for (const auto& key : accepted) {
            const auto label = std::binary_search(nested.begin(),nested.end(),key) ? std::string("iris") : lock.label;
            features.emplace(key,std::make_pair(lock.subject_id,label));
            parents[key.source_face_id] = lock.subject_id;
            risks[lock.subject_id+":"+label] = lock.reasons;
        }
    }
    std::vector<std::string> labels(keys.size()), subjects(keys.size());
    std::vector<int32_t> component_for_leaf(keys.size(),-1);
    std::set<size_t> reserved(details.reserved_faces.begin(),details.reserved_faces.end());
    std::set<size_t> mixed_roots;
    std::map<AI::BeautyLeafKey,size_t> previous_child_slots;
    for (const auto& child : previous_children) {
        const AI::BeautyLeafKey key{child.face_id,child.path.depth,child.path.value};
        if (!key.valid(count) || !key.depth || !std::isfinite(child.confidence) || child.confidence < 0 || child.confidence > 1)
            throw std::invalid_argument("Invalid existing child color identity.");
        size_t slot = palette.size();
        for (size_t i = 0; i < palette.size(); ++i) if (palette[i].rgb == child.color) { slot = i; break; }
        if (slot == palette.size() || !previous_child_slots.emplace(key,slot).second)
            throw std::invalid_argument("Existing child color leaves the fixed palette or repeats a leaf.");
        mixed_roots.insert(child.face_id);
    }
    for (const auto& child : analysis.subface_labels) mixed_roots.insert(child.face_id);
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto f = keys[i].source_face_id;
        const auto detail = features.find(keys[i]);
        if (detail != features.end()) {
            subjects[i] = detail->second.first; labels[i] = detail->second.second;
            if (original[f] == palette.size()) original[f] = nearest(f);
            continue;
        }
        // The remainder of a refined feature root is its face underlayer.
        if (!parents[f].empty()) { subjects[i] = parents[f]; labels[i] = "face"; }
        else {
            if (reserved.count(f) || mixed_roots.count(f) || original[f] == palette.size() || subject_ids.size() != 1 ||
                analysis.face_confidence[f] < AI::SemanticColoring::minimum_confidence) continue;
            switch (analysis.face_labels[f]) {
            case Label::FaceSkin: case Label::BodySkin: {
                const auto& rgb = details.base_colors[f];
                const auto source = PortraitColorPlan::oklab({rgb[0],rgb[1],rgb[2]});
                // A neutral base or red fabric inside a coarse body mask is not skin.
                if (source[0] < .40f || source[1] < -.01f || source[2] < .006f ||
                    std::hypot(source[1],source[2]) > .13f) continue;
                labels[i] = analysis.face_labels[f] == Label::FaceSkin ? "face" : "skin"; break;
            }
            case Label::Hair: labels[i] = "hair"; break;
            case Label::Clothes: labels[i] = "cloth"; break;
            case Label::Accessories: labels[i] = "accessories"; break;
            default: continue;
            }
            // Coarse native masks can be bound only in a single-subject run.
            subjects[i] = *subject_ids.begin();
        }
        if (original[f] == palette.size()) original[f] = nearest(f);
    }
    const auto adjacency_leaves = domain.adjacency(source_mesh,surface);
    for (size_t first = 0; first < keys.size(); ++first) {
        if (labels[first].empty() || component_for_leaf[first] >= 0) continue;
        const auto index = int32_t(plan.components.size());
        PortraitColorPlan::Component component;
        component.id = "component-"+std::to_string(index);
        component.label = labels[first]; component.region = subjects[first]+":"+labels[first];
        component.risks = risks[component.region];
        std::vector<size_t> pending {first}; component_for_leaf[first] = index;
        std::array<double,3> total {};
        std::map<size_t,double> votes;
        std::set<size_t> roots;
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const auto i = pending[cursor], f = keys[i].source_face_id;
            const auto area = std::max(surface.areas[f]*std::ldexp(1.,-2*keys[i].depth),1e-15);
            component.leaves.push_back(keys[i]); component.area += area; roots.insert(f);
            votes[original[f]] += area;
            for (size_t channel = 0; channel < 3; ++channel) total[channel] += area*details.base_colors[f][channel];
            for (const auto n : adjacency_leaves[i]) if (component_for_leaf[n] < 0 && labels[n] == labels[first] && subjects[n] == subjects[first] &&
                original[keys[n].source_face_id] == original[keys[first].source_face_id]) {
                const auto a = details.base_colors[f], b = details.base_colors[keys[n].source_face_id];
                if (!PortraitColorPlan::feature(labels[first]) && PortraitColorPlan::distance(
                    PortraitColorPlan::oklab({a[0],a[1],a[2]}),PortraitColorPlan::oklab({b[0],b[1],b[2]})) > .06) continue;
                component_for_leaf[n] = index; pending.push_back(n);
            }
        }
        for (size_t channel = 0; channel < 3; ++channel) component.source[channel] = float(total[channel]/component.area);
        component.original_slot = std::max_element(votes.begin(),votes.end(),[](const auto& a,const auto& b) { return a.second < b.second; })->first;
        std::sort(component.leaves.begin(),component.leaves.end());
        for (const auto f : roots) {
            const auto found = visibility.find(f);
            if (found != visibility.end()) for (const auto& view : found->second) component.visible_pixels[view.first] += view.second;
        }
        plan.components.push_back(std::move(component));
    }
    std::vector<std::set<size_t>> adjacency(plan.components.size());
    for (size_t i = 0; i < keys.size(); ++i) if (component_for_leaf[i] >= 0) {
        const auto a = size_t(component_for_leaf[i]);
        for (const auto n : adjacency_leaves[i]) if (component_for_leaf[n] >= 0 && component_for_leaf[n] != component_for_leaf[i]) {
            const auto b = size_t(component_for_leaf[n]);
            adjacency[a].insert(b); adjacency[b].insert(a);
        }
    }
    for (size_t i = 0; i < plan.components.size(); ++i)
        plan.components[i].neighbors.assign(adjacency[i].begin(),adjacency[i].end());
    // Repair only already-rendered, directly evidenced skin children. Their
    // boundaries and all unevidenced siblings remain the preserved R4 ones.
    if (subject_ids.size() == 1) for (const auto& child : analysis.subface_labels) {
        const Key key{child.face_id,child.path.depth,child.path.value};
        const auto old = previous_child_slots.find(key);
        if (old == previous_child_slots.end() || child.label != Label::FaceSkin ||
            !std::isfinite(child.confidence) || child.confidence < AI::SemanticColoring::minimum_confidence ||
            child.confidence > 1 || !child.samples || reserved.count(child.face_id) ||
            !parents[child.face_id].empty()) continue;
        bool overlapping = false;
        for (const auto& other : previous_child_slots) if (!(other.first == key) &&
            (key.contains(other.first) || other.first.contains(key))) { overlapping = true; break; }
        for (const auto& other : analysis.subface_labels) if (other.face_id == key.source_face_id &&
            other.label != Label::FaceSkin) {
            const Key semantic{other.face_id,other.path.depth,other.path.value};
            if (key.contains(semantic) || semantic.contains(key)) { overlapping = true; break; }
        }
        if (overlapping) continue;
        PortraitColorPlan::Component component;
        component.id = "existing-skin-"+std::to_string(key.source_face_id)+"-"+std::to_string(key.depth)+"-"+std::to_string(key.path);
        component.region = *subject_ids.begin()+":skin"; component.label = "skin"; component.leaves = {key};
        component.original_slot = old->second; component.area = std::max(surface.areas[key.source_face_id]*std::ldexp(1.,-2*key.depth),1e-15);
        const auto& rgb = details.base_colors[key.source_face_id]; component.source = {rgb[0],rgb[1],rgb[2]};
        component.risks = {"EXISTING_VERIFIED_SKIN_SUBFACE","SOURCE_COLOR_INHERITED_FROM_VERIFIED_ROOT"};
        plan.components.push_back(std::move(component));
    }
    plan.optimize();
    return plan;
}

inline void apply_portrait_color_plan(const PortraitColorPlan& plan,
    AI::SemanticColoring::FaceColors& faces, AI::SemanticColoring::SubfaceColors& children) {
    plan.validate_solution();
    if (plan.repair_domain) {
        // Resolve ancestor overrides into the complete partition before changing a sibling.
        std::set<size_t> targets;
        for (const auto& c : plan.components) for (const auto& key : c.leaves) if (key.depth) targets.insert(key.source_face_id);
        AI::SemanticColoring::SubfaceColors expanded;
        std::map<size_t,std::vector<AI::SemanticColoring::SubfaceColor>> groups;
        for (const auto& child : children) {
            if (targets.count(child.face_id)) groups[child.face_id].push_back(child);
            else expanded.push_back(child);
        }
        for (const auto& key : plan.repair_domain->split_leaves) if (targets.count(key.source_face_id)) {
            const AI::SemanticColoring::SubfaceColor* match = nullptr;
            for (const auto& child : groups[key.source_face_id]) {
                const AI::BeautyLeafKey old{child.face_id,child.path.depth,child.path.value};
                if (old.contains(key) && (!match || child.path.depth > match->path.depth)) match = &child;
            }
            if (match) expanded.push_back({key.source_face_id,{key.depth,key.path},match->color,match->confidence});
        }
        children = std::move(expanded);
    }
    std::map<AI::BeautyLeafKey,float> original_confidence;
    for (const auto& child : children)
        original_confidence[{child.face_id,child.path.depth,child.path.value}] = child.confidence;
    std::map<size_t,PortraitColorPlan::RGB> roots(faces.begin(),faces.end());
    std::set<size_t> changed_roots;
    for (size_t i = 0; i < plan.components.size(); ++i) {
        const auto& c = plan.components[i];
        if (c.conflict || c.label == "imouth") continue;
        const auto& rgb = plan.palette.at(plan.decisions[i].slot).rgb;
        for (const auto& key : c.leaves) {
            if (!key.depth) {
                const auto before = roots.find(key.source_face_id);
                if (before == roots.end() || before->second != rgb) changed_roots.insert(key.source_face_id);
                roots[key.source_face_id] = rgb;
            }
        }
    }
    std::map<size_t,std::vector<AI::BeautyLeafKey>> changed_keys;
    for (const auto& c : plan.components) if (!c.conflict && c.label != "imouth")
        for (const auto& key : c.leaves) if (key.depth || changed_roots.count(key.source_face_id))
            changed_keys[key.source_face_id].push_back(key);
    children.erase(std::remove_if(children.begin(),children.end(),[&](const auto& child) {
        const auto found = changed_keys.find(child.face_id);
        if (found == changed_keys.end()) return false;
        const AI::BeautyLeafKey key{child.face_id,child.path.depth,child.path.value};
        return std::any_of(found->second.begin(),found->second.end(),[&](const auto& changed) { return changed.contains(key); });
    }),children.end());
    for (size_t i = 0; i < plan.components.size(); ++i) {
        const auto& c = plan.components[i];
        if (c.conflict || c.label == "imouth") continue;
        const auto& rgb = plan.palette.at(plan.decisions[i].slot).rgb;
        for (const auto& key : c.leaves) if (key.depth) {
            const auto existing = original_confidence.find(key);
            children.push_back({key.source_face_id,{key.depth,key.path},rgb,
                existing == original_confidence.end() ? 1.f : existing->second});
        }
    }
    faces.assign(roots.begin(),roots.end());
    std::sort(children.begin(),children.end(),[](const auto& a,const auto& b) {
        return a.face_id == b.face_id ? a.path < b.path : a.face_id < b.face_id;
    });
}
} // namespace Slic3r::GUI
