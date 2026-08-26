# Phase 82: Beta Feedback Hardening Design

## Scope

Phase 82 addresses the model-generation issues recorded in the `3D打印内测问题跟踪` beta sheet without taking ownership of smart slicing. The changes remain in the model-generation panel, native sidecar client, sidecar, preprocessing prompts, and their tests.

Out of scope:

- automatic support generation;
- one-click slicing or filament mapping inside the Orca workspace;
- 3MF/profile changes;
- automatic paid retries or new provider calls during verification.

## Evidence and decisions

1. Palette recommendation, Image2 preprocessing, and long-running paid geometry generation currently share one single-thread executor. A geometry task can therefore leave palette recommendation visually queued at 3–5%. Use one serialized design executor and one serialized model executor. This preserves the one-paid-model-task-at-a-time rule while allowing local/Image2 design work to progress independently.
2. The four preview stages expose names but not semantics. Add a stage explanation driven by the existing cleanup metrics. A zero or near-zero change ratio explicitly means the source already passed that stage; it is not treated as a hidden failure.
3. Provider failures are stored in attempt metadata but flattened to an English message in the GUI. Expose the latest structured provider failure in job status and localize rejection, rate-limit, timeout, unavailable, and ambiguous-creation cases. The message must state that a new paid task is never created automatically.
4. Image2 must keep crop/framing independent from palette mode and preserve identity-critical face geometry. Strengthen the preview contract for face aspect ratio, jaw/chin/cheekbones, and source crop authority. These are provider quality instructions, not guarantees; final printable palette and structural gates remain authoritative.
5. Existing final-model processing already quantizes OBJ colors to the approved palette and provides a local recolor editor. Make that editor the explicit repair action for tiny color-block warnings instead of adding an unsafe automatic geometry repair.
6. Single-filament mode must still preserve the source-derived subject mask while using exactly one target material color. Add a direct regression test around the one-color pipeline and its transparent background.

## Safe degradation

- Both executor lanes shut down together and do not change persistence or restart recovery.
- Missing structured provider metadata falls back to the existing job message.
- Missing preview metrics display a static stage explanation.
- Provider rejection and ambiguous failures require an explicit user retry; no paid API is called by tests or GUI verification.
- The smart-slicing issues are documented for the integration line only and produce no code changes on this branch.

