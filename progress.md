# 进度日志

# 2026-08-13 阶段 34：Windows AI 同事测试包

- 用户反馈 Windows 本地无法顺利解压最终包。只读诊断确认 ZIP 头、SHA256、`tar -tf` 和 .NET ZipArchive 均正常；根因是 Windows `Expand-Archive`/资源管理器处理 15,648 个条目时 5 分钟仍未完成，而不是压缩包损坏。
- 新增 `01-extract-package.bat`，使用 Windows 自带 `tar.exe` 解压内部包到 `%USERPROFILE%\OrcaAI-demo3`；实际验证 14 秒解出 15,100 个文件、325.1 MB，正式启动入口存在。
- 最终对外文件改为 `output/packages/OrcaAI-demo3-delivery.zip`：外层仅 4 个条目，Windows `Expand-Archive` 实测 1.27 秒解开；测试者随后双击一键脚本展开内部资源。
- 外层交付 ZIP SHA256 为 `8B4DB8A7D4B031DA11789C3472DDD6648B3042F1D1CF6D8AF3B907BAE479E1DC`；内部 ZIP 校验一致。
- 根据用户试解压反馈，将配置方式从交互式 PowerShell/用户环境变量简化为 `setup/ai-config.bat` 文件；同事只填写 `OPENAI_API_KEY` 与 `TRIPO_API_KEY` 两行，检查、启动和 sidecar 都自动加载该文件。
- 环境检查现在只认当前进程中由该文件加载的 Key，不再回退到 Windows 用户环境变量；空配置会明确失败，两个假 Key 的离线检查通过。
- 打包脚本改为压缩包根目录直接包含入口文件，不再额外包一层同名目录；最终简化包为 `OrcaSlicer-AI-Windows-x64-20260813-demo3-simple-config.zip`，155,652,885 字节，SHA256 `3E9DFCDAF875D43524D5DD99F1DEF24093B1EB63063DDEBB49D72B784E655EAB`。
- 用户当前手动解压的 demo2 内层目录也已同步 `setup/ai-config.bat` 和新启动/检查脚本，可立即使用；旧交互式 `Configure-AI.ps1` 已删除。
- 生成完整 Windows x64 便携测试包，而不是不可独立运行的裸 `orca-slicer.exe`；包内包含正式 Orca 运行时、resources、正式 AI sidecar 和配置/检查/启动/停止入口。
- 新增 `packaging/windows-ai-test` 模板和 `scripts/package_windows_ai_test.ps1`；配置脚本交互采集同事自己的 OpenAI-compatible/Tripo 配置并写入当前用户环境变量，包内不保存真实 Key。
- 正式 AI 运行文件仅保留 `orca_ai_sidecar.py`、`openai_preprocessor.py`、`tripo_client.py` 和 4 个生命周期脚本；旧 DLL、Mock、测试、付费验证脚本、历史模型、`.git/.claude/.codex-recovery`、include/lib 均未进入包。
- 最终包 `output/packages/OrcaSlicer-AI-Windows-x64-20260813-demo2.zip` 为 156,874,911 字节，SHA256 `6DACF1300E3BB906AEC05BDC2686F61EA6F5248A7DCBEAE6412A2657366D981A`；独立校验文件一致。
- 包内 `OrcaSlicer.dll` 与正式 Release DLL 哈希一致：`BB5B51E39D2AB0642E2EE6A784D4E26A4A67C36DD5B29B24010AFB0E7035BA6C`。
- 依赖检查通过 Python 3.12.10、Pillow、两个 HTTPS Base URL、两个隐藏 Key 和默认模型配置；6 个批处理均为 CRLF。
- 在隔离端口 18766 启动包内 sidecar，`/health` 返回 v4、protocol 1、text/image、OBJ 和 `https://laotie.dev`；随后按 PID 停止，端口释放且 `generated_models` 为空，未创建任何付费任务。
- **状态：** complete

## 2026-08-11 阶段 26：真实彩色 OBJ 保色闭合修复
- 用户截图确认 14,480 面、15 色真实 OBJ 可正常预览，但确认生成 G-code 后返回 `mesh still open after hole filling`，未开始切片。
- 已确定优先增强本地确定性保色修复；不重新付费生成，不绕过开放网格/非流形门禁。
- 已定位任务 `4ae4d7e9-f511-4c39-8e93-fd181698eb70`：单连通、7,233 顶点、14,480 面，23 条边界边、3 条非流形边；sidecar 未修改拓扑并将 26 条异常边交给 Orca CGAL，最终闭合失败。
- 已确认 sidecar 将三个相距较远的小缺陷合并计算包围盒，20.5% 的全局跨度误触发 5% 阈值；实际只需删除 8 面，删面后 7 个边界组件最大约 1.79 mm。
- 已确定按共享顶点对缺陷面分簇并逐簇执行原 5% 安全校验，同时保留全部现有拓扑与颜色门禁。
- 已实现分簇校验并增加“两处相距较远的小型非流形缺陷”测试；新旧两个定向用例通过。
- 真实验证副本修复为 14,502 面、单连通、0 异常边，尺寸边界和 29 个原始顶点色值全部保持不变。
- 首次离线调用因未设置 `tools/ai` 模块路径而失败，显式设置项目 `PYTHONPATH` 后成功；首次前后对比误对原始开放网格使用严格校验，改为修复前诊断模式、修复后严格模式。
- AI Python 全量回归 68/68 与语法检查通过；修复副本已部署到任务规范 OBJ 和 downloads 副本，原文件保存在任务内 `phase26-backups`。
- 重启正式 sidecar 后首次 GUI 重试返回 `Model job not found`，确认 C++ 无条件重新下载而未复用本地 OBJ；已改为本地产物优先及历史模型显式确认导入。
- `download_and_import()` 现在接受 job ID 为空但已解析的本地 OBJ；当前路径和标准 downloads 路径均缺失时才访问 sidecar。远程任务删除也只在 job ID 非空时执行。
- 模型库双击现在恢复本地路径、OBJ/顶点色格式、ready 状态和 `use_printable_colors`；按钮在正式服务不可用但本地 OBJ 有效时仍可启用。
- Windows Release 构建成功；构建 DLL 已备份旧版本后部署到日常目录，构建与运行 SHA256 均为 `99709019EA4A0005920517772D4F071269529CECB063A675BC72CA2E35D73388`。
- 正式 GUI 从模型库双击任务 `4ae4d7e9...`，成功显示修复后的 14,502 面、15 色和 41.3 x 41.1 x 100.0 mm，自然色模式正确恢复为单耗材语义。
- 点击“确认并生成 G-code”后直接加载本地 OBJ，没有出现 job 丢失或网格修复错误；最终进入“预览”页。日志记录 `Exporting G-code finished` 和 `on_process_completed:finished`。
- GUI 证据为 `generated_models/gui-validation-phase26/06-history-model-foreground.png` 与 `07-after-local-import.png`；原始/修复诊断、DLL 备份和所有截图均位于项目 `generated_models`。
- AI Python 全量回归 68/68、相关模块 `py_compile` 和 `git diff --check` 通过；仅保留既有 Pillow 2027 弃用警告。本阶段未创建新 Tripo 任务。
- 正式 Orca PID 37912 当前响应正常，正式 sidecar PID 62156 监听 `127.0.0.1:18764`。
- `planning-with-files-zh` 自带 `check-complete.ps1` 因安装文件乱码产生 PowerShell 解析错误；项目内阶段 26 复选项与状态已直接核对完整，不修改外部技能文件。
- **状态：** complete

## 2026-08-11 阶段 25：彩色预览保持与持久模型库
- 用户要求修复颜色信息丢失、跨页面返回后 3D 预览消失，并将模型库改为可跨重启展示全部历史模型且支持双击载入预览。
- 已将远程任务状态与当前显示模型状态分离；成功导入和页面切换不再清除 3D 预览，返回生成页时刷新现有 OpenGL 画布。
- 已修复 OBJ 顶点色到 MMU 标注的基础色判断，新增 Catch2 用例通过 1 个测试、5 个断言。
- 模型库改为“图片预览 / 3D 模型 / 模型库”第三个同级标签页；扫描得到 15 个真实历史模型，按任务 ID 去重并优先选择 5 个规范产物，另外 10 个使用下载兜底。
- 双击最新真实历史模型成功载入 15,616 面、41.7 x 42.1 x 100.0 mm、10 色预览；跨“准备”页往返后统计和画布均保留。
- OpenGL 最终日志为 10 个颜色组、`gl_error=0`；高 DPI 下模型统计已改成两行，避免颜色信息和重置按钮被挤出。
- Windows Release 构建成功，正式启动器 `start_orcaslicer_with_ai.bat --check` 通过，未使用 mock 或新增付费 AI 调用。
- 最新构建 DLL 为 `build/src/Release/OrcaSlicer.dll`；PID 58140 正在使用日常运行目录的旧 DLL，为保护未保存内容暂未强制关闭，待进程正常退出后完成同步。
- 用户关闭旧实例后已完成部署；构建与运行 DLL SHA256 均为 `9539B7A1E718AF6846990C354E906B72222D55516ACF863406594249C8F71499`，旧版本已备份。
- `start_orcaslicer_with_ai.bat` 已启动正式 Orca PID 47888，进程响应正常，正式 AI sidecar 检查通过。
- **状态：** complete

## 会话：2026-07-29

### 阶段 1：恢复历史上下文
- **状态：** complete

## 2026-08-11 阶段 24：正式链路与准备页首开性能
- 用户截图显示本地正式 sidecar 已接收图片任务，但 sidecar 连接外部预处理服务失败，界面停留在“正在生成 AI 处理图”占位并显示英文错误。
- 用户明确要求后续不再使用 mock 版，所有功能与 GUI 验收统一走实际版本；本阶段不启动 `18765`。
- 同时调查首次点击“准备”页面卡顿，优先定位同步 OpenGL/Plater 初始化，而不是通过预先访问页面掩盖问题。
- 正式 sidecar 已固定为 `18764` 并确认实际使用 `https://laotie.dev`；`18765` 未监听，launcher `--check` 通过。
- 同一失败输入图已由真实图片服务生成 1,770,828 字节 Q 版预览，停在确认阶段；本阶段 Tripo 调用 0 次。
- OpenGL/GLCanvas 已改为首页空闲期预热；点击“准备”后 0.5 秒内出现盘面，且没有重复初始化。
- Windows Release `OrcaSlicer` target、AI Python 67/67、`py_compile` 和 `git diff --check` 全部通过。
- **状态：** complete

## 2026-08-11 阶段 23：可打印颜色开关
- 用户反馈严格可打印颜色效果不理想，要求可选是否使用可打印颜色。
- 采用默认开启的兼容方案；关闭后自然色贯穿 AI 预览和 OBJ 顶点色预览，导入与切片按单耗材处理。
- 本阶段只使用现有测试夹具和本地构建，不调用付费图片或 Tripo 服务。
- 首轮定向回归 50 项中 49 项通过；唯一失败是测试错误假定 OBJ 烘焙注释位于第一行，已改为验证实际顶点色和拓扑。
- 修正断言后定向回归 50/50；完整 AI 回归 65/65。随后语法检查因 Windows 不展开 `*.py` 通配符失败，改用 PowerShell 文件枚举重跑。
- Windows Release 增量构建通过。完整 `cmake --install` 同步资源后没有替换运行目录 DLL，改为在 Orca 未运行时精确部署新 DLL并校验哈希。
- 首次精确部署发现实际进程名为 `orca-slicer` 而非 `OrcaSlicer`；`tasklist /m` 定位到本项目 PID 49440，旧 DLL 未被覆盖，待正常停止该实例后部署。
- 正式 GUI 已显示默认开启的“使用可打印颜色”复选框；关闭后色板来源和色块正确隐藏，但说明文字在 200% DPI 下单行裁切，已改为两行等待复验。
- 最终 200% DPI 截图确认关闭态说明完整显示；同步更新 mock sidecar，使离线 GUI 回归也接受空色板自然色模式。
- 阶段 23 最终验收：AI Python 回归 66/66、Python 语法检查、`git diff --check` 和 Windows Release 构建全部通过。
- 构建与运行目录 DLL SHA256 均为 `2B8AA2EA57EFFFFBCD407CC30FC3137757FEA459FFC5715E96AC6E46484C259F`。
- 正式 sidecar 继续监听 `127.0.0.1:18764` 且 health 正常，正式 Orca PID 58632 响应正常；未调用付费图片或 Tripo 服务，真实 3D 预算仍剩余 17/20。
- GUI 证据：默认开启态 `generated_models/gui-validation-phase23/02-printable-colors-default.png`，最终关闭态 `04-printable-colors-disabled-final.png`。
- **状态：** complete
- 执行的操作：
  - 检查三个规划文件，均不存在。
  - 运行 `session-catchup.py`，没有恢复报告输出。
  - 确认目录中存在 Windows CMake 构建产物。
  - 确认项目根目录没有 `.git`，当前是源码工作副本而非可直接查询历史的 Git 工作区。
  - 从根目录文件名和时间戳识别到近期 AI/Tripo 集成痕迹。
  - 找到 `.claude/upstream-orcaslicer` 嵌套 Git 仓库，可用于恢复当前副本相对上游的改动。
  - 近期源码修改集中于 AI 侧车、模型生成 UI、主界面接入、资源图标与本地化。
  - 确认嵌套仓库 HEAD 为 `main@a62fb17`，但其版本 `02.08.01.55` 晚于当前副本 `02.06.00.51`，不能作为原始源码基线。
  - 全树 Git 对比因嵌套仓库工作树/索引异常不可用，转为通过统一初始时间戳、代码引用和构建产物恢复。
  - 从其他 CCD 会话中找到上次开发记录：目标为 Tripo 文生 3D / 图生 3D，最后阶段是创建并接入 `ModelGenerationPanel`。
  - 恢复到精确中断点：主体已编译、安装并运行验证；遗留默认页被抢占和选中态图标对比度两个 UI 问题，修复过程因上下文超限中断。
  - 核对运行截图与图标资源：页面布局和导航顺序已生效；选中态图标问题仍可见，且 inactive SVG 缺失。
  - 核对 `MainFrame` 接入点：没有隐藏新页，仍复用 `menu_obj_cube`，新 SVG 未被引用；两个遗留修复均未完成。
  - 恢复产品决策：3MF 优先、图生 3D 使用图片+文字联合提示；历史证据确认 GUI 编译和 4 个 Python 模块语法检查通过，但真实 Tripo 文生/图生两次付费 smoke 明确未执行。
  - 完成代码审计：两条 AI 用户流程已接通；模型库、sidecar 托管、远端取消、任务恢复和定向自动测试仍是缺口。
  - 完成 sidecar 边界审计：C++ 保持供应商无关，但 Python 绑定 AGNES/OpenAI/Tripo；另有端点校验、任务持久化、mock 漂移和仅 Windows 启动集成等上线前缺口。
  - 核对 build：VS 2022 多配置、Release、安装前缀 `build/OrcaSlicer`；存在 CTest 定义但无 LastTest 日志，当前未找到 OrcaSlicer.exe。
- 创建/修改的文件：
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### 阶段 2：检查当前开发状态
- **状态：** complete
- 执行的操作：
  - 核对源码工作副本、嵌套 Git、文件时间戳、历史会话、核心实现和构建目录。
  - 并行审计 AI 功能实现与 Windows 构建/CTest 证据。

### 阶段 3：总结与续作建议
- **状态：** complete
- 建议顺序：恢复可靠 Git 工作区；修复默认页与导航图标；重新编译并用 mock 复测；执行真实 Tripo 文生/图生两次付费 smoke；再处理模型库与架构增强。

### 阶段 4：补充架构与打包审计
- **状态：** complete
- 补充确认：Python sidecar 未纳入正式安装包；AI UI 未做 feature gate；本地化抽取清单已更新但 POT/PO 目录尚未重建；自动化测试仍无 AI 专项覆盖。

### 阶段 5：四项 AI 能力现状映射
- **状态：** complete
- 执行的操作：
  - 接收并记录四项产品目标。
  - 明确采用 provider-agnostic sidecar 与受控工作流编排原则。
  - 开始核对 OrcaSlicer 现有模型检查/修复、切片和交互基础设施。
  - 确定路线图原则：第一版采用固定、可恢复、可审计的工作流；LLM 负责理解/规划/解释，OrcaSlicer 确定性内核负责执行与校验。
  - 建立六阶段骨架：基础收尾 → 检查 MVP → 安全修复 → 切片闭环 → 低交互工作流 → 受控 Agent 化。
  - 初步定义 Intent、生成产物、可打印性报告、修复计划/结果、切片候选和工作流状态六类核心契约。
  - 定义能力成熟度 L0-L5，当前路线目标是先把单项能力推进到 L2/L3，再实现 L4 低交互编排；L5 自适应 Agent 后置。
  - 为外部付费调用、模型/配置变更、自动决策、feature gate、跨平台和 provider 契约定义统一验收原则。
  - 对照既有目标架构：四类能力已在文档中成型；当前应补平台/状态机并迁入现有生成流程，而非重写生成。
  - 确定双入口：专家工作台支持逐项操作，Guided/Auto 模式提供少提问的端到端流程；区分单步 `AIJob` 与跨步骤 `AIWorkflowRun`。
  - 完成模型检查/修复盘点：已有网格统计、越界/切片前校验、admesh 导入修复、CGAL 手动修复及标准 mesh 回写链；主要缺口是统一报告、风险/证据模型、修复前后差异、Undo 和自动化测试。

### 阶段 6：目标架构与边界
- **状态：** complete
- 完成 provider-agnostic 三层架构、六类数据契约、确定性执行边界、审批/预算/恢复策略和 Guided Workflow 状态机设计。

### 阶段 7：分期路线图
- **状态：** complete
- 给出 M0-M6 里程碑、各阶段验收标准和最短实施路径；建议先平台补洞和生成验收，再从 `PrintabilityIssue/Report` 与 `ModelPreflightService` 开始核心开发。

### 阶段 8：M0 AI 功能门控与能力发现
- **状态：** blocked — 代码与 Python 验证完成，C++ 构建/E2E 等待可用 Windows 工具链。
- 执行的操作：
  - 已重新读取 `task_plan.md`、`findings.md` 和 `progress.md`；`session-catchup.py` 无标准输出并以退出码 49 结束，未提供可操作的恢复报告。
  - 重新确认当前 Git 工作区可用，并审计 ModelGenerationPanel、AIAssistantPanel、MainFrame、Plater、AppConfig、Preferences 与 production/mock sidecar。
  - 实现 `enable_ai_features=false` 默认开关、实验设置 UI 与 `AppConfig` Catch2 覆盖。
  - 统一 `/health` 为 v1 capability schema，并新增 production/mock 无外部调用的 Python 契约测试。
  - 实现 `AIServiceManager` 的异步、loopback-only discovery、严格 schema 校验与关闭期取消；将生成页、AI Assistant AUI pane 和 View menu 改为 capability 成功后延迟注册。
  - 恢复标准 `TabPosition` 索引，生成页改为末尾追加且不自动选择；新增 inactive generate 图标。
  - `cmake --build` 失败，原因是 shell 找不到 `cmake`；随后 `where.exe` 也未找到 CMake、MSBuild、devenv 或 Ninja。构建目录仍有 `build/OrcaSlicer.sln`，但无可执行工具链可驱动它。

## 测试结果
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|---------|---------|------|
| 会话恢复 | 项目根目录 | 找到历史上下文或明确无记录 | 脚本无输出 | 完成 |
| Sidecar health contract | production/mock 临时 loopback server | v1 capability schema 一致且不泄露配置 | 3 tests passed | 完成 |
| Python syntax | sidecar、mock、contract test | Python 可编译 | `py_compile` 成功 | 完成 |
| Diff format | 当前工作区 | 无空白错误 | `git diff --check` 成功 | 完成 |
| C++ GUI build | `libslic3r_gui` Release | 编译改动后的 GUI 静态库 | 成功；日志含 AIServiceManager/MainFrame/Plater | 完成 |
| Application link | `OrcaSlicer` Release | 链接可启动应用目标 | 成功；生成并安装 `build/OrcaSlicer/orca-slicer.exe` | 完成 |
| GUI E2E (disabled) | 隔离 `--datadir` + mock | AI 默认关闭时无 discovery | 应用正常启动，配置持久化为 `enable_ai_features=false`，mock 无应用 `/health` 请求 | 完成 |
| GUI E2E (enabled) | 隔离 `--datadir` + mock | 发现后注册并打开生成页 | mock 收到 `/health`；窗口响应；“3D Generate” 内容页实际加载且未抢占默认页 | 完成 |
| Catch2 AppConfig | 独立 `.workbuddy/build-tests` Release | 构建并运行新增测试 | `AppConfig AI feature gate`：3 assertions 通过，随机顺序执行 | 完成 |
| AI Assistant menu E2E | OrcaSlicer 自定义菜单 | 验证菜单项与 AUI pane 显示/隐藏 | wx 顶栏未向 UI Automation 暴露命令，DPI 坐标点击不可靠；代码路径已在 Release 构建通过 | 待补充 |
| Simplified AI Python contract | `tools/ai/test_sidecar_contract.py` | 验证 OpenAI/Tripo capability matrix | 4/4 通过：无凭据、OpenAI-only、OpenAI+Tripo、mock schema | 完成 |
| Simplified AI Release build | VS 2022 CMake `OrcaSlicer` target | 构建固定生成页与 OpenAI migration | 成功，重新编译 AppConfig、MainFrame、ModelGenerationPanel、Preferences | 完成 |
| Permanent 3D Generate page | 无凭据 production sidecar + 隔离 `--datadir` | 页面无开关默认存在 | `/health` 两项能力均 false；实际窗口顶部仍显示 3D Generate | 完成 |
| Disabled generation controls | 无凭据 production sidecar | 打开页面后验证禁用动作与状态文案 | 当前桌面环境持续抢占 OrcaSlicer 前台，无法可靠进入自定义页完成视觉断言 | 待手动确认 |

## 错误日志
| 时间戳 | 错误 | 尝试次数 | 解决方案 |
|--------|------|---------|---------|
| 2026-07-29 | 未找到既有规划文件 | 1 | 创建新规划文件 |
| 2026-07-29 | 等待构建审计最终摘要超时，代理仍在运行 | 1 | 停止轮询，等待自动完成通知 |
| 2026-07-29 | 补充 GUI 追踪代理因上下文超限返回 502 | 1 | 不重复调用；现有独立证据已覆盖该范围 |
| 2026-07-29 | 新阶段恢复时 `git diff --stat` 因根目录无 `.git` 失败 | 1 | 停止根目录 Git 检查，沿用文件与历史证据 |
| 2026-07-29 | 模型检查/修复盘点代理因上游连接失败返回 502 | 1 | 缩小任务范围并复用同一代理上下文 |
| 2026-07-30 | `cmake --install build --config Release` 无法覆盖 `build/OrcaSlicer/OrcaSlicer.dll`（permission denied） | 1 | 用户关闭运行实例后重试安装成功；完成隔离 GUI E2E |
| 2026-07-30 | `libslic3r_tests` target 不存在 | 1 | 确认当前 CMake cache 禁用 `BUILD_TESTING`/`BUILD_TESTS`；不更改已有 build 配置，待独立测试构建目录 |
| 2026-07-30 | OrcaSlicer 自定义菜单不暴露标准 UI Automation 命令 | 1 | 不伪造菜单验收；记录 AI Assistant menu/pane 交互测试待专用驱动补充 |
| 2026-07-30 | 主 build 未生成 Catch2 target | 1 | 新建隔离 `.workbuddy/build-tests`，显式启用 `BUILD_TESTS`/`BUILD_TESTING`；`AppConfig AI feature gate` 随机顺序测试通过 |

## 五问重启检查
| 问题 | 答案 |
|------|------|
| 我在哪里？ | 四项 AI 能力现状评估与路线图已完成 |
| 我要去哪里？ | 等用户确认后，从 M0 工程基线与平台补洞开始实施 |
| 目标是什么？ | 构建 provider-agnostic、可审计、可恢复的意图到可切片结果工作流 |
| 我学到了什么？ | 见 `findings.md` |
| 我做了什么？ | 见上方记录 |

## 会话：2026-08-08

### 阶段 9：五大产品域状态与实施计划
- **状态：** complete
- 执行的操作：
  - 按 `planning-with-files-zh` 恢复并扩展现有计划。
  - 完整读取 `Docs/` 下 7 份 Markdown 架构资料。
  - 按 spreadsheet 技能使用 artifact runtime 读取 `Docs/开发进展.xlsx` 的 `Sheet1!B3:F30` 并完成整表视觉渲染核对。
  - 核对当前 `master@a1ef7204fe`、最近两次 AI 提交和当前工作区；保留用户已有未跟踪文档与设置改动。
  - 审计模型生成、彩色产物、会话模型库、参数建议、sidecar、能力发现、原生摆盘/上色/修复和账号计费相关代码。
  - 确认 27 项子能力中存在“开发表状态落后”和“历史计划结论已过时”的口径差异。
  - 使用架构设计方法补充状态所有权、双轨路线图、Proposed ADR、非功能门槛和失败补偿边界。
  - 新建 `Docs/AI能力状态与实施计划.md`。
  - 核对 `Docs/README.zip` 为同一套资料归档，并查看 AI 代码级与模块级目标架构图，未发现图中独有的当前实现声明。
  - 运行 `tools/ai/test_sidecar_contract.py`：4 tests passed。
  - 运行 `git diff --check`：通过；用户原有 `.claude/settings.local.json` 修改和其他未跟踪资料均保留。
- 遇到的问题：
  - 工作簿读取脚本在完成唯一工作表检查与渲染后，继续访问不存在的下一工作表而以退出码 1 结束；`Sheet1` 数据和 `01-Sheet1.png` 均已成功生成并完成视觉核对，不影响分析结论。
  - 清理 `.workbuddy/xlsx-analysis-20260808` 时，递归删除和逐项非递归 `Remove-Item` 均被本地安全策略拒绝；已用补丁删除检查脚本，剩余渲染缓存和依赖 junction 不影响源码或原始文档。

### 五问重启检查
| 问题 | 答案 |
|------|------|
| 我在哪里？ | 五大产品域 27 项能力状态复核已完成 |
| 我要去哪里？ | 先执行 F0 工程基线，再并行推进核心 AI 轨和荣耀商业平台轨 |
| 目标是什么？ | 在保持 OrcaSlicer 本地能力、兼容性和业务真值的前提下交付可恢复、可计费的 AI 工作流 |
| 我学到了什么？ | 见 `findings.md` 的“五大产品域状态复核” |
| 我做了什么？ | 输出 `Docs/AI能力状态与实施计划.md` 并完成契约与文档校验 |

### 阶段 10：当前软件基线验证
- **状态：** complete
- 验证基线：`master@a1ef7204febebfed36a69589ffb4da10e2c89002`。
- Windows Release `ALL_BUILD` 构建成功；安装完成，构建/安装 EXE 与 DLL 哈希一致。存在一条既有 `OrcaSlicer_profile_validator` `LNK4098` 告警。
- 重建并执行当前 HEAD 的 `libslic3r_tests`：129/129 通过。
- production/mock sidecar `/health` 正常；`tools/ai/test_sidecar_contract.py` 4/4 通过；相关 Python `py_compile` 通过。
- 使用 `.workbuddy/status-check-20260808` 隔离数据目录和 `http://127.0.0.1:18765` mock sidecar 启动安装目录程序：
  - 进程启动后恢复响应，默认首页未被抢占；
  - `3D Generate` 页面存在，capability discovery 后显示本地服务就绪；
  - mock 文生 3D 完成预处理、确认、生成和结果就绪；
  - 带顶点色 OBJ 经原生颜色映射确认后导入盘面，日志存在 Undo 快照与 `load_model 1`；
  - 正常退出后无 OrcaSlicer 残留进程，两个既有 sidecar 未被停止。
- 截图证据：`.workbuddy/status-check-20260808/generate-page.png`、`after-generate.png`、`imported-model.png`。
- 未覆盖：真实付费 provider、AI Assistant menu/pane、macOS/Linux、颜色 3MF round-trip 与切片保真。
- 测试缺口：当前 HEAD 不再包含历史 `AppConfig AI feature gate` 测试，尚无有效 C++ AI capability/GUI 回归覆盖。

### 阶段 11：2026-08-10 核心演示版冲刺
- **状态：** in_progress
- 用户将 macOS/Linux 降为最低优先级，要求下周一先跑通一个可演示核心版本。
- 已将冲刺范围压缩为 Windows 单平台、单条模型生成到切片预览的黄金路径。
- 用户已确认现场必须完成真实 AI provider 调用，且该通路此前已人工验证；mock 降为回归和灾备。
- 已读取 `Code` 技能的规划、执行和验证约束；实现将按可独立验收的小步推进。
- 检索启动入口时 PowerShell 将 `*.bat` 作为无效路径传给 `rg`，退出码 1；后续改用 `rg --files` 过滤文件清单，不重复该写法。
- production sidecar 当前 `/health` 返回 protocol v1，真实文生/图生 capability 均可用；已有历史真实产物 `real-tripo-text.3mf`。
- 启动脚本审计通过：无硬编码 API key，具备 sidecar readiness 等待和 Release 程序启动。
- 动态正则检查脚本首次因 PowerShell 引号解析失败，未读取或输出密钥；已改用固定模式的布尔审计完成检查。
- 凭据范围检查首次因 `foreach | Format-Table` 解析限制在执行前失败；改为先收集对象后成功，确认 OpenAI/Tripo key 均可在系统重启后继承，且全程未打印密钥内容。
- 已按 `writing-plans` 输出 `Docs/plans/2026-08-08-core-demo-implementation-plan.md`，拆分为启动门槛、smoke 客户端、真实双 smoke、GUI 彩排和演示手册五项可独立验收任务。
- Task 1 完成：新增 `tools/ai/check_sidecar_capability.ps1` 和 4 项 readiness unittest；启动器现在复用已就绪 sidecar、不可达时才启动、能力不可用/超时时 fail closed，并支持 `--check`。
- 验证结果：readiness 4/4 通过；production/mock 检查返回 0；非 loopback endpoint 返回 2；`start_orcaslicer_with_ai.bat --check` 成功。
- Task 2 完成：新增显式付费确认的 `tools/ai/smoke_model_generation.py` 与 3 项 mock 测试，覆盖文生、图生、multipart、轮询、下载和任务清理。
- 验证结果：smoke 3/3、sidecar contract 4/4、readiness 4/4、相关 Python `py_compile` 全部通过；缺少 `--confirm-paid-call` 时返回 2 且零任务创建。
- 真实文生 smoke 首次执行在预处理阶段失败：配置的 OpenAI-compatible `/models` 返回 HTTP 502；sidecar 未创建 Tripo 任务，未产生生成费用。
- 已确认下一步采用默认关闭的 `ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=1` 演示降级：保留原始文本/图片继续真实 Tripo，并在任务状态中明确披露预处理降级。
- 已实现预处理降级及回归：默认关闭；开启后文本保留原始 prompt，图片按签名保留原始 PNG/JPEG；health 在降级模式仅要求 Tripo 才开放模型生成 capability。演示启动批处理默认启用该显式开关。
- 本地测试结果：15 项 unittest 全部通过，相关 Python `py_compile` 通过。
- 第一次后台重启探针因旧 `cmd /K` sidecar 继续占用 `18764` 而仍走旧环境；未创建 Tripo 任务。清理已确认的两个生产 sidecar 子进程后，在 `18766` 启动受控实例，免费探针成功进入 `awaiting_confirmation` 并披露原始输入降级。
- 真实文生 smoke 通过：3MF 27,399,111 字节，283.6 秒，产物 `.workbuddy/core-demo-real-20260808/text/text-191806d4-5b3b-4f51-a1a5-b5803049b0d5.3mf`。
- 真实图生 smoke 通过：参考图 `resources/web/model/img/p1.png`，3MF 27,139,455 字节，173.0 秒，产物 `.workbuddy/core-demo-real-20260808/image/image-6a95b1b1-3e71-44da-83c1-5a5414a3aca2.3mf`。
- 两条真实链路均进入 `ready` 并完成客户端下载和 DELETE 清理；未将 OpenAI 预处理标记为成功。
- 已流式校验两个真实 3MF：均含合法模型部件；文生 704,186 顶点/1,408,374 面/约 71×71×100 mm，图生 703,848 顶点/1,408,070 面/约 47×100×27 mm。
- 已用隔离数据目录启动 Windows GUI 并载入文生 3MF；日志确认 1 个对象 geometry-only 导入和自动摆放成功，窗口标题正确、进程响应正常。
- 截图脚本首次因程序集加载顺序错误未生成图片；修正后截图成功，但桌面已锁屏，无法安全执行可见切片/保存交互。转为先用 OrcaSlicer CLI 内核完成切片验证，解锁后补 GUI 彩排。
- CLI 自动摆盘并导出 Orca 3MF 成功：`.workbuddy/core-demo-gui-20260808/roundtrip-text.3mf`，23,366,136 字节。
- CLI 重开该项目并切片失败：无 G-code 产物，`00000.log` 在模型重载阶段截断；Windows Event 记录 `OrcaSlicer.dll+0x1195fe` 的 `0xc0000005` APPCRASH。已停止重复命令，转入符号/转储诊断。
- 使用 Release PDB 将故障 RVA 定位到 `std::string` copy assignment；GUI 重开同一项目稳定完成，故该崩溃收敛为 CLI 专属问题。
- 已通过唯一窗口句柄确认“自定义的预设”安全提示，项目重开完成；程序化触发“切片单盘”后切到预览，但 Default Printer 的相对挤出 G-code 校验失败，未产生有效刀路。
- 校验日志同时确认模型位于盘内且存在可打印实例；下一轮改用正常配置中已选择的仓库系统预设 `WonderMaker ZR 0.2 nozzle`。
- 使用正常用户配置中的 `WonderMaker ZR 0.2 nozzle`、`0.08mm Optimal @WonderMaker ZR 0.2 nozzle` 和 `WonderMaker PLA Basic` 完成真实文生模型 GUI 切片。进程保持响应；切片及 G-code 导出约 14 秒完成，状态码 0，`psGCodeExport=1`。
- G-code 摘要：45,019,094 字节、1249 层、44.09 g PLA、预计打印时间 6 小时 47 分 40 秒；窗口控件确认“导出G-code文件”已启用、可见，GUI 已处于预览。
- 新增 `Docs/demo/2026-08-10-core-demo-runbook.md`，固化启动前检查、固定输入、正常预设、现场操作、真实产物和诚实灾备策略。
- 阶段 11 Windows 核心演示黄金路径完成；macOS/Linux、账号计费、AI Assistant、自动修复等继续冻结。
- 最终无付费回归通过：预处理降级、sidecar contract、readiness、smoke 共 15/15；相关 Python `py_compile` 通过；正式 sidecar readiness 返回 0；`start_orcaslicer_with_ai.bat --check` 成功；`git diff --check` 通过，仅有既有 LF/CRLF 提示。

### 阶段 12：3D Generate 页面交互优化
- **状态：** complete
- 根据用户标注图收敛两个目标：参考图完整预览并支持缩放；生成进度明确表达当前步骤。
- 修改 `ModelGenerationPanel.hpp/.cpp`：新增滚动预览、`- / Fit / + / 百分比`、选图即时预览、五阶段进度映射、步骤标题和百分比。
- 左侧工作流宽度调整为 380/340 DIP，右侧预览最小尺寸调整为 360×300 DIP，改善演示窗口的横向空间分配。
- 首轮构建发现 `m_preview_area` 仍为 `wxPanel*`，修正为 `wxScrolledWindow*` 后，`libslic3r_gui` 与完整 `OrcaSlicer` Release 构建、链接均通过；仅保留既有 `LNK4098` 告警。
- 通过隔离数据目录、mock sidecar 和窗口句柄完成锁屏 GUI 验收：1080×1620 图片 Fit 为 179×268；125% 为 209×314 且垂直滚动信息有效；Fit 恢复后滚动范围清零。
- 验证 Review 显示 `Step 3 of 5` / 35%，Ready 显示 `Step 5 of 5` / 95%；代码入口保证 Generate 从第四步 40% 开始。
- 发现并修复 Windows 中文代码页下 `·` / `×` 的乱码；失败状态不再把流程重置回 Input。
- 新增 `Docs/plans/2026-08-10-model-generation-interaction-design.md`。
- 最终重新构建 `OrcaSlicer` Release 成功；新实例确认 `Step 1 of 5 · Input` 无乱码。现有 AI Python unittest 15/15 通过，`git diff --check` 通过，仅有既有 LF/CRLF 提示。
- 本轮隔离 OrcaSlicer 实例和 `18765` mock sidecar 已关闭；截图、日志和窗口验收证据保留在 `.workbuddy/ui-interaction-20260810/`。

### 阶段 13：完整参考图与两段式图生 3D 流程
- **状态：** complete
- 用户反馈参考图仍被裁切，并要求图片模式先生成风格化二维预览，再确认是否生成 3D。
- 已确认启动命令为项目根目录下的 `start_orcaslicer_with_ai.bat`；`--check` 仅检查 sidecar capability。
- 已查看 1080×1620 原图，源文件构图完整，裁切来自显示或生成环节。
- 已确认 sidecar 的真实图片预处理调用 `/images/edits`，但当前演示降级会复制原图并把任务标记为可确认，需拆分语义并收紧确认条件。
- 将预览从 `wxStaticBitmap` 子控件改为 `wxScrolledWindow` 双缓冲自绘，Fit 等比缩放并居中留边，保留 50%–400% 缩放与滚动。
- 预览标题明确区分 `Reference image`、`AI style preview` 和 `No preview`；图片模式流程文案改为 `Input → Style preview → Review → Generate 3D → Import`。
- 图片模式的 3D 按钮仅在任务处于 `awaiting_confirmation` 且风格预览成功下载、解码和显示后启用；无效或下载失败时保持禁用。
- sidecar 的图片预处理不再允许复制原图 fallback；文字 prompt 仍允许显式演示降级。对应回归测试改为 `test_image_preprocessing_never_uses_original_as_style_preview`。
- 复核界面时进一步将左侧流程摘要拆为两行、缩短图片指令标签、压缩隐私说明并给长文件名增加尾部省略；动态状态按 330 DIP 换行。
- Windows Release 首次并行构建因系统分页文件不足出现 `C3859/C1076`；改用 `/m:1` 后连续构建、链接均通过，仅保留既有 `LNK4098` 告警。
- 最终构建 DLL 已同步到 `build/OrcaSlicer/OrcaSlicer.dll`，与 `build/src/Release/OrcaSlicer.dll` 的 SHA256 均为 `D4230825137664051FBB2F050D6F93E3E90E79D064BDD84E82F508025EC497A9`。
- Python `py_compile` 通过；预处理 fallback、sidecar contract、readiness、smoke 共 15/15 通过；`git diff --check` 无空白错误。
- 使用 `18765` mock sidecar 与隔离数据目录完成 GUI 验收：参考图完整显示，125%/Fit 状态有效，参考图阶段 3D 按钮禁用，风格预览显示后进入 `Step 3 of 5` 且按钮启用。
- 物理分辨率截图证据保留在 `.workbuddy/ui-interaction-20260810/stage13-final-reference-fit.png` 和 `stage13-final-style-preview.png`。

### 阶段 14：真实 AI 风格预览通路修复
- **状态：** complete
- 确认新代理 `https://laotie.dev` 的根路径与 `/v1` 模型列表均返回 200，并支持 `gpt-image-2`。
- `openai_preprocessor.py` 现在自动把域名根地址补全为 `/v1`，拒绝带 query/fragment 的 Base URL，并保留已有或自定义兼容路径。
- 新增图片风格预览提示词包装，保证短指令也要求真实风格化、身份保持、完整构图和禁止原图直出。
- 新增 `test_openai_preprocessor.py`；预处理、sidecar contract、readiness 与 smoke 共 21/21 通过，`py_compile` 与 `git diff --check` 通过。
- 真实图片编辑成功，输出保存在 `.workbuddy/live-style-preview-20260810/cartoon-preview.png`；输出 SHA256 与原图不同，人工复核风格及构图符合要求。
- 已停止继承旧代理的 sidecar PID `51604`，最终 sidecar PID `39212` 继承新 Base URL 并健康运行；现有 OrcaSlicer PID `53840` 无需重启。

### 阶段 15：原图与 AI 结果对照预览
- **状态：** complete
- 复核用户截图：AI 结果已下载并显示，但唯一预览位图覆盖了原图，页面缺少前后对照。
- 交互方案确定为同一画布左右并排显示 `Reference` 与 `AI result`，共享 Fit、缩放和滚动，并覆盖等待、生成、失败、成功状态。
- `ModelGenerationPanel.hpp/.cpp` 已拆分参考图和 AI 结果状态，在同一自绘画布生成两块稳定预览区域；模型库继续优先使用 AI 结果缩略图。
- `libslic3r_gui` 与完整 `OrcaSlicer` Release 单并发构建成功，仅有既有 `LNK4098` 告警；AI Python 回归 21/21 和空白检查通过。
- 隔离安装与 `18765` mock sidecar GUI 验收完成：两图同屏、尺寸摘要正确、Review 为 Step 3/5、3D 按钮启用；共享缩放 125% 与 Fit 100% 均有效。
- 首次 PrintWindow 截图因 200% DPI 虚拟化只分配半尺寸位图，误裁右半边；启用 Per-Monitor DPI awareness 后获得 3144×2008 完整证据 `.workbuddy/stage15-compare-run/comparison-review-dpi.png`。
- 正式 DLL 已同步且哈希一致；关闭隔离实例/mock 后，通过 `start_orcaslicer_with_ai.bat` 重启正式程序 PID `19100`，真实 sidecar PID `39212` 保持运行。
- 设计说明保存于 `Docs/plans/2026-08-10-image-comparison-preview-design.md`。

### 阶段 16：OBJ-only 生成与导入
- **状态：** complete
- sidecar 真实生成移除 3MF/STL 回退，固定将 Tripo 生成任务转换为 OBJ；OBJ 无效或缺少受支持顶点色时任务失败并显示原因。
- production/mock `/health` 统一声明 `artifact_formats: ["obj"]`；启动探针、smoke 客户端与 C++ capability discovery 已同步。
- `ModelGenerationPanel` 的格式说明改为 OBJ 顶点色，下载/导入入口明确拒绝非 OBJ 结果。
- 新增两条 OBJ 生成回归；`python -m unittest discover -s tools/ai -p 'test_*.py' -v` 共 23/23 通过，相关 `py_compile` 通过。
- Windows Release `OrcaSlicer` 构建和链接通过；首次链接因运行中的 OrcaSlicer 锁住 DLL 出现 `LNK1104`，正常关闭实例后重试成功。
- 完整 install 同步完成，构建与安装 DLL SHA256 均为 `CC8D6DA9CA1B29A4C494B7072570BC90CD3A0819D4C2317158565B5DD6E9E0FF`。
- 新 production sidecar PID `56008` 的 health 返回 OBJ-only 能力；最新版 OrcaSlicer PID `42900` 已启动并保持响应。
- `start_orcaslicer_with_ai.bat --check` 与 `git diff --check` 通过，仅有既有 LF/CRLF 提示；本轮未调用付费模型。

### 阶段 17：OBJ 资源包、颜色与本地产物闭环
- **状态：** complete
- 用户真实图生 3D 已完成 Tripo 转换，但 sidecar 把 ZIP 资源包误当成纯文本 OBJ，导致 UTF-8 校验失败。
- 已读取失败产物文件头和 ZIP 目录，确认其中包含 OBJ、MTL 和四张 JPEG 纹理；本轮诊断未触发新的付费调用。
- 已确认手工解压导入无颜色的原因是 OrcaSlicer 当前不启用 UV 图片纹理 OBJ 导入，只支持顶点色或 MTL 平面颜色映射。
- 已确定修复方案：项目根目录持久化全部任务文件，安全解包并保留原资源，同时烘焙生成 Orca 可识别的顶点色 OBJ。
- sidecar 已改为项目内持久化任务目录，保留输入、预览、原始下载和解包资源；DELETE、退出与 GUI reset 只释放状态，不删除磁盘产物。
- 新增安全 ZIP 解包与 `map_Kd` 纹理到顶点色 OBJ 转换，纹理接缝按顶点/UV/材质组合拆点，最终 OBJ 使用 OrcaSlicer 已支持的 XYZRGB 顶点格式。
- GUI 下载副本改存 `generated_models/downloads/`；`start_orcaslicer_with_ai.bat` 和直接 sidecar 启动器均固定 `ORCASLICER_AI_OUTPUT_DIR`。
- `test_obj_generation.py` 扩展为 15 项测试；全部 AI unittest 36/36 通过，Python `py_compile`、`git diff --check` 和 `libslic3r_gui` Release 构建通过。
- 使用真实失败 ZIP 离线恢复得到 75,029,064 字节顶点色 OBJ，包含 742,065 个顶点和 1,449,376 个面；验证器通过且抽样至少 1,000 种颜色。
- Windows Release 链接和 install 完成，构建/安装 DLL SHA256 均为 `3B6C6B8E18ABF6EFA2E6AC6DA240C47F7815DA666E277468828120599CC49DCE`。
- production sidecar 通过 `https://laotie.dev` 启动，18764 最终仅一个监听者且 health 声明 OBJ 可用；新版 OrcaSlicer 加载真实恢复 OBJ 后出现 `Obj文件导入颜色` 对话框并保持响应。
- 遇到的问题：真实转换和 install 均超过外层命令的短时等待窗口，但后台任务最终正常完成；对第一份重复恢复目录的删除被本地安全策略拒绝，因此两份恢复数据都留在 `generated_models/` 且已被 `.gitignore` 排除。

### 阶段 18：彩色低模与打印色板约束
- **状态：** in_progress（实现完成，等待用户确认付费 Tripo 3D 验收）
- 已读取 Tripo 官方文件上传、文本生成 3D、图片生成 3D 和图片编辑文档。
- 已确认生成阶段可用 `smart_low_poly=true` 和 `face_limit<=20000` 直接产出带贴图低模；`quad=true` 会强制 FBX，不适用于当前 OBJ-only 链路。
- 已确认 Tripo 没有严格色板参数，下一步需从 Orca 当前打印机/耗材槽读取可用颜色，并设计提示约束与两次确定性色板量化。
- 文档读取的 AutoGLM skill 因本机 token 服务未运行失败，随后使用应用内浏览器从官方网页取得参数定义，未调用任何生成 API。
- 已核对本地实现链路：`tripo_client.py` 当前未传低模/纹理参数，`AIModelGenerationClient` 当前未传色板，`openai_preprocessor.py` 仅通过提示词约束图片，`orca_ai_sidecar.py` 已有贴图到顶点色烘焙入口，适合加入最终色板映射。
- 已定位色板权威来源为 `Plater::get_extruder_colors_from_plater_config()`；它读取当前项目 `filament_colour`，并已被现有 OBJ 颜色导入对话框使用。
- 低模默认策略收敛为 20,000 三角面、保留 Base Color/UV、禁用 PBR 和 Quad；失败时不静默提高面数，只在用户明确选择兼容模式后以 50,000 面重试。
- 设计阶段将色彩策略收敛为三层：提示词加入当前耗材 HEX 色板、本地量化 AI 预览、OBJ 纹理烘焙时再次映射到同一色板；该设计步骤当时未触发付费生成。
- 已实现 Tripo 20,000 面彩色低模参数、当前耗材色板的 C++/sidecar 全链路传递、预览确定性量化和 OBJ CIE Lab 二次量化。
- 已修复 UV 接缝复制顶点造成的多零件/人工开边根因；最终 OBJ 在导入前必须通过单连通、封闭流体、三角面、面数与色板校验。
- Windows Release 完整构建通过，仅保留既有 `LNK4098`；构建与运行目录 DLL SHA256 同为 `3E2A4A060CE65A4993FED7659D3FB9D8C6CA9964DA7285AC9F7C19B2E346D147`。
- 200% DPI 真实 GUI 截图验收发现并修复色板末列与摘要裁切；16 色现稳定显示为 `6+6+4`，模型库文案同步为 `generated_models/downloads` 本地持久化事实。
- 使用当前 16 色耗材配置完成两次真实付费图片编辑。第一版暴露写实比例和背景噪点；第二版经提示词与色块净化修复后形成明确 Q 版盲盒造型和纯色背景。
- 第二版首次统计发现 RGB 众数滤波会组合出 9 个极少量色板外颜色；已改为索引色滤波并补回归。最终预览 `generated_models/paid-image-validation/palette-preview-v2-final.png` 使用 11 个允许色，色板外像素为 0。
- 旧真实 OBJ 的离线门禁返回 `exceeds the 20000-triangle low-poly limit`，证明超高面资产不会再进入 Orca 修复丢色流程。
- 本阶段未调用付费 Tripo 3D；端到端真实低模 OBJ 生成仍需用户明确确认。
- 遇到的问题：两次复杂进程重启命令被本机策略拦截，均改为核验 PID 后分步停止/启动；Windows curl 三次因 native 参数转义导致色板 JSON 在本地校验失败，确认未创建任务后改用仓库已测试的 Python multipart 编码器。
- 用户已明确授权一次付费 Tripo 3D 验收；提交前重新完成输入色板预检、客户端参数复核和 47/47 Python 回归，确认不存在既有付费状态文件。
- 新增可恢复验收脚本 `tools/ai/run_paid_tripo_validation.py`。脚本以输入 SHA256 绑定状态，生成 task ID 返回后立即写入仓库目录；无确认参数时只允许预检，已有状态时只恢复同一任务。
- 唯一付费生成任务 `9ba4255f-732c-430d-a381-0ae3a8e5507a` 成功，消耗 40 credits；OBJ 转换任务 `ea204146-0e6f-46cb-af55-e696f34d0fec` 成功，消耗 5 credits。
- 本地处理成功生成严格色板顶点色 OBJ；视觉复核确认 Q 版人物效果符合参考。恢复执行验证只查询上述两个 task ID，没有创建新的付费任务。
- 最终门禁失败：26,147 个三角面超过 20,000 上限；网格为单一连通体但有 26 条边界边和 14 条非流体边。13,066 个顶点均使用当前耗材色板，色板外颜色为 0。
- 结构化报告保存为 `generated_models/paid-tripo-validation/9ba4255f-732c-430d-a381-0ae3a8e5507a/validation-result.json`，根状态明确为 `validation_status=failed`；未导入 Orca，避免触发修复后丢色，也未自动提交第二个 Tripo 任务。
- 验收脚本 `py_compile` 与定向 `git diff --check` 通过；真实产物失败是服务端网格质量问题，不是本地颜色转换或任务恢复问题。
- 最终复验：AI Python 回归 47/47、相关 `py_compile` 与全局 `git diff --check` 通过；OrcaSlicer PID `53220` 保持响应，production sidecar PID `48420` 的 protocol-v1 health 正常。
### 阶段 19：3D Generate 导航、统一输入与色板交互（续）
- **状态：** in_progress
- 已恢复并复核设计文档 `Docs/plans/2026-08-10-unified-3d-generation-input-design.md`、用户标注截图和当前工作区差异。
- 当前实现已包含标签前移、统一文字/图片输入、四步紧凑流程、项目色板/自定义色板入口，但仍是未编译的中间状态。
- 本轮下一步：清理遗留五步进度与状态快照问题，修正色板源切换重建，完成 Windows Release 编译、Python 回归和 200% DPI GUI 验收。
- 本轮不会调用任何付费 AI 接口。
- 首次 Windows Release 构建未进入有效源码诊断：项目内 `/MP` 仍并发启动大量 PCH 编译，MSVC 报 `C3859/C1076` 与系统错误 1455（页面文件太小）。下一次构建将显式设置 `CL_MPCount=1` 并关闭 `UseMultiToolTask`。
- 使用真正串行的 `CL_MPCount=1` 后，改动对象与 `libslic3r_gui.lib` 编译成功；外层工具在 20 分钟上限终止后，增量重跑在 5 秒内完成 `OrcaSlicer` Release 链接。
- 新构建 DLL SHA256：`EE6358598A82A416C16EA768454CA429D30E8C7154310E910B94E65CAB2BC4FF`。
- AI Python 回归 `47/47` 通过；`git diff --check` 通过；旧 `m_mode`、五步进度与旧模式文案扫描无残留。
- 安装同步成功，运行目录 DLL 与新构建哈希一致；production sidecar `/health` 正常。
- 200% DPI 首张截图发现进度区仍重复显示当前阶段与 `Step 2 of 4`，且百分比在窄窗口右侧被裁切；已将进度区继续压缩为“流程行 + 当前阶段/百分比 + 进度条”。
- 误对无 `--help` 支持的 mock 脚本调用帮助参数，意外启动了两个临时 mock 进程；已按精确 PID 清理，production sidecar 未受影响。
- 200% DPI 隔离 mock GUI 已验证：空输入禁用、文字-only 启用、image-only 启用、text+image 启用、项目 5 色自动读取、自定义色板入口和完整参考图/AI 结果双栏预览。
- 验收中发现左栏最窄分配宽度下固定 350 DIP 换行和文件名最小宽度会造成约 20 DIP 横向溢出；已改为 310 DIP 换行并允许文件名省略收缩。
- 自定义 wxWidgets 页面未暴露 UI Automation 控件树；一次坐标输入误落到微信输入框，文字未发送且已立即清空。后续改用 Win32 子窗口句柄和 `PrintWindow` 完成无焦点干扰的交互与截图。
- 文件对话框顶层枚举脚本首次误用 PowerShell 保留变量 `$PID`，产生重复只读变量错误；改用 `$procId` 后成功完成本地图片选择。
- 阶段 19 最终实现完成：标签顺序为 `Home -> 3D Generate -> Prepare -> Preview`；统一输入允许文字-only、图片-only 和文字+图片，空输入保持禁用。
- 生成流程收敛为 `Input -> Review -> Generate -> Import` 四步，并仅展示当前状态可执行的主操作；参考图与 AI 处理图在 Review 中并排完整显示。
- 色板支持自动读取当前项目 `filament_colour`，也支持最多 16 色且不回写项目配置的自定义色板；预处理开始时冻结输入与色板快照，后续改动会使旧确认失效。
- 200% DPI 最终 GUI 验收覆盖空输入、三种有效输入组合、项目 5 色自动色板、自定义色板及手动新增颜色；首屏无纵向滚动、右侧裁切或控件重叠。
- Windows Release 构建与链接通过；AI Python 回归 `47/47`、`git diff --check` 和遗留状态扫描通过。安装目录最终 DLL SHA256 为 `75631D5E7B8AB6C5AA70D7DD11DAA2BEA1CC3BF17BFB3AAA326077C43181B213`。
- 正式 OrcaSlicer PID `41236` 保持响应，production sidecar 运行于 `127.0.0.1:18764` 且 health 正常；临时 `18765` mock 已停止。本轮 GUI 验收未调用任何付费 AI。
- 阶段 19 状态更新为 complete；最终 GUI 证据保存在 `.workbuddy/phase19-gui-20260810/`。

### 阶段 20：三风格预览、色板保真与一键 G-code
- **状态：** in_progress
- 用户要求修复严格色板后风格化预览只剩两种主色的问题，新增 Q 版卡通、赛博朋克、古典风格选择，并将风格预览之后的模型修复、简化、缩放、摆盘和切片默认自动执行到 G-code。
- 用户授权按需使用图片生成，并允许最多 20 次付费 3D 生成；后续所有 3D 调用必须持久化 task ID、用途、结果与预算计数。
- 首轮代码定位确认当前预览链路同时使用“只能使用耗材色”的强提示、无抖动最近色色板量化和 `5x5` 索引众数滤波；该组合会吞掉小面积色块，是只剩少数主色的直接高风险点。
- `Camera Roll` 当前有三张候选图：`方飞总.png`（1080x1620）、`刘亦菲.jpg`（1080x2400）和 `大雁塔.jpg`（1279x1967）；下一步视觉复核后选择人物/建筑输入验证不同风格。
- 已完成三张图片的视觉复核，并将技术设计写入 `Docs/plans/2026-08-10-style-palette-one-click-gcode-design.md`。首个端到端 3D 验收优先使用完整站姿人物，三风格图片质量可同时用干净背景半身人物交叉验证。
- 已实现 `q_cartoon`、`cyberpunk`、`classical` 三个结构化风格 profile，并在 sidecar/C++ 客户端/GUI 间传递稳定 ID；风格变化被纳入任务快照，旧预览不能继续生成 3D。
- 色板后处理已由“最近色 + 5x5 众数滤波”改为“自适应色簇 + CIE Lab 一对一耗材色匹配 + 可选 3x3 去噪”，并写出 `preview-colors.json`；定向测试证明全部选择色被使用且色板外像素为零。
- Tripo `face_limit` 目标从 20,000 下调到 12,000，为服务端超调和本地 20,000 硬门禁留余量。
- Python 语法、风格提示测试 8/8、色板/低模定向测试 9/9 通过；`libslic3r_gui` Windows Release 单并发构建通过。
- 重新按用户截图复核阶段 19 的四项交互：`3D Generate` 位于 `Prepare` 前；文字、图片或两者任意组合均可继续；左侧首屏压缩为四阶段流程；打印色板支持当前项目自动同步和本页自定义。200% DPI 证据 `09-final-empty.png`、`10-final-image.png` 无裁切或重叠。
- 交付复验通过：AI 预处理、OBJ、色板和 sidecar 契约共 41 项测试通过，启动健康检查正常，`git diff --check` 无新增空白错误；正式 `orca-slicer` PID 41236 与 production sidecar 继续响应。一次因进程名误判启动的重复实例 PID 7608 已通过正常关闭窗口退出。
- 2026-08-11 恢复后复核现有真实图片结果：Q 版 v4 可用；赛博朋克量化图因全局暖色肤色约束将护甲误映射为肤色而不合格。确定改为仅保护上半部面部暖色连通区域，并先用现有 raw 图离线回归，不重复调用付费图片服务。
- 色板算法定向回归 9/9 通过。第三轮离线输出中，背景预压平后赛博朋克主体红、绿、白护甲恢复，Q 版脸部正常；继续修正赛博朋克面部少量灰色高光。
- 色板语义修复完成：灰背景洪泛增加低色度限制，主体聚类改到背景统一之后，原图肤色检测与耗材肤色候选拆分。最终 `cyberpunk-palette-v7.png` 与 `q-cartoon-palette-v7.png` 人工复核通过，定向色板回归 9/9。
- 真实 `gpt-image-2` 古典风格编辑调用成功，产物为 `classical-raw.png`；随后将肤色保护范围按风格限制为 Q 版 39%、正常比例 17%。最终三风格严格色板图 `q-cartoon-palette-v9.png`、`cyberpunk-palette-v9.png`、`classical-palette-v3.png` 均人工复核通过，16 色全部出现且无色板外颜色。
- 本轮真实图片调用累计 3 次成功；真实 Tripo 3D 调用仍为 0，预算剩余 20 次。
- 最新验证：`libslic3r_gui` Release 增量构建成功；AI Python 全量回归 55/55、相关 `py_compile` 与 `git diff --check` 通过。production sidecar `/health` 正常，准备同步运行目录并进行真实 Tripo 黄金路径。
- 阶段 20 第 1 次真实 Tripo generation task 已提交并持久化：`4c907118-0e3e-45cb-8d18-b21f5c1dafef`，输入为 `q-cartoon-palette-v9.png`，用途为 12k 低模/单体/流体/严格色板黄金路径验收；提交后 3D 预算剩余 19/20，当前只轮询同一任务。
- generation `4c907118-0e3e-45cb-8d18-b21f5c1dafef` 与 conversion `a17a0d56-4154-410a-96ae-09fc5a8f23ae` 均成功。最终 OBJ 15,148 面、单连通、封闭流体、无非流形/退化三角、严格色板通过；未创建第 2 个 generation task，预算剩余 19/20。
- 最新 Release 已启动，运行目录 DLL SHA256 与构建产物同为 `26D8F6EDB4F14811A7C9C17419071938D81F6611A2DB7132EEC747CD1E841D1C`。200% DPI 截图 `generated_models/gui-validation-phase20/10-generate-direct-child.png` 证明页面首屏和动态 8 色项目色板显示正常。
- 修复 GUI 验收驱动的 `EN_CHANGE` 通知后，文字-only 输入能够启用 `Prepare 3D prompt`，并进入 Review；本次只调用 `18765` mock sidecar，没有新增真实图片或 3D 任务。
- mock `Generate 3D and slice` 端到端完成：下载 134 字节单体顶点色 OBJ、使用当前项目耗材色映射、自动导入与落床、触发切片并切换到 G-code Preview。全流程未出现普通 OBJ 颜色选择、网格修复、简化或尺寸确认弹窗。
- GUI 证据 `generated_models/gui-validation-phase20/15-after-auto-flow.png` 显示 Preview、四色耗材统计、179 次换料和完整 G-code。日志同时确认 `model_fits=1`、`Exporting G-code finished`、切片状态成功与 G-code viewer 加载完成。
- Preview 的红色“检测超出热床边界的 G-code 路径”来自当前 WonderMaker 多色配置生成的路径检查；导入模型位于热床中央且日志判定模型适配，故不将其归为 AI OBJ 或自动摆盘失败。
- 已正常关闭两个 mock Orca 实例并停止 `18765` mock sidecar。随后显式使用 `OPENAI_BASE_URL=https://laotie.dev` 恢复 production sidecar PID `52776` 与 OrcaSlicer PID `40280`；protocol v1 health 正常，文字、图片与 OBJ 能力均可用。
- 阶段 20 最终状态为 complete。真实 Tripo generation task 仍仅为 `4c907118-0e3e-45cb-8d18-b21f5c1dafef`，conversion task 为 `a17a0d56-4154-410a-96ae-09fc5a8f23ae`，预算剩余 `19/20`。
- 最终交付检查再次通过：AI Python `55/55`、相关模块 `py_compile`、`git diff --check`、构建/运行 DLL SHA256 一致、production `/health` 正常。测试只出现 Pillow `Image.getdata()` 的 2027 弃用警告，不影响当前演示版本。
- `planning-with-files-zh/scripts/check-complete.ps1` 因技能安装文件乱码无法解析；未修改用户技能。改用 UTF-8 计划状态扫描复核阶段 20 为 complete。
- 重新视觉审计发现旧算法为了覆盖全部 16 色会在底座制造彩虹条带；现已改为将耗材色板视为允许集合，三个自然色板离线预览均无色板外像素且至少保留 3 个有效颜色。
- 第 2 次真实 Tripo generation `92c11c8e-e949-4902-9bc6-1566b4536846` 与 conversion `a873c5f1-95a5-4ecb-bf56-0a4a79c2e3fc` 已完成并持久化，输入为 `q-cartoon-natural-v1.png`；真实 generation 预算已用 2/20，剩余 18/20。
- 新真实 OBJ 为 15,613 面、7,820 顶点、严格色板通过，但包含 2 个连通体、45 条边界边和 4 条非流形边，当前门禁拒绝导入。阶段 20 因此恢复为 `in_progress`，不得再以旧 mock 或旧强制全色 OBJ 作为最终完成证据。
- 下一步采用确定性本地修复：sidecar 仅删除相对主体足够小的脱离件；有限开放边进入 Orca 后自动调用 CGAL，强制 `keep_painting=true`，修复失败则保留原始 OBJ 和诊断且不进入切片。
- 2026-08-11 复核 PID `42584` 的第二轮真实 OBJ mock：页面状态为 `Automatic mesh repair failed`，具体错误为 `Repair failed: mesh still open after hole filling.`；没有创建新的付费图片或 3D 任务，真实 3D 预算仍为已用 2/20、剩余 18/20。
- 当前修复方向改为 sidecar 对这个有界局部缺陷执行确定性预修复：移除非流形边关联的小面片、拆分小边界回路并补洞，保持顶点色属于耗材色板；Orca 继续负责最终闭合与 MMU 保色校验。
- sidecar 首版局部拓扑预修复已实现并通过 3 项定向测试；并行运行 `py_compile` 与 `unittest` 曾因同时写 `__pycache__` 触发一次 Windows 拒绝访问，改为串行且测试禁写字节码后全部通过。
- 真实 `natural-q-repairable.obj` 离线修复结果保存在 `generated_models/gui-validation-phase20-natural/natural-q-sidecar-repaired.obj`，诊断为 `mesh-repair-sidecar.json`：15,498 面、单连通、0 异常边，原有 11 色完整保留且无新增颜色。未调用远端 AI。
- 原始 ZIP 完整重放产物位于 `generated_models/gui-validation-phase20-natural/full-sidecar-replay-v1/`，严格拓扑与色板门禁通过。AI Python 全量回归 62/62、`py_compile`、`git diff --check` 和 Windows Release 单并发增量构建均通过；DLL SHA256 为 `E6413D0F9BF0845EDA53542D4D16E345A7F4DC3DDBC8A8EE23CF2AAB329C64D3`。
- GUI 真实 OBJ mock 已自动导入、落床、切片并进入 Preview，截图为 `14-repaired-flow-result.png`。日志确认 `model_fits=1`、单对象、G-code 导出成功和切片状态 0，且无 CGAL 修复失败或撤销。没有新增付费 AI 调用，真实 3D 预算仍剩余 18/20。
- 已停止隔离 Orca 与 `18765` mock，并重启 production sidecar 以加载最新 sidecar 修复代码。正式 sidecar PID `9864` 仅监听 `127.0.0.1:18764`，正式 Orca PID `58360` 保持响应；启动脚本显式使用 `OPENAI_BASE_URL=https://laotie.dev`。production 页面证据 `15-production-restored.png` 显示服务就绪并动态同步当前项目 8 色。
- 阶段 20 最终状态更新为 complete；真实 Tripo generation 仍只使用 2/20，剩余 18/20。

## 2026-08-11 阶段 21：最终需求审计
- 读取 `planning-with-files-zh`、`Code`、`brainstorming` 和 `ui-ux-pro-max` 规则，恢复持久计划与工作区状态。
- 对照用户截图和当前源代码完成四项 UI 需求审计：页面顺序、统一输入、紧凑步骤区、动态/手动耗材色板均已落地。
- 复核 `.workbuddy/phase19-gui-20260810/09-final-empty.png`、`10-final-image.png` 与 `generated_models/gui-validation-phase20-natural/15-production-restored.png`；200% DPI 下布局完整，色板来源和双图预览状态清晰。
- 当前进入三风格用色与生产黄金路径的最终复验；真实 Tripo 预算保持已用 2/20、剩余 18/20。
- 三个自然色板预览人工复核通过：Q 版使用 10 色、赛博朋克 9 色、古典 11 色，均无允许色板外像素。
- 生产 GUI 使用 `刘亦菲.jpg`、Q 版风格、文字+图片和当前项目 8 色成功生成真实预览；任务目录为 `generated_models/9e7ab84e-5963-415d-a977-3ec575c90748`，本次结果实际使用白、绿、黑、棕 4 色。
- 第 3 次真实 Tripo generation 已提交并立即持久化：`1555b6ae-cbae-4a84-88f7-11c02d04ef2f`，用途为生产 GUI 连续黄金路径复验；预算已用 3/20、剩余 17/20，当前仅轮询该任务。
- 第 3 次真实 generation 与 conversion 均成功：conversion task 为 `c2d499d2-99af-483e-b8e7-a8d604f13cac`；OBJ 15,916 面、单连通、0 异常边，删除 1 个 67 面微小脱离件后被 GUI 自动导入并摆盘。
- 首次生产切片被安全门禁拒绝：项目槽 1 为 PLA 220°C（190–240°C），槽 2–8 为 ABS 270°C（240–280°C），真实模型同时使用两组槽位时不存在安全共同温区。
- 实现温度兼容色板选择：自动色板采用最大的互相兼容槽组，OBJ 映射保留原始耗材槽号，界面显示排除数量；不会自动打开“移除混合温度限制”。
- Windows Release 全量目标构建成功，仅有既有 `LNK4098`；一次短超时留下的并发子构建导致 `MainFrame.obj` 权限冲突，确认无残留编译进程后重跑成功。
- 最终运行复核发现旧证据 `08-real-config-compatible-palette.png` 仍停在 Home，`07-compatible-palette.png` 又只使用 5 色隔离配置；两者均废弃，不作为兼容色板验收证据。
- 修复 `compatible_project_slots()` 的数据来源：不再从可能只有单值的 Plater 合并配置读取温度，而是按 `PresetBundle::filament_presets` 逐槽读取实际选中的 WonderMaker PLA/ABS 预设。
- 真实 1 PLA + 7 ABS 项目最终显示 7 个温度兼容色并排除白色 PLA；将状态文案拆为两行后，200% DPI 截图 `generated_models/gui-validation-phase21-production/14-final-compatible-palette.png` 无裁切。
- 使用第 3 次真实生成得到的 15,916 面 OBJ 通过 `18765` mock 重放：自动导入单对象、落床并切片，实际使用原始耗材槽 2、4、5，槽 1 PLA 未参与。日志确认 `model_fits=1`、`Exporting G-code finished` 和 `on_process_completed:finished`，无混合温度错误。
- 最终 G-code 证据为 `generated_models/gui-validation-phase21-production/15-final-gcode-preview.png`；AI 下载 OBJ 位于项目内 `generated_models/downloads/`。
- Windows Release 单并发增量构建通过，仅有既有 `LNK4098`；构建与运行 DLL SHA256 均为 `60632EBD4C6DFB7BA97F5D4C4B2B96D6CF5C818D8DD44CCB7566D05FDDD9B4D5`。
- AI Python 全量回归 `62/62`、相关模块 `py_compile` 和 `git diff --check` 全部通过；仅保留 Pillow 2027 弃用警告。
- 已正常关闭测试 Orca、停止 `18765` mock，并通过 `start_orcaslicer_with_ai.bat` 恢复 production。当前仅 PID `9864` 监听 `127.0.0.1:18764`，正式 Orca PID `59364` 响应正常；production 证据为 `16-production-ready.png`。
- 阶段 21 状态更新为 complete；真实 Tripo generation 总计使用 `3/20`，剩余 `17/20`。

## 2026-08-11 阶段 22：生成页 3D 预览与中文化
- 读取 `planning-with-files-zh`、`brainstorming`、`ui-ux-pro-max` 和 `Code` 规则，恢复阶段 21 完成态、production 进程和脏工作区。
- 用户新增两项需求：生成结果在 `3D Generate` 页面内提供 3D 模型预览；本页相关界面统一为中文。
- 初步审计确认完整 `GLCanvas3D` 与 Plater 强耦合，优先研究基于 `GLModel` 的轻量只读预览；当前自动流程的预览停顿点仍需用户确认。
- 本轮不会新增真实 Tripo 调用，真实 3D 预算保持已用 `3/20`、剩余 `17/20`。
## 2026-08-11 阶段 22：确认交互方案

- 用户确认采用“模型生成后停留在生成页预览，点击确认后再生成 G-code”的流程。
- 确定右侧预览区采用“图片预览 / 3D 模型”两个标签页，保留原图与 AI 结果对照。
- 确定复用 OrcaSlicer 的 OBJ 解析、共享 OpenGL 上下文、着色器和 GLModel，不嵌入完整 GLCanvas3D。
- 已创建设计文档 `Docs/plans/2026-08-11-model-preview-chinese-ui-design.md`。
- 本阶段不触发新的 Tripo 付费任务。

## 2026-08-11 阶段 22：首次增量编译

- 已实现生成页双标签预览、轻量彩色 OBJ 预览控件、模型就绪后的人工确认点和本页中文文案。
- 首次 Windows Release 增量编译在 `ModelGenerationPanel.cpp:300` 失败：将仓库的 `stl_triangle_vertex_indices` 误写为 `Vec3i`，其余诊断均为级联错误。
- 已按 `indexed_triangle_set::indices` 的实际元素类型修正，保持单并发重新编译。

## 2026-08-11 阶段 22：首次 GUI 预览验证

- Windows Release 构建成功，AI Python 回归 `62/62` 通过，`git diff --check` 通过。
- 使用 `18765` mock 重放已有 15,916 面彩色 OBJ；流程正确停在“确认并生成 G-code”，统计显示 `40.1 × 39.9 × 100.0 mm`、3 种颜色，未触发导入和切片。
- 截图发现 3D 画布为空；点击“重置视角”后仍为空，排除标签切换后的单次漏刷。
- 已加入一次性 OpenGL 渲染诊断，下一轮从日志核对上下文、着色器、模型分组和 GL 错误码。
- 诊断版首次编译因当前 GL 头未声明 `GL_CURRENT_PROGRAM` 失败；该值非必要，已移除并保留其他诊断项。
## 2026-08-11 阶段 22：3D 预览空白诊断
- 从 PID 47412 的最新日志确认 OBJ 已解析并显示统计信息，但渲染诊断未写入。
- 对照 `GLCanvas3D::get_canvas_size()` 与 `SkipPartCanvas::Render()`，定位 Windows 200% DPI 下重复乘 `GetDPIScaleFactor()` 导致 OpenGL viewport 被放大两倍，模型中心落在可见画布之外。
- 修正 `ModelPreview3D`：仅 Apple Retina 使用额外 DPI 缩放；切换到“3D 模型”标签后显式刷新画布。
- 将流程提示压缩为“输入 → 图片 → 生成 → 预览 → G-code”，避免窄栏裁切。
- 诊断期间两次 PowerShell/rg 组合查询因引号与 Windows 通配符写法失败；后续拆分并改用明确文件路径完成查询。
- 首轮 DPI 修正构建成功，但真实屏幕截图仍为空白；强制刷新实际 `wxGLCanvas` 后也没有进入原有渲染日志，说明还存在更早的上下文绑定/绘制回调问题。
- 新增一次性绘制分支诊断，分别记录 `context_ok`、`current_ok`、画布可见性及着色器缺失；模型标签显示后再通过 UI 队列延迟一帧重绘。
- GUI 驱动状态查询曾因 PowerShell `foreach` 结果直接接管道产生一次语法错误，已改为先收集数组再格式化输出。
- 分支诊断最终确认 `has_model=true`、`context_ok=true`、`current_ok=true`、`shown=true`，但 `gouraud_light` 尚不可用。
- 根因是“3D 生成”移到“准备”之前后，用户可在 Orca 主 3D 画布首次绘制前进入生成页；新轻量画布遗漏了现有 `GLCanvas3D` 的 `SetCurrent -> init_opengl -> render` 初始化顺序。
- 在上下文绑定后调用幂等的 `GUI_App::init_opengl()`，使生成页可独立完成 GLAD 与全局着色器初始化，不再依赖访问“准备”页。
- 着色器修复后日志确认 `GLAD 4.6`、3 个颜色分组、shader 28、`gl_error=0`，但真实屏幕像素仍是背景色；进一步定位到共享上下文也共享 framebuffer/scissor/blend/color-mask/depth-mask 等渲染状态。
- 轻量预览每帧显式绑定默认 framebuffer 并恢复最小可预测 GL 状态，隔离主画布遗留状态。
- 首次编译 framebuffer 状态隔离时失败：文件未包含 GLAD，导致 `glBindFramebuffer`/`GL_FRAMEBUFFER` 未声明；按 `GLCanvas3D.cpp` 的既有依赖补充 `<glad/gl.h>` 后重编。

## 2026-08-11 阶段 22：最终验收
- Windows Release 单并发构建成功，仅保留既有 `LNK4098` 警告；构建与运行 DLL SHA256 均为 `6FB91EA3F2E30F50BDE1E29DBA689556DB92C9C0BB2D10B12B6053F562051D6E`。
- mock 重放 15,916 面真实彩色 OBJ：生成后停留在“3D 生成”页，显示 40.1 x 39.9 x 100.0 mm 和 3 种颜色，未自动导入或切片。
- 3144x2008 物理像素截图确认模型非空且居中；拖动和滚轮后模型朝向、尺寸发生可见变化。
- 点击“确认并生成 G-code”后才执行导入和切片；日志确认 `Exporting G-code finished` 与 `on_process_completed:finished`，最终进入中文“预览”页并保留彩色耗材统计。
- AI Python 全量回归 62/62、相关模块 `py_compile`、`git diff --check` 全部通过；仅有既有 Pillow 2027 弃用警告和 LF/CRLF 提示。
- 本阶段没有新增真实图片或 Tripo 调用，真实 3D 预算仍为已用 3/20、剩余 17/20。
- **状态：** complete

## 2026-08-11 阶段 27：修复失败手动导入与模型库切片
- 已恢复阶段 26 完成态并核对当前脏工作区，确认本轮只修改生成面板回调、导入失败交互、模型库按钮状态和主窗口页面切换。
- 已确定安全交互：自动修复失败后先撤销失败导入；用户选择“手动导入”时重新导入原 OBJ 到准备页，但不自动切片。
- 已确认模型库现有双击预览和本地 OBJ 优先导入逻辑可复用，本轮增加明确的“导入并切片”状态与回归验证。
- 已创建设计文档 `Docs/plans/2026-08-11-manual-import-library-slicing-design.md`。
- 本阶段不触发新的图片或 Tripo 付费任务。
- 首次 Windows Release 增量构建定位到单一编译错误：普通 `MessageDialog` 不支持自定义 Yes/No 文案；已切换为项目现有的 `RichMessageDialog` 后继续构建。
- 自动修复失败后现在提供“手动导入 / 取消”；选择手动导入只进入准备页，不自动切片，原始 OBJ 与诊断继续保留在 `generated_models`。
- 模型库双击仍只加载 3D 预览，按钮明确显示“导入并切片”；真实历史 OBJ 已完成导入、切片和 Preview 验证。
- 主窗口完成回调已区分“进入准备页”和“切片并进入预览”，不会让开放网格或自动上色失败模型绕过安全门禁。
- **状态：** complete

## 2026-08-11 阶段 28：模型导入颜色策略解耦
- 用户确认当前“关闭可打印颜色即按单耗材导入”的行为不符合预期，要求默认正常导入，并可选单色导入。
- 已核对 Orca 原生 OBJ 导入：`ObjImportColorFn == nullptr` 时打开标准颜色映射流程；当前空回调才是强制单色的直接原因。
- 已确定自动上色失败的降级：保留已导入模型、进入准备页、不自动切片，用户可使用 Orca 手动上色。
- 已创建设计文档 `Docs/plans/2026-08-11-model-import-color-mode-design.md`，本阶段不调用付费 AI。
- 已新增“导入颜色”选择，默认“正常导入（保留颜色）”，可显式切换“单色导入”；“使用可打印颜色”仅控制 AI 生成色板约束。
- 自动上色失败真实 GUI 验证通过：无顶点色封闭 OBJ 被保留到准备页，未触发 G-code，页面提示可手动上色；证据为 `generated_models/gui-validation-phase28/04-auto-color-fallback-prepare.png`。
- 模型库双击预览和“导入并切片”验证通过；显式单色导入时发现 G-code 导出后被擦料塔配置归一化作废，首次修复误写 project config，未覆盖当前打印预设的有效值。
- 已改为在单色自动切片前更新当前打印预设的 `enable_prime_tower=false`，并同步打印预设、项目脏状态和 Plater 完整配置。
- Windows Release 增量构建成功；构建与运行 DLL SHA256 均为 `F7554B849B52521B86C7553A50AAE19C5DF44FE933A736865E0F20F4BF39BD9D`，`git diff --check` 通过。
- 使用模型库真实 OBJ `AI 模型 4ae4d7e9`（14,502 个三角面）完成单色切片：日志记录一次 `Exporting G-code finished`、一次 `0 -> 1` 和一次 `on_process_completed:finished`，`1 -> 0` 次数为 0。
- 最终预览显示实际刀路、18.11g 用料和 3h3m 估算；证据为 `generated_models/gui-validation-phase28/11-library-single-gcode-valid.png`。
- 正式 Orca PID `48100` 保持响应，production sidecar 继续使用 `127.0.0.1:18764`；本阶段没有调用图片或 Tripo 付费能力。
- **状态：** complete
# 2026-08-11 阶段 29：高精度生成、OBJ 结构分组与颜色保真

- 用户确认生成质量改为 10 万、30 万、50 万、100 万面四档，默认 30 万面。
- 用户撤销此前“只要一个对象”的要求；正常导入应保留多个对象/部件、材质边界和相对位置。
- 审计确认现有 OBJ 每个顶点均有 RGB；颜色偏差主要来自 216 色粗量化、UV/材质颜色投票合并和结构分组丢失。
- 已创建 `Docs/plans/2026-08-11-high-detail-obj-color-groups-design.md`，下一步实现请求契约、GUI、sidecar 与回归。
- 本阶段不会在未再次确认的情况下创建付费 Tripo 任务。
- 实现时根据 Orca 拓扑模型修正了 UV 接缝方案：不复制几何顶点，避免人为制造开放边；自然色改为全精度 RGB 平均，耗材色板开启时才量化。
- 已完成高质量 Tripo 参数、四档面数请求、100 万面门禁、单次生成任务、多连通部件保留、对象/组标记保留和自然色精度修复。
- Python 语法、OBJ 生成、正式/测试 sidecar 契约共 54 项回归通过；未调用付费 3D 服务。
- 正式 Orca PID `61376` 与 production sidecar `127.0.0.1:18764` 响应正常；通过 Windows UI Automation 进入 `3D 生成` 页面，全程未点击生成按钮。
- 3144x2008 物理像素截图确认质量控件无裁切/重叠，默认选择 `30 万面（推荐）`；展开后 10 万、30 万、50 万、100 万四档文案均完整显示。
- GUI 验收证据为 `generated_models/gui-validation-phase29/03-generation-page-full.png` 和 `generated_models/gui-validation-phase29/04-quality-options.png`。
- 阶段 29 的 GUI、sidecar、颜色、多部件保留、回归和 Windows Release 构建项已完成；仅保留经用户明确授权后的付费 Tripo 端到端验收。

# 2026-08-11 阶段 30：质量优先的付费黄金路径验收

- 用户明确授权使用付费流程，Tripo 3D 生成总数硬限制为 20 次以内，质量优先。
- 图片阶段先关闭打印耗材颜色约束；三种现有风格均需保证主体外不新增内容，并适合后续 3D 打印成模。
- 采用分层漏斗：先生成三张真实风格预览，选择通过门禁的最佳图生成 30 万面 OBJ，只有明确质量问题才追加 50 万或 100 万面。
- 已创建 `Docs/plans/2026-08-11-quality-first-paid-golden-path-design.md`；付费 3D 计数从 `0/20` 开始。
- 正式 sidecar 已加载 `orcaslicer-ai-sidecar-v4`；首次手动重启命令因安全策略拒绝动态拼接，改用项目自带 stop 脚本与固定隐藏启动参数完成。
- Q 版付费预览 job `c4f5af5d-88d3-4d05-ba53-3ccdb775e07d` 成功；首次本地请求漏传 `X-OrcaSlicer-Client: native` 被 401 拦截，未触发外部调用，补齐后成功。
- 低多边形 sidecar 任务 `b3a30e58-5ba6-431b-be6d-b4fff2b03de2` 在预处理期间因 sidecar 进程被替换而丢失；外部是否计费无法本地确认，保守计入一次图片尝试，但 3D 计数仍为 `0/20`。
- 新增 `tools/ai/run_paid_style_preview_validation.py`，要求显式付费确认、使用空色板自然色模式，并在输出存在时拒绝重复付费调用。
- 低多边形与雕塑单进程付费预览成功；三种风格均未新增人物、道具、底座、文字或背景装饰。三次成功图片结果加一次中断尝试，共保守记录 4 次图片调用。
- 选择 Q 版作为首个 30 万面彩色 3D 输入；付费 Tripo 计数仍为 `0/20`。
- 第 `1/20` 个付费 Tripo 任务 `d960d74e-4801-4dd5-9d1f-af42982653b9` 成功，conversion task 为 `8218a284-2806-4633-82e2-f9fa67b9c3eb`。
- 产物为 296,642 面、148,323 顶点、单连通封闭流体，0 边界边、0 非流体边、0 退化面，自动修复无需运行；自然色顶点 RGB 已写入 OBJ。
- 验证输出最初逐项打印数万种自然色，已改为记录颜色总数与前 64 个主色，避免日志和 JSON 过大。
- 已实现准备/预览共用的六步自动流程侧栏，依次显示模型导入、网格检查/修复、颜色处理、自动摆放、切片和 G-code；流程完成后继续保留状态，后续普通切片不覆盖。
- Visual Studio CMake 不在当前 `PATH`，改用 VS 2022 Community 自带 CMake 绝对路径；使用 `/m:1`、`CL_MPCount=1`、`UseMultiToolTask=false`、`BuildInParallel=false` 后，`libslic3r_gui` 与 `OrcaSlicer` Release 均构建成功。
- 代码复核将“自动摆放”改为导入回调完成后才显示成功，并在此后才进入切片进行中；修正后的两个 Release 目标再次构建通过。
- 最终新构建 DLL 为 `build/src/Release/OrcaSlicer.dll`，SHA256 `D270262CA139637EE110B13450C9C136ABDDB45CD95B1980789D83B797B81144`；仅保留既有 `NOMINMAX` 重定义、`LNK4098` 和未使用局部变量警告。
- AI Python 全量回归 `75/75`、全部 `tools/ai/*.py` 的 `py_compile` 与 `git diff --check` 通过；测试未触发付费图片或 Tripo 调用，仅保留 Pillow 2027 弃用警告。
- 日常运行目录仍由响应中的 Orca PID `61376` 占用旧 DLL，production sidecar PID `62752` 监听 `127.0.0.1:18764`；`LockApp`/`LogonUI` 证明桌面仍锁定，不发送坐标操作，也不强制关闭窗口，待解锁并正常关闭后部署及完成真实 GUI 验收。
- 桌面短暂可用后向旧 Orca PID `61376` 发送正常关闭请求，进程在 10 秒内自行退出；备份旧 DLL 后部署新 DLL，构建与运行目录 SHA256 均为 `D270262CA139637EE110B13450C9C136ABDDB45CD95B1980789D83B797B81144`。
- production sidecar 已以隐藏批处理恢复，`/health` 确认 v4、`https://laotie.dev`、OBJ-only 以及 10/30/50/100 万面能力；正式 Orca PID `32408` 加载新 DLL 并响应正常。
- 从 Tripo 已有 generation 结果下载 512px 模型渲染，不创建新任务；URL 只接受 HTTPS 且主机精确为 `openapi.cdn.tripo3d.com`。`clawdefender` 因当前 Windows 无 Bash 无法直接运行，采用技能文档中的同等 SSRF allowlist 门禁。
- 直接解析最终 OBJ 的 148,323 个顶点和 296,642 个三角面，生成 `obj-orthographic-vertex-colors.png`、`obj-front-triangle-render.png` 与 `comparison-style-preview-vs-obj.png`。三角面填充渲染确认顶点色正常，人物、发型、肤色、眼睛、嘴、白外套、绿色内搭、交叉手臂和手表均与风格图一致。
- 当前可见差异为 3D 细节略微平滑、肩部略宽；衣摆和下装范围跟随输入图裁切，没有新增人物、道具、底座或其他几何内容。桌面再次进入 LockApp，GUI 导入、侧栏和 G-code 验收继续保持未完成。
- 第三次连续恢复仍确认前台为 `LockApp`，正式 Orca PID `32408` 在后台响应正常。检查 CLI 后确认裸 OBJ 切片需要额外机器/工艺/耗材配置，且仍不能证明模型库、六步侧栏和 Preview 交互，因此不以 CLI 结果替代用户路径验收。
- 阶段 30 当前仅被 Windows 锁屏阻塞；解锁后从已部署的新 DLL、真实模型库条目 `d960...` 和现有对比证据继续，无需新增付费图片或 3D 任务。
- 2026-08-12：恢复阶段 30 验收。确认正式 sidecar v4 健康、Base URL 为 `https://laotie.dev`、OBJ-only 且支持 10/30/50/100 万面；Tripo 付费使用仍为 1/20，本轮未创建新任务。定位模型库屏幕首项与磁盘不一致的原因是启动期缓存，准备重启 Orca 触发重新扫描后继续加载 `d960d74e...`、导入和切片。
- 2026-08-12：正式 Orca 重启后模型库重新扫描，`d960d74e...` 正确显示为第一条正式高质量模型（故障夹具之后）；双击加载成功，GUI 数据为 296,642 面、22 色、40.0 x 43.2 x 100.0 mm。首次导入暴露两个黄金路径缺陷：正常导入仍弹 OBJ 颜色确认，内部取消后虽自动重切并成功生成 G-code，六步侧栏却停在失败。已修改 `ModelGenerationPanel.cpp` 主动映射顶点色并在失败时转手动上色，修改 `Plater.cpp` 让内部重切保持工作流活跃，同时重置新工作流摘要颜色；待 Release 构建与重新 GUI 验收。
- 2026-08-12：上述两处修复通过 `git diff --check`，`libslic3r_gui` 与完整 `OrcaSlicer` Release 单并发构建成功；新 `build/src/Release/OrcaSlicer.dll` 于 10:12 生成，准备部署并复跑真实 GUI 黄金路径。
- 2026-08-12：真实 `d960d74e...` 首次稳定结果在 `0 -> 1` 和 G-code 加载后触发 `crash_Wed_Aug_12_11_58_31_0.log`；调用栈落在完成确认定时回调 `Plater.cpp:10286`。根因是临时 `wxTimer` 在自身事件回调中删除及 UI 生命周期重入风险，已改为 `Plater::priv` 持有的单次定时器并由统一计时器事件处理。
- 2026-08-12：修复后真实模型库流程再次完成，日志记录 `Exporting G-code finished`、`0 -> 1`、`on_process_completed:finished`，完成确认后进程保持响应且没有新增 crash；六步状态均显示完成。
- 2026-08-12：实际全屏刀路截图发现当前 WonderMaker 打印预设的擦料塔越过热床边界。AI 自动切片现统一关闭 `enable_prime_tower`，并将完成门禁从“切片结果存在”提高为 `is_slice_result_ready_for_export()`；越界、G-code 检查或耗材可打印性错误会显示失败，不再错误全绿。
- 2026-08-12：最终复跑使用同一已付费 OBJ，未调用图片或 Tripo API。正式 GUI 显示 296,642 面、22 色、封闭网格、自然色映射和六步完成；75,846,512 字节 G-code 已映射到 Viewer，打印按钮可用，完成后配置应用 `invalidated=0`，没有 `1 -> 0`。
- 最终证据：`generated_models/gui-validation-phase30/25-final-screen-gcode.png` 记录真实刀路与统计；`27-final-printable-gcode-stable.png`、`28-final-printable-gcode-window.png` 记录关闭越界擦料塔后的稳定六步状态。窗口尺寸切换后 OpenGL 画布存在延迟重绘，但日志、G-code 文件、打印按钮与可导出门禁均确认最终结果有效。
- 最终回归：AI Python `75/75`，`py_compile` 通过，C++ `[Model][OBJ][MMU]` 为 1 个用例/5 个断言全过，Windows Release 构建与 `git diff --check` 通过。部署 DLL SHA256 为 `9D0582BB678161FAB3210DBF803FE8FA2E89264161EB938036C8910D62729BD3`。
- 阶段 30 完成；Tripo 付费使用保持 `1/20`。
- 2026-08-12 12:45 单模型干净复跑完成：先新建空项目并放弃保存 AI 流程对预设的临时修改，再从模型库加载既有 `d960d74e...`。日志确认 `load_model_objects` 前对象数为 1、`Print::process` 的 `object size=1` / `total object counts 1`，归档仅生成 `model-vertex-color.obj_1.model`。
- 本次单模型切片于 12:46:07 导出新的 38,127,361 字节 G-code，状态从 `0 -> 1`，随后 `on_process_completed:finished` 且 `invalidated=0`；同一日志段没有 `gcode path conflicts found`、`toolpath_outside`、`1 -> 0` 或新增 crash。
- 最终证据 `generated_models/gui-validation-phase30/36-clean-single-model-gcode-hwnd.png` 同时显示单个模型缩略图、六步全绿、真实 G-code 行、耗材/时间统计和可用打印按钮；此前 `25-final-screen-gcode.png` 的双模型冲突已通过本次空项目单模型复跑排除。
- 正式 Orca PID `8480` 持续响应，实际加载 `build/OrcaSlicer/OrcaSlicer.dll`，SHA256 仍为 `9D0582BB678161FAB3210DBF803FE8FA2E89264161EB938036C8910D62729BD3`；本轮未调用图片或 Tripo API，付费使用保持 `1/20`。
- 2026-08-12 诊断用户报告的正常导入单色问题：未修改代码、未调用付费服务。确认生成页自然色预览正常，最终 OBJ 顶点 RGB 完整，但 OBJ 只有一个对象/组且无材质边界；同时定位正常导入错误复用了耗材色自动映射回调，会把自然色压到当前兼容耗材槽。下一步应分别修复正常导入语义和后续生成产物结构，不能用拆对象替代颜色映射修复。
- 2026-08-12 阶段 31：正常导入已改用全部有效项目槽，并从 OBJ 顶点色聚类出项目主色；真实正式 OBJ 日志为 `source_colours=2, mapped_colours=4, applied=true, collapsed=false`，G-code 中 4 个耗材均有实际用量。进一步确认左侧色块未更新是因为主色写入后错误用 `full_config()` 通知，耗材预设覆盖了项目色；现改为直接传播 `project_config`、刷新耗材控件并标记项目修改，待重新构建和 GUI 复验。
- 2026-08-12 阶段 31 完成：正式高精度 OBJ 的正常导入已验证产生 4 色 G-code，四槽用量分别为 19.29g、8.77g、11.96g、22.53g；证据为 `generated_models/gui-validation-phase31/17-final-multicolor-import.png` 和 `18-stable-gcode-multicolor.png`。左侧色块未同步的根因已修复为直接传播 `project_config`，路径与 Orca 原生耗材颜色编辑一致。Windows Release 构建成功，构建与部署 DLL SHA256 均为 `9AA6FCB9A52E238346E3640FD9375BD7556DBF6D7BEED6350099F305562284AF`，`git diff --check` 通过；正式 Orca PID `54216` 响应正常。本轮未创建图片或 Tripo 任务，付费使用保持 `1/20`。
- 2026-08-12 阶段 32 开始：在 3D 生成页导入颜色选项下新增默认开启的“导入后自动切片”。关闭后仍执行颜色映射、网格检查/修复与自动摆放，但不修改自动切片专用打印配置，也不触发切片；生成结果和模型库导入复用同一路径与开关状态。
- 2026-08-12 阶段 32 隔离 GUI 初验：新版开关默认开启、中文文案完整且布局正常；关闭后从模型库加载已有 OBJ，主按钮正确显示“导入到准备页”。初验同时发现模型预览说明仍写“导入并切片”，已继续将顶部流程、预览说明和结果摘要统一接入开关状态。
- 2026-08-12 阶段 32 完成：最终 Windows Release 构建成功，DLL SHA256 为 `15B9A84B5B4F1D6647C42FA5D49DC514FFAA6728DC9FADA2BA0D9CB6A7512C32`。隔离正式 GUI 中验证开关默认开启；关闭后顶部流程切换为“准备页”，模型库按钮与说明均同步变化。实际导入已有 OBJ 后停在准备页，第 5 步为“等待手动切片”、第 6 步为“手动切片后生成”，“切片单盘”可用而“打印”不可用，确认未自动生成 G-code。未调用图片或 Tripo 服务；旧正式 Orca PID `54216` 因项目带未保存标记而保持运行，运行目录 DLL 未强制覆盖，新 DLL 已保留在构建目录及隔离验证目录。
- 2026-08-12 阶段 32 部署补验：用户正常关闭 Orca 后确认启动脚本固定运行 `build/OrcaSlicer/orca-slicer.exe`，而该目录仍是阶段 31 DLL，因此重开后看不到新开关。现已将最终 Release DLL 正式同步到运行目录，构建与运行 DLL SHA256 均为 `15B9A84B5B4F1D6647C42FA5D49DC514FFAA6728DC9FADA2BA0D9CB6A7512C32`，等待从正式脚本启动复验。
- 2026-08-12 阶段 32 正式部署完成：已从 `build/OrcaSlicer/orca-slicer.exe` 启动正式实例并打开 3D 生成页，确认“导入后自动切片”开关实际显示且默认开启；production sidecar 保持 v4、`https://laotie.dev` 和真实 OBJ 能力。正式窗口保持运行供用户继续测试。
# 2026-08-12 阶段 33：导入颜色匹配修复

- 已根据用户截图复核 AI OBJ 导入、原生 `ObjColorDialog` 触发条件和耗材色块同步路径。
- 确认当前问题不是弹窗显示故障，而是“正常导入”自动回调绕过弹窗并覆盖项目耗材色导致的语义错误。
- 已确定修复边界：默认手动匹配；自动匹配和单色作为显式选项；取消/退化阻止自动切片；不调用任何付费服务。
- 已将“导入颜色”改为三项：手动匹配打印机耗材（推荐）、自动匹配当前耗材、单色导入；默认手动匹配并直接复用 Orca 原生 `ObjColorDialog`。
- 手动匹配会统计用户确认后实际使用的耗材槽；多色模型被全部映射到一个槽、用户取消或映射为空时，颜色步骤显示警告并禁止自动切片。
- 已删除正常导入写回 `project_config.filament_colour` 的逻辑，并移除自动映射助手中覆盖打印机色块的能力；自动模式只会将模型色就近映射到现有兼容耗材槽。
- `libslic3r_gui` 与完整 `OrcaSlicer` Windows Release 单并发构建、最终链接和 `git diff --check` 均通过；最终 DLL 哈希记录见本阶段后续部署条目。
- 当前正式 Orca PID `58784` 标题带未保存标记并加载旧 DLL，未关闭或覆盖。隔离运行副本被单实例机制拒绝，因此真实 OBJ GUI 验证与正式部署留待该窗口正常关闭后完成。
- 当前构建树未生成测试项目：`libslic3r_tests` 目标不存在且 `ctest -N -C Release` 返回 0 项。本轮未调用图片或 Tripo 服务。
- 最终审查确认模型库和新生成模型都调用 `import_local_artifact()`，三种导入颜色策略行为一致；手动取消、映射退化和自动失败分别使用准确中文状态，不再把手动取消描述成“自动上色失败”。
- 最终 Windows Release DLL SHA256 为 `BB5B51E39D2AB0642E2EE6A784D4E26A4A67C36DD5B29B24010AFB0E7035BA6C`，构建产物与隔离副本一致；日常运行目录仍保留旧哈希 `15B9A84B5B4F1D6647C42FA5D49DC514FFAA6728DC9FADA2BA0D9CB6A7512C32`，等待未保存窗口正常关闭后部署。
# 2026-08-13 阶段 35：正式模型生成任务持久化与恢复

- 用户授权继续实现正式模型生成，图片和 3D 均可使用收费 AI；本阶段 3D 付费调用无需逐次确认，硬上限 10 次。
- 完成正式链路审计，确认最大生产缺口为 sidecar 重启丢失内存任务，导致已付费任务无法恢复且 GUI 返回 job not found。
- 在 `tools/ai/orca_ai_sidecar.py` 增加原子 `job.json`、启动恢复、最近任务、参考图下载和远端 generation/conversion ID 续跑。
- 在 `AIModelGenerationClient` 与 `ModelGenerationPanel` 增加最近任务发现、文字/图片/风格/色板/质量/预览/OBJ 恢复。
- 修复图片恢复请求竞态；删除任务会清除任务清单但继续保留用户生成和诊断文件。
- AI Python 全量回归 80/80，`py_compile` 与 `git diff --check` 通过；没有调用图片或 Tripo 付费 API，计数 0/10。
- Windows Release 完整链接成功，最终构建文件 `build/src/Release/OrcaSlicer.dll` 时间为 2026-08-13 22:29:03；待部署并进行正式 GUI/真实 AI 验收。
- 2026-08-14 创建一次真实收费 3D 任务 `8267ded0-c96c-4c46-b263-cdc92d49891d`，付费计数更新为 1/10；generation ID 为 `c3e2c96a-2be8-411a-ab4f-24f7bcdff20f`，conversion ID 为 `b740163c-c7e6-48be-9937-feb7504e88f2`。
- 在隔离的正式 sidecar `127.0.0.1:18766` 上主动中断并重启任务，确认始终复用同一组远端 ID，尝试次数保持 1，没有新增收费任务。
- 修复官方 CDN 在本机代理环境下被安全校验误拒、10 万面目标严格下限误拒 95,338 面产物、以及恢复时重复解压已有 `package` 目录三个真实问题。
- 恢复任务最终进入 `ready / 100%`；OBJ 离线验收为 47,671 顶点、95,338 三角面、2 个连通部件、0 个异常边，顶点色有效，下载接口与磁盘文件 SHA256 完全一致。
- 最终 AI Python 全量回归 90/90，`py_compile` 与 `git diff --check` 通过；本轮未新增第二个图片或 3D 付费任务。
- 当前正式 Orca PID `40336` 标题为 `*未命名 - OrcaSlicer`，仍有未保存内容且加载旧运行目录 DLL；为保护用户状态，未强制关闭、未覆盖运行 DLL，正式 GUI 部署验收待正常关闭后继续。
- 2026-08-14 用户正常关闭未保存窗口后完成正式部署：运行目录 DLL SHA256 为 `EE75391125DE1AD4DEBC5E5621C97F4D8F9E6DCBED6170F5F4FBB4C0C5DF3DB7`，旧 DLL 备份为 `build/OrcaSlicer/OrcaSlicer.dll.pre-phase35.BB5B51E39D2A.bak`。
- production sidecar 重启后保持 v4、`https://laotie.dev` 和真实 OBJ 能力；最新任务正确恢复为 `8267ded0-c96c-4c46-b263-cdc92d49891d / ready / 100%`，未创建新 generation 或 conversion ID。
- 正式 GUI 验收通过：恢复原文字、Q 版卡通、10 万面档、自然颜色、95,338 面/21 色彩色 3D 预览；准备页往返后预览保持，模型库首项显示本次 4.6 MB 自然色 OBJ，历史条目继续存在。
- 阶段 35 完成，3D 付费计数最终保持 1/10；没有点击重新生成、G-code 或发起图片付费任务。

# 2026-08-14 阶段 36：双主线解耦架构

- 用户决定近期只并行开发“模型生成”和“智能切片”，交互、账号与计费后置。
- 开始基于真实代码盘点耦合点，并设计兼容 Orca 上游演进的模块边界、团队所有权和迁移顺序。
- 首次 PowerShell 文件统计命令因 `foreach` 后直接接管道而解析失败，已改用数组收集方式，不重复原命令。
- 完成当前 C++、Python sidecar、Orca 核心 diff、构建接入点、参数建议链路和既有架构资料审计。
- 确定推荐方案为“同仓模块化单体 + 模块化 sidecar + Orca 防腐适配层”，模型生成只发布不可变制品，智能切片通过工作区端口消费制品。
- 新增 `Docs/architecture/06-model-generation-smart-slicing-decoupling.md`，包含代码现状、三方案比较、目录、契约、状态机、双人分工、上游同步和五步迁移路线。
- 新增 `Docs/architecture/ADR-001-ai-modular-monolith-orca-adapter.md`，并更新架构资料索引。
- 文档变更通过 `git diff --check`；本阶段未修改业务代码、未构建程序、未调用任何收费 API。

# 2026-08-14 阶段 37：模型生成线与 Orca 第一批等价解耦

- 用户确认本人负责模型生成主线，并授权继续实现与 Orca 的解耦。
- 采用第一批等价迁移：制品/色板/导入契约、Orca adapter、Panel 去 `Plater` 化；不改变交互或算法。
- 恢复脚本返回了另一会话的旧架构图上下文，已识别为过期数据并忽略其动作建议。
- 完成第一项契约冻结：新增 provider/Orca/wx 无关的 `GeneratedModelArtifact`、`PrintablePaletteSnapshot`、`ModelImportRequest/Result` 和两个窄端口，并登记 CMake。
- 契约文件定向 `git diff --check` 通过；一次规划文件补丁因混用了两个文件的上下文而未应用，已拆分处理。
- 新增 `OrcaWorkspaceAdapter`：集中实现打印色板快照、OBJ 三种颜色策略、导入 snapshot/undo、开放边检查、CGAL 修复、手动导入降级、切片前配置和现有六步兼容展示。
- `ModelGenerationPanel` 已改为只依赖 `IModelArtifactConsumer` 与 `IPrintablePaletteProvider`；导入时发布 typed artifact/request，根据 typed result 投影中文状态。
- 定向搜索确认 Panel 内已无 `m_plater`、`Plater::`、`Sidebar::`、`ObjColorDialog`、CGAL 修复或 `PresetBundle` 引用，相关 diff 空白检查通过。
- `MainFrame` 已成为薄组合根：持有 `OrcaWorkspaceAdapter`，只传入已有的页面导航/切片回调，并将同一 adapter 的两个端口注入 `ModelGenerationPanel`。
- 新增契约、adapter、Panel 和 MainFrame 三个关键翻译单元均完成 Windows Release 定点编译；修正了深层目录 include 和 `glsafe` 间接 include 两个编译问题。
- 使用 MSBuild `_Lib` 将 362 个现有对象重新归档到 `libslic3r_gui.lib`，随后以 `_Link` 完整生成 `build/src/Release/OrcaSlicer.dll`；DLL SHA256 为 `7632896D8F90D638E2070DABB479D48202CEE4511E6CF6D44BE97DC099E9D35A`，只有项目既有 `LNK4098` 警告。
- AI Python 全量回归 90/90、全部 `tools/ai/*.py` 的 `py_compile`、静态 Orca 边界搜索和 `git diff --check` 均通过；没有发起图片或 Tripo 收费调用。
- 当前 CMake 构建树没有 CTest 目标（0 项）。日常 Orca PID `39372` 带未保存标记并使用运行目录 DLL，本轮未强制关闭或覆盖；新解耦 DLL 保留在构建目录，待正常关闭后再部署。
- 阶段 37 完成：模型生成页面与 Orca 工作区业务逻辑已建立可编译、可链接的第一批反腐边界，交互和现有导入/修复/切片语义保持不变。

# 2026-08-14 阶段 38：双机协作 Git 基线审核、精简与发布

- 用户要求审核、精简当前全部修改并同步到 Git，同时将两台电脑协作开发说明写入根 README。
- 采用独立共享集成分支，不直接推送 `master`；正式源码、测试和架构文档进入提交，本机配置、构建/打包输出、生成模型和过程文件只保留在本地。
- 当前仓库已有 `origin` 团队 fork 和 `upstream` Orca 官方远端；`master@a1ef7204fe` 与 `origin/master` 一致，工作树共 57 项修改/未跟踪内容，必须先建立受审计基线。
- 规划文件原始字节确认仍为正确 UTF-8；一次工具显示乱码不代表磁盘损坏，没有执行编码重写。
- 初步盘点得到 27 个 tracked 修改和约 30,276 个 untracked 文件；`output/` 单独约 1.27 GB，确认本轮必须采用白名单 staging，而不是 `git add .`。
- 本地 `build/`、`generated_models/` 和 `output/` 均只保留在磁盘；下一步逐项审核 `packaging/`、`scripts/`、`Docs/`、`website/` 与正式 AI 测试的提交价值。
- 密钥扫描未发现真实凭据；确认 `.claude/settings.local.json`、`.codex-recovery/`、独立 `website/` 仓库和 PPT `projects/` 必须排除。
- 打包模板、正式启动脚本和发布脚本使用空 Key 与正式 sidecar 白名单，可以进入源码基线；Mock 仅保留为测试依赖，不进入产品包。
- 完成 `MeshBoolean`、`Plater`、`MainFrame` 和 CMake 第一轮差异审查；高风险几何补丁需独立 commit，GUI/adapter 变更组成主 AI 基线。
- 完成 sidecar 解包、loopback 服务、外部下载重定向和大小门禁审计，未发现需要阻止提交的高危安全问题。
- 本机没有额外 lint/secret-scanner 工具；采用现有编译器、90 项测试、AST 结构检查和文件名级密钥扫描，不为本轮临时安装新依赖。
- `.gitignore` 已补充本机 agent 状态、API 本地配置、`output/`、PPT 工程、独立网站仓库和旧导出图；这些文件保留本地但不再污染 Orca status。
- 将发布配置改为 `ai-config.example.bat` 源模板，打包时才复制为可填写的 `ai-config.bat`，降低真实 Key 被提交的风险。
- 根 README 已加入双人职责、共享契约、分支、PR、日常同步、上游 Orca 合并和测试门禁说明。
- 已创建本地共享集成分支 `codex/ai-integration-20260814`，`master` 未移动。
- PowerShell 发布/健康检查脚本语法解析通过；配置模板的 `OPENAI_API_KEY` 与 `TRIPO_API_KEY` 均为空。
- 核心 `MeshBoolean.obj`、`Model.obj` 和三个 GUI 解耦对象均晚于对应源码，确认当前 Release 产物包含这些修改。
- AI Python 全量回归 90/90、全部 Python 文件 `py_compile` 通过；没有调用真实外部图片或 3D API。
- `libslic3r_cgal`、`libslic3r`、`libslic3r_gui` 重新归档成功，完整 Windows Release `OrcaSlicer.dll` 链接成功，仅有项目既有 `LNK4098` 警告。
- 已提交两个可独立审查的 Orca 核心补丁：`29b7e7d077`（OBJ 非基础耗材面保留及测试）、`0969951d96`（CGAL 可修复边界闭合）。
- AI 正式链路、Orca adapter、回归测试、发布模板和本机文件忽略规则已提交为 `f8ddcb3a9d`；暂存内容约 1.98 MB，密钥、非空 Key 和个人绝对路径检查通过。
- 文档提交候选已清除两处个人图片绝对路径、尾随空格和末尾空白行；根 README 协作入口包含在内。
