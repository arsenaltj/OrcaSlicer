# Six-Channel Model Generation Design

## Goal

Upgrade model generation from a fixed four-RGB-color workflow to an independently owned 1–6 physical-channel workflow that can describe both discrete filament colors and Orca-compatible process-mixing intent. Importing a generated model must stop in the Prepare workspace; model generation must not select print settings, start slicing, or generate G-code.

## Requirements

### Functional

- Accept and preserve 1, 2, 3, 4, 5, or 6 active physical filament channels.
- Preserve existing one-to-four-color requests and generated vertex-color OBJ artifacts during migration.
- Support two explicit output modes:
  - `discrete_filament`: a target color maps directly to one physical filament slot.
  - `process_mix`: a target color maps to a sparse Orca mixed-filament recipe selected from the active physical channel pool.
- Limit the first process-mix delivery to Orca's established one-to-three-component recipe path. Different target colors may use different subsets, so all six physical channels can participate across one model.
- Persist generation intent beside the OBJ in a versioned JSON manifest. Orca remains responsible for project 3MF persistence.
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

### C. Add a typed color-intent contract and reuse Orca through an adapter

Chosen. The generator owns printable appearance intent; the Orca adapter owns translation from that intent to physical or virtual filament slots. Existing OBJ output remains valid, and new data is additive and versioned.

## Architecture

```text
Text / image input
       |
       v
Model Generation application
  - geometry generation
  - target palette (1..6)
  - region / vertex-color intent
  - artifact validation
       |
       v
GeneratedModelArtifact
  - model-vertex-color.obj
  - color-intent.v1.json
       |
       v
OrcaWorkspaceAdapter
  - reads physical channel capability
  - reuses ColorDecomposeRecipe
  - creates/reuses virtual mixed slots
  - maps model regions
  - imports into Prepare workspace
       |
       +----> user edits / saves project 3MF
       |
       `----> SmartSlicing branch decides whether and how to slice
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

The v1 manifest records `schema`, `mode`, active physical channels, target colors, optional semantic roles, and optional recommended recipes. The OBJ vertex colors remain the region identifiers for the first delivery; the manifest does not duplicate mesh topology.

## Component ownership

| Component | Owns | Must not own |
|---|---|---|
| `ModelGenerationPanel` | User input, 1–6 target palette editing, preview, import command | Print parameters, slicing policy, G-code status |
| Model-generation application/sidecar | Generation request, geometry, target colors, manifest production and validation | Orca virtual-slot mutation, hardware commands |
| Neutral AI contracts | Versioned DTOs and capability types | wxWidgets, provider clients, slicing implementations |
| `OrcaWorkspaceAdapter` | Orca palette snapshot, recipe translation, color mapping, workspace import | Starting a slice or modifying print presets for automatic slicing |
| SmartSlicing branch | Preflight, parameter proposals, trial/final slicing | Model/provider generation |

## Import flow and failure behavior

1. Validate the OBJ and optional manifest before mutating the Orca workspace.
2. Validate active channel count in `[1, 6]`, unique physical slots, valid hex colors, positive finite ratios, and normalized recipe sums.
3. In discrete mode, map targets only to compatible physical slots.
4. In process-mix mode, reuse a compatible existing virtual slot or ask Orca's recipe engine for a one-to-three-component recipe from the active physical pool.
5. Import with one undo snapshot. If mapping fails, either keep the model for explicit manual coloring or roll back according to the existing import outcome; never begin slicing.
6. Navigate to Prepare and report import/color/repair status only.

Older artifacts without a manifest follow the current manual/automatic/single-color import modes. An unknown manifest version fails before workspace mutation with a diagnostic that the client can show directly.

## Testing strategy

- Contract tests for both output modes, recipe invariants, and backward-compatible legacy projections.
- Parameterized 1–6 channel tests in C++ and Python.
- Python request/response tests proving old four-color payloads retain their behavior.
- Orca adapter tests for physical-slot mapping, virtual mixed-slot reuse/creation, invalid recipes, and absence of slice events or print-config mutations.
- Existing 3MF tests extended to prove mixed slots and painted facet IDs survive save/load.
- Architecture guardrails forbidding SmartSlicing includes and automatic-slice tokens in model-generation-owned source.
- Targeted native tests followed by the established Windows Release build and GUI validation.

## Delivery slices

1. Delete automatic slicing from the model-generation path.
2. Introduce typed, backward-compatible 1–6 channel capability contracts.
3. Make the C++ UI and Python generation path cardinality-dynamic.
4. Produce and validate `color-intent.v1.json` beside the OBJ.
5. Translate process-mix intent through Orca's existing recipe engine.
6. Complete six-cardinality, 3MF, build, and GUI regression evidence.

