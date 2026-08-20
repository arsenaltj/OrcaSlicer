# AI Recommended Printable Palette Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a recoverable, explicitly invoked AI recommendation step that proposes four editable printable design colors before image preview and leaves physical filament matching to the user.

**Architecture:** Extend the existing model-job state machine so palette recommendation and image preprocessing share one persisted job. Keep provider parsing and validation in Python, expose only provider-neutral DTOs to C++, and reuse the existing custom palette editor plus a compact recommendation explanation panel.

**Tech Stack:** Python 3.12 `unittest`, loopback HTTP Sidecar, OpenAI-compatible chat/vision adapter, C++17, wxWidgets, nlohmann/json, Catch2, CMake/MSBuild.

---

### Task 1: Provider-neutral palette recommendation

**Files:**
- Modify: `tools/ai/openai_preprocessor.py`
- Modify: `tools/ai/test_openai_preprocessor.py`
- Modify: `tools/ai/printable_palette.py` only if an existing validation primitive cannot be reused

**Step 1: Write failing provider tests**

Add tests that mock `complete_text` and `complete_vision` and require `recommend_printable_palette()` to:

- accept text-only and optional image input;
- remove a surrounding JSON markdown fence;
- return four ordered records with `hex`, `name`, `role`, `usage`, and `reason`;
- reject malformed JSON, duplicate/invalid colors, missing/duplicate roles, excessive text, and a palette whose existing `assign_palette_roles()` result is low contrast.

**Step 2: Run the focused test**

Run: `python -m unittest tools.ai.test_openai_preprocessor -v`

Expected: new tests fail because `recommend_printable_palette` does not exist.

**Step 3: Implement the minimal provider adapter**

Add immutable recommendation record/result dataclasses and `recommend_printable_palette(instruction, style, custom_style="", image_path=None)`. Use `complete_text` for text-only input and `complete_vision` when an image is present. Require exactly one JSON object and exactly four unique roles. Normalize colors through `normalize_palette`, validate role membership and text byte limits, and reject `assign_palette_roles(colors).low_contrast` without retrying.

**Step 4: Run focused tests**

Run: `python -m unittest tools.ai.test_openai_preprocessor -v`

Expected: all provider tests pass.

### Task 2: Recoverable Sidecar task states and HTTP contract

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/test_sidecar_contract.py`
- Modify: `tools/ai/test_sidecar_mock.py`
- Modify: `tools/ai/test_sidecar_readiness.py`

**Step 1: Write failing state and contract tests**

Cover:

- `POST /v1/orcaslicer/model-jobs/recommend-text-palette` JSON input;
- `POST /v1/orcaslicer/model-jobs/recommend-image-palette` multipart input;
- public job fields `palette_recommendation` and `palette_recommendation_confirmed`;
- state transition `recommending_palette -> awaiting_palette_confirmation`;
- `POST /v1/orcaslicer/model-jobs/{id}/confirm-palette` with final 1–4 colors;
- confirmation continuing the same text/image preprocessing job;
- rejection outside the awaiting state and no implicit provider retry;
- job persistence/restore and old fixture compatibility;
- health capability discovery for palette recommendation.

**Step 2: Run focused Sidecar tests**

Run: `python -m unittest tools.ai.test_sidecar_contract tools.ai.test_sidecar_mock tools.ai.test_sidecar_readiness -v`

Expected: new tests fail on missing routes and fields.

**Step 3: Implement persisted job fields and workers**

Extend `Job` with a provider-neutral recommendation object and confirmation flag. Serialize/load them with safe defaults. Add a recommendation worker that updates progress, invokes `recommend_printable_palette`, persists the result, and ends in `awaiting_palette_confirmation`.

**Step 4: Implement routes and confirmation**

Add the two create routes and `confirm-palette` to `_job_route`. Reuse the existing request parsers and stored image file. Validate final palette/roles, update the job, mark the recommendation confirmed, and submit the existing `_preprocess_text_job` or `_preprocess_image_job`. Extend `latest`, DELETE/stop handling, and health capabilities for the new states.

**Step 5: Run focused Sidecar tests**

Run: `python -m unittest tools.ai.test_sidecar_contract tools.ai.test_sidecar_mock tools.ai.test_sidecar_readiness -v`

Expected: all focused Sidecar tests pass.

### Task 3: C++ client DTO and transport

**Files:**
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`
- Modify: `tests/slic3rutils/test_ai_model_generation_client.cpp` if a suitable client parser test target exists; otherwise add parsing coverage to the nearest AI GUI test seam

**Step 1: Add failing parsing/contract coverage**

Define expected parsing for recommendation summary, four color records and confirmation state, including backward-compatible responses with the fields absent.

**Step 2: Implement provider-neutral DTOs**

Add `PaletteRecommendationColor` and `PaletteRecommendation` to the client status contract. Parse bounded strings and valid `#RRGGBB` values defensively; an invalid optional recommendation must not crash recovery.

**Step 3: Implement transport methods**

Add `recommend_text_palette`, `recommend_image_palette`, and `confirm_palette`. Reuse existing loopback checks, JSON/multipart serialization, active-request cancellation and status callbacks.

**Step 4: Compile the touched C++ target**

Run the configured Release build for `libslic3r_gui`.

Expected: compilation and linking succeed.

### Task 4: GUI recommendation and editable confirmation flow

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

**Step 1: Add model/UI state helpers**

Add explicit recommendation states, input fingerprint/staleness tracking and helpers to apply a candidate palette without changing Orca filament slots. Preserve the existing project and manual palette modes.

**Step 2: Build the compact recommendation UI**

Add the “AI 推荐四色” source/action, confirmation button, status text and four compact explanatory rows/cards. Reuse the existing color picker and add/remove behavior for candidate editing. Mark a changed recommendation record as user-adjusted.

**Step 3: Wire requests and recovery**

Require explicit paid confirmation before recommendation. Dispatch text or image recommendation based on current input, poll the same job, restore `awaiting_palette_confirmation`, and call `confirm_palette` only after the user confirms. Disable image preview while a recommendation candidate is unconfirmed.

**Step 4: Handle stale and failure states**

When text/image/style changes, keep the palette but mark it stale and require reconfirmation. On request failure, retain the previous palette and expose retry/manual alternatives. Never auto-call the provider.

**Step 5: Compile the GUI and application**

Run Release builds for `libslic3r_gui` and `OrcaSlicer`.

Expected: both targets compile and link.

### Task 5: Documentation and full verification

**Files:**
- Create: `Docs/architecture/2026-08-20-phase61-ai-recommended-palette-review.md`
- Modify only model-generation tests/docs required by the implementation

**Step 1: Run Python regression**

Run: `python -m unittest discover -s tools/ai -p 'test_*.py'`

Expected: all AI tests pass with no real paid provider requests.

**Step 2: Run syntax and C++ focused tests**

Run Python compilation for touched files and the relevant Catch2 AI filters. Expected: all focused tests pass.

**Step 3: Run Release builds**

Build `libslic3r_gui` and full `OrcaSlicer` in Release. Expected: successful compile/link with only known repository warnings.

**Step 4: Perform isolated GUI validation**

Resolve the executable under `D:\Workspace\06_3DDY_claude`, enumerate running `OrcaSlicer.exe` processes with executable paths, and do not stop or control any process outside this workspace. Start only the local build. Validate text recommendation, image recommendation, edit/delete/add, confirm, stale input, restart recovery and provider-error fallback. Avoid a real provider call unless an already configured local/mock validation path covers it or explicit paid-call authorization exists.

**Step 5: Record evidence and inspect scope**

Write the architecture review with test/build/GUI evidence. Run `git diff --check`, inspect `git status`, and verify that root `task_plan.md`, `findings.md`, and `progress.md`, Orca core slicing, profiles and 3MF code are untouched.

**Step 6: Commit in reviewable units**

Commit provider/Sidecar changes, then C++/GUI changes and review documentation. Do not push without explicit authorization.

