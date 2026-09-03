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

## Task 10: Make geometry-reference prompts continuous-tone

**Files:**
- Modify: `tools/ai/test_openai_preprocessor.py`
- Modify: `tools/ai/test_preprocess_fallback.py`
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/model_job_support.py`
- Modify: `tools/ai/orca_ai_sidecar.py`

**Steps:**

1. Add failing tests proving a non-empty 1–6-color palette is not copied into the geometry-image prompt:

```python
prompt = build_geometry_reference_prompt("same person", "realistic")
self.assertNotIn("#D96B43", prompt)
self.assertIn("continuous tonal modeling", prompt)
self.assertIn("diffuse", prompt)
```

2. Add tests proving cast shadows, specular highlights, dense texture, dithering, and tiny material speckles remain forbidden while broad natural material relationships and sculptural planes remain allowed.
3. Add explicit `build_geometry_reference_prompt()` and `build_text_geometry_reference_prompt()` boundaries. Keep the old palette-preview builders for compatibility tooling, but stop using them for the image submitted to 3D.
4. Change `preprocess_image()` and production text-image generation to issue one continuous-tone paid image request. Continue passing the selected palette only to `process_printable_image()`.
5. Change `generation_prompt()` to accept a `constrain_palette` flag. Pass `False` when an image geometry reference exists and preserve the old exact-palette behavior for text-only fallback generation.
6. Run `python -m unittest tools.ai.test_openai_preprocessor tools.ai.test_preprocess_fallback tools.ai.test_printable_sidecar_pipeline -v`; expect PASS.

## Task 11: Generalize identity-first geometry to 1–6 colors

**Files:**
- Modify: `tools/ai/test_obj_generation.py`
- Modify: `tools/ai/test_printable_sidecar_pipeline.py`
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/orca_ai_sidecar.py`

**Steps:**

1. Add a parameterized failing test for palette lengths 1 through 6. Each quality realistic image job with an independently detected portrait must select `geometry-reference.png` and suppress hallucinated generated multiview geometry.
2. Add negative tests for non-realistic style, performance profile, missing input, unsupported color count, and missing portrait evidence.
3. Persist palette-independent portrait evidence from the existing face-lock mask:

```python
job.image_metrics["portrait_geometry"] = {
    "detected": face_lock_path.is_file(),
    "evidence": "source_face_lock",
}
```

4. Replace the fixed-four identity gate with the supported 1–6 cardinality policy plus the new evidence, retaining `portrait_skin_cleanup.activated` as a legacy-job fallback.
5. Keep `_quality_portrait_multiview_enabled()` and skin/garment cleanup separately gated; do not infer material roles when the selected palette cannot express them.
6. Run the two focused suites and the 1–6 palette suites; expect PASS.

## Task 12: Produce and validate `color-intent.v1.json`

**Files:**
- Create: `tools/ai/color_intent.py`
- Create: `tools/ai/test_color_intent.py`
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/test_obj_generation.py`
- Modify: `tools/ai/test_sidecar_contract.py`

**Steps:**

1. Write failing tests for 1–6 palette records, duplicate/invalid colors, inactive or duplicate roles, invalid schema/hash, missing artifact, and atomic replacement.
2. Define the v1 shape:

```json
{
  "schema": "orcaslicer.color-intent.v1",
  "mode": "discrete_filament",
  "artifact": {"filename": "model-vertex-color.obj", "sha256": "...", "color_encoding": "vertex_colors"},
  "references": {"geometry": {"sha256": "..."}, "material_preview": {"sha256": "..."}},
  "targets": [
    {"role": "primary", "fallback_color": "#F4F4F0", "desired_color": "#E9DDD2", "sample_count": 1200}
  ]
}
```

3. Compute each `desired_color` deterministically from raw continuous-tone pixels belonging to the corresponding exact-preview region. Fall back to the selected color when a region has no samples.
4. Bind the manifest to the final OBJ SHA-256 and reference hashes; validate the complete payload before writing `color-intent.v1.json.part`, then atomically replace the destination.
5. Add `color_intent_path`, schema, and hash to job persistence. Generate the manifest after model validation/visual review and before publishing `ready`; do the same for retexture output.
6. Publish optional manifest status under `artifact.color_intent` and add a size-limited `color-intent` download route. Legacy jobs without it stay ready and downloadable.
7. Run `python -m unittest tools.ai.test_color_intent tools.ai.test_obj_generation tools.ai.test_sidecar_contract -v`; expect PASS.

## Task 13: Attach the manifest to the native artifact hand-off

**Files:**
- Modify: `src/slic3r/AI/Contracts/ColorIntent.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: `tests/slic3rutils/test_ai_contracts.cpp`
- Modify: `tools/ai/test_integration_guardrails.py`

**Steps:**

1. Extend contract tests for the exact schema identifier, a 64-character lowercase SHA-256, and optional legacy absence.
2. Parse `artifact.color_intent` into `JobStatus` and add a dedicated, loopback-only, size-limited download method that checks the response hash and schema before atomic rename.
3. Download the manifest before declaring a new model preview locally complete. Store its path/schema/hash beside the model and in generated-model library metadata; old library entries continue to load without it.
4. Populate `GeneratedModelArtifact::color_intent_manifest` only after the verified local file exists. Do not invoke `ColorDecomposeRecipe`, create virtual slots, or modify print configuration.
5. Keep `ModelGenerationPanel.cpp` at or below its current architecture budget by extracting pure validation/presentation code if needed rather than raising the budget.
6. Compile the contract/client/panel units with the established production PCH command and run the small Catch2 executables; expect PASS.

## Task 14: Clarify shape, material, and printable preview language

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: `src/slic3r/GUI/AI/ModelGeneration/ModelGenerationStatusText.cpp` if status text is shared there.
- Modify: focused presentation tests.

**Steps:**

1. Replace the inaccurate “single-color sculptural reference is generated simultaneously” claim with “continuous-tone shape reference; selected colors are reassigned after the mesh exists.”
2. Label image stages as “造型参考（决定形体）” and “打印配色草图（决定材质）”. Reserve “可打印预览” for the actual 3D mesh render.
3. Keep paid-call confirmation explicit: one image request during preview and one model request after confirmation.
4. Verify the four workflow states and high-DPI layout in the real GUI when the build environment is available.

## Task 15: Add print-native artistic presets after the core path is green

**Files:**
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/test_openai_preprocessor.py`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: style recommendation/validation fixtures discovered during implementation.

**Steps:**

1. Add failing prompt/contract tests for `portrait_sketch` and `ink_relief`.
2. Implement portrait sketch as identity-first geometry with restrained exaggeration and broad 2–5-level material masses; forbid generic caricature substitution.
3. Implement ink relief as printmaking/woodcut-inspired negative space, 2–4 broad values, and modelable embossed/engraved strokes; forbid translucent washes and micro-halftone dots.
4. Do not force every selected channel to appear when doing so creates tiny or semantically false regions.
5. Run prompt, style recommendation, exact-palette, and 1–6 cardinality tests.

## Task 16: Full regression and delivery

**Files:**
- Modify: `task_plan.md`
- Modify: `findings.md`
- Modify: `progress.md`
- Create: milestone review documents under `docs/architecture/` as needed.

**Steps:**

1. Run all focused Python and native suites.
2. Run the project integration verifier and architecture budgets.
3. Build the full Windows Release target with the established command.
4. Exercise text/image generation, old four-color artifact import, and new 1/4/5/6-color late-palette imports with and without a manifest.
5. Confirm geometry requests contain no selected filament hex values, exact previews and OBJ files contain no out-of-palette colors, and the manifest hash binds to the downloaded OBJ.
6. Confirm no model-generation code starts a slice, changes print presets, solves process-mix recipes, or imports SmartSlicing implementation headers.
7. Record exact source SHA, tests, GUI evidence, shared files, and compatibility impact.
8. Commit reviewed milestones; do not merge to `codex/orca-integration-v2` or push without explicit user approval.
