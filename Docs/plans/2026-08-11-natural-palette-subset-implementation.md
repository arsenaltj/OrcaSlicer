# Natural Printable Palette Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Keep every preview pixel inside the configured filament palette while selecting only colors that naturally match the generated artwork, avoiding both two-color collapse and forced rainbow accents.

**Architecture:** Treat the filament palette as an allowed set, not a checklist. Quantize the AI preview into perceptual source clusters, map each cluster independently to the nearest CIE Lab filament color, preserve the connected background and face skin region, then validate exact palette membership plus a minimum meaningful color count. Do not inject missing colors into the base.

**Tech Stack:** Python 3.12, Pillow indexed-color quantization, `unittest`, existing Orca AI sidecar pipeline.

---

### Task 1: Define natural palette behavior with tests

**Files:**
- Modify: `tools/ai/test_obj_generation.py`

**Step 1: Replace the forced-all-colors base test**

Create a synthetic subject with neutral background, skin, dark hair, white clothing, green accent, and an integrated base. Assert that output colors are a subset of the configured palette, at least four meaningful colors remain, unrelated red/yellow/blue colors are not painted onto the base, and the background stays uniform.

**Step 2: Run the focused test and verify failure**

Run: `python -m unittest tools.ai.test_obj_generation.PrintablePaletteTests`

Expected: FAIL because the current implementation forces every selected color onto the base.

### Task 2: Remove forced palette completion

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`

**Step 1: Remove artificial missing-color placement**

Delete the lower-base segmentation that assigns all absent palette indices. Preserve background and face handling, then apply the existing indexed mode filter.

**Step 2: Return only actual color usage**

Validate that every output RGB belongs to the configured palette. Return a dictionary in configured slot order for colors that actually occur; do not fail when a valid palette color is unused.

**Step 3: Add a diversity gate**

For palettes with four or more colors, require at least three output colors including the background; this catches the original two-color collapse without manufacturing accents. Smaller palettes may use all naturally represented colors.

**Step 4: Run focused tests**

Run: `python -m unittest tools.ai.test_obj_generation.PrintablePaletteTests`

Expected: all palette tests pass.

### Task 3: Correct the image-generation prompt

**Files:**
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/test_openai_preprocessor.py`

**Step 1: Add a failing prompt assertion**

Assert that the prompt describes colors as an allowed palette, asks for a coherent style-appropriate subset, and does not require every color to appear.

**Step 2: Update the prompt**

Replace “Give every listed color” with guidance to use a small coherent subset selected by semantic role, reserve high-saturation accents for appropriate styles, and preserve broad contiguous printable regions.

**Step 3: Run prompt tests**

Run: `python -m unittest tools.ai.test_openai_preprocessor`

Expected: all prompt tests pass.

### Task 4: Regenerate and inspect three offline previews

**Files:**
- Create: `generated_models/style-validation-20260811/q-cartoon-natural-v1.png`
- Create: `generated_models/style-validation-20260811/cyberpunk-natural-v1.png`
- Create: `generated_models/style-validation-20260811/classical-natural-v1.png`
- Create: matching `*-colors.json` usage reports

**Step 1: Re-run only deterministic local quantization**

Use the existing `q-cartoon-raw.png`, `cyberpunk-raw.png`, and `classical-raw.png`; do not call paid image or 3D APIs.

**Step 2: Verify exact palette membership and diversity**

Assert zero outside-palette pixels, at least three colors per image, one connected uniform background, and no forced stripe containing all unused colors.

**Step 3: Visually inspect all three images**

Confirm identity, silhouette, style differentiation, skin color, clothing/armor readability, base continuity, and absence of rainbow striping or small color speckles.

### Task 5: Full regression and runtime recovery

**Files:**
- Modify: `task_plan.md`
- Modify: `findings.md`
- Modify: `progress.md`

**Step 1: Run full Python verification**

Run: `python -m unittest discover -s tools/ai -p "test_*.py"`

Expected: 55 or more tests pass without failures.

**Step 2: Run syntax and whitespace checks**

Run: `python -m py_compile tools/ai/orca_ai_sidecar.py tools/ai/openai_preprocessor.py`

Run: `git diff --check`

Expected: no errors.

**Step 3: Restart production sidecar and inspect health**

Start with `OPENAI_BASE_URL=https://laotie.dev`, verify protocol v1 and text/image/OBJ capability. Do not perform a paid request.

**Step 4: Update planning records**

Record the visual correction, test results, artifact paths, real 3D budget (`19/20`), and any remaining printer-profile warning.
