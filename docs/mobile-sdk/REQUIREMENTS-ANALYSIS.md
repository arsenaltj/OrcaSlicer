# 3D 打印平台工程架构与三期需求分析报告

- 报告类型：需求与系统架构分析（按工作区 analysis-report 模板整理）
- 文档版本：1.3；原报告版本：1.2
- 任务 ID：`task-20261016013901509-0f210b`
- 父任务：`task-20261015124945955-67069d`
- 报告更新时间：2026-10-17（Asia/Shanghai）
- 报告状态：待需求评审；父任务 SDK-only 整合与自动化门禁已验证，真实 transport/设备仍未验收
- 分析仓库：`OrcaSlicer`
- 分析分支：`task/task-20261016013901509-0f210b`
- 分析版本：代码整合基线 `d0516f9e2edc3ce0816d7529f9f41457435c0b0f`；报告同步副本记录本轮实现与验证结果
- 工作树：`D:/Codex/worktrees/OrcaSlicer/task-20261016013901509-0f210b`
- 差异身份：[源版本、工作树状态与逐文件 SHA-256](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/source-state.json)
- 权威归档目录：`D:/Codex/artifacts/task-20261016013901509-0f210b/reports`；工程内文件为同步阅读副本
- 阅读入口：[Markdown](REQUIREMENTS-ANALYSIS.md) · [HTML](REQUIREMENTS-ANALYSIS.html)

## 结论

产品不是单一的移动端控制工具，而是由桌面端、移动端和平台后端共同组成的 3D 打印平台：

```text
桌面端：完整建模、切片、打印工作台和高级设备能力
移动端：设备管理、局域网控制、监控、模型库、社区、账号和支付
后端：账号、设备、打印任务、模型、社区、AI、切片、支付、通知和远程链路
数据库：业务数据、任务状态、权益账本、内容元数据和审计记录
基础设施：对象存储、CDN、队列、搜索、实时网关、监控和部署
```

本报告采用以下工程决策：

1. `OrcaSlicer::MobileSDK` 是桌面端和移动端之间的共享核心契约，不包含桌面 UI、Python、供应商 DLL 和商业服务实现。
2. 桌面端继续保留完整的 `libslic3r`、桌面 GUI 和高级工作台，通过适配层使用 MobileSDK DTO。
3. 移动端使用 Flutter 承载产品页面和业务流程，通过 Kotlin/Swift 原生层处理系统能力，通过 C ABI/FFI 使用 C++ SDK。
4. 后端第一阶段采用模块化单体、Worker 和实时网关；账号、设备、支付、模型和社区按领域隔离，后续再按负载拆分服务。
5. 一期先完成局域网闭环，不把远程 Relay、AI 供应商、支付商城和完整社区塞入一期验收范围。
6. 一期切片以远程或桌面生成的 G-code 为主；移动端本地完整切片留到后续阶段。

本次报告保留原有三期需求内容，并新增父任务 SDK 整合结果、验证边界和未完成事项；一期明确规格仍为 13 个需求包，二期为 11 个框架包，三期为 10 个框架包。需求编号表示规划单元，不表示整期功能已实现或通过验收。

一期交付主线保持为：移动端通过 SDK 在同一局域网内发现/手动添加设备、连接、监控状态并执行基础控制。账号与云端数据底座可以独立开发；互联网或后端不可用不能成为局域网验收的前置条件。

## 影响范围

| 工程/领域 | 职责与交付物 | 当前报告约束 |
|---|---|---|
| 桌面端与共享 SDK | 中立契约、C ABI、协议适配、构建产物、桌面适配边界 | SDK 独立于桌面 UI；C ABI/Moonraker 已整合并通过 SDK-only 构建与测试，真实 transport/设备仍待验证 |
| Android/iOS 移动端 | Flutter 页面、原生发现与权限、SDK 桥接、局域网监控与控制 | 一期优先完成局域网使用闭环 |
| 平台后端 | 身份、设备元数据；后续扩展任务、模型、社区、支付权益 | 一期不转发局域网命令；后续使用稳定 API/事件 |
| 数据库与存储 | 关系数据、迁移、任务事件、文件元数据、对象存储 | 一期只实现必要表；文件本体不进入业务表 |
| 验证与运维 | 协议模拟器、真机验证、产物身份、诊断和迁移记录 | Fake transport 契约测试和 SDK-only 门禁已通过；真实设备联调、Android/iOS 真机和现场验收仍未完成 |

| 本轮变更对象 | 操作 |
|---|---|
| 用户审核产物 | 新增标准归档 `reports/analysis-report.md`、`analysis-report.html` |
| 原工程报告 | 同步更新 `Docs/mobile-sdk/REQUIREMENTS-ANALYSIS.md/.html`，保留原入口 |
| 可复核材料 | 保存旧版报告、源版本快照、需求保留校验和页面检查结果 |
| 父任务 SDK 代码 | 整合 SDK core、C ABI 和 Moonraker 验收提交；保留 SDK-only 根 CMake 注册范围例外，去除旧重复实现 |

**本轮实现同步（2026-10-17）**：

- 父 worktree 以 SDK core 提交 `a092eb01ec6087b58ec794402621d0ae95a083f9`、C ABI runtime 提交 `ff1b7e6c4e52b6ed8ba828d0f35a84861d5cac3d`、P1-SDK-02 验收提交 `ed76f26d5a42985b65c171cee10a4716c6bef1fb` 和 P1-SDK-03 验收提交 `49b21694a487ebd71e7489cc79df0100f51219b7` 为实现来源，父 worktree 的代码整合基线为 `d0516f9e2edc3ce0816d7529f9f41457435c0b0f`。旧未提交 SDK 快照另存为 Git stash `06b9843cc5be8294279e4ce095903141ea8a215c`，未恢复重复的 C ABI/Moonraker 实现。
- C ABI 当前采用按 session FIFO 排队的事件拉取语义；`orca_sdk_poll_event` 为事件分配 session 内单调递增的 sequence 和 timestamp，并把 task、task_id、message 和 payload 视图绑定到调用者的 `orca_sdk_event_t`。视图由 `orca_sdk_event_clear` 释放，复用同一输出结构进行下一次成功 poll 也会清理上一次 delivery；任务仍有排队或已交付但未清理的事件时，`orca_sdk_task_destroy` 返回 `ORCA_SDK_E_BUSY`。当前整合面没有 callback 注册接口；阻塞 poll 在 session 销毁时返回 invalid handle，显式 wakeup 返回 `ORCA_SDK_E_WAKEUP`。
- `orca_sdk_task_cancel` 只接受 pending task；成功后任务状态为 `ORCA_SDK_TASK_CANCELED`，并发出一次 `ORCA_SDK_EVENT_TASK_CANCELED`、状态码 `ORCA_SDK_E_CANCELED` 的事件；终态任务再次取消返回 `ORCA_SDK_E_ALREADY_COMPLETE`。
- Moonraker 将有限打印进度钳制到 `[0,1]`；`cancelled` 和 `canceled` 两种设备状态均映射为 SDK 的 `PrinterState::Completed`，这与 C ABI 任务取消事件是两条独立语义。
- Moonraker 通过可注入的 HTTP/WebSocket transport 接口实现；当前契约测试只使用 `FakeHttp` 和 `FakeWebSocket`。本轮没有真实 HTTP/WebSocket transport 或真实打印机，真实 transport/设备联调仍未验证。
- 本轮验证已覆盖 VS 17 2022 x64 SDK-only configure/build、32 个全量 CTest、`MobileSDK` 32 个、`CABI` 17 个、`Moonraker` 8 个、`threading` 7 个标签测试、boundary check、C ABI exports 和 C11 header；这些门禁均通过。
## 关键发现

### 3.1 整体架构

```text
┌──────────────────────────────────────────────────────────────┐
│                        移动端 Mobile App                     │
│ Flutter UI / Feature / Repository                           │
│ Android Kotlin Adapter / iOS Swift Adapter                   │
└──────────────────────────────┬───────────────────────────────┘
                               │ HTTPS / WebSocket / FFI
                               ▼
┌──────────────────────────────┴───────────────────────────────┐
│                       Platform Backend                       │
│ API/BFF · Identity · Device · Print Job · Model · Billing   │
│ Community · AI Job · Slicing Job · Notification · Admin     │
│ Realtime Gateway · Worker · Audit                            │
└──────────────┬──────────────────┬─────────────────┬──────────┘
               │                  │                 │
               ▼                  ▼                 ▼
        PostgreSQL          Object Storage/CDN   Queue/Search
               │
               ▼
┌──────────────────────────────────────────────────────────────┐
│                 OrcaSlicer Desktop / Edge Connector           │
│ Desktop Adapter · Local Slicing · Printer Protocol Adapter   │
│                       OrcaSlicer::MobileSDK                  │
└──────────────────────────────┬───────────────────────────────┘
                               ▼
                         3D 打印机
```

#### 3.1.1 数据面和控制面

局域网控制和远程控制必须分成两个层面：

```text
控制面：身份、设备注册、权限、会话、信令、审计和路径协调
数据面：局域网直连、P2P、云端 Relay、摄像头和文件传输
```

一期只实现局域网直连。二期增加云端控制面和远程 Relay；三期再根据设备能力增加 P2P、边缘 Connector 和多设备调度。

打印控制和摄像头传输也要分开。摄像头中断不能影响暂停、恢复和停止等控制命令。
### 3.2 仓库和代码工程规划

推荐的产品级仓库布局如下：

```text
repos/
├── OrcaSlicer/                  # 桌面端、libslic3r、MobileSDK 源码
├── orca-mobile/                 # Flutter 应用、Android/iOS 宿主和插件
├── orca-platform/               # 账号、设备、模型、社区、支付和任务后端
└── orca-infra/                  # 部署、环境、数据库迁移、监控和密钥配置
```

SDK 在接口和 ABI 稳定前保留在 OrcaSlicer 仓库。稳定后可以发布为独立 `orca-mobile-sdk` 版本包，但不能在移动端仓库复制一份 C++ 核心。

#### 3.2.1 OrcaSlicer 桌面端

```text
OrcaSlicer/
├── src/orca_sdk/
│   ├── include/orca/sdk/          # 中立公共头文件
│   ├── Connectivity.*             # LAN/P2P/Relay 路径和状态机
│   ├── CommandEnvelope.*          # 控制命令信封
│   ├── ModelGeneration.*          # AI 任务和产物 DTO
│   ├── Slicing.*                  # 本地/远程切片契约
│   └── CMakeLists.txt
├── src/slic3r/GUI/                # 桌面 GUI
├── src/slic3r/Utils/              # 桌面连接和厂商适配
├── tests/mobile_sdk/              # SDK 契约测试
├── tests/slic3rutils/             # 桌面工具测试
└── Docs/mobile-sdk/
```

桌面端适配关系：

```text
Desktop GUI / Workbench
        ↓ DTO 转换
OrcaSlicer::MobileSDK
        ↓
Desktop Adapter / libslic3r / 文件系统 / 厂商扩展
```

桌面 GUI 可以使用 `wxWidgets`、`Model`、`PrintConfig` 和完整工作区对象，但这些类型只能在桌面适配层转换，不能进入 MobileSDK 公共头文件。

#### 3.2.2 移动端

```text
orca-mobile/
├── apps/mobile/
│   ├── lib/
│   │   ├── app/                   # 路由、主题、启动和环境
│   │   ├── features/
│   │   │   ├── account/
│   │   │   ├── devices/
│   │   │   ├── printer_monitor/
│   │   │   ├── print_jobs/
│   │   │   ├── model_library/
│   │   │   ├── community/
│   │   │   ├── billing/
│   │   │   └── ai/
│   │   ├── domain/                # Use Case、状态和领域实体
│   │   ├── data/                  # API、缓存和持久化
│   │   └── presentation/          # 页面和组件
│   ├── packages/
│   │   ├── api_client/            # OpenAPI 客户端
│   │   ├── mobile_sdk_bridge/     # C ABI/FFI 和平台通道
│   │   ├── model_viewer/          # 3D/G-code 预览抽象
│   │   └── design_system/
│   ├── native/android/            # Kotlin、NSD、通知、前台服务
│   ├── native/ios/                # Swift、Bonjour、权限、通知
│   └── integration_test/
└── pubspec.yaml
```

移动端不能直接访问数据库、支付服务端密钥、AI Provider 或厂商私有云。所有账户、权益、模型下载和远程命令都必须经过后端授权。

#### 3.2.3 后端

```text
orca-platform/
├── apps/
│   ├── api/                       # 公共 API/BFF
│   ├── realtime-gateway/          # WebSocket、设备状态和命令
│   ├── worker/                    # AI、切片、媒体、审核、通知
│   ├── admin-api/                 # 运营、审核、订单和客服
│   └── scheduler/                 # 超时、重试、对账和清理
├── packages/
│   ├── contracts/                 # OpenAPI、事件和命令 Schema
│   ├── identity/
│   ├── organization/
│   ├── device/
│   ├── print-job/
│   ├── model-catalog/
│   ├── community/
│   ├── billing/
│   ├── entitlement/
│   ├── ai-job/
│   ├── slicing-job/
│   ├── moderation/
│   ├── notification/
│   ├── search/
│   ├── storage/
│   └── audit/
├── migrations/
└── tests/
```

第一阶段后端可以是一个部署单元，但每个领域使用独立模块、独立迁移目录和明确的事件接口，避免业务模块直接读写其他模块的表。

#### 3.2.4 数据库和存储

关系数据库保存业务元数据和状态：

```text
users
organizations
organization_members
printers
printer_capabilities
printer_bindings
printer_sessions
print_jobs
print_job_events
model_assets
model_versions
model_entitlements
slicing_jobs
ai_jobs
orders
payment_transactions
entitlement_ledger
community_posts
comments
moderation_cases
audit_events
```

对象存储保存大文件：

```text
models/{model_id}/{version_id}/source.3mf
models/{model_id}/{version_id}/preview.glb
models/{model_id}/{version_id}/thumbnail.webp
slicing/{job_id}/output.gcode
prints/{print_job_id}/snapshot.jpg
```

数据库不保存 STL、3MF、OBJ、GLB、G-code 或视频本体，只保存文件大小、SHA-256、媒体类型、版本和对象存储引用。
### 3.3 统一契约和状态模型

#### 3.3.1 SDK 契约

一期公共 SDK 至少包括：

- `ConnectionPath`：LAN、P2P、Relay；
- `ConnectionStateMachine`：发现、探测、连接、已连接、切换、离线；
- `PrinterCapability`：状态流、G-code 上传、开始/暂停/恢复/停止；
- `PrinterStatus`：设备状态、进度、温度、当前文件和错误；
- `CommandEnvelope`：会话、设备、命令、序列号、过期时间和 payload；
- `ModelJob`：AI 任务、产物和错误；
- `SlicingRequest` / `SlicingResult`：切片输入、引擎、配置、产物和进度。

#### 3.3.2 API 契约

| 接口 | 用途 | 一期方式 |
|---|---|---|
| Local SDK API | 移动端调用本地打印 SDK | C ABI/FFI |
| Query API | 账号、设备和配置查询 | HTTPS/JSON，最小化 |
| Command API | 后端转发打印命令 | 二期启用 |
| Realtime API | 状态、进度和消息 | 一期本地事件，二期 WebSocket |
| Artifact API | 模型和 G-code 文件 | 二期启用 |
| Payment Webhook | 支付交易回调 | 二期启用 |

#### 3.3.3 任务模型

AI、切片和打印都使用任务化状态：

```text
created → validating → running → completed
                         ├──────→ failed
                         └──────→ canceled
```

每个任务必须具备：

```text
request_id       客户端幂等键
job_id           服务端或本地任务 ID
state            状态
progress         进度
artifact_id      结果产物
artifact_sha256  内容校验
error_code       稳定错误码
retryable        是否可以重试
```
### 3.4 三期开发计划总览

| 阶段 | 产品目标 | 交付重点 | 主要不做内容 |
|---|---|---|---|
| 一期 | 局域网打印闭环 | SDK 封装、移动端 LAN 连接、状态监控、基础控制、最小账号/设备底座 | 远程 Relay、付费模型、完整社区、完整本地切片 |
| 二期 | 平台化和远程化 | 多协议、设备云端管理、AI、云切片、模型库、支付权益、基础社区 | 创作者结算、高级推荐、复杂 P2P 调度 |
| 三期 | 生态化和专业化 | P2P/边缘、完整社区、模型交易、创作者生态、本地切片、团队和推荐 | 根据运营数据决定的实验功能 |
### 3.5 一期详细需求规格

#### 3.5.1 一期目标和边界

一期完成下面的端到端场景：

```text
用户打开移动端
    ↓
授予局域网权限或输入打印机 IP
    ↓
发现/连接打印机
    ↓
读取设备能力和当前状态
    ↓
持续显示温度、进度、文件和错误
    ↓
执行暂停、恢复、停止等基础控制
    ↓
断开或切换页面后重新连接并对账
```

一期支持范围建议：

- 首批协议：优先 Moonraker；协议抽象必须可接入 OctoPrint/PrusaLink。
- 连接方式：局域网直连，支持自动发现和手动 IP。
- 移动端：Android 和 iOS 各一套真机验证。
- 控制命令：状态读取、开始、暂停、恢复、停止；高风险命令需要二次确认。
- 账号：最小登录和设备绑定能力，不阻塞局域网临时连接。
- 后端：账号、设备元数据和诊断接口；不承担一期局域网控制转发。
- 数据库：完成一期所需的最小表和迁移，不提前实现完整商城和社区模型。

#### 3.5.2 P1-SDK-01：中立契约稳定化

**目标**：让桌面端和移动端依赖同一组不含桌面类型的公共契约。

**输入和范围**：

- 现有 `src/orca_sdk/include/orca/sdk/`；
- 连接路径和状态机；
- 命令信封；
- 打印机能力和状态 DTO；
- AI 和切片任务 DTO。

**需求**：

1. 所有公共字段有明确类型、默认值和协议版本。
2. JSON 序列化字段稳定，未知扩展字段不能破坏基础解析。
3. 公共头文件不包含 wxWidgets、Python、GUI、Boost filesystem 或厂商类型。
4. 命令必须校验 session、device、command、sequence 和过期时间。
5. 同一 `command_id` 可以识别幂等重试。
6. 连接路径发生切换后必须标记需要重新同步设备状态。

**验收**：

- `MobileSDK` 独立目标可被桌面和测试目标链接；
- 契约测试覆盖正常、缺字段、错误版本、过期和重复命令；
- 不依赖桌面 GUI 和 Python；
- 版本字段可在移动端诊断信息中读取。

**可独立开发**：是。

**依赖**：无；当前任务已经提供基础实现。

#### 3.5.3 P1-SDK-02：C ABI 和异步 SDK 门面

**目标**：让 Android/iOS 宿主以稳定方式调用 C++ SDK。

**需求**：

1. 新增 C ABI 门面，避免移动端直接绑定 C++ 模板、异常和 STL 布局。
2. 使用不透明句柄表示 SDK 实例、连接会话和任务。
3. 提供创建、销毁、发现、连接、断开、读取状态和发送命令接口。
4. 提供事件回调或事件拉取接口，事件包含类型、序列号、时间和 JSON/结构化 payload。
5. 所有异步操作提供取消和错误回调。
6. C ABI 错误码稳定，错误详情可诊断但不含凭据。
7. 明确线程模型：SDK 内部线程不得直接触碰 Flutter/UI 线程。

**推荐接口形态**：

```text
orca_sdk_create()
      ↓
orca_sdk_discover()
      ↓
orca_sdk_connect(device)
      ↓
orca_sdk_poll_event() / callback
      ↓
orca_sdk_send_command(command)
      ↓
orca_sdk_disconnect()
      ↓
orca_sdk_destroy()
```

**验收**：

- Windows 桌面测试可以调用 C ABI；
- Android/iOS 桥接不需要了解 C++ 类布局；
- 异步回调不会在 SDK 销毁后触发；
- 连接断开、取消和重复销毁不会崩溃；
- 提供 C ABI 头文件、版本号和最小示例。

**本轮整合状态**：P1-SDK-02 已由父 worktree 的 `ed76f26d5a42985b65c171cee10a4716c6bef1fb` 整合并验证。事件接口当前以 poll-only 拉取为准，事件存储、清理、任务销毁 busy 保护、wakeup、取消映射和句柄生命周期语义见本报告的实现同步记录；Android/iOS bridge 和真实移动宿主尚未验证。

**可独立开发**：是。

**依赖**：P1-SDK-01。**可独立开发**：是。

**依赖**：P1-SDK-01。

#### 3.5.4 P1-SDK-03：局域网协议适配器

**目标**：通过一个真实协议打通 SDK 到打印机的局域网链路。

**首批建议**：实现 Moonraker 适配器，同时保持 `IPrinterProtocolAdapter` 可接入其他协议。

**需求**：

1. 支持手动 IP/端口配置。
2. 支持 HTTP/JSON-RPC 请求。
3. 支持 WebSocket 状态订阅；不支持订阅时回退到轮询。
4. 支持设备探测、能力读取、状态读取。
5. 支持 G-code 文件上传接口抽象。
6. 支持开始、暂停、恢复和停止命令。
7. 连接断开后使用退避重连，不重复执行非幂等命令。
8. 连接重建后先读取完整状态，再恢复实时订阅。
9. 每条控制命令关联 `session_id`、`command_id` 和设备回执。
10. 将协议错误转换为稳定 SDK 错误码。

**状态字段最低集合**：

- 设备在线状态；
- 打印状态；
- 当前文件；
- 打印进度；
- 喷嘴温度和目标温度；
- 热床温度和目标温度；
- 当前错误；
- 更新时间；
- 协议适配器版本。

**验收**：

- 真实设备或协议模拟器可以完成发现、连接、订阅、断开和重连；
- 断网后不重复启动打印；
- 状态订阅中断后可以重新对账；
- 暂停、恢复和停止命令具有明确回执；
- Moonraker 契约测试覆盖 HTTP、WebSocket 和错误响应。

**本轮整合状态**：P1-SDK-03 已由父 worktree 的 `49b21694a487ebd71e7489cc79df0100f51219b7` 整合并验证。Moonraker 覆盖 Fake transport 下的 HTTP/JSON-RPC、WebSocket 订阅、轮询回退、重连对账、错误映射、进度范围和取消状态归一；真实 transport、协议模拟器以外的设备以及打印机现场尚未验证。

**可独立开发**：是。

**依赖**：P1-SDK-01；C ABI 接入可与适配器并行。**可独立开发**：是。

**依赖**：P1-SDK-01；C ABI 接入可与适配器并行。

#### 3.5.5 P1-SDK-04：SDK 构建和移动产物

**目标**：输出移动端可以消费的 SDK 产物。

**需求**：

1. CMake 支持桌面静态库或共享库。
2. Android 生成 AAR/JNI 或供 Flutter FFI 使用的原生库。
3. iOS 生成 XCFramework 或供 Flutter FFI 使用的 Framework。
4. 产物携带 SDK contract version 和 source commit。
5. 产物不包含桌面 GUI、Python、供应商 DLL 和开发机路径。
6. SDK 头文件、二进制和示例的版本一致。
7. 移动端仓库通过版本号引用产物，不复制 SDK 源码。

**验收**：

- 从干净构建目录可以生成目标平台产物；
- Android/iOS 宿主可以链接并读取版本信息；
- SDK 产物在没有桌面工作区的环境中可以运行协议契约测试；
- 构建清单记录编译器、平台、SDK 版本和源代码身份。

**可独立开发**：是。

**依赖**：P1-SDK-01、P1-SDK-02。

#### 3.5.6 P1-MOB-01：移动端应用骨架

**目标**：建立 Android/iOS 共用的 Flutter 产品骨架。

**需求**：

- 环境配置：开发、测试、生产；
- 路由和主题；
- 设备列表、设备详情和监控页面；
- 统一错误展示和重试；
- 生命周期管理；
- 日志开关；
- SDK 版本诊断页；
- 原生权限申请入口。

**验收**：

- Android 和 iOS 真机可以安装和启动；
- 页面切换不会丢失连接状态；
- SDK 事件不会直接更新已销毁页面；
- 发生错误时用户能看到可操作的提示；
- 开发环境可以切换模拟器和真实局域网设备。

**可独立开发**：是。

**依赖**：P1-SDK-02 的接口定义；可以先用 Mock SDK。

#### 3.5.7 P1-MOB-02：局域网发现和手动连接

**目标**：让用户在移动端找到并连接局域网打印机。

**需求**：

1. 首次进入设备页申请 Android/iOS 本地网络权限。
2. 支持自动发现；发现失败时支持手动输入 IP、端口和协议类型。
3. 设备卡片显示地址、协议、型号、在线状态和最近连接时间。
4. 连接前执行协议探测和能力读取。
5. 保存已授权设备的非敏感连接信息。
6. 支持删除设备、重新配对和修改地址。
7. 将发现、探测、连接和认证错误区分显示。

**验收**：

- 在同一局域网内能发现首批协议设备；
- 手动 IP 可以绕过发现完成连接；
- 无局域网权限时给出系统设置引导；
- 不同协议不会错误显示为同一设备；
- 设备列表可以恢复上次连接信息但不会保存明文密钥。

**可独立开发**：是。

**依赖**：P1-MOB-01、P1-SDK-02、P1-SDK-03。

#### 3.5.8 P1-MOB-03：打印状态监控

**目标**：在移动端持续展示打印机当前状态。

**监控字段**：

- 连接路径和连接质量；
- 设备在线状态；
- 打印状态；
- 当前文件；
- 进度和预计剩余时间（设备提供时）；
- 喷嘴/热床当前温度和目标温度；
- 当前层数（设备提供时）；
- 错误、告警和最后更新时间；
- 当前协议和 SDK 版本。

**需求**：

1. 优先使用订阅状态流，协议不支持时使用有节流的轮询。
2. 页面显示“正在连接、已连接、状态过期、已断开、离线”等明确状态。
3. 状态时间戳超过阈值时不能继续显示为实时数据。
4. 重新连接后先执行状态对账，再恢复页面实时更新。
5. 页面销毁后停止 UI 订阅，但不能误关闭其他页面使用的共享连接。
6. 支持下拉刷新和手动重试。

**验收**：

- 打印状态变化可以在移动端正确反映；
- 断网、恢复网络和打印机重启后状态最终一致；
- 状态流异常不会导致 App 崩溃或重复订阅；
- 温度、进度和错误信息与协议模拟器一致；
- 使用录制数据可以进行无设备 UI 回归测试。

**可独立开发**：是。

**依赖**：P1-MOB-02、P1-SDK-03。

#### 3.5.9 P1-MOB-04：基础打印控制

**目标**：通过移动端使用 SDK 的幂等控制能力。

**一期命令**：

- `printer.start`；
- `printer.pause`；
- `printer.resume`；
- `printer.stop`；
- `printer.refresh_status`。

**需求**：

1. 高风险的停止命令必须二次确认。
2. 每次点击只生成一个 `command_id`。
3. 网络重试复用原 `command_id`，不能生成重复动作。
4. 命令执行期间显示发送中、已接受、已完成、失败和状态未知。
5. 状态未知时不能自动重复发送停止、开始等非幂等命令。
6. 控制成功后重新读取设备状态。
7. 无控制权限或设备能力不支持时，按钮不可用并解释原因。

**验收**：

- 暂停、恢复和停止在协议模拟器上能够完成闭环；
- 请求超时不会导致重复命令；
- 设备拒绝命令时显示稳定错误码和用户提示；
- 页面退出再进入后可以显示最终设备状态；
- 所有命令都有本地诊断记录。

**可独立开发**：是。

**依赖**：P1-MOB-03、P1-SDK-02、P1-SDK-03。

#### 3.5.10 P1-MOB-05：本地缓存和诊断

**目标**：保证局域网短时断开时用户仍能看到最后状态，并能定位问题。

**需求**：

- 缓存设备基本信息、最后状态和时间戳；
- 不缓存明文密码、Token 或支付凭证；
- 保存最近连接错误、命令结果和 SDK 版本；
- 支持导出脱敏诊断包；
- 支持清除本地设备和诊断数据；
- 日志按大小和时间滚动。

**验收**：

- App 重启后可恢复设备列表；
- 过期状态明确标记为缓存数据；
- 诊断包不包含凭据和完整模型文件；
- 日志出现异常时可以定位发现、连接、协议和命令阶段。

**可独立开发**：是。

**依赖**：P1-MOB-01。

#### 3.5.11 P1-BE-01：最小账号服务

**目标**：为后续模型、支付和远程功能建立稳定身份，不阻塞一期局域网直连。

**一期范围**：

- 注册、登录、刷新会话和退出；
- 用户基本资料；
- 设备绑定关系；
- 账号注销；
- 登录审计；
- 开发环境测试账号。

**一期不做**：

- 订阅支付；
- 创作者结算；
- 社交登录的全部供应商接入；
- 企业组织的完整计费模型。

**需求**：

1. 账号 API 只返回稳定用户 ID 和会话信息，不返回内部数据库对象。
2. 设备绑定操作需要用户身份和明确授权。
3. 局域网临时连接可以在未登录状态使用，但不能获得远程控制或云端权益。
4. 所有登录和设备绑定操作写入审计事件。

**验收**：

- 移动端可以登录、退出和恢复会话；
- Token 失效后能引导重新登录；
- 删除账号后设备绑定关系被撤销；
- 未登录用户不能访问后续需要身份的后端接口。

**可独立开发**：是。

**依赖**：P1-DB-01；移动端可以先使用 Mock API。

#### 3.5.12 P1-BE-02：设备注册和能力查询

**目标**：保存用户已知设备的元数据，但不把一期局域网控制转移到云端。

**接口建议**：

```text
POST   /api/v1/devices
GET    /api/v1/devices
GET    /api/v1/devices/{device_id}
PATCH  /api/v1/devices/{device_id}
DELETE /api/v1/devices/{device_id}
GET    /api/v1/devices/{device_id}/capabilities
```

**需求**：

- 设备 ID、名称、型号、协议、地址摘要和能力；
- 用户与设备绑定、解绑和共享预留；
- 设备能力版本化；
- 不保存局域网明文密码；
- 后端记录最后一次同步时间，但一期不要求后端实时控制。

**验收**：

- 移动端可以注册局域网设备元数据；
- 同一用户重复注册不会生成重复设备；
- 设备解绑后不能从账号 API 查询；
- 能力字段可以向后兼容扩展。

**可独立开发**：是。

**依赖**：P1-BE-01、P1-DB-01。

#### 3.5.13 P1-DB-01：一期数据库和迁移

**目标**：提供账号、设备、打印会话和审计的最小数据底座。

**一期表**：

```text
users
sessions
printers
printer_bindings
printer_capabilities
printer_sessions
print_jobs
print_job_events
audit_events
```

**需求**：

1. 所有表有主键、创建时间、更新时间和必要的软删除策略。
2. 设备绑定使用唯一约束，避免重复注册。
3. 打印会话与本地连接会话分开建模。
4. 打印任务事件采用追加写入，不能覆盖历史状态。
5. 数据库迁移可在空库和上一版本库上重复验证。
6. 测试数据和生产密钥分离。

**验收**：

- 空库初始化成功；
- 迁移升级和回滚策略有记录；
- 账号、设备和审计查询有索引；
- 删除账号后符合数据保留策略；
- 集成测试可以创建账号、绑定设备和写入打印会话。

**可独立开发**：是。

**依赖**：无。

#### 3.5.14 P1-QA-01：局域网端到端验证

**目标**：用协议模拟器和至少一台真实设备验证移动端闭环。

**测试层次**：

1. SDK 契约单元测试；
2. 协议适配器测试；
3. C ABI 测试；
4. Flutter Mock SDK 测试；
5. Android/iOS 真机局域网测试；
6. 断网、重连、重启和命令幂等测试。

**一期必须覆盖**：

- 自动发现失败后手动 IP 连接；
- 局域网权限拒绝和恢复；
- 打印机断电重启；
- WebSocket 中断；
- 状态过期；
- 暂停/恢复/停止超时；
- 重复点击和网络重试；
- App 进入后台再恢复；
- 多个页面共享一个设备连接；
- 设备不支持某项能力。

**验收**：

- 关键路径全部有可重复测试步骤；
- 测试报告包含设备、系统、SDK 和协议版本；
- 失败用例有日志和命令 ID；
- 不以“界面显示成功”代替设备状态回执。

**可独立开发**：是。

**依赖**：各模块提供可测试接口。
### 3.6 一期非功能需求

#### 3.6.1 可靠性

- 连接状态必须可恢复；
- 连接重建后先状态对账，再恢复命令；
- 任何重试都必须区分幂等和非幂等命令；
- 摄像头、日志和页面异常不能影响打印控制；
- 设备状态过期时明确显示，不继续伪装实时。

#### 3.6.2 安全

- 不在移动端包内保存厂商密钥、支付密钥或 AI Provider Key；
- 不在日志中记录密码、Token 和完整授权头；
- 所有控制命令都带 session、device、command 和过期时间；
- 绑定和解绑必须有身份校验；
- 诊断包默认脱敏。

#### 3.6.3 可观测性

每次连接和控制至少记录：

```text
device_id
session_id
command_id
protocol_adapter
connection_path
sdk_version
state_before
state_after
error_code
timestamp
```

#### 3.6.4 一期性能目标

这些是验收目标，需在真实设备和参考机型上校准：

- 同局域网自动发现结果通常在 5 秒内出现；
- 手动 IP 连接在协议正常时 5 秒内给出成功或明确失败；
- 状态订阅中断后 10 秒内进入“状态过期”或断开提示；
- 网络恢复后 15 秒内完成状态重新对账；
- 页面滚动和状态更新不能造成明显卡顿；
- 大量状态事件需要节流，不能每个底层事件都触发完整页面重建。
### 3.7 二期需求框架

二期目标是从“局域网工具”升级为“平台化产品”。下面给出可拆分的需求包，接口细节在一期验收后冻结。

| 编号 | 模块 | 主要需求 | 产物 |
|---|---|---|---|
| P2-SDK-01 | 多协议连接 | OctoPrint、PrusaLink、厂商 HTTP/WebSocket 适配器 | 协议插件和契约测试 |
| P2-CONN-02 | 远程连接 | 设备注册、信令、Relay、会话租约、状态对账 | Realtime Gateway、Relay 客户端 |
| P2-MOB-03 | 远程监控 | 云端设备列表、远程状态、推送和异常通知 | 移动端远程监控 |
| P2-DEV-04 | 设备管理 | 多设备、设备共享、能力和固件信息 | 设备域扩展 |
| P2-AI-05 | AI Gateway | 文本/图片任务、重试、取消、产物下载和配额 | AI Job 服务 |
| P2-SLICE-06 | 云端切片 | 上传模型、打印机/材料配置、切片任务和 G-code | Slicing Worker |
| P2-MODEL-07 | 模型库 | 上传、解析、缩略图、3D 预览、搜索、标签和下载 | Model Catalog |
| P2-BILL-08 | 支付权益 | 订阅、AI 额度、模型购买、退款、权益恢复 | Billing/Entitlement |
| P2-COMM-09 | 基础社区 | 作者、评论、点赞、收藏、关注、举报 | Community/Moderation |
| P2-ADMIN-10 | 运营后台 | 内容审核、设备支持、订单查询、任务重试 | Admin API/Web |
| P2-NOTIFY-11 | 通知系统 | 打印完成、异常、订单和任务通知 | Push/Inbox |

二期的关键约束：

- 云端控制命令继续复用 `CommandEnvelope`；
- AI、切片和打印任务都必须可取消、可恢复和可审计；
- 模型文件使用对象存储和签名下载地址；
- 支付交易必须转换为统一权益，不让每个业务模块直接判断支付状态；
- 远程控制和摄像头转发分开；
- 复杂切片优先放到 Worker，不把完整桌面工作台塞进移动端。
### 3.8 三期需求框架

三期目标是形成内容、设备和创作者生态，并支持专业用户和团队使用。

| 编号 | 模块 | 主要需求 |
|---|---|---|
| P3-SDK-01 | 本地切片 SDK | C ABI、Android AAR/JNI、iOS XCFramework、内存预算和取消 |
| P3-SDK-02 | 高级预览 | G-code 分层、路径动画、多材料和设备差异预览 |
| P3-CONN-03 | P2P/Edge | 设备侧 Connector、NAT 穿透、TURN、边缘离线能力 |
| P3-PRINT-04 | 打印编排 | 多打印机队列、批量任务、失败恢复和任务调度 |
| P3-MODEL-05 | 模型生态 | Remix、版本分支、授权、合集、打印成果和模型质量报告 |
| P3-MARKET-06 | 创作者商业化 | 付费模型、分成、结算、优惠和版权处理 |
| P3-COMM-07 | 社区增长 | 推荐、专题、活动、排行榜、创作者主页和消息 |
| P3-TEAM-08 | 团队能力 | 组织、角色、审批、共享打印机、配额和审计 |
| P3-DATA-09 | 数据平台 | 行为分析、打印成功率、模型质量和运营指标 |
| P3-EXT-10 | 扩展生态 | 协议插件、设备能力插件、材料和配置扩展 |

三期的重点不是简单增加页面，而是保证模型版本、打印机能力、切片配置、支付权益和打印结果之间可追溯。

## 风险

| 编号 | 风险/限制 | 影响与处理要求 |
|---|---|---|
| R-01 | 首批设备、协议和控制命令范围尚待确认 | 当前以 Moonraker 为建议基线；开发前冻结测试设备及协议版本 |
| R-02 | SDK 契约不等于完整移动 SDK | C ABI poll-only 事件语义和 Moonraker Fake transport 契约已有父任务门禁；callback、Android/iOS bridge、真实 transport 和设备联调仍需独立验收 |
| R-03 | 移动端权限、后台恢复和局域网发现存在平台差异 | 保留手动 IP 回退；Android/iOS 分别用真机验证 |
| R-04 | 重连与超时可能造成重复控制 | 复用 command_id；非幂等命令状态未知时先对账 |
| R-05 | 账号设备记录与局域网控制授权不能混用 | 绑定云端元数据不等于已获得设备凭据或可控制权限 |
| R-06 | 一期膨胀为全平台开发 | 支付、商城、社区、AI、远程链路和完整本地切片继续按后续阶段展开 |
| R-07 | 许可证和专有组件边界待评审 | 保留为 SDK 商业分发前置检查；本报告不构成法律结论 |
| R-08 | 自动化门禁不能替代现场验收 | VS SDK-only configure/build、CTest、boundary、exports 和 C11 已通过；真实 transport/设备、Android/iOS 真机和现场打印仍未验证 |

## 建议

### 5.1 模块依赖和并行开发

```text
P1-SDK-01 ──┬── P1-SDK-02 ──┬── P1-SDK-04 ──┬── Mobile SDK 绑定
            │                └── P1-MOB-01 ── P1-MOB-02 ── P1-MOB-03 ── P1-MOB-04
            └── P1-SDK-03 ───────────────────┘

P1-DB-01 ─── P1-BE-01 ─── P1-BE-02

P1-SDK-03 + P1-MOB-03 + P1-BE-02
                    ↓
              P1-QA-01 局域网端到端
```

可以并行开发的工作：

- SDK 契约和 C ABI；
- 协议模拟器和 Moonraker 适配器；
- Flutter 页面和 Mock SDK；
- Android/iOS 权限和发现插件；
- 数据库迁移和最小账号服务；
- 集成测试夹具和状态录制。

必须串行确认的工作：

- SDK 公共字段和错误码冻结后再生成移动端绑定；
- 协议状态模型稳定后再做监控页面最终验收；
- 设备绑定和权限模型确定后再接远程控制；
- 模型权益和支付账本确定后再做付费下载；
- 版本化模型产物确定后再做本地切片和打印任务追溯。

### 5.2 一期交付清单

一期完成标准是：

- `OrcaSlicer::MobileSDK` 契约和测试通过；
- C ABI 门面和版本信息可用；
- 首批局域网协议适配器可连接真实设备或协议模拟器；
- Android/iOS 可以自动发现或手动输入 IP；
- 移动端可以查看在线状态、打印状态、温度、进度和错误；
- 移动端可以执行暂停、恢复、停止等基础控制；
- 断网、重连、设备重启和重复命令有明确处理；
- 最小账号、设备绑定和数据库迁移可运行；
- 所有关键命令都有日志和回执；
- 有 Android/iOS 真机测试记录；
- 有 SDK、移动端、后端和数据库的版本兼容记录。

### 5.3 建议执行顺序与评审口径

1. 确认首批设备、命令范围和一期摄像头边界，冻结 P1-SDK-01 中立契约。
2. 分别推进 C ABI 门面、首批协议适配器、移动端 Mock SDK 骨架和数据库/最小账号服务。
3. 接入 Android/iOS 产物和局域网发现后，按连接 → 状态监控 → 控制回执完成设备闭环。
4. 将账号/设备元数据验证与纯局域网链路分开执行，避免云端故障阻塞本地能力验收。
5. 用一期交付清单评审一期完成情况；二、三期框架只作为后续细化输入。

此顺序表达依赖关系，未承诺日历工期、团队人数或固定上线日期。

## 证据

### 6.1 依据与材料索引

| 材料 | 用途 |
|---|---|
| [原 Markdown 报告](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/original/REQUIREMENTS-ANALYSIS.md) | 规范化前的需求基线，保留全部原始章节 |
| [原 HTML 报告](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/original/REQUIREMENTS-ANALYSIS.html) | 规范化前的浏览版备份 |
| [工程拓扑](PROJECT-TOPOLOGY.md) | 桌面、移动、后端和基础设施边界 |
| [MobileSDK API](API.md) | 现有 SDK 数据契约和接口说明 |
| [任务决策](../../../../../control/tasks/task-20261016013901509-0f210b/DECISIONS.md) | 已记录的工程边界与切片演进决策 |
| [新版任务 Skill](C:/Users/1/.agents/skills/codex-task-continuation/codex-task-continuation/SKILL.md) | 用户审核产物目录与报告身份规范 |
| [分析报告模板](../../../../../control/templates/reports/analysis-report.md) | 结论、影响范围、关键发现、风险、建议、证据和未完成项 |
| [源版本与未提交差异清单](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/source-state.json) | HEAD、分支、工作树状态及文件 SHA-256 |
| [内容与结构校验](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/validation.json) | 需求编号、原文保留、目录锚点、表格、编码与链接检查 |
| [本轮交付文件清单](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/delivery-manifest.json) | MD/HTML、阅读副本和证据的哈希及身份 |
| [可复现报告生成器](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/build-report.cjs) | 从备份重建标准版与工程阅读副本 |

### 6.2 验证范围

- 本轮执行文档结构、需求编号完整性、旧正文保留、UTF-8 编码、HTML 目录锚点、本地链接、无外部资源及报告文件哈希核对。
- 页面截图和静态页面检查属于文档可读性证据；不代表打印、支付或其他产品功能通过。
- 不沿用旧报告“已打开预览”等描述作为视觉验收；本轮生成的实际页面材料单独归档。
- VS 17 2022 x64 SDK-only configure/build、CTest 全量和标签测试、boundary check、C ABI exports/C11 已通过；Android/iOS 真机、真实 transport/设备与数据库迁移仍未验收。

### 6.3 页面复核记录

- [浏览器与视口检查结果](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/browser-check.json)：Microsoft Edge 检查桌面 1440px 与窄屏 390px；正文无页面级横向溢出，目录锚点均有效。
- [桌面页面截图](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/desktop-preview.png) · [窄屏页面截图](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/mobile-preview.png) · [一期需求详情截图](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/requirements-preview.png)。
- 页面检查只覆盖报告展示；未执行任何打印设备或移动 App 功能验收。

## 未完成项

### 7.1 待确认事项

以下事项不阻塞本报告，但在一期开发开始前需要确认：

1. 首批真实打印机型号和协议是否以 Moonraker 为主；
2. 一期是否允许未登录用户使用局域网控制；
3. 首期控制范围是否包含开始打印，还是只做暂停/恢复/停止；
4. 移动端是否需要摄像头快照，还是二期再做视频；
5. 账号后端的部署环境和身份提供商；
6. C ABI 由 Flutter FFI 直接消费，还是同时发布 AAR/XCFramework；
7. OrcaSlicer 切片核心的许可证、第三方依赖和商业分发边界；
8. 一期真实设备测试机和协议模拟器的归属。

在这些事项确认前，建议保持接口可扩展，不把厂商专有字段和支付供应商字段写进公共 SDK 契约。

### 7.2 尚待实施或验证的事项

- 需求评审：报告已按新规范整理，首批设备、登录策略、开始打印和摄像头范围仍待明确。
- 实现交付：一期 13 个需求包仍需按任务和证据逐项确认，不能以报告存在认定完成。
- 工具链与验证：VS 17 2022 x64 SDK-only configure/build、CTest 全量和标签测试、boundary check、C ABI exports/C11 已通过；Android/iOS 产物和 bridge 检查仍待运行。
- 现场验证：真实 HTTP/WebSocket transport、协议模拟器以外的设备、局域网发现、状态流、断网重连、控制回执和真机测试仍待执行。
- 后续细化：二、三期在相应阶段补齐详细规格、接口、数据模型和验收标准。
- 发布状态：父 worktree 已生成本地整合提交，尚未推送或发布；旧 SDK 快照保留在 stash `06b9843cc5be8294279e4ce095903141ea8a215c`。
