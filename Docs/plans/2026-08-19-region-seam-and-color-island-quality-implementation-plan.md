# Region Seam and Color-Island Quality Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Make local recolor selection cross duplicated OBJ seam vertices safely, then warn about tiny disconnected printable-color regions without changing Orca slicing behavior.

**Architecture:** `VertexColorRegionEditor` will augment indexed edge adjacency with constrained geometric boundary-edge adjacency while retaining the existing color and normal growth gates. `printable_model_quality.py` will derive deterministic face-color regions from optional OBJ vertex colors and add warning-only metrics to the existing report contract.

**Tech Stack:** C++17, Catch2, Python 3, unittest/pytest-compatible tests, wxWidgets integration build, CMake/MSBuild

---

### Task 1: Add seam-selection regression fixtures

**Files:**
- Modify: `tests/slic3rutils/test_vertex_color_region_editor.cpp`

1. Add a two-triangle mesh whose shared geometric edge uses duplicated vertex indices.
2. Add a failing test requiring `Replace` selection to cross the coincident seam.
3. Add negative tests for a positional gap, incompatible color, and sharp normal boundary.
4. Build or run `slic3rutils_tests`; record the expected pre-implementation failure.

### Task 2: Add constrained geometric boundary-edge adjacency

**Files:**
- Modify: `src/slic3r/GUI/AI/Model/VertexColorRegionEditor.cpp`
- Modify only if required: `src/slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp`

1. Preserve the existing indexed-edge adjacency path.
2. Identify boundary half-edges and key their endpoints using a mesh-relative position tolerance.
3. Join faces only when both edge endpoints match in either direction.
4. Deduplicate neighbor entries and keep color/normal checks in `smart_region` authoritative.
5. Run the new and existing selection tests.

### Task 3: Verify and publish phase 59

**Files:**
- Create: `Docs/architecture/2026-08-20-phase59-seam-resilient-region-review.md`

1. Run `git diff --check`.
2. Run the targeted C++ test target when available.
3. Build Release `libslic3r_gui` and `OrcaSlicer`.
4. Inspect the diff for Orca-core or format changes.
5. Commit only phase 59 files with `feat(ai): select local regions across obj seams`.

### Task 4: Add color-island quality regression fixtures

**Files:**
- Modify: `tools/ai/test_printable_model_quality.py`

1. Add a printable closed colored mesh with broad coherent color regions.
2. Add a colored mesh containing one tiny disconnected color region.
3. Assert old uncolored reports remain compatible.
4. Run the focused tests and confirm the tiny-island expectation fails before implementation.

### Task 5: Add warning-only printable color-region metrics

**Files:**
- Modify: `tools/ai/printable_model_quality.py`
- Modify if report transport requires it: `tools/ai/orca_ai_sidecar.py`

1. Parse optional `v x y z r g b` colors without weakening coordinate validation.
2. Assign a deterministic dominant color to each face.
3. Traverse edge-connected faces of the same color and calculate face and surface-area ratios.
4. Add threshold fields and stable metrics for color-region counts and tiny islands.
5. Emit a warning that yields `review`; never introduce a new hard rejection.
6. Preserve reports for OBJ files without complete vertex colors.

### Task 6: Verify and publish phase 60

**Files:**
- Create: `Docs/architecture/2026-08-20-phase60-color-island-quality-review.md`

1. Run focused model-quality tests.
2. Run all `tools/ai` tests and Python syntax checks.
3. Re-run representative historical OBJ reports offline.
4. Build Release targets if C++ or GUI files changed after phase 59.
5. Run `git diff --check` and compatibility review.
6. Commit only phase 60 files with `feat(ai): flag tiny printable color islands`.

### Task 7: Final audit

1. Confirm the root planning files are unchanged.
2. Confirm no Orca core, 3MF, profile, or smart-slicing files changed.
3. Confirm the branch contains two reviewable commits after phase 58.
4. Record exact test counts, build results, limitations, and remaining risks.
