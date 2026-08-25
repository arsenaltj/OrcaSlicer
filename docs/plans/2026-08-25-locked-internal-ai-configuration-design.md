# Locked Internal AI Configuration Design

## Problem

The first internal installer treated packaged provider settings as fallbacks. Existing process, user, or machine environment variables therefore remained authoritative. A coworker with stale `OPENAI_BASE_URL`, OpenAI credentials, or Tripo credentials could install the same package and still reach a different or unavailable service.

## Decision

Make the explicitly internal payload authoritative for provider configuration:

- mark generated payloads with `mode: internal_locked`;
- require the OpenAI key, OpenAI-compatible base URL, and Tripo key at package time;
- include the verified official Tripo base URL when no explicit Tripo base is configured;
- overwrite only the allow-listed provider variables when the validated locked payload is present;
- expose only `runtime.configuration_mode: internal_locked` in health and diagnostics, never credential values;
- preserve the existing environment-driven behavior when no internal payload is packaged.

The packaged configuration remains outside Git and is injected only into the final internal installer. This is intentionally distribution security rather than secret protection: recipients of the installer can extract the embedded credentials.

## Compatibility and failure policy

- The normal/public build path contains no payload and keeps all existing environment behavior.
- Provider configuration changes only inside the packaged AI Sidecar process; no global Windows environment variables are written.
- The local endpoint remains `127.0.0.1:18764` and generated files remain under `<data_dir>/generated_models`.
- Missing, malformed, oversized, or non-locked payloads fail closed and leave official Orca functionality available.
- MainFrame, Plater, smart slicing, 3MF, profiles, dependencies, and provider request behavior are unchanged.
- Verification must not submit image or model generation requests to paid providers.

## Verification

- Unit-test locked override precedence, payload validation, complete-config enforcement, and default Tripo endpoint injection.
- Run all local AI Python tests and C++ `[AI]` tests.
- Build Release and create a new clearly named internal locked installer.
- Start the packaged Sidecar after deliberately setting conflicting environment values; `/health` must report `internal_locked`, the verified credential-free OpenAI host, and both capabilities available.
- Confirm Git and logs do not contain real credential values.
