# Orca mobile SDK core

This directory contains the platform neutral contract layer for the mobile reuse work.

The `OrcaSlicer::MobileSDK` target intentionally has no wxWidgets, Python, GUI, desktop filesystem, or vendor plugin dependency. It provides:

- printer capabilities, status, protocol adapter boundaries, and LAN/P2P/relay path selection;
- a connection lifecycle state machine and an idempotent command envelope;
- model generation jobs and artifact manifests for a remote AI Gateway;
- slicing requests, progress, results, and a local/remote engine boundary.

Concrete HTTP, WebSocket, WebRTC, cloud, provider, iOS, Android, and desktop adapters belong in separate targets. The public DTOs use strings, standard containers, and `nlohmann::json` so Kotlin, Swift, and C ABI bindings can be added without exposing Orca GUI types.
