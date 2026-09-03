# Model Generation v1 Historical Context

This directory is a frozen, curated snapshot of the model-generation feature
line before development moved onto the shared Orca integration baseline.

- Captured: 2026-09-03
- Historical feature branch: `codex/model-generation`
- Historical feature tip: `16f8ac0fb048570edb5fe787bdfdfcde4c9fa4b7`
- Initial accepted snapshot: `320394e429` via integration receipt `9b81026d58`
- Final accepted portability patch: `16f8ac0fb0` via integration receipt `616a063192`
- New feature baseline: `codex/orca-integration-v2` at `808efe4401`

These files are historical evidence, not the current product specification.
New decisions and implementation records should be written against the active
integration-based feature branch.

## Context snapshots

- [Findings and decisions](findings.md) — accumulated investigation results and
  design decisions from the original Codex task.
- [Progress log](progress.md) — accumulated implementation and verification
  history.
- [Task plan](task_plan.md) — the historical model-generation roadmap and phase
  checklist. It may contain superseded or partially completed items.

The three context files include the preserved working-tree state from the old
feature checkout at migration time, so they can contain notes newer than the
historical feature tip.

## Product and quality evidence

- [Four-color image pipeline](FOUR_COLOR_IMAGE_PIPELINE.md)
- [Beta 1 status and roadmap](MODEL_GENERATION_BETA1_STATUS_AND_ROADMAP.md)
- [Blind pilot report](MODEL_GENERATION_BLIND_PILOT_V1_REPORT.md)
- [Multiview calibration wave 2](MULTIVIEW_CALIBRATION_WAVE2_REPORT.md)
- [500-run community quality report](../../../docs/MODEL_GENERATION_COMMUNITY_500_QUALITY_REPORT.md)
- [Three-style quality report](../../../docs/MODEL_GENERATION_THREE_STYLE_QUALITY_REPORT.md)

The final two reports were already present in the integration baseline under
the repository's lowercase `docs/` path, so this archive links to them instead
of storing duplicate copies.

## Architecture decisions

- [Model generation and smart slicing boundaries](architecture/06-model-generation-smart-slicing-decoupling.md)
- [Modular monolith and Orca adapter](architecture/ADR-001-ai-modular-monolith-orca-adapter.md)
- [Provider boundary for multiview generation](adr/0001-multiview-generation-provider-boundary.md)
- [Capability-aware reference-view routing](adr/0002-reference-view-strategy-routing.md)

## Full historical record

Detailed phase-by-phase plans and reviews remain available on the frozen
`codex/model-generation` branch. They were intentionally not copied wholesale
to keep the integration-based feature line concise and to avoid presenting old
implementation plans as current requirements.
