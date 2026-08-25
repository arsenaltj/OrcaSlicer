# ADR-004: Smart Slicing P2 Parameter Scopes and Multicolor Policy

- Status: Proposed
- Date: 2026-08-25
- Decision makers: Product owner, Smart Slicing technical lead
- Supersedes: None
- Related: ADR-002, ADR-003

## Context

ADR-002 established a deterministic smart-slicing workbench with isolated candidates, real trial slicing, stale-workspace detection, and transactional application through `OfficialSliceGateway`. P0 and P1 provide the foundation needed to compare candidate evidence without changing the official workspace before user confirmation.

P2 expands the candidate space beyond arrangement and orientation. It needs an explicit decision before implementation because parameter proposals and multicolor optimization can otherwise blur three boundaries:

1. A recommendation may become an opaque score that users cannot audit.
2. A parameter patch may target a scope that Orca cannot validate, apply, and undo atomically.
3. A multicolor optimization may silently change material identity, flushing, or prime-tower behavior.

The current implementation already has typed parameter proposals and deterministic comparison metrics, but intentionally supports only a narrow plate/process subset. Multicolor data is currently read as evidence and compatibility constraints; it is not rewritten.

## Decision drivers

- Preserve deterministic and explainable recommendations.
- Keep at most three isolated candidates and trial them sequentially with Orca-native slicing.
- Require the same proposal to pass Domain validation, Orca adapter validation, and final gateway validation.
- Preserve logical material identity, physical slot mapping, and official default behavior.
- Avoid format migrations and avoid persisting smart-slicing-only state in 3MF or profiles.
- Roll out only scopes that have stable identity, native patching, stale detection, and one-step undo.
- Keep local deterministic candidates usable when a sidecar is unavailable or returns invalid data.

## Decision

### 1. Candidate ordering uses gates and lexicographic evidence, not an opaque total score

P2 will retain deterministic goal-specific ordering. It will not introduce a weighted scalar score or user-adjustable weight sliders in the first P2 release.

Candidates first pass hard eligibility gates:

- the trial completed and returned valid evidence;
- required metrics are present and internally valid;
- material slots are compatible;
- logical-to-physical material mapping is not degraded;
- the proposal remains valid against the captured workspace revision.

Eligible candidates are then ordered lexicographically. The selected goal's primary measured outcome is compared first, followed by warnings and the existing secondary evidence dimensions such as adhesion risk, brim assistance, tool changes, flushing, wipe-tower use, support, time, and material. The exact secondary order remains goal-specific and must be represented by named comparison evidence.

A missing optional metric is never interpreted as a numerical advantage. If one candidate wins only because it has measured evidence that another candidate lacks, the comparison must report `more_complete_trial_evidence` rather than claiming a magnitude improvement.

No quality, safety, time, or material claim may be shown unless the corresponding real trial evidence supports it. Ties remain deterministic through stable candidate identity and generation order, but tie-breaking identity is not presented as a quality claim.

### 2. Parameter proposals are small typed patches grouped by intent

Each candidate may contain one coherent, bounded parameter patch. A patch belongs to one intent family so the user can understand the tradeoff:

- **Stability:** bounded brim and support changes; cooling only after material capability data is available.
- **Quality:** bounded layer height, wall/shell, seam, and support-interface changes with measured evidence.
- **Speed:** bounded layer height or wall/shell changes, provided eligibility and warning evidence remain acceptable.
- **Material:** support and brim changes, plus geometry/orientation/sequence candidates that reduce measured waste.

The Domain owns the allowlist, types, ranges, patch-size budget, mutually dependent keys, and forbidden combinations. The Orca adapter additionally verifies that every key exists for the active Orca build and is valid for the target configuration. `OfficialSliceGateway` repeats validation against the current workspace immediately before the single transactional commit.

A sidecar may return only typed suggestions. It cannot return an authoritative `DynamicPrintConfig`, native object pointer, file path, or final configuration blob. Invalid suggestions are discarded without preventing local deterministic candidates from running.

### 3. Parameter scope rollout is staged

P2 scope support will be enabled only when the complete validation/application/undo contract exists:

1. **Plate / Process:** first supported scope. It extends the current narrow allowlist cautiously and uses the captured plate configuration as the proposal base.
2. **Object / Process:** enabled only after stable object identifiers, native object-config patching, revision capture, and one-step undo are implemented and tested.
3. **Volume and layer range:** deferred until explicit Domain target types are introduced. They must not be encoded by overloading the existing `ConfigScope` enum or by passing opaque Orca identifiers through the Domain.
4. **Material / Filament:** deferred until material compatibility and calibration-readiness gates can prove a patch safe for the selected physical slot.
5. **Workspace, Project, and Printer:** forbidden for automatic P2 proposals. Hardware, machine limits, calibration values, nozzle diameter, temperatures, flow ratio, and pressure advance remain outside this ADR.

Unsupported scopes or owner/scope combinations are rejected, not silently narrowed to the current plate.

### 4. Multicolor optimization begins as constraint-preserving search

Multicolor P2 will optimize only through Orca-native, reversible candidate operations that preserve material identity and mapping. The staged policy is:

1. Continue using slot compatibility, mapping degradation, flush volume, wipe-tower volume, tool-change count, and prime-tower state as trial evidence.
2. Add deterministic candidates for orientation, arrangement, object/instance order, and other Orca-native sequence choices where the resulting mapping remains identical and trial metrics are available.
3. Hard-exclude candidates with incompatible slots or degraded logical-to-physical mapping.

P2 will not automatically edit `flush_volumes_matrix`, `flush_multiplier`, or disable the prime tower. It will not substitute materials, rewrite filament profiles, or infer calibration values. Direct flushing or material remapping requires a separate future ADR backed by calibration data and explicit persistence/undo semantics.

### 5. Isolation, privacy, and persistence remain unchanged

All parameter and multicolor candidates operate on captured copies. Trial slicing remains isolated. User confirmation is required before an official change, and the official write occurs only through `OfficialSliceGateway` after stale-revision and proposal revalidation.

Sidecar input is structured and minimal. It excludes mesh payloads, local paths, credentials, and profile contents unless a later separately approved contract explicitly requires them. Sidecar failure cannot disable local P2 behavior.

This ADR introduces no 3MF or profile schema changes. Once accepted, a committed native Orca configuration is persisted only through Orca's existing mechanisms.

## Consequences

### Positive

- Candidate recommendations remain reproducible and auditable.
- Parameter growth is limited by capabilities Orca can validate and undo safely.
- Multicolor improvements can use real waste evidence without silently changing color/material semantics.
- Provider failure or malformed output degrades to local deterministic behavior.
- Existing projects, profiles, and the feature-off official slicing path retain their original behavior.

### Negative

- P2 initially covers fewer parameter scopes than the long-term UX design.
- Lexicographic ordering offers less user customization than weighted scoring.
- Volume, layer-range, filament, and direct flush optimization require later architecture work.
- Some useful multicolor savings cannot be attempted until calibration and remapping policies are approved.

## Alternatives considered

### Weighted aggregate score

Rejected for the first P2 release. A single score hides tradeoffs, is difficult to calibrate across printers and materials, and can imply unsupported quality or safety precision.

### Sidecar-generated final configuration

Rejected. It bypasses local type, range, capability, stale-workspace, and transactional validation.

### Enable all Orca configuration scopes immediately

Rejected. Object, volume, layer-range, filament, project, and printer scopes do not share one safe identity and undo contract.

### Automatically tune flushing and prime-tower settings

Rejected. The required calibration evidence and material-remapping guarantees are not yet available.

### Staged scopes with constraint-preserving multicolor search

Proposed. It provides useful deterministic optimization while preserving the transaction and compatibility boundaries established by ADR-002.

## Acceptance criteria

P2 implementation may be accepted only when all applicable gates pass:

- Domain tests cover the allowlist, types, ranges, patch budget, owner/scope matrix, dependent keys, forbidden combinations, deterministic ordering, and named evidence.
- Missing metrics and incomplete trials cannot produce false magnitude claims.
- Every candidate is trial-sliced in isolation using Orca-native deterministic inputs.
- Adapter and gateway validation reject unsupported keys/scopes and stale revisions before official mutation.
- One confirmed application produces one coherent undo entry; failed applications do not leave partial official state.
- Multicolor tests prove slot compatibility, logical material mapping, and prime-tower state are preserved.
- Direct flush-matrix, flush-multiplier, prime-tower-disable, hardware, and calibration proposals are rejected.
- Feature-off behavior and existing 3MF/profile loading remain unchanged.
- Targeted tests pass on Windows and CI gates remain required for macOS and Linux.
- GUI verification, when needed, uses only the application built in this workspace with an isolated data directory.

## Approval gate

This ADR is **Proposed**. P2 implementation that expands parameter scopes, changes candidate ordering policy, or adds multicolor mutation must not begin until the designated decision makers mark it **Accepted**. P1 hardening, tests, documentation, and implementation-neutral P2 preparation may continue meanwhile.
