# Orca Nightly db29f570 Upstream Sync Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Merge the fixed standard Nightly commit `db29f570bd2f77742ab04e0bb8f0aa55237bd70a` into `codex/orca-integration-v2` while preserving AI architecture, release behavior, and official Orca lineage.

**Architecture:** Create one explicit upstream merge commit. Resolve only the two previewed conflicts semantically, update the machine-readable upstream receipt, and verify the complete desktop product without moving feature-branch or remote heads.

**Tech Stack:** Git, C++17, CMake/Visual Studio 2022, Catch2, Python unittest, wxWidgets, PowerShell.

---

### Task 1: Freeze and document the target

**Files:**
- Create: `docs/plans/2026-08-29-orca-nightly-db29f570-upstream-sync-design.md`
- Create: `docs/plans/2026-08-29-orca-nightly-db29f570-upstream-sync-implementation-plan.md`

1. Verify the annotated Nightly tag peels to the fixed SHA and matches `upstream/main`.
2. Verify the old locked upstream is an ancestor of the fixed target.
3. Run `git merge-tree --write-tree --messages HEAD upstream/main` and record the conflict list.
4. Commit the design and plan as a reversible local planning commit.

### Task 2: Merge the exact upstream commit

**Files:**
- Modify: `build_release_vs.bat`
- Modify: `deps/wxWidgets/wxWidgets.cmake`
- Delete through upstream merge: `deps/wxWidgets/0001-Clang-CL-fix.patch`
- Modify automatically: upstream files listed by `git diff 6fdd4945..db29f570`

1. Run `git merge --no-ff --no-commit db29f570bd2f77742ab04e0bb8f0aa55237bd70a`.
2. Confirm only the two previewed conflicts occur.
3. Resolve the batch file by composing upstream Clang-CL arguments with the existing AI arguments.
4. Resolve wxWidgets by accepting upstream patch removal.
5. Review all six overlapping paths and run `git diff --check`.

### Task 3: Update the reproducible upstream receipt

**Files:**
- Modify: `docs/architecture/ai-integration-lock.json`
- Modify: `docs/architecture/ADR-003-upstream-lineage-ai-integration.md`
- Create: `docs/architecture/UPSTREAM_SYNC_2026-08-29_DB29F570.md`
- Modify: `scripts/verify_ai_integration.py`

1. Change the locked upstream SHA to `db29f570bd2f77742ab04e0bb8f0aa55237bd70a`.
2. Record the Nightly tag object, fixed commit, three upstream commits, conflicts, and preserved contracts in the sync report.
3. Run focused guardrail tests and the full verifier; expected result is PASS with the new SHA as an ancestor.

### Task 4: Build and regression test

**Files:**
- Test existing build and test targets only.

1. Configure dependencies with the existing pinned dependency build directory; expect successful generate.
2. Configure/generate the commercial-review build; expect no missing source or target.
3. Run all AI Python tests; expect zero failures and no real paid request.
4. Build and run `slic3rutils_tests`; expect all assertions to pass with only declared conditional skips.
5. Build `OrcaSlicer_app_gui` in Release; expect bundled Python/Pillow verification to pass.
6. Launch the current Release with an isolated data directory and verify Chinese navigation, first-run wizard, and AI entry visibility without submitting generation.

### Task 5: Finalize the local merge

**Files:**
- Commit all merge resolutions, receipt updates, and sync report.

1. Confirm the merge has the previous integration HEAD and the fixed Nightly SHA as its two parents.
2. Run staged whitespace and credential-pattern checks.
3. Create the merge commit with a message naming the fixed Nightly SHA.
4. Re-run the verifier and focused guardrails after commit.
5. Confirm the worktree is clean and `origin/codex/orca-integration-v2` is unchanged.
