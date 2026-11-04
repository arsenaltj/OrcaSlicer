#pragma once

#include <nlohmann/json.hpp>
#include "slic3r/AI/Contracts/ModelBaseBinding.hpp"
#include <string>
#include <cmath>
#include <utility>
#include <vector>
#include <algorithm>

namespace Slic3r::GUI::PortraitPrint {

enum class Submission { Submitted, Pending, Cancelled, Rejected };
inline bool accepted(Submission value) { return value == Submission::Submitted || value == Submission::Pending; }

enum class Stage { Idle, Project, Palette, Design, Model, Check, Color, SaveColor,
                   Filaments, Import, Recommend, Slice, PrintReady, Partition, Base };

// Persisted v1-v3 stage numbers are stable. Workbench order is explicit so new
// local phases cannot accidentally authorize any of the old generation phases.
inline int workbench_order(Stage stage) {
    switch (stage) {
    case Stage::Project: return 0; case Stage::Check: return 1;
    case Stage::Partition: return 2; case Stage::Color: return 3;
    case Stage::SaveColor: return 4; case Stage::Base: return 5;
    case Stage::Filaments: return 6; case Stage::Import: return 7;
    case Stage::Recommend: return 8; case Stage::Slice: return 9;
    case Stage::PrintReady: return 10; default: return -1;
    }
}
inline bool model_processing(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 1 && order <= 7;
}
inline bool before_import(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 0 && order < 7;
}
inline bool protects_saved_color(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 4 && order <= 7;
}

inline bool same_model_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    const auto character = [](char c) {
#ifdef _WIN32
        // Asset paths may come from a native path or a generic path join.
        // Those spellings must retain the same workflow ownership on Windows.
        if (c == '\\') return '/';
        if (c >= 'A' && c <= 'Z') return char(c + ('a' - 'A'));
#endif
        return c;
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (character(a[i]) != character(b[i])) return false;
    return true;
}

inline bool valid_frozen(const nlohmann::json& input) {
    if (!input.is_object()) return false;
    for (const auto* key : {"image", "prompt", "style", "custom_style", "provider", "design_provider", "design_model",
                           "geometry_model", "geometry_quality", "texture_quality", "output_format", "profile", "print_mode", "shadow_color", "color_distance"})
        if (!input.contains(key) || !input.at(key).is_string()) return false;
    if (input.at("style") != "realistic" || (input.at("image") == "" && input.at("prompt") == "")) return false;
    if (!input.contains("face_limit") || !input.at("face_limit").is_number_integer()) return false;
    if (input.at("color_distance") != "ciede2000" && input.at("color_distance") != "delta_e76") return false;
    for (const auto* key : {"width_mm", "nozzle_mm", "line_width_mm", "minimum_feature_mm"}) {
        if (!input.contains(key) || !input.at(key).is_number()) return false;
        const double value = input.at(key).get<double>();
        if (!std::isfinite(value) || value < 0 || value > 100000) return false;
    }
    return true;
}

// Durable intent ledger. A reservation is saved BEFORE sending a request. It
// survives crashes even when the response (and therefore the remote ID) is lost.
// Authorization and automatic continuation deliberately never survive restart.
struct State {
    int record_version {4};
    std::string id;
    std::string entry {"generation"};
    bool from_workbench() const { return entry == "completed_model"; }
    std::string color_asset_id() const {
        if (record_version >= 4 && boundary_algorithm_version >= 8)
            return "finish-portrait-slots-" + id;
        return (record_version >= 4 ? "finish-portrait-v4-" : "finish-portrait-") + id;
    }
    bool colored_saved {false}, semantic_applied {false}, person_detected {false};
    std::string evidence_status, application_status;
    std::string geometry_id, appearance_id, region_binding_digest;
    uint64_t edit_revision {0};
    nlohmann::json base_binding;
    Stage stage {Stage::Idle};
    bool paused {false}, failed {false}, authorized {false}, restored {false};
    bool design_reserved {false}, model_reserved {false}, palette_confirmed {false};
    bool fallback_confirmed {false}, near_color_fallback {false};
    bool design_risk_accepted {false}, model_risk_accepted {false};
    bool repair_saved {false}, slice_started {false};
    bool repair_checked {false}, partition_completed {false};
    uint32_t boundary_algorithm_version {0};
    bool round_base_enabled {true}, native_repair_required {true}, native_repair_completed {false};
    int generation_attempt {0}, palette_retries {0}, design_retries {0}, model_retries {0}, conversion_retries {0};
    std::string semantic_status, recovery_project, native_geometry_id, preview_result_id;
    std::vector<std::string> notices, retired_task_ids;
    int requested_count {6}, final_count {0};
    std::string reason, request_id, design_job_id, model_job_id, provider_task_id, conversion_task_id;
    std::string source_image_hash, source_image_copy, design_path;
    std::string authorization_id;
    std::vector<std::string> final_palette;
    std::string original_model_path, original_model_hash, model_path, model_hash;
    std::string repair_source_hash, repair_result_hash;
    std::string basis_signature, suite_revision, workspace_id, project_revision, slice_revision, gcode_path;
    nlohmann::json frozen = nlohmann::json::object();

    bool active() const { return !id.empty() && stage != Stage::Idle && stage != Stage::PrintReady; }
    bool owns_workbench_model(const std::string& path, const std::string& geometry,
                              const std::string& appearance) const {
        // Completion ends automation, not ownership of its source and saved
        // colour carrier. Reopening either must not start the legacy repair.
        return from_workbench() && !id.empty() && stage != Stage::Idle && !path.empty() &&
            !geometry_id.empty() && geometry == geometry_id &&
            !appearance_id.empty() && appearance == appearance_id &&
            (same_model_path(path, original_model_path) || same_model_path(path, model_path));
    }
    bool running() const { return active() && !paused && !failed && authorized; }
    bool current(const std::string& token, Stage expected) const {
        return running() && id == token && stage == expected;
    }
    bool partition_needs_rebuild(uint32_t version) const {
        return from_workbench() && record_version >= 4 && boundary_algorithm_version != 0 &&
            boundary_algorithm_version != version && workbench_order(stage) >= workbench_order(Stage::Partition);
    }
    bool rebuild_partition(uint32_t version, const std::string& repaired_path, const std::string& verified_hash) {
        if (!partition_needs_rebuild(version) || !paused || !repair_checked || !repair_saved ||
            repaired_path.empty() || repair_result_hash.empty() || verified_hash != repair_result_hash ||
            geometry_id.empty() || appearance_id.empty()) return false;
        model_path = repaired_path; model_hash = verified_hash;
        stage = Stage::Check;
        boundary_algorithm_version = version;
        partition_completed = colored_saved = semantic_applied = person_detected = false;
        slice_started = native_repair_completed = near_color_fallback = false;
        native_repair_required = false;
        basis_signature.clear(); region_binding_digest.clear();
        // Color publication preserves the repaired geometry and original
        // appearance identities. Keep them until restore verifies the saved
        // repair, including if the app exits immediately after this record.
        evidence_status.clear(); application_status.clear(); semantic_status.clear();
        native_geometry_id.clear(); project_revision.clear(); slice_revision.clear();
        preview_result_id.clear(); gcode_path.clear();
        return true;
    }
    void pause(std::string message, bool hard = false) {
        paused = true; failed = hard; reason = std::move(message);
    }
    bool advance(const std::string& token, Stage expected, Stage next) {
        if (!current(token, expected)) return false;
        if (from_workbench()) {
            if (workbench_order(expected) < 0 || workbench_order(next) != workbench_order(expected) + 1 ||
                (expected == Stage::Check && !repair_checked) ||
                (expected == Stage::Partition && (!partition_completed || basis_signature.empty()))) return false;
        } else if (int(next) != int(expected) + 1) return false;
        stage = next; reason.clear(); return true;
    }
    bool reserve_design(const std::string& token, const std::string& request) {
        if (from_workbench() || !current(token, Stage::Palette) || design_reserved || request.empty()) return false;
        request_id = request; design_reserved = true; return true;
    }
    bool observe_job(const std::string& token, const std::string& job,
                     const std::string& provider = {}, const std::string& conversion = {}, int attempt = 0,
                     const std::vector<std::string>& terminal_tasks = {}) {
        if (from_workbench() || id != token || !active() || job.empty() || !design_reserved) return false;
        if (attempt < generation_attempt) return false;
        const auto terminal = [&](const std::string& task) {
            return !task.empty() && std::find(terminal_tasks.begin(), terminal_tasks.end(), task) != terminal_tasks.end();
        };
        auto next_provider = provider_task_id;
        auto next_conversion = conversion_task_id;
        auto retired = retired_task_ids;
        if (attempt > generation_attempt && terminal(next_provider)) {
            retired.push_back(next_provider);
            next_provider.clear(); next_conversion.clear();
        }
        if (!conversion.empty() && conversion != next_conversion && terminal(next_conversion)) {
            retired.push_back(next_conversion); next_conversion.clear();
        }
        if ((!design_job_id.empty() && design_job_id != job) ||
            (!provider.empty() && !next_provider.empty() && provider != next_provider) ||
            (!conversion.empty() && !next_conversion.empty() && conversion != next_conversion) ||
            (!provider.empty() && std::find(retired.begin(), retired.end(), provider) != retired.end()) ||
            (!conversion.empty() && std::find(retired.begin(), retired.end(), conversion) != retired.end())) return false;
        // Earlier phases can arrive after a remote submission. Missing fields
        // never erase an identity, and conflicting fields change nothing.
        design_job_id = job;
        provider_task_id = std::move(next_provider);
        conversion_task_id = std::move(next_conversion);
        retired_task_ids = std::move(retired);
        if (!provider.empty()) provider_task_id = provider;
        if (!conversion.empty()) conversion_task_id = conversion;
        generation_attempt = attempt;
        return true;
    }
    bool reserve_model(const std::string& token, const std::string& job) {
        if (from_workbench() || !current(token, Stage::Model) || model_reserved || design_job_id.empty() || job != design_job_id)
            return false;
        model_reserved = true; model_job_id = job; return true;
    }
    bool accept_repair(const std::string& source, const std::string& candidate,
                       bool accepted_and_saved, bool identities_match) {
        // A manual acceptance may finish while automation is paused. Record
        // the published identity, but leave authorization and pause untouched.
        if (!active() || failed || stage != Stage::Check || !accepted_and_saved || !identities_match ||
            source.empty() || candidate.empty() || source != model_hash) return false;
        repair_source_hash = source; repair_result_hash = candidate; repair_saved = true;
        repair_checked = true; native_repair_required = false;
        model_hash = candidate; return true;
    }
    bool slice_complete(const std::string& token, const std::string& revision,
                        bool completed, bool valid_result, const std::string& gcode) {
        if (!current(token, Stage::Slice) || !slice_started || revision.empty() || revision != slice_revision ||
            !completed || !valid_result || gcode.empty()) return false;
        gcode_path = gcode; stage = Stage::PrintReady; return true;
    }
    bool bind_slice_input(const std::string& token, const std::string& revision) {
        if (!current(token, Stage::Slice) || slice_started || revision.empty()) return false;
        slice_revision = revision; slice_started = true; return true;
    }
};

inline nlohmann::json encode(const State& s) {
    return {{"schema", s.from_workbench() ? s.record_version >= 4 ? "orca.portrait-print/v4" : "orca.portrait-print/v3" : "orca.portrait-print/v2"}, {"id", s.id}, {"stage", int(s.stage)},
        {"repair_checked", s.repair_checked}, {"partition_completed", s.partition_completed},
        {"boundary_algorithm_version", s.boundary_algorithm_version},
        {"entry", s.entry}, {"colored_saved", s.colored_saved}, {"semantic_applied", s.semantic_applied},
        {"person_detected", s.person_detected}, {"evidence_status", s.evidence_status}, {"application_status", s.application_status},
        {"geometry_id", s.geometry_id}, {"appearance_id", s.appearance_id}, {"edit_revision", s.edit_revision},
        {"region_binding_digest", s.region_binding_digest}, {"base_binding", s.base_binding},
        {"round_base_enabled", s.round_base_enabled}, {"base_height_mm", 3.0}, {"base_overlap_mm", 0.2},
        {"native_repair_required", s.native_repair_required}, {"native_repair_completed", s.native_repair_completed},
        {"native_geometry_id", s.native_geometry_id}, {"preview_result_id", s.preview_result_id},
        {"semantic_status", s.semantic_status}, {"recovery_project", s.recovery_project},
        {"generation_attempt", s.generation_attempt}, {"palette_retries", s.palette_retries},
        {"design_retries", s.design_retries}, {"model_retries", s.model_retries}, {"conversion_retries", s.conversion_retries},
        {"notices", s.notices}, {"retired_task_ids", s.retired_task_ids},
        {"paused", s.paused}, {"failed", s.failed}, {"reason", s.reason},
        {"design_reserved", s.design_reserved}, {"model_reserved", s.model_reserved},
        {"palette_confirmed", s.palette_confirmed}, {"fallback_confirmed", s.fallback_confirmed},
        {"near_color_fallback", s.near_color_fallback}, {"design_risk_accepted", s.design_risk_accepted},
        {"model_risk_accepted", s.model_risk_accepted}, {"repair_saved", s.repair_saved},
        {"slice_started", s.slice_started},
        {"requested_count", s.requested_count}, {"final_count", s.final_count},
        {"request_id", s.request_id}, {"design_job_id", s.design_job_id}, {"model_job_id", s.model_job_id},
        {"provider_task_id", s.provider_task_id}, {"conversion_task_id", s.conversion_task_id},
        {"source_image_hash", s.source_image_hash}, {"source_image_copy", s.source_image_copy},
        {"authorization_id", s.authorization_id}, {"final_palette", s.final_palette},
        {"design_path", s.design_path}, {"original_model_path", s.original_model_path},
        {"original_model_hash", s.original_model_hash}, {"model_path", s.model_path}, {"model_hash", s.model_hash},
        {"repair_source_hash", s.repair_source_hash}, {"repair_result_hash", s.repair_result_hash},
        {"basis_signature", s.basis_signature}, {"suite_revision", s.suite_revision},
        {"workspace_id", s.workspace_id}, {"project_revision", s.project_revision}, {"slice_revision", s.slice_revision}, {"gcode_path", s.gcode_path},
        {"frozen", s.frozen}};
}

inline bool decode(const nlohmann::json& j, State& result) {
    try {
        const auto schema = j.at("schema").get<std::string>();
        if (schema != "orca.portrait-print/v1" && schema != "orca.portrait-print/v2" &&
            schema != "orca.portrait-print/v3" && schema != "orca.portrait-print/v4") return false;
        const int stage = j.at("stage").get<int>();
        if (stage < int(Stage::Project) || stage > int(schema == "orca.portrait-print/v4" ? Stage::Base : Stage::PrintReady)) return false;
        State s;
        s.record_version = schema == "orca.portrait-print/v4" ? 4 : schema == "orca.portrait-print/v3" ? 3 : 2;
        s.id = j.at("id").get<std::string>(); s.stage = Stage(stage);
        if (s.id.empty() || s.id.size() > 128 || s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            return false;
        s.frozen = j.at("frozen");
        if (s.record_version >= 3) {
            s.entry = j.at("entry").get<std::string>();
            s.geometry_id = j.at("geometry_id").get<std::string>();
            s.appearance_id = j.at("appearance_id").get<std::string>();
            s.edit_revision = j.at("edit_revision").get<uint64_t>();
            s.colored_saved = j.at("colored_saved").get<bool>();
            s.semantic_applied = j.at("semantic_applied").get<bool>();
            s.person_detected = j.value("person_detected", false);
            s.evidence_status = j.value("evidence_status", std::string{});
            s.application_status = j.value("application_status", std::string{});
            s.region_binding_digest = j.at("region_binding_digest").get<std::string>();
            s.base_binding = j.at("base_binding");
            if (!s.base_binding.is_null()) {
                AI::ModelBaseBinding binding;
                if (!AI::ModelBaseBinding::decode(s.base_binding, binding)) return false;
            }
            if (!s.from_workbench() || s.geometry_id.empty() || s.appearance_id.empty() ||
                (s.stage >= Stage::Palette && s.stage <= Stage::Model) || !s.frozen.is_object()) return false;
        } else if (!valid_frozen(s.frozen)) return false;
#define PORTRAIT_READ(field) s.field = j.at(#field).get<decltype(s.field)>()
        PORTRAIT_READ(failed); PORTRAIT_READ(reason);
        PORTRAIT_READ(design_reserved); PORTRAIT_READ(model_reserved); PORTRAIT_READ(palette_confirmed);
        PORTRAIT_READ(fallback_confirmed); PORTRAIT_READ(near_color_fallback);
        PORTRAIT_READ(design_risk_accepted); PORTRAIT_READ(model_risk_accepted); PORTRAIT_READ(repair_saved);
        PORTRAIT_READ(slice_started);
        PORTRAIT_READ(requested_count); PORTRAIT_READ(final_count);
        PORTRAIT_READ(request_id); PORTRAIT_READ(design_job_id); PORTRAIT_READ(model_job_id);
        PORTRAIT_READ(provider_task_id); PORTRAIT_READ(conversion_task_id);
        PORTRAIT_READ(source_image_hash); PORTRAIT_READ(source_image_copy); PORTRAIT_READ(design_path);
        PORTRAIT_READ(authorization_id); PORTRAIT_READ(final_palette);
        PORTRAIT_READ(original_model_path); PORTRAIT_READ(original_model_hash);
        PORTRAIT_READ(model_path); PORTRAIT_READ(model_hash);
        PORTRAIT_READ(repair_source_hash); PORTRAIT_READ(repair_result_hash);
        PORTRAIT_READ(basis_signature); PORTRAIT_READ(suite_revision); PORTRAIT_READ(project_revision);
        PORTRAIT_READ(workspace_id);
        PORTRAIT_READ(slice_revision); PORTRAIT_READ(gcode_path);
#undef PORTRAIT_READ
        s.round_base_enabled = j.value("round_base_enabled", true);
        if (j.value("base_height_mm", 3.0) != 3.0 || j.value("base_overlap_mm", 0.2) != 0.2) return false;
        s.native_repair_required = j.value("native_repair_required", true);
        s.native_repair_completed = j.value("native_repair_completed", false);
        s.repair_checked = s.record_version >= 4 && j.value("repair_checked", false);
        s.partition_completed = s.record_version >= 4 && j.value("partition_completed", false);
        s.boundary_algorithm_version = j.value("boundary_algorithm_version", uint32_t(0));
        s.native_geometry_id = j.value("native_geometry_id", std::string{});
        s.preview_result_id = j.value("preview_result_id", std::string{});
        s.semantic_status = j.value("semantic_status", std::string{});
        s.recovery_project = j.value("recovery_project", std::string{});
        s.generation_attempt = j.value("generation_attempt", 0);
        s.palette_retries = j.value("palette_retries", 0); s.design_retries = j.value("design_retries", 0);
        s.model_retries = j.value("model_retries", 0); s.conversion_retries = j.value("conversion_retries", 0);
        s.notices = j.value("notices", std::vector<std::string>{});
        s.retired_task_ids = j.value("retired_task_ids", std::vector<std::string>{});
        if (s.generation_attempt < 0 || s.palette_retries < 0 || s.design_retries < 0 ||
            s.model_retries < 0 || s.conversion_retries < 0) return false;
        if (s.requested_count < 4 || s.requested_count > 6 ||
            (s.final_count != 0 && (s.final_count < 4 || s.final_count > 6)) ||
            (!s.final_palette.empty() && s.final_palette.size() != size_t(s.final_count)) ||
            (s.design_reserved && s.request_id.empty()) ||
            (s.model_reserved && (s.model_job_id.empty() || s.model_job_id != s.design_job_id))) return false;
        if (s.from_workbench() && (s.original_model_hash.empty() || s.model_hash.empty() ||
            s.model_path.empty() || s.design_reserved || s.model_reserved)) return false;
        if (s.from_workbench() && s.record_version >= 4 &&
            ((s.partition_completed && (!s.repair_checked || s.basis_signature.empty())) ||
             (s.repair_checked && (!s.repair_saved || s.repair_result_hash.empty())))) return false;
        s.restored = true; s.authorized = false; s.paused = s.active();
        if (s.active() && !s.failed) s.reason = "流程已恢复，请核对当前工程后点击“继续一键流程”。";
        result = std::move(s); return true;
    } catch (const std::exception&) { return false; }
}

} // namespace Slic3r::GUI::PortraitPrint
