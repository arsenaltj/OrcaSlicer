# ADR-002：智能切片采用 Plater 工作台与事务式候选架构

- 状态：Accepted
- 日期：2026-08-19
- 决策者：项目负责人、智能切片负责人
- 批准日期：2026-08-19

## 背景

当前智能链路已经能在模型导入后执行颜色映射、网格检查/修复、摆盘和切片，并在 Sidebar 显示六步状态；另有实验性的 `AIAssistantPanel` 可向 Sidecar 请求参数修改。

但当前实现还没有稳定的智能切片领域对象、隔离试切和事务应用。`OrcaWorkspaceAdapter` 混合了多类职责，部分兼容处理会在用户确认前修改正式配置。若继续直接扩展 Panel、Sidebar 或适配器，会放大 `Plater` 耦合、回归风险和与 Orca 上游合并成本。

阶段 59 对 Bambu 官方经验和 Orca 代码的对照表明，方向、摆盘、修复、兼容校验、切片和真实 G-code 指标大多已有确定性实现。缺口主要是编排、候选、隔离、解释和应用边界。

## 决策

1. 智能切片保持同仓模块化单体，不拆独立切片服务。
2. 用户入口采用 Prepare/Preview 原生 `Plater` 右侧可停靠工作台，不创建第二套 3D 或 G-code 画布。
3. 工作台只投影 ViewModel；`SmartSlicingCoordinator` 持有工作流状态机。
4. Domain/Application 通过窄 Ports 访问 Orca；只有 `GUI/AI/Orca` 适配层可接触 `Plater`、正式 Config、正式切片和页面导航。
5. 几何、方向、摆盘和参数在用户接受前均为绑定 `WorkspaceRevision` 的非破坏候选。
6. 候选使用隔离 Model/Config/Print 试切，比较指标来自 `GCodeProcessorResult` 和结构化警告。
7. Sidecar 仅提供结构化参数建议和解释；输出必须经过 key/type/range/scope/compatibility 校验。
8. 应用候选前重新校验 revision，并以一个 Orca Undo snapshot 事务式写入；随后走原生 dirty、invalidation、正式切片和 Preview。
9. AI 报告、原始回答和试切缓存不进入 3MF；接受后的模型/配置继续使用 Orca 原生持久化。
10. 现有 Sidebar 六步流程迁移为同一 ViewModel 的只读兼容投影；当前自由问答 `AIAssistantPanel` 不作为智能切片主流程。

## 理由

- 唯一画布和唯一正式工程真值，减少状态分叉。
- 用户确认前零正式副作用，满足可比较、可取消和可回滚要求。
- 真实试切指标比 LLM 文本评分更可靠，也能复用 Orca 已有切片能力。
- 工作台保留上下文并减少向导式反复确认，普通用户和专家都能随时进入原生设置。
- 领域端口把新增代码与 `Plater`、Provider 和上游高频文件隔离，便于单测和跨平台演进。

## 放弃的方案

### 独立六步向导作为默认入口

过程清楚，但会割裂 Prepare/Preview 上下文、增加确认次数，并容易复制 3D 预览和参数真值。可作为教学或首次使用引导，但不作为正式默认架构。

### 在现有 Orca 功能点分散增加 AI 按钮

实现改动看似小，但无法保证端到端候选使用同一上下文，也难处理失效、取消、比较和一次性 Undo。

### 让 Sidecar 直接产生并应用最终配置

Sidecar 无法可靠掌握 Orca 完整作用域、兼容关系、当前工作区 revision 和正式切片状态；直接应用会扩大安全与回归风险。

### 复制 Plater/Preview 建立独立智能页面

能够完全定制布局，但会形成第二套画布状态、选择、相机、warning 和 G-code 真值，长期成本不可接受。

## 后果

正面：

- 候选可真实比较、失效检测和安全取消；
- 正式工程仍完全遵循 Orca 原生 Undo、3MF/profile 和 Preview；
- Sidecar 离线时本地确定性能力仍可工作；
- GUI、领域、Orca 和 Provider 可独立测试与替换；
- 对 `MainFrame`/`Plater` 的长期增量可限制在装配、入口和 adapter。

代价：

- 需要先投入 P0 建立 DTO、Ports、状态机和只读快照，短期功能增长会变慢；
- 隔离试切增加 CPU、内存、临时磁盘和取消管理复杂度；
- `WorkspaceRevision` 必须覆盖足够上下文，否则会出现错误复用或过度失效；
- wxAUI 工作台需处理窄窗口、现有 Sidebar 和 AI Assistant 的共存布局。

## 约束与检查

- 用户确认前正式 Model、Config 和 Preview 的差异必须为零。
- 候选必须带 base revision；stale 候选在 Application 层被拒绝，不能只靠按钮禁用。
- 试切默认并发为 1，候选默认不超过 3。
- AI 关闭、Sidecar 离线和工作台关闭回归为发布门禁。
- 旧 `.3mf`、printer/process/filament profile 和普通导入/切片必须通过兼容测试。
- Domain/Application 不得 include wx、`Plater` 或具体 Provider SDK。
- 任何正式配置写入必须能在一个原生 Undo 操作中撤销。

## 后续决策

- P1 前补充试切对象复制/缓存和内存预算的实现 ADR。
- P2 前补充候选评分权重、参数作用域和多色联合优化 ADR。
- 若未来要把远端几何分析引入 Sidecar，必须另行评审 mesh 上传、隐私、缓存和失败降级边界。
