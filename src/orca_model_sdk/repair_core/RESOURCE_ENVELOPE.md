# v2 bounded 2M resource envelope

The repair entry, CGAL residual predicates and appearance policy are unchanged.
The 768 MiB estimated workspace ceiling, 120s cooperative deadline, 100 MiB
source/candidate and 2.5M exported vertices are not raised. Release candidates
advertising generated2M remain unqualified until independent review and QA.
v1 keeps its original 1M generated capability; v2's profile remains static,
embedded, single primitive, unextended, UV0 (not arbitrary 2M success).

The former 4B+96V+448F admission made every true2M input mathematically impossible.
It is replaced **only for v2** after removing simultaneous allocations:

- Exact face/edge counts use sorted 12F face keys and 24F directed-edge words,
  with unchanged first-occurrence point identity, orientation and ordered faces.
  Indices are uint32 instead of 3F doubles. Input bytes are borrowed for the
  synchronous operation rather than copied into each Glb reader.
- Core repair is still desktop323c repair_mesh. The original appearance geometry
  is reconstructed from immutable source only after residual checking; it is
  not held simultaneously with Core/CGAL scratch. Repair output is not fabricated.
- Per-corner projection is emitted immediately in source face order; the entire
  88F mapping array no longer exists. Exact source face keys/AABB behavior and
  bounded projection, normal orientation, UV seams and cancellation remain.
- Pinned CGAL's full soup predicate is extracted into a private namespace with
  complete file/function SHA and unique anchors. Its dead directed-edge set is
  cleared before allocating Orienter::Edge_map. Every original check/return,
  singular-link and subsequent self-intersection call remain. IDs are statically
  constrained to trivial integral types. Upstream headers/cache/global symbols
  are not modified. The source license is retained verbatim. Release NDEBUG
  avoids debug-only upstream preconditions repeating the same validation.

## Estimated admission and actual allocation checks

The receipt reports each phase separately, not one reduced face coefficient.
B is actual input bytes; V/F are declared counts already format-bounded. Dense
geometry is V*sizeof(V3double)+F*sizeof(T3uint32); capacity bounds conservatively
allow growth. Each phase adds 128 MiB allocator/library reserve plus 16 times
actual JSON-header bytes. Preflight takes the maximum decode, sorted counting,
Core (including explicit native scratch allowance), and residual phase. CGAL
soup planning includes 3F directed set nodes, each value plus four pointers and
32 bytes alignment/allocator allowance; other residual connectivity/boxes are
planned separately. The exact byte arithmetic and architecture sizeof values
are emitted as workspaceAdmission in the private proof.

Carrier admission is independent and uses actual geometry/attribute capacities,
source bytes, explicitly reserved output strings, actual split-node count and
mapping workspace. Lazy AABB allocation requests its additional allowance before
construction; a negative budget throws RESOURCE_LIMIT before publishing. Heavy
UV splitting/projection may therefore fail safely even below format maxima.
The root-approved profile is an estimated/empirically checked envelope, **not an
OS hard RSS limiter**, and 120s is cooperative, not preemption in every CGAL call.

## Candidate and display storage

A predicted carrier above the existing64MiB display target uses an active-image
layout instead of retaining dead original geometry in BIN. The100MiB hard limit
is unchanged. All image encoded bytes, MIME and other image semantics, material/
texture/sampler/PBR/alpha/wrap and per-corner UV/normal evidence remain exact or
explicitly reconstructed under the existing policy. Only image bufferView IDs
are rebased, all active references are validated and serialized geometry is
actually reread against the native ordered ID. The original source is immutable.
Uncompacted candidates retain the stronger whole-original-BIN prefix equality.
This is not permission to omit a live channel or guarantee every model fits64MiB.

## Development evidence, not device qualification

The first true2M untextured run completed but peaked885047296B (844.05MiB):
resource failure, retained, not counted as supported. After lifetime repair,
the same true2M input measured524152832B (499.87MiB),26.23s wall; authored-normal/
PBR/UV-seam true2M measured543977472B (518.78MiB),26.34s. The64MiB layout revision
has its own necessary untextured execution and original72MB result retained.
These are Windows warm-cache process measurements, not phone timings or hard
memory guarantees. Both actual2M candidates have source/output/readback IDs,
SHA and standard GLTF validation evidence. Geometry deviation stays not_run;
native_compatible does not confer manufacturing safety. No 2M Android execution
or renderer pixel equivalence is claimed. Fixed large witnesses are not a
universal success-rate guarantee.
