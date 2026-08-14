# Core Demo Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `executing-plans` to implement this plan task-by-task.

**Goal:** Deliver a repeatable Windows demo that uses real OpenAI preprocessing and Tripo generation, imports the result into OrcaSlicer, and reaches slice preview.

**Architecture:** Keep the existing C++ GUI and provider-neutral sidecar contract unchanged. Add a small loopback capability preflight used by the Windows launcher, plus an explicit paid-call smoke client used for repeatable text/image verification and artifact capture. Use OrcaSlicer's existing import, arrange, validation, slicing, and 3MF persistence paths for the remainder of the demo.

**Tech Stack:** Windows batch/PowerShell, Python 3 standard library, OrcaSlicer C++17/wxWidgets, existing sidecar protocol v1.

---

### Task 1: Capability-aware Windows launcher

**Files:**
- Create: `tools/ai/check_sidecar_capability.ps1`
- Create: `tools/ai/test_sidecar_readiness.py`
- Modify: `start_orcaslicer_with_ai.bat`

**Steps:**
1. Add failing unittest cases for ready, unavailable, invalid-protocol, and unreachable sidecars.
2. Run `python tools/ai/test_sidecar_readiness.py -v`; expect failures because the checker does not exist.
3. Implement the loopback-only protocol-v1 capability checker with exit codes `0=ready`, `1=unreachable`, `2=reachable but incompatible/unavailable`.
4. Update the launcher to reuse an already-ready sidecar, start one only when unreachable, fail fast when generation is unavailable, and stop launching OrcaSlicer on timeout.
5. Add `--check` so demo readiness can be verified without opening the GUI.
6. Run readiness tests and `start_orcaslicer_with_ai.bat --check`; expect all tests and the real preflight to pass.

### Task 2: Explicit real-provider smoke client

**Files:**
- Create: `tools/ai/smoke_model_generation.py`
- Create: `tools/ai/test_smoke_model_generation.py`

**Steps:**
1. Add mock-backed tests for text preprocessing/generation/artifact download and image preprocessing/generation/artifact download.
2. Run the new tests; expect failure because the client does not exist.
3. Implement protocol-v1 health validation, native-client headers, text JSON, image multipart, bounded polling, artifact download, job cleanup, and sanitized status output.
4. Require `--confirm-paid-call` before any job creation; never read or print provider keys.
5. Run mock smoke tests, the existing sidecar contract tests, and `py_compile`.

### Task 3: Real text and image smoke

**Files:**
- Create outputs under: `.workbuddy/core-demo-20260810/`

**Steps:**
1. Run one paid text smoke against `http://127.0.0.1:18764` with a small printable demo prompt.
2. Verify the returned artifact exists, has a supported extension, non-zero size, and a valid sidecar status trail.
3. Run one paid image smoke using `tripo-reference-test.png` plus a short printable-object instruction.
4. Verify and retain both artifacts; record durations, formats, and non-sensitive failures only.

### Task 4: OrcaSlicer golden-path rehearsal

**Files:**
- Create outputs under: `.workbuddy/core-demo-20260810/`

**Steps:**
1. Launch the installed Release build through the hardened launcher.
2. Run or load a real generated artifact, confirm any OBJ color mapping, and verify an undoable import.
3. Use existing arrange/validation paths, slice with the configured printer/material, and reach Preview without blocker errors.
4. Save a 3MF demo project, close, reopen, and verify model/config persistence.
5. Capture screenshots for generation-ready, imported plate, and slice preview states.

### Task 5: Demo runbook and final regression

**Files:**
- Create: `Docs/demo/2026-08-10-core-demo-runbook.md`
- Modify: `task_plan.md`
- Modify: `findings.md`
- Modify: `progress.md`

**Steps:**
1. Document prerequisites, one-click startup, fixed text/image inputs, expected confirmations, normal timing, and honest fallback procedure.
2. Run launcher readiness, Python contract/readiness/smoke tests, targeted C++ tests, and Windows Release build as time permits.
3. Run `git diff --check` and inspect the final diff for credentials, generated artifacts, or unrelated changes.
4. Mark the sprint complete only after both real calls and the saved/reopened slice project pass.
