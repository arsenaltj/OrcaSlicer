# Windows One-Click Installer Design

## Goal

Ship the accepted OrcaSlicer AI integration as one Windows x64 installer executable. The installed application must start from the normal OrcaSlicer shortcut without asking the user to extract an archive or run setup/start batch files.

## Selected approach

Use OrcaSlicer's existing CPack/NSIS installer. Extend only the integration and release layer:

- install the production AI Sidecar modules under `resources/tools/ai`;
- reuse Orca's bundled Python 3.12 runtime and Pillow package;
- let `AIServiceManager` start the packaged local Sidecar after the first failed loopback health check;
- keep custom Sidecar endpoints authoritative and provide an environment opt-out;
- write generated models and Sidecar logs below the active Orca data directory, never below `Program Files`;
- terminate only the child Sidecar process owned by the closing Orca instance;
- keep official Orca behavior available if Sidecar startup fails.

This avoids a second installer technology and keeps upstream updates concentrated in the existing CPack install rules and the already-thin AI service composition layer.

## Runtime flow

1. The user launches OrcaSlicer from the desktop or Start menu shortcut created by NSIS.
2. `AIServiceManager` probes the configured loopback health endpoint.
3. If the default local endpoint is unavailable and packaged runtime files exist, the manager launches bundled `pythonw.exe` with an installed bootstrap script.
4. The bootstrap sets `ORCASLICER_AI_OUTPUT_DIR` to `<data_dir>/generated_models`, redirects output to `<data_dir>/log/orca-ai-sidecar.log`, and executes the production Sidecar module.
5. Existing bounded discovery retries detect the Sidecar and enable model generation/assistant capabilities.
6. On shutdown, Orca terminates only the child it started. A Sidecar already running before Orca started is never terminated.

## Safety and compatibility

- Auto-start is Windows-only in this delivery and is skipped when `ORCASLICER_AI_SIDECAR_URL` is set or `ORCASLICER_AI_DISABLE_AUTOSTART=1`.
- No API keys, provider credentials, user profiles, generated models, or machine-specific configuration are packaged.
- The installer does not change 3MF or printer/profile formats.
- Missing Python/runtime/module files produce a log message and leave all official non-AI functionality usable.
- Production packaging excludes tests, benchmarks, paid-validation scripts, BAT launchers, and mock services.

## Verification

- Run Python tests for the installed bootstrap and existing model-generation suite without paid credentials.
- Build the modified C++ objects and full Release installer input.
- Stage `cmake --install` and verify the production manifest contains only the intended AI runtime files.
- Build the NSIS installer, check its SHA-256, install silently into an isolated directory, and verify files/shortcuts/uninstaller.
- Launch the installed executable with an isolated data directory; verify `/health`, writable output/log paths, and clean process shutdown.
- Uninstall silently and verify program files are removed while the isolated user data directory remains untouched.
