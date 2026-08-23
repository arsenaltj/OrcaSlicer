# Phase 79: Delivery format and printable color-boundary design

## Goal

Choose the highest-quality artifact path for the current four-color model-generation workflow, then reduce color-boundary noise without coupling the Sidecar to Orca project internals.

## Format decision

Three options were evaluated:

1. **Standards-only 3MF.** 3MF provides explicit units, packaged resources, object/component structure, and standardized per-triangle color/material properties. However, the current Orca importer explicitly ignores standard triangle `pid`, `p1`, `p2`, and `p3` properties in `src/libslic3r/Format/bbs_3mf.cpp`. A standards-only file would therefore not preserve the generated four-color segmentation in the current application.
2. **Orca/Bambu project-flavoured 3MF.** Existing `paint_color` facet annotations can preserve slicer-specific painting, but generating them in the Sidecar would bind model generation to Orca project serialization and would bypass the existing OBJ color-to-filament matching flow. This conflicts with the independent-module boundary and increases 3MF compatibility risk.
3. **Palette-constrained vertex-color OBJ.** This is the recommended current delivery format. The repository-local Orca build already imports its vertex colors, asks the user to map generated colors to available filaments, and supports local recoloring. OBJ remains a geometry/editing artifact; after material matching, the user may save an ordinary Orca project through Orca's native 3MF writer.

The decision is therefore to keep OBJ as the primary artifact until the Orca adapter exposes a format-neutral painted-facet contract. This is an implementation-quality choice, not a claim that OBJ is more capable than 3MF as a specification.

## Quality problem

The accepted four-color prototype has exact palette coverage and valid geometry, but many triangles contain two vertex colors. Orca preserves these transitions by subdividing painted facets. Necessary boundaries should remain, while isolated boundary spikes create avoidable micro-segments and color changes.

## Boundary regularization

Add a bounded, deterministic cleanup pass after tiny color-component consolidation:

- inspect only vertices incident to mixed-color triangles;
- consider only colors already present on edge-adjacent vertices;
- recolor a vertex only when the candidate strictly decreases mixed-face surface area and has stronger edge support than the current color;
- apply changes incrementally so every accepted change is re-evaluated against the current mesh state;
- cap changed surface contribution globally and per source color;
- never reduce a previously meaningful palette color below the existing 2% meaningful-surface threshold;
- preserve positions, face indices, winding, object/group statements, and all non-vertex lines;
- emit a separate `color-boundary-cleanup.json` audit report.

The pass is advisory-safe: if no change satisfies every gate, the OBJ is left byte-for-byte unchanged and the report records `not_needed`.

## Error handling and fallback

Parsing, non-finite geometry, invalid color data, or report-write failures remain controlled Sidecar generation errors. No paid API is retried. The cleanup never changes topology and therefore cannot create a new watertightness failure. Existing OBJ validation and model-quality gates run after cleanup.

## Verification

- focused tests for spike removal, stable intentional boundaries, palette protection, deterministic reports, and no-op preservation;
- full Python model-generation suite;
- reprocess the existing paid prototype without another provider call and compare mixed-face metrics, palette coverage, color-region metrics, and mesh topology;
- Release build;
- import the reprocessed prototype with the repository-local `build/src/Release/orca-slicer.exe` and confirm the four-color mapping flow and visible model integrity.

## Evidence

- 3MF Core Specification: <https://github.com/3MFConsortium/spec_core/blob/master/3MF%20Core%20Specification.md>
- 3MF Materials and Properties Specification: <https://github.com/3MFConsortium/spec_materials/blob/master/3MF%20Materials%20Extension.md>
- Wavefront OBJ specification archive: <https://paulbourke.org/dataformats/obj/obj_spec.pdf>
- Orca implementation evidence: `src/libslic3r/Format/OBJ.cpp`, `src/libslic3r/Format/bbs_3mf.cpp`, and `src/libslic3r/Model.cpp`.
