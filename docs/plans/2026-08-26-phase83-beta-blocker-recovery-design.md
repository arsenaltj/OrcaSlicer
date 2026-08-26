# Phase 83: Beta Blocker Recovery Design

## Scope

This phase fixes the internal-beta blockers in the model-generation journey without changing 3MF, profiles, slicing algorithms, or provider selection.

## Request lifetime

State-changing and polling requests remain serialized through the existing active request. File downloads use a separate bounded-by-workflow request collection so input images, image previews, auxiliary previews, and the final OBJ cannot cancel one another. `cancel_current()` remains the single shutdown/reset boundary and cancels both categories.

## Recovery and cancellation

The Sidecar explicitly routes `GET /input`. A restored image job may therefore recover its original input while the ready artifact and preview assets download. When a ready model is only downloading or parsing locally, the action is labelled “取消加载”; it cancels local requests, keeps the ready job, and exposes “重新加载 3D 模型”. Remote `/stop` remains reserved for queued/running generation.

## Paid review and service recovery

AI visual review receives the same explicit Yes/No credit warning as palette, image, and geometry generation. When the Sidecar is unavailable, the footer shows “重新检测服务”; the panel calls a thin callback supplied by MainFrame, which resets and reuses the existing discovery flow.

## Verification

- HTTP contract test for `input_ready -> GET /input` and invalid missing input.
- Full offline AI Python suite.
- C++ build and available tests.
- Release GUI acceptance for unavailable-service retry, history load/cancel/retry, and cold-start recovery of an image-backed ready job.
- No paid Provider calls.
