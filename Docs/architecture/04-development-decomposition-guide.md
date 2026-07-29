# 开发任务拆解指南

本指南用于把目标架构转换成可分派给开发的 Epic、Feature 和 Task。

## 1. 建议工作包

### Epic A：AI 平台基础

#### A1. AI 服务抽象

- 定义 Provider 无关的生成/推理接口；
- 定义任务 ID、进度、取消、错误和产物协议；
- 禁止核心业务依赖具体厂商 SDK。

#### A2. AIServiceManager

- 服务启动、停止、健康、版本和能力发现；
- Provider registry；
- 应用启动与关闭生命周期；
- sidecar 崩溃和重连；
- 三平台打包。

#### A3. AIJobStore

- 任务历史、恢复、缓存、清理和保留策略；
- 不保存凭据；
- 默认不写入项目 3MF。

**验收重点**：离线降级、超时、取消、退出清理、版本不兼容、敏感信息脱敏。

---

### Epic B：AI 工作区与状态机

#### B1. AIWorkspacePanel

- MainFrame 页面或正式 AUI pane；
- Generate / Inspect / Repair / Tune 四类任务；
- 当前对象/plate 范围；
- 进度、取消、重试、历史和结果展示。

#### B2. AIWorkflowCoordinator

建议状态：

```text
Idle
→ Preparing
→ Submitting
→ Running
→ Downloading / Analyzing / TrialSlicing
→ ReadyForReview
→ Applying
→ Completed

任意活动态 → Canceling → Canceled
任意活动态 → Failed → Retry / Dismiss
```

#### B3. AI Jobs

- `process()` 中执行网络、分析和试切；
- `finalize()` 中更新 wx UI 和正式项目；
- 生命周期纳入 ORCA Worker。

**验收重点**：重复点击、页面关闭、项目切换、应用退出、任务取消和重试均不产生悬挂回调。

---

### Epic C：AI 模型生成

#### C1. Generation request model

- 文本、图片、生成模式和质量参数；
- 输入文件尺寸和类型限制；
- 可脱敏的日志字段。

#### C2. Provider adapter

- 提交、查询、取消和下载；
- 统一错误；
- 断线与重试；
- 产物哈希、大小和格式校验。

#### C3. GeneratedModelImporter

- 复用 `Model::read_from_*`；
- 转换为 `ModelObject`；
- 单位、尺寸和 multipart 规范化；
- 通过 `Plater::load_model_objects()` 进入标准流程。

#### C4. 用户交互

- 生成预览；
- 选择结果；
- 导入当前 plate；
- 失败、取消、重新生成。

**验收重点**：导入后可以编辑、Undo、Save 3MF、重新打开、切片和打印。

---

### Epic D：模型检查与修复

#### D1. ModelPreflightService

定义结构化问题：

```text
Issue {
  stable object/volume reference
  category
  severity
  evidence geometry / location
  message key
  suggested actions
}
```

#### D2. 诊断可视化

- ObjectList 问题标记；
- GLCanvas3D 位置高亮；
- 问题筛选和定位；
- 不把诊断结果写进 Model 真值。

#### D3. Repair adapters

- 现有 CGAL/mesh repair；
- 摆放和方向修复；
- 支持后续新增独立算法 adapter。

#### D4. Repair workflow

- 修复计划；
- before/after 预览；
- 用户确认；
- 写回 Model；
- Undo snapshot；
- bbox/hull/plate/切片失效。

**验收重点**：拒绝不改变模型；接受后可 Undo；修复后保存/加载一致；取消不留下部分修改。

---

### Epic E：AI 参数调优

#### E1. SlicingContextBuilder

输入：

- 模型诊断；
- 当前有效配置；
- Printer/Filament/Process；
- plate/object 范围；
- 正式切片统计和警告；
- 用户目标：质量、速度、强度、耗材等。

输出必须脱敏、结构化、可版本化。

#### E2. AIConfigProposalService

验证顺序：

1. key 是否存在；
2. option 类型是否正确；
3. 数值和枚举是否合法；
4. 作用范围是否允许；
5. 与 printer/material 是否兼容；
6. 是否修改被禁止的安全或机器能力字段；
7. 预计影响哪些切片步骤。

#### E3. TrialSliceJob

- 为候选创建隔离 Print/Model snapshot；
- 不覆盖当前 plate 的 Print 和 result；
- 候选数量和并发预算；
- 取消和临时文件清理。

#### E4. 评分和比较

最低指标：

- 预计时间；
- 耗材；
- 切片警告；
- 支撑、换料和冲刷；
- 几何/路径可计算质量代理；
- 与 baseline 的参数差异。

#### E5. 正式应用

- 用户选择候选；
- 写入正确 scope；
- 创建 Undo；
- 更新 dirty；
- 触发标准 `Print::apply` 失效；
- 正式重切片并进入 Preview。

**验收重点**：未接受时项目不变；候选无效时不能 apply；功能关闭时原切片结果不变。

---

### Epic F：兼容、测试与交付

#### F1. 单元测试

- Provider response validation；
- workflow state machine；
- config key/type/range/scope；
- issue model；
- score/comparison；
- task persistence。

#### F2. 集成测试

- mock Provider / mock sidecar；
- 超时、重试、取消、断线和重启；
- 生成产物导入；
- 修复 Undo；
- 调参候选试切与正式 apply。

#### F3. 兼容测试

- 旧 3MF、当前 3MF round-trip；
- 系统、用户和项目内嵌 profiles；
- AI 功能关闭；
- Windows/macOS/Linux；
- 无网络和服务不可用。

#### F4. 交付

- CMake；
- 安装包与 sidecar/provider 资源；
- 本地化；
- 日志和诊断；
- 隐私提示与凭据管理。

## 2. 建议实施顺序

```text
A 平台基础
→ B 工作区和状态机骨架
→ C 模型生成 MVP
→ D 检查与修复
→ E 参数调优与试切闭环
→ F 全量兼容与交付
```

建议先用单一 mock/provider 打通接口，不在第一阶段同时实现多 Provider。

## 3. 每个开发任务的标准字段

每个 Task 至少写明：

| 字段 | 内容 |
|---|---|
| 用户价值 | 用户为什么需要它 |
| 现状入口 | 对应 ORCA 用户旅程和模块 |
| 修改范围 | 现有文件/类、新增文件/类 |
| 状态所有权 | 应用、项目、plate、对象、任务或设备 |
| 输入/输出 | 类型、schema、版本 |
| 线程模型 | GUI、Worker、切片线程或 TBB |
| 取消/异常 | 如何中止、回滚和清理 |
| 持久化 | 3MF、profile、AppConfig、job store 或不保存 |
| 兼容影响 | 旧项目、默认行为、三平台 |
| 依赖 | 前置任务和下游任务 |
| 验收标准 | 可观察行为和测试 |

## 4. Definition of Done

功能任务只有同时满足以下条件才算完成：

- 正常路径在真实应用中走通；
- 取消、错误、重试和关闭路径走通；
- UI 线程边界正确；
- 无悬挂 worker、回调或临时文件；
- Undo/dirty/Save 与状态修改一致；
- 旧项目和 profiles 可加载；
- 功能关闭时现有行为不变；
- Windows、macOS、Linux 至少完成约定级别验证；
- 有目标单测/集成测试或明确人工验证记录；
- 没有把 Provider、算法或长期状态继续堆入 `Plater`/`GUI_App`。
