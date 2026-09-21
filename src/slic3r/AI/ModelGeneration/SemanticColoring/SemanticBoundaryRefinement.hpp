#pragma once

#include "SemanticColoring.hpp"

namespace Slic3r::AI::SemanticColoring {

struct EarHairBoundaryResult {
    Prediction prediction;
    BoundaryRefinement refinement;
    // One byte per input pixel. Only the seed-connected foreground component
    // may change semantics; refinement retains the unthresholded probability.
    std::vector<uint8_t> accepted;
    struct ContourSegment {
        std::array<float, 2> a, b;
        Label foreground {Label::FaceSkin}, background {Label::Hair};
        float confidence {0.f};
    };
    std::vector<ContourSegment> contours;
    std::vector<BoundaryRunDiagnostic> diagnostics;
};

BoundaryRefinementRequest facial_boundary_request(const RGBImage&, const Prediction& body,
                                                  const Prediction& face);
EarHairBoundaryResult apply_facial_boundaries(const RGBImage&, const Prediction& body,
                                             const Prediction& face, IBoundaryRefiner&,
                                             const Cancel& = {});

// Builds conservative side-face prompts from the body and face providers.
// Returns an empty request when an exposed ear/adjacent hair pair is not proven.
BoundaryRefinementRequest ear_hair_boundary_request(const RGBImage&, const Prediction& body,
                                                    const Prediction& face);

EarHairBoundaryResult apply_ear_hair_boundary(const RGBImage&, const Prediction& body,
                                              const Prediction& face, IBoundaryRefiner&,
                                              const Cancel& = {});

// Applies only seed-connected, high-confidence FaceSkin recovery. Existing
// facial details and all pixels outside the requested ear regions are retained.
Prediction refine_ear_hair_boundary(const RGBImage&, const Prediction& body,
                                    const Prediction& face, IBoundaryRefiner&,
                                    const Cancel& = {});

} // namespace Slic3r::AI::SemanticColoring
