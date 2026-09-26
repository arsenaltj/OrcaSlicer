# Beauty region evidence lifecycle repair

Iteration: COL-010, candidate beauty-region-cache-r1, 2026-09-26.

Baseline: codex/integrate/pr15-beauty-safe, 234d6e2ef68e7ddf3e798315dc6bc1dc7cfea7a0.

User symptom: automatic region matching repeatedly reports no semantic cache, including after semantic optimization.

Static cause: candidate loading clears Analysis; saved results, preview caches and Beauty transaction snapshots retain paint but not face labels. Selection availability is incorrectly tied to palette display toggles.

Hypothesis: independent immutable region evidence, validated against ordered geometry and runtime identity, restores automatic selection without changing recognition or palette algorithms.

Changes: selection-only evidence, candidate/cache/undo restoration, content-addressed local cache references for accepted versions, legacy Analysis decoding without prediction, actionable status and selection counts. Geometry edits invalidate evidence; appearance edits inherit only on exact face/corner geometry identity.

No changes to four/six-color mapping, confidence threshold, recognition masks, manual color priority or preparation-page import. No mobile-SAM experimental evidence imported. Automatic selection never initiates recognition.

Code validation: Release application and slic3rutils test builds passed. The 10 new evidence cases passed with 88 assertions; the combined BeautyWorkbench, ModelPreviewState, SemanticColoring, SemanticMaterialRegions, ModelSemanticColoring and ColorTrialState regressions passed with 176 cases and 9,170 assertions (random order). AI integration verification passed with Git checks enabled; git diff --check passed. The pre-existing linker default-library warning LNK4098 remains.

The real GLB regression uses the existing textured baseline with neutral fixture material multipliers. Earlier fixture attempts exposed existing restrictions on absolute texture color editing and unavailable Assimp export symbols; production editing restrictions were not weakened.

Runnable directory: build-pr15/packages/20260926-beauty-region-cache-r1. EXE SHA256: 4a46ccc6aa7703c78683b35bdd76fbab3f799fddf3ab9a80cb15725cfb4fbe5d. OrcaSlicer.dll SHA256: 7218b6eb48c3ef71e457196aa86a0bed2c84b858bc7239a528d8fd61c3877810. Installed bytes match the final Release build. Bundled Python 3.12.13 / Pillow 12.2.0 isolated native PNG check passed. Both the existing portrait runtime and isolated Beauty worker resources are staged; this repair uses the existing portrait recognizers, not the experimental Beauty worker.

Source identity: baseline plus complete uncommitted file-hashed checkpoint, not a clean integrated commit. The delivery directory records the exact checkpoint and runtime hashes. No commit, push, installer, replacement installation or paid generation was performed.

Acceptance: actual main-window reproduction and visual acceptance NOT_RUN, assigned to the user. Check explicit reoptimization, five automatic selections, edit preview without yellow overlays, undo/redo, acceptance, and reopening. Preparation-page behavior and printed fidelity have not been visually accepted in this iteration. Unit tests do not substitute for those checks; no new same-model screenshot comparison was captured.

Limits: local evidence references are not portable between machines. Missing, corrupt, incompatible runtime, changed ordered geometry or wrong source hash safely requires explicit recognition. Legacy states containing only paint cannot reconstruct labels. Automatic selection does not start recognition, and geometry edits do not reuse old labels. Fixed validation task registration was absent; no replacement task was created.

User feedback, 2026-09-26: "current changes are acceptable"; requested a clean installer and Git commit. This confirms acceptance of the current modification, not independently documented completion of every main-window or printed-fidelity check. Packaging and a local commit are now authorized; remote push remains outside this request. The installer will be rebuilt from the committed source with no embedded provider defaults, and its source/binary identities and payload inspection will be recorded in the package manifest. The earlier direct-run snapshot remains retained as evidence and is not overwritten.
