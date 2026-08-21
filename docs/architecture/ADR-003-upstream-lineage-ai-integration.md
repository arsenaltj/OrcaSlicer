# ADR-003: Keep AI integration on the official Orca Git lineage

**Status:** Accepted
**Date:** 2026-08-21
**Upstream baseline:** `6ef02a67dbb22ae1a019d9f485f46bfc3e1b44aa`

## Context

Model generation and smart slicing are developed and accepted independently. The earlier development repository was created from an aggregate source import and has no Git merge base with the official Orca `upstream/main` history. Directly merging or rebasing that history cannot provide a reliable long-term upstream update path.

The product must preserve Orca's native Model, Config, 3MF/profile, slicing and Preview behavior. AI features must remain optional, cross-platform and independently testable. Changes to high-churn Orca GUI composition files must stay small and auditable.

## Decision

1. `codex/orca-integration-v2` is the long-lived AI product integration line and is created directly from the pinned official upstream commit above.
2. The official `upstream/main` reference remains read-only. Shared integration history receives upstream changes through explicit merge commits; published product history is not rebased.
3. Model generation and smart slicing continue on independent branches. Integration consumes an exact user-accepted commit SHA, never a moving branch head.
4. Feature-owned code is ported before shared Orca touchpoints. Shared `MainFrame`, `Plater`, GUI build registration and navigation changes are owned by the integration line.
5. The two feature modules do not depend on each other. Cross-feature handoff uses stable DTOs and Ports through a thin composition layer.
6. `libslic3r` does not contain provider or AI workflow policy. Any necessary core change must be a generally useful Orca capability with focused tests.
7. Feature-off behavior, old 3MF/profile compatibility and normal manual slicing are release gates.

## Branch and commit policy

```text
upstream/main
      |
      v
codex/orca-integration-v2
      ^                    ^
      |                    |
accepted model commit   accepted slicing commit
```

Each integration cycle records:

- pinned upstream SHA;
- accepted model-generation SHA and its verification evidence;
- accepted smart-slicing SHA and its verification evidence;
- one commit per ported module or architectural boundary;
- full build, test, compatibility and GUI results.

## Runtime isolation

Concurrent development instances use separate build directories, `--datadir` values, Sidecar ports and output roots. The reserved defaults for local verification are:

| Role | Sidecar port | Runtime/output identity |
|---|---:|---|
| Model generation | 18764 | `model-generation` |
| Smart slicing | 18765 | `smart-slicing` |
| Combined integration | 18766 | `orca-integration-v2` |

GUI automation must select the executable path and concrete window identity. Combined acceptance runs only against the integration build.

## Consequences

- The first migration requires controlled porting rather than a history merge.
- Later Orca updates have a normal merge base and produce reviewable conflicts.
- Upstream, model-generation, smart-slicing and composition changes remain attributable.
- Conflicts cannot be eliminated when Orca changes the same public integration point, but they are constrained to a small, tested boundary.

## Release gates

- Windows Release build and available Catch2 suites pass.
- Model-generation Python/C++ and smart-slicing C++ tests pass.
- Independent and combined GUI journeys pass with isolated runtime state.
- AI-disabled and Sidecar-offline paths preserve normal Orca behavior.
- Representative old 3MF projects and profiles open, slice and save without migration regressions.
- No credentials, generated models, build products or machine-specific paths are committed.
