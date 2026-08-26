# Phase 79: Color-boundary regularization implementation plan

## Step 1: Add failing boundary-quality tests

- Output: focused tests in `tools/ai/test_obj_generation.py` covering safe recoloring and protected boundaries.
- Test: the new tests fail because the regularizer does not yet exist.

## Step 2: Implement bounded OBJ boundary regularization

- Output: Sidecar constants, deterministic cleanup function, atomic OBJ rewrite, and `color-boundary-cleanup.json` report.
- Test: focused OBJ-generation tests pass.

## Step 3: Integrate the pass into artifact preparation

- Output: palette-constrained artifacts run tiny-island cleanup, boundary regularization, metrics, validation, and structural quality analysis in that order. Natural-color artifacts remain unchanged.
- Test: existing palette and natural-color preparation tests pass; the full Python suite passes.

## Step 4: Reprocess and compare the accepted real artifact

- Output: a no-paid-call Stage 79 prototype and comparison report under the ignored generated-model directory.
- Test: geometry/topology and four-color coverage remain valid; mixed-face area/count improve without crossing recolor budgets.

## Step 5: Build and GUI-verify

- Output: Release build result and local Orca import evidence.
- Test: repository-local Orca detects four colors, opens the filament mapping flow, and displays the reprocessed model without visible corruption.

## Step 6: Review, document, and commit

- Output: Phase 79 architecture review, clean worktree, and append-only commits on `codex/model-generation`.
- Test: final status is clean and all verification evidence is recorded.
