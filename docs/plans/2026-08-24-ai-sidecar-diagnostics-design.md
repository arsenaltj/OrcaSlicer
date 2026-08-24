# AI Sidecar diagnostics design

## Problem

Installed clients currently collapse DNS, proxy, TLS, timeout and connection failures into one user-facing message. The bootstrap appends stdout and stderr to one unbounded file, but the worker does not emit enough context to identify the failing stage.

## Decision

Add a small production-only diagnostics module used by the Sidecar and provider client. It writes newline-delimited JSON events to the existing per-user Sidecar log through stdout/stderr. Each worker runs inside a job-ID context so support can correlate a GUI failure with backend events.

The installed bootstrap rotates the log at startup at 5 MiB and retains three backups. The model-generation panel always exposes an `Open diagnostic logs` button and includes the job UUID in failed status text.

## Logged fields

- UTC timestamp, level, event, process ID and thread name
- job UUID, worker/stage and elapsed milliseconds
- credential-free HTTPS scheme/host/port/path
- HTTP status, selected safe request-ID headers and proxy scheme names
- bounded exception type/message/errno chain
- Python, OpenSSL and Sidecar versions plus configured-capability booleans

## Never logged

- API keys, Authorization headers or environment values
- prompts, request/response bodies or uploaded image contents
- endpoint credentials, query strings or fragments
- generated artifact contents

## Compatibility and failure policy

Diagnostics are observational only. They do not change provider requests, retry policy, port, output layout, 3MF/profile formats or Orca defaults. If diagnostics cannot format or write an event, the production workflow continues. No paid provider is called by the tests.
