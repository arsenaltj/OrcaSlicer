# OrcaSlicer 智能切片架构与用户交互设计

- 状态：Proposed
- 日期：2026-08-19
- 范围：智能切片主线第一版至可扩展目标态
- 前置材料：阶段 59 Bambu 打印经验映射、`03-ai-target-architecture.md`、ADR-001

## 1. 结论

第一版采用 **Prepare/Preview 原生画布 + 右侧可停靠“智能切片”工作台 + 后台工作流协调器**。

用户只看到四个渐进阶段：模型与材料、健康与准备、优化方案、检查并切片。系统内部保留更细的状态机，自动执行无破坏、低风险步骤，只在以下节点要求决定：

- 修复可能改变几何语义；
- 颜色或材料映射发生退化；
- 候选间存在明显质量、时间、耗材取舍；
- 将候选一次性写入正式工程并正式切片。

智能切片不是“让 AI 直接改一组参数”，而是：

```text
读取 Orca 当前上下文
→ 确定性预检
→ 生成非破坏候选
→ 隔离试切
→ 用真实 G-code 指标比较
→ 用户确认
→ 事务式应用
→ Orca 正式切片与 Preview
```

这套设计最大限度复用 Orca 已有的方向、摆盘、修复、配置验证、切片和预览能力，新增部分主要是领域契约、编排、候选隔离、解释和交互投影。

## 2. 目标与非目标

### 2.1 产品目标

- 普通用户无需理解大量切片参数，也能得到可打印、可解释的推荐方案。
- 专业用户可以展开证据、进入原生参数页、保留当前设置或只接受部分建议。
- 推荐方案必须以真实试切结果为依据，而不是只靠模型文本判断。
- 任一步失败都不损坏当前工程，并提供明确的继续、重试或回退路径。
- 模型生成产物与普通导入模型进入同一智能切片入口。

### 2.2 架构目标

- `libslic3r` 不依赖 wxWidgets 或 AI Provider。
- AI/Sidecar 不直接持有或修改 Orca `Model`、`DynamicPrintConfig`、`Print`。
- 正式工程状态只有 Orca 原生模型、配置和切片结果一套真值。
- 用户接受前，候选与正式工程完全隔离。
- AI 功能关闭时不改变现有导入、准备、切片、保存和打印行为。

### 2.3 第一版非目标

- 不训练新的切片模型，不把现有确定性算法改写为 LLM 算法。
- 不自动修改硬件能力、校准值、喷嘴直径、最大温度等高风险设备参数。
- 不静默简化网格、统一闭孔、自动降低冲刷乘数或替用户决定特殊几何语义。
- 不在 `.3mf` 中保存 AI 原始回答、试切缓存或完整工作流历史。
- 不创建第二套 3D 编辑器、参数系统或 G-code Viewer。

## 3. 方案比较

| 方案 | 描述 | 优点 | 主要问题 | 结论 |
|---|---|---|---|---|
| A. Prepare 内可停靠工作台 | 在原生 `Plater` 右侧呈现四阶段流程，深链到 Prepare/Preview | 复用唯一画布和真实项目上下文；低学习成本；可随时手动介入 | 需要控制与现有 Sidebar/AUI 面板的宽度和状态同步 | **推荐** |
| B. 六步独立向导 | 导入、修复、颜色、摆盘、调参、切片逐页确认 | 过程清晰、实现状态相对线性 | 交互频繁；遮挡/割裂原生画布；专家用户重复确认 | 不作为默认，可保留为教学模式候选 |
| C. 分散增强现有界面 | 在方向、摆盘、参数、Preview 各处增加“智能”按钮 | 对现有布局改动最小 | 入口和真值分散；难表达端到端进度、候选一致性和回滚 | 不采用 |

推荐 A，但不把业务逻辑塞进 Panel。Panel 只展示 `SmartSlicingViewModel`，所有阶段推进、取消、失效和事务应用由 Application 层负责。

## 4. 总体架构

```text
┌──────────────────────── Orca GUI / wxWidgets ────────────────────────┐
│ MainFrame  ──入口/装配                                               │
│ Plater Prepare/Preview（唯一 3D 与 G-code 画布）                     │
│ SmartSlicingPanel ─ SmartSlicingPresenter ─ SmartSlicingViewModel   │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ commands / immutable snapshots
┌──────────────────────── Application ─────────────────────────────────┐
│ SmartSlicingCoordinator                                              │
│  ├─ PreflightWorkflow       ├─ CandidatePlanningWorkflow             │
│  ├─ TrialSlicingWorkflow    ├─ ApplyWorkflow                         │
│  └─ state machine / cancellation / revision checks / observer       │
└───────────────┬───────────────────────┬───────────────────────────────┘
                │ ports                 │ optional typed proposal
┌───────────────▼────────────┐  ┌───────▼──────────────────────────────┐
│ GUI/AI/Orca adapters      │  │ IParameterAdvisor                   │
│ IOrcaWorkspace            │  │ SidecarParameterAdvisor / LocalRule │
│ ITrialSliceExecutor       │  │ schema + allowlist + explanations   │
│ IOfficialSliceGateway     │  └──────────────────────────────────────┘
│ IWorkflowRuntimeStore     │
└───────────────┬────────────┘
                │
┌───────────────▼──────────────── Orca core ───────────────────────────┐
│ Model / Geometry / Orient / Arrange / Print::validate               │
│ DynamicPrintConfig / PresetBundle / GCodeProcessorResult / Preview  │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.1 目录建议

```text
src/slic3r/AI/SmartSlicing/
  Domain/
    SmartSlicingTypes.hpp
    WorkflowState.hpp
    PrintabilityReport.hpp
    RepairPlan.hpp
    PlacementCandidate.hpp
    ParameterProposal.hpp
    SliceCandidate.hpp
    SlicingMetrics.hpp
  Application/
    SmartSlicingCoordinator.hpp/.cpp
    PreflightWorkflow.hpp/.cpp
    CandidatePlanningWorkflow.hpp/.cpp
    TrialSlicingWorkflow.hpp/.cpp
    ApplyWorkflow.hpp/.cpp
  Ports/
    IOrcaWorkspace.hpp
    IParameterAdvisor.hpp
    ITrialSliceExecutor.hpp
    IOfficialSliceGateway.hpp
    IWorkflowRuntimeStore.hpp

src/slic3r/GUI/AI/SmartSlicing/
  SmartSlicingPanel.hpp/.cpp
  SmartSlicingPresenter.hpp/.cpp
  SmartSlicingViewModel.hpp

src/slic3r/GUI/AI/Orca/
  OrcaSmartSlicingAdapter.hpp/.cpp
  OrcaTrialSliceExecutor.hpp/.cpp
  OrcaOfficialSliceGateway.hpp/.cpp
```

当前 `OrcaWorkspaceAdapter` 继续作为模型生成导入兼容门面，但其智能切片职责不再增长。迁移时先在内部委托给新的窄接口，最终让导入适配和智能切片适配分离。

### 4.2 依赖规则

- Domain 只含值对象、枚举、评分和纯规则；不得包含 wx、`Plater` 或 Provider SDK。
- Application 依赖 Ports 与 Domain，不依赖具体 Orca GUI 类型。
- `GUI/AI/Orca` 是唯一允许直接接触 `Plater`、Preset、正式切片进程和页面导航的 AI 目录。
- Provider 输出只能进入 `IParameterAdvisor`，并经过 schema、key、type、range、scope 和 compatibility 校验。
- Panel 不直接修改 `Model` 或 Config，不直接启动正式切片。

## 5. 核心领域契约

### 5.1 工作区版本

每次启动工作流创建 `WorkspaceContext`：

```cpp
struct WorkspaceRevision {
    uint64_t model_revision;
    uint64_t config_revision;
    uint64_t plate_revision;
    std::string fingerprint;
};
```

`fingerprint` 至少覆盖：对象/实例 ID 与 mesh 内容摘要、变换、plate、打印机/喷嘴、过程/材料 preset ID、有效配置摘要、材料槽位与颜色、床类型。

任何正式工程变化都使依赖旧版本的诊断或候选标为 `Stale`。系统不静默套用旧候选，而是在工作台提示“工程已变化，需要重新检查”。

### 5.2 结构化预检

```cpp
struct PrintabilityIssue {
    IssueCode code;
    Severity severity;
    IssueScope scope;
    Evidence evidence;
    std::vector<ResolutionOption> resolutions;
    bool blocks_trial_slice;
    bool requires_user_decision;
};

struct PrintabilityReport {
    WorkspaceRevision revision;
    std::vector<PrintabilityIssue> issues;
    Readiness readiness;
};
```

Issue 使用稳定 code，中文文案只存在于 GUI 映射层。首批覆盖：

- 开边、非流形、退化面、自交；
- 超出打印空间、对象相交、顺序打印碰撞；
- 薄壁/小特征、悬垂/桥接、底面接触与翘曲风险；
- 材料温度不兼容、耗材映射缺失、校准/设备信息不足；
- 互斥配置，例如可变层高与独立支撑层高、擦料塔约束等。

### 5.3 候选模型

候选由独立部分组成，允许解释和局部接受，但最终以一个一致配置集合试切：

```cpp
struct SliceCandidate {
    CandidateId id;
    WorkspaceRevision base_revision;
    CandidateGoal goal;             // 稳定、质量、速度、省料
    std::optional<RepairPlan> repair;
    std::vector<ObjectTransform> transforms;
    ScopedConfigPatch config_patch;
    CandidateExplanation explanation;
    CandidateStatus status;
    std::optional<SlicingMetrics> metrics;
};
```

第一版固定最多三个：当前设置（基线）、推荐方案、一个明确取舍的备选，例如“更快”或“更省料”。候选数量有上限，避免 CPU、内存和用户认知成本失控。

### 5.4 真实试切指标

`SlicingMetrics` 由隔离试切后的 `GCodeProcessorResult` 和结构化警告生成：

- 总耗时、首层/各阶段耗时；
- 模型、支撑、冲刷、擦料塔耗材体积/重量；
- 换料次数、冲刷量、空驶；
- 支撑接触、悬垂/桥接风险代理指标；
- 接缝、层数、最小层时间、速度受限原因；
- error/warning code 与作用对象。

“质量更好”必须显示证据来源；不能计算的指标显示“暂无可靠数据”，不伪造总分。

## 6. 状态机与并发

### 6.1 内部状态机

```text
Idle
 └→ CapturingContext
    └→ Preflighting
       ├→ AwaitingRiskDecision
       └→ PlanningCandidates
          └→ TrialSlicingBaseline
             └→ AdvisingParameters（Sidecar 可选）
                └→ TrialSlicingCandidates
                   └→ ReadyToApply
                      └→ Applying
                         └→ OfficialSlicing
                            └→ Completed

任一后台态 → Canceling → Canceled
任一态     → Stale（工程变化）
任一态     → Failed（带可恢复动作）
```

用户界面的四个阶段是上述状态的投影，不与内部枚举一一对应。

### 6.2 线程边界

GUI 线程负责捕获正式工作区快照、更新 ViewModel、写入正式 Model/Config、创建 Orca Undo snapshot、触发 dirty/invalidation 和导航 Prepare/Preview。

Worker 负责几何预检、修复/方向/摆盘副本演算、隔离试切、指标提取、Sidecar 调用与响应校验。

所有任务携带取消令牌和 `workflow_id`；晚到结果必须同时匹配当前 workflow 与 base revision 才能进入 ViewModel。

### 6.3 资源控制

- 默认顺序执行试切，一次最多一个候选；第一版不并行抢占 Orca 的 CPU/TBB 资源。
- 默认基线 + 推荐，只有取舍明显时才计算第三个候选。
- 临时模型、配置、Print 和 G-code 结果由 workflow RAII 管理；结束、取消或超时统一清理。
- GUI 不展示伪造百分比；无法精确计量的阶段显示操作名称、已用时间和不定进度。

## 7. 事务式应用

### 7.1 用户确认前

- 预检只读。
- Repair、方向和摆盘在模型副本中计算。
- 参数建议存为 typed patch。
- 试切使用隔离的 Model/Config/Print，不替换正式 Preview。
- 现有适配器中提前关闭 `independent_support_layer_height` 或 `enable_prime_tower` 的逻辑必须先包装为显式 legacy decision，随后删除正式配置直改。

### 7.2 应用步骤

```text
1. 再次读取 WorkspaceRevision
2. 与候选 base_revision 比较；不一致则拒绝应用
3. 重新运行关键兼容校验
4. 在 GUI 线程创建一个 Orca Undo snapshot
5. 依次应用已接受的 mesh/transform/scoped config patch
6. 标记 dirty，触发标准 cache invalidation
7. 启动 Orca 正式切片
8. 成功后进入正式 Preview；失败则保留可撤销状态并给出恢复动作
```

应用是一个用户可理解的动作，Undo 也应只需一次。若正式切片失败，系统不宣称完成；用户可以撤销整次应用、返回候选修改或保持新设置手动处理。

## 8. 用户交互设计

### 8.1 入口

- 模型生成导入成功：主操作改为“导入并智能切片”，进入 Prepare 并打开工作台。
- 普通模型/3MF：Prepare 顶部或 View 菜单提供“智能切片”，作用于当前 plate；选中对象时可切换为“仅优化所选对象”。
- AI 服务不可用：本地预检、方向、摆盘和规则候选仍可运行；只隐藏/禁用依赖 Sidecar 的参数建议，并说明原因。

### 8.2 默认布局

工作台作为 `wxAuiPane` 停靠在 Prepare/Preview 右侧，建议宽度 360–420 DIP，可关闭、恢复和在大屏上拖动。它与现有原生 Sidebar 不同时强制展开；窗口过窄时采用互斥显示或浮动，不压缩画布到不可用。

```text
┌──────────────────────── Prepare / Preview ───────────────────┬──────────────────────────┐
│                                                              │ 智能切片             ×   │
│                                                              │ ①模型 ②准备 ③优化 ④检查 │
│                 Orca 原生 Plater / Preview                    ├──────────────────────────┤
│                                                              │ 当前打印条件             │
│        问题定位、方向预览、支撑/接缝/G-code 仍在此显示        │ X1C · 0.4 mm · PLA × 4   │
│                                                              │ 目标  [稳定▼]             │
│                                                              ├──────────────────────────┤
│                                                              │ 当前阶段卡片/问题/候选     │
│                                                              │ ...                      │
│                                                              ├──────────────────────────┤
│                                                              │ [保留当前设置] [主操作]   │
└──────────────────────────────────────────────────────────────┴──────────────────────────┘
```

现有 Sidebar 的“AI 自动流程”六行状态在迁移期保留为只读摘要，并由同一 ViewModel 投影；不得继续持有独立业务状态。目标态可缩成一个“智能切片进行中/需处理”入口卡。

### 8.3 四阶段内容

#### 阶段 1：模型与材料

首屏只显示当前 plate、对象数量、打印机/喷嘴、床类型、材料和颜色映射、优化目标。

默认目标为“稳定打印”，另有“质量优先”“速度优先”“省料优先”。高级权重放在折叠区，不要求普通用户调滑块。阻断条件就近显示，例如未选打印机、材料温度不兼容、对象为空。主操作是“开始检查”。

#### 阶段 2：健康与准备

问题按“必须处理、建议优化、已通过”分组。每项显示图标 + 严重度文字 + 一句话影响。展开后显示证据、影响对象、画布定位和可选解决方式。

低风险修复可默认勾选但不直接写入；改变开孔、薄壁或特殊切片语义的修复必须用户选择。主操作是“生成优化方案”。

#### 阶段 3：优化方案

先展示进度卡：正在分析方向、摆盘、支撑和参数；当前子任务、已用时间、取消按钮可见。

完成后最多三张候选卡。推荐卡突出“为什么推荐”，而不是只显示综合分：

```text
推荐 · 稳定打印
预计 4h 18m  ↓12m       总耗材 128g  ↑3g
支撑 19g     ↓8g        换料 142 次  ↓37
风险：底边仍建议 5 mm brim
[查看全部变更] [在画布预览]
```

候选对比使用相同指标单位和基线差值。颜色、箭头之外必须保留文本；提升与代价同时可见。

#### 阶段 4：检查并切片

显示最终变更摘要：模型修复与变换，plate/对象/局部参数，预计时间与耗材变化，尚未消除的 warning，以及将触发的正式动作。

固定底部主操作为“应用并正式切片”；次操作为“保持当前设置并切片”或“返回比较”。应用前不弹通用确认框，只有高风险未确认项才使用就地确认或专用对话框。

正式切片成功后切到 Orca Preview，工作台变成结果摘要：“已按推荐方案切片”，并提供“查看变更”“撤销本次应用”“重新优化”。

### 8.4 交互状态表

| 状态 | 主操作 | 次操作 | 画布行为 |
|---|---|---|---|
| 无可打印对象 | 禁用“开始检查” | 导入模型 | 保持 Prepare |
| 上下文就绪 | 开始检查 | 关闭工作台 | 原生编辑可用 |
| 后台分析/试切 | 取消 | 收起工作台 | 画布可查看；会使 revision 变化的编辑将触发失效提示 |
| 有阻断问题 | 处理所选问题 | 保持当前并手动处理 | 点击 issue 定位对象/区域 |
| 候选就绪 | 继续检查并应用 | 保留当前设置 | 可切换候选可视化，不改变正式工程 |
| 工程已变化 | 重新检查 | 放弃旧候选 | 清除候选覆盖层 |
| 正式切片失败 | 查看错误并修正 | 撤销本次应用 | 保持 Prepare/失败 Preview 状态 |
| 完成 | 查看 Preview | 撤销/重新优化 | 使用正式 G-code Viewer |

### 8.5 手动路径与专家控制

- 每条问题或变更都可“在原生设置中打开”，跳到对应 Orca 页面。
- 用户手动更改后，系统明确标记旧候选失效并可重新计算，不阻止专业操作。
- “查看全部变更”按 scope 分组：项目、plate、对象、volume/layer range；禁止用未经翻译的 config key 作为默认文案。
- 保留当前 `AIAssistantPanel` 作为实验性自由问答入口，但它不承担端到端智能切片主流程；长期可复用同一个参数建议校验器。

## 9. 视觉与无障碍规范

设计系统脚本给出的“工业灰 + 安全强调色、可见焦点、高对比、快速状态反馈”原则可采用，但具体字体、颜色和控件全部继承 Orca 当前主题；不引入网页字体、发光效果或独立暗色体系。

- 使用 Orca 现有 `Label`、`Button`、`StaticBox`、图标和 `FromDIP()`。
- 8 DIP 基础间距；卡片内边距 12 DIP；主要控件高度至少 32 DIP。
- 标题/正文/辅助信息优先复用 `Label` 字体 token，不硬编码平台字体。
- 运行、成功、警告、失败均使用“图标 + 文本 + 颜色”，不依赖颜色单独传义。
- Tab 顺序按阶段、内容、底部操作排列；Space/Enter 激活，Esc 取消当前后台任务但不关闭整个工程。
- 焦点框始终可见；正文与背景对比度目标至少 4.5:1。
- 动画仅用于短暂状态过渡；尊重系统减少动画设置。未知时长使用不定进度，不循环显示虚假百分比。
- 空态、加载、取消、离线、超时、部分成功、过期候选和正式切片错误都有独立文案和动作。

## 10. 失败模式与恢复

| 失败模式 | 检测 | 用户看到 | 恢复策略 |
|---|---|---|---|
| Sidecar 不可用/超时 | health/请求超时 | “参数建议暂不可用，本地优化仍可继续” | 使用规则候选；可单独重试参数建议 |
| Provider 返回非法 key/type/range | schema + allowlist | 被拒建议和简明原因，不显示为可应用 | 忽略非法项；记录脱敏诊断 |
| 试切内存或时间超预算 | watchdog/资源计数 | 某候选未完成，基线仍保留 | 减少候选、降低并发、允许只用基线 |
| 用户编辑导致 revision 变化 | fingerprint 比较 | “工程已变化，旧方案不能应用” | 一键重新检查；不自动合并高风险变更 |
| 候选修复改变特殊几何语义 | issue policy | 必须处理项与原始/建议模式 | 用户显式选择普通/even-odd/close holes/不修复 |
| 正式应用中途异常 | apply transaction | “应用未完成” | 不启动正式切片；回滚或一次 Undo |
| 正式切片失败 | Orca slicing event | 原生错误 + 智能摘要 | 保留可检查状态，允许撤销或修改后重切 |
| 应用关闭/崩溃 | runtime journal | 重启后提示未完成工作流 | 丢弃临时候选；正式内容由 Orca backup/3MF 恢复 |
| 多色映射退化 | slot/palette check | 显示丢色/合并色及影响 | 手动映射或明确接受降级，不自动覆盖项目耗材 |

## 11. 非功能要求

### 11.1 性能与响应

- GUI 事件处理目标小于 100 ms；重计算不得运行在 GUI 线程。
- 参考 Windows 设备上，10 万面快速预检目标 2 秒内、100 万面目标 8 秒内进入首批结果；超时仍需持续响应取消。
- 点击取消后 2 秒内更新为“正在停止”；算法在安全检查点收敛，不能强杀共享 Orca 线程。
- 第一版试切并发为 1，候选上限为 3，临时磁盘和内存按任务设预算并记录峰值。

以上为工程预算，需在 P1 建立基准后校准，不作为当前已达成性能承诺。

### 11.2 可靠性与兼容

- 同一输入、同一版本、同一目标的确定性部分应产生稳定 issue code 和候选顺序。
- AI 关闭、服务离线和工作台关闭时，普通 Orca 行为与默认值不变。
- 接受后的参数继续使用 Orca 现有 scope 和 3MF/profile 序列化；新增持久字段必须可选且有迁移测试。
- Windows 为首发验证平台；Domain/Application 测试必须跨 Windows/macOS/Linux，wx 布局与取消/关闭做三平台烟测。

### 11.3 隐私与安全

- 参数 Advisor 默认只接收脱敏结构化上下文：issue code、尺寸/统计、有效参数白名单、打印机/材料能力和基线指标。
- 第一版不上传原始 mesh、图片、文件路径、用户名或 Provider 凭据。
- 请求/日志移除 token、绝对路径和用户输入中的敏感字段；原始 Provider 回答不进入 3MF。

## 12. 测试策略

### 12.1 Domain 单元测试

- issue severity/readiness 聚合；
- scope 和参数白名单校验；
- candidate ranking 与不可比较指标；
- workspace revision 失效；
- 状态机合法/非法转换；
- 高风险 decision policy。

### 12.2 Application 合同测试

- mock workspace 的快照、候选、取消、晚到结果丢弃；
- Sidecar 离线/非法输出降级；
- 试切部分失败仍保留基线；
- stale candidate 拒绝应用；
- 一次性 apply/rollback/Undo 语义。

### 12.3 Orca 集成测试

- `.3mf` 老项目与普通 STL/OBJ 导入；
- 单色/四色、单 plate/多 plate、锁定对象/锁定 plate；
- 擦料塔、顺序打印、可变层高、独立支撑层高、特殊切片模式；
- AI 禁用、Sidecar 离线、取消、关闭应用、重新打开工程；
- 接受前正式配置与正式 Preview 零变化；接受后原生 Undo、dirty、保存和 Preview 正确。

### 12.4 GUI 验收

- 1366×768、1547×981 和高 DPI 下主操作始终可见；
- 只用键盘完成开始、查看候选、应用、取消；
- light/dark 和 Windows/macOS/Linux 文本不截断；
- 所有风险、错误和指标不只靠颜色表达。

## 13. 分期实施

### P0：领域骨架与可用预检

- 新建 Domain/Application/Ports 和 `SmartSlicingCoordinator`；
- 建立 `WorkspaceContext`、`WorkspaceRevision`、`PrintabilityReport`、稳定 issue code；
- 以 Orca adapter 只读采集当前 plate、模型、材料、配置和 `Print::validate()` 结果；
- 实现工作台空壳、四阶段 ViewModel、开始/取消/失效；
- 把现有六行 Sidebar 流程改为 ViewModel 的兼容投影；
- 封装并停止确认前的正式配置直改。

### P1：方向/摆盘候选与隔离试切

- 接入 `Orient`、`ArrangeJob` 的副本候选；
- 建立 baseline/recommended 的隔离 `Print` 与 `GCodeProcessorResult` 指标；
- 完成候选对比、stale 检测、事务式应用和正式 Preview；
- 覆盖擦料塔、锁定 plate、顺序打印、材料兼容和取消。

### P2：参数建议与多色联合优化

- 抽取现有 `AIAssistantConfig` 校验能力为可复用 typed validator；
- Sidecar 只返回参数候选/解释，不直接控制工作区；
- 加入冷却、层高、接缝、支撑、brim 和多色换料/冲刷联合试切；
- 支持项目/plate/对象/volume/layer range scope。

### P3：质量闭环与恢复

- 建立基准集、性能预算、可解释评分校准；
- 增加运行时历史、崩溃恢复、候选缓存和清理策略；
- 三平台 GUI/打包、旧 3MF/profile 和上游 Orca 合并门禁。

## 14. 第一版验收定义

P0/P1 完成时，以下场景必须成立：

1. 普通 STL 或生成模型进入 Prepare 后可从右侧工作台开始智能切片。
2. 用户确认前，正式模型、配置和 Preview 不发生变化。
3. 系统能给出结构化预检，并在画布定位至少对象级问题。
4. 系统能比较当前设置与一个推荐候选，时间/耗材/换料/警告来自真实隔离试切。
5. 用户编辑工程后旧候选立即失效，不能错误应用。
6. 用户一次确认后走原生 Undo/dirty/invalidation/正式切片，并进入正式 Preview。
7. Sidecar 离线、试切取消或候选失败时，普通手动切片仍然可用。
8. AI 功能关闭时现有 Orca 默认行为和旧 3MF/profile 不变。

## 15. 待负责人确认

- 默认交互采用本设计的单页工作台，还是将六步向导作为默认；本设计推荐前者。
- 第一版优化目标是否只保留“稳定打印”，另外三种先显示为 P2；架构支持四种，但减少首版范围更稳。
- P1 是否把多色换料/冲刷纳入首批候选评分；从价值看应至少读取指标，但联合调参可延后。
