# Optional native Moonraker host transport

The Device SDK remains a protocol library. Android/iOS applications consume its public C ABI and own their platform network/JNI adapters. A C++ host that needs a concrete network implementation may additionally link `OrcaSlicer::MoonrakerTransport`.

## Build boundary

`OrcaSlicer::MobileDeviceSDK` and `OrcaSlicer::MobileDeviceSDKShared` compile the protocol adapter, state, commands, tasks and events without CURL, OpenSSL or Boost.Beast. `OrcaSlicer::MoonrakerTransport` is a separate static target containing HTTP/multipart and WebSocket/TLS implementations. It is not linked into either Device C ABI artifact, `OrcaSlicer::Core`, the Slicing SDK, or the mobile JNI adapter.

The default is `ORCA_MOONRAKER_CONCRETE_TRANSPORT=OFF`:

- `OFF` skips network dependency discovery.
- `AUTO` enables the host target only when all dependencies are available; otherwise the protocol targets continue to build.
- `ON` requires Boost.Beast/Asio headers, CURL::libcurl and OpenSSL::SSL/Crypto and fails configuration if they are missing.

Use verified dependency packages via CMAKE_PREFIX_PATH and OPENSSL_ROOT_DIR. Installed Boost headers are detected normally. `ORCA_MOONRAKER_BOOST_SOURCE_DIR` optionally points at a pinned modular Boost source tree; only this host target and its tests receive those header paths. No production dependency is downloaded by this CMake entry.

For example, a host SDK-only configure can use:

```text
cmake -S . -B build-native-host -DORCA_MOBILE_DEVICE_SDK_ONLY=ON -DBUILD_TESTS=ON -DORCA_MOONRAKER_CONCRETE_TRANSPORT=ON -DCMAKE_PREFIX_PATH=<verified-prefix>
cmake --build build-native-host --config Release --target moonraker_transport_tests mobile_sdk_tests mobile_sdk_cabi_tests
ctest --test-dir build-native-host -C Release --output-on-failure
```

On Windows the runtime DLLs supplied by these dependency packages must be on PATH when running tests or a native host. This entry does not install a host's runtime DLLs into the mobile app. Local validation dependency patches, compiler identity, source archives and hashes are recorded in task evidence, not silently substituted for official desktop dependency builds.

## C++ host API

```cpp
#include <orca/connectivity/moonraker/MoonrakerTransport.hpp>
using namespace Orca::MobileDevice;

MoonrakerTransportOptions options;
options.api_key = host_credentials.api_key;
// TLS verification is enabled by default. A custom trust file is optional.
options.ca_file = host_credentials.ca_file;
auto http = std::make_shared<MoonrakerHttpTransport>(options);
auto websocket = std::make_shared<MoonrakerWebSocketTransport>(options);
MoonrakerAdapter adapter(http, websocket, options.api_key);
```

The `host_credentials` values in the example are provided by the embedding host; they are not SDK configuration globals. The public header exposes no Beast, CURL, OpenSSL, desktop GUI or core slicing types.

HTTP requests have connection and total time limits and support cancellation of in-flight operations. Uploads verify the requested SHA-256 before transmission and stream local UTF-8/file-URI paths through the host filesystem. Response buffers are bounded; C callbacks do not propagate C++ exceptions.

Each WebSocket connection owns a serial asynchronous executor. DNS, TCP/TLS/upgrade, reads, queued writes and cancellation run there. Opening and send operations have explicit timers; an idle read timeout is opt-in, since a quiet printer connection is valid. TLS verifies both trust and the requested hostname on the actual SSL stream. Explicit close suppresses stale close callbacks. Closing or reconnecting from a message callback does not wait on the old worker itself. A `send()` from that callback acknowledges queue admission; other callers wait for the write result.

Adapter probe/subscribe lifecycles are serialized independently of the state mutex. Status callbacks only take the state lock, so teardown does not hold it while waiting for transport callbacks. A later subscription cannot be closed by an earlier stale lifecycle. Destroying the adapter still requires the host to stop initiating new public calls, as with ordinary C++ object lifetime rules.

## Regression scope

Tests cover production protocol/C ABI behavior and local HTTP/HTTPS/WS/WSS servers, including upload SHA, non-ASCII file paths, certificate trust/hostname rejection, silent handshakes, quiet connections, cancellation and callback/concurrent reconnects. Test certificates and keys are generated in memory at runtime; only temporary public CA certificates are written and removed. Loopback results do not claim connection to a physical printer, Android JNI integration of this optional backend, or print quality acceptance.
