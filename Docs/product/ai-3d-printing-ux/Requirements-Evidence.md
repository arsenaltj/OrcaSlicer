# 需求、Figma、代码与测试证据

**观察基线：** `codex/team/integration` / `19a0b7f05e96c304b0a1e333a61d67643cb2e21b`，2026-09-28 静态核对。Figma 节点来自 `6pro/OrcaSlicer_Requirements_Evidence_v1.md` 的交接映射；未在本次给 Figma 固定版本或逐控件复核。代码链接表示可定位的实现，**不自动证明 GUI 已打通、硬件已连接或实物达标**。较新交接稿的 `a6d3743…` 不能充当本集成快照的测试结果。

状态标签与 [PRD §7](PRD.md#7-功能需求) 一致。表中测试文件是已有定位入口；“测试关注点”是下一次验收要观察的行为，不表示本轮已执行。

| 需求 / 状态 | Figma 节点（设计意图） | 集成分支代码模块（实现证据） | 测试关注点 |
|---|---|---|---|
| F01 图片/文字入口 **[部分实现]** | [设计输入 215:28535](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-28535) | [ModelGenerationPanel](../../../src/slic3r/GUI/ModelGenerationPanel.cpp)、[提交链](../../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationSubmission.cpp) | 图片/文字分支、替换输入、服务失败保留草稿；Figma 三入口未当作实现 |
| F02 付费确认与同任务恢复 **[已实现]** | [生成确认 215:28757](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-28757) | [ModelGenerationSubmission.cpp](../../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationSubmission.cpp) | 确认前零付费请求；响应中断同 ID 查询、不二次提交；本地停止语义 |
| F03 2D 设计确认 **[部分实现]** | [设计方案 215:28535](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-28535)、[版本返回 394:15108](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=394-15108) | [ModelGenerationPanel](../../../src/slic3r/GUI/ModelGenerationPanel.cpp)、[产物流](../../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationArtifactFlow.cpp) | 确认正确设计、旧版标记、2D 不满意返回 |
| F04 生成产物交接 **[已实现]** | [3D 模型 215:28918](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-28918) | [GeneratedModelArtifact](../../../src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp)、[IModelArtifactConsumer](../../../src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp)、[模型产物测试](../../../tests/slic3rutils/test_model_artifact.cpp) | job/格式/颜色意图身份，原文件不变；生成产物不等于 3MF 工程 |
| F04 历史作品 **[部分实现]** | [模型库 215:27531](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-27531) | [ModelGenerationPanel](../../../src/slic3r/GUI/ModelGenerationPanel.cpp)、[BeautyWorkbenchControls](../../../src/slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.cpp) | 原件/派生版、旧记录、缺失文件、跨进程打开 |
| F05 3D 美颜 **[部分实现]** | [美颜模块 145:15087](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=145-15087) | [ModelGenerationBeautyView](../../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationBeautyView.cpp) → [BeautyWorkbenchControls](../../../src/slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.cpp) → [BeautyDocument](../../../src/slic3r/GUI/AI/Model/BeautyDocument.hpp) | 保存/撤销/历史恢复、跳过、取消保存、失败保留原件；参照 [beauty 测试](../../../tests/slic3rutils/test_beauty_appearance.cpp) |
| F05 区域与人工优先 **[部分实现]** | [局部修改 145:15087](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=145-15087) | [BeautyPuzzle](../../../src/slic3r/GUI/AI/Model/BeautyPuzzle.hpp)、[区域编辑](../../../src/slic3r/GUI/AI/Model/VertexColorRegionEditor.cpp) | 错选修正、边界、迟到识别不覆盖手动决定；复杂组合待验 |
| F06 目标色与材料能力 **[部分实现]** | [配色 215:28918](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-28918) | [ColorIntent](../../../src/slic3r/AI/Contracts/ColorIntent.hpp)、[IPrintablePaletteProvider](../../../src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp) | 目标色 ≠ 物理槽位；1–6 通道、材料/工艺指纹与失效 |
| F06 可执行颜色匹配 **[部分实现]** | [配色结果 145:16593](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=145-16593) | [LocalPrintColorPanel](../../../src/slic3r/GUI/AI/ColorMatching/LocalPrintColorPanel.cpp)、[应用边界](../../../src/slic3r/GUI/AI/Orca/LocalPrintColorCommit.hpp)、[匹配测试](../../../tests/slic3rutils/test_local_print_color_matching.cpp) | 未分配/recipe 不可用/锁色失效要阻断；预览色不证明实物色差 |
| F07 Orca Prepare **[已实现]** | [打印准备 215:29515](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=215-29515) | [OrcaWorkspaceAdapter](../../../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)、[MainFrame](../../../src/slic3r/GUI/MainFrame.cpp) | 导入后 Prepare、原生预设与 3MF；不静默切片/改预设；[导入测试](../../../tests/slic3rutils/test_model_color_import.cpp) |
| F08 智能切片建议 **[部分实现]** | [方案比较 315:10184](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=315-10184) | [SmartSlicingPanel](../../../src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.cpp) → [SmartSlicingCoordinator](../../../src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp) | 候选比较、revision 过期、显式应用/撤销；[协调器测试](../../../tests/slic3rutils/test_smart_slicing_coordinator.cpp) |
| F08 正式切片新鲜度 **[部分实现]** | [切片/确认 145:16144](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=145-16144) | [WorkspaceRevision](../../../src/slic3r/AI/SmartSlicing/Domain/WorkspaceRevision.hpp)、[ApplyWorkflow](../../../src/slic3r/AI/SmartSlicing/Application/ApplyWorkflow.hpp) | 模型/配置/材料变化后旧 G-code 不可当本次结果；[应用门控测试](../../../tests/slic3rutils/test_smart_slicing_apply_guard.cpp) |
| F09 设备与监控 **[部分实现]** | [监控 120:9467](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=120-9467)、[完成 128:10102](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=128-10102) | [MainFrame 原生 Monitor/PrinterWebView](../../../src/slic3r/GUI/MainFrame.cpp) | 指定设备/作业、真实时间戳、启动/停止/失败/完成；未做硬件验收 |
| F10 新 UX 状态 **[规划]** | [框架 21:77](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=21-77)、[异常 394:14730](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=394-14730) | 无可确认的完整新 UX 对应模块；现有面板不可代替整套设计 | 空白、加载、无网、错误、窄窗/DPI、返回路径逐状态验收 |
| F11 实物颜色保障 **[规划]** | [打印配色 388:13392](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=388-13392) | [打印颜色边界](../../domain/printing-color-boundaries.md)；契约与预览不足以证明实物 | 固定耗材/工艺/机型与色卡、真实打印、色差及主观关键区域评价 |

## 关键边界与待验证能力

1. **Figma → UI：** Figma 文案和节点是目标；集成快照的 `ModelGenerationPanel`/`BeautyWorkbenchControls` 证明部分实现，不能推出新导航、所有状态或所有按钮已可用。
2. **生成 → Orca：** `GeneratedModelArtifact` 携带本地路径、job、格式、颜色意图，不拥有 Orca 项目和预设。`OrcaWorkspaceAdapter` 负责导入边界。[ADR-006](../../architecture/ADR-006-six-channel-model-color-intent.md) 说明该拆分。
3. **美颜 → 配色：** `BeautyPuzzle` 中用户目标色、区域与物理材料映射要分别审查；保存、撤销和历史恢复有代码/测试入口，组合 GUI 仍需实际验证。
4. **智能切片 → 设备：** `SmartSlicingCoordinator` 处理候选与 revision，Orca 执行正式切片；设备启动与监控由实际连接决定。切片通过不能替代实体成品评价。[ADR-002](../../architecture/ADR-002-smart-slicing-transactional-workbench.md) 是架构边界。

## 来源与维护

- 需求整理自 `6pro/OrcaSlicer_PRD_v1.md`、`OrcaSlicer_User_Journey_v1.md`、`OrcaSlicer_Requirements_Evidence_v1.md`，并对本集成分支重核代码路径。
- 设计原始入口为 [Figma 21:77](https://www.figma.com/design/EDEWtTOoWCBYt1FAezUArZ/?node-id=21-77)；节点是活动链接，当前未固定设计快照。
- 本文不新增进度台账。业务实现与验收状态随负责分支/集成提交变化时，应重新核对标签、链接和测试证据。
