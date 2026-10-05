#pragma once

#include "libslic3r/Model.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <string>

namespace Slic3r::GUI {

struct OrcaParameterApplyResult
{
    bool accepted{false};
    std::string diagnostic_code;
};

// Prepared on the GUI thread. Copying the frozen ModelConfig does not touch
// the native global timestamp counter from the trial worker.
struct OrcaObjectParameterPatch
{
    uint64_t object_id{0};
    ModelConfig original;
    ModelConfig replacement;
};

class OrcaParameterProposalAdapter
{
public:
    // Three nozzle-bounded process alternatives. The caller supplies only
    // current-plate objects without instances on another plate.
    std::vector<AI::SmartSlicing::SliceCandidate> priority_candidates(
        const AI::SmartSlicing::WorkspaceRevision& revision, int64_t plate_id,
        const DynamicPrintConfig& base_config, const std::vector<ModelObject*>& targets) const;

    OrcaParameterApplyResult validate_and_apply(const AI::SmartSlicing::ParameterProposal& proposal,
                                                int64_t expected_plate_id,
                                                const DynamicPrintConfig& base_config,
                                                DynamicPrintConfig& patched_config) const;

    OrcaParameterApplyResult prepare_object_patches(
        const AI::SmartSlicing::ParameterProposal& proposal, int64_t expected_plate_id,
        const DynamicPrintConfig& base_config, const std::vector<ModelObject*>& targets,
        std::vector<OrcaObjectParameterPatch>& patches) const;

    OrcaParameterApplyResult apply_object_patches(
        Model& model, const std::vector<OrcaObjectParameterPatch>& patches) const;
};

} // namespace Slic3r::GUI
