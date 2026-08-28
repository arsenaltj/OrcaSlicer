# Neutral AI Contracts and Build Boundaries Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Isolate cross-feature AI contracts and SmartSlicing core compilation without changing accepted runtime behavior or Orca file formats.

**Architecture:** Move three stable contracts into a neutral dependency-free directory, retain old include paths as forwarding headers, introduce explicit contracts and SmartSlicing CMake targets, and enforce the dependency direction with executable guardrails.

**Tech Stack:** C++17, CMake, Catch2, Python unittest, Windows Release build.

---

## Task 1: Record the phase design

1. Add the accepted design note and this execution plan.
2. Record the checkpoint in the local planning files.

## Task 2: Add failing architecture checks

1. Add tests for the neutral contract inventory and migration phase.
2. Add a forbidden-dependency fixture for the Contracts layer.
3. Require exact legacy forwarding headers.
4. Require the new CMake targets and exclusive SmartSlicing source ownership.
5. Run focused tests and confirm the new assertions fail before implementation.

## Task 3: Move contract definitions safely

1. Add the three neutral headers under src/slic3r/AI/Contracts.
2. Replace the previous headers with exact forwarding headers.
3. Update integration-owned GUI consumers to include neutral paths.
4. Preserve namespaces, layouts, defaults, and signatures.

## Task 4: Update the architecture lock and guardrails

1. Change the migration state to neutral_contracts.
2. Record neutral contract paths as allowed cross-feature contracts.
3. Remove the ModelGeneration-to-SmartSlicing exception.
4. Scan Contracts for feature, GUI, and provider dependencies.
5. Make the focused architecture checks pass.

## Task 5: Create independent build targets

1. Add orcaslicer_ai_contracts and its namespaced alias.
2. Add orcaslicer_ai_smart_slicing and its namespaced alias.
3. Move the four SmartSlicing core implementation files out of libslic3r_gui.
4. Link the new targets into libslic3r_gui explicitly.
5. Add a native contract compatibility test to slic3rutils_tests.

## Task 6: Verify behavior and build integrity

1. Run focused guardrail tests.
2. Run the complete Python test suite.
3. Configure or reuse a valid Windows build tree.
4. Build and run targeted C++ tests.
5. Run the established Release build and relevant regression tests.
6. Confirm no 3MF/profile/default/service configuration change.

## Task 7: Review and commit

1. Inspect git diff, diff check, and worktree status.
2. Document shared files and verification evidence.
3. Commit once as refactor(ai): isolate contracts and smart slicing core.
4. Do not push without explicit user confirmation.
