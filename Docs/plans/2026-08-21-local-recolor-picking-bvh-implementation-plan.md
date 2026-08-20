# Local Recolor Picking BVH Implementation Plan

## Goal

Keep local-region picking responsive on generated models approaching the supported one-million-triangle limit, without changing selection semantics, OBJ data, GUI contracts, or OrcaSlicer slicing behavior.

## Architecture

`VertexColorRegionEditor` will build a private, immutable triangle BVH during model initialization. Ray picking will traverse node bounds front-to-back and run the existing exact ray/triangle test only for candidate leaves. Color/normal region growth and seam adjacency remain authoritative and unchanged.

## Tasks

1. Add correctness fixtures for many stacked triangles, nearest-hit ordering, misses, and invalid rays.
2. Add compact private BVH node/order storage to `VertexColorRegionEditor`.
3. Build the BVH from face bounds and centroid median splits with a small fixed leaf size.
4. Replace the linear ray scan with bounded front-to-back traversal while preserving deterministic nearest-face tie breaking.
5. Run the focused Catch2 region suite, Release OrcaSlicer build, and diff/compatibility checks.
6. Record the phase review and commit only model-generation editor, tests, and documentation files.

## Safety and compatibility

- The acceleration structure contains face indices and bounds only; it never modifies mesh topology or vertex colors.
- Selection results still come from the existing Möller–Trumbore triangle test.
- BVH state is cleared with the editor and rebuilt for each loaded model.
- The code remains standard C++17 and has no wxWidgets, Provider, Sidecar, Orca workspace, 3MF, profile, or slicing dependency.
