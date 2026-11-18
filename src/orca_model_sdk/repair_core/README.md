# Desktop repair Core: M1 link boundary

This opt-in target compiles the existing desktop `Slic3r::repair_mesh` entry from
desktop commit `323c6c1c9ca7e5f94c7b7161af64584af0a69f3c`. It is the enhanced
desktop-menu path, not `strict_repair_mesh` or the complete AI/CGAL fallback chain.
The default Model SDK build and its existing capabilities/receipt remain unchanged.

Configure the Model SDK with `ORCA_MODEL_REPAIR_CORE=ON`, explicit pinned
`ORCA_MODEL_DEPENDENCY_ROOT` and `ORCA_MODEL_CGAL_INCLUDE`, then build
`orca_repair_core_consumer`. The dependency root contains the existing Boost,
Eigen, cereal and oneTBB source directories; oneTBB is built in this build tree.
No GUI, provider, file upload or desktop application build is needed.

`extract_desktop_repair.py` checks normalized source SHA256 and unique anchors
before copying exact desktop definitions into the build directory. It does not
rewrite repair algorithms. Selected TriangleMesh/BoundingBox definitions avoid
unrelated import, segmentation, slicing and GUI dependencies. The headless
adapters redirect diagnostic logging and provide the existing null translation
callback; they do not replace geometry functions. `Utils.hpp` no longer imports
an unused OpenSSL header; actual MD5 implementations retain their explicit include.

The callable boundary is the original `repair_mesh(TriangleMesh&, MeshRepairStats*,
std::string*, progress_callback)` and `mesh_repair_geometry_id`. Geometry IDs bind
the actual ordered triangle coordinates. The consumer emits actual return/error,
modification, input/output face count, both IDs, progress callbacks, CPU and wall
time. Its completion is **geometry_only_stage**; `strictVerified` and
`strictAiChainPerformed` are false and the self-intersection certificate is
`not_run`. These fields are stage diagnostics, not an SDK receipt or an API finish
payload. The CTest runs a closed tetrahedron and a duplicate-face tetrahedron.

M1 Windows execution covers those two synthetic cases. Android arm64/API24 is a
compiled and linked consumer only, not an Android runtime/resource measurement.
There is no GLB readback, appearance-channel projection, cancellation/resource
certificate or publication path in this milestone. In particular, the real 895
and 78 models are not repaired by these tests. The existing 1M generated-face,
768 MiB workspace and 120 s product bounds are not increased.

The full strict attempt reached the actual IGL fallback and failed because its
exact numeric dependency requires CGAL CORE or LEDA. `ORCA_MODEL_FULL_STRICT_CORE`
therefore fails configuration explicitly in the current numeric profile. No
fallback is stubbed or declared supported. A later reviewed stage must supply the
full dependency or define an honest enhanced compatibility receipt separately.

M2 must bind source/output geometry to a real re-read GLB, preserve or reconstruct
each appearance channel under `appearance-reconstruction-v1`, and distinguish
`glb-repair-v2` strict verification from native compatibility. It must not infer
strict success from this consumer, a boolean return, or a new geometry hash.


## M2 opt-in SDK operation

The same option now links this Core into Model SDK `repairGlbArtifact` only when
explicit policyVersion=glb-repair-v2 is requested. See CONTRACT.md for supported
profile, preserved appearance channels, canonical Core-frame carrier, real
residual checks, publication and limits. M1 historical link evidence above is
not the M2 quality evidence. The full strict branch remains unsupported.
