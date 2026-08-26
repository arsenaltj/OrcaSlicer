# Semantic Material Region Selection Implementation Plan

**Goal:** Add deterministic one-click semantic material selection to the existing local recolor editor while keeping Provider semantics, mesh selection, and Orca workspace integration decoupled.

**Architecture:** Reuse the palette-role contract already persisted by model generation. `VertexColorRegionEditor` groups final OBJ faces by the nearest color in the loaded model palette and only updates its existing selection bitmap. `ModelPreview3D` owns selection history/render refresh, while `ModelGenerationPanel` supplies role labels and keeps source-material colors separate from target printer-filament colors.

**Tech Stack:** C++17, wxWidgets, existing `VertexColorRegionEditor`, Catch2, Windows CMake Release build.

## Task 1: Add deterministic material selection

Files:

- Modify `src/slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp`
- Modify `src/slic3r/GUI/AI/Model/VertexColorRegionEditor.cpp`
- Modify `tests/slic3rutils/test_vertex_color_region_editor.cpp`

Steps:

1. Add failing tests for disconnected same-material faces, nearest palette assignment, deterministic equal-distance ties, invalid inputs and snapshot restoration.
2. Add a replace-selection method accepting an ordered RGBA palette and target index.
3. Reuse `face_color`; do not add connectivity, Sidecar or GUI dependencies.
4. Run the focused `[AI][VertexColorRegion]` suite.

## Task 2: Expose selection through the 3D preview

Files:

- Modify `src/slic3r/GUI/ModelGenerationPanel.cpp`

Steps:

1. Preserve the decoded model palette used by the current preview.
2. Add a preview method that snapshots the current selection, invokes deterministic material selection, rebuilds the highlight mesh and notifies the existing callback.
3. Do not push an undo entry when the selection is unchanged or the request is invalid.

## Task 3: Add semantic material controls and history recovery

Files:

- Modify `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify `src/slic3r/GUI/ModelGenerationPanel.cpp`

Steps:

1. Add at most four source-material buttons above the manual selection row.
2. Label them from `palette_roles` when valid, otherwise use generic material labels.
3. Read optional `palette_roles` from existing model-library metadata and restore them when a historical model is loaded.
4. Keep source palette independent from `local_recolor_palette()`, which remains the target physical-filament provider boundary.
5. Preserve palette roles when writing a local recolor derivative whose palette ordering is unchanged.

## Task 4: Verify and document

Files:

- Create `Docs/architecture/2026-08-23-phase65-semantic-material-region-review.md`

Steps:

1. Run the focused Catch2 suite and relevant offline AI tests.
2. Build the repository-local Windows Release `OrcaSlicer` target.
3. Launch only the repository-local executable and verify the semantic selection journey on a repository-local colored model.
4. Run `git diff --check`, confirm no shared Orca/format/profile files changed, and commit all stage 65 changes.
