# Internal API Defaults Design

## Goal

Produce an explicitly internal Windows installer that starts the packaged AI Sidecar with the organization's existing OpenAI-compatible preprocessing and Tripo model-generation credentials, without asking each coworker to configure environment variables.

## Selected approach

Keep credentials outside Git and inject them only while staging an internal installer:

- a small packaging helper reads an allow-listed set of values from the build machine environment and writes a bounded JSON payload outside the source tree;
- an opt-in CMake file-path setting installs that payload as `resources/tools/ai/orca_ai_internal_defaults.json` only for the internal package;
- the installed bootstrap validates the payload and applies each value with `os.environ.setdefault`, so explicit process/user/machine configuration remains authoritative;
- public/default builds leave the setting empty and contain no payload or credentials;
- diagnostics report only configured booleans and sanitized endpoint metadata, never values.

The allowed settings are `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_TEXT_MODEL`, `OPENAI_IMAGE_MODEL`, `TRIPO_API_KEY`, `TRIPO_API_BASE`, and `TRIPO_MODEL`.

## Safety and compatibility

- Anyone who receives the internal installer can technically extract its embedded credentials. Distribution and revocation are therefore operational controls, not cryptographic guarantees.
- No credential value appears in source, commits, build output, command lines, or application logs.
- Invalid, oversized, missing, or unreadable defaults are ignored safely; Orca continues with its existing non-AI behavior.
- The payload is only a fallback. Existing explicit environment configuration wins.
- No provider request is made during packaging or verification.
- Ports, output directories, dependencies, 3MF files, profiles, and official Orca defaults are unchanged.

## Verification

- Unit-test allow-listing, malformed/oversized input, environment precedence, and absence of secret values in status output.
- Generate a payload from test values and verify the helper never prints values.
- Run the complete local AI Python suite without paid provider calls.
- Configure and build Release with an external real payload, stage the installer, and verify the expected payload exists without displaying it.
- Start the staged Sidecar and inspect only `/health` configured booleans and sanitized endpoint host; do not submit generation jobs.
