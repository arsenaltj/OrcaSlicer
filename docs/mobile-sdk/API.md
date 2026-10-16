# MobileSDK 核心接口说明

公共头文件目录：`src/orca_sdk/include/orca/sdk/`

## `MobileSDK.hpp`

统一入口，包含所有公共头文件，并提供：

- `kMobileSdkContractMajor`；
- `kMobileSdkContractMinor`；
- `kMobileSdkContractName`；
- `mobile_sdk_source_commit()`；
- `mobile_sdk_source_commit_short()`。

宿主可以将 source commit 写入诊断信息，用于确认桌面端和移动端是否来自同一源码身份。

## `Connectivity.hpp`

### 连接路径

| 类型 | 含义 |
|---|---|
| `ConnectionPath::Lan` | 局域网直连 |
| `ConnectionPath::P2p` | 经信令协商后的 P2P 连接 |
| `ConnectionPath::Relay` | 云端中继或厂商云转发 |

`ConnectionPathSelector` 根据 `PathObservation` 选择路径。当前路径健康时保持粘性；当前路径失效后按 LAN、P2P、Relay 顺序切换。

`ConnectionStateMachine` 提供：

- `begin_discovery()`；
- `begin_probe()`；
- `select_path()`；
- `mark_connecting()`；
- `mark_connected()`；
- `mark_disconnected()`；
- `mark_offline()`。

切换路径或进入离线状态时，`PathSelection::requires_state_sync` 为 `true`，宿主必须重新查询打印机状态，不能直接重发非幂等命令。

### `IPrinterProtocolAdapter`

协议适配器必须实现：

- `id()`：稳定协议名称；
- `probe()`：验证设备端点；
- `capabilities()`：返回设备能力；
- `get_status()`：读取当前状态；
- `upload_gcode()`：上传 G-code；
- `start_print()`、`pause()`、`resume()`、`cancel()`：打印控制。

HTTP、WebSocket、MQTT、WebRTC 和厂商云 SDK 应放在实现类中，不能进入公共接口头文件。

## `CommandEnvelope.hpp`

`CommandEnvelope` 是所有远程控制命令的统一信封，字段如下：

| 字段 | 作用 |
|---|---|
| `protocol_version` | 当前为 `orca.command/v1` |
| `session_id` | 一次连接会话的标识 |
| `device_id` | 目标打印机 |
| `command_id` | 幂等命令标识 |
| `sequence` | 会话内递增序列号 |
| `expires_at_epoch_seconds` | 命令过期时间 |
| `type` | 例如 `printer.pause` |
| `payload` | 命令参数 |

发送前调用 `validate()`。接收端应使用 `is_retry_of()` 判断重复命令，并结合设备当前状态决定是否执行。

## `ModelGeneration.hpp`

### `IModelGenerationClient`

接口覆盖：

- `create_text_job()`；
- `create_image_job()`；
- `get_status()`；
- `stop()`；
- `get_artifact()`；
- `remove()`。

Provider、供应商凭据、配额、计费和实际 HTTP 请求不属于接口实现的公共核心，应由 AI Gateway 客户端注入。

### `ModelJob`

包含请求 ID、任务 ID、输入来源、任务状态、进度、Provider 任务 ID、生成选项、产物 ID、SHA-256 和可重试错误字段。

## `Slicing.hpp`

### `SlicingRequest`

包含：

- 输入模型 URI 和 SHA-256；
- STL、OBJ、3MF 或 STEP 输入格式；
- Local 或 Remote 执行模式；
- 打印机、工艺和材料配置 ID；
- Orca 切片引擎版本；
- Plate 索引。

### `ISlicingEngine`

实现：

- `validate()`；
- `slice()`；
- `cancel()`。

`slice()` 通过 `SlicingProgressCallback` 报告进度。移动端本地切片、桌面 `libslic3r` 适配器和云端切片客户端都应实现同一接口。

## 桌面适配原则

桌面端适配器可以访问 `Model`、`Print`、`PrintConfig`、wxWidgets 和 Orca 工作区，但只能在 `src/slic3r/GUI` 或桌面适配层中完成转换。转换后的数据必须进入 MobileSDK 的 DTO 或 Port。

MobileSDK 公共代码不能：

- 引用 `wxWindow`、`wxString`；
- 引用 `libslic3r_gui`；
- 依赖 Python 或 pybind11；
- 直接加载 Bambu DLL；
- 读取桌面用户目录；
- 保存供应商密钥；
- 调用商业 Gateway 的私有接口。
