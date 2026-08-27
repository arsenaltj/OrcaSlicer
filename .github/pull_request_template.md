# Summary / 变更摘要

<!-- Explain the user-visible outcome, motivation, failure fallback and rollback plan. -->

## Development lane / 开发线

- [ ] Orca upstream synchronization
- [ ] Model generation
- [ ] Smart slicing
- [ ] Integration/shared GUI
- [ ] Shared Sidecar runtime/authentication/network transport
- [ ] Diagnostics, build or release

Accepted input (required for feature integration):

- Source branch:
- Accepted full 40-character SHA:
- Previous accepted SHA:
- Pinned Orca upstream SHA:
- Integration receipt commit (for snapshot-port history):
- Verified feature Git objects/paths:

> Integration must consume the accepted SHA above. Do not replace it with a moving branch HEAD.

## Boundaries and compatibility / 边界与兼容性

- Shared Orca files changed (`MainFrame`, `Plater`, CMake, adapters, workflows):
- Reason each shared-file change is necessary:
- [ ] Model generation does not copy or call smart-slicing implementation code.
- [ ] Smart slicing does not copy or call model-provider implementation code.
- [ ] No reverse merge or port from the integration branch into a feature branch.
- [ ] Orca default behavior is unchanged when AI is disabled or unavailable.
- [ ] 3MF and profile formats/defaults are unchanged, or migration is documented and tested.
- [ ] Sidecar/provider failure safely degrades without corrupting the workspace.
- [ ] Shared runtime changes have integration-owner review and do not move feature policy into `AIServiceManager`, `AISidecarClient` or generic HTTP transport.

## Runtime and distribution / 运行时与发布

- Sidecar protocol/version:
- Product and development ports:
- Configuration/dependency changes:
- Output-directory changes:
- Installer/update changes:
- [ ] No API key, credential, generated model or machine-specific path is committed.
- [ ] No paid API was called, or the approved scope and result are documented below.
- [ ] Native loopback requests disable proxies and redirects; session challenge/proof and stale-listener behavior are covered by negative tests.
- [ ] A `commercial` build-channel value is treated only as candidate metadata; public release approval/evidence is recorded separately.
- [ ] AI commercial support remains Windows-only, or platform-specific packaging/signing/qualification evidence is linked.

## Verification / 验证

- Python tests:
- C++ tests:
- Windows Release build:
- GUI journeys and screenshots/recordings:
- AI-disabled and Sidecar-offline checks:
- Old 3MF/profile compatibility checks:
- Installer/install/uninstall checks:
- Rollback point or last-known-good SHA:

<!-- Attach UI evidence for visible changes and link the relevant CI run/artifact. -->
