# Windows One-Click Installer Design

> **Status note (2026-08-27):** This document originally specified a package with no provider credentials. That credential decision has been superseded for the controlled **internal beta only** by the locked internal configuration. A locked client configuration is extractable and is not a commercial security boundary; it must not be distributed publicly or promoted into a commercial release. Public/commercial installers must contain no provider key and must use the commercial Provider Gateway defined by `docs/architecture/ADR-004-commercial-ai-product-line.md`. The installer lifecycle and local-runtime decisions below otherwise remain applicable.

## Goal

Ship the accepted OrcaSlicer AI integration as one Windows x64 installer executable. The installed application must start from the normal OrcaSlicer shortcut without asking the user to extract an archive or run setup/start batch files.

## Selected approach

Use OrcaSlicer's existing CPack/NSIS installer. Extend only the integration and release layer:

- install the production AI Sidecar modules under `resources/tools/ai`;
- reuse Orca's bundled Python 3.12 runtime and Pillow package;
- let `AIServiceManager` start the packaged local Sidecar after the first failed loopback health check;
- use the Sidecar v8 / protocol-v2 per-launch challenge/proof session and a local-only HTTP transport that disables proxies and redirects;
- keep custom Sidecar endpoints authoritative and provide an environment opt-out;
- write generated models and Sidecar logs below the active Orca data directory, never below `Program Files`;
- terminate only the child Sidecar process owned by the closing Orca instance;
- keep official Orca behavior available if Sidecar startup fails.

This avoids a second installer technology and keeps upstream updates concentrated in the existing CPack install rules and the already-thin AI service composition layer.

## Runtime flow

1. The user launches OrcaSlicer from the desktop or Start menu shortcut created by NSIS.
2. `AIServiceManager` issues an anonymous loopback challenge, verifies the Sidecar proof, and only then sends authenticated health/business requests; raw session credentials are not sent as HTTP headers.
3. If the default local endpoint is unavailable and packaged runtime files exist, the manager launches bundled `pythonw.exe` with an installed bootstrap script and a child-only per-launch session secret.
4. The bootstrap sets `ORCASLICER_AI_OUTPUT_DIR` to `<data_dir>/generated_models`, redirects output to `<data_dir>/log/orca-ai-sidecar.log`, and executes the production Sidecar module.
5. Existing bounded discovery retries detect the Sidecar and enable model generation/assistant capabilities.
6. On shutdown, Orca terminates only the child it started. A Sidecar already running before Orca started is never terminated.

## Safety and compatibility

- Auto-start is Windows-only in this delivery and is skipped when `ORCASLICER_AI_SIDECAR_URL` is set or `ORCASLICER_AI_DISABLE_AUTOSTART=1`.
- The supported commercial AI target is currently Windows only. macOS/Linux support requires separate Sidecar packaging, signing and qualification before it can be advertised.
- Public/commercial packages contain no API keys or provider credentials. The locked internal-beta package is a temporary, access-controlled exception and is not a commercial deliverable.
- The build channel value `commercial` is candidate metadata and a credential-exclusion guard, not approval to publish; the commercial release gates remain separate.
- User profiles, generated models and machine-specific configuration are never packaged.
- The installer does not change 3MF or printer/profile formats.
- Missing Python/runtime/module files produce a log message and leave all official non-AI functionality usable.
- Production packaging excludes tests, benchmarks, paid-validation scripts, BAT launchers, and mock services.

## Verification

- Run Python tests for the installed bootstrap and existing model-generation suite without paid credentials.
- Build the modified C++ objects and full Release installer input.
- Stage `cmake --install` and verify the production manifest contains only the intended AI runtime files.
- Build the NSIS installer, check its SHA-256, install silently into an isolated directory, and verify files/shortcuts/uninstaller.
- Launch the installed executable with an isolated data directory; verify `/health`, writable output/log paths, and clean process shutdown.
- Exercise hostile/stale loopback listeners, missing/invalid proofs, replay attempts, parent exit and Sidecar restart; confirm AI fails closed while normal Orca remains usable.
- Uninstall silently and verify program files are removed while the isolated user data directory remains untouched.
