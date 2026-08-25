# Smart Slicing P2 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** After ADR-004 is accepted, add explainable goal-specific parameter candidates and constraint-preserving multicolor sequence candidates while retaining isolated trials, at most three comparable candidates, and one transactional official application.

**Architecture:** Domain owns intent, allowlists, combination rules, sequence invariants, and named lexicographic evidence. Application continues to plan and trial immutable candidates without wx or Orca types. Orca adapters capture and patch cloned plate/process configuration, run native trial slicing, and repeat every validation immediately before `OfficialSliceGateway` commits one Undo transaction. The first P2 increment remains Plate/Process only; Object/Process stays rejected until its separate stable-ID, object-config, revision, and undo contract is implemented.

**Tech Stack:** C++17, OrcaSlicer `Model`/`Print`/`DynamicPrintConfig`/`PartPlate`, wxWidgets, CMake, Catch2.

---

## Execution gate and non-goals

- **Current status: approved for execution.** ADR-004 was accepted by the product owner on 2026-08-25; execute Tasks 1-7 within its staged-scope boundaries.
- This plan does not change model-generation business code and does not consume code from `codex/orca-integration-v2`.
- Do not modify repository-root `task_plan.md`, `findings.md`, or `progress.md`.
- Do not add a weighted score, user weight sliders, direct flush tuning, material remapping, profile writes, or 3MF/profile schema changes.
- `ConfigScope::Object`, `Material`, and `Workspace`, plus Filament/Printer/Project owners, remain rejected in this increment. Do not silently reinterpret them as Plate/Process.
- Volume and layer-range targets require new explicit Domain target types in a future decision; do not overload `ConfigScope` or `target_id`.
- Keep the feature-off path byte-for-byte equivalent in behavior. Candidate and trial data remains runtime-only until confirmation.
- Keep MainFrame and Plater untouched. The only anticipated shared-file change is thin source registration in `src/slic3r/CMakeLists.txt` if the sequence DTO/validator is added as a new translation unit.

## First-increment policy to encode after acceptance

The implementation must encode the following as named, testable policy rather than dispersed conditionals:

- One `ParameterProposal` has exactly one `ParameterIntent`: `Stability`, `Quality`, `Speed`, or `MaterialSaving`.
- Maximum patch size remains four entries, all entries target the same plate, and all are Plate/Process.
- Stability may use bounded brim and support keys. Quality may use bounded layer height, walls/shells, seam, and support-interface keys. Speed may use bounded layer height or walls/shells. Material saving may use bounded support/brim keys; its first useful multicolor candidate is a separate tool-sequence proposal.
- A key allowed in more than one intent still has one shared type/range/delta rule. Intent decides whether that key is allowed for this proposal.
- Dependent native settings are atomic in the proposal. In the first increment, a non-empty other-layer sequence always carries both the flattened sequence and its range-count metadata through one typed sequence proposal; parameter patches may not write either raw key.
- Forbidden combinations include disabling support while also changing support-interface layers, setting `brim_type` to `no_brim` while increasing `brim_width`, and mixing conflicting speed/quality directions in one proposal.
- `first_layer_print_sequence`, `other_layers_print_sequence`, `other_layers_print_sequence_nums`, `flush_volumes_matrix`, `flush_multiplier`, `enable_prime_tower`, hardware, temperatures, flow ratio, pressure advance, and calibration values remain explicit forbidden parameter keys even if the active Orca build exposes them. Sequence changes use only the typed sequence proposal in Task 5.
- Quality UI/evidence may claim only a concrete validated change such as a finer effective layer height; it must not claim general surface quality improvement. Speed and material claims require real trial time/material evidence.
- A multicolor sequence candidate is eligible only when at least two used logical filaments exist, slot compatibility is known-compatible, mapping is not degraded, and all source sequences are valid permutations of the used IDs.
- Sequence generation may reorder logical filament execution only. It preserves the exact logical-ID set, logical-to-physical mapping, prime-tower state, layer ranges, object/material assignment, and every flushing value.

### Task 1: Add parameter intent and coherent-patch validation

**Files:**

- Modify: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposal.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_parameters.cpp`

**Steps:**

1. Add failing cases for a missing/mismatched intent, the complete owner/scope matrix, per-intent key allowlists, mixed target IDs, dependent keys, and each forbidden combination above.
2. Add a table-driven case proving raw sequence keys, direct flush matrix/multiplier, prime-tower disable, hardware, temperature, flow, pressure-advance, Filament, Printer, Project, Material, Object, and Workspace proposals stay rejected.
3. Run `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Parameters]"`; expect compile failures for the absent intent field and failed assertions for combination rules.
4. Add `ParameterIntent` to `ParameterProposal`, plus rejection codes for missing/mixed intent, mixed targets, missing dependency, and forbidden combination. Keep rejection strings stable and user-independent.
5. Refactor the current rule array into one Domain policy table containing type, range, maximum delta, and allowed intents. Do not add an Orca include.
6. Validate proposal-wide invariants only after entry-level type/range validation, so malformed variants are never inspected as another type.
7. Re-run the focused filter and the full smart-slicing filter; expect all cases to pass.
8. Commit as `feat(smart-slicing): validate parameter intent patches`.

### Task 2: Make local advice goal-aware without weakening fallback

**Files:**

- Modify: `src/slic3r/AI/SmartSlicing/Ports/IParameterAdvisor.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaParameterAdvisor.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaParameterAdvisor.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaCandidateProposalTask.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.cpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_parameters.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_candidates.cpp`

**Steps:**

1. Add failing tests proving the selected workbench goal reaches candidate proposal generation, local advice sets the matching intent, and invalid/empty advisor output leaves native deterministic candidates available.
2. Add characterization tests for the current stability brim behavior before changing its call signature.
3. Run the parameter and proposal-task filters; expect compile failures where the advisor/task lacks a goal input.
4. Pass `CandidateGoal` explicitly from the panel background command into `OrcaCandidateProposalTask::execute`; do not read wx state from the worker.
5. Extend `OrcaParameterAdvisorInput` only with copied primitive/native-config values needed by a bounded rule. Capture those values from the effective plate config in `OrcaSmartSlicingAdapter`.
6. Preserve the existing geometry-derived stability brim rule. Add at most one conservative local proposal for an eligible goal: speed may increase layer height within Domain/nozzle bounds; quality may decrease it and describe only the finer layer; material saving relies on Task 5 sequence candidates unless a bounded support/brim reduction is demonstrably eligible.
7. Ensure a goal with no safe local parameter rule returns an empty proposal rather than fabricating a recommendation.
8. Keep each alternative coherent: never attach a proposal of one intent to a candidate generated for another goal, never exceed two alternatives plus baseline, and retain deterministic IDs/order.
9. Re-run focused and full smart-slicing tests.
10. Commit as `feat(smart-slicing): generate goal-aware local parameter advice`.

### Task 3: Keep Domain, trial adapter, and official gateway validation identical

**Files:**

- Modify: `src/slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_parameters.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_workflow.cpp`

**Steps:**

1. Add adapter contract cases for unsupported native options, stale expected values in a multi-entry patch, native range rejection, and zero writes to the output clone on any rejected entry.
2. Add workflow/gateway cases proving a proposal accepted during trial is rejected before commit when intent, expected values, target plate, or workspace revision becomes stale.
3. Run the focused tests and confirm the new gateway/adapter cases fail before implementation where applicable.
4. Keep adapter application all-or-nothing by building a local working clone and assigning `patched_config` only after every entry passes.
5. Ensure trial slicing and `OfficialSliceGateway` call the same adapter and Domain validator; do not duplicate a weaker allowlist in either caller.
6. Confirm the gateway prepares the whole patch before `TakeSnapshot`/formal mutation. A rejected proposal must not create Undo, dirty state, invalidation, or official slice work.
7. Confirm one accepted mixed-entry proposal becomes one plate patch inside the existing single native Undo transaction.
8. Re-run parameter, apply, workflow, and full smart-slicing tests.
9. Commit as `fix(smart-slicing): enforce parameter validation parity`.

### Task 4: Encode goal-specific lexicographic comparison as named dimensions

**Files:**

- Modify: `src/slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/CandidateComparison.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_candidates.cpp`

**Steps:**

1. Add failing table-driven cases that pin the complete dimension order for Stability, Quality, Speed, and MaterialSaving, including a conflict where each goal must select a different candidate.
2. Add cases for missing optional metrics at every compared dimension. A present-vs-missing win must emit only `more_complete_trial_evidence`, never a magnitude claim.
3. Add cases proving incomplete trials, invalid numeric metrics, incompatible slots, and degraded mappings fail hard eligibility before ordering.
4. Run `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Candidate]"`; expect policy-order assertions to fail.
5. Represent each goal's lexicographic order as a named static policy. Reuse one comparison/evidence walker so ordering and explanation cannot drift apart.
6. Use only measured dimensions already captured by isolated trials. Keep warnings ahead of optional optimization costs; preserve the stability-only adhesion/brim direction rule.
7. Keep stable candidate identity as the final deterministic tie breaker and report `deterministic_tie_break`, not a quality claim.
8. Re-run candidate and full smart-slicing tests.
9. Commit as `refactor(smart-slicing): name goal comparison evidence`.

### Task 5: Add a typed, constraint-preserving multicolor tool-sequence candidate

**Files:**

- Create: `src/slic3r/AI/SmartSlicing/Domain/ToolSequenceProposal.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Domain/ToolSequenceProposalValidator.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Domain/ToolSequenceProposalValidator.cpp`
- Modify: `src/slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaToolSequenceCandidateProvider.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaCandidateProposalTask.hpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.cpp`
- Modify (thin registration only): `src/slic3r/CMakeLists.txt`
- Test: `tests/slic3rutils/test_smart_slicing_candidates.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_workflow.cpp`
- Test: `tests/fff_print/test_multifilament.cpp`

**Steps:**

1. Add Domain failures for duplicate/missing logical IDs, changed layer ranges, unknown IDs, changed physical mapping, changed prime-tower state, unavailable/incompatible slots, degraded mapping, and a no-op sequence.
2. Add provider cases proving generation occurs only for MaterialSaving with two or more used filaments and that identical input yields one stable candidate ID and byte-identical sequence DTO.
3. Define the deterministic alternative: for each valid layer sequence, rotate the same logical-ID permutation to continue from the preceding layer's final tool when possible; never add/remove/substitute an ID. Generate no candidate if the result is a no-op.
4. Run candidate tests; expect compile failures for the absent DTO/provider.
5. Implement the typed proposal with explicit expected/new first-layer sequence, explicit expected/new layer-range sequences, expected physical mapping, and expected prime-tower state. Do not encode vectors into `ConfigValue` and do not expose `PartPlate` types to Domain.
6. Apply the proposal to the trial's cloned effective config using Orca's native `first_layer_print_sequence`, `other_layers_print_sequence`, and `other_layers_print_sequence_nums` representation. Keep flush settings and mapping untouched.
7. Trial-slice the candidate and require returned mapping equality, compatible slots, unchanged prime-tower state, valid metrics, and no color-mapping degradation before it can be recommended.
8. Add focused `fff_print` coverage showing both baseline and reordered configs retain feature-to-filament assignment while tool order may differ.
9. In `OfficialSliceGateway`, re-read revision, current sequences, current mapping, and prime-tower state; validate all expected values before mutation. Apply `PartPlate::set_first_layer_print_sequence` and `set_other_layers_print_sequence` only inside the existing one-step Undo transaction.
10. Add failure-injection tests proving a rejected or partially prepared sequence creates no formal changes; one Undo restores the exact prior sequences after success.
11. Run:
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Candidate][Multicolor]"`
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Apply]"`
    - `build-p0/tests/fff_print/Release/fff_print_tests.exe "[MultiFilament]"`
12. Commit as `feat(smart-slicing): trial safe multicolor tool sequences`.

### Task 6: Project intent and evidence without adding hidden claims

**Files:**

- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.cpp`
- Modify: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_workflow.cpp`

**Steps:**

1. Add failing projection cases for all four intents, concrete parameter deltas, multicolor sequence preservation, incomplete evidence, and excluded unsafe candidates.
2. Map Domain codes to concise Chinese labels at the GUI boundary. Keep raw provider text out of the view model.
3. Show what changes, why it was proposed, measured benefit/cost, and preserved multicolor constraints. Do not render a total score or unsupported “quality improved”/“safer” claim.
4. Reuse the existing candidate cards and confirmation command; do not add a second apply path.
5. Verify keyboard focus and supported-size layout only if the new rows alter geometry.
6. Run workflow/view-model and full smart-slicing tests.
7. Commit as `feat(smart-slicing): explain P2 candidate intent`.

### Task 7: Regression, Release build, and workspace-local GUI acceptance

**Files:**

- Modify only if recording results: `Docs/plans/2026-08-25-smart-slicing-p2-implementation.md`
- Do not modify repository-root planning files.

**Steps:**

1. Run `git diff --check` and audit `git diff --name-only` against the allowed Smart Slicing/Orca/test boundary. Explain any shared-file change.
2. Run focused Release tests:
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Parameters]"`
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Candidate]"`
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing][Apply]"`
    - `build-p0/tests/fff_print/Release/fff_print_tests.exe "[MultiFilament]"`
3. Run the full Release smart-slicing and `slic3rutils` suites:
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe "[AI][SmartSlicing]"`
    - `build-p0/tests/slic3rutils/Release/slic3rutils_tests.exe`
4. Build both configurations:
    - `cmake --build build-p0 --config Release --target slic3rutils_tests fff_print_tests OrcaSlicer OrcaSlicer_app_gui -- -m`
    - `cmake --build build-p0 --config RelWithDebInfo --target slic3rutils_tests fff_print_tests OrcaSlicer OrcaSlicer_app_gui -- -m`
5. Repeat focused smart-slicing tests from `build-p0/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.exe`.
6. For GUI verification, launch only `D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe --datadir D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-p2-gui-data`. Resolve and verify the running process executable path before interaction. Do not interact with any other Orca process.
7. In disposable projects, verify stability/quality/speed/material goals, invalid-provider fallback, multicolor candidate preservation, cancel/stale rejection, one confirmation/one Undo, official Preview, and the ordinary feature-off Slice plate path.
8. Verify existing 3MF/profile loading and record that configuration, dependencies, ports, data directories, and formats did not change. The isolated smoke-test directory is test runtime data, not a product data-directory change.
9. Record Windows evidence honestly and leave macOS/Linux build/test as explicit CI gates unless actually run.
10. Commit verification notes, then ensure `git status --short` is empty.

## Deferred scope gates

The following are deliberately outside this first ADR-004 implementation and must remain rejected:

- **Object/Process:** requires a separate Domain object target, stable object ID resolution on trial and official paths, native object-config clone/patch support, revision hashing of object overrides, one Undo restoration test, and multi-object stale-target tests.
- **Volume/layer range:** requires explicit target DTOs and native range identity; it cannot use the scalar plate `target_id`.
- **Material/Filament:** requires physical-slot capability and calibration-readiness evidence plus persistence/undo policy.
- **Direct flushing/remapping:** requires a future ADR. P2 only searches safe orderings and measures the real result.
- **Remote sidecar transport:** `IParameterAdvisor` remains the typed boundary and local deterministic fallback is mandatory. Adding an endpoint, credentials, network payload, or provider-specific dependency requires a separately reviewed adapter/configuration task; no such integration is needed to complete this plan.

## Completion definition

- Every presented parameter candidate has one visible intent, a bounded typed Plate/Process patch, and matching Domain/native/gateway validation.
- Every recommendation is selected by a goal-specific named lexicographic policy backed by real isolated trial evidence; missing evidence cannot become a false magnitude claim.
- At most baseline plus two alternatives are trial-sliced sequentially, and candidate/provider failure leaves the formal workspace unchanged.
- The multicolor alternative changes only tool sequence, preserves IDs/mapping/prime-tower/flushing/material assignments, and is hard-excluded if preservation cannot be proved.
- Confirmation remains the only formal-write boundary; the gateway revalidates current revision and expected values, applies one transaction, and one Undo restores the prior state.
- Feature-off slicing, existing 3MF/profile loading, and default behavior remain unchanged.
- Windows focused/full tests, Release/RelWithDebInfo builds, and workspace-local isolated-data GUI acceptance are recorded; macOS/Linux remain required CI gates.
