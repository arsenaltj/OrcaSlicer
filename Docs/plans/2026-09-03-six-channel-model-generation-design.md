# Six-Channel Model Generation Design

## Goal

Upgrade model generation from a fixed four-RGB-color workflow to an independently owned 1–6 physical-channel workflow. Preserve continuous tonal evidence for geometry, constrain printable colors only after a mesh exists, and publish enough appearance intent for a separate SmartSlicing branch to choose future process-mixing recipes. Importing a generated model must stop in the Prepare workspace; model generation must not select print settings, start slicing, or generate G-code.

## Requirements

### Functional

- Accept and preserve 1, 2, 3, 4, 5, or 6 active physical filament channels.
- Preserve existing one-to-four-color requests and generated vertex-color OBJ artifacts during migration.
- Support two explicit output modes:
  - `discrete_filament`: a target color maps directly to one physical filament slot.
  - `process_mix`: a target color maps to a sparse Orca mixed-filament recipe selected from the active physical channel pool.
- Limit the first process-mix delivery to Orca's established one-to-three-component recipe path. Different target colors may use different subsets, so all six physical channels can participate across one model.
- Persist generation intent beside the OBJ in a versioned JSON manifest. Orca remains responsible for project 3MF persistence.
- Generate one continuous-tone, low-texture geometry reference without forcing selected filament RGB values into the paid image prompt.
- Derive the exact 1–6-color material preview deterministically from that geometry reference, without a second paid image call.
- Treat the exact preview as a material-approval view and the submitted image as a shape reference; neither is the final printed result.
- Apply identity-first portrait geometry for every supported target palette cardinality when independent portrait evidence is available. Keep skin/garment repair separately gated by material evidence.
- Remove every model-generation-owned automatic slicing control, field, print-setting mutation, event, and status.

### Non-functional

- Preserve Orca defaults and backward compatibility for existing 3MF projects and printer/filament profiles.
- Keep changes cross-platform and C++17 compatible.
- Keep model generation independent from SmartSlicing implementation headers and business logic.
- Use deterministic validation and explicit errors for missing, duplicated, incompatible, or out-of-range channels.
- Introduce changes behind additive/versioned contracts until all old callers have migrated.

## Considered approaches

### A. Replace every four-color constant with six

This is small initially, but it still treats physical inputs, target appearance colors, and mixed recipes as the same RGB list. It cannot represent CMYK-style layering and creates another migration later. Rejected.

### B. Implement a second mixing solver in the Python sidecar

This keeps the generator self-contained, but recipe results would drift from Orca's `ColorDecomposeRecipe`, calibration changes would need two implementations, and the sidecar would start owning slicing-domain behavior. Rejected.

### C. Add a typed color-intent contract and preserve a late-palette hand-off

Chosen. The generator owns printable appearance intent and a direct 1–6-color OBJ fallback. The Orca adapter validates and preserves that intent during import; the independent SmartSlicing branch owns translation into physical or virtual process-mix recipes. Existing OBJ output remains valid, and new data is additive and versioned.

### D. Generate a fully palette-constrained 2D image before image-to-3D

This is the current behavior. It makes the preview easy to validate, but hard color blocks and a ban on continuous shading remove face landmarks and sculptural planes before the geometry provider sees them. Rejected as the primary geometry path.

### E. Use an unrestricted photographic image and quantize vertices independently

This retains detail but allows cast shadows, reflections, makeup, fabric texture, and background color to become false materials. Independent nearest-color assignment also produces fragmented regions. Rejected.

The selected variant is a print-aware continuous-tone reference: clean silhouette, transparent background, diffuse lighting, restrained texture, and sturdy connected forms, but no exact filament palette. Exact colors are introduced by deterministic image and mesh stages.

## Architecture

```text
Text / image input
       |
       v
Print-aware reference preparation (one paid image)
  - continuous tone and identity evidence
  - clean silhouette and printable structure
       |
       +----> exact 1..6-color material approval preview
       |
       `----> geometry reference ---> image-to-3D mesh
                                      |
                                      v
                         semantic surface color cleanup
       |
       v
GeneratedModelArtifact
  - model-vertex-color.obj
  - color-intent.v1.json
       |
       v
OrcaWorkspaceAdapter
  - reads physical channel capability
  - validates OBJ and manifest linkage
  - preserves direct 1..6-color fallback regions
  - imports into Prepare workspace
       |
       +----> user edits / saves project 3MF
       |
       `----> SmartSlicing branch maps desired colors to recipes and decides how to slice
```

## Color model

The public contract must not expose a single ambiguous palette vector as the final representation.

```cpp
enum class ColorOutputMode {
    DiscreteFilament,
    ProcessMix,
};

struct PhysicalFilamentChannel {
    size_t      slot;
    std::string display_color;
    std::string material_type;
    bool        compatible;
};

struct MixedColorComponent {
    size_t slot;
    double ratio;
};

struct MixedColorRecipe {
    std::string                     target_color;
    std::vector<MixedColorComponent> components;
    std::optional<size_t>            existing_virtual_slot;
};
```

`PrintablePaletteSnapshot` gains typed capability data while its current flat fields remain temporarily available as a compatibility projection. One source of truth is built first, and legacy vectors are derived from it until callers are migrated.

The v1 manifest records `schema`, the bound OBJ filename/hash, the exact fallback palette, semantic roles, continuous desired colors sampled from the unconstrained appearance reference, and hashes of the geometry/material references. The OBJ vertex colors remain the region identifiers for the first delivery; the manifest does not duplicate mesh topology and does not contain a slicing recipe.

## Component ownership

| Component | Owns | Must not own |
|---|---|---|
| `ModelGenerationPanel` | User input, 1–6 target palette editing, preview, import command | Print parameters, slicing policy, G-code status |
| Model-generation application/sidecar | Continuous-tone geometry reference, exact preview, generation request, geometry, target colors, manifest production and validation | Orca virtual-slot mutation, recipe solving, hardware commands |
| Neutral AI contracts | Versioned DTOs and capability types | wxWidgets, provider clients, slicing implementations |
| `OrcaWorkspaceAdapter` | Orca palette snapshot, manifest/hash validation hand-off, legacy color mapping, workspace import | Recipe solving, starting a slice, or modifying print presets for automatic slicing |
| SmartSlicing branch | Desired-color-to-recipe translation, preflight, parameter proposals, trial/final slicing | Model/provider generation |

## Import flow and failure behavior

1. Validate the OBJ and optional manifest before mutating the Orca workspace.
2. Validate active channel count in `[1, 6]`, unique physical slots, valid hex colors, positive finite ratios, and normalized recipe sums.
3. In discrete fallback mode, map targets only to compatible physical slots using the existing import behavior.
4. Preserve desired-color and semantic-role records for downstream consumers; do not create or solve mixed-filament recipes here.
5. Import with one undo snapshot. If mapping fails, either keep the model for explicit manual coloring or roll back according to the existing import outcome; never begin slicing.
6. Navigate to Prepare and report import/color/repair status only.

Older artifacts without a manifest follow the current manual/automatic/single-color import modes. An unknown manifest version fails before workspace mutation with a diagnostic that the client can show directly.

## Testing strategy

- Contract tests for both output modes, recipe invariants, and backward-compatible legacy projections.
- Parameterized 1–6 channel tests in C++ and Python.
- Prompt tests proving selected filament RGB values are absent from geometry-image and provider-geometry prompts while the deterministic preview still contains exactly the requested colors.
- Identity-path tests for 1, 2, 3, 4, 5, and 6 colors, plus negative tests for non-portrait and missing-evidence jobs.
- Manifest round-trip, invalid schema/hash/path, and legacy no-manifest tests.
- Python request/response tests proving old four-color payloads retain their behavior.
- Orca adapter tests for physical-slot mapping, virtual mixed-slot reuse/creation, invalid recipes, and absence of slice events or print-config mutations.
- Existing 3MF tests extended to prove mixed slots and painted facet IDs survive save/load.
- Architecture guardrails forbidding SmartSlicing includes and automatic-slice tokens in model-generation-owned source.
- Targeted native tests followed by the established Windows Release build and GUI validation.

## Delivery slices

1. Delete automatic slicing from the model-generation path.
2. Introduce typed, backward-compatible 1–6 channel capability contracts.
3. Make the C++ UI and Python generation path cardinality-dynamic.
4. Separate continuous-tone geometry references from exact 1–6-color approval previews.
5. Produce and validate `color-intent.v1.json` beside the OBJ, with desired colors and direct fallback colors.
6. Hand the manifest to Orca without solving process-mix recipes in this branch.
7. Add sculpture-realism and portrait-sketch presets first; translate ink wash as printmaking/relief rather than translucent texture.
8. Complete six-cardinality, build, and GUI regression evidence.
