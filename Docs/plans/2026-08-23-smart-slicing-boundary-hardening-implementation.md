# Smart Slicing Boundary Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce the smart-slicing integration surface in shared Orca files, make the formal mutation gateway physically authoritative, align the legacy Sidebar projection, and re-verify the latest local build.

**Architecture:** Keep Domain/Application/Ports unchanged except for test-driven state projection corrections. Move Plater-specific candidate validation and transactional writes into the Orca gateway implementation, then collect workflow ownership in one Orca workbench integration object. Preserve the existing revision guards, isolated trial copies, native Undo, and official Preview path.

**Tech Stack:** C++17, OrcaSlicer Model/Print/DynamicPrintConfig, wxWidgets/wxAUI, CMake, Catch2.

---

### Task 1: Record the hardening decision

**Files:**
- Create: `Docs/architecture/ADR-003-smart-slicing-orca-bridge-and-release-hardening.md`
- Create: `Docs/plans/2026-08-23-smart-slicing-boundary-hardening-design.md`
- Create: `Docs/plans/2026-08-23-smart-slicing-boundary-hardening-implementation.md`

**Steps:**

1. Record the accepted constraints, alternatives, shared-file reasons, failure modes, and release gates.
2. Run `git diff --check`.
3. Commit only these documentation files as `docs(smart-slicing): plan boundary hardening`.

### Task 2: Correct the legacy workflow projection

**Files:**
- Modify: `tests/slic3rutils/test_smart_slicing_workflow.cpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.cpp`

**Steps:**

1. Add failing projection checks for ReadyToApply, Applying, OfficialSlicing, Completed, and ApplyFailed.
2. Run `build-p0/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.exe "[AI][SmartSlicing][Workflow]"` and confirm the ReadyToApply legacy checks fail.
3. Set legacy steps from the same workflow state: pre-apply preparation complete; official Slice/G-code waiting until apply; Slice running during official slicing; all success only after completion.
4. Run focused and full smart-slicing tests.
5. Commit as `fix(smart-slicing): align legacy workflow projection`.

### Task 3: Move formal candidate writes into the Orca gateway

**Files:**
- Modify: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.cpp`
- Modify: `src/slic3r/GUI/Plater.cpp`
- Modify: `src/slic3r/CMakeLists.txt`
- Modify: `tests/slic3rutils/test_smart_slicing_workflow.cpp`

**Steps:**

1. Extend gateway tests for the Plater-native construction seam where practical; retain the existing callback constructor tests.
2. Move transform resolution, matrix validation, typed plate patch preparation, one-snapshot mutation, dirty/invalidation, Preview and Undo callbacks into `OrcaOfficialSliceGateway.cpp`.
3. Leave only the standard official-slice start callback in `Plater::enable_smart_slicing()`.
4. Build `slic3rutils_tests` and `OrcaSlicer`; run gateway and smart-slicing tests.
5. Verify `Plater.cpp` no longer contains `set_transformation` or `plate->config()->set_key_value` in the smart-slicing block.
6. Commit as `refactor(smart-slicing): move formal writes into Orca gateway`.

### Task 4: Consolidate workbench ownership

**Files:**
- Create: `src/slic3r/GUI/AI/Orca/OrcaSmartSlicingWorkbench.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaSmartSlicingWorkbench.cpp`
- Modify: `src/slic3r/GUI/Plater.cpp`
- Modify: `src/slic3r/GUI/Plater.hpp`
- Modify: `src/slic3r/CMakeLists.txt`

**Steps:**

1. Add an Orca workbench object that owns the adapter, executor, gateway, coordinator, runtime store, presenter, and panel.
2. Move candidate capture, cancellation, UI dispatch, runtime cleanup, and Sidebar projection wiring into that object.
3. Replace the seven `Plater::priv` smart-slicing members with one workbench owner.
4. Keep wxAUI pane creation/showing and the native slice-start hook as the only Plater integration.
5. Build application and tests; inspect shutdown/cancel behavior.
6. Commit as `refactor(smart-slicing): consolidate workbench integration`.

### Task 5: Reconcile native validation evidence

**Files:**
- Modify: `tests/slic3rutils/test_smart_slicing_workflow.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp`
- Modify if needed: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.cpp`

**Steps:**

1. Add a failing test showing a successful real baseline trial resolves `NativeValidationUnavailable` without hiding unrelated warnings.
2. Update only the workflow-owned report after a successful baseline result; never modify the formal workspace.
3. Recalculate readiness and publish the updated immutable snapshot.
4. Run focused and full smart-slicing tests.
5. Commit as `fix(smart-slicing): resolve validation evidence after trial`.

### Task 6: Build and local-only GUI verification

**Files:**
- Modify: `Docs/plans/2026-08-23-smart-slicing-boundary-hardening-implementation.md`
- Do not modify repository-root planning files

**Steps:**

1. Run `git diff --check` and the architecture include/write-path audit.
2. Build RelWithDebInfo `slic3rutils_tests`, `fff_print_tests`, `OrcaSlicer`, and `OrcaSlicer_app_gui`.
3. Run the smart-slicing filter, full `slic3rutils_tests`, focused real-trial tests, and the established FFF baseline/exclusion checks.
4. Stage the latest executable and DLL into the workspace-local validation directory using the repository's normal build/package mechanism; verify hashes/timestamps.
5. Confirm no other Orca process is running before launch. Do not terminate unrelated instances automatically.
6. Use only `D:/Workspace/06_3DDY_smart_slicing/build-p0/OrcaSlicer/orca-slicer.exe` with a disposable project to verify normal closed-workbench slicing, offline behavior, candidate completion, cancel, stale, apply, official Preview, and one Undo.
7. Record exact evidence and remaining macOS/Linux CI gates, commit as `docs(smart-slicing): record boundary hardening verification`.

### Task 7: Continue roadmap candidates after hardening

**Files:**
- Add only under smart-slicing modules and tests; shared files require a separate reason

**Steps:**

1. Design and test one native Orient candidate on cloned models.
2. Connect a narrow `IParameterAdvisor` adapter without importing model-generation business code.
3. Add low-risk typed candidates incrementally; keep hardware, calibration, flush multiplier, and wipe-tower protections.
4. Add cache/benchmark and cross-platform gates as separate commits.
5. Stop before any integration-line fetch or handoff until the project owner confirms.

