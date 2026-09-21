# ImageMap internal interface probe

Upstream: https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap

Fixed commit: `92548381056dbf72836b0a1bdc455f238218dbfb`.
The downloaded files are unchanged. `sources.json` records their source paths,
SHA256 values and sizes. They stay outside the main application source/build.

| Component | Recorded license | Included notice |
|---|---|---|
| ImageMap ColorSolver | AGPL-3.0-or-later (source header) | Upstream `LICENSE.txt`, source copyright |
| ImageMap texture image fields | Upstream AGPL-3.0 project | `LICENSE.txt`, TextureMapping author header |
| Pigment Painter integration | GPL-3.0 text supplied by upstream | `deps_src/pigment-painter/COPYING` |
| Pigment Painter LUT | Copyright/provenance not separately documented beside the downloaded LUT | Keep upstream file and COPYING; product redistribution review remains open |
| Prusa FDM Mixer | MIT | `deps_src/prusa-fdm-mixer/LICENSE` |

This is local engineering evaluation. The source/license files above are kept
with the probe. It is not a completed product redistribution or training-data
audit, and none of these resources are added to the Orca product runtime.

## Reproduce

Run `prepare.ps1 -Destination <scratch>/upstream`, then configure this directory
as a separate CMake project with `-DIMAGEMAP_SOURCE_DIR=<scratch>/upstream` and
`-DCMAKE_PREFIX_PATH=<existing Orca dependency prefix>` for libpng/zlib.
Build `imagemap_probe` and run `imagemap_probe <result-directory>`.

An optional second argument accepts a JSON array of `{ "id": "region-id",
"rgb": [0.7, 0.5, 0.4] }` entries representing region colors **before** filament
quantization. The built-in colors are synthetic contract fixtures, not metrics
from the four user models.
An optional third argument accepts a JSON array of `{ "id": "slot-id",
"rgb": [0.9, 0.7, 0.6] }` entries for the frozen physical palette. Both target
and palette source paths are recorded; real-model runs must provide both.

Fixed experiment parameters: PigmentPainter, ClosestMix, Oklab, 20 total units.
Candidate caches include stable slot IDs and colors. Duplicate RGB slots stay
separate in the output. The probe exercises 1–6 candidates, repeated use,
identity changes, invalid inputs and a small face-corner UV/image-field bridge.

The UV probe uses the actual upstream `TextureMappingPrimeTowerImage` fields;
its tiny sampler is a contract fixture. It does not run ImageMap's mesh loader,
slice pipeline, raw offset atlas, wall modulation, ordered stacks or G-code.
No projection image is described as a model UV atlas.

Outputs are uncalibrated color predictions. Mixture weights do not specify an
ordered print-layer recipe, and a lower predicted Oklab distance is not evidence
of improved physical print color.
