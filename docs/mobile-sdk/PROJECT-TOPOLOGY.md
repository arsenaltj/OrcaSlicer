# 移动端、桌面端与后端工程规划

本文定义 OrcaSlicer 移动端平台的代码仓库边界、共享核心、接口契约和发布关系。

## 1. 总体决策

产品由四类工程组成：

1. **OrcaSlicer Desktop**：桌面建模、完整切片、桌面工作台和桌面打印连接。
2. **Mobile App**：Android/iOS 移动应用，负责设备管理、打印控制、模型社区、账号、支付和消息。
3. **Platform Backend**：账号、设备、打印任务、模型库、社区、AI、切片、支付和通知的服务端。
4. **Infrastructure**：部署、数据库、对象存储、CDN、队列、搜索、监控和密钥配置。

推荐的依赖方向如下：

```text
                         ┌──────────────────────────┐
                         │     Mobile App            │
                         │ Flutter + Kotlin/Swift   │
                         └────────────┬─────────────┘
                                      │ HTTPS / WebSocket
                                      │ OpenAPI / Events
┌──────────────────────┐              ▼
│  OrcaSlicer Desktop  │──────► Platform Backend
│  CMake + wxWidgets   │       ├── Identity / Account
│  libslic3r + GUI     │       ├── Device / Print Job
└──────────┬───────────┘       ├── Model / Community
           │                   ├── Billing / Entitlement
           │                   ├── AI / Slicing Jobs
           │                   └── Realtime / Relay
           │
           ▼
   OrcaSlicer::MobileSDK
   中立 DTO、命令契约、状态机

Platform Backend ───────► Object Storage / CDN / Search / Queue
        │
        ├───────────────► AI Provider Gateway
        ├───────────────► Cloud Slicing Workers
        └───────────────► Printer Edge Connector / Relay
```

依赖必须保持单向：

- `MobileSDK` 不依赖桌面 GUI、Python、供应商 DLL 或商业服务代码。
- 移动端只依赖移动端 API 和 SDK 产物，不复制桌面实现。
- 后端不依赖桌面 UI；需要切片时调用独立的切片 Worker 或切片 SDK。
- 支付、模型访问权限和账号状态以后端为准，移动端不能直接授予权益。
- 打印机厂商协议在连接适配器、云端适配器或设备侧 Connector 中实现，不进入公共 DTO。

## 2. 推荐仓库布局

第一阶段保留三个主要代码仓库和一个部署仓库：

```text
repos/
├── OrcaSlicer/                  # 现有桌面端和共享 C++ 核心
├── orca-mobile/                 # Flutter 移动端和 Android/iOS 宿主
├── orca-frontend/               # 用户前台与管理后台 monorepo，按应用编译隔离
├── orca-platform/               # 账号、社区、支付、设备和任务后端
└── orca-infra/                  # 环境、部署、数据库迁移和运行配置
```

不要把移动端、后端和基础设施代码直接放进 OrcaSlicer 桌面仓库。桌面仓库可以继续维护共享 SDK 源码和切片适配，但产品服务的发布、权限和部署生命周期不同。

当 `OrcaSlicer::MobileSDK` 的 ABI 和版本策略稳定后，再将它拆成独立的 `orca-mobile-sdk` 仓库或发布包。拆分前，OrcaSlicer 是 SDK 的源代码归属仓库；拆分后，桌面端和移动端都从同一个 SDK 版本消费产物。

## 3. OrcaSlicer Desktop 仓库

现有桌面仓库建议保持下面的职责：

```text
OrcaSlicer/
├── src/orca_sdk/
│   ├── include/orca/sdk/          # 公共中立头文件
│   ├── Connectivity.*             # 路径和连接状态机
│   ├── CommandEnvelope.*          # 控制命令信封
│   ├── ModelGeneration.*          # AI 任务和模型产物 DTO
│   ├── Slicing.*                  # 本地/远程切片契约
│   └── CMakeLists.txt
├── src/slic3r/
│   ├── GUI/                       # 桌面 UI，只能在桌面适配层使用
│   ├── Utils/                     # 桌面连接和打印机适配器
│   └── ...
├── tests/mobile_sdk/              # 与桌面 UI 无关的 SDK 契约测试
├── tests/slic3rutils/             # 桌面连接和工具测试
├── Docs/mobile-sdk/
└── scripts/verify_mobile_sdk_boundary.py
```

桌面端适配方向：

```text
桌面 GUI / 工作台
        ↓ 转换
MobileSDK DTO 和命令
        ↓
桌面连接适配器、libslic3r、本地文件和桌面服务
```

桌面 UI 可以使用 `Model`、`PrintConfig`、`wxString` 和工作区对象，但这些类型必须在适配层转换为 `MobileSDK` 或后端 API 所使用的 DTO。公共 SDK 头文件不能反向包含桌面类型。

桌面端还可以保留更完整的能力：

- 本地完整切片；
- 多盘和多材料工作台；
- 高级支撑、参数和预览；
- 桌面插件；
- 本地模型目录；
- 厂商桌面扩展。

这些能力不必全部承诺给移动端。移动端应该通过 `SlicingRequest` 选择本地切片或远程切片，并根据设备、内存和模型大小决定执行路径。

## 4. Mobile App 仓库

本节以工作区中 `orca-mobile` 仓库的当前代码为准。Flutter 应用、FFI bridge、平台服务和测试都在同一个移动端仓库中，当前没有 `apps/mobile/` 或顶层 `native/` 目录。

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
├── integration_test/             # App smoke 和 Android SDK FFI 集成测试
├── test/                         # Flutter、Widget、FFI、生命周期和 Mock 测试
├── tool/                         # 环境校验、配置校验、Android SDK 构建工具
├── config/                       # development/test/production 配置样例
├── docs/                         # ownership、handoff、模块和参考分析文档
├── pubspec.yaml                  # 根应用和三个本地 package 依赖
├── sdk.lock.json                 # MobileSDK 产物和 ABI 锁定信息
└── BUILDING.md / DECISIONS.md
```

当前代码依赖方向：

```text
lib/features + lib/core
          ↓
packages/printer_client   packages/orca_sdk_bridge   packages/platform_services
          ↓                         ↓                         ↓
     Mock/Unavailable       C ABI/FFI bridge          Android/iOS host
```

真实打印机协议、WonderMaker 适配器和后端 API Client 尚未进入当前基线；后续应在既有 `packages/` 边界内新增，并先冻结公共契约。

### 4.1 原生平台层边界

Android 和 iOS 各自维护：

- 局域网设备发现；
- 本地网络权限；
- 文件选择和系统分享；
- 后台任务和前台服务；
- 推送通知；
- 摄像头和视频播放；
- 安全存储；
- Apple/Google 应用内购买桥接；
- 设备厂商原生 SDK。

Flutter 只依赖统一的 Dart 接口，例如 `PrinterRepository`、`BillingRepository` 和 `ModelRepository`。平台差异在插件或桥接层消化。

### 4.2 3D 预览边界

大模型解析、网格简化、G-code 分层和路径动画不要全部放进 Dart。建议使用独立的渲染模块：

```text
Flutter ModelViewer Widget
        ↓
Platform View / FFI
        ↓
Native GPU Renderer 或 C++ Parser
```

模型详情页优先加载缩略图、低面数预览和分层摘要；开始切片或打印时再获取完整模型。

## 5. Platform Backend 仓库

本节以工作区中 `orca-platform` 仓库的当前代码为准。当前仓库是 NestJS 模块化单体基线，不是已经拆分好的多应用后端。

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
├── test/                         # unit、contract、integration、fixtures
├── docs/                         # ownership、modules-roadmap、handoff
├── package.json / tsconfig.json / nest-cli.json
└── BUILDING.md / README.md
```

当前可确认的后端能力包括健康检查、配置校验、请求上下文、错误格式、日志、PostgreSQL 迁移/种子、内部审计、OpenAPI 契约和开发部署基线。identity/devices 仍返回 501，正式账号、设备注册、能力查询、实时网关、远程命令、Worker 和管理 API 尚未形成独立目录或业务实现。

第一阶段继续保持模块化单体。后续只有在接口、负载和部署边界稳定后，才考虑把 realtime、worker 或 admin API 拆成独立进程或应用；这些属于演进目标，不作为当前仓库结构描述。

### 5.1 账号与设备

账号域至少包含：

- 用户；
- 登录凭据和第三方身份；
- 家庭或团队；
- 成员角色；
- 设备绑定；
- 控制权限；
- 会话和刷新 Token；
- 账号注销和数据导出。

设备域至少包含：

- `device_id`；
- 设备型号和固件；
- 能力矩阵；
- 绑定关系；
- 最后在线时间；
- 当前连接路径；
- 当前打印任务；
- 设备证书或 Connector 身份。

### 5.2 模型库和社区

模型文件与业务元数据分开：

```text
关系数据库：Model、ModelVersion、Author、Tag、Price、License
对象存储：STL、OBJ、3MF、GLB、G-code、缩略图和视频
搜索系统：全文、标签、作者、分类和排序
消息队列：解析、缩略图、审核、索引和通知
```

模型上传流水线：

```text
上传
  ↓
格式、大小、哈希和安全检查
  ↓
3D 解析、缩略图、预览和元数据提取
  ↓
内容与版权审核
  ↓
搜索索引
  ↓
发布或进入人工审核
```

每个打印任务必须固定引用一个模型版本、文件哈希、打印机配置、材料配置和切片引擎版本。不能让历史打印任务指向会被覆盖的“最新模型”。

### 5.3 支付和权益

支付模块必须把第三方交易和内部权益分开：

```text
StoreKit / Google Play Billing / 实体支付
        ↓
PaymentTransaction
        ↓ 服务端验证和对账
Order + LedgerEntry
        ↓
Entitlement
        ├── Subscription
        ├── ModelOwnership
        ├── AiCredits
        └── FeatureUnlock
```

移动端只负责发起购买和展示结果。后端负责：

- 商品目录；
- 交易凭证验证；
- 订阅续期和取消；
- 退款和撤销；
- 消耗型额度账本；
- 模型购买权益；
- 创作者分成和结算；
- 重复回调和幂等处理。

模型购买、AI 额度和会员权限都应通过 `Entitlement` 判断，业务模块不能各自实现一套会员判断。

### 5.4 AI、切片和打印任务

AI、切片和打印都采用任务化接口：

```text
request_id       客户端请求幂等键
job_id           服务端任务 ID
state            状态机
progress         进度
artifact_id      结果产物
artifact_sha256  内容校验
error_code       稳定错误码
retryable        是否允许重试
```

推荐的任务链路：

```text
ModelVersion
    ↓
SlicingJob
    ↓
GcodeArtifact
    ↓
PrintJob
    ↓
Device Command
```

模型生成和云端切片只生成产物，不直接控制打印机。打印控制必须经过独立的 `PrintJob` 和设备权限检查。

### 5.5 用户前台与管理后台

用户前台和管理后台放在同一个 `orca-frontend` 前端 monorepo 中，通过应用目录和编译目标隔离，均通过版本化契约访问 `orca-platform`：

```text
orca-frontend/apps/web    ── 用户 API / Events ──┐
                                                  ├── orca-platform
orca-frontend/apps/admin  ── admin-api / 管理事件 ┘
```

- `orca-frontend/apps/web` 负责用户账号、设备、打印任务、模型、切片、通知和远程控制入口。
- `orca-frontend/apps/admin` 负责管理员登录、RBAC、用户/设备支持、打印任务重试、内容审核、审计和诊断。
- 两个应用共享仓库但独立入口、依赖边界、会话、路由、构建配置、CI/CD、环境、产物和发布回滚；共享只限版本化 OpenAPI、事件 Schema、设计 token 和不依赖应用的基础 UI 包。
- `apps/web` 的编译不能拉入 `apps/admin` 的页面和管理员依赖；`apps/admin` 的编译也不能依赖用户前台业务模块。共享包不能反向依赖任一应用。
- `orca-platform` 的 `api` 面向用户前台，`admin-api` 面向管理后台；后端服务端必须再次校验用户权限和管理员 RBAC。
- 一期在同一仓库交付两个可独立编译的前台/管理后台骨架和契约测试，二期交付业务页面，三期交付团队、内容运营和数据分析页面。

## 6. API 与事件契约

对外接口建议分为四类：

| 接口 | 用途 | 传输方式 |
|---|---|---|
| Query API | 账号、模型、社区、订单和设备查询 | HTTPS/JSON |
| Command API | 创建任务、提交打印、暂停和恢复 | HTTPS/JSON |
| Realtime API | 打印状态、任务进度、消息 | WebSocket 或 SSE |
| Artifact API | 上传、下载、断点续传 | 签名 URL/对象存储 |

内部事件建议使用版本化事件：

```text
model.version.published.v1
model.artifact.processed.v1
billing.entitlement.granted.v1
billing.entitlement.revoked.v1
slicing.job.completed.v1
print.job.state_changed.v1
printer.status.updated.v1
moderation.review_required.v1
```

`OrcaSlicer::MobileSDK` 的 `CommandEnvelope` 是设备控制命令的基础格式；云端命令、移动端本地命令和 Edge Connector 命令都应保留 `session_id`、`command_id`、`sequence`、过期时间和设备回执。

OpenAPI、事件 Schema 和 MobileSDK DTO 要分别版本化：

- `orca.command/v1`：设备控制命令；
- `orca.model-job/v1`：模型生成任务；
- `orca.slicing/v1`：切片请求和产物；
- `api/v1`：后端 HTTP API；
- `events/v1`：内部事件。

不要让移动端、桌面端和后端共享数据库实体类。共享的是稳定的协议和数据契约，而不是数据库 ORM 模型。

## 7. 构建和发布产物

### Desktop

```text
OrcaSlicer Desktop
├── OrcaSlicer executable
├── libslic3r / libslic3r_gui
└── MobileSDK contract target
```

### SDK

```text
orca_mobile_sdk static/shared library
├── Windows/Linux/macOS desktop artifact
├── iOS XCFramework
└── Android AAR/JNI
```

iOS 和 Android 产物应由 SDK 发布流水线生成，移动端仓库通过版本号引用，不直接复制 `src/orca_sdk` 源码。

### Mobile

```text
Android App Bundle
iOS Archive / IPA
```

移动端构建需要绑定：

- MobileSDK 版本；
- API 环境；
- 功能开关；
- 支付商品配置；
- 推送环境；
- 崩溃和日志环境。

### Backend

```text
api image
realtime-gateway image
worker image
admin image
migration image
```

每个后端部署都必须记录 API 版本、事件版本、数据库迁移版本和 MobileSDK 兼容范围。

## 8. CI 检查

每个仓库分别负责自己的质量门禁：

### OrcaSlicer

- C++ 编译；
- MobileSDK 边界检查；
- MobileSDK Catch2 契约测试；
- 桌面适配器测试；
- 切片 golden project；
- `git diff --check`。

### Mobile App

- Dart 分析和单元测试；
- Android/iOS 编译；
- FFI 和平台通道测试；
- 购买恢复、退款和权益同步测试；
- 模型下载、断点续传和打印任务测试；
- 真机局域网发现测试。

### Platform Backend

- OpenAPI 兼容性检查；
- 事件 Schema 兼容性检查；
- 数据库迁移测试；
- 支付沙箱验证；
- 权益幂等和退款测试；
- 模型文件处理和审核测试；
- 设备命令重试、过期和权限测试。

### 跨仓库

每次发布前至少验证：

```text
Mobile App API client
        ↔ Platform API
Mobile App Native SDK bridge
        ↔ MobileSDK artifact
Platform Print Job
        ↔ Printer Command Envelope
Slicing Artifact
        ↔ Desktop / Mobile Print Job
Billing Transaction
        ↔ Entitlement
```

## 9. 分阶段实施

### 阶段一：当前任务

保留并完成：

- 中立 DTO；
- 连接路径状态机；
- 命令信封；
- AI 任务和切片任务契约；
- CMake 边界检查；
- 契约测试。

### 阶段二：连接和平台骨架

实现：

- Moonraker、OctoPrint、PrusaLink 适配器；
- 后端设备注册和登录；
- 移动端 API Client；
- Flutter 设备列表；
- Android/iOS 局域网发现；
- G-code 上传和基本控制。

### 阶段三：模型、任务和账号商业化

实现：

- 模型上传和对象存储；
- 预览生成和搜索；
- 账号、家庭和设备共享；
- AI Gateway；
- 云端切片；
- 支付交易验证；
- 会员和模型权益。

### 阶段四：社区和远程链路

实现：

- 评论、点赞、收藏和关注；
- 模型版本与 remix；
- 内容审核；
- LAN/P2P/Relay 路径切换；
- 摄像头独立转发；
- 打印任务恢复和通知。

### 阶段五：本地切片和高级能力

实现：

- `orca-slicing-core`；
- iOS XCFramework；
- Android AAR/JNI；
- 移动端轻量本地切片；
- 大模型自动转云端切片；
- 高级模型预览；
- 创作者结算和团队功能。

## 10. 不应采用的工程边界

以下做法会导致后续难以维护：

- 把移动端 Flutter 页面放进 OrcaSlicer 桌面仓库；
- 把后端 ORM 模型作为移动端共享模型；
- 让移动端直接访问 AI Provider 或支付服务商密钥；
- 让打印机协议适配器直接依赖页面状态；
- 把模型文件存进业务数据库；
- 让支付成功回调直接修改用户会员字段；
- 把本地切片、云端切片和打印控制实现成同一个任务状态机；
- 把摄像头视频连接和打印控制连接绑定在同一个长连接上；
- 在没有 ABI、版本和 golden test 约束前，把 C++ 核心拆成多个移动端私有副本。

当前 `src/orca_sdk` 是这个工程边界的第一块基础。后续协议适配器、移动端绑定、云端 API 和业务平台都应围绕这些稳定契约扩展，而不应把桌面工程整体搬进移动端。
