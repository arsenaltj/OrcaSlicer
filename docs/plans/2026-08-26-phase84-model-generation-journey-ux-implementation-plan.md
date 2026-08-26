# Model Generation Journey UX Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Improve model-generation decision clarity, performance evidence, history management, and internal-Beta print feedback without weakening paid-call or recovery safeguards.

**Architecture:** Keep changes inside `ModelGenerationPanel` and its compatible local metadata. Use task-oriented labels and collapsible details, measure local OBJ parsing, and persist only facts the application can prove or the tester explicitly records.

**Tech Stack:** C++17, wxWidgets, Boost filesystem, nlohmann JSON, CMake/MSVC, Python unittest.

---

### Task 1: Simplify preview stages

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. Rename the four visible preview stages with task-oriented labels.
2. Add a collapsed processing-details control.
3. Keep plain-language stage guidance visible and move metrics into details.
4. Build the Release target and inspect the page visually.

### Task 2: Complete paid 3D confirmation

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. State one paid task, Provider-owned pricing, expected 3–10 minute duration, and remote cancellation limitations.
2. Retain target triangle count and explicit Yes/No confirmation.
3. Verify declining the dialog creates no task.

### Task 3: Record local OBJ performance

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. Time generated and historical OBJ parsing with `steady_clock`.
2. Show actual seconds and a high-complexity warning at 300,000 triangles.
3. Persist parse duration in library metadata without breaking older metadata.
4. Verify with existing ready models of different sizes.

### Task 4: Make history actions discoverable

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. Add visible Load and Delete Local buttons to each card.
2. Confirm deletion and validate all deleted paths are descendants of the generated-model root.
3. Preserve double-click as a secondary shortcut.
4. Refresh the list and handle deletion of the currently displayed model safely.

### Task 5: Add honest Beta lifecycle feedback

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

1. Record successful import time and whether automatic slicing was requested.
2. Show those facts in history details.
3. Add explicit tester actions for print success or print issue, storing timestamp and outcome locally.
4. Never infer print success from import or slice request.

### Task 6: Regression and delivery

1. Run the 280-test Sidecar suite.
2. Rebuild and run `slic3rutils` tests.
3. Build and stage Release.
4. Run GUI acceptance for preview disclosure, paid confirmation, performance display, history load/delete/feedback, and Phase 83 recovery.
5. Run `git diff --check`, document results, and rebuild the Windows AI test package.
