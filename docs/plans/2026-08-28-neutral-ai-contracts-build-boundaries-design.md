# Neutral AI Contracts and Build Boundaries Design

## Goal

Remove the remaining compile-time dependency from model generation to smart slicing, while preserving accepted behavior, source compatibility, Orca defaults, and project/profile formats.

## Current problem

The shared artifact import contract is physically owned by SmartSlicing, so ModelGeneration must include a sibling feature module. SmartSlicing core sources are also compiled directly into libslic3r_gui, which hides the dependency direction and makes feature integration harder to review and test independently.

## Considered approaches

### A. Move headers only

This fixes include paths but leaves SmartSlicing source ownership implicit inside the GUI target. It does not provide a build-level boundary, so regressions can return unnoticed.

### B. Split every AI GUI component now

This creates the strongest separation immediately, but it touches wxWidgets-heavy code, resources, generated translations, and shared GUI wiring in one change. The blast radius is too large for a safe upstream-sync checkpoint.

### C. Neutral contracts plus a SmartSlicing core target

Chosen. Put only the three cross-feature contracts in src/slic3r/AI/Contracts, expose them through an interface target, and compile the wx-free SmartSlicing core as its own static target. Keep GUI adapters and ModelGeneration UI in libslic3r_gui for now.

## Target dependency direction

    AI contracts
       ^       ^
       |       |
    ModelGeneration GUI     SmartSlicing core
       ^                         ^
        \                       /
         Orca GUI adapters and libslic3r_gui

The neutral contract layer must not include GUI, provider, ModelGeneration, or SmartSlicing implementation headers.

## Contract migration

The following definitions move without changing namespace, member order, defaults, or signatures:

- GeneratedModelArtifact
- IPrintablePaletteProvider and PrintablePaletteSnapshot
- IModelArtifactConsumer and its import request/result types

Existing include paths remain as forwarding headers so accepted feature commits and downstream code continue to compile. Integration-owned consumers switch to the neutral paths directly.

## Build boundaries

- orcaslicer_ai_contracts is an INTERFACE target with alias OrcaSlicer::AIContracts.
- orcaslicer_ai_smart_slicing is a STATIC target with alias OrcaSlicer::SmartSlicing.
- SmartSlicing application/domain source files move out of the libslic3r_gui source list and into the static target.
- libslic3r_gui links both targets explicitly.
- ModelGeneration remains in libslic3r_gui during this phase because its remaining implementation is GUI-heavy; an empty library would create a cosmetic boundary only.

## Compatibility and failure behavior

No 3MF/profile schema, default behavior, service endpoint, credential policy, or runtime feature flow changes. Forwarding headers preserve source compatibility. If the new targets are omitted or source ownership drifts, configuration or architecture checks must fail rather than silently compiling duplicate sources.

## Enforcement

Architecture guardrails validate neutral paths, forbidden dependency directions, exact forwarding headers, migration state, target declarations, target links, and exclusive SmartSlicing source ownership. A small native contract test ensures the moved public types remain consumable through both neutral and legacy paths.

## Verification

Run focused and full Python guardrail tests, configure the Windows build, build targeted native tests, run slic3rutils tests, perform the established Release build, and verify the worktree diff before committing.
