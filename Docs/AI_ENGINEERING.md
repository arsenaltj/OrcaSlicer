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
| 人像语义环境与美颜分区 | [离线 CPU 环境与验证](../tools/ai/README_local_semantic_runtime.md) | 固定依赖与权重 → 原生真实推理/面对应 → GUI 保色编辑；颜色算法及实物效果分别验收 |
| 模型生成团队协作 | [团队 SOP](coordination/team-integration-sop.md) | 只在明确请求团队提交或集成时加载 |
| 打包/发布 | [release runbook](../release/README.md)、根 AGENTS 发布条款 | 当前操作涉及的准确产物、授权记录、检查脚本和 CI；导航不是授权来源 |
| 翻译 | [localization 规则](../localization/AGENTS.md) | 仅加载涉及的子项目资料 |

## 五模块算法入口（2026-10-05）

以下为本机实现入口；本机旧界面的改色/保存恢复、多色工程和服务离线切片已有真实 GUI 证据，共同 UX 快照、新保存入口及其完整旅程仍按主计划保留待验。默认算法保留迁移前行为，正式切片宿主另修复完成结果复用的交接误判。调优在对应目录和测试中完成，工程应用仍走现有适配器。

| 能力 | 可改目录与稳定数据边界 | 实现选择和实际消费者 |
|---|---|---|
| 人像/模型生成 | `AI/ModelGeneration/{ModelGenerationTypes,IModelGenerationService,ModelGenerationClient}.hpp`，供应商在 `tools/ai/*provider_gateway.py`；原 Options/JobStatus/回调保留 | GUI AIModelGenerationClient 保留 API，默认 SidecarModelGenerationService；可构造注入服务。GenerationOptions.provider 由原 Sidecar 路由，Task ID、确认、幂等和未知提交规则不变 |
| 颜色匹配 | `AI/ColorMatching`；只读面/区域/材料快照 → 未确认候选。边界、patch、质量评分在同目录；贴图管线复用原 libslic3r 数学 | Input.engine 与 TextureImportOptions.texture_color_engine；默认 region-matching / native-texture-color。3D美颜实际由 BeautyPuzzle 调用 AutomaticColorRegions 保留首次匹配的连续次要色块；传 false 可退回均色基线，已保存/手工区域不重分 |
| 3D 美颜 | `AI/AppearanceEditing`；来源/新目标路径和只读 Options → Result。共享读写与面身份在 `AI/ModelArtifacts` | ModelFinishingOptions.engine；默认 appearance-baseline。原三种 finish 入口都转发，接受/草稿/发布流程保留；改变拓扑不得复用旧面映射 |
| 自动摆盘 | `AI/Placement`；去掉 model setter 的轮廓、床/禁区/间距/方向 → 身份明确的变换 | ArrangeJob(engine) 与 OrcaPlacementCandidateInput.engine；默认 native-arrange。产品异步摆盘与智能候选复用；finalize/Undo 留 Orca，候选仍限制当前盘 |
| 智能切片 | `AI/SmartSlicing`；WorkspaceContext → 参数建议、试切指标 → 排序；BrimParameterAdvisor 保留原规则 | OrcaSmartSlicingAdapter 的 IParameterAdvisor 实际生成参数候选；协调器构造时捕获 CandidateScoringStrategy，默认 candidate-comparison。评分仍受可用性/物理槽位/身份守卫限制，正式切片与撤销不变 |

表中 C++ 目录均位于 `src/slic3r/`。每个任务固定实现；算法 ID/版本进入候选或返回诊断，生成的供应商身份独立保留。颜色/美颜/摆盘清空实现指针即回基线，生成使用默认 endpoint 构造，评分使用默认空函数策略。算法切换不重写历史资产；首轮不做动态加载，新实现必须满足约束并验证实际消费者。

五个调优任务各有自己的可执行测试目标，`dev.ps1` 同时按目标前缀和标签筛选，避免运行另一个模块或旧桌面测试。生成与智能切片策略目标不依赖网格内核；颜色、美颜、摆盘保留必要的原生几何/格式依赖。均不链接 wxWidgets/libslic3r_gui。

| 调优任务与交接说明 | 单独构建、验证命令（仓库根） |
|---|---|
| [人像/模型生成](../src/slic3r/AI/ModelGeneration/README.md) | `./dev.ps1 CppTest -TestSuite ai_generation_tests -TestLabel '.*' -Jobs 2` |
| [颜色匹配](../src/slic3r/AI/ColorMatching/README.md) | `./dev.ps1 CppTest -TestSuite ai_color_matching_tests -TestLabel '.*' -Jobs 2` |
| [3D 美颜](../src/slic3r/AI/AppearanceEditing/README.md) | `./dev.ps1 CppTest -TestSuite ai_appearance_tests -TestLabel '.*' -Jobs 2` |
| [自动摆盘](../src/slic3r/AI/Placement/README.md) | `./dev.ps1 CppTest -TestSuite ai_placement_tests -TestLabel '.*' -Jobs 2` |
| [智能切片策略](../src/slic3r/AI/SmartSlicing/README.md) | `./dev.ps1 CppTest -TestSuite ai_slicing_strategy_tests -TestLabel '.*' -Jobs 2` |

以下聚合目标保留跨模块原生候选样例及原命令兼容；改变模块间几何/工艺语义时加跑受影响标签：

```powershell
./dev.ps1 CppTest -TestSuite ai_capabilities_tests -TestLabel 'ColorMatchingEngine|TextureColorEngine|LocalPrintColorBoundary|AppearanceEngine|BeautyAppearance|ModelFinishing|GenerationService|PlacementEngine|SlicingStrategy|SmartSlicing'
```

可直接复用的输入/输出样例，均来自 `tests/ai_capabilities` 中的实际接口测试；替换测试使用记录调用的实现，不冒充生产算法效果：

| 能力与样例文件 | 输入 | 输出与不可破坏的约束 |
|---|---|---|
| 生成：`test_generation_service.cpp` | 已有 `existing-job`、确认过的 options 与 prompt；另测查询已有任务及下载 GLB | 原 Job ID/provider/参数传递，回调带服务 ID/版本；查询恢复不增加提交，本地取消与远端停止分开 |
| 颜色：`test_color_matching_engine.cpp` | 两面红/蓝，面积 9/1；实际槽位 3/11；红区锁定到 3；材料/工艺及几何身份 | 两面分别映射 3/11，`region-direct-v5`、confirmed=false；非连续槽位不能重新编号，几何面数不匹配拒绝 |
| 美颜：`test_appearance_engine.cpp` | 一张三角形 OBJ 或含重复面的 OBJ；来源/独立目标路径；smooth_surface=false | 新候选、来源 SHA 不变；去重复面后 preserves_face_order=false，拓扑不变才可保留对应；取消不创建目标 |
| 摆盘：`test_placement_engine.cpp` | 实例 ID 7、selected/fixed/禁区快照及带 setter 的原生消费者输入；注入记录策略 | 策略拿不到任何 live setter；只返回 ID 7 的变换，应用由消费者显式执行；未知 ID/取消不改输入变换，坐标沿用内核单位 |
| 智能切片：`test_smart_slicing_strategy.cpp` | plate ID 17，brim=2mm，实例尺寸 20×20×10 / 4×6×10mm；另有当前 revision 和试切指标 | 基线建议 brim 2→5mm，context 不变；替换评分器可返回合法候选排序，失败/物理槽位不兼容/过期候选不能绕过守卫 |

调优任务只改该模块实现/参数和对应样本，先比较原 baseline；字段或几何依赖变更按主计划重新核验下游。测试中的 300000 face_limit 只是 DTO 传递样本，不代表授权新生成或已有质量结论。不要为一个效果优化任务重新编辑页面、Plater 或正式工程保存格式。

分配单模块任务时可直接附上相应 README，并使用以下指令：

```text
阅读该模块 README 和适用 AGENTS，在其中登记的实现/参数及测试范围内优化指定效果。
先固定输入与 baseline，记录输入身份、算法版本、效果指标和耗时；保留原 baseline 的装配方式。
保持现有接口语义、原件/历史、取消和候选身份守卫，跑该模块独立命令并修正新增失败。
公共类型或几何/工艺语义改变时，明确影响的下游与相应组合回归，由统一接线任务修改共享契约/适配器。
交付具体改动、同输入对照、验证证据和未验范围。页面布局由 UX 任务继续推进。
```

五个任务可在不同 checkout/构建目录并行调优；同一 `.tmp/dev/build` 仍只有一个写者。模块共同 CMake、Contracts、ModelArtifacts 和 GUI 接线由一个集成任务维护。UX 完成后只对实际新版消费者补集成核验，不作为开始算法调优的前置。

生成离线网关用 `test_model_provider_gateway.py`；桌面提交/恢复、工程应用和存储兼容仍在 slic3rutils_tests 中按对应标签验证。旧 GUI 头文件和客户端保留转发，中性模块禁止依赖 GUI/供应商 SDK。安装配置在 `cmake/OrcaWindowsAIRuntime.cmake`，宿主绑定在 `GUI/AI/{MainFrameAIWorkflow,SidebarAIWorkflow,PlaterSmartSlicingWorkflow}.ipp`；来源和依赖门禁同样检查被委托的片段。

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
