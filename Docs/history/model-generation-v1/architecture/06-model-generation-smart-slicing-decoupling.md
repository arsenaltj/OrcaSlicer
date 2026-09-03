# 模型生成与智能切片双主线解耦架构

> 状态：提议，供两人并行开发评审。
>
> 日期：2026-08-14。
>
> 范围：模型生成、智能切片；交互重构、账号、计费暂不实施。

## 1. 结论

推荐采用“同一仓库内的模块化单体 + 一个模块化本地 sidecar + Orca 防腐适配层”。

两条业务主线彼此不直接调用：

```text
模型生成域
  └─ 产出不可变 GeneratedModelArtifact
             │
             │ 用户确认/应用层命令
             ▼
智能切片域
  └─ 导入、颜色映射、检查、修复、摆盘、参数建议、切片
             │
             ▼
Orca 防腐适配层
  └─ Model / Plater / PresetBundle / Print / Preview
```

不建议现在拆成多个 Git 仓库或多个服务进程。当前只有两名开发者、共享一个桌面应用和一套安装包；过早拆仓会增加协议发布、兼容矩阵、调试和打包成本，却不能消除 Orca 集成点。先用代码模块、接口和目录所有权实现解耦，未来账号或云端任务系统进入后再评估独立服务。

## 2. 当前代码框架

### 2.1 模型生成现状

| 位置 | 当前职责 | 判断 |
|---|---|---|
| `ModelGenerationPanel.cpp/.hpp` | 页面、输入、图片预处理、轮询、任务恢复、预览、模型库、OBJ 下载、颜色映射、修复、摆盘、切片触发 | 约 2,612 行，是当前最大耦合点 |
| `AIModelGenerationClient.cpp/.hpp` | sidecar HTTP、任务 DTO、下载、取消 | 已有传输边界，但仍属于 GUI 具体实现 |
| `AIServiceManager.cpp/.hpp` | sidecar 健康、协议和能力发现 | 可作为平台级服务保留 |
| `orca_ai_sidecar.py` | HTTP、任务持久化、图片处理、供应商调用、OBJ/MTL/纹理、修复、参数建议 | 约 2,496 行，是 Python 侧单体 |
| `openai_preprocessor.py` | 文字/图片预处理供应商调用 | 可转成 provider adapter |
| `tripo_client.py` | Tripo 提交、查询、下载 | 已接近 provider adapter |
| `generated_models/` | 任务、预览、OBJ 与模型库 | 已形成文件制品存储，但契约尚未独立定义 |

模型生成主链已具备真实 AI、任务持久化、重启恢复、预览、OBJ 顶点色和模型库。主要欠缺不是能力，而是职责边界。

### 2.2 智能切片现状

| 位置 | 当前职责 | 判断 |
|---|---|---|
| `ModelGenerationPanel::import_local_artifact()` | 导入、颜色策略、网格检查/CGAL 修复、摆盘、自动切片 | 应整体迁出模型生成 UI |
| `Plater.cpp/.hpp` AI 增量 | 六步流程 UI、内部取消、稳定计时器、最终可打印门禁 | AI 语义侵入上游高频文件，升级风险高 |
| `AIAssistantConfig.cpp/.hpp` | 当前配置上下文、参数白名单、类型/范围校验 | 是参数建议能力的良好起点 |
| `AIAssistantPanel.cpp/.hpp` | 请求建议、人工勾选、写 Tab、重切片 | 直接依赖 GUI/Plater，尚无候选试切闭环 |
| `AISidecarClient.cpp/.hpp` | `/config-proposal` HTTP | 可转成切片建议 gateway |
| `libslic3r/Model.cpp` | OBJ 顶点色导入修复 | 通用兼容补丁，应独立维护 |
| `libslic3r/MeshBoolean.cpp` | CGAL 开边/孔洞修复增强 | 高风险通用算法补丁，不应被 AI 工作流隐式绑定 |

当前已有“白名单参数建议 + 人工应用 + 重切片”和“生成模型导入后自动切片”，但还没有完整的智能切片领域：没有结构化 Preflight、隔离试切、候选指标比较、事务式应用和独立状态机。

### 2.3 当前依赖问题

```text
ModelGenerationPanel
 ├─ wx UI / OpenGL preview
 ├─ AIModelGenerationClient
 ├─ 文件系统模型库
 ├─ libslic3r OBJ / Model / Print / Geometry
 ├─ ObjColorDialog / CGAL GUI repair
 └─ Plater（导入、Undo、摆盘、切片、Preview）

Plater
 └─ 反向感知“AI 自动流程”及其六步状态
```

这导致两名开发者都会修改 `ModelGenerationPanel`、`Plater` 和 sidecar；并行开发会频繁冲突，上游 Orca 合并也会不断冲突。

## 3. 方案比较

| 方案 | 优点 | 代价/风险 | 结论 |
|---|---|---|---|
| A. 继续在 Panel/Plater 上增加功能 | 短期改动少 | 两人持续冲突、无法单测、上游升级成本持续上升 | 不采用 |
| B. 同仓模块化单体 + 防腐层 | 可增量迁移、单安装包、接口清晰、适合两人并行 | 前两轮需要先抽接口和兼容门面 | 推荐 |
| C. 模型生成、切片分别拆仓/拆服务 | 组织隔离最强 | 协议发布、部署、调试、版本治理过重；切片仍必须嵌入 Orca | 暂缓 |

## 4. 目标分层与依赖方向

```text
┌──────────────────── GUI / Presentation ────────────────────┐
│ ModelGenerationPanel/Presenter  SmartSlicingPanel/Presenter │
└──────────────────────────┬───────────────────────────────────┘
                           ▼
┌──────────────────── Application Use Cases ──────────────────┐
│ ModelGenerationCoordinator   SmartSlicingCoordinator         │
└───────────────┬──────────────────────────┬────────────────────┘
                ▼                          ▼
┌──────── Model Generation Domain ┐  ┌── Smart Slicing Domain ─┐
│ Request/Job/Artifact/Policy     │  │ Report/Plan/Candidate   │
│ IModelGenerationGateway        │  │ IWorkspace/ISliceExecutor│
└───────────────┬────────────────┘  └─────────────┬────────────┘
                ▼                                 ▼
┌──────────────────── Infrastructure Adapters ────────────────┐
│ Sidecar HTTP / Artifact Store / Provider adapters            │
│ OrcaWorkspaceAdapter / CGAL / libslic3r / Plater / Preview   │
└──────────────────────────────────────────────────────────────┘
```

依赖规则：

1. 模型生成域不包含 wx、`Plater`、`ModelObject` 或供应商 SDK；
2. 智能切片域不包含 Tripo/OpenAI job，也不负责远程模型生成；
3. 只有 Orca adapter 可以直接包含 `Plater.hpp`、`GUI_App.hpp`、`PresetBundle` 等高耦合头文件；
4. `libslic3r` 不反向依赖 AI 或 GUI；
5. GUI 只渲染状态并发出命令，不持有业务真值；
6. 两域之间只通过不可变制品契约交接，且依赖为单向。

## 5. 建议目录

### 5.1 C++

```text
src/slic3r/AI/
├─ Shared/
│  ├─ Result.hpp
│  ├─ Cancellation.hpp
│  └─ JobEvent.hpp
├─ ModelGeneration/
│  ├─ Domain/GenerationRequest.hpp
│  ├─ Domain/GenerationJob.hpp
│  ├─ Domain/GeneratedModelArtifact.hpp
│  ├─ Application/ModelGenerationCoordinator.*
│  ├─ Application/IModelGenerationGateway.hpp
│  └─ Infrastructure/SidecarModelGenerationGateway.*
└─ SmartSlicing/
   ├─ Domain/PrintabilityReport.hpp
   ├─ Domain/RepairPlan.hpp
   ├─ Domain/SlicingPlan.hpp
   ├─ Domain/SlicingCandidate.hpp
   ├─ Application/SmartSlicingCoordinator.*
   ├─ Application/IOrcaWorkspace.hpp
   ├─ Application/IParameterAdvisor.hpp
   └─ Infrastructure/SidecarParameterAdvisor.*

src/slic3r/GUI/AI/
├─ ModelGeneration/ModelGenerationPanel.*
├─ ModelGeneration/ModelGenerationPresenter.*
├─ SmartSlicing/SmartSlicingPanel.*
├─ SmartSlicing/SmartSlicingPresenter.*
├─ Orca/OrcaWorkspaceAdapter.*
├─ Orca/OrcaWorkflowEventAdapter.*
└─ Integration/AIFeatureModule.*
```

说明：第一轮不要求一次性搬完文件。先新增接口和 coordinator，用兼容 facade 包住现有类，再逐方法迁移；这样每一步都能构建并回归。

### 5.2 Python sidecar

```text
tools/ai/orca_ai/
├─ api/http_server.py
├─ api/v1_compat.py
├─ shared/contracts.py
├─ shared/errors.py
├─ model_generation/application.py
├─ model_generation/jobs.py
├─ model_generation/artifacts.py
├─ smart_slicing/advisor.py
├─ providers/openai_adapter.py
├─ providers/tripo_adapter.py
├─ storage/job_store.py
└─ artifacts/
   ├─ obj_pipeline.py
   ├─ palette.py
   └─ mesh_validation.py

tools/ai/orca_ai_sidecar.py   # 保留为很薄的兼容启动入口
```

仍然只启动一个 loopback 进程。内部模块化，不先引入微服务。

## 6. 两条主线的核心契约

### 6.1 模型生成输出

```cpp
struct GeneratedModelArtifact {
    std::string artifact_id;
    std::filesystem::path obj_path;
    std::string sha256;
    std::string format;              // 当前固定 obj
    std::string color_encoding;      // vertex_rgb / materials / none
    uint64_t vertex_count;
    uint64_t triangle_count;
    uint32_t connected_components;
    BoundingBoxMm bounds;
    std::vector<RgbColor> dominant_colors;
    std::string contract_version;
};
```

要求：

- 文件已完整落盘、哈希和格式已校验后才发布；
- 发布后视为不可变，重新处理产生新 artifact ID；
- 不携带 API key、供应商对象或 wx/Orca 指针；
- 模型生成完成不等于自动切片，只代表可以进入用户确认。

### 6.2 智能切片输入

```cpp
struct SmartSlicingRequest {
    GeneratedModelArtifact artifact;
    ImportColorPolicy color_policy;
    RepairPolicy repair_policy;
    ArrangementPolicy arrangement_policy;
    AutoSlicePolicy auto_slice_policy;
    WorkspaceTarget target;
};
```

### 6.3 Orca 工作区端口

```cpp
class IOrcaWorkspace {
public:
    virtual WorkspaceSnapshot snapshot() = 0;
    virtual ImportResult import_artifact(const GeneratedModelArtifact&, const ImportOptions&) = 0;
    virtual PrintabilityReport inspect(const ImportedObjectRef&) = 0;
    virtual RepairResult repair(const ImportedObjectRef&, const RepairPlan&) = 0;
    virtual ArrangeResult arrange(const ImportedObjectRef&, const ArrangementPolicy&) = 0;
    virtual SliceStartResult start_slice(const SliceOptions&) = 0;
    virtual void rollback(const WorkspaceSnapshot&) = 0;
};
```

具体 `OrcaWorkspaceAdapter` 负责把这些命令翻译成 Orca 的 snapshot、`load_files()`、颜色对话框、mesh 更新、dirty、invalidation、`reslice()` 和 Preview。领域层不直接知道这些细节。

## 7. 状态机与线程

模型生成状态：

```text
Idle → Preprocessing → AwaitingApproval → Submitting → Generating
→ Downloading → Validating → Ready
活动态 → Canceling → Canceled
活动态 → Failed → Retry
```

智能切片状态：

```text
Idle → Importing → ColorMapping → Inspecting → RepairReview
→ Arranging → ParameterProposal → TrialSlicing → ReadyToApply
→ Applying → OfficialSlicing → Completed
```

规则：

- 网络、文件处理、分析和试切不阻塞 GUI 线程；
- Orca Model/Config 的正式修改只在 GUI/Orca 允许的线程执行；
- 每个 paid generation 使用持久化 idempotency key；
- 智能切片的修改位于一个 Orca snapshot/Undo 事务内，失败可回滚；
- 当前合并的“六步状态”只作为 Presenter 投影，不再作为 `Plater` 内业务状态。

## 8. Sidecar HTTP 兼容策略

第一阶段不删除已有接口：

```text
/health
/v1/orcaslicer/model-jobs/*
/v1/orcaslicer/config-proposal
```

它们由 `api/v1_compat.py` 转发到新 application 模块。C++ 新客户端通过健康响应中的 `protocol_version` 和 capabilities 协商。只有在演示包、当前 GUI、契约测试全部迁移后，才考虑增加新命名空间；旧路由至少保留一个发布周期。

契约版本原则：

- 新增可选字段：minor 兼容；
- 删除/改名/语义变化：新 major 路由或版本；
- 未知字段由旧客户端忽略；
- 必填字段缺失必须 fail closed；
- 契约样例和 contract tests 与代码同库维护。

## 9. 两人分工

| 所有者 | 独占目录 | 主要交付 | 默认不修改 |
|---|---|---|---|
| 开发者 A：模型生成 | `AI/ModelGeneration`、`GUI/AI/ModelGeneration`、sidecar `model_generation`/providers/artifacts | 请求/任务/恢复、预览、模型库、Validated Artifact | `Plater`、切片配置、CGAL 正式应用 |
| 开发者 B：智能切片 | `AI/SmartSlicing`、`GUI/AI/SmartSlicing`、`GUI/AI/Orca`、sidecar `smart_slicing` | 导入、颜色策略、Preflight、修复计划、摆盘、参数候选、试切/正式切片 | Tripo/OpenAI 生成任务、模型库 |
| 双方评审 | `AI/Shared`、制品/HTTP 契约、`AIFeatureModule`、CMake | 边界变更、版本升级、集成测试 | 未经另一方评审不得破坏契约 |

协作规则：

1. 先合并接口和测试样例，再分别实现；
2. 每个分支只允许一个人修改 `MainFrame`、`Plater`、CMake 等共享接入文件，由集成负责人集中合入；
3. 共享 DTO 只承载数据，不放业务方法和 GUI 类型；
4. PR 保持小步：抽取、兼容、切换调用、删除旧代码分别提交；
5. 每天从集成分支同步，禁止两条大分支最后一次性合并；
6. 模型生成的完成验收以 artifact contract 为界，智能切片的完成验收以 Orca 正式结果为界。

## 10. 兼容 Orca 上游演进

### 10.1 上游补丁预算

目标是把长期直接修改 Orca 原文件控制在以下范围：

| 文件 | 允许的长期增量 |
|---|---|
| `MainFrame.*` | 创建/销毁 `AIFeatureModule` 和页面导航的薄接入 |
| `GUI_App.*` | feature flag 与服务生命周期装配 |
| `Plater.*` | 最多保留通用切片完成事件/查询接口，不保留 AI 文案和状态机 |
| `src/slic3r/CMakeLists.txt` | 新模块源文件登记 |
| `libslic3r/*` | 仅独立、通用、可测试且适合回馈上游的修复 |

### 10.2 Git 同步策略

仓库已有 `origin` 和 Orca 官方 `upstream`。建议：

```text
upstream/main
   ↓ 定期同步到 upstream-sync 分支并只解决原版冲突
integration/ai-core
   ├─ feature/model-generation
   └─ feature/smart-slicing
```

- 不在功能分支中顺便升级 Orca；
- 上游同步和 AI 功能改动使用不同 PR；
- 每次同步先构建“AI 关闭”基线，再构建“AI 开启”版本；
- 高风险 `libslic3r` 补丁独立维护，记录上游 issue/PR 或补丁来源；
- 不改变现有 `.3mf` 和 profile schema。AI job、远端 ID 和临时报告继续放 job store；只有用户确认后的 mesh 和配置走 Orca 原生持久化。

### 10.3 Feature gate

`enable_ai_features=false` 时：

- 不创建 AI 页面和服务；
- 不请求 sidecar；
- 不改变导入、配置、切片和 Preview 路径；
- 原版项目、profile、CLI 与切片结果保持原行为。

## 11. 测试边界

### 模型生成

- domain/coordinator 单测：状态、取消、恢复、幂等；
- provider contract：超时、断线、错误 schema、重复回调；
- artifact：ZIP 安全、OBJ/MTL/纹理、哈希、顶点色、面数/拓扑；
- GUI：页面切换后状态与预览保持、模型库加载。

### 智能切片

- import policy：手动/自动/单色、取消和降级；
- preflight/repair：before/after、Undo、dirty、重新打开；
- parameter proposal：白名单、类型、范围、scope；
- trial slice：不污染正式 `Print`/配置，候选可比较；
- official slice：成功、内部重启、取消、不可打印和 Preview 门禁。

### 上游兼容

- AI 关闭的 Orca smoke；
- 旧 `.3mf`、profile、OBJ 和普通切片；
- Windows Release 构建为当前交付门禁；macOS/Linux 暂不作为近期演示阻塞项，但代码边界不引入 Windows-only domain 依赖。

## 12. 增量迁移顺序

### 第 0 步：冻结契约（0.5–1 天）

- 建立 `GeneratedModelArtifact`、`SmartSlicingRequest`、typed error；
- 为现有 sidecar v1 路由补契约样例；
- 不改变 UI 行为。

### 第 1 步：拆模型生成（1–2 天，可由 A 独立进行）

- 从 Panel 抽出 generation coordinator、job 状态和模型库 repository；
- `AIModelGenerationClient` 变成 gateway adapter；
- Panel 只保留控件和 Presenter；
- 保持现有真实生成/恢复链路不变。

### 第 2 步：拆智能切片（1–2 天，可由 B 与第 1 步并行）

- 把 `import_local_artifact()` 迁成 `SmartSlicingCoordinator`；
- 建立 `OrcaWorkspaceAdapter`；
- 把颜色、检查、修复、摆盘、自动切片拆成可组合步骤；
- 先保持现有功能等价。

### 第 3 步：清理 Plater 侵入（1 天）

- AI 六步状态移入 Presenter；
- `Plater` 只暴露通用切片事件/结果查询；
- 删除 AI 文案、AI timer 和业务分支。

### 第 4 步：拆 Python sidecar（1–2 天）

- 先机械搬迁并保留兼容入口；
- artifact pipeline 与 provider adapter 分别测试；
- 旧 HTTP 契约测试必须全部通过。

### 第 5 步：真正扩展智能切片

- 增加结构化 Preflight；
- 增加 RepairPlan 与确认/回滚；
- 增加隔离 Trial Slice、指标比较和正式 apply；
- 这一阶段不再需要模型生成开发者修改切片代码。

## 13. 近期完成定义

解耦第一阶段完成需要同时满足：

1. 两名开发者可以分别在独占目录内新增功能；
2. 模型生成完成只发布 `GeneratedModelArtifact`，不直接调用 `Plater`；
3. 智能切片通过 `IOrcaWorkspace` 操作 Orca，不依赖模型供应商；
4. `ModelGenerationPanel` 不再包含颜色/修复/摆盘/切片实现；
5. `Plater` 不再持有 AI 文案和 AI 工作流状态；
6. sidecar 旧接口、正式任务恢复、真实 OBJ 与模型库回归通过；
7. AI 关闭时原版 Orca 行为不变；
8. Windows Release 构建和目标回归通过。
