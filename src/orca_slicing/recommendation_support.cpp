#include "recommendation_support.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace orca::slicing {
namespace {

Json diff_json(const Slic3r::DynamicPrintConfig& base, const Slic3r::DynamicPrintConfig& candidate)
{
    Json result = Json::object();
    for (const auto& key : base.diff(candidate)) {
        const auto* before = base.option(key);
        const auto* after = candidate.option(key);
        result[key] = Json{{"before", before ? before->serialize() : Json()},
                           {"after", after ? after->serialize() : Json()}};
    }
    for (const auto& key : candidate.diff(base)) {
        if (result.contains(key))
            continue;
        const auto* before = base.option(key);
        const auto* after = candidate.option(key);
        result[key] = Json{{"before", before ? before->serialize() : Json()},
                           {"after", after ? after->serialize() : Json()}};
    }
    return result;
}

RecommendationCandidate make_candidate(const LoadedProject& project, std::string id,
                                       std::string quality, Json reasons)
{
    RecommendationCandidate result;
    result.candidateId = std::move(id);
    result.quality = std::move(quality);
    result.config = result.candidateId == "baseline" ? project.config : quality_config(project.config, result.quality);
    result.configDiff = diff_json(project.config, result.config);
    result.reasons = std::move(reasons);
    result.warnings = Json::array();
    return result;
}

bool valid_trial(const Json& trial)
{
    if (!trial.is_object() || trial.value("validated", false) != true)
        return false;
    if (!trial.contains("candidateId") || !trial["candidateId"].is_string())
        return false;
    if (!trial.contains("metrics") || !trial["metrics"].is_object())
        return false;
    const auto& metrics = trial["metrics"];
    if (!metrics.contains("printTimeSeconds") || !metrics.contains("filamentGrams"))
        return false;
    if (!metrics["printTimeSeconds"].is_number() || !metrics["filamentGrams"].is_number())
        return false;
    const double time = metrics["printTimeSeconds"].get<double>();
    const double grams = metrics["filamentGrams"].get<double>();
    return std::isfinite(time) && std::isfinite(grams) && time >= 0.0 && grams >= 0.0;
}

double metric(const Json& trial, const char* key)
{
    return trial.at("metrics").at(key).get<double>();
}

} // namespace

std::vector<RecommendationCandidate> recommendation_candidates(const LoadedProject& project)
{
    std::vector<RecommendationCandidate> result;
    result.reserve(4);
    result.push_back(make_candidate(project, "baseline", "standard",
                                    Json::array({"Current native project configuration is the baseline."})));
    result.push_back(make_candidate(project, "balanced", "standard",
                                    Json::array({"Uses the native standard 0.4 mm layer height of 0.20 mm.",
                                                 "All other machine, material, paint and model settings remain native."})));
    result.push_back(make_candidate(project, "speed", "fast",
                                    Json::array({"Uses the native fast 0.4 mm layer height of 0.28 mm.",
                                                 "The coordinator must validate the real Print result before presenting metrics."})));
    result.push_back(make_candidate(project, "quality", "fine",
                                    Json::array({"Uses the native fine 0.4 mm layer height of 0.12 mm.",
                                                 "The coordinator must validate the real Print result before presenting metrics."})));
    return result;
}

Json describe_candidate(const RecommendationCandidate& candidate)
{
    return Json{{"candidateId", candidate.candidateId}, {"quality", candidate.quality},
                {"config", config_json(candidate.config)}, {"configDiff", candidate.configDiff},
                {"reasons", candidate.reasons}, {"warnings", candidate.warnings},
                {"validated", false}};
}

Json select_recommendation(const Json& completedTrials, const std::string& preference)
{
    if (!completedTrials.is_array())
        throw std::invalid_argument("completedTrials must be an array");
    if (preference != "balanced" && preference != "speed" && preference != "quality")
        throw std::invalid_argument("preference must be balanced, speed or quality");

    std::vector<const Json*> valid;
    for (const auto& trial : completedTrials)
        if (valid_trial(trial))
            valid.push_back(&trial);
    if (valid.empty())
        throw std::runtime_error("no validated recommendation trial is available");

    auto tie_key = [](const Json* trial) { return trial->at("candidateId").get<std::string>(); };
    const Json* selected = valid.front();
    if (preference == "speed") {
        for (const Json* trial : valid) {
            if (std::make_tuple(metric(trial, "printTimeSeconds"), metric(trial, "filamentGrams"), tie_key(trial)) <
                std::make_tuple(metric(selected, "printTimeSeconds"), metric(selected, "filamentGrams"), tie_key(selected)))
                selected = trial;
        }
    } else if (preference == "quality") {
        auto quality_rank = [](const Json* trial) {
            const std::string quality = trial->value("quality", std::string());
            return quality == "quality" || quality == "fine" ? 0 : quality == "balanced" || quality == "standard" ? 1 : 2;
        };
        for (const Json* trial : valid) {
            if (std::make_tuple(quality_rank(trial), metric(trial, "printTimeSeconds"), tie_key(trial)) <
                std::make_tuple(quality_rank(selected), metric(selected, "printTimeSeconds"), tie_key(selected)))
                selected = trial;
        }
    } else {
        double min_time = std::numeric_limits<double>::max();
        double max_time = 0.0;
        double min_grams = std::numeric_limits<double>::max();
        double max_grams = 0.0;
        for (const Json* trial : valid) {
            min_time = std::min(min_time, metric(trial, "printTimeSeconds"));
            max_time = std::max(max_time, metric(trial, "printTimeSeconds"));
            min_grams = std::min(min_grams, metric(trial, "filamentGrams"));
            max_grams = std::max(max_grams, metric(trial, "filamentGrams"));
        }
        const double time_span = max_time - min_time;
        const double grams_span = max_grams - min_grams;
        auto score = [&](const Json* trial) {
            const double time_score = time_span > 0.0 ? (metric(trial, "printTimeSeconds") - min_time) / time_span : 0.0;
            const double grams_score = grams_span > 0.0 ? (metric(trial, "filamentGrams") - min_grams) / grams_span : 0.0;
            return 0.65 * time_score + 0.35 * grams_score;
        };
        for (const Json* trial : valid) {
            if (std::make_tuple(score(trial), tie_key(trial)) < std::make_tuple(score(selected), tie_key(selected)))
                selected = trial;
        }
    }

    return Json{{"selectedCandidateId", selected->at("candidateId")},
                {"preference", preference},
                {"reasons", Json::array({"Selected from validated native Print trial metrics.",
                                          "No proxy estimate was used for time or material."})},
                {"warnings", Json::array()}};
}

} // namespace orca::slicing
