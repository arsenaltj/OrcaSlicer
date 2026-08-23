# Phase 79 printable color boundary review

## Scope

Phase 79 keeps the model-generation branch independent from Orca workspace and
slicing business logic. It adds a conservative Sidecar post-process for palette
OBJ artifacts: isolated color-boundary spikes may be absorbed into a better
supported neighboring color when the change strictly improves printable color
boundaries. Geometry, topology, face order, winding, object groups, and
non-vertex OBJ records are preserved.

No paid provider request was made for this review. The benchmark reused the
accepted Phase 78 model.

## OBJ and 3MF delivery decision

The 3MF Core specification provides explicit model units, packaged resources,
components, and per-triangle property references. Its Materials and Properties
extension standardizes color and material groups. In the abstract, 3MF is the
better manufacturing container:

- <https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md>
- <https://github.com/3MFConsortium/spec_materials/blob/master/3MF%20Materials%20Extension.md>

The original OBJ specification is a simpler geometry exchange format whose
material convention is normally carried through a separate MTL file. Vertex RGB
on `v` records is a widely implemented extension rather than part of the
original Appendix B1 format:

- <https://paulbourke.org/dataformats/obj/obj_spec.pdf>

For the current OrcaSlicer integration, palette vertex-color OBJ remains the
highest-fidelity delivery artifact. The local OBJ importer recognizes vertex
colors and enters Orca's existing user color-to-filament matching flow. By
contrast, the current 3MF reader ignores the standard per-triangle `pid`, `p1`,
`p2`, and `p3` properties and relies on Orca-private painted-facet/project data
for editable multicolor state. Emitting that private project representation in
the Sidecar would couple model generation to Orca project serialization and
bypass the current consumable-matching contract.

Therefore Phase 79 deliberately does not add direct 3MF generation. Users may
save the imported and matched model as native Orca 3MF. A future direct 3MF
artifact should follow a format-neutral painted-facet adapter contract rather
than reimplementing Orca project serialization in the Sidecar.

## Boundary regularization rules

The regularizer is palette-only and deterministic. A vertex can change only
when all of the following are true:

- it is incident to a mixed-color face;
- it has no more than one same-color mesh neighbor;
- the target is an existing adjacent color;
- target edge support is greater than source support by at least 1.25 times;
- no incident face gains another distinct color;
- the incident mixed-color surface area strictly decreases;
- the global changed-area budget stays at or below 0.25 percent;
- a meaningful source color loses no more than 2 percent of its original area
  and cannot be reduced below the existing 2 percent meaningful-color floor.

The operation is limited to two passes. The OBJ is replaced atomically only
when a change is accepted. `color-boundary-cleanup.json` records pass counts,
budgets, protected candidates, and before/after mixed-face area metrics.

## Real artifact result

Artifact:
`generated_models/printable-palette-phase64-v1/portable_radio/tripo/stage79-boundary-prototype-v2/model-vertex-color.obj`

SHA-256:
`86D9318C872877A207E71A290D87C31826D5ADC68BFAA55CBCD61868BCE8D6D1`

| Metric | Phase 78 | Phase 79 | Result |
| --- | ---: | ---: | ---: |
| Vertices | 248,053 | 248,053 | unchanged |
| Faces | 496,170 | 496,170 | unchanged |
| Mesh components | 1 | 1 | unchanged |
| Invalid topology edges | 0 | 0 | unchanged |
| Recolored vertices | 0 | 474 | 0.191 percent of vertices |
| Changed surface contribution | 0 | 19.290725 mm2 | 0.0737 percent |
| Mixed-color faces | 38,592 | 37,007 | down 4.1 percent |
| Mixed-color surface area | 1,403.304125 mm2 | 1,369.298543 mm2 | down 2.4 percent |
| Three-color faces | 0 | 0 | protected |
| Color regions | 305 | 268 | down 12.1 percent |
| Tiny color regions | 223 | 183 | down 17.9 percent |
| Meaningful palette colors | 4 | 4 | preserved |
| Palette coverage | 1.0 | 1.0 | preserved |

The quality status remains `review` because the inherited model still reports
thin local walls, localized overhangs, and tiny printable color regions. Phase
79 reduces color fragmentation without claiming to resolve those geometry-level
limitations.

## Verification

- Focused boundary tests: 3/3 passed.
- OBJ generation suite: 68/68 passed in 64.227 seconds.
- Sidecar readiness suite: 6/6 passed in 70.727 seconds.
- Full offline AI suite: 245/245 passed in 285.710 seconds.
- Release target `OrcaSlicer`: built successfully.
- GUI: repository-local `build/src/Release/orca-slicer.exe` imported the Phase
  79 OBJ, recognized four recommended source colors, displayed all four mapping
  rows, and loaded a visually complete radio on the plate.

The readiness test process allowance is now 30 seconds because Windows
PowerShell cold start exceeded the prior 10-second outer test limit on a busy
Release host. The capability checker's actual HTTP timeout remains two seconds.

## Compatibility and integration inventory

- Shared Orca files changed: none.
- Model-generation files changed: Sidecar and model-generation tests only.
- Configuration or environment variables changed: none.
- Dependencies changed: none.
- Network ports changed: none.
- Default output root changed: none.
- Generated-model output content: adds the palette-only
  `color-boundary-cleanup.json` report beside a prepared OBJ.
- 3MF/profile schema changed: none.
- Original Orca behavior changed: none.
- Failure behavior: controlled Sidecar error; no fallback into slicing or
  workspace business logic.
