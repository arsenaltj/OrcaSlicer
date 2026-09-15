# Windows CI speed experiment

The current default is still MSBuild. This change retains Windows MSVC, PCH,
Release/debug symbols, all tests and the existing downloadable artifacts. It
does not change merge protections, the Team integration caller's four inputs,
required check names, Linux/macOS jobs, or the bot's deployed code.

The reusable workflow input `slicer-generator` accepts `auto`, `msbuild`, and
`ninja-multi`. Explicit selection is limited to Windows x64/MSVC source builds.
Dependencies retain their existing generator and cache key. Existing clang
callers retain their automatic Ninja route. `auto` remains MSBuild for MSVC
unless the comparison below supports a subsequent, scoped adoption commit.

Hosted Windows packaging uses isolated staging directories, at most two
packaging processes, and checked process exits. It removes the intermediate
portable ZIP that was never uploaded; the portable directory artifact remains.
PDB compression changes from `-mx9` to `-mx3`. Installer, PDB and MSIX uploads
disable redundant artifact compression. NSIS and MSIX remain unsigned internal
build artifacts; this workflow does not publish releases. Existing custom
runner behavior and ARM64's omission of PDB remain unchanged.

## Measurement protocol

From integration baseline `4e3037f6f827baa9355ec6a8b9e7cff1a5112bfa`, prepare one
reviewed experiment commit. Push that exact SHA to the following temporary refs
**one at a time**, waiting for successful build, packaging validation and Windows
unit tests before starting the next. Do not batch-push these refs:

1. `codex/ci-speed-20260915-1-msbuild`
2. `codex/ci-speed-20260915-2-ninja`
3. `codex/ci-speed-20260915-3-ninja`
4. `codex/ci-speed-20260915-4-msbuild`

These four refs trigger the dedicated `windows-ci-benchmark.yml` push workflow.
Separate runs keep the unchanged test artifact names unique. GitHub-hosted
runner capacity is shared with ordinary PRs. No paid runners are introduced.
Never use these benchmark runs as protected integration CI evidence.

Each run requires a fresh source build directory and records dependency cache
hit/key/configuration inventory, source SHA/tree, image version, CPU/RAM,
compiler/SDK/CMake/Ninja/MSBuild identities, actual CMake configuration, phase
timings, test results and artifacts. `ORCA_CI_METRICS_PATH` enables JSONL phase
timing; optional `ORCA_CI_BUILD_LOGS=1` adds MSBuild binlogs without embedded
project imports. Timing is recorded identically for both generators.

Each run also packages the same installed build twice: baseline serial packaging
(including the unused ZIP and `-mx9`) and optimized packaging. Runs 1 and 3 put
baseline first; runs 2 and 4 put optimized first to balance filesystem-cache
order effects within each generator. Independent input
hashes, MSIX staging hashes, 7-Zip integrity tests and archive file/size/CRC
inventories must agree. Entries without CRCs additionally require extraction
and matching SHA-256 hashes. The baseline packages are measurement intermediates;
all normal optimized downloadable artifacts are still uploaded. The additional
baseline packaging and archive verification time is reported separately and is
present in every generator comparison. Archive validation does not exercise the
installer or desktop application.

Stop further generator trials on a build, test or packaging failure. At most
four trials are authorized by this experiment. Two comparable successful samples
per generator are required: identical source SHA/tree, dependency cache hit/key
and configuration inventory, runner image/hardware and compiler/SDK/CMake/Ninja
versions. Environment differences or missing evidence make the experiment
inconclusive. Do not attribute those differences to a generator speedup.

Adopt Ninja only if its median build/link time improves by at least 10% and its
median elapsed workflow time improves by at least 5%, with tests and artifacts
valid. Record the extra measurement overhead when interpreting elapsed time.
Otherwise keep MSBuild. Keep packaging changes only if content validation passes
and paired packaging times improve. Any adoption of Ninja through `auto` must be
limited to Team integration Windows x64/MSVC; other automatic callers retain
their behavior and an explicit MSBuild override remains available.

## Delivery and rollback

Create the independent `codex/ci-windows-speed` PR against the latest integration
baseline after recording results and the final choice. Run fresh normal AI
integration checks, Windows build and Windows unit tests on the final candidate;
do not reuse benchmark checks. Preserve branch protection and use an ordinary
protected PR merge. Rollback uses an ordinary follow-up commit or revert PR.
No force push, reset, reopening PR #7, old bot acceptance, local full application
build or desktop test is part of this task. Desktop flows remain unverified.

Results and the final generator decision are pending real hosted CI runs.
