# 移动 Device / Slicing SDK 公共接口

本文描述当前可构建契约。历史需求分析中的混合 SDK、模型生成接口和旧命名不作为当前 API。接口细节以公开头文件和锁定制品为准。

## 产物和边界

| SDK | 构建目标 | 公开 C ABI | Android 产物 |
|---|---|---|---|
| Device | `orca_mobile_device_sdk` / `OrcaSlicer::MobileDeviceSDK`；共享目标 `orca_mobile_device_sdk_ffi` / `OrcaSlicer::MobileDeviceSDKShared` | `orca/mobile/device_sdk_c.h`，`orca_mobile_device_*` | `liborca_mobile_device_sdk_c.so` |
| Slicing | Android `orca_mobile_slicing_sdk`，链接 `OrcaSlicer::Core` | `orca/mobile/slicing_sdk_c.h`，`orca_mobile_slicing_*` | `liborca_mobile_slicing_sdk_c.so` |
| AI | 移动公共仓独立占位包 | 当前没有可调用 API | 当前不生成可调用二进制 |

OrcaSlicer 生产核心和 SDK 制品；移动公共仓拥有 Dart 契约、SDK 锁定、Android JNI/Kotlin 和 iOS 桥接。JNI 仅引用这两份公开 C 头，不包含 libslic3r 或切片引擎私有 C++ 头。设备协议 SDK 不包含切片或 AI 实现，移动仓不编译核心。

## Device C++ 宿主入口

```cpp
#include <orca/mobile/device/DeviceSDK.hpp>
using namespace Orca::MobileDevice;
```

该入口包含 Connectivity、CommandEnvelope 和生成的 MobileDeviceSDKBuildInfo；命名空间是 `Orca::MobileDevice`。没有 ModelGeneration.hpp 或混合 Slicing.hpp 入口。C++ 宿主由自身适配器提供网络实现；移动 JNI 使用公开 C ABI。

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

## Device C ABI

```c
#include <orca/mobile/device_sdk_c.h>
```

ABI 主/次版本使用 `ORCA_MOBILE_DEVICE_ABI_VERSION_MAJOR` / `ORCA_MOBILE_DEVICE_ABI_VERSION_MINOR`。UTF-8 字符串和字节使用显式长度 span；struct_size 和错误码均遵循公开头文件，不依赖 C++ 对象布局。当前20个导出函数为：

```text
orca_mobile_device_abi_version
orca_mobile_device_contract_version
orca_mobile_device_source_commit
orca_mobile_device_source_commit_short
orca_mobile_device_error_message
orca_mobile_device_get_abi_version
orca_mobile_device_get_contract_version
orca_mobile_device_get_source_commit
orca_mobile_device_create
orca_mobile_device_destroy
orca_mobile_device_session_create
orca_mobile_device_session_destroy
orca_mobile_device_session_submit_task
orca_mobile_device_task_cancel
orca_mobile_device_task_destroy
orca_mobile_device_task_get_id
orca_mobile_device_task_get_state
orca_mobile_device_poll_event
orca_mobile_device_event_clear
orca_mobile_device_wakeup
```

创建、会话、任务和事件句柄必须按公开 API 的生命周期管理；事件由 event_clear 释放，轮询由 wakeup 唤醒。调用方应在加载时核对 ABI、contract、source commit 和锁文件，不混用不同制品的句柄或内存。错误、取消、超时和终态行为由 `tests/mobile_sdk` 的 C/C++ 契约及线程测试覆盖。

## Slicing C ABI

```c
#include <orca/mobile/slicing_sdk_c.h>
```

当前7个导出函数为：

```text
orca_mobile_slicing_version
orca_mobile_slicing_capabilities
orca_mobile_slicing_create
orca_mobile_slicing_execute
orca_mobile_slicing_cancel
orca_mobile_slicing_destroy
orca_mobile_slicing_free_string
```

execute 接受 UTF-8 JSON 请求及事件回调，返回结果字符串；调用方使用同一 SDK 的 free_string 释放结果，并通过公开 cancel/destroy 管理取消与生命周期。具体请求/预览/推荐/错误字段以 `Docs/mobile-slicing/CONTRACT-v1.md`、移动公共包契约及该制品 capabilities 为准，不在本页另行定义第二套协议。

STL、OBJ、3MF、本地配置、场景、推荐、切片、预览、G-code、取消和恢复属于 Slicing SDK。未支持的高级语义须明确阻断，单材料输出需显式确认。Android 摘要和依赖头布局位于 SDK/platform 或构建目录，桌面摘要源码保留上游实现。

## AI 预留与平台状态

`orca_mobile_ai` 只保留包结构、构建占位和能力未实现状态，不提供图片转2D、2D转3D、3D上色的虚假接口，也不定义最终 provider/请求/远程协议。

SDK 制品应记录 sdk_name、sdk_version、abi_version、contract_version、core_source_commit、sdk_source_commit、target_platform、architectures、minimum_os_version、compiler_or_ndk_version 与 artifact_sha256。正式二进制走 Release/制品存储，移动公共仓按锁消费。

本轮桌面完整 configure/build 按用户决定跳过，iOS 验收搁置；工程结构和公开头存在不等于这些平台编译或实机通过。Android 构建、ABI/符号/16KiB 与设备验收各自记录，不互相替代。


## Optional native Moonraker host implementation

C++ 宿主可显式链接 `OrcaSlicer::MoonrakerTransport`，入口为 `orca/connectivity/moonraker/MoonrakerTransport.hpp`，命名空间为 `Orca::MobileDevice`。该独立目标提供 HTTP/HTTPS、上传、WS/WSS、超时、取消与连接代次隔离，依赖 CURL、Boost.Beast 和 OpenSSL。Device C ABI 的20个公开符号和纯协议依赖保持；新目标不会进入移动 JNI、Core 或 Slicing 目标。默认 OFF，可选 AUTO/ON，不为宿主静默下载依赖。构建和生命周期说明见 [native transport guide](../../src/orca_connectivity_moonraker/README.md)。
