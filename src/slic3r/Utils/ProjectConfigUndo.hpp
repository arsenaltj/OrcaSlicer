#pragma once

#include "libslic3r/PrintConfig.hpp"
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::UndoRedo::ProjectConfigUndo {

// Native project settings only. Printer connections, provider state, and
// installed preset definitions do not belong in this change record.
class Change {
public:
    static std::shared_ptr<const Change> capture(const DynamicPrintConfig& before,
        const std::vector<std::string>& before_presets, const DynamicPrintConfig& after,
        const std::vector<std::string>& after_presets)
    {
        auto change=std::shared_ptr<Change>(new Change);
        auto keys=before.keys();const auto after_keys=after.keys();
        keys.insert(keys.end(),after_keys.begin(),after_keys.end());
        std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        for(const auto& key:keys) {
            const auto* a=before.option(key);const auto* b=after.option(key);
            if(equal(a,b)) continue;
            change->m_keys.push_back(key);
            if(a) change->m_before.set_key_value(key,a->clone());
            if(b) change->m_after.set_key_value(key,b->clone());
        }
        change->m_presets_changed=before_presets!=after_presets;
        if(change->m_presets_changed) {change->m_before_presets=before_presets;change->m_after_presets=after_presets;}
        if(change->m_keys.empty() && !change->m_presets_changed) return {};
        return change;
    }

    // Apply to a temporary config. A conflict must never partially overwrite
    // live state; prepare_jump below publishes its destination only on success.
    bool apply(DynamicPrintConfig& config,std::vector<std::string>& presets,bool forward,std::string& error) const
    {
        const auto& expected=forward?m_before:m_after;const auto& replacement=forward?m_after:m_before;
        const auto& expected_presets=forward?m_before_presets:m_after_presets;
        if(m_presets_changed && presets!=expected_presets) {error="Material preset selections changed after this operation.";return false;}
        for(const auto& key:m_keys) if(!equal(config.option(key),expected.option(key))) {
            error="Project setting changed after this operation: "+key;return false;
        }
        for(const auto& key:m_keys) {
            if(const auto* option=replacement.option(key)) config.set_key_value(key,option->clone());
            else config.erase(key);
        }
        if(m_presets_changed) presets=forward?m_after_presets:m_before_presets;
        return true;
    }

    size_t memsize() const
    {
        // Conservative allocation estimate for native option objects, map
        // nodes, vector elements and strings. Count it in the stack's budget.
        size_t bytes=sizeof(*this);
        for(const auto& key:m_keys) bytes+=sizeof(std::string)+key.capacity();
        for(const auto* config:{&m_before,&m_after}) for(const auto& key:config->keys()) {
            const auto* option=config->option(key);
            bytes+=256+key.size()+option->serialize().size()*2;
            if(const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(option)) bytes+=vector->size()*64;
        }
        for(const auto* names:{&m_before_presets,&m_after_presets})
            for(const auto& name:*names) bytes+=sizeof(std::string)+name.capacity();
        return bytes;
    }

private:
    static bool equal(const ConfigOption* a,const ConfigOption* b)
    { return (!a || !b) ? a==b : a->type()==b->type() && *a==*b; }
    DynamicPrintConfig m_before,m_after;
    std::vector<std::string> m_keys,m_before_presets,m_after_presets;
    bool m_presets_changed {false};
};

struct Prepared {
    DynamicPrintConfig config;
    std::vector<std::string> filament_presets;
    bool changed {false};
};

// A snapshot denotes the start of an action. Crossing [older,newer) applies
// its changes in chronological order for redo, reverse order for undo.
// Snapshot is a template parameter so the native history logic is testable
// without constructing wx windows, OpenGL canvases, or a second undo stack.
template<class Snapshot>
bool prepare_jump(const std::vector<Snapshot>& history,size_t from,size_t to,
    const DynamicPrintConfig& config,const std::vector<std::string>& presets,Prepared& destination,std::string& error)
{
    error.clear();
    for(size_t i=1;i<history.size();++i) if(history[i-1].timestamp>=history[i].timestamp) {
        error="Project undo history is not strictly ordered.";return false;
    }
    const auto find=[&](size_t time){return std::lower_bound(history.begin(),history.end(),time,[](const auto& s,size_t t){return s.timestamp<t;});};
    const auto a=find(from),b=find(to);
    if(a==history.end() || b==history.end() || a->timestamp!=from || b->timestamp!=to) {
        error="Project undo destination is no longer available.";return false;
    }
    std::vector<std::shared_ptr<const Change>> changes;
    const bool forward=from<to;
    if(forward) {for(auto it=a;it!=b;++it) if(it->project_config_change) changes.push_back(it->project_config_change);}
    else {for(auto it=a;it!=b;) {--it;if(it->project_config_change) changes.push_back(it->project_config_change);}}
    Prepared prepared;
    if(!changes.empty()) {
        prepared.config=config;prepared.filament_presets=presets;
        for(const auto& change:changes) if(!change->apply(prepared.config,prepared.filament_presets,forward,error)) return false;
        prepared.changed=true;
    }
    destination=std::move(prepared);
    return true;
}
} // namespace Slic3r::UndoRedo::ProjectConfigUndo
