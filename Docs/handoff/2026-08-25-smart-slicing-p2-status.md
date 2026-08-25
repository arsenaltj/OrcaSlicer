# 智能切片 P2 当前状态与后续开发交接

- 记录日期：2026-08-25
- 功能分支：`codex/smart-slicing`
- P2 功能提交：`e6808459f69290e5e1a45e0f2001fed252967d8f`
- 本交接文档提交：使用 `git log -1 --format=%H -- Docs/handoff/2026-08-25-smart-slicing-p2-status.md` 获取
- ADR-004 验收基线：`8b77ad5b2f424fd95e45f1e7a26c03961dff0a89`
- 当前结论：ADR-004 / P2 已完成并通过 Windows 验证，可进入独立试用或集成前验收；它还不是覆盖所有对象、参数层级和多材料策略的通用自动切片器。

## 必读资料

- [ADR-002：事务式智能切片工作台](../architecture/ADR-002-smart-slicing-transactional-workbench.md)
- [ADR-003：Orca 桥接与发布加固](../architecture/ADR-003-smart-slicing-orca-bridge-and-release-hardening.md)
- [ADR-004：P2 参数范围与多色策略](../architecture/ADR-004-smart-slicing-p2-parameter-scopes-and-multicolor-policy.md)
- [智能切片最终目标实施记录](../plans/2026-08-20-smart-slicing-final-goal-implementation.md)
- [P2 实施计划](../plans/2026-08-25-smart-slicing-p2-implementation.md)
- [下一位 Codex 的交接提示词](2026-08-25-smart-slicing-codex-handoff-prompt.md)

## 当前架构

智能切片保持在独立模块中：

```text
src/slic3r/AI/SmartSlicing/
  Domain/        纯领域状态、候选、修订、参数和多色约束
  Application/   工作流编排、取消、过期检测、候选试切和应用流程
  Ports/         Orca 与外部建议器的稳定边界

src/slic3r/GUI/AI/
  SmartSlicing/  wxWidgets 工作台、ViewModel 和中文证据投影
  Orca/          Orca 上下文捕获、原生候选、隔离试切和正式网关
```

核心数据流：

```text
正式工作区只读捕获
  -> WorkspaceRevision
  -> PrintabilityReport
  -> 最多两个不可变替代候选
  -> 候选配置副本中的顺序试切
  -> 按目标命名的字典序证据比较
  -> 用户选择与一次确认
  -> OfficialSliceGateway 重新校验
  -> 一个 Orca Undo 事务
  -> 官方 Slice plate / Preview
```

候选、参数补丁和试切结果在用户确认前均保持隔离。正式 Model、`DynamicPrintConfig` 和正式切片结果只允许由 `OfficialSliceGateway` 修改。

## 已具备的产品能力

### 1. 完整受控工作流

- 捕获当前 Plate、模型、材料、喷嘴、有效配置和多色上下文。
- 使用 `WorkspaceRevision` 绑定模型、配置、Plate 和正式切片状态。
- 工作区变化后将旧检查和旧候选标为 `Stale`，禁止继续应用。
- 输出结构化 `PrintabilityReport`，区分阻塞项、可接受风险项和普通提示。
- 基线加最多两个替代方案，按固定顺序隔离试切，避免候选之间共享可变状态。
- 支持取消、预算超时、失败隔离、无效证据排除和中断恢复。
- 选择候选后才允许确认应用；正式写入前再次校验修订和所有期望值。
- 一次应用对应一个原生 Undo 快照；失败结果会明确报告工作区是否已改变以及是否可撤销。

### 2. 四种优化目标

| 目标 | 当前确定性候选 |
| --- | --- |
| 稳定打印 | Orca 原生摆放、原生朝向，以及小底面/细高模型的保守自动 Brim 或 Brim 宽度建议 |
| 质量优先 | 在喷嘴和 Domain 边界内降低有效层高，例如 `0.20 -> 0.16` |
| 速度优先 | 在安全边界内提高有效层高，并用真实试切时间评价 |
| 节省材料 | Orca 原生摆放；符合条件的多色项目可生成工具执行顺序候选 |

本地建议器没有安全建议时返回空候选，原生确定性候选仍然可用。无效建议不会削弱回退路径。

### 3. 可解释候选比较

- 使用硬门槛加目标专属字典序证据，不使用不可解释的加权总分。
- 不完整试切、无效数值、不兼容槽位和退化映射在排序前即被排除。
- 缺失可选指标只产生 `more_complete_trial_evidence`，不会伪造更快、更省料或更稳定的幅度结论。
- 最终相同时只使用稳定候选 ID 打破平局，并显示确定性平局说明。
- GUI 显示候选意图、具体参数变化、真实时间/材料代价，以及能够证明的多色保持约束。

### 4. 参数安全边界

- 每个 `ParameterProposal` 必须有且只有一个意图。
- 最多四个参数项，必须属于同一个 Plate，当前只允许 `Plate/Process`。
- Domain、Orca 参数适配器、隔离试切和正式网关使用相同校验合同。
- 多项补丁先在本地克隆中完整准备；任一项失败时输出克隆和正式配置均保持不变。
- 原始工具序列键、冲刷矩阵/倍率、擦料塔、硬件、温度、流量、压力提前和校准参数均禁止通过通用参数补丁写入。

### 5. 多色工具顺序候选

多色候选只调整逻辑耗材执行顺序，并强制保持：

- 逻辑耗材 ID 集合；
- 逻辑到物理槽位映射；
- 模型、对象和特征的材料分配；
- 擦料塔状态；
- 层范围；
- 所有冲刷设置。

只有至少两个已用逻辑耗材、物理槽位兼容性明确、映射未退化、源序列是有效排列时才生成候选。试切和正式应用都会重复校验这些约束。

## 验证证据

### 自动测试

- Release 全量 `slic3rutils_tests`：168/168 test cases，1352 assertions。
- 智能切片总集：157 passed，1 个显式 Release benchmark skip，1245 assertions。
- 参数过滤器：19 test cases，264 assertions。
- 候选过滤器：34 test cases，195 assertions。
- Apply 过滤器：21 test cases，242 assertions。
- MultiFilament：4 test cases，25 assertions。
- Release 与 RelWithDebInfo 的以下目标均构建成功：
  - `slic3rutils_tests`
  - `fff_print_tests`
  - `OrcaSlicer`
  - `OrcaSlicer_app_gui`

Windows 构建应使用串行 MSBuild：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' `
  --build build-p0 --config Release `
  --target slic3rutils_tests fff_print_tests OrcaSlicer OrcaSlicer_app_gui -- /m:1
```

并行构建曾触发 MSVC PDB 的 C1090/C2471；这不是智能切片源代码错误。

### Windows GUI 验收

只使用了工作区本地应用：

```text
D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe
```

隔离数据目录：

```text
D:\Workspace\06_3DDY_smart_slicing\build-p0\smart-slicing-p2-gui-data
```

启动前确认没有其他 `orca-slicer.exe`，启动后核对了实际进程路径。已人工验证：

- 本地系统 profile 和测试 STL 可加载；
- 四种优化目标可见；
- 官方普通 `Slice plate` 和 Preview 正常；
- 正式切片改变修订后，旧检查立即失效并要求重检；
- 质量候选显示 `0.20 -> 0.16`，但正式 Process 和 Preview 在确认前仍保持基线；
- 选择候选后才启用“确认并应用”。

本次人工 GUI 验收没有点击正式应用。正式事务、一次 Undo、取消、异常路径和多色保持约束由定向自动测试覆盖。后续仍应补充复杂多色项目的人工 GUI 验收。

测试目录中偶尔出现既有 `info/nozzle_info.json` 解析日志和多材料 SupportSpots 警告；相关用例仍通过，且本批次没有修改这些共享切片实现。

## 相对 ADR-004 验收基线的变更

基线：`8b77ad5b2f424fd95e45f1e7a26c03961dff0a89`

- 增加参数意图、范围、方向、依赖关系和组合校验。
- 增加按目标运行的本地参数建议。
- 统一 Domain、试切和正式网关的参数校验。
- 将候选比较重构为命名的目标专属证据维度。
- 增加类型化多色工具顺序提案和约束校验。
- 增加 GUI 参数意图、具体差异和多色保持说明。
- 增加相应 Catch2 和 `fff_print` 覆盖。

本批次唯一修改的共享文件是 `src/slic3r/CMakeLists.txt`，仅增加四个源文件注册项。相对该基线没有修改 MainFrame、Plater、3MF/profile、依赖或默认切片实现。

## 尚未完成的能力

以下范围必须继续保持拒绝状态，不能在适配器中静默转成 Plate 参数：

1. **Object/Process**：需要稳定对象目标 DTO、对象配置副本、对象覆盖的修订哈希、多对象过期检测和一次 Undo 恢复合同。
2. **Volume/层范围**：需要明确的 Domain 目标和原生层范围身份，不能复用标量 `target_id`。
3. **Material/Filament**：需要物理槽位能力、校准就绪证据、持久化和撤销策略。
4. **直接冲刷、重映射和擦料塔优化**：需要新的 ADR；当前只允许保持约束的顺序搜索。
5. **完整自动修复和失败预测**：当前可打印性检查是安全门，不是完整网格修复、支撑诊断或碰撞预测系统。
6. **跨平台发布门禁**：Windows 已验证；macOS/Linux 仍需 CI。
7. **性能基线**：Release benchmark 仍是显式跳过项，需要独立、稳定、可重复的性能门禁。
8. **远程建议器**：`IParameterAdvisor` 已提供内部扩展点，但没有 endpoint、凭据、sidecar 或远程模型依赖。

## 建议的后续顺序

1. 先完成复杂多色 GUI、正式 Apply/Undo、取消、异常恢复、现有 3MF/profile 和可访问性回归，并建立 Release benchmark 基线。
2. 在可用 CI 环境完成 macOS/Linux 构建和测试门禁。
3. 若要扩展参数层级，先提交并审批新的 ADR，建议下一份 ADR 专门定义 `Object/Process` 稳定目标、修订和 Undo 合同。
4. Object/Process 完成后，再分别决策 Volume/层范围和 Material/Filament；不要一次开放全部范围。
5. 直接冲刷、颜色重映射、擦料塔或远程 sidecar 必须单独评审。

## 不可破坏的交接约束

- 不合并或反向移植 `codex/orca-integration-v2`。
- 不复制模型生成业务代码；智能切片继续在独立 Domain/Application/Ports/GUI/Orca 模块演进。
- 不改写已验收提交历史，只在当前 HEAD 后提交。
- 正式写入只经过 `OfficialSliceGateway`；确认前候选配置和试切结果必须隔离。
- MainFrame、Plater 和 CMake 等共享文件只做最薄接入，并单独说明原因。
- 不修改 3MF/profile 格式和原版默认行为。
- 不修改仓库根 `task_plan.md`、`findings.md`、`progress.md`。
- 未经明确授权不调用付费 API。若确需生成服务，优先 image2 GPT、其次 Tripo，但不得把模型生成业务带入本分支。
- GUI 只操作本工作区构建的应用，并使用隔离数据目录；必须核对进程路径。
- 未经用户确认，不允许集成线自动获取功能分支最新 HEAD。

## 下一次交付格式

交付前确保工作树干净且全部改动已提交，并报告：

- 完整 40 位 SHA；
- 相对本交接 SHA 的变更摘要；
- 修改过的共享文件及原因；
- 测试、Release/RelWithDebInfo 构建和 GUI 验收结果；
- 配置、依赖、内部/网络端口、产品数据目录及 3MF/profile 是否变化；
- macOS/Linux、性能和人工验收中仍未完成的门禁。
