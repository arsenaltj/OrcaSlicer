# AI FeatureHost and Thin GUI Boundary Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move AI lifecycle and workflow orchestration out of Orca shared GUI hotspots and split cohesive presentation code from the oversized model-generation panel without changing accepted behavior.

**Architecture:** Add a desktop AI composition host, model-generation FeatureHost, and smart-slicing FeatureHost; keep only narrow construction/navigation/event bridges in MainFrame and Plater; extract the OpenGL preview widget and reusable presentation helpers behind stable internal APIs.

**Tech Stack:** C++17, wxWidgets, CMake, Catch2, Python unittest, Windows Release build.

---

## Task 1: Record the phase design

1. Add the accepted phase design and this implementation plan.
2. Mark phase 3 in progress in local planning state.
3. Record lifecycle, rollback, compatibility, and build-boundary decisions.

## Task 2: Add failing GUI boundary checks

1. Advance the expected migration phase to `gui_feature_hosts`.
2. Require exact desktop, model-generation, and smart-slicing FeatureHost paths.
3. Reject direct Sidecar/model-generation ownership in MainFrame.
4. Reject direct smart-slicing services and workflow implementation in Plater.
5. Require the extracted model preview and presentation units.
6. Run focused tests and confirm failures are caused only by missing phase-3 implementation.

## Task 3: Introduce desktop and model-generation FeatureHosts

1. Add `ModelGenerationFeatureHost` to own the workspace adapter and panel lifecycle.
2. Add `AIDesktopFeatureHost` to own discovery, retry, shutdown, and capability dispatch.
3. Reduce MainFrame to host construction, page mounting, navigation callbacks, and one menu-registration hook.
4. Preserve existing availability text, retry count/timing, editor-only startup, and safe shutdown.

## Task 4: Introduce the smart-slicing FeatureHost

1. Move smart-slicing adapter/coordinator/presenter/panel ownership from `Plater::priv` into the host.
2. Move candidate validation, transactional mutation, pane control, and Sidebar projection into the host.
3. Keep one narrow Plater bridge for starting the official slice and one completion forwarding call.
4. Preserve undo rollback, cancel semantics, internal restart filtering, and runtime-store location.

## Task 5: Split model-generation presentation units

1. Move the OpenGL preview widget into `ModelPreview3D`.
2. Move formatting, progress, palette, validation, path, and JSON helpers into `ModelGenerationPresentation`.
3. Keep `ModelGenerationPanel` public methods, wx bindings, task states, and output metadata unchanged.
4. Add native tests for deterministic presentation mappings.

## Task 6: Update ownership, lock, and build manifests

1. Record FeatureHost paths and the new migration phase in the integration lock.
2. Assign integration/model/smart ownership without introducing overlap.
3. Add all new sources to the existing GUI target and lower the model panel line budget.
4. Make focused boundary checks and the complete verifier pass.

## Task 7: Verify GUI and release integrity

1. Run focused and full AI Python tests.
2. Build targeted native tests and run model-presentation plus smart-slicing suites.
3. Build the Windows x64 Release GUI target.
4. Launch the built application and verify Chinese UI, the model-generation page, smart-slicing registration, and Sidecar-offline fallback without paid calls.
5. Confirm no change to port 18764, Sidecar v8/protocol v2, 3MF/profile formats, output directories, or Orca defaults.

## Task 8: Review and commit

1. Inspect shared-file reductions, ownership, diff budgets, and whitespace.
2. Record tests and remaining risks in local progress files.
3. Commit the phase as one independently revertible local commit.
4. Do not push without explicit user confirmation.
