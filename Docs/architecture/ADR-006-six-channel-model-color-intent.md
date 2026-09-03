# ADR-006: Separate model color intent from slicing and physical execution

## Status

Accepted

## Date

2026-09-03

## Context

The model-generation workflow currently produces a vertex-colored OBJ and treats printable capability as an untyped list of RGB colors and slot indexes. Multiple C++ and Python paths assume exactly four colors. The import contract also carries an `auto_slice_after_import` option, and the Orca adapter can modify print settings and start a slice after importing the model.

The v2 Orca baseline already contains mixed-filament configuration, color recipe recommendation, sublayer/whole-layer execution, and 3MF persistence. The product now requires six-color operation with compatibility for every physical channel count from one through six. Smart slicing is developed on a separate branch, and hardware/vendor protocol implementation is out of scope for model generation.

## Decision

1. Model generation ends at a validated immutable model artifact plus versioned color intent. It never starts slicing or generates G-code.
2. Remove automatic-slice fields from the import request/result, remove the UI switch, stop changing print presets during import, and change import-success navigation to an unconditional Prepare-workspace action.
3. Represent colors using typed concepts: output mode, physical channels, target colors, and mixed recipes. Do not use a single RGB list as the authoritative contract.
4. Support one through six active physical channels. In the first process-mixing release, each target recipe uses one through three components selected from that pool, matching the established Orca implementation.
5. Keep OBJ as the geometry and vertex-region carrier. Add `color-intent.v1.json` beside it; do not make model generation emit project 3MF.
6. Generate geometry from a continuous-tone, low-texture, print-aware reference. Apply the exact selected filament colors only in deterministic preview and mesh post-processing stages.
7. Publish both continuous desired colors and direct 1–6-color fallback regions. `OrcaWorkspaceAdapter` validates and preserves this hand-off; the separate SmartSlicing branch may later reuse `ColorDecomposeRecipe` to solve process-mix recipes.
8. Migrate additively: derive legacy palette vectors from typed capabilities until existing callers and payloads have moved.

## Consequences

### Positive

- Six physical channels and CMYK-style layering are expressible without conflating them with six visible RGB swatches.
- Model generation and SmartSlicing can evolve independently.
- Recipe behavior stays aligned with Orca and benefits from upstream fixes.
- Geometry retains facial landmarks and sculptural shading that were previously flattened by early palette constraints.
- Existing OBJ artifacts and old one-to-four-color requests remain usable.
- Import no longer silently changes print presets or consumes compute by slicing.

### Negative

- Typed and legacy palette representations coexist temporarily and need consistency tests.
- The first release does not promise a six-component recipe for a single target color.
- A continuous desired-color record and a direct fallback palette coexist and require hash/linkage validation.

### Neutral

- Existing 3MF/profile schemas and Orca default slicing behavior do not change.
- Hardware calibration data may later enrich the attainable recipe catalog, but it is an input to the boundary rather than model-generation-owned compiler code.

## Alternatives Considered

**Direct four-to-six constant replacement:** rejected because it does not represent process mixing or physical/virtual slot identity.

**Python-owned mixing solver:** rejected because it duplicates Orca behavior and creates cross-runtime recipe drift.

**Palette-constrained 2D geometry reference:** rejected because exact material blocks remove tonal geometry evidence before image-to-3D.

**Unrestricted photographic reference:** rejected because lighting, reflections, and high-frequency texture become false materials and fragmented regions. The accepted reference remains print-aware while preserving continuous tone.

**Generate project 3MF directly from model generation:** rejected because it couples generation to Orca project persistence and future slicing state.

## References

- `docs/plans/2026-09-03-six-channel-model-generation-design.md`
- `docs/architecture/ADR-005-guarded-incremental-ai-decomposition.md`
- `src/libslic3r/ColorDecomposeRecipe.hpp`
- `src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp`
- `src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp`
