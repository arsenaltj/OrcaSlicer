# Phase 82: Beta Feedback Hardening Implementation Plan

## 1. Sidecar scheduling and failure contract

- Split design/preprocessing work from paid model generation using two serialized executors.
- Route only `_generate_job` to the model executor; keep palette and preview work on the design executor.
- Publish the latest structured provider failure from persisted attempt metadata.
- Add mocked regression coverage for executor routing and public failure metadata.

## 2. Preview and model-repair UX

- Parse cleanup change ratio and structured provider failure in the native client.
- Add per-stage preview explanations, including the meaning of an unchanged strict/clean result.
- Localize actionable provider failure messages and state the manual-paid-retry rule.
- Change the local recolor affordance to an explicit repair action when tiny printable color regions are detected.

## 3. Image quality contracts

- Make source crop and visible anatomy invariant across palette modes.
- Strengthen face aspect-ratio and identity-landmark preservation instructions.
- Add prompt tests and a one-color silhouette/palette regression test.

## 4. Verification and delivery

- Run targeted Python suites and the full model-generation Python test set.
- Build the Release target.
- Launch only `build/src/Release/orca-slicer.exe` for GUI acceptance.
- Commit all Phase 82 changes while leaving unrelated `.tmp/` untouched.
- Prepare row-by-row Feishu results and request final confirmation before writing them.
