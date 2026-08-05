# 发现与决策

## 需求
- 用户的总体目标是在 OrcaSlicer 上构建四类 AI 能力：
  1. 模型生成：以 Tripo 为当前示例，从文字和图片生成可作为切片输入的 3D 模型。
  2. 模型可打印性检查与自动修复。
  3. 切片参数 AI 化。
  4. 简化的端到端 AI 交互：用户只描述目标或上传图片，系统自动完成生成、检查、修复、参数配置和切片，中间最多提出少量必要问题。
- 架构必须 provider-agnostic；具体模型、LLM、生成服务和修复服务应可替换。

## 路线图初步判断
- 推荐先实现受控的固定工作流：`需求澄清 → 模型生成/导入 → 可打印性检查 → 必要修复 → 试切 → 参数选择/比较 → 最终切片 → 结果摘要`。
- 第一版不应做“LLM 自由调用任意能力”的开放 Agent。LLM 只负责理解意图、提出少量澄清问题、选择候选方案和解释结果；几何检查、修复、配置验证、切片、指标计算和导入必须由 OrcaSlicer 的确定性代码执行。
- 当前四项能力成熟度的初步排序：模型生成已有可运行纵向切片；切片参数 AI 已有受控建议/应用入口但不是闭环优化；检查/修复需要统一服务化现有内核；端到端编排尚未实现，只具备部分组件。
- 端到端 MVP 应优先采用显式状态机和 typed tools，每一步有输入/输出 schema、前置条件、结果证据、可重试策略和审批级别；后续再允许模型在白名单工具上动态规划。

- 四项能力应遵循“独立服务先验收、编排层后组合”的依赖方向：模型生成、模型检查/修复、切片试验/比较都应能被普通 GUI 单独调用；端到端 AI 交互只编排这些已验证能力，不承载核心算法。
- 推荐的核心抽象不是“一个万能 AI Assistant”，而是三层：OrcaSlicer 确定性能力服务；provider-agnostic sidecar/provider adapters；持久化工作流编排与用户交互层。

## 分期路线图骨架
1. **基础治理与现有生成收尾**：恢复 Git；修复页面回归；增加 feature gate；统一 `/health`/版本/能力协商；让 sidecar 可跨平台安装；建立生产/mock 契约测试；完成真实 Tripo 文生/图生 smoke。
2. **模型检查 MVP**：把现有确定性几何/摆放/切片前检查收敛成统一 `PrintabilityReport`，先只检查和解释，不自动改模型。
3. **安全修复 MVP**：按“安全自动修复 / 需用户确认 / 不可自动修复”分级；始终保留原模型和可撤销副本；修复后重新检查并比较差异。
4. **切片参数闭环**：从当前单次参数建议升级为有约束的候选配置、隔离试切、结构化指标比较、项目级应用与撤销；LLM 负责生成候选/解释，OrcaSlicer 负责配置校验和切片评估。
5. **低交互端到端 MVP**：固定状态机串联输入澄清、生成/导入、检查、修复、试切、参数选择和最终切片；只在缺失关键约束、产生外部费用、执行高风险修复或最终输出前询问用户。
6. **受控 Agent 化**：在固定工作流稳定后，开放白名单 typed tools 的动态规划；所有调用仍经过前置条件、权限、预算、审计和恢复机制。

## 建议的跨能力数据契约
- `IntentSpec`：输入类型、用途、目标尺寸、打印机/材料/喷嘴、质量/速度/强度偏好和允许的用户问题数。
- `GeneratedArtifact`：来源、provider、任务 ID、格式、hash、单位、尺寸、临时位置和生成参数。
- `PrintabilityReport`：检查版本、几何指标、问题代码/严重度/区域、是否阻塞、可修复性和建议动作。
- `RepairPlan` / `RepairResult`：动作、风险、原模型快照、修复副本、前后报告和几何变化摘要。
- `SliceCandidate`：配置快照、切片结果、时间/耗材/支撑/层数/警告等指标、硬约束和评分。
- `WorkflowState`：状态、步骤、产物、尝试次数、预算、待确认事项、错误和恢复点。

## 建议的成熟度定义
- **L0 规划**：只有文档或 UI 占位，没有可调用能力。
- **L1 建议器**：AI 给出文本/结构化建议，用户手动执行。
- **L2 受控执行器**：AI 可调用白名单动作，但每项输入经过 schema 与领域校验，关键动作需确认。
- **L3 可度量闭环**：系统执行候选方案，以确定性指标评估并选择，支持快照、撤销和复现。
- **L4 低交互编排**：用户只给目标/图片，工作流自动串联 L2/L3 能力，只在关键缺口和风险点询问。
- **L5 自适应 Agent**：在预算、权限、状态机和审计约束内动态规划；不是当前 MVP 目标。

## 各阶段最低验收原则
- 每个能力先有普通 GUI/API 可独立调用，再接入编排。
- 每个外部付费调用都有费用确认、幂等键、任务 ID、取消语义和重试边界。
- 每个模型/配置变更都有原始快照、差异摘要、撤销路径和确定性复验。
- 每个自动决策都保存输入、候选、指标、选择理由和工具版本，保证可复现。
- 功能关闭或 sidecar 未配置时，不改变现有 OrcaSlicer 默认行为。
- Windows、macOS、Linux 使用同一契约和验收集；provider 适配器通过契约测试而不是 GUI 特判。

## 与既有架构文档的关系
- `Docs/architecture/03-ai-target-architecture.md` 已覆盖用户本次提出的四类能力，目标模块包括 `AIProviderGateway`、`GeneratedModelImporter`、`ModelPreflightService`、`ModelRepairWorkflow`、`SlicingContextBuilder`、`SliceTuningOrchestrator`、`AIWorkflowCoordinator`、`AIServiceManager` 和 `AIJobStore`。
- 当前实际实现顺序与文档建议不同：模型生成纵向切片已经先落地，而平台服务、统一状态机、检查/修复、隔离试切仍未落地。因此不应重做生成，而应先补平台骨架，再把当前 `ModelGenerationPanel`/sidecar 流程迁入统一任务模型。
- 既有文档的 `AIWorkspacePanel` 偏向多功能工作台；用户新提出的低交互目标需要在其上增加“Guided/Auto Workflow”模式。建议保留两种入口：专家模式可逐项操作 Generate/Inspect/Repair/Tune；简化模式只显示问题、进度、必要提问和最终结果。
- 文档中的状态机适合单个 Job，但端到端工作流还需要步骤级状态：每步产物、审批、重试、回滚点、预算和依赖。建议区分 `AIJob`（单个外部/计算任务）与 `AIWorkflowRun`（跨步骤编排）。

## 模型可打印性检查与修复现状
- 已有确定性检查基础：`TriangleMeshStats` 保存面数、包围盒、体积、壳数和开放边（`src/libslic3r/TriangleMesh.hpp:47-85`）；CGAL 有自交/闭合/体积检查（`src/libslic3r/MeshBoolean.hpp:64-75`）；`BuildVolume::object_state()` 判断越界/碰撞/低于床面（`src/libslic3r/BuildVolume.cpp:377-408`）；`Print::validate()` 与 `Plater::validate_current_plate()` 提供正式切片前阻塞检查（`src/libslic3r/Print.cpp:1262-1404`、`src/slic3r/GUI/Plater.cpp:17796-17885`）。
- 已有确定性修复基础：STL 导入时 admesh 会接边、删除孤立面、统一法线和翻转负体积，但不补洞（`src/libslic3r/TriangleMesh.cpp:79-178`）；手动 CGAL 修复包含 polygon soup 清理、退化面/孤立点删除、非流形顶点拆分、自并集、补洞和方向修正（`src/libslic3r/MeshBoolean.cpp:478-556`）。
- 已有正确回写链：`FixModelByCgal` 分壳并清理零体积薄片，经 `ModelVolume::set_mesh()` 写回、重建凸包和失效缓存，GUI 后续落床并刷新（`src/slic3r/Utils/FixModelByCgal.cpp:115-181`、`src/slic3r/GUI/GUI_ObjectList.cpp:6117-6164`）。
- 当前缺口：检查结果分散且没有统一 issue/severity/evidence 模型；自交、零体积和壳数没有统一进入 preflight；薄壁、小特征和悬垂没有稳定的模型级诊断；CGAL 修复缺少完整修改统计；缺少修复前后差异、风险分级、自动工作流、Undo 验收和端到端测试。
- 结论：检查/修复不是“从零开发算法”，而是先建立 `ModelPreflightService` 聚合现有事实，再建立 `ModelRepairWorkflow` 包装现有 repair adapter 和标准回写/撤销流程。

## 四项能力成熟度矩阵
| 能力 | 当前成熟度 | 已有基础 | 主要缺口 | 下一目标 |
|---|---|---|---|---|
| 文/图生 3D | L2 受控执行器（未产品化验收） | Tripo 流程、GPT 预处理、轮询、3MF/STL 下载、确认后导入 | UI 回归、真实双 smoke、产物深检、持久化/恢复、feature gate、三平台打包、provider registry | 稳定可发布的独立生成能力 |
| 可打印性检查/修复 | 底层算法成熟；统一 AI 能力约 L0-L1 | mesh stats、CGAL/admesh、越界、`Print::validate()`、标准 mesh 回写 | 统一 issue/report、证据定位、薄壁/悬垂模型、修复分级、before/after、Undo/E2E | 先只读 Preflight，再安全 Repair workflow |
| 切片参数 AI 化 | L2 受控执行器 | 小范围上下文、结构化 proposal、key/type/range 校验、用户选择、preset 应用和重切片 | 完整诊断/设备材料上下文、scope、baseline 指标、候选、隔离试切、评分比较、快照/撤销 | L3 可度量闭环优化 |
| 低交互端到端流程 | L0 设计 | 生成页、AI Assistant、目标架构文档 | 统一工具、协调器、状态持久化、审批策略、步骤恢复、问题预算、统一 UI | L4 固定状态机 Guided Workflow |

## 推荐目标架构
```text
AIWorkspacePanel
├─ Expert mode: Generate / Inspect / Repair / Tune
└─ Guided mode: Intent → Workflow progress → Questions → Review
                    │
            AIWorkflowCoordinator
            ├─ ApprovalPolicy / BudgetPolicy
            ├─ AIWorkflowRunStore
            └─ typed domain tools
                    │
    ┌───────────────┼──────────────────┐
GenerationService  ModelPreflight/Repair  SliceTuningOrchestrator
    │                  │                    │
GeneratedModelImporter │             TrialSliceJob / Comparison
    └────────────── Orca application facade ──────────────┘
                     │
       Model / Mesh / Config / Print / Preview

AI domain services → AIProviderGateway → provider adapters / sidecar
```
- **OrcaSlicer C++ 应用层**拥有工作流状态、项目修改、Undo/dirty、正式导入、检查/修复和切片真值。
- **sidecar**负责 provider registry、LLM 意图/候选生成和外部模型生成服务适配；它只能返回结构化计划/候选/产物，不能直接修改项目。
- **libslic3r**继续保持 AI/provider 无关，只暴露确定性几何、配置和切片能力。
- 第一版保留一个真实 Tripo adapter 和一个 mock adapter 即可；通过 capability/contract 验证 provider 无关，不需要同时接入多个真实供应商。

## Guided Workflow 建议状态机
```text
Intake
→ Clarifying?                      # 只收集阻塞执行的缺失信息
→ Generating / Importing
→ Preflight
→ RepairPlanning
→ RepairReview? → Repairing → Recheck
→ BaselineTrialSlice
→ ProposingCandidates
→ TrialSlicing → Comparing
→ FinalReview?
→ Applying → OfficialSlicing → Completed

任意活动态 → Pausing / Canceling / Failed
持久化步骤 → Resume / Retry / Rollback
```
- 推荐默认交互预算：一次集中澄清（尺寸/用途/打印机材料/质量速度强度偏好），一次付费或高风险动作授权，一次最终结果确认；已有当前 printer/material 时不重复询问。
- 可自动执行：只读检查、低风险且可撤销的变换、候选试切、指标比较。
- 必须确认：外部付费生成/用户数据上传（可会话级预授权）、中高风险几何修复、覆盖正式模型/配置、最终导出或打印。
- 高风险且无法证明意图保持的修复（例如自动加厚关键结构、删除不确定壳体）不自动执行，只解释并提供选项。

## 分期路线图与验收
### M0：工程基线与平台补洞
- 恢复可靠 Git；修复默认页抢占和导航图标；feature gate；本地化目录；统一 `/health`、协议版本和 capabilities；`AIServiceManager`/最小 `AIJobStore`；mock/production 契约一致；sidecar 三平台打包。
- **验收**：AI 关闭时现有行为不变；离线可正常切片；三平台可启动/关闭；崩溃后有明确恢复/清理；无凭据写入日志或 3MF。

### M1：模型生成产品化
- 复用当前生成链，抽出 `AIModelGenerationService` 和 `GeneratedModelImporter`；强化 3MF/STL、hash、单位、尺寸和 multipart 校验；完成真实 Tripo 文生/图生 smoke。
- **验收**：生成模型可导入、Undo、保存 3MF、重开、切片；失败/取消/退出无悬挂任务和临时文件；收费提交不因重试重复创建。

### M2：只读 Printability Preflight
- 定义稳定 `PrintabilityIssue/Report` schema；聚合 mesh stats、自交、build volume、`Print::validate()`；增加问题定位和严重度；先不自动修复。
- **验收**：固定坏网格语料有稳定报告；检查不修改项目；报告可版本化、可缓存、可在 GUI 定位；单测覆盖 issue 分类。

### M3：安全自动修复
- adapter 化 admesh/CGAL/摆放修复；生成 `RepairPlan`；按风险分级；在模型副本执行；输出 before/after；接受时统一 `set_mesh`、Undo、dirty、cache invalidation；随后复检。
- **验收**：拒绝/取消零修改；接受后可 Undo；保存重开一致；修复后 report 改善且不引入新 blocker；端到端测试覆盖开放边、反法线、退化面、补洞和回写。

### M4：切片参数 L3 闭环
- 扩充 `SlicingContextBuilder`；构造 2-3 个有边界的候选；创建隔离 Model/Print snapshot 试切；从 `GCodeProcessorResult` 采集时间、耗材、支撑、换料/冲刷、警告和质量代理；硬约束过滤后比较；用户接受才正式 apply。
- **验收**：候选不污染正式项目；同输入可复现；无效配置无法执行；有 baseline/candidate 差异和指标证据；应用后可撤销并正式重切片。
- 第一版只宣称优化“时间/耗材/支撑及已定义风险代理”，不要宣称能预测真实成品质量；真实打印反馈闭环后置。

### M5：低交互 Guided Workflow MVP
- 用固定状态机组合 M1-M4；IntentSpec 集中澄清；统一进度、问题、审批、取消、重试、恢复和最终摘要；专家模式仍可逐步干预。
- **验收**：文字或图片输入能在少量问题内走到可切片结果；每一步有证据与恢复点；中途关闭后可继续；费用和高风险动作不会静默执行。

### M6：受控 Agent 化
- 将已验收能力暴露为 typed tools；LLM 可在白名单、预算、权限和状态机约束内动态选择步骤；引入工具版本、审计和 eval 集。
- **验收**：自由规划不能绕过 schema、审批、项目快照或预算；失败可回放；同一目标有离线回归评估。

## 最短实施路径
1. 先完成 M0 中的 Git、两个 UI 回归、feature gate、协议/capability 和 mock 契约。
2. 立即封闭当前生成链的真实 Tripo 双 smoke，作为 M1 基线。
3. 下一段核心开发从 `PrintabilityIssue/Report` + `ModelPreflightService` 开始；不要先写 Agent UI。
4. Preflight 稳定后接现有 CGAL/admesh 为 Repair adapters。
5. 同时仅设计切片候选/指标 schema，等 Preflight issue 成为上下文后实现隔离试切。
6. M1-M4 均有独立验收后，再做 Guided Workflow；Agent 动态规划最后开放。

## 研究发现
- 项目根目录原先没有 `task_plan.md`、`progress.md` 或 `findings.md`。
- `session-catchup.py` 执行成功但没有输出，未发现可恢复的未同步会话。
- 当前构建配置证据：`build/CMakeCache.txt` 使用 `Visual Studio 17 2022` 多配置生成器，`CMAKE_BUILD_TYPE=Release`，安装前缀为 `build/OrcaSlicer`。
- 当前 `build/` 中存在多级 `CTestTestfile.cmake`（说明测试被配置），但没有找到 `Testing/Temporary/LastTest*.log`（没有可核验的 CTest 实际执行记录）。
- 当前 `build/` 未找到 `OrcaSlicer.exe`；因此可依赖历史会话确认上一轮完整 Release 构建/安装/运行成功，但不能声称当前磁盘仍保留可执行产物。
- 项目根目录不是 Git 仓库，且顶层没有 `.git`，因此暂时无法直接获取分支、提交或工作区 diff。
- 根目录时间戳显示近期工作与 AI/Tripo 集成有关：`start_orcaslicer_with_agnes.bat`、`start_orcaslicer_with_ai.bat`、`real-tripo-text.3mf` 以及多张 Tripo UI 验证截图。
- 找到嵌套 Git 仓库 `.claude/upstream-orcaslicer`，最初作为上游对比候选，后因版本晚于当前副本而排除为原始基线。
- 该基线记录为 `main` / `origin/main`，HEAD 为 `a62fb17e03d159d5b562cc6d64163346e454b5de`（2026-07-25 22:31:40 +0800，`Remove cloud deletion of owned plugins from the plugin dialog (#14946)`）。
- 基线仓库自身工作树已被清空，因此普通 `git status` 显示全量删除；尝试组合其对象库与当前根目录时，索引/工作树配置仍将同一批文件同时识别为删除和未跟踪，该全树结果不可采信。
- 版本文件进一步证明该仓库不是当前副本的原始基线：当前根目录为 `SLIC3R_VERSION 02.06.00.51`，嵌套仓库 HEAD 为 `02.08.01.55`。
- 当前源码最后一轮修改集中于 2026-07-28：`ModelGenerationPanel`、`AIAssistantPanel`、`MainFrame`、`GUI_App`、`Plater`、GUI CMake、图标与本地化清单；此前 2026-07-25 至 2026-07-27 还修改了 AI sidecar、Tripo 客户端、OpenAI 预处理和架构文档。
- 已从其他会话记录恢复用户原始目标：使用 Tripo 补齐文生 3D 和图生 3D。
- 历史会话末尾证实：导航和本地化调整后编译通过，并完成了 Release/安装及 mock 驱动的真实运行验证；顶部顺序为 `3D Generate → Prepare → Preview → Device → Project`，独立模型生成页面已成功显示。
- 运行验证发现两个待收尾问题：`3D Generate` 新页启动时意外抢占默认页；顶部立方体图标在青色选中态对比度不足。
- 当时决定在 `InsertPage` 后显式隐藏新页，并新增“立方体+闪光”的 active/inactive SVG。随后因上下文超限中断，因此必须以当前文件确认这两个修复是否落盘并重新验证。
- 当前截图 `model-generation-main-page.png` 证实独立页面整体布局已经运行：`3D Generate` 位于“准备”之前，左侧为输入/GPT 预处理，右侧为预览结果；截图也清楚显示选中态立方体图标为灰色、对比度不足。
- `resources/images/` 当前只找到 `tab_generate_3d_active.svg`，未找到配对的 inactive 资源，说明专用导航图标修复至少没有完整落盘。
- 当前 `MainFrame.cpp:1327-1332` 创建并插入 `ModelGenerationPanel` 后未调用 `Hide()`，且 active/inactive 图标仍都传入 `menu_obj_cube`；全 GUI 搜索没有 `tab_generate_3d` 引用。因此两个已知 UI 修复均未接入代码。
- 历史会话还保留了产品决策：生成结果优先 3MF；图生 3D 采用“图片+文字联合提示”，若 Tripo v3 不直接支持附加文字，则应走可验证的组合流程，不能静默丢弃 prompt。
- 可确认完整 Release 构建、安装和 mock 驱动运行曾成功；历史记录还明确表示 GUI 编译与 4 个 Python 模块语法检查通过，并将真实 Tripo 的两次付费 smoke（文生/图生）留作下一阶段。
- “界面问题已全部复测”的记录发生在独立主页面迁移前；迁移后新发现的默认页抢占和导航图标问题仍未完成，不能混为同一轮验收。

- 当前实现已形成两条用户流程：
  - AI 调参助手：菜单打开右侧停靠面板，调用 `/config-proposal`，校验后按勾选项写入 preset 并重新切片（`MainFrame.cpp:3111-3114`、`Plater.cpp:5139-5147`、`AIAssistantPanel.cpp:98-128,161-205`）。
  - AI 模型生成：独立 `3D Generate` 页支持 Text-to-3D 与 Image+Text-to-3D，经 GPT 预处理、用户审核、额度确认、任务轮询、3MF/STL 下载及二次确认后，通过 `Plater::add_model` 导入当前盘并切回编辑页（`ModelGenerationPanel.cpp:143-179,285-371,420-448,506-556`）。
- C++ 只依赖 sidecar HTTP 契约，供应商细节位于 Python；但 Python 当前直接绑定 AGNES、OpenAI 和 Tripo，尚无 provider registry。
- 明确未完成项：Model Library 仅为占位（`ModelGenerationPanel.cpp:244-254`）；GUI 不管理 sidecar 生命周期；取消操作不保证终止远端 Tripo；任务持久化/恢复、生成产物深度验证和 AI 定向自动测试尚缺失。
- 上线前技术缺口：任务仅在 sidecar 内存中且无 TTL；mock 与生产协议存在漂移；sidecar 启动集成目前仅有 Windows 批处理；AGNES 端点缺少 OpenAI/Tripo 已有的 HTTPS/无内嵌凭据校验；本地请求标记不是有效身份认证。
- Python sidecar 未被正常 CMake/install/package 规则包含；当前启动依赖源码目录、外部 Python 和 Windows 批处理，不满足 Windows/macOS/Linux 三平台交付要求。
- AI 功能未受配置或 feature gate 控制：模型生成页和 AI Assistant 面板无条件构造，sidecar 未配置时仍会改变导航并暴露不可用入口。
- 新面板已加入 gettext 抽取清单，但 POT 创建日期早于这轮接入，新增 AI/Tripo/GPT 文案尚未进入 POT/PO 目录。
- 当前启动方式是批处理分别启动 sidecar 和 OrcaSlicer，无 readiness、健康版本协商、崩溃重启或关闭联动。
- 7 月 28 日这一轮主要把此前独立的客户端/sidecar 能力接成可见产品 UI，包括新页签、停靠助手、导入回调、关闭清理和 CMake 接入；现状是首个可操作纵向切片，而非完整 AI Workspace。
## M0 AI 功能门控与能力发现实现（2026-07-30）
- 已新增 `enable_ai_features` AppConfig 布尔设置，默认 `false`；Preferences 的 Developer → Experimental Features 提供需重启生效的显式开关。默认状态不创建 AI page/menu/AUI pane，也不会请求 sidecar。
- production 与 mock 的 `GET /health` 已统一为 provider-neutral v1 文档：`ok`、整数 `protocol_version`、诊断性 `sidecar_version` 与 `capabilities.config_proposal/model_generation`。模型生成仅在 OpenAI 预处理与 Tripo 均配置时标记 available；协议不暴露 provider、密钥、模型或 endpoint。
- 新增 `tools/ai/test_sidecar_contract.py`，使用临时 loopback servers 验证 production/mock schema、未配置/配置 capability 和无凭据泄露；该测试不触发外部 provider 请求。
- 新增 `AIServiceManager`，只在用户启用功能后非阻塞请求 loopback `/health`，限制响应为 16 KiB，接受严格 v1 schema；请求取消和 `CallAfter` 生命周期由 weak token 保护。非 loopback endpoint 直接 fail closed，与模型生成路径的本地信任边界一致。
- `MainFrame` 将 3D Generate 改为 capability 成功后追加标签，不再占用 `TabPosition` 固定 index，因此默认 Prepare/Preview 等现有索引回到历史位置；新增 active/inactive 专用图标。AI Assistant 也仅在 config proposal capability 成功后延迟创建 AUI pane 和 View 菜单项。
- `Plater` 的 AI pane API 已做空指针安全处理，避免无 capability、reset layout 或关闭过程访问不存在的 pane。
- 验证已通过：`python tools/ai/test_sidecar_contract.py`（3 tests）、对三份 Python 文件的 `py_compile`、`git diff --check`。未完成：C++ GUI 编译、Catch2 AppConfig 测试和运行时 GUI E2E；本 shell 没有可用的 `cmake`、`MSBuild.exe`、`devenv.com` 或 `ninja.exe`，即使 `build/OrcaSlicer.sln` 存在也不能在当前会话执行。

- 验证更新：用户关闭占用实例后，Visual Studio CMake 成功构建 `libslic3r_gui` 与 `OrcaSlicer` Release target，并生成安装目录的 `orca-slicer.exe`。隔离 `--datadir` GUI E2E 证实默认值会持久化为 `enable_ai_features=false` 且不触发 mock discovery；启用后 mock 收到 `/health`，响应窗口实际出现并可打开运行时追加的 `3D Generate` 完整输入/预览页面，且该页没有抢占默认首页。当前 build 禁用 `BUILD_TESTING`/`BUILD_TESTS`，没有生成 Catch2 target；wx 自定义菜单未暴露给 Windows UI Automation，故 “Show AI Assistant” 菜单/停靠 pane 的互动验收待专用驱动补充。

- 验证补充：已在隔离 `.workbuddy/build-tests`（`BUILD_TESTS=ON`、`BUILD_TESTING=ON`）构建 `libslic3r_tests`，并随机顺序执行 `AppConfig AI feature gate`，3 项断言全部通过。AI Assistant 菜单与 AUI pane 的实际点击验证仍待专用 wx/DPI-aware 驱动；现有 Windows UI Automation 不暴露顶栏菜单项，坐标自动化会误触其他 GUI 控件，故未将其宣称为已验证。

## 技术决策
| 决策 | 理由 |
|------|------|
| 继续检查 Git 与本地构建状态 | 这是恢复实际开发进度最可靠的信息来源 |
| 续作顺序：Git 保护 → UI 收尾 → mock 复测 → 真实 Tripo 双 smoke | 降低无版本控制修改的风险，并优先封闭已经明确的功能缺口 |
| 模型库、sidecar 托管和完整 AI Workspace 后置 | 当前应先把已实现纵向切片验收到可稳定继续开发的状态 |

## 遇到的问题
| 问题 | 解决方案 |
|------|---------|
| 当前环境元数据显示目录不是 Git 仓库 | 检查顶层目录和可能的嵌套仓库 |

## 资源
- `CLAUDE.md` / `AGENTS.md`：OrcaSlicer 项目说明。
