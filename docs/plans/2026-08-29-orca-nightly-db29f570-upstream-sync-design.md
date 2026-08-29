# Orca Nightly db29f570 Upstream Sync Design

## Goal and fixed target

Synchronize `codex/orca-integration-v2` with the standard OrcaSlicer Nightly source without rewriting any accepted AI history. The official annotated `nightly-builds` tag object was resolved once on 2026-08-29 to commit `db29f570bd2f77742ab04e0bb8f0aa55237bd70a`; `upstream/main` and upstream `HEAD` pointed to the same commit at that time. The belt-printer build is explicitly outside this sync.

The target is immutable for this integration cycle. A later movement of `nightly-builds` must be handled as a separate reviewed sync. The merge parent, integration lock, ADR, and sync report all record the full 40-character commit.

## Chosen integration strategy

Use an explicit non-fast-forward merge of the fixed upstream commit into the current integration branch. Rebasing is rejected because it would rewrite accepted model-generation, smart-slicing, and architecture commits. Cherry-picking the three upstream commits is also rejected because it obscures official lineage and makes later upstream merges harder to audit.

The old locked upstream `6fdd4945c19348cc5fc9ed9ae2f26f22a778786b` is the merge base. The Nightly delta contains three commits: UTF-8 STEP part-name handling and tests, missing dependency build registration, and warning cleanup. No AI protocol, provider, model-generation, smart-slicing, 3MF schema, or profile-format change is introduced by the upstream delta.

## Conflict policy

Merge preview reports two textual conflicts. `build_release_vs.bat` must retain both the upstream Clang-CL `-l` support and the integration branch's explicit AI installer/cache tuple. Default invocations continue to build without the AI installer. `deps/wxWidgets/wxWidgets.cmake` must accept upstream's removal of the old Clang-CL patch command, and `deps/wxWidgets/0001-Clang-CL-fix.patch` remains deleted because the updated upstream dependency flow replaces it.

The other overlapping files, including `Plater.cpp`, merge automatically. They are reviewed to ensure the FeatureHost boundary remains intact and no AI business orchestration returns to shared Orca files.

## Verification and release safety

The integration lock and verifier pin the new upstream SHA while preserving the accepted feature SHAs. Verification includes merge ancestry and parent checks, architecture guardrails, all AI Python tests, dependency configure, targeted and full Catch2 suites, Windows Release compilation, bundled runtime verification, and an isolated GUI smoke check for Chinese navigation and first-run behavior. No paid provider call is made. Port 18764, Sidecar protocol v2, Sidecar v8, output directories, 3MF/profile formats, and Orca's default non-AI behavior remain unchanged.
