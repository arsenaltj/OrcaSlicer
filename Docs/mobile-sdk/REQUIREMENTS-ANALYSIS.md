# 3D 打印平台工程架构与三期需求分析报告

- 报告类型：需求与系统架构分析（按工作区 analysis-report 模板整理）
- 文档版本：1.7；原报告版本：1.6
- 任务 ID：`task-20261016013901509-0f210b`
- 父任务：`task-20261015124945955-67069d`
- 报告更新时间：2026-10-19（Asia/Shanghai）
- 报告状态：供应商通信需求修订稿；代码基线与自动化证据已更新，真实供应商设备、正式远程链路和 iOS 仍未验收
- 分析仓库：`OrcaSlicer`
- 分析分支：`task/task-20261016013901509-0f210b`
- 分析版本：OrcaSlicer `f43723fcf2999c4bc00236d1dc9bd5fd0fda717e`；Android FFI `97241360f278a0fc350a5daa7aace97c7f7a94e3`；供应商与竞品证据截至 2026-10-19
- 工作树：当前 OrcaSlicer worktree（分支 `task/task-20261016013901509-0f210b`）
- 差异身份：[源版本、工作树状态与逐文件 SHA-256](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-standardization-20261016/source-state.json)
- 权威归档目录：工作区 `artifacts/task-20261016013901509-0f210b/reports/requirements-analysis-v1.7.*`；工程内文件为同步阅读副本
- 阅读入口：[Markdown](REQUIREMENTS-ANALYSIS.md) · [HTML](REQUIREMENTS-ANALYSIS.html)

## 结论

产品按桌面端、移动端、用户前台、独立管理后台和平台后端建设，但本轮证据已经改变了打印机接入的基本假设：

- 供应商只提供一个型号，型号范围不再作为多型号规划问题；需要冻结的是该型号的硬件、固件和通信协议版本。
- 用户账号、登录、设备归属、权限和审计由我方自建。供应商不需要提供用户账号体系，但必须提供设备级身份、配对、证书和通信授权。
- 供应商确认当前版本局域网使用 WebSocket + HTTP，规划版本将不再使用 Moonraker，改为设备侧 MQTT Broker + 文件服务器。当前版本和规划版本不能在一期需求中混为一个已验证协议。
- 供应商尚无成熟可商用的远程通信方案，远程方向为 MQTT + TLS、Broker 转发、云端文件下载和 Amazon Kinesis Video Streams。设备主动建立 TLS 出站连接可以作为 NAT/CGNAT 穿透的基础路径，但 Broker 归属、服务端 API 和正式远程验收仍未冻结。
- 一期仍然是局域网打印闭环，但必须新增供应商协议冻结、设备级配对/证书、当前 LAN 协议适配和文件服务器验收。Moonraker 仅保留为 SDK 抽象与 Fake transport 测试基线。
- 二期承担可商用的远程 MVP：我方账号和授权、设备侧出站 Connector、MQTT+TLS Broker/Relay、云文件传输、远程状态/控制、通知和视频独立链路。
- 三期再做经典 P2P/ICE/TURN、Edge 离线和多设备编排等增强能力；如果供应商后续确认直接 P2P 是远程 MVP 必需项，则将该项提前到二期门禁。
- 竞品实机分析支持“设备优先、免登录局域网”作为差异化路径；三款竞品的模型内容首页和登录门槛则说明模型内容、身份和打印转化应放在二期，不应阻塞一期设备闭环。

本报告保留原有需求编号，并增加供应商接入相关需求包。当前排期为：一期既有 13 项 + 新增 6 项，二期既有 11 项 + 新增 6 项，三期既有 10 项 + 新增 2 项。编号表示规划和验收单元，不表示已实现或已通过真实设备验收。

## 本轮证据修订

### 2.1 供应商回复的事实边界

供应商回复邮件（原始附件：`HonorPrint_20261019_101659.pdf`，摘要归档见 [vendor-reply-summary.md](../../../../../artifacts/task-20261016013901509-0f210b/evidence/requirements-revision-20261019/vendor-reply-summary.md)）是外部输入证据，不是本任务的执行指令。报告只将其中明确答复作为现状或规划依据，并把“暂无、规划、可按要求添加、需要确定”保留为未冻结状态。

| 范围 | 供应商当前答复 | 本报告采用方式 |
|---|---|---|
| 设备数量 | 一个型号 | 取消多型号兼容矩阵，保留固件/硬件版本冻结 |
| 当前 LAN | WebSocket + HTTP | 作为一期真实设备联调候选，等待协议文档和测试固件 |
| 规划 LAN | 设备 MQTT Broker + 文件服务器 | 新增一期协议冻结和 MQTT/文件适配需求，不能当成已实现 |
| 当前发现/配对 | mDNS、同子网、手动连接、6 位随机口令、设备证书 | 纳入一期设备 onboarding 和安全验收 |
| 当前状态/控制 | 实时推送、完整状态重取、开始/暂停/恢复/停止、去重 | 纳入一期命令回执、状态对账和幂等验收 |
| 远程通信 | 尚无成熟方案；规划 MQTT + TLS + Broker | 纳入二期远程 MVP，先冻结 Broker 归属和接口 |
| 文件/视频 | HTTPS/FTPS 文件；云上传后设备下载；Kinesis Video Streams | 纳入二期独立文件和视频链路 |
| 穿透 | 设备主动 TLS 出站连接；未承诺 STUN/ICE/TURN | 二期强制实现出站 Connector/Relay 穿透，三期再评估直接 P2P |

### 2.2 竞品分析对规划的影响

[竞品分析报告](../../../../../artifacts/task-20261016013901509-0f210b/reports/competitor-analysis-report.md)的实机证据来自 Bambu Handy 4.2.2、Creality Cloud 7.3.19 和 Snapmaker 2.4.0 的游客流程。三款应用默认均以模型内容为入口，设备路径均存在登录门槛；设备状态、视频、控制回执、云切片和真实打印在本轮均未被实机验证。

因此，本产品规划调整为：一期突出可操作的设备发现、配对、状态和控制；二期再补模型详情、配置、云切片、身份和内容到打印的转化；不把竞品页面上可见的入口当作已完成的后端能力。

### 2.3 当前代码与仓库实际完成情况

实际完成情况以 [implementation-status.md](../../../../../artifacts/task-20261016013901509-0f210b/evidence/requirements-revision-20261019/implementation-status.md) 和各任务验收报告为准：

| 需求域 | 当前证据 | 当前结论 |
|---|---|---|
| OrcaSlicer MobileSDK | `f43723fcf2999c4bc00236d1dc9bd5fd0fda717e`；SDK-only、C ABI 和契约测试通过 | 公共契约已完成；真实设备协议未接入 |
| Android FFI | `97241360f278a0fc350a5daa7aace97c7f7a94e3`；Android ABI、APK/AAB、arm64 验证通过 | Android bridge 已有独立证据；iOS/macOS 仍延后 |
| 设备协议 | Moonraker Fake transport 通过；concrete transport 的 ON 配置和 loopback/TLS 未通过门禁 | 不能把 Moonraker 作为 WonderMaker 真实协议 |
| Flutter 应用 | 页面、Mock SDK、生命周期、诊断和构建骨架已具备 | UI/Mock 已有，真实发现/状态/控制未验收 |
| 平台后端 | NestJS、迁移、种子、内部审计和 501 预留接口 | 后端基线已完成，账号/设备/远程业务尚未交付；同时为用户前台和管理后台提供独立 API |
| 用户前台 | 面向终端用户的 Web 页面、账户、设备、打印任务、模型和通知 | 与管理后台同仓库但独立应用、独立路由、独立编译和独立发布；一期先做骨架和契约联调 |
| 管理后台 | 面向运营/客服/设备支持人员的设备、任务、内容、审计和诊断页面 | 与用户前台同仓库但独立鉴权、导航、编译和发布；一期先做最小运维闭环 |
| 现场验收 | 无真实设备、无供应商 MQTT/文件服务器联调 | 一期闭环仍未通过现场验收 |

### 2.4 账号边界

账号体系由我方自建。供应商不参与用户登录和用户权限判断；供应商只负责设备级身份和通信授权。远程控制的授权链路固定为“我方用户账号 → 我方设备绑定/权限 → 我方后端命令审计 → 供应商设备 Connector/Broker → 打印机”，移动端不直接持有供应商云端长期密钥。

## 影响范围

| 工程/领域 | 职责与交付物 | 本轮修订后的边界 |
|---|---|---|
| OrcaSlicer / MobileSDK | 中立 DTO、命令信封、连接路径和协议适配边界 | 公共契约、C ABI 和路径状态机已通过；需新增 WonderMaker WebSocket/HTTP 与 MQTT/文件适配，不把供应商字段直接写入公共 DTO |
| orca-mobile | 根目录 Flutter 应用、`lib/features` 页面、`packages/printer_client`、`packages/orca_sdk_bridge`、`packages/platform_services`、Android/iOS 宿主和测试工具 | Android FFI 已有证据；真实设备发现、PIN 配对、状态流、控制和 iOS bridge 仍待完成 |
| orca-platform | NestJS 模块化单体基线、`src/modules`、OpenAPI/事件契约、数据库迁移、部署和测试 | 当前只有后端基线；identity/devices 仍为 501 预留，实时网关、Worker、远程控制和管理 API 尚未形成独立目录或业务实现 |
| 供应商设备/固件 | 单一型号的 LAN 协议、设备身份、配对、证书、Connector、远程 MQTT/TLS 和文件/视频能力 | 供应商需提供协议文档、测试固件、设备级凭据和联合验收支持；用户账号由我方管理 |
| 基础设施 | Broker/Relay、对象存储、CDN、队列、实时网关、监控、密钥和证书 | Broker 归属尚未确定；二期必须先做责任边界、证书生命周期和故障恢复设计 |
| 验证与运维 | 协议模拟器、真机、断网/重启/穿透、日志、版本和回滚 | 当前自动化契约证据已有，供应商设备与远程链路验收未完成 |

| 本轮变更对象 | 操作 |
|---|---|
| 工程阅读副本 | 更新 `Docs/mobile-sdk/REQUIREMENTS-ANALYSIS.md/.html` 到 v1.7 |
| 权威归档 | 新增 `artifacts/task-20261016013901509-0f210b/reports/requirements-analysis-v1.7.md/.html` |
| 外部证据 | 保存供应商答复摘要、实现状态快照和旧版报告备份 |
| 代码 | 本轮不修改 C++、Flutter 或后端实现；需求先按供应商协议冻结后再开实现任务 |

## 关键发现

### 3.1 整体架构

```text
┌─────────────────────────────┐
│ Mobile App                  │
│ Flutter + Android/iOS host │
│ LAN discovery / pairing     │
└──────────────┬──────────────┘
               │ HTTPS / WebSocket / FFI
               ▼
┌────────────────────────────────────────────┐
│ 我方 Platform Backend                      │
│ 自建账号 · 设备绑定 · 授权 · 审计          │
│ Remote Command · Realtime · Notification   │
└──────────────┬──────────────┬──────────────┘
               │              │
        LAN direct      MQTT/TLS Broker/Relay
               │              │
┌──────────────▼──────────────▼──────────────┐
│ WonderMaker 单型号打印机                    │
│ 当前 LAN：WebSocket + HTTP                  │
│ 规划 LAN：设备 MQTT Broker + 文件服务器     │
│ 远程：设备主动 TLS 出站 Connector           │
│ 文件：HTTPS/FTPS 或云端下载                 │
│ 视频：独立 Kinesis Video Streams 链路       │
└────────────────────────────────────────────┘
```

#### 3.1.1 数据面和控制面

控制面由我方平台负责用户身份、设备绑定、授权、会话、命令审计、路径选择和状态对账；供应商提供设备级身份、证书和设备连接能力。数据面按能力拆分为：

- **LAN 控制/状态**：一期直连设备，当前协议是 WebSocket + HTTP；规划固件可能切换为设备 MQTT Broker + 文件服务器。
- **远程控制/状态**：二期由设备主动建立 MQTT + TLS 出站连接到 Broker/Relay。移动端不直接连接设备，也不保存供应商长期密钥。
- **文件传输**：LAN 使用设备文件服务器；远程先上传云端，再由设备下载，支持校验、断点续传和进度。
- **视频**：独立于打印控制和状态通道，供应商规划使用 Amazon Kinesis Video Streams；视频故障不能阻断控制命令。
- **路径切换**：LAN 优先；远程进入 Broker/Relay。经典 P2P 只有在三期完成 STUN/ICE/TURN 方案冻结后再启用。

“穿透”在二期的最低定义是：设备在家庭 NAT/CGNAT/企业防火墙后，通过设备侧 Connector 主动建立 TLS 出站连接，不要求用户开放入站端口；当直接出站路径受限时，使用 Broker/Relay 回退。该定义与供应商当前答复一致，也为后续直接 P2P 留出扩展点。

### 3.2 仓库和代码工程规划

推荐的产品级仓库布局如下：

```text
repos/
├── OrcaSlicer/                  # 桌面端、libslic3r、MobileSDK 源码
├── orca-mobile/                 # Flutter 应用、Android/iOS 宿主和插件
├── orca-frontend/               # 用户前台与管理后台 monorepo，按应用编译隔离
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

#### 3.2.2 移动端（按当前本地仓库）

以下结构以工作区中 `orca-mobile` 仓库的当前代码为准；报告中的目录不是当前仓库事实的推测：

```text
orca-mobile/
├── lib/
│   ├── app/                      # 配置、启动、路由、主题、会话
│   ├── core/                     # Mock、错误、日志、本地化、设计系统
│   └── features/                 # shell、discover、devices、monitor、mine、services、settings
├── packages/
│   ├── printer_client/           # PrinterClient、模型、Mock/Unavailable 实现
│   ├── orca_sdk_bridge/          # C ABI/FFI、Native/Mock SDK bridge
│   └── platform_services/        # Android/iOS 平台能力抽象
├── android/                      # Android 宿主、JNI/FFI 原生库和权限
├── ios/                          # iOS 宿主工程
├── integration_test/             # App smoke、Android SDK FFI 集成测试
├── test/                         # Flutter、Widget、FFI、生命周期和 Mock 测试
├── tool/                         # 环境校验、配置校验、Android SDK 构建工具
├── config/                       # development/test/production 配置样例
├── docs/                         # ownership、handoff、模块和参考分析文档
├── pubspec.yaml                  # 根应用和三个本地 package 依赖
├── sdk.lock.json                 # MobileSDK 产物和 ABI 锁定信息
└── BUILDING.md / DECISIONS.md
```

当前代码的真实依赖方向是：

```text
lib/features + lib/core
          ↓
packages/printer_client   packages/orca_sdk_bridge   packages/platform_services
          ↓                         ↓                         ↓
     Mock/Unavailable       C ABI/FFI bridge          Android/iOS host
```

当前仓库没有 `apps/mobile/`、顶层 `native/` 或独立 `api_client/`、`model_viewer/` package。真实打印机协议、WonderMaker 适配器和后端 API Client 尚未进入当前代码基线；后续可以在既有 `packages/` 边界内新增，但必须先冻结契约。

移动端不能直接访问数据库、支付服务端密钥、AI Provider 或厂商私有云。所有账户、权益、模型下载和远程命令都必须经过后端授权。

#### 3.2.3 后端（按当前本地仓库）

以下结构以工作区中 `orca-platform` 仓库的当前代码为准：

```text
orca-platform/
├── src/
│   ├── main.ts / bootstrap.ts / app.module.ts
│   ├── common/                   # errors、logging、request-context
│   ├── config/                   # 环境和配置校验
│   ├── database/                 # PostgreSQL 连接和事务服务
│   └── modules/
│       ├── health/               # liveness/readiness
│       ├── contracts/            # OpenAPI 文档入口
│       ├── identity/             # 当前为 501 预留
│       ├── devices/              # 当前为 501 预留
│       └── audit/                # 内部审计写入
├── contracts/
│   ├── http/openapi.yaml         # HTTP 契约唯一来源
│   └── events/README.md          # 事件契约入口
├── db/
│   ├── migrations/               # 001_initial.sql
│   └── seeds/                    # development seed
├── deploy/                       # Dockerfile、Compose 和本地部署样例
├── scripts/                      # migrate、seed、compose 和数据库辅助
├── test/
│   ├── unit/
│   ├── contract/
│   ├── integration/
│   └── fixtures/
├── docs/                         # ownership、modules-roadmap、handoff
├── package.json / tsconfig.json / nest-cli.json
└── BUILDING.md / README.md
```

当前后端是一个 NestJS 模块化单体基线，实际领域边界通过 `src/modules`、`contracts` 和数据库迁移表达。当前没有 `apps/api/`、`apps/realtime-gateway/`、`apps/worker/`、`apps/admin-api/`、`apps/scheduler/` 或 `packages/*` 目录；这些是后续扩展方向，不能作为当前实现状态描述。

当前可确认的后端能力包括健康检查、配置校验、请求上下文、错误格式、日志、PostgreSQL 迁移/种子、内部审计、OpenAPI 契约和开发部署基线。identity/devices 仍返回 501，正式账号、设备注册、能力查询、实时网关、远程命令、Worker 和管理 API 需要后续在现有 NestJS 模块结构上逐项实现。

第一阶段继续保持模块化单体；只有在接口、负载和部署边界稳定后，才考虑把 realtime、worker 或 admin API 拆成独立进程或应用。用户前台和管理后台通过 `orca-frontend` 的 `apps/web`、`apps/admin` 访问不同后端契约，但不应据此反推后端当前已经存在对应目录。

#### 3.2.4 用户前台与独立管理后台

```text
orca-frontend/apps/web              orca-frontend/apps/admin
├── 用户账号/设备/打印任务         ├── 管理员登录与 RBAC
├── 模型/切片/下载/通知             ├── 用户与设备管理
├── 远程监控与控制入口             ├── 打印任务/异常/重试
└── 公开内容与帮助                  └── 内容审核/审计/诊断
          │                                  │
          └──────── OpenAPI / Events ───────┘
                         │
                 orca-platform API
```

- `orca-frontend` 是一个前端 monorepo，`apps/web` 和 `apps/admin` 是两个独立应用和独立编译目标。
- 两个应用分别配置入口、依赖边界、环境变量、构建命令、测试套件、产物目录、部署目标和回滚版本；一个应用的编译不能隐式拉入另一个应用的页面、路由、管理员会话或业务状态。
- 两者可以共享版本化的 API client、设计 token、基础 UI 包和事件类型，但不共享路由、权限判断、页面容器或管理业务状态；共享包不能依赖任一应用。
- 用户前台只能访问面向用户的 API；管理后台使用独立的管理员身份、RBAC、审计和 `admin-api`。后端必须在服务端再次校验权限，不能把前端隐藏菜单当作授权。
- 一期交付同一仓库内两个可独立编译的前端骨架、登录/权限边界、设备与任务最小页面、错误/空态/审计入口和契约测试；二期补齐模型、云切片、远程控制和运营页面；三期补齐团队、内容生态和数据分析。

#### 3.2.5 数据库和存储

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

现有 SDK 契约继续保持供应商中立，并补充以下连接与协议语义：

- `ConnectionPath`：`LAN`、`RemoteBroker`、`Relay`；`P2P` 作为三期可选路径保留。
- `ConnectionStateMachine`：发现、探测、配对、认证、连接、已连接、状态同步、切换、过期、离线。
- `PrinterCapability`：状态流、设备配对、证书认证、G-code 上传、开始/暂停/恢复/停止、摄像头、文件续传。
- `PrinterStatus`：设备状态、进度、温度、当前文件、错误、设备端时间、事件序号和最后同步时间。
- `CommandEnvelope`：会话、设备、命令、序列号、过期时间、幂等键和 payload。供应商去重能力必须通过适配器转换为统一的命令状态。
- `TransferJob`：文件 ID、大小、摘要算法、摘要、已上传偏移、状态、重试策略和设备下载状态。
- `RemoteSession`：设备 ID、平台会话、设备证书标识、连接路径、Broker/Relay 提供方、租约和状态同步标记。

供应商的 MQTT Topic、WebSocket 字段、证书对象和文件服务器私有字段只能进入协议适配器，不能进入公共头文件。

#### 3.3.2 API 契约

| 接口 | 用途 | 阶段 | 归属 |
|---|---|---|---|
| Local SDK API | 移动端调用本地 SDK | 一期 | OrcaSlicer / C ABI |
| Device Pairing API | 设备级配对、PIN、证书和撤销 | 一期 | 移动端 + 供应商设备 |
| Query API | 我方账号、设备、能力和配置查询 | 一期/二期 | 我方后端 |
| Remote Command API | 后端授权后生成远程打印命令 | 二期 | 我方后端 |
| Broker/Connector API | 设备上线、租约、命令和事件 | 二期 | 供应商 + 我方 Broker 方案 |
| Transfer API | LAN/云端文件上传、校验、续传和设备下载 | 一期/二期 | 供应商文件服务 + 我方对象存储 |
| Realtime API | 状态、进度、告警和通知 | 一期本地；二期远程 | MQTT/WebSocket/HTTPS 适配 |
| Video API | 快照、视频、连接授权和断流 | 二期 | 供应商视频服务 |

#### 3.3.3 任务模型

AI、切片、文件传输、远程命令和打印都使用任务化状态：

```text
created → validating → queued → running → completed
 ├──────→ failed
 └──────→ canceled
```

每个任务必须具备 `request_id`、`job_id`、`state`、`progress`、`error_code`、`retryable`；文件任务还必须具备 `artifact_sha256` 或明确的摘要算法、已确认偏移和设备端最终校验结果。远程控制状态未知时先对账，不自动重发开始、停止等非幂等命令。

### 3.4 三期开发计划总览

| 阶段 | 原规划目标 | 本轮修订后的主线 | 新增重点 |
|---|---|---|---|
| 一期 | 局域网打印闭环 | 单型号 WonderMaker 的协议冻结、发现/配对、状态、控制、文件和诊断闭环 | 当前 WebSocket+HTTP 与规划 MQTT/文件服务器的协议适配；设备证书和 PIN；供应商真机验收 |
| 二期 | 平台化和远程化 | 我方账号/授权、设备云端管理、设备出站 Connector、MQTT+TLS Broker/Relay、远程控制、云文件和视频 | 设备侧 NAT/CGNAT 穿透、Broker 归属、远程审计、状态对账、通知 |
| 三期 | 生态化和专业化 | 直接 P2P/ICE/TURN（如仍有必要）、Edge 离线、多设备编排、本地切片、模型和团队生态 | 连接路径优化、边缘容灾、生产级网络适配和规模化运营 |

本轮需求数量按“原有包 + 新增包”记录：一期 13+6，二期 11+6，三期 10+2。远程 MVP 的关键路径不再等到三期：二期必须完成设备主动 TLS 出站和 Broker/Relay 回退；三期的 P2P 仅是直接连接增强项。用户前台和管理后台的代码、路由、鉴权、编译目标和发布流水线始终解耦：一期在同一仓库的 `apps/web` 与 `apps/admin` 中完成独立骨架，二期按业务领域分别交付，三期继续独立演进。

| 新增需求包 | 目标阶段 | 依赖 | 当前状态 |
|---|---|---|---|
| P1-VENDOR-01 | 一期 | 供应商样机、固件、协议文档 | 待供应商冻结 |
| P1-CONN-02 | 一期 | device_id、PIN、设备 CA/CRL | 需求已明确，代码未实现 |
| P1-FILE-01 | 一期 | 设备文件服务器、摘要/续传协议 | 供应商口头确认，接口未冻结 |
| P1-QA-02 | 一期 | 真机、测试固件、协议模拟器 | 尚未开始现场验收 |
| P1-WEB-01 | 一期 | OpenAPI、账号和设备领域契约 | `orca-frontend/apps/web` 骨架待建 |
| P1-ADMIN-01 | 一期 | admin-api、管理员身份和审计契约 | `orca-frontend/apps/admin` 骨架待建 |
| P2-CONN-03 | 二期 | Broker 归属、设备 Connector、TLS 证书 | 供应商规划中 |
| P2-FILE-04 | 二期 | 对象存储、设备云下载、校验 | 供应商规划中 |
| P2-MEDIA-05 | 二期 | Kinesis Video Streams 或等价服务 | 供应商规划中 |
| P2-OBS-06 | 二期 | 远程事件、审计、日志和告警 | 待设计 |
| P2-VENDOR-07 | 二期 | Server-to-Server/API 或 Broker 接入 | 供应商尚未提供 |
| P2-WEB-08 | 二期 | 模型、切片、远程打印和通知 API | 用户前台业务页面待建 |
| P3-CONN-04 | 三期 | P2 远程链路稳定、STUN/ICE/TURN 方案 | 可选增强 |
| P3-EDGE-05 | 三期 | 设备侧 Connector、边缘网关和离线策略 | 规划项 |

### 3.5 一期详细需求规格

#### 3.5.1 一期目标和边界

一期完成单一 WonderMaker 型号的局域网闭环，不依赖互联网、供应商用户账号或我方云端远程服务：

```text
授予局域网权限或输入打印机 IP
 ↓
同一子网 mDNS 发现或手动连接
 ↓
6 位随机口令配对，建立设备级证书/授权
 ↓
读取能力和完整状态
 ↓
订阅状态、温度、进度、文件和错误
 ↓
上传并校验 G-code，必要时断点续传
 ↓
开始、暂停、恢复、停止并读取设备回执
 ↓
断网/重启后重连、状态对账和诊断
```

一期协议策略：

- 真实设备协议以供应商提供的当前测试固件和文档为准。供应商当前答复是 WebSocket + HTTP；规划协议是设备 MQTT Broker + 文件服务器，必须先完成版本冻结。
- Moonraker 适配器只作为已有 SDK/Fake transport 的契约测试，不作为 WonderMaker 真实设备接入承诺。
- 局域网临时连接可以免登录；需要远程控制、设备云端管理、通知、模型权益或跨设备共享时，使用我方自建账号。
- 摄像头只做独立快照/视频需求预留，一期可验收快照是否不影响打印控制，视频流放二期。
- 开始打印要绑定已校验文件、设备能力和当前状态；停止等高风险命令需要二次确认。

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

#### 3.5.4 P1-SDK-03：WonderMaker 局域网协议适配与协议冻结

**目标**：把供应商单型号的当前 LAN 协议和规划 LAN 协议纳入可替换适配器，不把尚未冻结的 MQTT 私有字段写死到公共 SDK。

**需求**：

- 供应商提供当前测试固件、协议版本、端口、接口文档、状态字段、命令回执、错误码和文件服务接口。
- 适配器第一阶段支持当前 WebSocket + HTTP；供应商协议冻结后增加设备 MQTT Broker + 文件服务器适配。
- 支持 mDNS 探测、手动 IP/端口连接和同子网限制的明确提示。
- 支持设备级凭据、6 位随机口令配对、证书校验、授权撤销和重新配对。
- 状态通道优先使用实时推送；如果供应商 MQTT/推送不可用，使用有节流的轮询或完整状态查询回退。
- 支持设备能力、状态快照、开始/暂停/恢复/停止、文件上传、摘要校验、断点续传和自动打印。
- 重连后先获取完整状态，再恢复状态订阅；重复命令使用统一 `command_id` 对账，不重复执行非幂等动作。
- 将供应商协议错误转换为稳定 SDK 错误码，记录 `device_id`、`session_id`、`command_id`、协议版本和适配器版本。

**验收**：

- 真实测试设备或供应商协议模拟器完成发现、配对、连接、状态推送、控制回执、文件上传、断线和重连。
- 当前 WebSocket + HTTP 与规划 MQTT 适配器分别有版本和测试夹具；在协议未冻结前不得宣称 MQTT 已完成。
- Moonraker Fake transport 测试继续通过，但单独标记为兼容性测试，不计入 WonderMaker 现场验收。

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

#### 3.5.7 P1-MOB-02：局域网发现、手动连接和设备配对

**目标**：让移动端在免登录条件下发现单型号打印机，并通过设备级 PIN 和证书完成安全配对。

**需求**：

- 首次进入设备页申请 Android/iOS 本地网络权限；同一子网使用 mDNS 自动发现。
- 发现失败时支持手动输入 IP、端口和协议类型；明确提示 mDNS 不能跨子网发现。
- 连接前执行协议探测、能力读取和设备身份展示；设备卡片显示不可变 `device_id`、序列号、型号、固件和地址摘要。
- 配对时要求设备侧 6 位随机口令确认；本地保存非敏感连接信息和受保护的设备凭据引用，不保存明文密钥。
- 校验证书链、设备 CA、撤销名单/撤销结果和重新配对流程；区分发现、探测、配对、认证和连接错误。
- 支持删除设备、撤销授权、重新配对和地址修改；不依赖供应商用户账号。

**验收**：同一子网自动发现、手动 IP 连接、PIN 配对、证书校验、撤销后拒绝访问、恢复出厂/重新配对和无网络权限引导均有真机记录。

#### 3.5.8 P1-MOB-03：打印状态监控

**目标**：在移动端通过供应商实时推送或协议回退持续显示单型号设备的真实状态。

**监控字段**：在线状态、打印状态、当前文件、进度、预计剩余时间（设备提供时）、喷嘴/热床当前与目标温度、当前层数（设备提供时）、错误、告警、设备端时间、事件序号、最后同步时间、连接路径和协议版本。

**需求**：

- 当前设备使用 WebSocket+HTTP 时支持状态推送/轮询；规划 MQTT 协议冻结后优先使用 MQTT 实时推送。
- 页面明确区分正在连接、已配对、已连接、状态过期、已断开、离线和缓存状态。
- 状态通道中断后先标记过期；重连后获取完整状态，再恢复订阅，不能用旧事件覆盖新状态。
- 状态事件需要节流、去重和序列检查；页面销毁不能关闭其他页面共享的连接。
- 摄像头/视频通道独立；视频异常不影响状态和控制。

**验收**：协议模拟器和真实设备分别验证状态变化、温度/进度、WebSocket/MQTT 中断、设备重启、状态过期、完整状态对账和多页面共享连接。

#### 3.5.9 P1-MOB-04：基础打印控制

**目标**：使用统一命令信封和供应商设备去重能力完成局域网控制。

**一期命令**：`printer.start`、`printer.pause`、`printer.resume`、`printer.stop`、`printer.refresh_status`。

**需求**：

- 开始打印前校验文件摘要、上传完成、设备能力、当前状态和必要温度/前置条件。
- 停止命令二次确认；每次用户操作只生成一个 `command_id`，网络重试复用原 ID。
- 设备已答复具备去重能力，但必须通过真实设备或供应商模拟器验证重复点击、超时和重连场景。
- 命令状态显示发送中、已接受、执行中、已完成、失败和状态未知；状态未知先刷新状态，不自动重发开始/停止。
- 控制成功后重新获取完整设备状态；设备拒绝时显示稳定错误码和可操作提示。

**验收**：暂停、恢复、停止和开始打印在供应商设备或等价协议模拟器上完成闭环，并有命令 ID、设备回执和最终状态证据。

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

#### 3.5.11 P1-BE-01：我方自建账号服务

**目标**：为二期远程控制建立我方身份和权限底座，账号实现不依赖供应商账号。

**一期范围**：注册、登录、刷新会话、退出、用户基本资料、设备绑定关系、账号注销、登录审计和开发测试账号。

**需求**：

- 平台签发自己的 `user_id`、会话和权限，不把供应商账号对象写入业务表。
- 局域网临时连接可以免登录；远程控制、云端设备管理、通知、模型权益和跨设备共享必须经过我方账号授权。
- 设备级凭据不进入移动端日志和普通业务接口；后端只保存受保护的凭据引用或短期授权材料。
- Token 失效、设备解绑、账号注销和撤权必须使后续远程命令失效。
- 登录、绑定、授权、撤权和远程命令写入我方审计事件。

**当前状态**：orca-platform 只有基线迁移和 501 预留接口，正式账号业务尚未交付；该项仍是一期后半段和二期远程的前置。

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
#### 3.5.15 P1-VENDOR-01：供应商协议、固件和设备能力冻结

**目标**：在真实设备开发前解决供应商答复中的当前协议/规划协议差异。

**需求**：供应商必须提供指定测试固件、硬件版本、协议版本、接口文档、端口、设备能力、错误码、命令状态、文件限制、证书和配对流程；明确当前 WebSocket+HTTP 与规划 MQTT Broker+文件服务器分别对应的固件版本和交付时间。未冻结前，MQTT 不能计为已实现。

**验收**：双方签署型号/固件/协议矩阵，保存协议样例和版本兼容承诺，完成设备身份、状态、控制和文件的最小联调。

#### 3.5.16 P1-CONN-02：设备级配对、证书和授权撤销

**目标**：在不依赖供应商账号的情况下建立设备级安全连接。

**需求**：实现 6 位随机口令确认、设备 CA/证书校验、设备授权列表、CRL/撤销结果、重新配对、凭据保护和审计；明确设备恢复出厂后的授权状态和 `device_id` 行为。

#### 3.5.17 P1-FILE-01：局域网文件服务器

**目标**：完成 G-code 上传、完整性校验、断点续传、进度和自动打印启动。

**需求**：适配供应商文件服务器；支持 SHA-256，兼容供应商实际支持的 SHA/MD5；记录文件大小、摘要、偏移、设备端校验和最终打印任务 ID；上传失败不得触发重复打印。

#### 3.5.18 P1-QA-02：供应商设备现场验收

**目标**：用指定型号和固件替代 Mock/Fake 证据，完成一期闭环。

**必须覆盖**：mDNS 和手动 IP、PIN 配对、证书撤销、状态推送、设备重启、WebSocket/MQTT 中断、文件续传、开始/暂停/恢复/停止、重复命令、App 后台恢复、摄像头故障不影响控制。


#### 3.5.19 P1-WEB-01：用户前台基础工程

**目标**：为后端业务提供独立、可发布的用户前台，不把用户页面和管理页面混在同一前端工程。

**需求**：在 `orca-frontend/apps/web` 建立独立用户前台应用和编译目标；独立用户登录、设备列表、打印任务、错误/空态和通知入口；只消费用户 API/OpenAPI 与事件契约；独立构建配置、环境配置、版本和回滚；不能直接访问数据库、管理 API 或管理员权限。

**一期验收**：前台可以启动、登录、读取设备/任务契约、显示最小设备和任务页面，后端不可用时有明确错误状态；契约测试不依赖管理后台。

#### 3.5.20 P1-ADMIN-01：独立管理后台基础工程

**目标**：为运营、客服和设备支持提供独立管理页面，与用户前台解耦。

**需求**：在 `orca-frontend/apps/admin` 建立独立管理后台应用和编译目标；独立管理员登录、RBAC、菜单、设备/任务/审计/诊断入口；只消费 `admin-api` 和管理事件；独立构建配置、环境、审计、版本和回滚；用户前台不能复用管理员会话或管理路由。

**一期验收**：管理员可以登录并按角色访问最小设备、任务和审计页面；后端拒绝越权请求；管理前台与用户前台可以独立构建和发布。

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

二期目标是把一期设备闭环升级为我方账号驱动、供应商设备可远程接入的产品平台。账号和权限由我方实现；供应商提供设备级通信和远程设备能力。

| 编号 | 模块 | 主要需求 | 产物/责任边界 |
|---|---|---|---|
| P2-SDK-01 | 协议连接 | 当前 WebSocket+HTTP、规划 MQTT Broker+文件服务器适配；协议版本和能力兼容 | SDK 适配器，供应商提供协议 |
| P2-CONN-02 | 远程连接 | 设备注册、会话租约、MQTT+TLS Broker、命令回执、状态对账和 Relay 回退 | 我方实时网关 + 供应商 Broker/设备端 |
| P2-CONN-03 | 设备侧穿透 | Connector/固件主动 TLS 出站，适配 NAT/CGNAT/企业防火墙；受限网络回退 Broker/Relay | 供应商设备侧 + 双方联调 |
| P2-MOB-03 | 远程监控 | 云端设备列表、在线状态、远程状态、异常通知和路径质量 | 移动端 + 我方实时 API |
| P2-DEV-04 | 设备管理 | 单型号设备注册、能力/固件信息、解绑、授权范围和设备诊断 | 我方设备域；供应商提供字段 |
| P2-FILE-04 | 云端文件 | 模型/G-code 对象存储、签名地址、云上传、设备下载、校验和断点续传 | 我方对象存储 + 供应商设备下载 |
| P2-MEDIA-05 | 视频与快照 | Kinesis Video Streams 或等价方案，独立认证、快照/视频、断流恢复 | 供应商视频服务 + 我方展示 |
| P2-OBS-06 | 远程可观测性 | command_id、session_id、设备事件、连接路径、错误码、审计和脱敏诊断 | 我方审计/监控 + 供应商设备日志 |
| P2-VENDOR-07 | 供应商云接入 | Server-to-Server API、Broker 接入、Webhook/事件和版本维护 | 供应商必须提供正式接入资料 |
| P2-AI-05 | AI Gateway | 文本/图片任务、取消、重试、产物和配额 | 我方 AI Gateway |
| P2-SLICE-06 | 云端切片 | 模型、打印机/材料配置、切片任务、G-code 和打印前校验 | 我方 Worker；设备能力作为输入 |
| P2-MODEL-07 | 模型库 | 模型版本、详情、搜索、3D 预览、下载和打印转化 | 我方 Model Catalog |
| P2-BILL-08 | 支付权益 | 订阅、AI 额度、模型购买、退款和权益恢复 | 我方 Billing/Entitlement |
| P2-COMM-09 | 基础社区 | 作者、评论、点赞、收藏、关注和举报 | 我方 Community/Moderation |
| P2-ADMIN-10 | 运营后台 | 内容审核、设备支持、任务重试、远程问题诊断 | 我方 Admin |
| P2-NOTIFY-11 | 通知系统 | 打印完成、异常、离线、温度和任务通知 | 我方 Push/Inbox |

二期远程控制必须满足：移动端只调用我方后端；我方后端完成用户授权和审计；设备通过主动 TLS 出站连接到 Broker/Relay；远程命令、状态、文件和视频通道分离；路径切换或超时后先对账，不自动重放危险命令。

二期启动前的硬门槛是：供应商明确 Broker 归属和维护方，提供设备侧 Connector/固件方案、证书生命周期、MQTT Topic/QoS/心跳/重连/命令回执、远程文件下载和状态事件文档。

### 3.8 三期需求框架

三期在二期远程链路稳定后，继续做网络增强、专业编排和生态能力。经典直接 P2P 不是当前供应商已承诺能力，只有在实际网络数据证明 Broker/Relay 不满足时才进入实施。

| 编号 | 模块 | 主要需求 |
|---|---|---|
| P3-CONN-03 | 直接 P2P/ICE/TURN | 设备和客户端的 ICE 协商、STUN/TURN、对称 NAT 处理、P2P 失败回 Relay；供应商未确认前保持可选 |
| P3-CONN-04 | 连接路径优化 | LAN、Broker、Relay、P2P 的健康度、粘性、成本、带宽和状态同步策略 |
| P3-EDGE-05 | Edge/离线 | 设备侧 Connector 或家庭边缘网关的离线控制、缓存、断点和故障恢复 |
| P3-PRINT-04 | 打印编排 | 多打印机队列、批量任务、失败恢复和任务调度；当前单型号也可先做多设备实例 |
| P3-SDK-01 | 本地切片 SDK | C ABI、Android AAR/JNI、iOS XCFramework、内存预算和取消 |
| P3-SDK-02 | 高级预览 | G-code 分层、路径动画、多材料和设备差异预览 |
| P3-MODEL-05 | 模型生态 | Remix、版本分支、授权、合集、打印成果和模型质量报告 |
| P3-MARKET-06 | 创作者商业化 | 付费模型、分成、结算、优惠和版权处理 |
| P3-COMM-07 | 社区增长 | 推荐、专题、活动、排行榜、创作者主页和消息 |
| P3-TEAM-08 | 团队能力 | 组织、角色、审批、共享打印机、配额和审计 |
| P3-DATA-09 | 数据平台 | 行为分析、打印成功率、模型质量和运营指标 |
| P3-EXT-10 | 扩展生态 | 协议插件、设备能力插件、材料和配置扩展 |

三期验收必须保留二期的安全语义：设备证书和用户授权分离，远程命令有过期时间和幂等键，路径切换后先状态同步，视频和文件异常不能阻断停止/暂停/恢复。

## 风险

| 编号 | 风险/限制 | 影响与处理要求 |
|---|---|---|
| R-01 | 当前 LAN 协议和规划 LAN 协议不同 | 供应商需冻结测试固件、协议版本和迁移时间；一期分别验收 WebSocket+HTTP 与 MQTT/文件服务器 |
| R-02 | 供应商远程方案尚不成熟 | 二期先做可验证的 MQTT+TLS 出站 MVP，不把“规划中”写成已交付；Broker 归属和 API 是开工门槛 |
| R-03 | 设备主动 TLS 出站不等于直接 P2P | 二期把 NAT/CGNAT 穿透定义为 Connector + Broker/Relay；三期再评估 STUN/ICE/TURN |
| R-04 | 设备侧 MQTT Broker 与云端 MQTT Broker 的关系不清 | 要求供应商说明 LAN/远程两个 Broker 的边界、Topic、认证、QoS、心跳和转发路径 |
| R-05 | 设备证书、6 位 PIN、CRL 生命周期未现场验证 | 在一期加入配对、撤销、重新配对、恢复出厂和证书失效测试 |
| R-06 | 当前 SDK 使用 Moonraker Fake transport | 不能据此宣称 WonderMaker 已接入；必须实现供应商协议适配并完成真机验收 |
| R-07 | Android FFI 已验证但 iOS/macOS 未完成 | 一期移动发布范围先限定 Android；iOS bridge/XCFramework 单独设门禁 |
| R-08 | 后端仍是基线和 501 预留接口 | 账号、设备注册、远程命令和审计需独立任务；远程二期不得直接依赖未实现接口 |
| R-09 | 文件和视频由供应商规划服务承载 | 二期分别验收云文件校验/续传和 Kinesis 视频权限/断流，不把页面入口当成能力完成 |
| R-10 | 竞品运行能力证据受游客和设备条件限制 | 竞品只用于产品方向和交互参考，真实控制、视频、云切片和支付仍需自有验收 |
| R-11 | 用户前台和管理后台混用会扩大权限和发布风险 | 同一仓库内两个应用独立编译、独立会话/路由/CI；共享只限无应用依赖的版本化契约和基础 UI 包 |

## 建议

### 5.1 模块依赖和并行开发

1. 先由我方和供应商共同冻结单型号测试固件、当前 LAN 协议、规划 MQTT 协议和文件服务器接口，形成 P1-VENDOR-01。
2. 保持 MobileSDK 公共 DTO 中立；把 WebSocket+HTTP、设备 MQTT Broker、远程 MQTT/TLS 和文件/视频服务分别放入适配器。用户前台与管理后台放在 `orca-frontend` 同一仓库的 `apps/web`、`apps/admin`，分开鉴权、编译和发布，共享 OpenAPI/事件契约。
3. 先完成 P1-CONN-02、P1-SDK-03、P1-FILE-01，再做移动端真实发现、配对、状态和控制；Mock/Fake 只能作为并行开发夹具。
4. Android FFI 可沿用已有验证结果继续集成；iOS bridge 不能阻塞 Android 一期，但必须作为移动发布门禁单列。
5. 二期先确定 Broker 的建设和运维归属，再实现设备侧 Connector、我方实时网关和远程文件/视频；不要让移动端直接连接供应商云或持有长期密钥。
6. 三期根据真实网络数据决定是否引入 STUN/ICE/TURN 直连和 Edge 离线，不提前把经典 P2P 写成供应商已经承诺的能力。

### 5.2 一期交付清单

一期完成需要同时满足：

- 公共 SDK、C ABI 和 Android FFI 版本可读、可构建并通过现有自动化门禁；
- 指定型号/固件的协议、端口、状态、命令、错误码和文件接口已冻结；
- mDNS、手动 IP、6 位 PIN 配对、设备证书、撤销和重新配对在真机上通过；
- 移动端读取真实状态、温度、进度、文件和错误；
- G-code 上传有校验、进度、断点续传和自动打印前置校验；
- 开始、暂停、恢复、停止有设备回执、命令 ID 和最终状态；
- 断网、WebSocket/MQTT 中断、设备重启、App 后台恢复和重复命令有明确结果；
- 摄像头故障不影响打印控制；
- 我方账号/设备绑定接口完成到足以支撑二期，但 LAN 临时使用不依赖登录；
- 至少一台供应商真实设备和一套协议模拟器形成可重复测试报告。

### 5.3 二期/三期排期门槛

- **二期开工门槛**：供应商给出远程通信初版，并明确 Broker 归属、设备 Connector 交付、证书发放/撤销、MQTT Topic/QoS/回执、云文件下载和视频授权；我方账号、设备绑定和审计接口可运行。
- **二期完成门槛**：设备位于普通 NAT、CGNAT 和受限出站网络时可以通过 TLS 出站和 Broker/Relay 建立远程会话；远程控制、状态、文件、通知和视频分别可验收；命令超时不重复执行。
- **三期开工门槛**：二期远程链路稳定，有真实网络数据证明直接 P2P 或 Edge 能降低延迟、带宽或离线风险；供应商确认 STUN/ICE/TURN 或等价实现责任。

前端页面和竞品入口不能替代这些后端、设备和网络验收条件。

## 证据

### 6.1 依据与材料索引

| 材料 | 用途 |
|---|---|
| 供应商邮件附件 `HonorPrint_20261019_101659.pdf` | 单型号设备、当前/规划协议、远程 MQTT+TLS、文件、视频和设备侧出站连接答复 |
| [供应商回复证据摘要](../../../../../artifacts/task-20261016013901509-0f210b/evidence/requirements-revision-20261019/vendor-reply-summary.md) | 把外部邮件事实与未冻结项分开记录 |
| [竞品分析报告](../../../../../artifacts/task-20261016013901509-0f210b/reports/competitor-analysis-report.md) | Bambu Handy、Creality Cloud、Snapmaker 实机游客路径和产品规划启示 |
| [实现状态快照](../../../../../artifacts/task-20261016013901509-0f210b/evidence/requirements-revision-20261019/implementation-status.md) | OrcaSlicer、orca-mobile、orca-platform 的实际完成边界 |
| [本地仓结构对照](../../../../../artifacts/task-20261016013901509-0f210b/evidence/requirements-revision-20261019/local-repo-structure.md) | 以当前本地仓库目录核对移动端和后端工程拓扑 |
| `Docs/mobile-sdk/API.md` | MobileSDK 公共契约、连接路径和命令信封 |
| `Docs/mobile-sdk/PROJECT-TOPOLOGY.md` | 桌面端、移动端、前端 monorepo 中用户前台/管理后台的编译边界、平台后端和 Connector/Relay 的代码边界 |
| [Android FFI 验收](../../../../../artifacts/p1-mob-02-android-ffi/reports/acceptance-report.md) | Android ABI、APK/AAB、arm64 和生命周期证据 |
| [Android 集成独立审查](../../../../../artifacts/p1-qa-01-android-sdk-integration/reports/independent-review.md) | fixture-only 集成证据与真实 FFI/设备边界 |
| [Moonraker transport 验收](../../../../../artifacts/p1-sdk-04-moonraker-transport/reports/acceptance-report.md) | concrete transport 依赖门禁和未完成的 loopback/TLS 范围 |
| [任务进度快照](../../../../../artifacts/task-20261016013901509-0f210b/evidence/report-progress-20261017/progress-snapshot.json) | 一期 13 项需求及关联任务状态 |

### 6.2 验证范围

本轮完成了供应商 PDF 文本提取和事实分级、竞品报告核对、多仓库 Git/任务状态核对、需求重排和 Markdown/HTML 结构生成。没有把供应商的规划答复、竞品页面入口或 Mock/Fake 测试当作真实设备验收。

代码证据截至 2026-10-19：OrcaSlicer SDK/C ABI 基线和 Android FFI 有自动化证据；WonderMaker 真实协议、真实设备、正式远程 Broker、云文件、Kinesis 视频、后端账号/设备业务和 iOS/macOS 仍未验收。

### 6.3 页面复核记录

本轮 HTML 由 v1.7 Markdown 生成，检查目录锚点、表格配对、代码块、中文 UTF-8、本地证据链接和窄屏布局；页面可读性检查不代表任何打印、远程、支付或视频功能通过。

## 未完成项

### 7.1 供应商必须继续确认

1. 单型号硬件版本、当前固件版本、测试固件和协议版本对应关系。
2. 当前 WebSocket+HTTP 和规划设备 MQTT Broker+文件服务器的迁移版本、端口、Topic、QoS、心跳、重连、回执和错误码。
3. 设备侧 MQTT Broker 与云端 MQTT Broker 的架构关系，以及 LAN/远程各自的认证和权限边界。
4. MQTT+TLS Broker 的归属、部署、维护、区域、限流、SLA 和版本变更通知。
5. 设备 Connector 的固件/软件交付方式、资源占用、升级/回滚和断网恢复。
6. NAT/CGNAT/企业防火墙下的 TLS 出站端口和失败回退；是否需要 TURN 或其他 Relay。
7. 远程 Server-to-Server API、事件/Webhook、设备命令状态查询和状态续传。
8. 云文件下载、断点续传、摘要校验、设备下载失败和自动打印关联。
9. Kinesis Video Streams 的鉴权、快照/视频接口、区域和断流恢复。
10. 并发控制、远程运维、安全问题响应和协议兼容承诺。

### 7.2 我方尚待实施或验证

- WonderMaker 当前 WebSocket+HTTP 真实 transport 和规划 MQTT/文件服务器适配器；
- mDNS、手动 IP、PIN、设备证书、CRL、重新配对和真实状态/控制回执；
- 一期 G-code 文件上传、断点续传、自动打印前置校验和现场打印；
- 自建账号、设备注册、能力查询、权限、解绑和远程命令 API；
- 前端 monorepo `orca-frontend` 中 `apps/web`、`apps/admin` 的独立页面、编译目标、鉴权、OpenAPI 客户端和管理审计；
- 二期设备侧出站 Connector、MQTT/TLS Broker/Relay、远程状态/控制、云文件和视频；
- 真实 NAT/CGNAT/受限网络穿透和路径切换；
- iOS/macOS bridge、XCFramework、Apple 真机和正式签名；
- 三期直接 P2P/ICE/TURN、Edge 离线、多设备编排、本地切片和生态能力；
- 竞品登录后设备页、真实绑定、状态、视频、云切片、支付和打印现场验证。

在供应商协议、测试固件和 Broker 责任边界冻结前，不应把 WonderMaker 专有字段写入公共 SDK，也不应把当前 Moonraker Fake transport 通过写成真实设备交付。
