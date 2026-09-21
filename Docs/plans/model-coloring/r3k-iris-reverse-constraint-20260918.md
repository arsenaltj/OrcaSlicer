# R3k: iris source and geometry reverse constraints

Date: 2026-09-18
Branch: `codex/exp/mobile-sam-local-validation`

## Finding

The previous portrait-card policy selected the blue iris role from negative
Oklab channels alone. Dark gray and black irises can have slightly negative
channels after the source image is converted to Oklab, so the role binding
changed a neutral source material into the blue filament.

## Change

- A portrait role is treated as a proposal, not an unconditional override.
- The blue iris role requires source chroma of at least `0.04`, positive hue
  agreement with the blue role anchor, and a bounded source-to-anchor distance.
- Neutral or brown irises fall through to the ordinary source-material matcher.
- Saturated blue irises retain the portrait-card blue role when it is available.
- The existing geometry path remains authoritative: face IDs, depth and
  barycentric evidence are used before material mapping; no new mesh topology
  is guessed by the color policy.

## Verification

- Added regression for neutral dark iris with a complete portrait card.
- Added regression for saturated blue iris with the same card.
- Existing eye-white, brown-iris, boundary and manual-override tests remain
  part of the same suite.

## Scope

This round changes material selection only. It does not alter the MobileSAM
ROI, prompt generation, subface budget, or physical filament model. A real
model run is required before enabling the change as a visual acceptance
candidate.

## Run note

The source and regression build passed. The full four-model diagnostic render
was started with the fixed local runtime, but the high-face-count OBJ batch
reached approximately 1.7 GB working memory during material discovery and was
stopped before producing a complete new contact sheet. That incomplete run is
not visual acceptance evidence; the existing fixed R3j images remain the
comparison baseline until the models are rerun one at a time.
