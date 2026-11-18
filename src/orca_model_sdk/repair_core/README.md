# Desktop repair Core: M1 link boundary

This opt-in target compiles the existing desktop `Slic3r::repair_mesh` entry from
desktop commit `323c6c1c9ca7e5f94c7b7161af64584af0a69f3c`. It is the enhanced
desktop-menu path, not `strict_repair_mesh` or the complete AI/CGAL fallback chain.
The default Model SDK build and its existing capabilities/receipt remain unchanged.

Configure the Model SDK with `ORCA_MODEL_REPAIR_CORE=ON`, explicit pinned
`ORCA_MODEL_DEPENDENCY_ROOT` and `ORCA_MODEL_CGAL_INCLUDE`, then build
`orca_repair_core_consumer`. The dependency root contains the existing Boost,
Eigen, cereal and oneTBB source directories. Windows builds oneTBB in this build
tree. Android instead requires the explicit approved runtime described below.
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

## Android runtime binding

Android enhanced builds require absolute `ORCA_MODEL_ANDROID_TBB_RUNTIME` pointing
to the previously approved Slicing `libtbb.so` (726384 bytes, SHA256
`cc093ffa23653600950960121ab5c4261e6586aa14f7dadc91aab6773daaa038`).
`approved_tbb.json` pins its original Mobile Git blob/source provenance and all
139 public oneTBB headers. The pinned source is v2021.5.0/interface12050; matching
version strings alone are not accepted. `verify_approved_tbb.py` rejects missing,
different or ambiguous inputs before CMake creates the imported target. Changed
runtime/header bytes force reconfiguration and validation. There is no search
for a latest library, alternative Android TBB build, or pick-first rule.

The Model SDK and link consumer use `TBB::tbb` as an imported shared target and
link with `-z defs`. The dependency keeps its real SONAME `libtbb.so`. This is
reuse of **existing-slicing-runtime**, not a runtime built from this producer's
current source commit. `approved-tbb-binding.json` captures the exact input;
the packager revalidates it and the Android CMake cache before copying that
original binary. Windows still has its private source-built `tbb12.dll`.

The Model-only package must not replace the old Slicing/TBB/malloc runtime group
with different bytes. Its approval record preserves Mobile Git
`1b96991f715368ac8a5f80ecde0ca512760083db` and original Orca source
`50247bb89ecb9d4af364242bc8c6ba037395e774` independently of the new Model producer.
Complete native linking and ELF dependency checks are build evidence; Android
same-process execution and resource validation remain separate device gates.
