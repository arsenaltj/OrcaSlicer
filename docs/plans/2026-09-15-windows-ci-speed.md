# Windows CI packaging optimization and generator experiment

The current default is still MSBuild. This change retains Windows MSVC, PCH,
Release/debug symbols, all tests and the existing downloadable artifacts. It
does not change merge protections, the Team integration caller's four inputs,
required check names, Linux/macOS jobs, or the bot's deployed code.

The reusable workflow input `slicer-generator` accepts `auto`, `msbuild`, and
`ninja-multi`. Explicit selection is limited to Windows x64/MSVC source builds.
Dependencies retain their existing generator and cache key. Existing clang
callers retain their automatic Ninja route. `auto` remains MSBuild for MSVC.
The generator experiment stopped after its first failure; the later normal PR
validated packaging preservation and benefit, as recorded below.

Hosted Windows packaging uses isolated staging directories, at most two
packaging processes, and checked process exits. It removes the intermediate
portable ZIP that was never uploaded; the portable directory artifact remains.
PDB compression changes from `-mx9` to `-mx3`. Installer, PDB and MSIX uploads
disable redundant artifact compression. NSIS and MSIX remain unsigned internal
build artifacts; this workflow does not publish releases. Existing custom
runner behavior and ARM64's omission of PDB remain unchanged.

## Original measurement protocol (stopped)

This protocol is retained as experiment history. The first trial failed package
validation, so the remaining three refs will not run and the failed sample will
not be rerun or replaced. The normal PR packaging pair below is separate evidence.

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

## First run and generator decision

[Run 34935451971, attempt 1](https://github.com/arsenaltj/OrcaSlicer/actions/runs/34935451971)
used experiment SHA `e4ce1d416d0c96972a11526198a4cb06509acc0a` and restored the
dependency cache. The MSBuild source build succeeded. Both packaging profiles
finished with unchanged, equal input manifests and equal MSIX staging manifests.
The package comparison then failed before it could establish payload equality:
the 7-Zip NSIS listing contained empty `Size` values, which the validator tried
to parse as integers. Windows unit tests and normal package uploads were skipped.

The generator experiment is **stopped after this first failure**, as required by
the protocol. There are zero valid generator samples. The other three trial
refs will not be started and the failed sample will not be rerun or replaced.
MSBuild remains the automatic Windows MSVC generator; no Ninja speedup or adoption
is claimed. The original experiment commit and failed run remain the record of
this attempt. The explicit Ninja selector is experimental and has no successful
hosted sample from this experiment.

These measurements are diagnostic observations from the failed run, not accepted
optimization results:

| Measurement | Seconds |
| --- | ---: |
| MSBuild build/link phase | 6262.287 |
| Baseline packaging | 654.727 |
| Optimized packaging | 436.459 |

The apparent 33.34% packaging-time reduction cannot justify delivery until
complete archive equality is verified. The optimized PDB archive was also larger
(209,418,503 bytes versus 167,734,359 bytes); that tradeoff must be retained in the
final result. These numbers do not measure artifact upload savings or an overall
CI speedup. Baseline packaging ran first, so order effects are not excluded.

The repaired validator retains unknown-size entries and extracts them to measure
real lengths and SHA-256 hashes. It preserves integrity, input identity,
declared-size, file-set and content checks. Offline cases cover the observed NSIS
format, changed bytes, missing files and incorrect declared sizes. The successful
normal PR evidence below validates this repair with actual packages; it does not
restart the terminated generator experiment. Fresh checks on the final PR commit
and the actual integration push remain separate delivery requirements. Desktop
and installer execution remain unverified.

## Packaging validation in normal PR CI

The final optimization PR uses its normal Team integration Windows build. A
shared path predicate schedules native CI and selects paired packaging when the
PR changes the Windows packaging scripts, MSIX scripts, relevant workflows,
root CMake configuration or `cmake/` definitions. The selector checks the event's
repository, PR merge ref, checked-out candidate SHA and exact ordered base/head
parents before comparing paths. Unknown or inconsistent applicable PR identity
fails the job. The Team caller retains its four original inputs.

Both packaging profiles reuse that one completed MSBuild installation. Their
input and staging manifests, archive integrity, extracted file sets, lengths
and content evidence must pass before the normal installer, portable, MSIX and
PDB artifacts qualify. Baseline packages and measurements are retained for
inspection. A failed packaging attempt retains diagnostics and any available
packages under an explicitly failed artifact name, while the job remains failed.
The existing Windows unit-test job still depends on a successful build job.

Unrelated PRs, integration pushes, other repositories and other workflow callers
keep ordinary packaging. Linux/macOS scheduling rules and protection remain
unchanged. The additional paired packaging, comparison and baseline-upload time
is validation overhead on packaging changes; it is not part of the optimized
ordinary-PR packaging path. Any reported benefit must identify the actual passing
run and its scope. One passing pair does not establish cross-run medians.

## Normal PR measurement and decision

[PR #11 run 34948961371, attempt 1](https://github.com/arsenaltj/OrcaSlicer/actions/runs/34948961371)
used source Head `867843cab5bf29f7192a07ee97850d1b80dc577b`, integration Base
`4e3037f6f827baa9355ec6a8b9e7cff1a5112bfa` and checked-out merge candidate
`cd6c224c14dea0233ea7637fb05923258cda38f8`. The current Windows build and
`windows_tests / Unit Tests` jobs both succeeded; `AI integration checks` also
succeeded on this Head. Linux and macOS failures are informational under the
unchanged protection, so the overall Team workflow is red despite Windows passing.

The dependency cache was restored. Current build logs identify the
`windows-2025-vs2026` image `20260907.229.1`, Visual Studio 18 2026,
MSVC `19.51.36256.0`, MSBuild `18.9.1` and CMake `4.3.4`. Actual configuration
retained x64, Release, `BUILD_TESTS=ON` and PCH. The build/link phase took
6,568.298 seconds; configure, gettext and install also completed successfully.
SDK `10.0.26100.0` was installed, but the normal build log does not independently
attest the exact selected SDK. This route has no full benchmark environment
fingerprint and supplies no generator comparison.

Both packaging profiles used the same installed input and unchanged file-hashed
input/MSIX staging manifests. Baseline ran first. The measurement includes input
hashing, packaging and publication copies; archive comparison and uploads happen
afterward.

| Measurement | Baseline | Optimized |
| --- | ---: | ---: |
| Packaging elapsed seconds | 741.169 | 464.659 |
| PDB archive bytes | 167,612,943 | 209,285,889 |
| Concurrent packaging processes, maximum | 1 | 2 |
| PDB compression | `-mx9` | `-mx3` |

The independently recalculated saving is **276.510 seconds (37.3073%)** for
this packaging pair. The PDB grows **41,672,946 bytes (24.8626%)**. Retain the
packaging optimization with that size tradeoff; keep MSBuild as the default and
leave the explicit Ninja selector experimental with zero accepted generator
samples. There is no measured overall CI reduction, upload-compression timing
comparison or cross-run median. One baseline-first pair cannot exclude filesystem
cache and ordering effects.

Archive integrity tests and complete inventories matched: 4,271 NSIS files,
4,269 MSIX files and two PDB files. NSIS entries with missing sizes/CRCs were
retained and resolved by extraction, actual lengths and SHA-256 for all 4,271
files, including plugins, resources and `Uninstall.exe`. MSIX/PDB comparison used
their complete file/size/CRC inventories and archive integrity tests; it did not
independently extract and SHA-256 every file in those two formats.

The paired-packaging step took 20m09s, archive validation 56s and baseline upload
8s. Baseline packaging plus those latter checks are validation overhead on a
packaging-changing PR. Ordinary optimized packaging does not repeat the baseline.
Installer, portable, MSIX, PDB, profile validator, candidate test binaries and
baseline packages were retained. Each outer artifact ZIP returned a valid 4 KiB
HTTP range sample. That establishes download availability, not a complete local
download or independent unpacking of the published portable artifact. Complete
measurement and JUnit evidence were downloaded and hash-verified:

| Evidence | Artifact ID | Downloaded archive SHA-256 |
| --- | ---: | --- |
| Windows measurements | `10393212138` | `a3b49bd9750b4cdf005414f8bb9c90267f6b9793e50b4b51ef7968bdd863398d` |
| Windows JUnit | `10393980824` | `b87049f096c225c13896e14a8474e61efdbdf5cf6824413c5d9c3f3062b8b166` |

The real Windows suite reported **882 cases: 877 passed, five skipped, zero
failures/errors**, with CTest elapsed time 111.76 seconds. Current JUnit records
explicit missing-numpy reasons for all five existing array-dependent cases in
`test_slicing_pipeline_bindings.cpp`: readonly rows, writable rows, Polygon
array access, ExtrusionPath points and SurfaceCollection geometry. These remain
unexecuted array coverage. The Polygon case ran six non-array assertions before
the conditional skip; the whole case is still reported skipped.

The full named inventory and status multiplicities match historical Windows run
`34846403588`; tests, the CTest launcher and the unit workflow are unchanged from
Base. Registration IDs/order differ, and the two existing `init_print`
registrations are both preserved. No tests, assertions or filters were removed
or relaxed. This establishes preservation of the existing hosted suite; desktop
flows, installer execution, ARM64 and other workflow callers remain unverified.

This section records measurement evidence and the packaging/default-generator
decision. It does not substitute these earlier checks for fresh normal CI on
the final documentation commit, a protected merge, or verification of the actual
integration push. Those delivery results must be read from the final PR and
integration CI records.
