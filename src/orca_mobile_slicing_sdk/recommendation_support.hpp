#ifndef ORCA_SLICING_RECOMMENDATION_SUPPORT_HPP
#define ORCA_SLICING_RECOMMENDATION_SUPPORT_HPP

#include <vector>
#include "project_support.hpp"

namespace orca::mobile::slicing {

struct RecommendationCandidate {
    std::string candidateId;
    std::string quality;
    Slic3r::DynamicPrintConfig config;
    Json configDiff = Json::object();
    Json reasons = Json::array();
    Json warnings = Json::array();
};

// Deterministic baseline + at most balanced/speed/quality plans, for SERIAL native
// Print trials by the caller. Planning never fabricates time, mass or validation.
std::vector<RecommendationCandidate> recommendation_candidates(const LoadedProject& project);
Json describe_candidate(const RecommendationCandidate& candidate);

// completedTrials is an array of CONTRACT-v1 candidate snapshots from real Print
// executions. Requires validated=true, finite positive time/mass, and artifact
// paths. Returns {selectedCandidateId, reasons, warnings}; throws if none qualify.
// Optional preference: balanced (default), speed, quality.
Json select_recommendation(const Json& completedTrials,
                           const std::string& preference = "balanced");

} // namespace orca::mobile::slicing
#endif
