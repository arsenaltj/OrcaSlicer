# 快速开发与跨电脑同步

默认单 Agent 完成“定位 → 改动 → 最小充分验证 → 检查 diff → 结束”。逻辑优先自动测试，界面行为检查受影响的真实主窗口；跨模块、共享接口、凭据、付费、上传、删除等按风险升级，不按文件数量判断。完整规则见[根 AGENTS](../../AGENTS.md)。

## 日常入口

在当前 Windows checkout 的 PowerShell 中执行：

| 需要做什么 | 命令 |
| --- | --- |
| 首次配置独立开发构建目录 | `./dev.ps1 Setup` |
| Python 小改，只跑对应离线测试 | `./dev.ps1 Test -TestPattern test_<模块>.py` |
| C++ 小改，编译相关组并筛选标签 | `./dev.ps1 CppTest -TestSuite slic3rutils_tests -TestLabel ModelPreviewPalette` |
| 无原生构建环境，只跑离线测试 | `python scripts/run_ai_offline_tests.py --pattern test_smoke_model_generation.py` |
| 增量编译 | `./dev.ps1 Build` |
| 编译并打开软件 | `./dev.ps1` |
| 只改 Python，刷新并打开 | `./dev.ps1 Sidecar` |
| 准备运行文件，暂不启动 | `./dev.ps1 -NoLaunch` |
| 只读检查源码归属、工具链和配置 | `./dev.ps1 Check` |

默认固定使用 `.tmp/dev/build` 的 Release 配置，运行和测试配置分别为 `.tmp/dev/run`、`.tmp/dev/data`。其他已有目录用 `-BuildDir <目录>`，必要时用 `-Configure`，并行度用 `-Jobs 2`。每台电脑使用自己的源码、依赖和构建目录，不复制 CMakeCache、对象文件、PDB 或运行记录。

本机已登记忽略的 `.tmp/dev/settings.json`，仅包含 `python`、`cmake`、`deps_prefix` 三个本机工具路径；不进入 Git，不放凭据。另一台电脑可保存自己的路径，或首次显式执行 `./dev.ps1 Setup -PythonPath <Python3.12+路径> -CMakePath <cmake.exe路径> -DepsPrefix <已安装依赖前缀>`。Setup 使用 Visual Studio 2022 x64，开启测试和内部运行文件安装，只配置不编译；已有 cache 会核对源码归属并重新配置，也可恢复未完成的配置。第三方 C++ 依赖使用已有前缀；CMake 若缺少固定 Pillow wheel 会尝试从官方 PyPI 文件站下载。本机已复用经哈希验证的已有 wheel，无需下载。Setup 后使用同一参数或保存本机设置。旧[构建存储说明](local-build-storage.md)来自另一台机器，不直接照抄路径。

`Test` 必须指定文件名或 glob，复用现有离线测试器的凭据清理与 Python 外网防护；只需要 Python，不要求 CMake cache，不编译、不复制运行环境、不启动 GUI。测试失败或未匹配均失败。此防护不是 OS 沙箱，测试仍须实际隔离提供商。

C++ 逻辑修改增量构建对应 `<suite>_tests`，再按 CTest 标签或名称选择测试，见 [tests/AGENTS.md](../../tests/AGENTS.md)。纯文档检查内容、链接和 diff。检查通过后，只有相关源码、配置、依赖、测试条件变化或新失败证据才重验。

`Run` 增量编译有变化的翻译目录，构建 `OrcaSlicer_app_gui` 及依赖，通过 CMake install 准备完整运行文件，不生成安装包。复用 `.tmp/dev/run` 和独立配置 `.tmp/dev/data`；更新前正常关闭该试用实例，保留其他实例和用户工程。构建期间不要改源码。`Sidecar` 刷新安装清单中的 Python 模块（包括当前语义处理组件）；原生源码、翻译、资源、安装清单、配置或二进制变化时要求先运行 `Run`。安装资源及依赖 DLL 哈希变化也会拒绝快捷刷新。首次完整原生构建较慢，此后复用自己的增量构建。

入口核对构建目录的源码归属，以本地锁防止并发写入，并在构建/安装后核对源码和运行文件。手工构建也须遵守同一目录单写者约定。启动前检查本机 18764 端口；`Build` 和 `-NoLaunch` 不启动实例。端口占用、缺少运行文件和 SDK 访问错误须按原始输出诊断，脚本失败本身不等于平台审批拒绝。

完整日志保存在 `.tmp/dev/logs/<时间>/`，`result.json` 记录步骤、耗时和退出码；正常返回摘要，失败显示相关日志片段。运行身份保存在 `.tmp/dev/runtime-state.json`。`Check` 只证明前置条件，`STARTED` 只证明进程启动，都不代表功能验收通过。

实际验收走真实主窗口及真实服务，不准备独立 mock 服务。离线测试中的 fixture 保留。查看、编辑、导入和恢复复用软件历史中的已有真实资产；新生成沿用明确的输入、接收服务和剩余预算，不能因重试或换电脑重置额度。明确的平台拒绝保留原始理由并走正式复核，不能换脚本或任务执行同一动作。

## 两台电脑使用同一套规则

先保存另一台电脑的未提交工作，再拉取包含本次改动的分支。PR 合入前需拉取 PR 分支；合入后按[团队 SOP](team-integration-sop.md)同步集成。不要用 reset/clean 覆盖本机未完成工作。

项目级 Skill 随 Git 一起同步：

- [code](../../.agents/skills/code/SKILL.md)：日常实现和相关验证。
- [planning-with-files-zh](../../.agents/skills/planning-with-files-zh/SKILL.md)：跨阶段、需恢复上下文的复杂任务；小改不触发。
- [model-generation-evaluation](../../.agents/skills/model-generation-evaluation/SKILL.md)：已有模型或冻结批次的质量评估。

同名个人 Skill 不叠加使用，优先本项目版本。同步 Skill 不会同步聊天、未提交任务进度、密钥、模型配置、插件安装状态或编译缓存；需要交接的非敏感任务记录应单独纳入明确提交范围。个人 Skill 的删除、Ponytail 卸载等需在每台电脑分别处理，不通过仓库脚本修改用户目录。

独立验收按[交接流程](validation-handoff.md)执行；内部打包使用 `release/build_internal.ps1 -SourceManifest <manifest.json>`，远程提交按团队 SOP。只有明确要求这些动作时才执行。

## 每轮只解决一个可见问题

从[当前状态](../../docs/coordination/current-development.md)恢复上下文，指定一个固定样本、预期效果和相关验证范围。默认单 Agent / Astra medium，明确的小改用 low，几何/切片/状态排错用 high；不默认 max/ultra。模型选择由客户端控制，项目文字不会切换活动模型。

先交付可判断效果的试验，方向确认后补齐受影响的稳定性收尾。导入、保存、撤销、切片变化仍验证对应行为；Windows 之外的平台不进入当前本地迭代。独立 Agent 只用于可并行的审查或明确独立子任务，不同时写同一构建目录或操作同一窗口。只在阶段交付时冻结新快照；保留 R101 对照资产，不每轮复制完整运行环境。

每次运行的 `result.json` 自动记录步骤耗时和退出码；比较从提出想法到可见效果的时间、失败/返工次数。当前状态只保留目标、版本、结果、缺口、证据路径；历史大报告不再重复追加当前摘要。已确认的阶段成果可形成明确范围的本地提交，不自动推送。

## 已有 3D 模型离线回归（B.1）

`python scripts/run_beauty_model_regression.py --manifest <私有清单.json> --executable <slic3rutils_tests.exe> --output <全新结果目录>`。清单格式见脚本说明；仅支持 GLB 与自包含顶点色 OBJ，模型路径和真实资产留在忽略目录。记录程序指纹，不自动认证源码版本；固定色板来自原生 BeautyTargetModelProbe，清单条件是证据说明而非修改算法参数。失败、缺报告、哈希不符均非通过，已有目录不覆盖。

脚本回归：`python -m unittest discover -s scripts -p test_beauty_model_regression.py -v`。真实模型结果只证明两条配色路径一致，不证明未见泛化、视觉或实物颜色；任务状态只更新主计划 B.1。
