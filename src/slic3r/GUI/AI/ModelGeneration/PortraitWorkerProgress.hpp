#pragma once
#include "PortraitOptimization.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r::GUI {
// Telemetry never grants permission to apply a result. Foreign, late, malformed
// and incomplete records leave both the sequence and visible progress unchanged.
inline bool decode_portrait_worker_progress(const std::string& bytes, const std::string& request,
    const std::string& source, const std::string& geometry, uint64_t& sequence, PortraitProgress& output)
{
    if (bytes.size()>4096) return false;
    try {
        const auto value=nlohmann::json::parse(bytes);
        if (value.at("schema")!="orca.portrait-progress/v1" || value.at("request_id")!=request ||
            value.at("source_sha256")!=source || value.at("geometry_id")!=geometry) return false;
        for (const char* key : {"sequence","completed","total"})
            if (!value.at(key).is_number_integer() || value.at(key)<0 || value.at(key)>10000000) return false;
        const auto serial=value.at("sequence").get<uint64_t>();
        const auto done=value.at("completed").get<uint64_t>(), total=value.at("total").get<uint64_t>();
        const auto stage=value.at("stage").get<std::string>(), detail=value.at("detail").get<std::string>();
        if (serial<=sequence || detail.size()>1024 || (total && done>total)) return false;
        if (stage!="recognizing" && stage!="ownership" && stage!="coloring") return false;
        output={stage=="recognizing" ? PortraitStage::Recognizing : stage=="ownership" ? PortraitStage::Ownership : PortraitStage::Coloring,
            detail,done,total};
        sequence=serial;
        return true;
    } catch (...) { return false; }
}
}
