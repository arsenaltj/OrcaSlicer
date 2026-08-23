# Printable Palette Real-Provider Benchmark Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Build and run a resumable real-provider benchmark that measures AI four-color recommendation and Image2 printable-preview quality before any Tripo task is allowed.

**Architecture:** A manifest-driven Python runner owns benchmark state and paid-call auditing. A separate image-review module validates GPT vision output into a provider-neutral report, while existing palette recommendation and printable-image modules remain the production implementation under test. All artifacts stay under `generated_models/`; no Orca workspace, 3MF, profile, or slicer code changes are required.

**Tech Stack:** Python 3.10+, `unittest`, existing OpenAI-compatible adapter, Pillow-based printable image pipeline, JSON/CSV artifacts.

---

### Task 1: Freeze the phase 64 benchmark manifest

**Files:**
- Create: `Docs/benchmarks/printable-palette-phase64-v1.json`
- Test: `tools/ai/test_printable_palette_benchmark.py`

**Step 1: Write the failing manifest-loader tests**

Cover unique case IDs, allowed styles, bounded non-empty prompts, optional reference images constrained to the manifest directory, canonical fingerprinting, and filtering by case ID.

**Step 2: Run the test and verify it fails**

Run: `python -m unittest tools.ai.test_printable_palette_benchmark -v`

Expected: import failure because `printable_palette_benchmark` does not exist.

**Step 3: Add a 12-case manifest**

Use previously uncalibrated prompts across character, animal, stable product, vehicle, architecture, and organic categories. Do not include fixed palettes; palette recommendation is the behavior under test.

**Step 4: Implement the minimal typed loader**

Create immutable `PaletteBenchmarkCase` and `PaletteBenchmarkManifest` records. Normalize manifest JSON and derive a SHA-256 fingerprint from the canonical value.

**Step 5: Run the tests**

Expected: manifest tests pass.

### Task 2: Add resumable per-case recommendation and Image2 stages

**Files:**
- Create: `tools/ai/printable_palette_benchmark.py`
- Modify: `tools/ai/test_printable_palette_benchmark.py`

**Step 1: Write failing tests**

Cover new-state creation, fingerprint mismatch rejection, recommendation persistence, no duplicate successful recommendation call, uncertain-call refusal, Image2 output hashing, local processing reuse, and explicit paid-call confirmation.

**Step 2: Run the targeted tests and confirm failure**

Run: `python -m unittest tools.ai.test_printable_palette_benchmark -v`

**Step 3: Implement atomic state updates**

Store `palette-case-state.json` with stages, paid-call attempts, recommendation DTO, artifacts, hashes, and timestamps. Mark a stage `calling` before invoking its provider function. Reuse a complete stage when its artifact and hash still match.

**Step 4: Implement local gating**

Pass the recommended ordered palette and semantic role mapping into `generate_image` and `process_printable_image`. Persist raw, strict, clean, model-reference, and metrics artifacts.

**Step 5: Run targeted tests**

Expected: all state, recovery, and local-gate tests pass without network access.

### Task 3: Add printable palette visual review

**Files:**
- Create: `tools/ai/printable_palette_visual_quality.py`
- Create: `tools/ai/test_printable_palette_visual_quality.py`
- Modify: `tools/ai/printable_palette_benchmark.py`

**Step 1: Write failing validation tests**

Test fenced JSON, missing checks, bounded scores/text, `review` aggregation, score-below-80 behavior, provider exceptions, caching by strict-preview SHA and model, and the two-image limit.

**Step 2: Run the tests and verify failure**

Run: `python -m unittest tools.ai.test_printable_palette_visual_quality -v`

**Step 3: Implement the review module**

Call `complete_vision` through an injected callable. Normalize all six checks and write `palette-visual-quality.json`. Provider or schema failures become `unavailable` and never invalidate local artifacts.

**Step 4: Integrate the review stage into the runner**

Require explicit visual-call confirmation and apply the same uncertain-call refusal as recommendation and Image2 stages.

**Step 5: Run both new test modules**

Expected: all tests pass with mocked provider functions.

### Task 4: Add summary and manual-review contracts

**Files:**
- Modify: `tools/ai/printable_palette_benchmark.py`
- Modify: `tools/ai/test_printable_palette_benchmark.py`

**Step 1: Write failing summary tests**

Cover paid-call totals, stage counts, local and visual pass rates, `tripo_candidate` requiring local pass plus visual pass plus explicit human approval, deterministic JSON/CSV ordering, and error rows.

**Step 2: Implement collection and CLI**

Support `prepare`, `review`, `report`, and `manual-review` actions; `--case` filtering; explicit confirmation flags; explicit retry of uncertain paid calls; and a default output under `generated_models/printable-palette-phase64-v1`.

**Step 3: Run targeted tests**

Expected: summary and CLI tests pass without paid calls.

### Task 5: Run offline regression

**Files:**
- No source changes unless a regression is found.

**Step 1: Run new tests**

Run: `python -m unittest tools.ai.test_printable_palette_benchmark tools.ai.test_printable_palette_visual_quality -v`

**Step 2: Run related tests**

Run: `python -m unittest tools.ai.test_openai_preprocessor tools.ai.test_printable_image_pipeline tools.ai.test_printable_palette tools.ai.test_sidecar_contract -v`

**Step 3: Run the complete offline AI suite**

Run: `python -m unittest discover -s tools/ai -p 'test_*.py'`

Expected: all tests pass except any previously documented environment-only readiness limitation, which must be reported separately rather than hidden.

### Task 6: Execute the controlled paid pilot

**Files:**
- Runtime output only: `generated_models/printable-palette-phase64-v1/`

**Step 1: Verify configuration without printing secrets**

Confirm OpenAI-compatible text and image models and API key presence. Do not log credential values.

**Step 2: Run recommendation and Image2 stages**

Run the frozen cases with explicit paid confirmation. Start with two cases, inspect recovery and artifacts, then continue the remaining cases.

**Step 3: Run GPT visual review**

Review every locally completed case and generate the benchmark summary.

**Step 4: Inspect the evidence**

Record provider failures, contract validity, local-gate pass rate, visual scores, and proposed Tripo candidates. Do not launch Tripo from this benchmark.

### Task 7: Verify and commit

**Files:**
- Modify only files identified by preceding tasks.

**Step 1: Run syntax and formatting checks**

Run: `python -m py_compile tools/ai/printable_palette_benchmark.py tools/ai/printable_palette_visual_quality.py`

Run: `git diff --check`

**Step 2: Confirm architectural boundary**

Verify no changes to `MainFrame`, `Plater`, CMake, 3MF, profiles, or root `task_plan.md`, `findings.md`, and `progress.md`.

**Step 3: Commit implementation and evidence documentation**

Use focused commits after the existing HEAD. Do not rewrite history and do not notify the integration line.
