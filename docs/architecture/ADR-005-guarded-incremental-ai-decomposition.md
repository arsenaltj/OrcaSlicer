# ADR-005: Guard AI decomposition with machine-verifiable architecture budgets

**Status:** Accepted

**Date:** 2026-08-28

## Context

ADR-003 establishes the official Orca Git lineage and fixed-SHA intake policy. ADR-004 selects a desktop modular monolith, one authenticated local Sidecar and a commercial Provider Gateway. The remaining desktop implementation still has two large concentration points: `ModelGenerationPanel.cpp` and `orca_ai_sidecar.py`. AI composition also adds code to upstream hotspots such as `MainFrame`, `Plater` and the CMake graph.

A big-bang rewrite would mix behavior changes, file moves, packaging changes and upstream conflicts. Documentation alone cannot stop new responsibilities from being added while decomposition is in progress.

## Decision

Adopt guarded incremental decomposition:

1. Extend the machine-readable AI integration lock with an architecture contract.
2. Record the current maximum line count of designated decomposition targets.
3. Record per-file added-line and net-added-line budgets for integration-owned upstream composition roots relative to the locked upstream SHA. Deleting extracted business logic is allowed and lowers structural pressure.
4. Fail integration verification when a file exceeds its budget, a composition-root diff grows without an explicit lock review, or release-promotion invariants are weakened.
5. Move cross-feature Ports/DTOs to `src/slic3r/AI/Contracts` through compatibility forwarding headers before moving feature implementations.
6. Add independent CMake targets, FeatureHost composition and Sidecar packages in later, separately testable commits.
7. Only lower or remove a budget when the corresponding responsibility has been extracted. A budget increase requires an explicit lock change and integration-owner review.

The release contract states that an internal fast package is not promotable, a commercial candidate is tied to an exact source SHA, and production promotes the accepted candidate artifact without rebuilding.

## Consequences

### Positive

- Architecture drift becomes a CI failure rather than a review convention.
- Every refactor can be small, behavior-preserving and independently reversible.
- Upstream merge hotspots have a visible cost and cannot grow silently.
- Release-channel semantics are machine-readable.

### Negative

- Legitimate upstream sync or composition changes may require an explicit budget update.
- Line and diff budgets measure structural pressure, not code quality; they supplement rather than replace review.
- Compatibility forwarding headers temporarily add indirection during migration.

### Neutral

- Product port 18764, Sidecar v8/protocol v2, 3MF/profile formats and Orca defaults do not change.
- Existing historical integration receipts remain immutable evidence even after files move in later commits.

## Alternatives Considered

**Documentation-only ownership rules:** rejected because they cannot be enforced in CI.

**Immediate full rewrite:** rejected because it combines GUI, runtime, packaging and provider risk in one change.

**Separate desktop microservices:** rejected by ADR-004 due to process, port, installer and debugging overhead.

## References

- `docs/architecture/ADR-003-upstream-lineage-ai-integration.md`
- `docs/architecture/ADR-004-commercial-ai-product-line.md`
- `docs/architecture/COMMERCIAL_READINESS.md`
- `docs/plans/2026-08-28-ai-modular-integration-release-design.md`
