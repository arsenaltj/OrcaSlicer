# Model Generation Beta Blocker Recovery Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove the model-generation internal-beta recovery blockers and align paid/service recovery interactions.

**Architecture:** Keep control requests serialized, retain file downloads independently, and make cancellation reflect remote-generation versus local-loading state. Reuse MainFrame service discovery through a narrow callback.

**Tech Stack:** C++17, wxWidgets, Orca HTTP wrapper, Python unittest, CMake/MSVC.

**Status:** Completed on 2026-08-26. Python 280/280, C++ 23/23, Release build, GUI recovery acceptance, staged install, and Windows AI test package all passed.

---

### Task 1: Restore input HTTP contract

**Files:**
- Modify: `tools/ai/test_sidecar_contract.py`
- Modify: `tools/ai/orca_ai_sidecar.py`

1. Add a failing contract test that registers an image job with an input file and downloads `/input`.
2. Run the targeted test and verify the route returns 404 before the fix.
3. Add `input` to the validated job action set.
4. Run the targeted test and full sidecar contract suite.

### Task 2: Independent download lifetime

**Files:**
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`

1. Add a request collection dedicated to downloads.
2. Stop cancelling the control request when a download starts.
3. Make `cancel_current()` cancel and clear both control and download requests.
4. Build the affected Release target.

### Task 3: Correct local-loading cancellation

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. Label ready artifact activity as “取消加载”.
2. In `on_stop()`, cancel locally and preserve ready state when the artifact is only loading.
3. Ensure controls expose retry and no `/stop` request is sent.
4. Verify history load, cancel, and retry in the Release GUI.

### Task 4: Paid review and service retry UX

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`
- Modify: `src/slic3r/GUI/MainFrame.cpp`

1. Add a Yes/No confirmation before visual review.
2. Add an unavailable-state “重新检测服务” button with a narrow callback.
3. Reset and reuse MainFrame discovery when requested.
4. Verify labels, disabled states, focus order, and recovery in the Release GUI.

### Task 5: Regression and delivery verification

**Files:**
- Modify: `Docs/architecture/2026-08-26-phase83-beta-blocker-recovery-review.md`

1. Run targeted and full Python tests.
2. Run configured C++ tests or document if the build has tests disabled.
3. Build Release and run `git diff --check`.
4. Perform cold-start image-job recovery without paid calls.
5. Record package readiness and remaining Beta risks.
