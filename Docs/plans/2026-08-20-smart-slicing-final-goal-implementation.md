# Smart Slicing Final Goal Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Complete the ADR-002 smart-slicing workbench from the current read-only P0 through isolated deterministic candidates, real trial-slice comparison, transactional application, typed parameter advice, and recovery/compatibility gates.

**Architecture:** Domain and Application remain provider- and wx-free. Orca-specific cloning, trial slicing, official application, Undo, invalidation, and Preview navigation live only under `GUI/AI/Orca`; the panel projects immutable workflow snapshots and never writes the formal workspace. Every report and candidate is bound to a `WorkspaceRevision`, trial slicing is sequential, and formal mutation occurs only after a final revision check in one native Undo transaction.

**Tech Stack:** C++17, OrcaSlicer `Model`/`Print`/`DynamicPrintConfig`, wxWidgets, CMake, Catch2.

---

## Scope decisions

- Keep the approved single-page dockable workbench.
- P1 optimizes for stable printing first. It reads multicolor/tool-change metrics but leaves joint flush tuning to P2.
- Generate at most baseline plus two alternatives; execute at most one trial slice at a time.
- Treat incomplete metrics as unavailable data, never as zero-cost or a fabricated score.
- Keep the established model-generation import compatibility path unchanged.
- Do not modify repository-root `task_plan.md`, `findings.md`, or `progress.md`.

### Task 1: Candidate comparison domain

**Files:**
- Modify: `src/slic3r/AI/SmartSlicing/Domain/SmartSlicingTypes.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/SlicingMetrics.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Domain/CandidateComparison.cpp`
- Modify: `src/slic3r/CMakeLists.txt`
- Test: `tests/slic3rutils/test_smart_slicing_candidates.cpp`

**Steps:**

1. Add failing tests for deterministic ordering, hard-failure exclusion, stable tie breaking, unavailable metrics, and the three-candidate cap.
2. Run `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Candidate]"` and verify the new cases fail or do not compile.
3. Add explicit metric availability/status and comparison result types. Preserve stable IDs and avoid a single opaque AI score.
4. Implement pure comparison rules: valid candidates first, stability hard gates, then goal-specific Pareto evidence, then candidate ID as deterministic tie breaker.
5. Build `slic3rutils_tests` and run candidate plus full smart-slicing tests.
6. Commit as `feat(smart-slicing): add deterministic candidate comparison`.

### Task 2: Candidate and trial-slice application workflow

**Files:**
- Create: `src/slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.cpp`
- Create: `src/slic3r/AI/SmartSlicing/Application/TrialSlicingWorkflow.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Application/TrialSlicingWorkflow.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/WorkflowSnapshot.hpp` or keep the snapshot in `WorkflowState.hpp` if extraction would not simplify dependencies
- Modify: `src/slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_workflow.cpp`

**Steps:**

1. Add fake planner/executor tests for baseline-first ordering, sequential execution, partial candidate failure, cancel propagation, late result rejection, and stale revision rejection.
2. Verify the tests fail before implementation.
3. Extend the state machine with `PlanningCandidates`, `TrialSlicingBaseline`, `TrialSlicingCandidates`, and `ReadyToApply`.
4. Make executor results explicit (`success`, `canceled`, `failed`, metrics, diagnostic code) instead of throwing across the Application boundary.
5. Store immutable candidates and comparison data in the workflow snapshot; require matching workflow ID and base revision for every accepted result.
6. Keep baseline available when an alternative fails; fail the workflow only when no comparable baseline exists.
7. Build and run all smart-slicing tests, then full `slic3rutils_tests`.
8. Commit as `feat(smart-slicing): orchestrate isolated trial candidates`.

### Task 3: Orca candidate snapshot and isolated trial slice

**Files:**
- Create: `src/slic3r/GUI/AI/Orca/OrcaCandidateFactory.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaCandidateFactory.cpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.cpp`
- Modify: `src/slic3r/CMakeLists.txt`
- Test: `tests/fff_print/test_smart_slicing_trial.cpp` or the nearest existing Print integration target

**Steps:**

1. Add an integration fixture that creates a tiny printable model and captures formal Model/config/Print fingerprints before a trial.
2. Verify a missing executor causes the new test to fail.
3. Clone Model and effective config into workflow-owned RAII state; never reuse the formal plate `Print` as the candidate output.
4. Apply transforms and typed config patches only to the clone, then construct and process an isolated `Print` sequentially.
5. Extract available `GCodeProcessorResult`/`PrintStatistics` values into `SlicingMetrics`, with explicit availability for unsupported values.
6. Add cancellation checkpoints and guarantee temporary state cleanup on success, failure, cancellation, and exception.
7. Assert before/after formal fingerprints, dirty state, Undo stack, and formal Preview result are identical.
8. Build the focused integration target and OrcaSlicer Release target; run focused tests.
9. Commit as `feat(smart-slicing): add isolated Orca trial slicing`.

### Task 4: Deterministic placement candidates

**Files:**
- Create: `src/slic3r/GUI/AI/Orca/OrcaPlacementCandidateProvider.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaPlacementCandidateProvider.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/PlacementCandidate.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Application/CandidatePlanningWorkflow.*`
- Test: `tests/slic3rutils/test_smart_slicing_candidates.cpp`
- Test: focused Arrange/Orient integration test target

**Steps:**

1. Add failing tests for baseline preservation, stable candidate IDs, locked object/plate protection, build-volume fit, and deterministic candidate order.
2. Expose only the minimum adapter DTO needed to invoke existing Orient/Arrange behavior against cloned data.
3. Generate a baseline and one stability-oriented alternative; add a third only when it represents a measurable trade-off.
4. Reject candidates that move locked targets, violate sequential-print clearance, exclusion areas, or wipe-tower space.
5. Run domain, adapter, and integration tests.
6. Commit as `feat(smart-slicing): generate isolated placement candidates`.

### Task 5: Candidate comparison workbench

**Files:**
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.cpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.*`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.*`
- Test: `tests/slic3rutils/test_smart_slicing_workflow.cpp`

**Steps:**

1. Add projection tests for planning, trial slicing, partial failure, candidate-ready, stale, and canceled states.
2. Render at most three candidate cards using identical units and baseline deltas for time, material, support, and tool changes.
3. Show recommendation reasons and costs in text; do not rely on color or an opaque total score.
4. Add select, retry failed candidate, keep baseline, and proceed-to-apply commands without mutating the workspace.
5. Ensure the primary action remains visible at supported window sizes and keyboard focus order is stable.
6. Build OrcaSlicer, launch the Release app, and capture/inspect the workbench states where automation permits.
7. Commit as `feat(smart-slicing): add candidate comparison workbench`.

### Task 6: Transactional apply and official slicing

**Files:**
- Create: `src/slic3r/AI/SmartSlicing/Application/ApplyWorkflow.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Application/ApplyWorkflow.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.cpp`
- Modify: `src/slic3r/GUI/Plater.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_apply.cpp`
- Test: focused GUI/Plater integration test if available

**Steps:**

1. Add contract tests for stale rejection, compatibility revalidation, one transaction, apply failure rollback, official-slice failure, and successful Preview navigation.
2. Change the gateway to a prepare/commit/result contract so Application can distinguish rejected, applied, slicing, completed, and failed states.
3. Re-read `WorkspaceRevision` immediately before apply and reject mismatches in Application and adapter layers.
4. On the GUI thread, create one native Undo snapshot, apply accepted transforms/config patches, mark dirty, and use normal invalidation.
5. Trigger the standard official plate slicing path and navigate to the existing Preview only after a successful result.
6. On pre-commit failure, leave the project unchanged; on post-commit slicing failure, expose one native Undo recovery action and never claim completion.
7. Verify one Undo restores the exact pre-apply model/config state.
8. Build, run focused tests and full smart-slicing tests.
9. Commit as `feat(smart-slicing): apply candidates transactionally`.

### Task 7: Typed parameter proposals and safe advisor degradation

**Files:**
- Create: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposal.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Ports/IParameterAdvisor.hpp`
- Add an adapter under: `src/slic3r/GUI/AI/Orca/`
- Test: `tests/slic3rutils/test_smart_slicing_parameters.cpp`

**Steps:**

1. Add failing tests for key, type, enum/range, scope, ownership, hardware/calibration forbidden keys, and change-budget limits.
2. Extract reusable validation knowledge from existing AI config suggestion code without changing its current public behavior.
3. Require provider output to be typed and validate it before it becomes a `SliceCandidate`.
4. Run `Print::validate()` against the isolated candidate and trial slice it before presentation.
5. If the advisor is unavailable or invalid, retain deterministic local candidates and expose a non-blocking explanation.
6. Run tests and commit as `feat(smart-slicing): validate typed parameter candidates`.

### Task 8: Multicolor metrics and joint optimization

**Files:**
- Modify: `src/slic3r/AI/SmartSlicing/Domain/SlicingMetrics.hpp`
- Modify: Orca trial metrics extractor
- Modify: candidate comparison rules
- Test: multicolor focused fixtures

**Steps:**

1. Add tests for physical-slot compatibility, layer tool sequence, flush volume, wipe-tower material, color mapping degradation, and unavailable metrics.
2. Read the existing tool order and flush matrix; do not infer cost from color count.
3. Add low-risk candidates only for combinations already accepted by Orca validation.
4. Never silently disable the wipe tower or reduce flush multipliers.
5. Compare multicolor costs with explicit evidence and retain the original mapping as baseline.
6. Run focused and regression tests, then commit as `feat(smart-slicing): compare multicolor slicing costs`.

### Task 9: Runtime journal, resource budgets, and cleanup

**Files:**
- Implement: `src/slic3r/AI/SmartSlicing/Ports/IWorkflowRuntimeStore.hpp` adapter
- Add: workflow runtime DTO and cleanup policy
- Modify: coordinator and trial executor
- Test: workflow recovery and cleanup tests

**Steps:**

1. Add tests for restart with an incomplete workflow, stale journal discard, cancellation cleanup, timeout, candidate cap, and sequential execution.
2. Journal only workflow metadata and candidate descriptors; never persist provider raw text, mesh copies, credentials, or trial G-code in 3MF.
3. Restore as a recoverable summary, recalculate against the current revision, and discard mismatched temporary candidates.
4. Add elapsed-time, memory/disk budget hooks, and cleanup on every terminal state.
5. Verify interrupted workflows cannot affect normal manual slicing.
6. Commit as `feat(smart-slicing): recover and budget trial workflows`.

### Task 10: Final compatibility and release verification

**Files:**
- Modify: `Docs/plans/2026-08-20-smart-slicing-final-goal-implementation.md`
- Add focused verification notes under `Docs/plans/` if needed
- Do not modify repository-root planning files

**Steps:**

1. Run `git diff --check` and a write-path audit proving pre-confirmation zero mutation of formal Model/config/Preview.
2. Build Release `slic3rutils_tests`, focused Print tests, `OrcaSlicer`, and `OrcaSlicer_app_gui`.
3. Run full available Catch2/CTest suites relevant to changed slicing and GUI code.
4. Smoke-test ordinary import/slice/Preview with the workbench closed and AI unavailable.
5. Smoke-test old 3MF/profile loading, single/multicolor, multi-plate, locked objects, cancel, stale, apply, one-step Undo, and official Preview.
6. Record any platform verification that cannot run locally as an explicit CI gate rather than claiming it passed.
7. Review the diff for architecture boundaries, remove duplication and generated-code bloat, and commit final documentation.

## Completion definition

- A normal or generated OBJ/STL can enter the same smart-slicing workbench.
- Preflight, planning, and trial slicing have zero formal workspace side effects.
- Baseline and at least one deterministic candidate are compared using real isolated slice metrics.
- Editing the workspace invalidates reports and candidates; stale application is rejected in Application and adapter layers.
- A selected candidate applies through one native Undo transaction and normal Orca official slicing/Preview.
- Sidecar absence, candidate failure, cancellation, and workbench closure do not alter normal Orca behavior.
- Typed parameter and multicolor changes obey compatibility, scope, calibration, and hardware boundaries.
- Tests, build evidence, compatibility notes, and remaining CI-only gates are documented honestly.
