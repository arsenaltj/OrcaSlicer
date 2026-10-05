# AI 工程导航

本项目在 OrcaSlicer 上增加模型生成和智能切片。目标是将文字/图片转为可检查、可导入的彩色打印模型，并由 Orca 管理打印配置、切片和预览。先按任务选择下表入口，不必通读所有历史报告或整个 Orca 仓库。

## 有效入口

- [短状态](../docs/coordination/current-development.md)指向唯一[主计划](plans/2026-09-25-product-convergence-and-lightweight-plan.md)；实现以注明版本的源码/测试为准。
- [快速开发](coordination/quick-development.md)：本机独立增量构建和验证；机器路径以 checkout 的忽略配置为准，不照抄历史 `D:/Workspace/11_3DDY_Continue` 或其他机器路径。
- [架构评审](coordination/architecture-review.md)、[集成锁](../docs/architecture/ai-integration-lock.json)、[打印与颜色边界](domain/printing-color-boundaries.md)：按修改范围读取。Accepted 设计不代表已实现/验收。
- [团队 SOP](coordination/team-integration-sop.md)仅用于明确请求的远程协作；[交接流程](coordination/validation-handoff.md)仅用于相应交付。
- [配色历史证据](plans/model-coloring/README.md)和 `Docs/audits/`、`Docs/history/` 保留版本结论，不提供另一套当前待办。[续建来源](../CONTINUATION.md)解释已退役入口的 Git 追溯方法。

`Docs/` 与 `docs/` 有历史 Git 大小写差异，按目标实际拼写链接，不批量改名。忽略目录中的模型、用户资产、运行实例及证据不因文档清理而删除。

## 按任务定位

| 任务 | 先读 | 再沿调用链阅读 |
|---|---|---|
| 生成界面/结果交互 | [ModelGeneration FeatureHost](../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp) | `ModelGenerationPanel.cpp`、`ModelGenerationPresentation*`、`ModelGenerationArtifactFlow.cpp`、`AIModelGenerationClient.cpp` |
| Provider/预处理/质量 | [Gateway](../tools/ai/model_provider_gateway.py) | `tripo_client.py`、`openai_preprocessor.py`、Sidecar 中对应 job/下载/质量函数 |
| 导入与颜色保真 | [导入契约](../src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp)、[ColorIntent](../src/slic3r/AI/Contracts/ColorIntent.hpp) | [OrcaWorkspaceAdapter](../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)、OBJ/Model/ObjColorUtils；查消费者是否实际使用元数据 |
| 智能切片 | [SmartSlicingCoordinator](../src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp) | `AI/SmartSlicing/Domain`、`Ports`、`GUI/AI/SmartSlicing`、`GUI/AI/Orca` |
| 原生尺寸/底座与 AI 连续流程 | [原生 UX 审查、修改前后与回归表](audits/2026-09-08-native-ai-workflow-ux.md) | `GUI/AI/Orca/OrcaModelPreparation*`；几何计算先于原生快照提交，既有涂色和原件保留；实际 GUI 验收与离线引擎测试分开记录 |
| 桌面组合/运行时 | [AIDesktopFeatureHost](../src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp)、集成锁的 ownership | MainFrame/Plater、AIServiceManager/AISidecarClient、network_policy；共享入口需遵守既有集成所有权 |
| 原生切片/配方 | [Print](../src/libslic3r/Print.cpp)、[ColorDecomposeRecipe](../src/libslic3r/ColorDecomposeRecipe.hpp) | PrintConfig、GCode、ToolOrdering、Format；先与锁定基线对照，区分继承与本地增量 |
| 已有模型质量复评 | [model-generation-evaluation Skill](../.agents/skills/model-generation-evaluation/SKILL.md) | 按 SOP 读取已有模型、报告和对应测试 |
| 模型生成团队协作 | [团队 SOP](coordination/team-integration-sop.md) | 只在明确请求团队提交或集成时加载 |
| 打包/发布 | [release runbook](../release/README.md)、根 AGENTS 发布条款 | 当前操作涉及的准确产物、授权记录、检查脚本和 CI；导航不是授权来源 |
| 翻译 | [localization 规则](../localization/AGENTS.md) | 仅加载涉及的子项目资料 |

## 选择验证范围

命令从仓库根执行，使用已有可用 Python 和构建环境。按实际改动选择，不要求每个文档任务运行全量构建。

| 改动 | 验证入口 |
|---|---|
| AI 架构/契约/运行版本 | `python scripts/verify_ai_integration.py --json`，保留 Git 检查；缺历史对象应报告，不能把 skip-git 的结果称为完整验证 |
| 单个 Python 模块 | `./dev.ps1 Test -TestPattern test_<对应模块>.py`，或 `python scripts/run_ai_offline_tests.py --pattern test_<对应模块>.py`；确认测试已 mock 提供商 |
| AI 集成 Python 回归（影响集成时） | `python scripts/run_ai_offline_tests.py`；保留离线防护，真实生成脚本不属于该命令的替代品 |
| C++ AI DTO/面板/智能切片 | `slic3rutils_tests`；按 [tests/AGENTS.md](../tests/AGENTS.md) 配置、构建和运行，Windows 需 `-C Release` |
| 网格/格式/颜色数据 | `libslic3r_tests` 中对应测试；产生切片/G-code 的行为用 `fff_print_tests` |
| GUI/导入/组合流程 | 使用确定的可执行文件、datadir、端口和模型 SHA；核实导入无隐式切片/配置改变，以及 AI 关闭/离线时普通 Orca 流程 |
| 团队内部包与实际验收 | `release/build_internal.ps1 -SourceManifest <handoff/manifest.json>`；完整主程序、EXE/ZIP、3MF/profile 和真实主窗口证据。无需先推送或提交；当前不做正式发布验收。 |

## 维护导航

新架构决策继续放已有 architecture/ADR 体系；通用知识不写成 Skill。只有反复发生且可明确输入、步骤、输出的一类任务才新增 SOP。历史 phase/plans/proposal 按需读取，不当作当前完成状态。

已有 Git 路径混用 `Docs/` 与 `docs/`；新链接按目标的 Git 拼写定位。跨平台归一化应另立可验证改动，不顺手批量重命名。不要将用户照片、生成大文件、包或供应商配置复制进导航或 Skill。
