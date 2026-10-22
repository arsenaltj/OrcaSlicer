# Orca mobile SDK core

`src/orca_mobile_device_sdk` contains the platform neutral contract layer for mobile hosts, desktop integration code, and protocol adapters. The public SDK is C++17 code with standard library and `nlohmann::json` dependencies. It does not depend on wxWidgets, Python, GUI targets, desktop user directories, vendor DLLs, or commercial service code.

The v1.7 connection contract is **LAN → RemoteBroker → Relay**. `P2p` remains only as a deprecated C++ source compatibility alias for `RemoteBroker`; it is not emitted by the v1.7 JSON contract and classic ICE/TURN P2P is a phase-three optional capability.

## Build artifacts

| Logical artifact | CMake target | Consumer | Status and boundary |
|---|---|---|---|
| C++ static SDK | `orca_mobile_device_sdk` / `OrcaSlicer::MobileDeviceSDK` | Native C++ hosts and tests | Buildable contract library. Platform output is a static library (`.lib` or `.a`). |
| C ABI shared SDK | `orca_mobile_device_sdk_ffi` / `OrcaSlicer::MobileDeviceSDKShared` | C11, Android, Flutter/Dart FFI | Buildable poll-first facade. The canonical Unix and Android filename is `liborca_mobile_device_sdk_c.so`; Windows produces `orca_mobile_device_sdk_c.dll` and its import library. |
| WonderMaker LAN adapter | `orca_connectivity_wondermaker` / `OrcaSlicer::WonderMakerAdapter` | LAN integration and simulator tests | Injected HTTP, WebSocket, and discovery boundary. Current automated evidence is simulator-only; it is not a real-device or vendor-protocol acceptance result. |
| Moonraker compatibility adapter | `orca_connectivity_moonraker` / `OrcaSlicer::MoonrakerAdapter` | Compatibility and Fake transport tests | Retained as an SDK abstraction and test baseline. It is not the WonderMaker phase-one LAN implementation. |
| Android native package | `liborca_mobile_device_sdk_c.so` in `arm64-v8a`, `armeabi-v7a`, and `x86_64` | `orca-mobile` Android FFI loader | Three ABI files with the same logical artifact name. `sdk.lock.json` records identity, hash, size, API level, and NDK metadata. |
| Apple native package | XCFramework | iOS/macOS hosts | Deferred. No iOS/macOS/XCFramework artifact is part of the v1.7 Android delivery. |

The C++ and C ABI targets compile the same public source set. The shared target changes the output filename through CMake `OUTPUT_NAME`; the logical target remains `orca_mobile_device_sdk_ffi`.

Android artifact files are packaged under the ABI-specific `jniLibs` directories. The checked-in mobile lock records the ABI, contract, Android API, NDK and source identity. Its exact source commit is the value in sdk.lock.json. The lock must be regenerated after selecting the final OrcaSlicer integration commit; it must not be treated as proof that a stale artifact matches a newer SDK worktree.

## Public C++ entry points

Include the single entry point when a host needs the complete contract:

```cpp
#include <orca/mobile/device/DeviceSDK.hpp>
```

`DeviceSDK.hpp` includes these public headers:

- `CommandEnvelope.hpp`: versioned, expiring, idempotent command envelopes.
- `Connectivity.hpp`: connection paths, monitoring states, printer DTOs, transfer/session DTOs, path selection, state machine, and protocol adapter interface.

`DeviceSDK.hpp` also exposes the contract name/version and generated source identity helpers: `kMobileSdkContractName`, `kMobileSdkContractMajor`, `kMobileSdkContractMinor`, `mobile_sdk_source_commit()`, and `mobile_sdk_source_commit_short()`.

The public C++ headers use canonical SDK values only. WonderMaker wire fields, MQTT topics, WebSocket private fields, certificate objects, file-server fields, and vendor error codes stay inside adapter implementations.

## C ABI entry point

C and FFI consumers include:

```c
#include <orca/mobile/device_sdk_c.h>
```

The C ABI uses opaque `orca_mobile_device_t`, `orca_mobile_device_session_t`, and `orca_mobile_device_task_t` handles; fixed-width integers; explicit UTF-8 byte spans; stable negative error codes; and `struct_size` checks for forward-compatible structures. It is a poll-first API and does not require a cross-thread callback for一期 use.

The 20 exported functions are grouped as follows:

- Identity and errors: `orca_mobile_device_abi_version`, `orca_mobile_device_contract_version`, `orca_mobile_device_source_commit`, `orca_mobile_device_source_commit_short`, `orca_mobile_device_error_message`.
- Span-based identity queries: `orca_mobile_device_get_abi_version`, `orca_mobile_device_get_contract_version`, `orca_mobile_device_get_source_commit`.
- SDK lifetime: `orca_mobile_device_create`, `orca_mobile_device_destroy`.
- Session lifetime and asynchronous work: `orca_mobile_device_session_create`, `orca_mobile_device_session_destroy`, `orca_mobile_device_session_submit_task`, `orca_mobile_device_task_cancel`, `orca_mobile_device_task_destroy`, `orca_mobile_device_task_get_id`, `orca_mobile_device_task_get_state`.
- Event delivery: `orca_mobile_device_poll_event`, `orca_mobile_device_event_clear`, `orca_mobile_device_wakeup`.

The v1.7 facade accepts these task kinds: `SYNC` for the compatibility baseline, plus JSON-payload `DISCOVER`, `CONNECT`, `DISCONNECT`, `STATUS`, `COMMAND`, and `TRANSFER`. The facade validates task kind, UTF-8, JSON, structure size, cancellation, timeout, event ownership, and handle lifetime. The current facade is the stable ABI boundary; it does not claim that every task kind is already wired to a real device operation.

See [Docs/mobile-sdk/API.md](../../Docs/mobile-sdk/API.md) for the public C++ types, C ABI structures, lifecycle rules, adapter boundary, and JSON compatibility details.

## Connectivity and monitoring model

The v1.7 path model is:

1. `ConnectionPath::Lan`: direct local-network access.
2. `ConnectionPath::RemoteBroker`: the remote broker path reserved for the MQTT/TLS remote design.
3. `ConnectionPath::Relay`: broker or cloud relay fallback.

`ConnectionPathSelector` preserves a healthy current path and falls back in that order. A path change, disconnect, or expired status requires a fresh state synchronization before a host retries non-idempotent commands. `ConnectionStateMachine` represents discovery, probing, pairing, authentication, connecting, connected, state syncing, expired, switching, and offline states.

`PrinterCapability`, `PrinterStatus`, `TransferJob`, and `RemoteSession` carry device pairing, certificate-authentication capability, status stream metadata, device time, event sequence, synchronization time, state-expired state, resumable transfer state, and session lease state without embedding a vendor protocol.

## WonderMaker LAN boundary

`OrcaSlicer::WonderMakerAdapter` is the current v1.7 LAN integration boundary. Its injected transports cover:

- discovery through an `IWonderMakerDiscoveryTransport` implementation or a manually supplied `DeviceEndpoint`;
- HTTP pairing, authentication, capabilities, status, control, and file-transfer requests;
- WebSocket status messages and close notifications;
- HTTP status polling when the WebSocket is unavailable;
- reconnect and full status reconciliation before status streaming resumes;
- command de-duplication by `command_id`;
- chunk offset, digest, and final completion checks for file transfer.

`WonderMakerSimulator` implements the same injected interfaces in-process and is used for deterministic discovery, pairing, authentication, status, control, disconnect, reconnect, restart, and resumable-transfer tests. The simulator exercises the v1.7 monitoring targets of entering stale state after 10 seconds without synchronization and reconciling within 15 seconds after recovery. Simulator results do not prove a real WonderMaker firmware, port, field name, certificate flow, or production device path.

The current vendor candidate is WebSocket plus HTTP on LAN. The planned device-side MQTT broker and file-server protocol, remote MQTT/TLS broker ownership, and production relay endpoints remain subject to protocol and firmware freeze. They must not be documented as completed transports.

`OrcaSlicer::MoonrakerAdapter` remains a separate compatibility/Fake transport baseline. It is useful for exercising the common `IPrinterProtocolAdapter` boundary, but it does not count toward WonderMaker phase-one device acceptance.

## Scope and delivery gates

The v1.7 code-complete gate covers the public contract, C ABI facade, Android artifact format, injected LAN transport boundary, simulator, monitoring state transitions, reconnect reconciliation, and automated tests.

Real-device acceptance is a separate gate requiring the specified WonderMaker hardware and firmware, frozen protocol documentation, device-level pairing and certificate behavior, discovery, state/control receipts, file transfer, restart recovery, and an on-device print loop. Until those inputs are available, the correct status is **code complete; real-device acceptance pending**.

AI Gateway clients, real cloud slicing, production remote MQTT/TLS, video transport, and classic ICE/TURN P2P are outside the一期 SDK delivery. P2P remains a phase-three optional extension unless a later supplier decision explicitly moves it into a remote MVP gate.

For detailed interfaces, use [the MobileDeviceSDK API reference](../../Docs/mobile-sdk/API.md). For the repository-level mobile guide, see [Docs/mobile-sdk/README.md](../../Docs/mobile-sdk/README.md).
