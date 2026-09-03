# 发现与决策

## 当前基线

- 当前开发分支：`codex/model-generation-v2`
- 创建基线：`codex/orca-integration-v2`，提交 `808efe4401`
- 冻结旧分支：`codex/model-generation`，提交 `16f8ac0fb0`
- 集成提交 `616a063192` 明确记录 `Accepted-feature: 16f8ac0fb0`。
- 旧功能线和集成线没有共同祖先，因此不应直接双向 merge。

## 文档审计

- 旧功能线有大量未被集成线收录的逐阶段设计、实施和复核文档；全部恢复会让当前产品线混入过期要求。
- 三份根目录上下文文件包含最完整的长期决策、进度和路线图，因此以迁移时工作树状态冻结归档。
- 精选补充资料覆盖四色图片管线、Beta 1 路线、盲测、多视图校准、模块边界和供应商边界。
- 两份大型质量报告已经存在于集成线的小写 `docs/` 路径，且 Git blob 与旧分支版本完全一致，不需要复制。

## 分支工作流

- 功能开发从最新集成线创建新分支。
- 验收后把固定 SHA 集成到 `codex/orca-integration-v2`。
- 已完成的功能分支冻结或归档；下一轮从更新后的集成线重新创建功能分支。
- 不把集成线反向 merge 或快照移植到旧功能线。

## 风险提示

- 当前本地集成线 `808efe4401` 比远端 `origin/codex/orca-integration-v2` 的 `1fbcd10416` 多一个提交；多电脑协作前应先统一远端主线。
- 工作区仍有大量未跟踪的临时产物，提交时必须使用精确路径，不能直接 `git add -A`。

## 历史资料

- [模型生成 v1 历史上下文](Docs/history/model-generation-v1/README.md)

## 2026-09-03：六通道模型生成复核

- v2 基线已经包含 Orca 原生混色能力：混合耗材配置、CMYW/RYBW/材料列表配方推荐、子层或整层执行，以及 3MF 往返保存。
- 当前模型生成链路仍把可打印能力表示为槽位编号和 RGB 字符串，并把 compatible slots 截断到前四个；这无法区分物理通道和虚拟混色槽。
- C++ 面板与 Python sidecar 存在大量固定四元素结构；简单把常量 4 改为 6 只能得到六个离散色，不能表达过程混色。
- 当前 Orca 的产品级配方搜索和 UI 以每个目标色 2～3 个分量为主。六通道应作为物理颜色池；不同目标色可选不同稀疏配方，整个模型可共同使用全部六通道。
- `ModelImportRequest::auto_slice_after_import`、`ModelImportResult::slice_after_import`、面板开关、适配器打印配置写入和布尔导航回调共同构成模型生成触发切片的完整链路，需要一起移除。
- 目标边界：模型生成发布经校验的几何制品与颜色意图；Orca 工作区负责导入和持久化；智能切片分支负责后续切片决策。

## 2026-09-03：方案比较

| 方案 | 结论 | 原因 |
|------|------|------|
| 将所有四色常量直接改成六色 | 拒绝 | 仍会混淆物理通道、目标色和混色配方，后续继续返工 |
| 在 Python sidecar 复制一套 CMYK/六色求解器 | 拒绝 | 与 Orca 原生能力产生双实现和颜色结果漂移 |
| 类型化颜色契约 + OBJ/颜色意图清单 + Orca 适配层复用原生配方引擎 | 采用 | 保持模块边界、兼容旧制品，并能逐阶段交付 |

## 2026-09-03：自动切片删除边界

- `ModelGenerationPanel` 的导入设置区同时包含颜色模式和自动切片复选框；删除复选框后保留颜色模式，并把区标题从“导入与切片”改为“导入设置”。
- `ModelImportRequest` 和 `ModelImportResult` 中的两个切片布尔值可以直接删除；它们尚未形成持久化格式，不涉及 3MF/profile 迁移。
- `OrcaWorkspaceAdapter` 当前会在自动切片前关闭独立支撑层高和擦料塔；删除自动切片后，这段打印预设写入也应整体删除，避免模型导入改变打印参数。
- `MainFrame` 的导入完成回调目前用 `bool slice` 同时负责准备页导航和发起切片；应收窄成无参回调，只更新工作区并进入准备页。
- Sidebar 仍可显示导入、网格检查、颜色处理和摆放结果；切片/G-code 步骤统一保持等待，不从模型生成域启动。
- 原生契约回归位于 `tests/slic3rutils/test_ai_contracts.cpp`，可先用编译失败/测试变化驱动字段删除；架构预算由 `tools/ai/test_integration_guardrails.py` 和 `scripts/verify_ai_integration.py` 守护。
- 当前仓库已有可复用的 Visual Studio 构建树 `build/` 和 `build-ai-tests/`；实现阶段优先构建 `slic3rutils_tests`，再执行架构守卫和完整 Release 验证。

## 2026-09-03：自动切片移除结果

- 生产调用链已不再包含 `auto_slice_after_import`、`slice_after_import` 或“导入后自动切片”控件；导入完成回调只有更新工作区和进入 Prepare 两项职责。
- 模型生成导入不再修改 `independent_support_layer_height`、`enable_prime_tower` 等打印预设，也不再发出切片事件或写入 `slice_requested` 旅程事件。
- 旧模型库 JSON 中的自动切片键不会阻断加载；新代码忽略它们，并在模型再次导入时删除这些过期键。这保持了读取兼容，同时停止继续发布错误状态。
- 架构守卫新增仓库正例和包含旧字段、预设写入、切片事件的反例 fixture；全套 41 项 Python 守卫通过。
- 本机现有构建树缺少完整的捆绑 Python 3.12.13 开发运行时，无法可靠重新配置或链接完整原生测试；独立 MSVC 编译契约测试源成功，GUI 全量验证需在恢复标准构建环境后补做。
