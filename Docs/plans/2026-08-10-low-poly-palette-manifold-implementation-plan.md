# Printable Low-Poly Color Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Generate a single-volume, low-poly, color-constrained OBJ that imports into OrcaSlicer without UV-seam non-manifold warnings or loss of printable colors.

**Architecture:** OrcaSlicer supplies the current project filament colors to the local sidecar. The sidecar constrains and deterministically quantizes the AI preview, sends low-poly options to Tripo, then bakes the OBJ texture onto the original shared vertex topology and validates face count, connected components, manifold edges, and palette membership before import.

**Tech Stack:** C++17/wxWidgets, nlohmann/json, Python 3 standard library, Pillow, Tripo v3 API, unittest.

---

### Task 1: Tripo low-poly request contract

**Files:**
- Modify: `tools/ai/tripo_client.py`
- Test: `tools/ai/test_obj_generation.py`

1. Add failing tests that capture text-to-model and image-to-model JSON bodies.
2. Assert `smart_low_poly=true`, `face_limit=20000`, textured standard output, `pbr=false`, `quad=false`, and `export_uv=true`.
3. Assert image-to-model additionally sends `texture_alignment=original_image`.
4. Implement one shared payload builder and rerun focused tests.

### Task 2: Filament palette transport

**Files:**
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/ai_sidecar_mock.py`
- Test: `tools/ai/test_sidecar_contract.py`

1. Read colors through `Plater::get_extruder_colors_from_plater_config()`.
2. Normalize valid `#RRGGBB` values, deduplicate them while preserving slot order, and display color swatches in the generation panel.
3. Send the palette in text JSON, image multipart, and generation confirmation requests.
4. Validate one to sixteen palette colors in the sidecar and persist them on the job.
5. Reject palette changes between preview confirmation and 3D generation.
6. Keep the mock sidecar contract aligned and run sidecar contract tests.

### Task 3: Exact preview color constraint

**Files:**
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/orca_ai_sidecar.py`
- Test: `tools/ai/test_openai_preprocessor.py`
- Test: `tools/ai/test_obj_generation.py`

1. Add palette-aware prompt tests requiring solid printable regions and only supplied HEX colors.
2. Add deterministic Pillow quantization tests with dithering disabled.
3. Extend `preprocess_image()` to accept a palette and include it in the image-edit prompt.
4. Quantize the downloaded/generated preview in place before marking it ready.
5. Verify every preview pixel belongs to the palette.

### Task 4: Manifold-preserving OBJ color bake

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`
- Test: `tools/ai/test_obj_generation.py`

1. Add a fixture where one source vertex has multiple UV coordinates across a texture seam.
2. Assert the output keeps one vertex per original OBJ vertex instead of duplicating by `(material, vertex, uv)`.
3. Choose a deterministic palette color per source vertex from its UV samples, using majority count and palette order for ties.
4. Reuse original face vertex indices so UV seams do not become artificial open edges.
5. Add topology validation for triangular face limit, exactly one connected face component, and exactly two uses per undirected edge.
6. Reject invalid topology before exposing the artifact to OrcaSlicer, with an actionable regeneration message.

### Task 5: Full verification

**Files:**
- Update: `task_plan.md`
- Update: `findings.md`
- Update: `progress.md`

1. Run all `tools/ai` unittests and Python bytecode compilation.
2. Run `git diff --check`.
3. Build Windows Release `libslic3r_gui`, then full `OrcaSlicer` if the focused build passes.
4. Use an existing OBJ resource package to verify one connected manifold component, at most 20,000 faces for new low-poly fixtures, and exact palette membership without a paid Tripo call.
5. Run one authorized real image preview to inspect the printable palette result.
6. Ask the user before starting any paid Tripo 3D generation for final end-to-end acceptance.
