# MobileSDK API 参考

本文档对应 `src/orca_sdk` 当前公开头文件和 v1.7 SDK 任务基线。源目录入口见 [`src/orca_sdk/README.md`](../../src/orca_sdk/README.md)。

## 1. 产物与构建 target

| 逻辑产物 | CMake target | 公开入口 | 状态 |
|---|---|---|---|
| C++ 静态 SDK | `orca_mobile_sdk` / `OrcaSlicer::MobileSDK` | `orca/sdk/MobileSDK.hpp` | 可构建，用于 C++ 宿主和测试 |
| C ABI 共享 SDK | `orca_mobile_device_sdk_ffi` / `OrcaSlicer::MobileDeviceSDKShared` | `orca/mobile/device_sdk_c.h` | 可构建；输出逻辑名为 `orca_mobile_sdk_c` |
| WonderMaker LAN adapter | `orca_connectivity_wondermaker` / `OrcaSlicer::WonderMakerAdapter` | `orca/connectivity/wondermaker/WonderMakerAdapter.hpp` | 注入式 LAN 边界；自动化证据为 simulator-only |
| Moonraker compatibility adapter | `orca_connectivity_moonraker` / `OrcaSlicer::MoonrakerAdapter` | `orca/connectivity/moonraker/MoonrakerAdapter.hpp` | Fake/兼容性测试基线，不计入 WonderMaker 一期真实设备验收 |

C++ 静态库的文件后缀由平台决定。C ABI 共享库的 canonical Unix/Android 文件名是 `liborca_mobile_device_sdk_c.so`；Windows 构建生成 `orca_mobile_sdk_c.dll` 和对应导入库。Android 打包为三个 ABI 文件：

- `arm64-v8a/liborca_mobile_device_sdk_c.so`
- `armeabi-v7a/liborca_mobile_device_sdk_c.so`
- `x86_64/liborca_mobile_device_sdk_c.so`

`orca-mobile/sdk.lock.json` 记录每个 ABI 的 artifact 路径、SHA-256、文件大小、ELF 架构、ABI/contract version、OrcaSlicer source commit、Android API 和 NDK 版本。当前 checked-in lock 记录 ABI、contract、Android API、NDK 和 source commit；source commit 的具体值以 sdk.lock.json 为准。在最终 OrcaSlicer 集成提交确定后，必须重新生成并验证 lock，旧 lock 不能证明它已经匹配更新后的 SDK worktree。

iOS、macOS、Xcode、CocoaPods、Swift bridge 和 XCFramework 当前为 deferred，不属于一期 Android 交付。

## 2. C++ 统一入口

```cpp
#include <orca/sdk/MobileSDK.hpp>
```

`MobileSDK.hpp` 包含：

- `CommandEnvelope.hpp`
- `Connectivity.hpp`
- `ModelGeneration.hpp`
- `Slicing.hpp`

并公开：

- `kMobileSdkContractName`，当前为 `orca.mobile-sdk/v1`；
- `kMobileSdkContractMajor` 和 `kMobileSdkContractMinor`；
- `mobile_sdk_source_commit()`；
- `mobile_sdk_source_commit_short()`。

宿主应将 ABI version、contract version 和 source commit 写入诊断或启动日志，并在加载 Android artifact 时与 `sdk.lock.json` 比对。

## 3. Connectivity.hpp

### 3.1 连接路径

```cpp
enum class ConnectionPath : uint8_t {
    Lan = 0,
    RemoteBroker = 1,
    P2p = RemoteBroker, // deprecated source compatibility alias
    Relay = 2,
};
```

`P2p` 是旧源码的兼容别名，不是 v1.7 的独立运行时路径。v1.7 JSON 只输出 `lan`、`remote_broker` 和 `relay`；旧输入 `p2p` 可以被兼容解析为 `remote_broker`。经典 ICE/TURN P2P 是三期可选能力。

`ConnectionPathSelector::select()` 按以下优先级选择可用路径，并对当前健康路径保持粘性：

```text
LAN → RemoteBroker → Relay
```

`PathObservation` 包含路径、可达性、认证状态、延迟和连续失败次数；`PathSelection` 返回选中路径、可用性、是否改变以及是否需要重新同步状态。

### 3.2 连接状态机

`ConnectionStateMachine` 的状态值保持原有 `Disconnected=0`、`Discovering=1`、`Probing=2`、`Connecting=3`、`Connected=4`、`Switching=5`、`Offline=6`，v1.7 在其后追加：

- `Pairing=7`
- `Authenticating=8`
- `StateSyncing=9`
- `Expired=10`

主要操作为：

- `reset()`；
- `begin_discovery()`；
- `begin_probe()`；
- `begin_pairing()`；
- `begin_authentication()`；
- `select_path()`；
- `mark_connecting()`；
- `mark_connected()`；
- `begin_state_sync()`；
- `mark_state_synchronized()`；
- `mark_state_expired()`；
- `mark_disconnected()`；
- `mark_offline()`。

路径切换、断开或状态过期后，`state_sync_required()` 必须为 true，宿主应先完成完整状态查询和对账，再恢复非幂等控制命令。

### 3.3 公共 DTO 和 adapter 接口

`Connectivity.hpp` 的公共类型包括：

- `DeviceEndpoint`：scheme、host、port、TLS 标记和证书指纹；
- `PrinterCapability`：状态流、设备配对、证书认证、发现、G-code 上传、可续传上传和开始/暂停/恢复/停止能力；
- `PrinterStatus`：设备状态、进度、温度、当前文件、诊断码、设备端时间、事件序号、最后同步时间、过期标记、连接路径和协议版本；
- `TransferJob`：文件摘要、总大小、已上传偏移、传输状态、重试策略和设备下载状态；
- `RemoteSession`：设备、平台 session、设备证书标识、连接路径、provider、租约和同步标记；
- `PrinterOperationResult`、`GcodeUploadRequest`、`PrintCommand`；
- `IPrinterProtocolAdapter`：`id()`、`probe()`、`capabilities()`、`get_status()`、`upload_gcode()`、`start_print()`、`pause()`、`resume()`、`cancel()`。

所有这些 DTO 都支持 JSON 转换，并忽略未知扩展字段。供应商 MQTT topic、WebSocket 私有字段、文件服务器字段和厂商错误码不得进入这些公共头文件。

## 4. CommandEnvelope.hpp

命令协议版本是 `orca.command/v1`。`CommandEnvelope` 的字段为：

| 字段 | 语义 |
|---|---|
| `protocol_version` | 协议版本，必须是 `orca.command/v1` |
| `session_id` | 连接会话标识 |
| `device_id` | 目标设备标识 |
| `command_id` | 幂等命令标识，同时用于重复命令对账 |
| `sequence` | 会话内递增序号，必须为正数 |
| `expires_at_epoch_seconds` | 过期时间 |
| `type` | 命令类型，例如 `printer.pause` |
| `payload` | 命令参数 JSON |

发送前调用 `validate(now_epoch_seconds, &error)`。`CommandValidationError` 覆盖缺少协议版本、协议版本不支持、缺少 session/device/command/type、无效序号和过期。`is_retry_of()` 只有在 session、device、command ID 和 payload 都匹配时才返回 true。

## 5. ModelGeneration.hpp

`IModelGenerationClient` 是供应商中立的模型任务边界，提供：

- `create_text_job()`；
- `create_image_job()`；
- `get_status()`；
- `stop()`；
- `get_artifact()`；
- `remove()`。

`ModelJob`、`ArtifactManifest`、`TextModelRequest` 和 `ImageModelRequest` 只描述任务、产物和请求 DTO。真实 AI Gateway、Provider 凭据、配额、计费和 HTTP 实现由外部 adapter 注入，不属于一期设备 LAN 验收。

## 6. Slicing.hpp

`SlicingRequest` 支持 STL、OBJ、3MF、STEP 输入，`Local` 或 `Remote` 执行模式，以及打印机、工艺、材料、引擎版本和 plate 索引。`SlicingResult` 提供 G-code URI、摘要、状态、警告、错误、层数、材料体积和耗时；`SlicingProgress` 通过 `SlicingProgressCallback` 报告进度。

`ISlicingEngine` 的操作为 `validate()`、`slice()` 和 `cancel()`。本接口是 DTO 和 engine boundary，不代表当前 SDK 已经交付真实 AI Gateway 或云端切片引擎。

## 7. C ABI：结构和函数

### 7.1 ABI 规则

```c
#include <orca/mobile/device_sdk_c.h>
```

C ABI 版本常量为 `ORCA_SDK_ABI_VERSION_MAJOR=1`、`ORCA_SDK_ABI_VERSION_MINOR=0`。字符串和字节使用显式长度 span，不要求 NUL 结尾：

- `orca_sdk_string_t`：`const char* data` + `uint32_t size`；
- `orca_sdk_bytes_t`：`const uint8_t* data` + `uint32_t size`。

公共结构均以 `struct_size` 开头。调用方应填入实际结构大小；库会拒绝小于最小结构的请求，并对被截断的未来字段使用兼容默认值。句柄是 opaque 类型，不能由调用方释放或复制内部内存。

稳定错误码包括：`ORCA_SDK_OK`、`ORCA_SDK_E_INVALID_ARGUMENT`、`ORCA_SDK_E_INVALID_HANDLE`、`ORCA_SDK_E_STRUCT_TOO_SMALL`、`ORCA_SDK_E_BUFFER_TOO_SMALL`、`ORCA_SDK_E_TIMEOUT`、`ORCA_SDK_E_CANCELED`、`ORCA_SDK_E_ALREADY_COMPLETE`、`ORCA_SDK_E_BUSY`、`ORCA_SDK_E_UNSUPPORTED`、`ORCA_SDK_E_WAKEUP`、`ORCA_SDK_E_OUT_OF_MEMORY`、`ORCA_SDK_E_INTERNAL` 和 `ORCA_SDK_E_ABI_MISMATCH`。

### 7.2 公共结构

- `orca_sdk_options_t`：SDK 创建选项；
- `orca_sdk_session_options_t`：session 选项和可选 device ID；
- `orca_sdk_task_request_t`：`struct_size`、task kind 和 UTF-8 JSON payload；
- `orca_sdk_event_t`：事件类型、状态、序号、时间戳、task、task ID、消息和 payload；
- `orca_sdk_t`、`orca_sdk_session_t`、`orca_sdk_task_t`：opaque handles。

Task kind 为：

| 常量 | 数值 | 用途 |
|---|---:|---|
| `ORCA_SDK_TASK_KIND_SYNC` | 1 | 兼容基线 task |
| `ORCA_SDK_TASK_KIND_DISCOVER` | 2 | 发现 facade |
| `ORCA_SDK_TASK_KIND_CONNECT` | 3 | 连接 facade |
| `ORCA_SDK_TASK_KIND_DISCONNECT` | 4 | 断开 facade |
| `ORCA_SDK_TASK_KIND_STATUS` | 5 | 状态 facade |
| `ORCA_SDK_TASK_KIND_COMMAND` | 6 | 命令 facade |
| `ORCA_SDK_TASK_KIND_TRANSFER` | 7 | 文件传输 facade |

非 `SYNC` task 的 payload 必须是合法 UTF-8 JSON。当前 C ABI facade 完成 kind、payload、生命周期和异步事件门面校验；具体设备操作仍由宿主或适配器接入。

### 7.3 导出函数

**Identity and errors**

- `orca_sdk_abi_version()`
- `orca_sdk_contract_version()`
- `orca_sdk_source_commit()`
- `orca_sdk_source_commit_short()`
- `orca_sdk_error_message()`
- `orca_sdk_get_abi_version()`
- `orca_sdk_get_contract_version()`
- `orca_sdk_get_source_commit()`

**Lifecycle and tasks**

- `orca_sdk_create()` / `orca_sdk_destroy()`
- `orca_sdk_session_create()` / `orca_sdk_session_destroy()`
- `orca_sdk_session_submit_task()`
- `orca_sdk_task_cancel()`
- `orca_sdk_task_destroy()`
- `orca_sdk_task_get_id()`
- `orca_sdk_task_get_state()`

**Events**

- `orca_sdk_poll_event()`：阻塞或超时轮询 session 事件；
- `orca_sdk_event_clear()`：释放最近一次轮询事件的 backing storage；
- `orca_sdk_wakeup()`：唤醒等待该 session 的 poller。

事件类型包括 `ORCA_SDK_EVENT_SESSION_READY`、`ORCA_SDK_EVENT_TASK_SUBMITTED`、`ORCA_SDK_EVENT_TASK_COMPLETED`、`ORCA_SDK_EVENT_TASK_CANCELED` 和 `ORCA_SDK_EVENT_ERROR`。task 状态包括 `PENDING`、`COMPLETED` 和 `CANCELED`。

### 7.4 生命周期规则

1. 创建 `orca_sdk_t`，再创建 session。
2. 通过 `orca_sdk_session_submit_task()` 提交 task；非 `SYNC` task 要提供 JSON payload。
3. 使用 `orca_sdk_poll_event()` 消费事件，并在处理后调用 `orca_sdk_event_clear()`。
4. pending task 可调用 `orca_sdk_task_cancel()`；取消通过事件和 task state 报告。
5. 未清理 terminal task event 或仍在执行的 task，`orca_sdk_task_destroy()` 返回 `ORCA_SDK_E_BUSY`。
6. 先销毁 task，再销毁 session，最后销毁 SDK；销毁会使句柄失效并唤醒阻塞 poller。
7. 重复销毁、过期句柄和销毁期间的新调用返回稳定错误码，不释放调用方拥有的内存。

## 8. WonderMaker LAN adapter

头文件：`orca/connectivity/wondermaker/WonderMakerAdapter.hpp`。

### 8.1 注入接口

- `IWonderMakerHttpTransport::request()`：pair、authenticate、capabilities、status、control、file transfer 资源；
- `IWonderMakerWebSocketTransport::connect()`、`send()`、`close()`：状态流和断开回调；
- `IWonderMakerDiscoveryTransport::discover()`：发现候选设备。

### 8.2 adapter API

`WonderMakerAdapter` 实现 `IPrinterProtocolAdapter`，并额外提供：

- `discover()`、`set_manual_endpoint()`；
- `pair()`、`authenticate()`、`connect()`、`disconnect()`；
- `subscribe_status()`、`handle_websocket_message()`、`poll_status()`、`reconcile_status()`、`tick()`；
- `send_control()`、`upload_file()`；
- `monitor_state()`、`status_snapshot()`、`connected()`、`websocket_connected()`、`polling_fallback()`、`status_age_seconds()`。

WebSocket 断开后 adapter 使用轮询回退；恢复连接后先完整状态对账，再恢复状态订阅。状态超过 10 秒没有同步时进入 `MonitorState::Stale`；恢复网络后由注入式时钟测试在 15 秒目标内完成重新对账。控制请求以 `command_id` 去重，文件传输以 offset、总大小和摘要校验支持续传。

### 8.3 simulator-only 说明

`WonderMakerSimulator` 实现同一组三个注入接口，供确定性测试覆盖发现、配对、认证、能力、状态推送、温度/进度、开始/暂停/恢复/停止、错误回执、断线、重连、设备重启和文件续传。

这些测试只证明 SDK adapter boundary 和 canonical DTO 的行为。它们不证明真实 WonderMaker 固件、供应商端口、私有字段、Topic、证书服务、错误码或现场打印闭环已经通过。当前供应商 LAN 候选是 WebSocket + HTTP；规划中的设备 MQTT broker + file server 和二期 MQTT/TLS remote broker/relay 仍需协议、固件和服务端冻结。

## 9. Moonraker compatibility boundary

`orca_connectivity_moonraker` 继续提供 `IMoonrakerHttpTransport`、`IMoonrakerWebSocketTransport`、`MoonrakerServerInfo` 和 `MoonrakerAdapter`，用于共同 `IPrinterProtocolAdapter` 的兼容性与 Fake transport 测试。

Moonraker adapter 不是 WonderMaker 一期真实设备 adapter，也不能用 Moonraker Fake 通过来替代供应商协议、配对、证书、LAN 设备状态和现场验收。

## 10. 依赖边界和验收状态

公共 SDK 不能引用 GUI、wxWidgets、Python/pybind11、桌面用户目录、厂商 DLL、商业 Gateway 私有接口或私有凭据。边界检查命令为：

```text
python scripts/verify_mobile_sdk_boundary.py --source-root . --json
```

一期代码完成包括公共契约、C ABI、Android 产物格式、LAN adapter boundary、simulator、连接监控、过期/恢复/对账和自动化测试。真实设备门禁还需要指定硬件/固件、冻结协议文档、mDNS/手动连接、6 位口令或证书、状态/控制回执、文件上传和重启恢复等证据。

在这些外部条件满足前，状态应写为 **代码完成，真实设备验收待供应商门禁**。iOS/macOS/XCFramework、生产远程 MQTT/TLS、视频链路、真实云切片和经典 ICE/TURN P2P 都不属于当前一期交付。
