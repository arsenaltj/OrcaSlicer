# Six-Channel Model Generation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver a model-generation workflow that supports one through six physical color channels, expresses discrete or process-mixed color intent, and imports into Orca without starting a slice.

**Architecture:** Keep OBJ as the geometry/region carrier, add a versioned color-intent manifest, and introduce typed neutral contracts for physical channels and mixed recipes. Model generation owns appearance intent; `OrcaWorkspaceAdapter` reuses Orca's native color decomposition and imports only into Prepare, while SmartSlicing remains independent.

**Tech Stack:** C++17, wxWidgets, OrcaSlicer/libslic3r, CMake, Catch2, Python 3 unittest, JSON, Windows multi-config build.

---

## Task 1: Record the accepted architecture

**Files:**
- Create: `docs/plans/2026-09-03-six-channel-model-generation-design.md`
- Create: `docs/architecture/ADR-006-six-channel-model-color-intent.md`
- Create: `docs/plans/2026-09-03-six-channel-model-generation-implementation-plan.md`
- Modify: `task_plan.md`
- Modify: `findings.md`
- Modify: `progress.md`

**Steps:**

1. Record requirements, alternatives, dependency direction, failure modes, and the 1–6 verification matrix.
2. Run `python -m unittest tools.ai.test_integration_guardrails -v`.
3. Expect all architecture guardrail tests to pass.
4. Review exact staged paths and commit only the planning files with `docs(model-generation): define six-channel color intent architecture`.

## Task 2: Make the no-auto-slice contract fail first

**Files:**
- Modify: `tests/slic3rutils/test_ai_contracts.cpp`
- Modify: `tools/ai/test_integration_guardrails.py`

**Steps:**

1. Change the recording consumer so it returns import/color results only.
2. Remove expectations for `auto_slice_after_import` and `slice_after_import`.
3. Add a source-boundary assertion rejecting these identifiers and `EVT_GLTOOLBAR_SLICE_PLATE` from the model-generation import path.
4. Run `python -m unittest tools.ai.test_integration_guardrails -v`; expect the new source-boundary assertion to fail against current code.
5. Build `cmake --build build --config Release --target slic3rutils_tests -- -m`; expect compilation to fail until the public structs are updated.

## Task 3: Remove automatic slicing from the public import contract

**Files:**
- Modify: `src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp`
- Verify forwarding header: `src/slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp`
- Modify: `tests/slic3rutils/test_ai_contracts.cpp`

**Steps:**

1. Delete `ModelImportRequest::auto_slice_after_import`.
2. Delete `ModelImportResult::slice_after_import`.
3. Keep `ImportColorMode`, outcomes, coloring/repair diagnostics, and forwarding include behavior unchanged.
4. Build `slic3rutils_tests`; expect callers in the GUI to identify every remaining dependency at compile time.

## Task 4: Remove automatic slicing from GUI composition and import behavior

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`
- Modify: `src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp`
- Modify: `src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.cpp`
- Modify: `src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp`
- Modify: `src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp`
- Modify: `src/slic3r/GUI/MainFrame.cpp`

**Steps:**

1. Remove the checkbox, event binding, member, request assignment, conditional labels, and `slice_requested` journey event.
2. Rename the section to “导入设置”; all successful buttons and messages say the model will enter Prepare.
3. Narrow `NavigateAfterImportFn` and `ImportSucceededFn` from `std::function<void(bool)>` to `std::function<void()>`.
4. In `MainFrame`, keep `exit_gizmo()`, `update(true, true)`, and `select_tab(TAB_ID_PREPARE)`; remove the slice event and Preview navigation from this callback.
5. Delete the adapter's automatic-slice gate and its writes to `independent_support_layer_height` and `enable_prime_tower`.
6. Keep mesh validation/repair, color import, placement, undo, and manual-follow-up diagnostics.
7. Make Sidebar slice/G-code steps Waiting after every successful import; do not report this manual handoff as a slicing failure.
8. Run `rg -n "auto_slice_after_import|slice_after_import|导入后自动切片" src tests`; expect no matches.

## Task 5: Verify and checkpoint the import-only milestone

**Files:**
- Modify: `progress.md`
- Modify: `task_plan.md`

**Steps:**

1. Run `python -m unittest tools.ai.test_integration_guardrails -v` and expect PASS.
2. Build `cmake --build build --config Release --target slic3rutils_tests -- -m` and expect PASS.
3. Run the AI contract Catch2 case from the built `slic3rutils_tests` executable and expect PASS.
4. Run `python scripts/verify_ai_integration.py` and expect no new boundary or budget issue.
5. Inspect `git diff --check` and the exact changed-file list.
6. Commit the milestone as `refactor(model-generation): stop after workspace import`.

## Task 6: Introduce typed 1–6 color capability contracts

**Files:**
- Create: `src/slic3r/AI/Contracts/ColorIntent.hpp`
- Modify: `src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp`
- Modify: `src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp`
- Modify: `tests/slic3rutils/test_ai_contracts.cpp`
- Modify: `src/slic3r/AI/CMakeLists.txt` only if the new public header must be listed explicitly.

**Steps:**

1. Add failing tests for output modes, physical channels, valid one-to-three-component recipes, and channel counts 1–6.
2. Add `ColorOutputMode`, `PhysicalFilamentChannel`, `MixedColorComponent`, `MixedColorRecipe`, and `ColorIntentManifestRef` as wx/provider-free value types.
3. Add typed capabilities to `PrintablePaletteSnapshot`; temporarily derive old flat vectors from the same data.
4. Add optional manifest path/schema metadata to `GeneratedModelArtifact` without changing old default construction.
5. Build and run `[AIContracts]`; expect PASS.

## Task 7: Populate typed capabilities from Orca

**Files:**
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp`
- Test: add a wx-free extraction/helper test under `tests/slic3rutils/` or `tests/libslic3r/`.

**Steps:**

1. Extract physical-vs-mixed filament classification into a deterministic helper.
2. Read `filament_is_mixed`, `filament_mixed_components`, and `filament_mixed_sublayer_ratios` from Orca configuration.
3. Remove the four-slot truncation and cap active physical channels at six only at the model-generation capability boundary.
4. Populate virtual recipes separately; never count virtual mixed slots as physical channels.
5. Preserve the old flat projection for legacy callers and test projection consistency.

## Task 8: Make Python palette policy cardinality-dynamic

**Files:**
- Modify: `tools/ai/printable_palette.py`
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: palette/style/portrait helpers discovered by the focused four-color inventory.
- Modify: `tools/ai/test_printable_palette.py`
- Modify: related sidecar and visual-quality tests.

**Steps:**

1. Add parameterized failing tests for channel counts 1–6 and legacy four-color payloads.
2. Replace `MAX_PRINTABLE_COLORS = 4` with explicit minimum/maximum capability policy and request-driven cardinality.
3. Replace exactly-four validation and four fixed semantic roles with deterministic dynamic role assignment.
4. Preserve old payload field names and defaults while accepting typed capability fields.
5. Run focused Python suites; expect all six cardinalities and legacy fixtures to pass.

## Task 9: Make the wxWidgets palette UI support 1–6

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: presentation helpers/tests if fixed-four assumptions are found.

**Steps:**

1. Add logic-level tests for cardinality and role projection outside wxWidgets where practical.
2. Replace four-element widget arrays with six-capacity/dynamic collections owned by the panel.
3. Generate recommendation cards, swatches, replacement/removal actions, and role selectors from active count.
4. Change every user-facing constraint to 1–6 and keep layouts usable at 100%, 125%, and 150% scaling.
5. Verify counts 1, 4, 5, and 6 in the real GUI; capture screenshots before checkpointing.

## Task 10: Produce and validate `color-intent.v1.json`

**Files:**
- Create: `tools/ai/color_intent.py`
- Create: `tools/ai/test_color_intent.py`
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

**Steps:**

1. Define schema fixtures for discrete and process-mix artifacts, including invalid version/channel/ratio cases.
2. Write validation tests before serialization code.
3. Write the manifest atomically beside `model-vertex-color.obj` only after artifact validation succeeds.
4. Return manifest URL/hash/schema through the existing job result without breaking older sidecars or clients.
5. Download and attach the manifest reference to `GeneratedModelArtifact`; artifacts without it retain legacy behavior.

## Task 11: Translate process-mix intent through Orca

**Files:**
- Create: a wx-free color-intent-to-Orca helper under `src/slic3r/AI/ModelGeneration/` or `src/libslic3r/` after dependency review.
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`
- Reuse: `src/libslic3r/ColorDecomposeRecipe.hpp/.cpp`
- Test: focused recipe and adapter tests under `tests/libslic3r/` and/or `tests/slic3rutils/`.

**Steps:**

1. Add failing tests for six physical candidates producing valid one-to-three-component recipes.
2. Validate material compatibility and normalized ratios before workspace mutation.
3. Reuse an equivalent virtual mixed slot when present; otherwise create a deterministic mixed-slot configuration through Orca APIs.
4. Map OBJ target colors to the selected physical/virtual slots using one import transaction.
5. Test clear failure for insufficient compatible channels and unknown manifest versions.
6. Verify mixed slot and painted facet IDs survive a 3MF save/load round trip.

## Task 12: Full regression and delivery

**Files:**
- Modify: `task_plan.md`
- Modify: `findings.md`
- Modify: `progress.md`
- Create: milestone review documents under `docs/architecture/` as needed.

**Steps:**

1. Run all focused Python and native suites.
2. Run the project integration verifier and architecture budgets.
3. Build the full Windows Release target with the established command.
4. Exercise text/image generation, old four-color artifact import, new six-channel discrete import, and six-channel process-mix import.
5. Confirm no model-generation code starts a slice, changes print presets, or imports SmartSlicing implementation headers.
6. Record exact source SHA, tests, GUI evidence, shared files, and compatibility impact.
7. Commit reviewed milestones; do not merge to `codex/orca-integration-v2` or push without explicit user approval.

