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

## Portability and benchmark gate — 2026-08-24

The runtime journal uses `boost::nowide` streams so non-ASCII data-directory paths remain valid on Windows
without adding platform-specific code. The portability contract creates a journal under a Unicode path,
round trips bounded workflow metadata, and removes it through the existing store API. This test is part of
the normal smart-slicing suite on every platform.

The opt-in `[AI][SmartSlicing][Performance]` benchmark measures both a real isolated tiny trial slice and a
successful in-memory cache hit. It is skipped unless `ORCA_SMART_SLICING_BENCHMARK=1` is set, so ordinary
Catch/CTest execution remains deterministic. Release verification should run it with a small explicit
sample count and record both timings; it is evidence for comparison, not a hardware-independent time limit.

Windows Release evidence with three samples and a 50 ms warm-up: the isolated tiny trial averaged about
86.1 ms, while a successful cache hit averaged about 0.551 us. These values are machine-specific and are
recorded only as the first regression-comparison baseline; correctness and resource-budget gates remain the
portable pass/fail criteria.

## Asynchronous UI ordering gate — 2026-08-24

Background candidate generation intentionally avoids reading `Plater` from the worker thread, but that also
defers workspace-revision validation until control returns to the UI thread. The panel now schedules an
immediate UI-thread revision refresh when background work finishes. The callback is guarded by a wx weak
reference, so closing or destroying the workbench cannot leave a live panel callback behind.

Presenter publications carry a monotonically increasing sequence. A delayed older dispatch is discarded
instead of replacing a newer workflow view. The contract test runs all four preflight publications in reverse
dispatch order; before the fix the final view regressed from `preflight_complete` to `ready_to_start`, and after
the fix only the newest snapshot is published.

### Windows verification

- Presenter focus: 1 test case, 4 assertions, all passed.
- Smart-slicing suite: 75 test cases, 463 assertions passed; the opt-in benchmark remains skipped by default.
- Full Release `slic3rutils_tests`: 85 test cases, 570 assertions, all passed.
- Full RelWithDebInfo `slic3rutils_tests`: 85 test cases, 570 assertions, all passed.
- Release and RelWithDebInfo `OrcaSlicer` plus `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and
  LNK4098 warnings are unchanged.
- GUI smoke used only `build-p0/src/Release/orca-slicer.exe`, the existing workspace-local
  `build-p0/smart-slicing-gui-smoke-data`, and `tests/data/test_3mf/Geräte/Büchse.3mf`. Preflight and isolated
  candidate trial slicing completed, the final view remained `candidates_ready`, the native-validation warning
  resolved after the baseline trial, and baseline plus the recommended orientation candidate were visible.
  `确认并应用` was not clicked, so no formal model/config/slice mutation was performed.
- Workspace-local PID 37776 was path-verified and stopped after the smoke. Unrelated integration PIDs 35252
  and 39428 were neither targeted nor stopped.

This gate changes no shared `MainFrame`, `Plater`, or CMake file; no configuration, dependency, port, runtime
journal path, persistent data directory, 3MF/profile format, profile data, or default Orca behavior changes.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Crash-resilient runtime journal publication gate — 2026-08-24

Runtime metadata publication now keeps two complete same-directory generations. A save first writes and closes
the bounded `.tmp` payload, atomically renames the prior primary to `.bak`, then renames the complete temporary
generation to the primary path. If publication fails, the adapter attempts to restore the prior primary before
reporting failure. Loading falls back to a valid backup when an interrupted publication left the primary absent
or unreadable, and terminal cleanup removes primary, temporary, and backup generations.

The recovery contract simulates the precise interruption window with a valid `.bak` and partial `.tmp`, verifies
the prior workflow summary is recovered, then performs repeated saves and proves the newest primary wins without
leaving side generations. Before the implementation, recovery was empty and cleanup left the backup behind.

### Windows verification

- Interrupted-publication focus: 1 test case, 10 assertions, all passed in Release and RelWithDebInfo.
- Runtime suite: 9 test cases, 64 assertions, all passed.
- Smart-slicing suite excluding the opt-in benchmark: 79 test cases, 503 assertions, all passed in Release and
  RelWithDebInfo.
- Full `slic3rutils_tests`: 90 test cases, 610 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this adapter-only change has no visual, interaction, candidate, or formal
  workspace-write behavior; prior workspace-local isolated GUI evidence remains applicable.

The scoped primary journal path and bounded JSON v1 payload are unchanged. `.tmp` and `.bak` are private,
transient siblings in the same isolated `datadir/cache` and never enter 3MF/profile data. This gate changes no
shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration, dependency, port,
persistent schema, profile/default data, or ordinary Orca behavior. macOS and Linux native build/test execution
remains a separate host/CI gate.

## Multi-instance runtime journal isolation gate — 2026-08-24

The recovery journal is now scoped to both the active Orca data directory and executable instance:
`<datadir>/cache/OrcaSlicer-smart-slicing-runtime-v1-<instance-token>.json`. This prevents independent
branches or installations from restoring, overwriting, or clearing one another's smart-slicing recovery
state. The executable-instance value is reduced to a fixed-width FNV-1a token, so raw executable identity
cannot become a path component or leak through the filename. The existing bounded `v1` journal payload and
recovery semantics are unchanged.

The former global temporary journal is deliberately neither migrated nor deleted: it may belong to another
running branch. New workbench sessions no longer read or write it.

### Windows verification

- The path-isolation contract: 1 test case, 6 assertions, all passed. It covers data-directory isolation,
  executable-instance isolation, cache placement, JSON extension, and raw-instance-name suppression.
- Runtime focus: 8 test cases, 54 assertions, all passed.
- Full Release `slic3rutils_tests`: 86 test cases, 576 assertions, all passed.
- Full RelWithDebInfo `slic3rutils_tests`: 86 test cases, 576 assertions, all passed.
- Release and RelWithDebInfo `OrcaSlicer` plus `OrcaSlicer_app_gui` built successfully. The existing LNK4075,
  LNK4098, and non-failing empty-working-directory `info/nozzle_info.json` warnings are unchanged.
- GUI smoke used only `build-p0/src/Release/orca-slicer.exe`,
  `build-p0/smart-slicing-gui-smoke-data`, and `tests/data/test_3mf/Geräte/Büchse.3mf`. Starting preflight
  created `cache/OrcaSlicer-smart-slicing-runtime-v1-5ed96e69b158ee23.json` inside that isolated data
  directory. The legacy global temporary journal retained its earlier timestamp, proving this session did
  not rewrite it. `确认并应用` was not clicked.
- Workspace-local PID 31324 was executable-path verified and stopped. Unrelated integration PIDs 35252 and
  39428 were neither targeted nor stopped.

This gate intentionally changes only the runtime-journal data location. It changes no configuration schema,
dependency, network port, journal payload schema, 3MF/profile format, profile data, or default Orca slicing
behavior. It changes no shared `MainFrame`, `Plater`, or CMake file. macOS and Linux native build/test execution
remains a separate host/CI gate.

## Deterministic bed-adhesion evidence gate — 2026-08-24

The stability goal now compares bed adhesion with explicit evidence instead of allowing lower time/material
cost to make a fragile baseline win automatically. Each isolated trial candidate records a geometry risk score
from its transformed printable-instance dimensions: the maximum of the small-footprint ratio and the
height-to-minimum-footprint ratio. A score of 1.0 is the deterministic attention threshold used by the local
advisor. The candidate also records actual `erBrim` material volume from the exported trial G-code statistics.

Slice warnings remain the first stability gate. With equal warnings, lower geometry risk wins; only while at
least one candidate is at or above the attention threshold does additional real brim volume act as positive
stability evidence. The remaining time, material, support, multicolor, and deterministic-ID tie breakers are
unchanged. Tests also lock the inverse case: adding brim to a low-risk model does not beat a cheaper baseline.

The local advisor now reads the effective plate `brim_type`. If Orca native `auto_brim` is already active it
emits no duplicate proposal. For high-risk geometry with `no_brim`, it creates a typed, bounded plate patch from
`no_brim` to `auto_brim`; supported manual outer-brim policies retain the existing bounded width proposal. The
native config adapter revalidates the enum and expected value on the isolated trial config and again in the
official gateway before any confirmed write.

### Windows verification

- Bed-adhesion comparison focus: 1 test case, 7 assertions, all passed.
- Native auto-brim advisor focus: 1 test case, 6 assertions, all passed.
- Real isolated trial focus: 1 test case, 12 assertions, all passed; the result includes measured risk and brim
  volume while the formal model/config remain unchanged.
- Smart-slicing suite excluding the opt-in benchmark: 77 test cases, 486 assertions, all passed.
- Full Release `slic3rutils_tests`: 88 test cases, 596 assertions, all passed.
- Full RelWithDebInfo `slic3rutils_tests`: 88 test cases, 596 assertions, all passed.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke used only `build-p0/src/Release/orca-slicer.exe`, the isolated
  `build-p0/smart-slicing-gui-smoke-data`, and `tests/data/test_3mf/Geräte/Büchse.3mf`. Candidate cards showed
  baseline risk 1.50 and recommended-orientation risk 0.40, with actual brim volume 0.00 cm³ for both. This
  proves the recommendation came from safer geometry rather than added material. `确认并应用` was not clicked.
- Workspace-local PID 45232 was executable-path verified and stopped. Unrelated integration PIDs 35252 and
  39428 were neither targeted nor stopped.

This gate adds a possible user-confirmed plate-level `brim_type: no_brim → auto_brim` candidate, but changes no
configuration schema, profile/default value, dependency, port, data directory, runtime journal schema,
3MF/profile format, or ordinary Orca behavior. It changes no shared `MainFrame`, `Plater`, or CMake file.

## Candidate metric validity gate — 2026-08-24

Candidate ranking now rejects a ready candidate when any measured time, volume, or bed-adhesion value is
negative or non-finite. It also validates the derived total-material value so finite inputs whose sum overflows
cannot enter the comparator. This keeps ordering deterministic and prevents corrupt trial evidence from winning
by exploiting NaN or infinity comparison behavior. An absent optional metric remains admissible and continues
to use the existing explicit missing-evidence reporting.

The contract covers estimated time, filament, support, brim, flush and wipe-tower volume, bed-adhesion risk,
derived-total overflow, and reversed candidate input order. Before the gate, the infinite-risk candidate was
recommended and all four original assertions failed; after the gate, the valid candidate wins and every invalid
candidate is excluded in stable identifier order.

### Windows verification

- Metric-validity focus: 1 test case, 4 assertions, all passed.
- Smart-slicing suite excluding the opt-in benchmark: 78 test cases, 493 assertions, all passed in Release and
  RelWithDebInfo.
- Full `slic3rutils_tests`: 89 test cases, 600 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a Domain-only, nonvisual comparison gate with no interaction or
  formal-write-path change; the preceding workspace-local isolated GUI evidence remains applicable.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Duplicate placement-target gate — 2026-08-24

Isolated trial placement now rejects a candidate that contains more than one transform for the same instance.
This matches the existing formal gateway rule and keeps candidate evaluation deterministic: a target has one
declared final transform, rather than order-dependent repeated mutations. Invalid candidates fail before Orca
creates or processes a trial `Print`, retaining the stable `invalid_candidate_placement` diagnostic.

Before the gate, two identical transforms for one instance completed a real isolated trial slice successfully,
while the formal gateway would later reject the same selected candidate as `duplicate_transform_target`. The
focused contract reproduced that trial/apply rule drift, with a successful result and empty diagnostic before the
fix.

### Windows verification

- Duplicate-target focus: 1 test case, 2 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 94 test cases, 602 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 105 test cases, 709 assertions, all passed in Release and RelWithDebInfo; the benchmark
  case remained explicitly skipped.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because this is a nonvisual trial-validation correction with no interaction or
  formal-write-path change; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It only aligns isolated trial validation with the existing formal apply contract. macOS and Linux native
build/test execution remains a separate host/CI gate.

## Placement matrix-validity gate — 2026-08-24

Isolated trial and formal transactional apply now use one Orca validator for transform finiteness, affine
homogeneous form, and nonsingular linear geometry. The shared determinant rule requires an absolute determinant
strictly greater than `1e-12`; both boundaries therefore reject the threshold value instead of disagreeing at
exact equality. The existing geometry-semantics validator remains a separate second gate for scale, mirror, and
shear preservation.

Before the gate, trial validation rejected `abs(determinant) <= 1e-12` while the formal gateway rejected only
values below the threshold, and both duplicated finite/affine checks. The focused contract first failed to compile
without a shared function, then verified a valid affine transform plus NaN, non-affine last-row, and determinant
threshold rejection.

### Windows verification

- Matrix-validity focus: 1 test case, 4 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 95 test cases, 606 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 106 test cases, 713 assertions, all passed in Release and RelWithDebInfo; the benchmark
  case remained explicitly skipped.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual shared validation gate with no new interaction; prior
  workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Plate-lock placement-scope gate — 2026-08-24

The formal transactional gateway now treats the native plate lock as a placement constraint instead of a slicing
ban. A locked current plate rejects candidates that contain one or more placement transforms, while a baseline or
parameter-only candidate with no transforms continues through the existing official slicing and typed-config
path. Placement and orientation providers already emit no transforms for a locked plate, and the gateway remains
the final guard against a forged or stale placement candidate.

Before the gate, `OrcaOfficialSliceGateway` rejected every candidate on a locked plate, including an unchanged
baseline and a parameter-only alternative. This was stricter than Orca's native behavior, where plate locking is
consulted by arrange, orient, and movement operations but does not disable ordinary slicing. The focused contract
first failed to compile without a shared placement-scope rule, then verified unlocked placement, locked unchanged,
and locked placement cases.

### Windows verification

- Plate-lock placement-scope focus: 1 test case, 3 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 97 test cases, 609 assertions; 96 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 108 test cases, 716 assertions; 107 passed and the benchmark case remained explicitly
  skipped in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual formal gateway rule with no new interaction; prior
  workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It only restores native slicing behavior for locked plates when no placement mutation is requested. macOS and
Linux native build/test execution remains a separate host/CI gate.

## Trial multicolor evidence-availability gate — 2026-08-24

Isolated trial metrics now preserve the workflow snapshot's physical-slot compatibility evidence. Compatible and
single-material/not-applicable contexts become explicit `true`, incompatible and invalid-temperature contexts
become explicit `false`, and unavailable evidence remains unavailable. The same session input carries the captured
color-mapping degradation flag; generated G-code mapping evidence can only retain or worsen that flag, never erase
an already detected degradation.

Before the gate, every successful trial unconditionally set `physical_slots_compatible` to `true`, even when
preflight could not establish compatibility. That converted missing evidence into a fabricated success and made
candidate cards/comparison data disagree with the immutable workspace context. The focused contract first failed
to compile without the explicit compatibility mapper, then verified all five domain states. The real isolated
slice contract also verifies that adapter-less input keeps both compatibility and mapping evidence unavailable.

### Windows verification

- Multicolor evidence-availability focus: 1 test case, 5 assertions, all passed in Release and RelWithDebInfo.
- Real isolated ownership/evidence focus: 1 test case, 14 assertions, all passed in Release.
- Smart-slicing suite excluding the opt-in benchmark: 98 test cases, 616 assertions; 97 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 109 test cases, 723 assertions; 108 passed and the benchmark case remained explicitly
  skipped in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because this is a nonvisual evidence-propagation correction; prior workspace-local
  isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Post-apply Undo ownership gate — 2026-08-24

The official gateway now binds its one-click recovery action to the exact workspace revision captured after the
smart-slicing mutation and official-slice start attempt. Before calling Orca's native Undo, it re-reads the current
revision and requires an exact match. A later model, config, or plate edit therefore invalidates only the smart
recovery shortcut instead of allowing that shortcut to undo the user's newer action. If ownership cannot be
verified, Application disables the recovery entry after the first safe refusal and exposes the stable
`apply_undo_unavailable` detail.

Before the gate, `undo_last_apply()` checked only a boolean left by the original commit. The focused gateway
contract changed the workspace revision after a failed official slice and observed the native Undo callback being
called. The focused coordinator contract also observed `can_undo_apply` remaining true after a gateway refusal.
Both behaviors are now rejected before any Undo mutation.

### Windows verification

- Post-apply Undo ownership focus: 2 test cases, 13 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 100 test cases, 629 assertions; 99 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 111 test cases, 736 assertions; 110 passed and the benchmark case remained explicitly
  skipped in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because this is a nonvisual recovery-ownership gate; prior workspace-local
  isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Legacy summary projection gate — 2026-08-24

The right-side smart-slicing workbench and the compatibility Sidebar now use one complete localized summary
mapper. Candidate preparation, trial slicing, candidate application, and official slicing therefore retain their
actual workflow summary when projected into the legacy Sidebar. Previously that projection had a separate partial
mapping, so those phases could display the generic printability-preflight message even while their step states
were correct.

The regression contract first failed to compile because no shared summary mapper existed. It now verifies that
candidate-ready, applying, and official-slicing summaries do not collapse to either the preflight text or the
unknown-key fallback.

### Windows verification

- Legacy summary projection focus: 1 test case, 4 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 101 test cases, 633 assertions; 100 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 112 test cases, 740 assertions; 111 passed and the benchmark case remained explicitly
  skipped in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because this is a nonvisual text-projection correction with no layout or interaction
  change; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes only smart-slicing GUI projection code and its tests. It changes no shared `MainFrame`, `Plater`,
or CMake file and no model-generation code, configuration, dependency, port, data directory, journal path/schema,
3MF/profile format, profile data, or default Orca behavior. macOS and Linux native build/test execution remains a
separate host/CI gate.

## Apply recovery summary accuracy gate — 2026-08-24

The workbench now promises one-click recovery only when the official gateway actually reports an owned native
Undo action. A pre-commit compatibility rejection, unavailable revision, or other failure without recovery uses a
neutral application/slicing failure summary and asks the user to inspect the project state. Failures after a
mutation that retain an owned Undo action continue to expose the existing recovery summary and button.

The focused projection contract first observed both failure classes producing `official_slice_failed`; it now
requires a distinct `official_slice_failed_no_recovery` summary when `can_undo_apply` is false.

### Windows verification

- Apply recovery summary focus: 1 test case, 5 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 102 test cases, 638 assertions; 101 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 113 test cases, 745 assertions; 112 passed and the benchmark case remained explicitly
  skipped in the final sequential Release and RelWithDebInfo matrix.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- One earlier six-process concurrent test launch produced a non-reproducible RelWithDebInfo full-suite failure;
  the immediate isolated rerun and the complete sequential matrix passed. The recorded release gate therefore
  uses sequential test-process execution.
- GUI smoke was not repeated because this changes only state-derived failure text and no control layout or formal
  write behavior; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes only smart-slicing GUI projection code and its tests. It changes no shared `MainFrame`, `Plater`,
or CMake file and no model-generation code, configuration, dependency, port, data directory, journal path/schema,
3MF/profile format, profile data, or default Orca behavior. macOS and Linux native build/test execution remains a
separate host/CI gate.

## Candidate failure explanation gate — 2026-08-24

Failed candidate cards now consume their existing structured `diagnostic_code` instead of showing one generic
trial-failure sentence. The localized projection distinguishes timeout, memory and temporary-disk budgets,
cancellation, unavailable workspace revision, invalid placement or printable objects, forbidden parameters,
invalid metrics, native validation, stale/mismatched results, and isolated executor failures. Unknown diagnostics
degrade to a bounded generic reason and never expose raw internal codes to the user.

The focused GUI projection contract first failed to compile because no candidate-failure text mapper existed. It
now verifies that the major resource, revision, and placement classes do not collapse to the unknown fallback.

### Windows verification

- Candidate failure diagnostic focus: 1 test case, 5 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 103 test cases, 643 assertions; 102 passed and the benchmark
  case remained explicitly skipped in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 114 test cases, 750 assertions; 113 passed and the benchmark case remained explicitly
  skipped in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because candidate reason text already uses the existing wrapped label and no control,
  layout, interaction, or formal write behavior changed; prior workspace-local isolated-data-directory GUI
  evidence remains valid.

This gate changes only smart-slicing GUI projection code and its tests. It changes no shared `MainFrame`, `Plater`,
or CMake file and no model-generation code, configuration, dependency, port, data directory, journal path/schema,
3MF/profile format, profile data, or default Orca behavior. macOS and Linux native build/test execution remains a
separate host/CI gate.

## Candidate retry exception-containment gate — 2026-08-24

Candidate selection and single-candidate retry now contain workspace-revision and trial-executor exceptions at
the Application boundary. A temporarily unavailable revision prevents selection without changing the current
selection or comparison. A retry executor exception, or an unavailable final revision after retry work finishes,
discards only that retry result, marks the alternative failed with a stable diagnostic, recomputes comparison,
and returns to `ReadyToApply` with the valid baseline retained. A real revision mismatch still invalidates the
workflow as stale; unavailable evidence is not misreported as a confirmed workspace edit.

Before the gate, all three contracts propagated exceptions out of `SmartSlicingCoordinator`; the GUI worker's
outer safety net would then cancel and clear the complete candidate set. The focused contract now locks baseline
retention, selected-candidate retention, metric discard, deterministic recomparison, and the
`candidate_selection_revision_unavailable`, `retry_executor_exception`, and `retry_revision_unavailable`
diagnostics.

### Windows verification

- Candidate-failure focus: 3 test cases, 25 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 83 test cases, 532 assertions, all passed in Release and
  RelWithDebInfo.
- Full `slic3rutils_tests`: 94 test cases, 639 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI fault injection was not repeated because the changed behavior is an Application-only exception contract;
  candidate projection is covered by the same test target, and the current workspace-local ordinary-slice GUI
  verification remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, formal apply
gateway, configuration, dependency, port, data directory, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Bed-adhesion derived-metric degradation gate — 2026-08-24

Bed-adhesion extraction now discards a non-finite derived risk even when all source dimensions are finite. This
can occur when a positive footprint is small enough that the bounded formula division overflows. The invalid
instance no longer contaminates other valid instances: a mixed input retains the maximum finite risk, while an
all-invalid input reports the optional metric as unavailable and emits no brim proposal. Candidate comparison
therefore retains its strict invalid-metric gate without excluding an otherwise valid trial due to a derived
adapter overflow.

### Windows verification

- Derived-risk degradation focus: 1 test case, 4 assertions, all passed in Release and RelWithDebInfo. Before
  the fix, the single-instance score was infinity, infinity masked a valid mixed score of 1.5, and a false
  auto-brim proposal was emitted.
- Parameter suite: 9 test cases, 84 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 80 test cases, 507 assertions, all passed in Release and
  RelWithDebInfo.
- Full `slic3rutils_tests`: 91 test cases, 614 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual metric-degradation correction with no interaction or
  formal-write-path change; prior workspace-local isolated GUI evidence remains applicable.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Trial-executor exception-boundary gate — 2026-08-24

Trial execution failures are now represented as explicit failed results with the stable
`trial_slice_executor_exception` diagnostic instead of escaping the Application boundary. The cache wrapper
does not retain these failed results, and the coordinator independently contains an uncaught executor exception
as defense in depth. An alternative failure therefore leaves a successful baseline available, selected, and
ready to apply; a baseline failure continues to fail the workflow without exposing an infrastructure exception.

Cancellation signaling is now best effort at both the caching executor and coordinator boundaries. Even if an
adapter throws while receiving the signal, candidate and comparison state is cleared and the workflow reaches
the deterministic `Canceled` terminal state. Before this gate, the three focused contracts all failed: execution
escaped the cache, an alternative exception failed the entire workflow, and a cancel exception left the workflow
in `Failed` with candidate state retained.

### Windows verification

- Exception-boundary focus: 3 test cases, 24 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 86 test cases, 556 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 97 test cases, 663 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI fault injection was not repeated because this gate changes only nonvisual Application exception handling;
  the prior workspace-local, isolated-data-directory ordinary-slice GUI verification remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, formal apply
gateway, configuration, dependency, port, data directory, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Observer exception-isolation gate — 2026-08-24

Coordinator observers are now a best-effort presentation channel: failures during initial publication or later
state transitions cannot escape an Application API, change workflow state, or suppress runtime-journal
publication. Notification remains synchronous and in the same order, so existing reentrant cancellation behavior
is preserved; only observer control over the workflow through an exception is removed.

Before the gate, an initial observer exception escaped `set_observer`, while a transition observer exception
prevented workspace capture, caused a false `Failed` state, escaped `start`, and skipped the corresponding journal
save. The focused contracts now cover both immediate and transition publication plus the final recoverable runtime
record.

### Windows verification

- Observer exception focus: 2 test cases, 12 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 88 test cases, 568 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 99 test cases, 675 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI fault injection was not repeated because this is a nonvisual Application notification contract; the prior
  workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, formal apply
gateway, configuration, dependency, port, data directory, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Workflow identifier recovery-boundary gate — 2026-08-24

Runtime recovery now rejects and clears the reserved workflow identifier zero. The coordinator also advances
explicitly from the maximum unsigned identifier to one, rather than relying on overflowing increment behavior.
Every started workflow therefore has a nonzero identifier and remains eligible for runtime-journal publication,
even when the prior recovered record contains the maximum representable value.

Before the gate, a zero identifier was accepted as a recoverable summary. A maximum identifier then caused the
next start to emit zero, and `persist_runtime_state` intentionally skipped that workflow, silently removing crash
recovery coverage. The contract verifies both the rejected-zero path and the maximum-to-one path through the
actual in-memory runtime-store port.

### Windows verification

- Workflow-ID boundary focus: 1 test case, 10 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 89 test cases, 578 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 100 test cases, 685 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual Application recovery boundary with no interaction or
  formal-write-path change; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, formal apply
gateway, configuration, dependency, port, data directory, journal field/schema/path, 3MF/profile format, profile
data, or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Placement geometry-semantics gate — 2026-08-24

Placement candidates can now change only position and orientation. A shared Orca adapter validator compares the
current and requested linear-transform Gram matrices and determinant signs before both isolated trial slicing and
formal transactional apply. This rejects candidate-introduced scaling, mirroring, and shear while still allowing
translation and rotation over an instance that already has non-uniform scale, mirroring, or shear from the user's
project.

Before the gate, both a doubled-axis candidate and a newly mirrored candidate completed a real isolated slice as
successful placement alternatives. The focused contract now verifies their rejection with the existing stable
`invalid_candidate_placement` diagnostic and verifies that a rigid move preserves pre-existing complex geometry
semantics. The formal gateway consumes the exact same pure validator, preventing trial/apply rule drift.

### Windows verification

- Placement geometry-boundary focus: 1 test case, 5 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 90 test cases, 583 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 101 test cases, 690 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual adapter validation gate with no new interaction; prior
  workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It narrows the existing formal gateway to the documented placement-only semantics. macOS and Linux native
build/test execution remains a separate host/CI gate.

## Unprintable placement-target gate — 2026-08-24

Placement and orientation providers now treat an object-level `printable == false` marker exactly like an
instance-level marker: the target remains fixed and is never emitted in a candidate transform. A shared Orca
adapter eligibility check requires both the owning object and the instance to be printable. Isolated trial apply
and the formal transactional gateway enforce the same rule, so a forged or stale candidate cannot move an
excluded formal object. Trial rejection retains the stable `invalid_candidate_placement` diagnostic; the formal
gateway reports `transform_target_not_printable` before any workspace mutation.

Before the gate, the native placement and orientation providers each emitted a transform for an object-level
unprintable target, and isolated trial execution accepted such a transform. The three focused contracts failed
at those exact boundaries before the shared eligibility rule was connected.

### Windows verification

- Placement-provider focus: 5 test cases, 17 assertions, all passed in Release and RelWithDebInfo.
- Orientation-provider focus: 2 test cases, 12 assertions, all passed in Release and RelWithDebInfo.
- Trial target-eligibility focus: 1 test case, 2 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 91 test cases, 586 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 102 test cases, 693 assertions, all passed in Release and RelWithDebInfo; the benchmark
  case remained explicitly skipped.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual Orca adapter safety gate with no new interaction; prior
  workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It only narrows candidate generation and apply eligibility for targets already marked unprintable. macOS and
Linux native build/test execution remains a separate host/CI gate.

## Printable parameter-evidence gate — 2026-08-24

Bed-adhesion parameter advice and post-trial evidence now derive instance geometry through one shared Orca
function. The function returns evidence only when both the owning object and the instance are printable. Candidate
planning also retains its current-plate containment check, so excluded objects cannot influence an automatic Brim
proposal and off-plate objects cannot enter the advice input. Trial metrics use the same object/instance rule,
preventing planning and measured evidence from drifting.

Before the gate, candidate planning checked only `ModelInstance::printable`; a slender object marked
`ModelObject::printable == false` could therefore trigger an otherwise valid Brim alternative even though Orca's
trial print excluded that object. The focused contract first failed to compile because the shared evidence
function did not exist, then verified printable dimensions plus object-, instance-, and null-target exclusion.

### Windows verification

- Printable-geometry eligibility focus: 1 test case, 8 assertions, all passed in Release and RelWithDebInfo.
- Parameter suite: 10 test cases, 92 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 92 test cases, 594 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 103 test cases, 701 assertions, all passed in Release and RelWithDebInfo; the benchmark
  case remained explicitly skipped.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke was not repeated because this is a nonvisual Orca evidence-selection correction with no interaction
  or formal-write-path change; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It only removes non-printing geometry from advisory evidence. macOS and Linux native build/test execution remains
a separate host/CI gate.

## Trial-execution serialization gate — 2026-08-24

`OrcaTrialSliceExecutor` now serializes its complete execution boundary with one mutex. Even if a caller outside
the current sequential Application workflow issues overlapping requests, only one request can own an active Orca
`Print`, input snapshot, deadline, temporary G-code lifecycle, and cancellation target at a time. Cancellation
remains independent of the execution mutex, so it can still interrupt the active print while another request is
waiting.

Before the gate, two calls entered the input provider concurrently and could replace `m_active_print`; a cancel
signal would then target only the most recently registered Print. The focused two-thread contract held the first
request at the provider boundary and observed the second entering concurrently (`entered_concurrently == true`).
After the gate, the second request does not enter until the first has completed and both requests terminate
normally.

### Windows verification

- Trial concurrency focus: 1 test case, 6 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 93 test cases, 600 assertions, all passed in Release and
  RelWithDebInfo; the benchmark case remained explicitly skipped.
- Full `slic3rutils_tests`: 104 test cases, 707 assertions, all passed in Release and RelWithDebInfo; the benchmark
  case remained explicitly skipped.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075, LNK4098, and the
  non-failing empty-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual Orca execution-ownership gate with no interaction or
  formal-write-path change; prior workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Multicolor comparison-eligibility gate — 2026-08-24

Candidate comparison eligibility is now enforced at every command boundary. A Ready candidate excluded because
its physical slots are incompatible or its color-to-slot mapping is degraded cannot be selected, retained after a
retry, or passed to formal transactional apply. The presentation model exposes the same exclusion state and a
stable reason code, disables the selector, and explains why the candidate is unavailable. Candidate cards also
show physical-slot compatibility, color-mapping integrity, and prime-tower policy alongside the existing trial
metrics.

Before the gate, comparison correctly excluded these multicolor candidates from ranking, but the coordinator and
GUI still treated every non-failed Ready candidate as selectable. A manually selected excluded candidate could
therefore remain the selected candidate and reach `OfficialSliceGateway`. The focused regression first failed to
compile because the exclusion fields did not exist, then verified both physical-slot and color-mapping rejection
through the comparison, ViewModel, selection command, and retained baseline selection.

### Windows verification

- Multicolor eligibility focus: 1 test case, 16 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite excluding the opt-in benchmark: 104 test cases, 659 assertions; 103 passed and the benchmark
  case remained explicitly skipped, in Release and RelWithDebInfo.
- Full `slic3rutils_tests`: 115 test cases, 766 assertions; 114 passed and the benchmark case remained explicitly
  skipped, in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings are
  unchanged.
- GUI smoke used only
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe` with isolated data directory
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-gui-smoke-data`. The restored disposable test project
  completed preflight and isolated candidate slicing; both baseline and recommended cards visibly reported
  physical slots compatible, color mapping preserved, and prime-tower policy enabled. The UI retained the
  confirmation boundary, `确认并应用` was not clicked, and the verified workspace-local PID was stopped afterward.
  The source 3MF remained unchanged.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data, or default Orca
behavior. It adds only comparison-safety enforcement and evidence presentation inside the smart-slicing modules.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Candidate-retry resource-budget gate — 2026-08-24

Failed-candidate retry now reuses the Application workflow's elapsed-time, memory, temporary-disk, and candidate
count budget check after the final workspace-revision guard and before the trial executor is called. A budget
violation leaves the failed alternative unavailable, retains the comparable baseline and ReadyToApply state, and
publishes the existing stable budget diagnostic instead of starting more isolated slicing work.

Before the gate, the initial candidate loop enforced the workflow budget but `retry_candidate` bypassed it. The
focused regression raised measured memory above the configured limit after the first comparison; the retry still
called the executor, accepted the alternative, and reported `candidate_retry_succeeded`. After the gate, the
executor call count remains unchanged and the baseline remains selected with
`workflow_memory_budget_exceeded` attached to the failed alternative.

### Windows verification

- Candidate-retry budget focus: 1 test case, 12 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 105 test cases, 671 assertions; 104 passed and the opt-in benchmark remained explicitly
  skipped, in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 115 test cases, 778 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a nonvisual Application resource-ownership correction with no new
  interaction or formal-write path. The immediately preceding workspace-local isolated-data-directory GUI
  evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Multicolor material-accounting normalization gate — 2026-08-24

The Orca trial adapter now normalizes native multicolor material statistics before publishing the Domain
`SlicingMetrics`. Orca's native total volume already includes flush and wipe-tower extrusion, while the Domain
contract tracks print material, flush, and wipe-tower waste separately and adds them when calculating total
material. The adapter therefore subtracts the explicit waste components from the native total instead of causing
the Domain total to count them twice.

The focused regression uses a native total of 650 mm3 with 100 mm3 of flush and 50 mm3 of wipe-tower material.
The normalized print-material value is 500 mm3, and the Domain total remains the original 650 mm3. Non-finite,
negative, or internally inconsistent native breakdowns now degrade the print-material metric to unavailable
instead of fabricating a comparable value. The regression first failed to compile because the normalization helper
did not exist, then passed after the adapter correction.

### Windows verification

- Material-accounting focus: 1 test case, 6 assertions, all passed in Release and RelWithDebInfo.
- Multicolor candidate focus: 5 test cases, 34 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 106 test cases, 677 assertions; 105 passed and the opt-in benchmark remained explicitly
  skipped, in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 116 test cases, 784 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is numeric adapter normalization with no interaction, layout, or formal
  write-path change. The immediately preceding workspace-local isolated-data-directory GUI evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
macOS and Linux native build/test execution remains a separate host/CI gate.

## Trial cancellation and timeout ownership gate — 2026-08-24

Application now distinguishes an explicit user cancellation from an Orca trial deadline that reports the same
`Canceled` transport status. A matched `workflow_timeout` is accepted as a failed candidate result: an alternative
timeout retains the comparable baseline and continues to ReadyToApply. Other matched canceled results are owned by
the workflow cancellation path, which clears candidates, comparison, and selection before entering the Canceled
terminal state. The same rules apply to initial candidate slicing and a failed-candidate retry.

Before the gate, an alternative timeout deleted the ready baseline and canceled the entire workflow, while an
explicit cancellation during retry left the workflow in ReadyToApply with `retry_canceled` and retained candidate
state. The two focused regressions first reproduced those opposite failures, then verified the intended baseline
retention and clean cancellation terminal state.

### Windows verification

- Cancellation/timeout focus: 2 test cases, 16 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 108 test cases, 693 assertions; 107 passed and the opt-in benchmark remained explicitly
  skipped, in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 118 test cases, 800 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke launched only
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe` with isolated data directory
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-gui-smoke-data` and the disposable `Büchse.3mf`
  fixture. The project loaded in Prepare. wx did not expose a reliable Windows UI Automation control tree, so no
  coordinate-based retry/cancel or formal-apply action was attempted. The exact workspace PID 46648 was verified
  and stopped, no workspace Orca process remained, and the source 3MF was unchanged. The coordinator focus tests
  are the acceptance evidence for the retry cancellation transition itself.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data, or default Orca
behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Unsupported RepairPlan capability gate — 2026-08-24

The current Orca trial and official-apply adapters now reject every candidate carrying a `RepairPlan` with the
stable `candidate_repair_unsupported` diagnostic. The isolated executor rejects before acquiring or slicing a
workspace copy. The official gateway rejects after its revision guard but before compatibility checks, formal
mutation, or official slicing, both for prepare and a defensive direct commit. The workbench maps the diagnostic
to an explicit message that this version cannot yet safely trial-slice or apply mesh repair.

Before this gate, Domain and ViewModel could carry and display repair operations, but neither Orca adapter executed
them. The red regressions showed a repair candidate completing a real isolated slice and the official gateway
calling compatibility, apply, and slice callbacks. That made the candidate evidence and formal result disagree
with its declared changes. Fail-closed behavior preserves candidate truthfulness until a future native,
color-preserving, transaction-owned repair capability is implemented explicitly.

### Windows verification

- Repair capability boundary focus: 2 test cases, 10 assertions, all passed in Release and RelWithDebInfo.
- Localized candidate diagnostic focus: 1 test case, 6 assertions, all passed in Release.
- Smart-slicing suite: 110 test cases, 704 assertions; 109 passed and the opt-in benchmark remained explicitly
  skipped, in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 120 test cases, 811 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because production candidate providers do not emit RepairPlan today, so the state
  cannot be constructed safely through the workbench. The focused gateway test proves compatibility, apply, and
  official-slice callbacks remain at zero; the immediately preceding workspace-local isolated-data-directory GUI
  evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca behavior.
It intentionally records native deterministic mesh repair as a remaining capability rather than claiming the
existing RepairPlan is executable. macOS and Linux native build/test execution remains a separate host/CI gate.

## Locked-plate trial/apply parity gate — 2026-08-24

The isolated Orca trial snapshot now carries the current plate lock state. A candidate containing placement
transforms on a locked plate is rejected before parameter patching, model-copy mutation, or native slicing with
the same stable `current_plate_locked` diagnostic used by `OfficialSliceGateway`. A locked plate still permits a
candidate without placement transforms, preserving parameter-only and unchanged baseline slicing behavior. The
smart-slicing panel maps the diagnostic to an explicit locked-plate explanation instead of the unknown-failure
fallback.

Before this gate, candidate providers normally suppressed placement proposals for a locked plate and the formal
gateway rejected any defensive or externally supplied transform, but the isolated trial input omitted the lock
fact. The red regression demonstrated that such a candidate completed a native trial slice successfully and
published metrics even though the identical candidate could not pass formal transactional apply. A second red
regression showed that the resulting parity diagnostic had no actionable GUI projection.

### Windows verification

- Locked-plate parity focus: 2 test cases, 6 assertions, all passed in Release and RelWithDebInfo.
- Localized candidate diagnostic focus: 1 test case, 7 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 111 test cases, 708 assertions; 110 passed and the opt-in benchmark remained explicitly
  skipped, in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 121 test cases, 815 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a non-layout snapshot/validation parity correction with no startup,
  interaction, or formal-write-path change. The immediately preceding workspace-local isolated-data-directory GUI
  evidence remains valid.

This gate changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, public port, data directory, journal path/schema, 3MF/profile format, profile data, or default Orca
behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Workbench comparison-goal selector — 2026-08-24

The workbench now exposes the four comparison goals already supported by Domain and Application: stability,
quality, speed, and material saving. The wx choice defaults to stability, preserves that default for an invalid
selection, and is enabled only while candidate planning is available and no worker is running. The selected goal
is captured on the GUI thread before candidate generation starts and passed into the isolated trial comparison;
background work never reads the control. This changes ranking and recommendation only, using the existing real
trial metrics. It introduces no candidate mutation field, parameter key, sidecar contract, or formal write path.

Before this change, the comparison engine and focused tests supported all four goals, but the workbench always
passed `CandidateGoal::Stability`, leaving quality, speed, and material-saving ranking unreachable from the GUI.
The red regression first failed to compile because the selection-to-goal mapping did not exist, then covered all
four valid indices plus the `wxNOT_FOUND` and out-of-range fallbacks after implementation.

### Windows verification

- Goal-selection focus: 1 test case, 6 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 112 test cases; 111 passed and the opt-in benchmark remained explicitly skipped, with 714
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 122 test cases, 821 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke launched only
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe` with isolated data directory
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-gui-smoke-data` and the repository `Büchse.3mf`
  fixture. The exact workspace process and window/menu ownership were verified before interaction. The workbench
  displayed the unclipped optimization-goal row with `稳定打印` selected by default. A read-only preflight reached
  the existing native-configuration-validator-unavailable diagnostic for this isolated fixture, so candidate
  generation, trial slicing, and formal apply were not invoked. The source fixture SHA-256 remained
  `DE20508DFF06F8FAF8CA992C00238D4AFFC916BA4B812E8FF9EC1571FEC533A1`, the workspace process closed, and no Orca
  process remained.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, configuration,
dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data, or default Orca
behavior. The isolated smoke data directory only retained local GUI test state. macOS and Linux native build/test
execution remains a separate host/CI gate.

## Dynamic workbench text wrapping gate — 2026-08-24

Dynamic summary, issue, candidate-metric, reason, change, and isolation-notice labels now retain the workbench's
bounded width after their text changes. These controls use `wxST_NO_AUTORESIZE`, and the shared label updater first
disables the previous wrap before applying the intended DIP width and invalidating the best size. The reset is
required because wxWidgets caches the last `Wrap()` width across `SetLabel()` calls and otherwise returns early
when the same width is requested again.

Before this gate, the read-only preflight evidence expanded its `wxStaticText` to roughly twice the panel width,
clipping the diagnostic horizontally and pushing the primary action label out of view. An initial implementation
that called only `SetLabel()` and `Wrap(width)` reproduced the defect. Resetting with `Wrap(-1)` before rewrapping
produced the green GUI evidence: the complete diagnostic occupied three visible lines, the panel stayed bounded,
and `生成并试切方案` and the enabled `稳定打印` goal selector remained visible.

### Windows verification

- Smart-slicing suite: 112 test cases; 111 passed and the opt-in benchmark remained explicitly skipped, with 714
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 122 test cases and 821 assertions, all passed in Release and RelWithDebInfo.
  The first RelWithDebInfo full run had one transient failure in the unrelated external HTTP Digest test with
  status zero; that test passed immediately in isolation and the subsequent full rerun passed completely.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI verification used only
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe`, isolated data directory
  `D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-gui-smoke-data`, and the repository `Büchse.3mf`
  fixture. The exact workspace process was verified before interaction. A read-only preflight exercised the
  dynamic issue label without generating candidates, trial slicing, or applying a candidate. The source fixture
  SHA-256 remained `DE20508DFF06F8FAF8CA992C00238D4AFFC916BA4B812E8FF9EC1571FEC533A1`; the process closed and no Orca
  process remained.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no workflow state, formal write path,
model-generation code, configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile
format, profile data, or default Orca behavior. The isolated smoke data directory only retained local GUI test
state. macOS and Linux native build/test execution remains a separate host/CI gate.

## Current-plate material completeness gate — 2026-08-24

Printability preflight now requires every logical filament referenced by the current plate to resolve to a
non-empty material preset. An unrelated valid preset can no longer hide a missing used preset or a dangling logical
filament reference. When a legacy workspace DTO has no explicit logical-filament list, `used_on_plate` flags remain
authoritative; only DTOs with neither form of usage evidence retain the previous any-non-empty compatibility
fallback. Empty presets that are not used on the current plate do not block preflight.

Before this gate, a red regression supplied one valid unused material and one empty used material. Preflight
reported no issue in both that case and a dangling logical-filament case. The green contract covers those failures,
the legacy usage-flag path, and the non-blocking unused-empty case with 11 focused assertions. An older multicolor
fixture also declared logical filament IDs 1 and 2 while leaving its material snapshot IDs at the default zero; the
fixture was corrected to represent the same valid production contract rather than weakening the new boundary.

### Windows verification

- Material-boundary focus: 1 test case, 11 assertions, all passed in Release.
- Smart-slicing suite: 113 test cases; 112 passed and the opt-in benchmark remained explicitly skipped, with 725
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 123 test cases and 832 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a Domain/Application preflight correction with no layout,
  interaction, adapter, startup, candidate/trial execution, or formal-write-path change. The immediately preceding
  workspace-local isolated-data-directory GUI evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, candidate contract,
configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Trial metric acceptance gate — 2026-08-24

Numeric trial measurements must now be finite and non-negative before a successful transport result can make a
candidate `Ready`. The same Domain predicate covers time, print material, support, brim, bed-adhesion risk, flush,
wipe-tower material, and overflow of their complete total-material sum; CandidateComparison reuses that predicate
instead of maintaining a second validation implementation. Missing optional measurements remain valid unavailable
evidence. Physical-slot incompatibility and degraded color mapping also retain their existing evidence-backed
comparison exclusions rather than being reclassified as executor failures.

Before this gate, an alternative carrying a NaN time was marked `Ready`, retained the invalid metrics without a
diagnostic, and could not enter the retry path. The red workflow regression failed six assertions. After the gate,
the result becomes a retryable failed candidate with `invalid_candidate_metrics`, a repeated invalid retry remains
failed, and the valid baseline remains selected. A separate contract proves an invalid baseline fails before any
comparison with `baseline_trial_failed` and retains no metrics.

### Windows verification

- Metric-validation focus: 4 test cases, 28 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 115 test cases; 114 passed and the opt-in benchmark remained explicitly skipped, with 745
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 125 test cases and 852 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a Domain/Application result-acceptance correction with no layout,
  interaction, startup, Orca adapter, or formal-write-path change. The immediately preceding workspace-local
  isolated-data-directory GUI evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, candidate DTO field,
configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Invalid trial metric cache-admission gate — 2026-08-24

The trial-slice cache now admits a successful, revision-matched result only when its numeric measurements satisfy
the shared Domain validity predicate. An invalid result still passes through unchanged so the Application layer can
produce its stable diagnostic, but it is not persisted in the cache. A subsequent retry therefore executes the
native trial slicer again and can recover; once a valid result is returned, later identical requests reuse it.

Before this gate, a transport-success result containing a NaN time entered the cache. The first Application-level
acceptance correctly rejected it, but the retry replayed the same invalid result without invoking the native
executor. The red cache regression had one test case and seven assertions, with three failures: the second result
remained invalid, the third result could not reuse a recovered measurement, and the delegate was called once rather
than twice. The green contract covers invalid pass-through, recovery through a second delegate execution, and valid
cache reuse on the third call.

### Windows verification

- Cache-admission focus: 1 test case and 7 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 116 test cases; 115 passed and the opt-in benchmark remained explicitly skipped, with 752
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 126 test cases and 859 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is an Application cache-admission correction with no layout, interaction,
  startup, Orca adapter, or formal-write-path change. The preceding workspace-local isolated-data-directory GUI
  evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, candidate DTO field,
configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format, profile data,
or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Official gateway prepare/commit transaction gate — 2026-08-24

`OrcaOfficialSliceGateway` now requires a one-use preparation token before any formal candidate mutation. The token
is bound to the Ready candidate ID, expected workspace revision, and an exact lightweight snapshot of executable
placement and parameter content. It is invalidated by a failed commit guard and consumed before compatibility
revalidation and apply. Commit repeats the compatibility check so candidate content cannot bypass the last
non-mutating validation boundary after prepare. While an official slice is pending, both a new prepare and a second
commit fail closed with `official_slice_in_progress` and leave the active transaction untouched.

Before this gate, direct `commit()` entered Slicing without `prepare()`, a Failed candidate could become Prepared,
and a second commit replaced the in-flight gateway state while invoking apply and slice again. The initial red
regression had one test case and 11 assertions, with eight failures and three apply/slice executions instead of one.
A second red step changed placement content while retaining the prepared ID and revision; it entered Slicing and
then blocked the original candidate from re-preparing, failing three assertions. The final 19-assertion contract
covers an unprepared commit, a non-Ready candidate, a token bound to a different candidate, same-ID executable
content substitution, compatibility revalidation, one successful mutation, and both prepare/commit overlap
rejection. Existing undo tests were also corrected to enter the same public two-phase transaction used by the
coordinator.

### Windows verification

- Transaction-boundary focus: 1 test case and 19 assertions, all passed in Release and RelWithDebInfo.
- Smart-slicing suite: 117 test cases; 116 passed and the opt-in benchmark remained explicitly skipped, with 774
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 127 test cases and 881 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is an Orca gateway transaction-state correction with no layout,
  interaction, startup, candidate generation, or trial-slice change. The preceding workspace-local
  isolated-data-directory GUI evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, Domain/Application
contract, configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format,
profile data, or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Native Undo refusal ownership gate — 2026-08-24

The Orca formal-apply gateway now owns failure containment and one-shot cleanup for native Undo. If the native Undo
callback reports unavailable or throws, the exception cannot leave the gateway, and the gateway consumes its
recovery capability by clearing the guarded revision and every outward `can_undo` flag. It retains
`workspace_mutated=true`, because a failed recovery must not claim that the formal workspace was restored. A later
call therefore returns false without invoking native Undo again. Successful Undo retains its existing behavior and
is the only path that clears the mutation fact.

Before this gate, a false callback left `poll().can_undo` true and a second call invoked native Undo again; an
exception escaped the gateway entirely. The two-input red regression executed 13 assertions before termination
and reported four failures across those paths. The final contract passes 16 assertions and aligns the real Orca
gateway with the Coordinator's existing rule that an unavailable recovery is disabled after one safe attempt.

### Windows verification

- Undo-ownership focus: 1 test case with 2 generated inputs and 16 assertions, all passed in Release and
  RelWithDebInfo.
- Smart-slicing suite: 118 test cases; 117 passed and the opt-in benchmark remained explicitly skipped, with 790
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 128 test cases and 897 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a gateway failure-containment correction with no layout, interaction,
  startup, candidate generation, trial slicing, or successful formal-apply-path change. The preceding
  workspace-local isolated-data-directory GUI evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, Domain/Application
contract, configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format,
profile data, or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.

## Failed-apply recovery priority gate — 2026-08-24

The Orca formal-apply gateway now rejects both prepare and commit while its last official transaction is Failed and
still owns a revision-guarded native Undo. The stable `apply_recovery_required` result preserves the original
mutation fact and recovery token without invoking compatibility, apply, or slice callbacks. After Undo succeeds, or
after the one safe recovery attempt is consumed as unavailable, a new transaction can be prepared normally. A
Completed transaction remains restartable even while its historical native Undo is still available; the gate is
scoped only to unresolved failure recovery.

Before this gate, prepare could replace the transaction context while recovery was pending, and a direct commit
reported `candidate_not_prepared` instead of expressing the recovery ownership conflict. The two-input red
regression had 22 assertions and three failures. The green contract verifies both entry points, one and only one
apply/slice attempt before recovery, retained `can_undo`, successful native recovery, and normal prepare afterward.

### Windows verification

- Recovery-priority focus: 1 test case with 2 generated inputs and 22 assertions, all passed in Release and
  RelWithDebInfo.
- Smart-slicing suite: 119 test cases; 118 passed and the opt-in benchmark remained explicitly skipped, with 812
  assertions passed in Release and RelWithDebInfo.
- Default full `slic3rutils_tests`: 129 test cases and 919 assertions, all passed in Release and RelWithDebInfo.
- Release and RelWithDebInfo `OrcaSlicer_app_gui` built successfully. Existing LNK4075 and LNK4098 warnings and
  the non-failing test-working-directory `info/nozzle_info.json` warning are unchanged.
- GUI smoke was not repeated because this is a defensive gateway state-priority correction with no layout,
  interaction, startup, candidate generation, trial slicing, or normal Coordinator-path change. The preceding
  workspace-local isolated-data-directory GUI evidence remains valid.

This batch changes no shared `MainFrame`, `Plater`, or CMake file and no model-generation code, Domain/Application
contract, configuration, dependency, port, data directory contract, journal path/schema, 3MF/profile format,
profile data, or default Orca behavior. macOS and Linux native build/test execution remains a separate host/CI gate.
