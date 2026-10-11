#pragma once
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"
#include <atomic>
#include <boost/filesystem/path.hpp>

namespace Slic3r::GUI {
struct ColorMatchingComputationRequest {
    LocalPrintColorMatching::Input input;
    LocalPrintColorRecipes::Input recipes;
    std::shared_ptr<const indexed_triangle_set> topology;
    boost::filesystem::path source, config, modules, temporary_parent, semantic_cache;
    bool recognize_details {false};
};
struct ColorMatchingComputationResult {
    LocalPrintColorMatching::Computation matching;
    std::shared_ptr<const LocalSemanticEvidence::Evidence> evidence;
    std::string notice, cleanup_notice;
};
// Runs on a caller-owned worker or synchronously, never posts a GUI callback.
ColorMatchingComputationResult compute_color_matching(ColorMatchingComputationRequest request, std::atomic<bool>& cancelled);
}
