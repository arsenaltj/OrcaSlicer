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

---

## Verification record — 2026-08-24

### RelWithDebInfo build and tests

- Built `slic3rutils_tests`, `fff_print_tests`, `OrcaSlicer`, and `OrcaSlicer_app_gui` successfully from `build-p0`.
- Smart-slicing suite: 55 test cases, 366 assertions, all passed.
- Full `slic3rutils_tests`: 66 test cases, 473 assertions, all passed. The existing test-working-directory warning for `info/nozzle_info.json` remains non-failing.
- Focused native trial slice (`[OrcaTrial]`): 3 test cases, 20 assertions, all passed.
- Focused native-validation evidence reconciliation: 1 test case, 11 assertions, all passed.
- FFF fixed seed `635773145`:
  - exact `Scenario: Skirt and brim generation`: 1 test case, 7 assertions, all passed;
  - suite excluding `[SkirtBrim]`: 47 test cases, 578 assertions, all passed;
  - full suite reproduced the established order-sensitive `SIGSEGV` at `tests/fff_print/test_skirt_brim.cpp:308` (37 cases run, 36 passed and 1 failed; 426 assertions, 425 passed and 1 failed). The two isolated runs above keep this pre-existing failure separate from the smart-slicing changes.

### Workspace-local package

The normal CMake install step staged the latest binaries into `build-p0/OrcaSlicer`. Source-build and installed hashes match:

| Artifact | Size | Build timestamp (Asia/Shanghai) | SHA-256 |
| --- | ---: | --- | --- |
| `orca-slicer.exe` | 336,896 bytes | 2026-08-24 01:18:03.671 +08:00 | `69A4A99198F3037F22D5D6308AC859267F50E829800136138A164478E6E38252` |
| `OrcaSlicer.dll` | 126,475,776 bytes | 2026-08-24 01:17:55.369 +08:00 | `F559FAF895975A6C2CF75E04B06602866B34C186134AFEF417832A2507FEA845` |

GUI interaction was intentionally not started. Two unrelated Orca instances were still active from
`C:/Users/ltj/AppData/Local/Temp/orca_ai_integration_v1_release_build2/src/orca-slicer.exe`
(PIDs 35252 and 39428, both using `--datadir C:/Users/ltj/AppData/Local/Temp/orca_ai_gui_smoke_3d188b4abd`).
Per the local-only validation rule, they were not terminated and the workspace executable was not launched alongside them. The GUI scenarios in Task 6 step 6 therefore remain an external-environment verification gate.

### Boundary audit

- `git diff --check` passed and the worktree was clean before this verification record.
- Domain, Application, and Ports contain no wxWidgets or `Plater` includes.
- Formal candidate transform/config writes, snapshot rollback, native Undo, and Preview transition are confined to `OrcaOfficialSliceGateway`.
- The other candidate-related mutations are isolated: `OrcaTrialSliceExecutor` writes only to cloned trial models, and `OrcaParameterProposalAdapter` writes only to a temporary validation config.
- No model-generation business code was copied or modified in this hardening batch.
- No configuration schema, dependency, network port, 3MF/profile format, profile data, or default Orca behavior changed. The existing temporary smart-slicing runtime journal location is unchanged; no new data directory was introduced.
- macOS and Linux build/test gates remain for CI or their native build hosts.

## Cache and Release gate — 2026-08-24

Commit `79627374dfae927b1b461fe92f9d2a7b7d4c1cb3` adds an Application-layer
`ITrialSliceExecutor` decorator. It keeps at most 16 successful, identity-matching results in memory.
The key includes the workspace revision and all executable candidate repair, transform, and typed-parameter
content. Failed, canceled, and mismatched results are never cached; cancellation is delegated to the native
executor. The cache writes no disk data, runtime journal, 3MF, profile, or official slice result.

### Release build and tests

- Windows Release builds passed for `slic3rutils_tests`, `fff_print_tests`, `OrcaSlicer`, and
  `OrcaSlicer_app_gui` from `build-p0`.
- Cache focus: 4 test cases, 13 assertions, all passed. The tests cover content hits and misses,
  revision isolation, failure/cancellation/mismatch rejection, cancellation delegation, and FIFO eviction.
- Smart-slicing suite: 63 test cases, 405 assertions, all passed.
- Full `slic3rutils_tests`: 74 test cases, 512 assertions, all passed. The existing
  `info/nozzle_info.json` working-directory warning remains non-failing.
- FFF fixed seed `635773145`:
  - exact `Scenario: Skirt and brim generation`: 1 test case, 7 assertions, all passed;
  - suite excluding `[SkirtBrim]`: 47 test cases, 578 assertions, all passed.

| Release artifact | Size | Build timestamp (Asia/Shanghai) | SHA-256 |
| --- | ---: | --- | --- |
| `build-p0/src/Release/orca-slicer.exe` | 270,848 bytes | 2026-08-24 04:33:10.067 +08:00 | `83EE9595630138C9C107F92F88750260D144F16DCE0BF7DCE88F9CBD976D625C` |
| `build-p0/src/Release/OrcaSlicer.dll` | 73,097,728 bytes | 2026-08-24 04:33:02.292 +08:00 | `0602F7FC9E3841C6C5BF58A94BEB5F7AD3C5F23B9ED2837883FACD98F75A526C` |

GUI automation could not proceed safely. The workspace Release executable was selected explicitly, but it
did not expose a targetable window. A subsequent process-path check showed that the two unrelated integration
instances (PIDs 35252 and 39428) were still active; the earlier compact process output had obscured them. The
workspace-local process (PID 24800) was path-verified and stopped, while the unrelated processes were left
untouched. No GUI action was performed. The local-only GUI scenarios and macOS/Linux build gates remain open.

This batch changes no configuration, dependency, network port, data directory, 3MF/profile format, profile
data, or default Orca behavior. Its only shared-file change is the smart-slicing source registration in
`src/slic3r/CMakeLists.txt`; `MainFrame.cpp` and `Plater.cpp` are unchanged.
