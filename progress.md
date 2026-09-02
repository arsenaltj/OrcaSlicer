# 进度日志

# 2026-08-17 阶段 54 启动：生成页紧凑排版

- 用户在正式界面截图中指出左侧文字间距松散，并确认高级设置折叠入口显示为空白条。
- 决定使用明确的“显示高级设置 / 收起高级设置”文字按钮替代原生折叠控件，同时统一压缩字段、分组与输入框间距。
- 本轮只修改独立 3D 生成面板 UI，不改任务契约、生成服务、图片产物或付费链路。

# 2026-08-17 阶段 54 完成：紧凑排版与明确高级设置按钮

- 页面、旅程区、字段组和高级参数改用 4/6/8/12 DIP 的紧凑间距，左侧有效信息密度明显提高。
- 主体描述和自定义风格框保留自适应高度，同时移除右侧无必要的上下滚动箭头。
- 用 `显示高级设置 / 收起高级设置` 全宽文字按钮替换原生折叠控件；正式 GUI 和无障碍树均能稳定读取当前动作。
- 展开后颜色用途与打印尺寸保持紧凑排列，收起后不留空白标题条；展开/收起不改变 Orca 主窗口尺寸。
- Windows Release 构建、AI Python 179/179 和 `git diff --check` 均通过；运行 DLL SHA-256 为 `2E5C8BF71D8DCEDB848BA23C37A751AD7F0B6ED8203E059FC55FD7FFC81C133D`。
- 正式 Orca 保持打开在 3D 生成页，右侧继续显示 AI 原图与可打印清理对比；未新增 Image2 或 Tripo 调用。



# 2026-08-15 阶段 42：3D 生成页面渐进式交互

- 将 3D 生成页重构为四步旅程：输入、图片确认、生成 3D、导入；顶部步骤和进度固定显示。
- 左栏改为“固定旅程区 + 可滚动阶段设置 + 固定状态/操作区”，解决主按钮被长参数列表推到屏幕外的问题。
- 首屏只保留文字、图片、风格、可打印颜色和折叠的高级约束；模型精度在确认阶段出现，导入颜色与自动切片在模型就绪阶段出现。
- 右栏标签统一为“图片确认 / 3D 预览 / 历史模型”，保留图片阶段切换、3D 旋转和双击历史模型能力。
- 模型精度不再参与图片预览过期判断，并在正式提交 3D 前读取当前选择；图片确认后可直接调整 10/30/50/100 万面。
- 修复历史模型无远程任务 ID 时被错误重置为第 1 步的问题；95,338 面、21 色红熊猫 OBJ 正确进入第 4 步、95% 和导入设置。
- 清理最终状态残留的禁用预处理按钮，最终固定操作区只显示“导入并自动切片”和“重新开始”。
- `libslic3r_gui` Release 串行增量编译通过，`OrcaSlicer.dll` Release 链接通过；仅保留项目已有的 `LNK4098` 默认库警告。
- 最终构建和运行副本 SHA256 均为 `A0503FA544088E966E770EA7032A5E87A301D91D117BA8A151FB01DBFCB9E3E5`。
- 正式验证实例 PID 222500 使用 `build/OrcaSlicer/orca-slicer.exe` 和隔离数据目录 `generated_models/gui-validation-phase42/data_dir`；未触发付费 API。
- 验收截图：`generated_models/gui-validation-phase42/initial-input-state.png`、`final-history-model-import-state.png`。
- **状态：** complete

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
- 协作文档和历史设计以 `33d93b4803` 提交；提交前文档密钥与个人绝对路径扫描通过。
- `codex/ai-integration-20260814` 已成功推送并跟踪 `origin/codex/ai-integration-20260814`；GitHub 已返回可创建 PR 的分支地址。
- 阶段 38 完成：`master` 保持 `a1ef7204fe`，共享分支包含四个分层提交，验证通过且没有上传本机配置、生成产物或真实密钥。
# 阶段 39：四色可打印图像链路（2026-08-14）

- 已完整读取用户新增的 `Docs/FOUR_COLOR_IMAGE_PIPELINE.md`。
- 已核对正式 Sidecar、OpenAI Image2 预处理、任务恢复、C++ 客户端和 3D 生成 GUI 的现状。
- 已确认主要缺口为确定性后处理产物/指标、物理打印参数、文字先出图和多阶段预览，而不是继续堆叠提示词。
- 已确定实现边界：独立 Python 图像领域模块 + Sidecar 契约 + 薄 GUI 消费；不修改智能切片职责，不触发 Tripo 付费任务。
- 新增独立 `printable_image_pipeline.py`，完成实际耗材色板校验、CIEDE2000、确定性无抖动映射、毫米级最小特征清理、互斥色层/主体/背景遮罩、热力图、评分和元数据。
- Sidecar 已统一文字、图片和文字+图片链路：开启可打印颜色时先得到 Image2 原图，再生成严格色板图和可打印清理图，用户确认清理图后才进入图生 3D；任务恢复兼容旧状态文件。
- 中文 GUI 已增加打印宽度、喷嘴直径、线宽、最小特征、阴影色角色，以及“AI 原图 / 严格色板 / 可打印清理 / 问题热力图”阶段切换。
- 真实 Image2 验证使用 1 次图片调用、0 次 Tripo 调用，产物位于 `generated_models/four-color-validation-phase39/`；清理后小区域占比约 0.0004%，输出像素严格来自四个指定 HEX 色。
- Python 全量回归 103/103 通过；两个修改过的 C++ 源文件定向编译通过；GUI 静态库重新归档和 Windows Release `OrcaSlicer.dll` 最终链接通过，仅保留项目既有 `LNK4098` 警告。
- 已将当前实现和后续边界回写 `Docs/FOUR_COLOR_IMAGE_PIPELINE.md`；阶段 39 完成。

# 阶段 40：正式 Orca 四色生成流程 GUI 验收（2026-08-15）

- 已读取 computer-use 的 Windows 自动化、安全确认和 API 指引，并恢复文件规划上下文。
- 已建立正式 GUI 验收计划：只使用 18764 正式 Sidecar，不使用 mock；先验证 Image2 与预览门禁，再决定是否需要一次 Tripo 任务。
- 已创建 630 MB 隔离运行副本并替换为最新 DLL，哈希校验一致；没有覆盖或关闭用户当前旧 Orca 窗口。
- 已精确停止旧 Sidecar 并用当前源码重启，正式健康契约已出现可打印图像管线能力。
- 隔离 Orca 首次启动捕获真实启动崩溃，定位为旧 `Plater.obj` 与新 `MainFrame` 类布局混用的构建一致性问题；正在进行完整 GUI 增量重编译，不修改业务源码来掩盖该问题。
- 用户恢复验证后，重新编译 `Plater.cpp` 并修复中断构建留下的零字节 `DeviceManager.obj`，重新归档和链接成功；第二次启动越过原崩溃点，但在 `Plater` 初始化完成后出现无栈 `ntdll` 访问冲突，独立 `data_dir` 下可稳定复现，已增加适配器/模型生成面板构造检查点继续定位。
- 检查点日志证明 `OrcaWorkspaceAdapter`、`ModelGenerationPanel`、`create_preset_tabs()` 均已完整构造，崩溃随后发生在旧最近项目/打印配置初始化阶段；这进一步确认问题来自 GUI 对象的新旧 ABI 混链，而不是四色页面构造逻辑。
- 已启动 `libslic3r_gui` 全量对象重编译。首次并发 2 在工具 30 分钟上限后继续后台运行，精确停止时已完成 155/362 个对象且无零字节文件；随后并发 4 从头完整重编，工具窗口到期后后台构建仍正常推进，当前按文件名已越过 `MultiTaskManagerPage`，未归档、未启动半成品 DLL。
- 362/362 个 GUI 对象最终完整编译，静态库归档和 Windows Release 链接成功；隔离 Orca 使用 72,640,000 字节、SHA256 `FC03AD55F8AEFB35A77A50B4540F082699C14CA746EBB4B35D8D97A4FE7DCBC8` 的 DLL 稳定启动。
- 正式 GUI 已确认 3D 生成页位于准备页之前，全部中文；文字输入、红绿蓝白项目色板、160 mm 打印宽度、0.4 mm 喷嘴/线宽、0.8 mm 最小特征和蓝色暗部参数正确进入任务。
- 付费门禁正常出现并经既有授权确认；本轮创建 1 次真实 Image2 任务 `def4b6a3-d84b-4e16-b0c0-de9efe5de7a5`，未创建 Tripo 任务。
- 修复 `/model-jobs/{id}` 状态 GET 白名单遗漏 `status` 导致的 404，并增加 `test_job_status_endpoint_returns_registered_job`；复用已创建任务恢复验证，没有重复付费。
- GUI 已逐项切换 AI 原图、严格色板、可打印清理和问题热力图；四个产物哈希不同，指标显示区域数 2678→146、小区域 2558→33、清理后小区域约 0.01%。
- 从准备页返回后，预览、参数、任务状态和可打印清理阶段均保持；确认预览后“生成 3D 模型”按钮可用，但未点击。
- AI Python 全量测试 104/104 通过，`git diff --check` 通过；阶段 40 GUI 验收完成。

# 阶段 41：四色预览质量与即时刷新（2026-08-15）

- 使用 computer-use 查看用户当前 Orca 界面，确认参考图为彩色 Q 版人物，而可打印清理图退化成蓝白线稿。
- 在 GUI 中选择“AI 原图”后位图没有立即变化；点击缩放按钮后才显示真实 AI 原图，稳定复现阶段切换重绘缺陷。
- 对照任务四份产物确认 AI 原图本身是灰白单色雕塑；严格色板和清理阶段无法恢复红绿区域，问题不是最小特征清理造成的。
- 当前阶段进入设计确认：即时重绘可直接修复；效果改进需要确定是强制生成四色玩具式大色块，还是尽量保留参考图自然配色。
- 用户确认采用设计师玩具风；完成通用四色构图提示、有效颜色质量门禁、低质量重新生成入口、Q 版彩色默认风格和阶段位图即时失效重绘。
- 提示词、确定性图像管线、Sidecar 合约定向回归 32/32 通过。
- 首次 Windows Release `/m:4` 因历史增量记录误触发 libslic3r 大范围重编，出现 PDB 争用和页面文件不足；转为 GUI 单工程串行构建方案。
- `libslic3r_gui` 以单线程、关闭项目依赖重编成功（0 警告、0 错误），随后完整链接 `OrcaSlicer.dll` 成功（仅项目既有 `NOMINMAX`/`LNK4098` 警告）；验证副本已部署新 DLL。
- 正式 GUI 显示新风格项“Q 版设计师玩具（彩色推荐）”与“生成设计师玩具预览”，并用现有人物参考图完成 3 次真实 Image2 调优；未调用 Tripo。
- 真实结果驱动补齐三类缺口：主体被背景色吞没的 18% 门禁、至少 64 个自适应颜色簇以保留小面积绿/蓝材料、亮中性灰归一到最亮耗材色以清理影棚背景。
- 预览下拉从“可打印清理”切到“AI 原图”后位图立即更新，未再点击缩放或其他按钮，确认 bitmap 缓存失效修复生效。
- 最终可打印产物位于 `generated_models/gui-validation-phase41/rerun4/`；像素严格属于红绿蓝白色板，红/绿/蓝/白面积约 29.75%/1.33%/7.08%/61.84%，主体面积约 38.88%，质量门禁通过。
- Python AI 全量回归 110/110、修改文件 `py_compile` 和 `git diff --check` 均通过；Pillow 仅有未来版本弃用警告。
- 阶段 41 完成：设计师玩具原图明显优于旧灰白雕塑，严格四色版保留完整人物轮廓和四种耗材色，后续新任务自动使用最终 Sidecar 规则。

# 2026-08-15 阶段 43：设计师玩具效果调优规划

- 按用户授权精确强制关闭当前项目 Orca 验证实例 PID `222500`；Sidecar 和其他服务保持不变。
- 盘点并目视检查 `Camera Roll` 三张样图，确定其可组成稳定回归集：半身人物测身份和补全策略，全身人物测比例与细节简化，建筑场景测主体选择和背景剥离。
- 审视当前 Image2 提示后确认，阶段 41 的四色分配已具备基础能力，本轮主要瓶颈是参考图编辑契约过度强调原地保真，阻止了为 image-to-3D 所需的完整单体构图重建。
- 已在 `task_plan.md` 增加阶段 43 的五轮调优计划、固定样图验收点和约束；规划阶段未调用 Image2 或 Tripo 收费 API。
- 下一步等待用户确认默认风格权重后，用固定三图创建当前基线，再做小批量 Image2 对照迭代。
- 用户确认 `方飞总.png` 默认生成带稳定底座的半身设计师玩具，不补全未知下肢；全身图仍保留完整全身。
- 已开始重构 image-to-3D 参考图契约：允许剥离背景与摄影附属内容，增加单一稳定底座；按源图完整度分别生成全身摆件或收口明确的半身胸像，并禁止臆造腿部。
- 首轮定向测试因用例沿用默认雕塑风格却断言 Q 版比例而失败；已确认正式 GUI 显式使用 `q_cartoon`，用例改为显式目标风格后继续验证。
- 第一次真实图片尝试在本地 `_image_kind()` 阶段因 PowerShell→Python 中文路径编码失败，尚未发送远端请求、未产生费用；后续将样图复制为项目内 ASCII 文件名作为固定回归输入。
- 已将三张回归图复制为 `generated_models/designer-toy-tuning-phase43/inputs/` 下的 ASCII 文件名，原始图片未修改。
- 完成第 1 次真实 Image2 调用：`fangfei-bust-raw.png` 正确生成带底座半身玩具且无下肢臆造；构图通过，但人物眼睛偏大、自然肤色和手表金属仍需收敛。
- 对第 1 次原图离线运行四色管线：四种颜色均有效，主体面积约 27.96%，区域数 58→29，小区域清零，质量门禁通过；未增加付费调用。
- 完成第 2 次真实 Image2 身份优先对照：成人脸部比例、四色材质和半身底座均明显优于第 1 版；离线评分 0.091495、主体面积约 34.31%，确定将 Q 版正式默认改为 85% 身份保持、15% 玩具化。
- 完成第 3 次真实 Image2 全身人物验证和第 4 次大雁塔验证：两张 AI 原图构图均显著改善，但严格色版暴露白色四肢/白色层间装饰与背景融合造成主体断裂。
- 新增主体连通性质量指标、失败告警 `printable_subject_is_disconnected` 和回归用例；首版 92% 门槛对半身颈部风险过宽，已收紧为 98%。
- 四色提示新增结构规则：背景色不得用于外轮廓、颈部、四肢、承重连接、底座接触或建筑层间分隔，所有关键连接必须使用非背景耗材色。
- 最终三图回归完成：半身人物、全身人物和大雁塔的最大主体连通占比均为 1.0，`palette_quality_ok=true`，小区域清理后均为 0。
- 完整 AI Python 回归 111/111、修改文件 `py_compile` 和 `git diff --check` 通过；仅有既有 Pillow 未来版本弃用警告。
- 本轮仅修改 Python 提示契约、确定性质量门禁和测试，没有 C++/DLL 变化，因此不重复 Windows Release 编译。
- 正式 Sidecar 已从 PID `177832` 重启为 PID `69512`，`127.0.0.1:18764/health` 返回正常，仍使用 `https://laotie.dev` 和 `orcaslicer-ai-sidecar-v4`。
- 阶段 43 完成：共 8 次真实 Image2 调用、0 次 Tripo；最终产物和对照均保存在 `generated_models/designer-toy-tuning-phase43/`。
# 2026-08-15 阶段 44：顶点色打印风格规划启动

- 已确认本轮目标不是增加只在二维预览中成立的图片滤镜，而是设计能在无贴图、仅 OBJ 顶点色的正式链路中保持的打印风格。
- 已盘点现有三种风格入口、Orca OBJ 顶点色支持和 Sidecar 贴图转顶点色产物链路。
- 已把候选风格、技术路线、验证指标和付费调用预算写入 `task_plan.md`；当前尚未调用 Image2 或 Tripo。
- 下一步将审计共享顶点的取色/插值细节与减面颜色保持能力，并在用户确认颜色上限后冻结第一批滤镜契约。
- 已完成烘焙细节审计：现实现按原几何顶点合并 UV 采样，色板验证只检查顶点值，不检测跨色三角形；这是顶点色滤镜正式化前必须修复的质量缺口。
- 已确认 Sidecar 不负责减面，Tripo 继续按 10/30/50/100 万面高质量档位生成；新增低多边形风格不改变现有面数策略。
- 已确认局部拓扑修复保留既有颜色，新补洞顶点采用边界多数色，可在增加混色面指标后继续复用。
- 用户已确认正式滤镜最多使用 4 种实际耗材色，并要求同一套滤镜动态适配不同打印机的不同四色组合。
- 已核对现有动态色板基础：项目耗材读取、温度兼容筛选、手动色板、任务色板冻结和变更失效逻辑均可复用。
- 已发现需修复的红绿蓝白硬编码：四角色推断、阴影色 GUI 和提示词都假定固定色相，不适用于任意四耗材组合。
- 已将动态感知角色、透明背景、槽位元数据、跨打印机重映射和旧任务兼容写入阶段 44 计划；尚未修改业务代码或触发付费生成。
- 动态四色领域层已实现，6/6 独立测试通过；图片管线已输出透明 `model_reference.png` 并按主体统计有效颜色，图片/提示/Sidecar 定向测试 15/15 通过。
- 核对 Orca 的 `obj_import_vertex_color_deal()` 后修正硬边界策略：双色三角形是 Orca 原生多耗材分区协议，不能通过复制顶点强行清零；后续目标改为保留流体共享拓扑、消除三色面并抑制小色岛。
- 完成动态四色领域契约、感知角色自动分配与 GUI 角色交换；自动/手动色板和 Sidecar 均收紧到最多 4 个唯一颜色，当前项目超过 4 个兼容槽位时保留稳定槽位顺序取前四个。
- 新增 `cel_shaded` 与 `enamel_inlay`，正式 GUI 共显示 5 种中文风格；Image2 提示按实际角色色生成，不再假定红绿蓝白。
- 图片管线输出透明 `clean_preview.png` 与 `model_reference.png`，后者作为 Tripo 输入；GUI 为透明区域绘制中性棋盘格，不向模型引入第 5 种颜色。
- OBJ 烘焙保留共享顶点与流体拓扑，通过顶点标签迭代消除三色面，并输出 `vertex-color-metrics.json` 记录单双/三色面和各色用量。
- 正式 GUI 验证 5 种风格、当前项目 4 色和四角色交换；本阶段完成 2 次真实 Image2、0 次 Tripo。
- 第一次真实结果暴露质量提示归因错误，第二次 Q 版结果暴露不透明棋盘格吞掉白色主体；新增原始边界背景分割后，同一缓存原图离线重跑由失败变为 `palette_quality_ok=true`。
- Python AI 全量回归 118/118，Windows `libslic3r_gui` 与正式 DLL 链接通过；最新运行 DLL SHA256 为 `834A10936E7C0E8A2641F27AFC4F0155055769CFE8FA09EE992BCD94604A176E`。
# 2026-08-15 阶段 45 启动：质量优先多样本调优

- 用户确认 Image2 调用次数不作为限制，要求优先保证最终效果，并允许扩充测试图片。
- 恢复阶段 44 计划和设计文档，复核 `Camera Roll` 三张固定输入；确认现有样本覆盖三类核心旅程，但缺少宠物、商品、深色主体和复杂非白背景。
- 将下一阶段定义为可重复的小型质量基准：扩充到 8 至 12 张高差异输入，批量生成候选、统一评分、目视复核并只把优胜结果送入 Tripo/Orca。
- Wikimedia 批量 API 查询被出口限流；已记录错误并切换为使用检索结果中的明确文件页和直接文件路径，不继续重复请求。
- 直接文件路径也被相同出口限流，已停止使用 Wikimedia 下载；后续用 Image2 受控合成输入补齐五类失败模式。
- 使用 `gpt-image-2` 实际生成 5 张受控测试输入并逐张目视检查，全部可用于回归；本阶段至此新增 5 次 Image2 调用，尚未调用 Tripo。
- 新增可恢复的付费 Image2 基准运行器、8 图清单和纯逻辑测试；第一次定向测试发现包导入路径不一致，已改为项目包路径后继续验证。
- 首次启动 22 候选时，本地色板角色校验发现映射方向反了，所有候选都在远程请求前失败；已修正为 `color_by_role` 并增加提示契约测试。
- 修复后完成第一轮 22/22 个真实 Image2 候选，全部通过自动四色质量门；生成原始/清理总览并完成目视对照，定位到成人身份幼化、动物凭空增色、稳定商品强制加底座和动态色板禁色措辞冲突四个系统问题。
- 完成第二轮 8 个定向 Image2 候选：真人成年身份、动物原毛色和稳定商品去底座均得到目视改善；本阶段累计新增 35 次 Image2（5 张受控输入 + 22 首轮 + 8 第二轮），仍为 0 次 Tripo。
- 将“最多四色”与“强制用满颜色”解耦：配色多样性只保留告警，主体面积与主体连通性继续作为硬门禁；缓存重处理验证纯黑猫现在可继续生成 3D。Python AI 全量回归 123/123 通过。
- 完成 2 次 Tripo 50 万面级生成：人物 478,292 面、机器人 467,634 面，均严格四种顶点色且无边界边/非流体边/退化面；顶点色报告仍检出少量三色三角面，进入离线收敛修复，不创建新 Tripo 任务。
- 第一版固定轮数单调修复在真实大网格上仍剩 4 个三色面，已改为有终止界的局部邻接队列后再次离线重做。
# 2026-08-15 阶段 45 完成：四色综合调优与 Orca 拓扑闭环

- 完成 8 类质量基准、22 个第一轮候选、8 个第二轮定向候选，共 35 次 Image2 调用。
- 完成 2 次真实 Tripo 50 万面生成：设计师玩具半身人像与机器人样本。
- 完成最多四色语义、动态耗材色板、5 种中文打印风格、透明模型参考图和主体连通性门禁。
- 完成 OBJ 三色面收敛与面绕序规范化；新增同向共享边单元测试。
- 修正后人像在正式 Orca 中导入无非流形错误，完成 500 层、604 次换色切片。
- 机器人在正式 Orca 中完成 500 层、597 次换色切片；模型拓扑复核为 0 非法边。
- 验证截图：`generated_models/gui-validation-phase46/portrait-winding-fixed.jpg`、`portrait-winding-fixed-sliced.jpg`、`robot-sliced.png`。
- `python -m unittest discover -s tools/ai -p "test_*.py"`：124/124 通过。
- `git diff --check`：通过。
- 正式 sidecar 已重启，PID 82476，健康检查返回 `orcaslicer-ai-sidecar-v4`、`https://laotie.dev` 和 5 种生成风格。

# 2026-08-16 阶段 46 启动：模型生成 Beta 1 与 3D 结构质量门禁

- 将当前能力、验证证据、成熟度判断、Beta 1 优先级和建议验收指标汇总到 `Docs/MODEL_GENERATION_BETA1_STATUS_AND_ROADMAP.md`。
- 采用“确定性结构门禁 + 可插拔视觉评分”路线；第一版完全本地运行，不新增付费调用。
- 新增 `Docs/plans/2026-08-16-printable-model-quality-gate-design.md`，冻结 `pass / review / reject` 状态、指标、兼容策略和验证方法。
- 真实机器人模型证明多个连通部件不能作为硬失败条件；第一版只阻断确定性拓扑错误，微小部件、弱接地和悬垂进入复核。
- 一次 Windows `rg` 通配路径错误已记录并改用明确路径；没有造成文件修改或进程影响。
- 新质量测试首次从仓库根运行时因 `tools/ai` 不在模块搜索路径而失败；已按项目既有测试模式在测试入口补齐路径，生产代码未变。
- 微小组件用例首次使用两个面数相同的四面体，未满足正式“小面数占比”条件；已只调整测试阈值，未放宽生产门禁。
- 定向 OBJ 回归发现新门禁会阻断既有的 Orca 小缺陷修复路径；已增加显式 `allow_repairable_topology` 能力参数，默认严格、Sidecar 小缺陷复核、超限拒绝。
- 首次兼容补丁因文件上下文归属错误被 `apply_patch` 整体拒绝，没有产生半成品；拆分文件段后完成修正。
- 新增独立 `printable_model_quality.py`、9 项合成网格测试和 Sidecar `model_quality` 契约；新 OBJ 后处理写入 `model-quality.json`。
- 阶段 45 人像离线质量结果为 `pass`；机器人为 `review`，只提示 2 个微小脱离组件，均无边界、非流形、绕序或退化面错误。
- 真实校准报告保存于 `generated_models/model-quality-gate-phase46/portrait.json` 与 `robot.json`。
- 质量模块、OBJ 生成和 Sidecar 契约定向回归 77/77 通过；最终完整 AI Python 回归 135/135、全部 Python 语法检查和 `git diff --check` 通过。
- 阶段 46 第一批完成；没有付费调用和 C++ 修改。下一批为 GUI 复核详情、历史模型显式重验和多视角视觉质量评分。
- 正式 Sidecar 首次组合重启命令被本机策略在执行前拦截，旧进程未受影响；改为短命令分步操作。
- 正式 Sidecar 最终重启为 PID `198400`；健康检查返回 v4、模型生成可用，latest-job 契约确认包含 `model_quality`。

# 2026-08-16 阶段 47 启动：质量复核 UI 与历史模型重新检查

- 用户确认继续下一批：先将 `model_quality` 接入中文 3D 预览，再为历史模型增加本地重新检查。
- 采用 3D 预览旁常驻质量卡，状态为“通过 / 建议复核 / 未通过 / 尚未检查”，避免弹窗打断旅程。
- recheck 接口只接收已登记的 `job_id`，不接受任意本地路径；重新检查不会创建任何付费任务。
- 新增 `Docs/plans/2026-08-16-model-quality-review-ui-design.md`，冻结数据流、失败降级、导入门禁和验证范围。
- Sidecar recheck 契约 11/11 通过，确认只分析任务目录内登记 OBJ，且不会调用任何付费 Provider。
- C++ 客户端已增加可选 `ModelQuality` 契约和 recheck 调用；生成页已加入常驻质量卡、中文风险映射和 reject 导入门禁。
- 静态检查发现新生成开始时可能短暂残留旧质量结论，已在清空旧 3D 预览时同步清空质量状态。
- 一次辅助搜索重复使用 Windows 路径通配而失败；未影响源码读取和差异检查，已记录并改用明确文件。
# 2026-08-16 阶段 47：质量复核 UI 与历史模型重新检查完成

- 新增 Sidecar 本地重验接口；新任务复用已登记 OBJ，早期无 `job.json` 的模型只按 UUID 和固定文件名安全接管，成功后补写质量报告与任务清单。
- C++ 客户端已解析 `model_quality`，3D 预览新增中文常驻质量卡、指标折叠区和“重新检查”。
- 模型库加载保留独立显示 job ID；已有报告自动恢复，无报告显示“尚未检查”，重验失败不会清空预览。
- 修复历史模型目录遍历错误码复用问题：坏条目不再截断后续列表。
- 正式 Orca 验收：`d960d74e`/`0c85a1eb` 为通过，`8267ded0` 为建议复核，`ec2d5d40` 为阻断；三态导入门禁符合设计。
- 发现 GUI 与 Sidecar 的模型目录不能依赖进程当前目录；正式启动必须给两者设置相同的 `ORCASLICER_AI_OUTPUT_DIR`。
- Windows Release 完整链接通过，运行 DLL SHA-256 为 `84A8008751194027127805D1D72D56FCA0CD8F6D9B33998C4F86791858DE75F1`。
- 最终 AI Python 回归 138/138 通过，语法检查和差异检查通过；未调用付费 AI。

# 2026-08-16 阶段 48：多视角渲染与 AI 视觉复核完成

- 对比三条路线后采用 Sidecar 本地软件渲染，不复用依赖 GUI/OpenGL/打印盘状态的 Orca 缩略图，也不依赖 Tripo 供应商截图。
- 新增 `printable_model_views.py`：从最终顶点色 OBJ 生成前、后、左、右、等轴五视图、拼图和 SHA-256 缓存清单；全部产物保存在任务目录。
- 新增 `printable_visual_quality.py` 与 OpenAI 兼容多图完成接口；图生 3D 显式复核时发送原参考图和拼图，文生 3D 发送描述和拼图。
- 新增 `POST /visual-review`，只按任务 ID 读取登记制品；API 失败写 `unavailable`，模型与结构门禁不受影响。
- GUI 在结构卡下增加中文“AI 视觉复核”，显示分数、摘要和告警；视觉结果只有 `pass/review/unavailable`，不参与导入阻断。
- 29.6 万面人物真实模型首次五视图渲染约 12.8 秒；目视确认方向、轮廓、背面和顶点色可读，采样针孔经小范围中值处理消除。
- 完成 1 次真实视觉模型调用：86 分、置信度 0.91，准确提示半身截断且缺独立底座；未调用 Image2 或 Tripo。
- 正式 GUI 恢复该历史模型后自动显示视觉结果；再次点击快速命中缓存，视觉 `review` 时导入按钮仍可用。
- 修复 Windows 测试包运行时清单，补入色板、结构质量、多视图和视觉质量四个 Sidecar 依赖文件及环境检查。
- 将打包白名单复制到独立目录后，`orca_ai_sidecar`、视觉质量和多视角模块可在隔离路径正常导入；视图下载接口返回文件与任务目录原件 SHA-256 一致。
- AI Python 全量回归 148/148、Python 语法检查、差异检查、`libslic3r_gui` 和完整 Release 链接均通过。
- 正式 DLL SHA-256 为 `658A5C545BEF582B843BF3CB4743301A6A822C546B5052D2E027A4F9BF80C5F8`；Sidecar PID `110424`、Orca PID `134608` 正在运行。

# 2026-08-16 阶段 49 启动：模型生成小批量盲测与评分校准

- 冻结第一批 4 个未参与调优的新样本：狐狸探险家、机器人技师、复古拍立得和中式塔楼，覆盖角色/动物、稳定商品和复杂结构物。
- 每例采用不同的四耗材色板，统一使用 30 万面 Tripo 档位；上限为 4 次 Image2、4 次 Tripo 生成和 4 次视觉复核。
- 新增 `Docs/plans/2026-08-16-model-generation-blind-benchmark-design.md`，明确盲测数据流、人工 7 维评分、防重复付费和恢复规则。
- 下一步实现清单驱动的可恢复编排器及测试；框架测试通过前不创建远端付费任务。

# 2026-08-16 阶段 49 完成：模型生成小批量盲测与评分校准

- 新增 `model_blind_benchmark.py`、冻结清单和 8 项定向回归，支持逐阶段执行、样本指纹、防重复付费、失败保留、人工评分和 JSON/CSV 汇总。
- Image2 四例全部通过动态四色门禁；完成 4 次 Tripo 30 万面生成和 4 次转换，没有重复任务。
- 四个 OBJ 均为严格目标四色且无边界、非流形或退化面；3 个结构通过，塔楼因 5 个微小组件建议复核。
- 完成 4 次五视图 AI 复核，分数为狐狸 82、机器人 78、相机 71、塔楼 89；人工 7 维均分分别为 4.57、3.86、3.71、4.71。
- 自动与人工三态结论均为 4 个 `review`，精确一致率 100%；主要缺口集中在单视图无法约束背面/侧面语义。
- 完整产物位于 `generated_models/model-blind-benchmark-phase49/`，结论已写入 `Docs/MODEL_GENERATION_BLIND_PILOT_V1_REPORT.md` 和 Beta 1 状态文档。
- 最终 AI Python 全量回归 156/156、全部 AI 脚本语法检查和 `git diff --check` 通过。
- 再次执行四例视觉复核只命中已有报告，19.4 秒完成，付费计数保持 Image2/Tripo/视觉各 4 次，确认不会隐式重试。

# 2026-08-16 阶段 50 启动：多视图约束 3D 与架构复核

- 用户明确 Image2 可以充分使用，但要求继续控制 Tripo 次数，并在大需求完成后同步检查架构。
- 核对 Tripo v3 官方文档，确认原生命名多视图建模支持前/左/后/右文件 token，正面必填且至少两张。
- 选择“Image2 生成 2×2 四视图板、本地切分和一致性门禁、Tripo 一次多视图建模”；先验证机器人和相机，默认只新增 2 次 Tripo 3D。
- 新增多视图设计文档和 ADR-0001，明确多视图属于模型生成领域与供应商 Gateway，不进入 Orca GUI、3MF 或打印配置。
- 下一步先实现完全离线可测的视图切分、门禁和 Tripo 请求映射；测试通过前不创建新付费任务。
- 已实现原始 Image2 编辑边界、固定 2×2 四视图提示/切分/复核模块和 Tripo 命名多视图请求映射；首次定向回归仅有一项测试生命周期错误，已修正后重跑。
- 已实现可恢复多视图验证编排器，并复用既有 OBJ 转换、严格四色顶点色和拓扑门禁链路；定向回归与最终 AI Python 全量回归 166/166、`git diff --check` 均通过。
- 离线预检完成后进入真实验证：先生成机器人和相机四视图并执行 AI 一致性门禁，未通过的样本不会创建 Tripo 任务。
- 机器人和相机各完成两版 Image2 四视图；旧版均归档，生成前 AI `review` 由人工对照原图后持久化批准理由、图片哈希和警告。
- 实际创建且只创建 2 个 Tripo 多视图任务：机器人 282,616 面、结构 `pass`、视觉 88；相机 279,578 面、一个 4 顶点微小组件为 `review`、视觉 82。两例都严格四色且零边界/非流形/退化面。
- 相对阶段 49 同源单视图基线，机器人视觉 78→88，相机 71→82；不启用第 3 个狐狸样本。
- 完成阶段 50 架构复核：纯多视图参考模块移除默认 OpenAI 客户端依赖，供应商调用留在实验编排器；正式 Sidecar 的 Tripo 直连记录为 Gateway 抽取债务。
- 阶段 50 最终 AI Python 全量回归 169/169、34 个脚本语法检查和 `git diff --check` 通过；未修改 C++/3MF，阶段状态标记为完成。

# 2026-08-16 阶段 51 启动：多视图配对校准第二批

- 用户确认继续按多视图校准路线推进；Image2 可充分使用，Tripo 继续控制次数，并要求大需求后同步检查架构。
- 选择 2→4→10→20 的分批扩样，不一次消耗 18 个 Tripo，也不在证据不足时接入正式 Sidecar。
- 第二批冻结阶段 49 的狐狸和中式塔楼，最多新增 2 个 Tripo 生成任务；先把最终结构、视觉与基线比较做成正式可恢复 `review` 阶段。
- `planning-with-files` 会话恢复脚本返回早期 PPT 上下文且编码异常；已核对当前规划、阶段 50 产物和工作区，陈旧内容被忽略。
- 只读汇总首次再次触发 PowerShell 空管道语法错误；已改为 `$rows` 两步写法，并确认第二批真实目录为 `fox_explorer` 与 `pagoda_watchtower`。
- 正式 `review` 阶段完成 75 项定向回归，并对阶段 50 两例无付费缓存回放成功：自动写出机器人 +10、相机 +11 的 `comparison.json`。
- 狐狸首版 Image2 四视图完成，预检 82 分；帽子、背包、指南针和底座一致，当前软性疑问集中在尾巴与腿、手爪与指南针的遮挡关系，尚未调用 Tripo。
- 塔楼首版预检 72 分，目视发现关键漏判：原始三层/三道屋檐被重画成两层，不能提交 Tripo。决定新增可审计的样本专属几何契约后再生成第二版。
- 塔楼第二版恢复三层但后视触边/断连；第三版视图完整却再次丢失一层。达到每例 3 次 Image2 上限后停止，塔楼 Tripo 调用保持 0。

# 2026-08-16 阶段 51 完成：多视图配对校准第二批

- 最终 `review` 阶段已实现结构分析、五视图复核、缓存复用和单图基线比较；阶段 50 的机器人、相机已无付费回放生成正式比较报告。
- 新增可审计人工拒绝：视图或最终模型记录理由和 SHA-256；同一已拒绝视图不能通过人工批准参数继续创建 Tripo，重新生成后才可重新评估。
- 狐狸完成 1 次 Tripo 多视图生成与转换，产物 288,836 面、严格四色、零边界/非流形/退化面；自动评分 82→83，但人工发现头顶悬浮圆环，最终拒绝。
- 塔楼用满 3 次 Image2 后仍不能稳定保持三层结构，在 3D 前人工拒绝，未创建 Tripo 任务。
- 累计 4 组配对中机器人、相机接受，狐狸、塔楼拒绝；固定 Image2 四宫格方案仅 2/4 达标，暂不接入正式 Sidecar。
- 新增第二批报告、ADR-0002（Proposed）和阶段架构复核；下一步转为供应商无关的参考视图策略路由，再做 6 个跨类别校准样本。
- 最终 AI Python 全量回归 175/175、34 个 `tools/ai/*.py` 语法检查和 `git diff --check` 通过；缩小范围后的 C++ 边界搜索确认供应商专属字段命中为 0。

# 2026-08-17 阶段 52 启动：自定义生成风格与正式用户旅程复验

- 用户要求在图片生成风格中增加“自定义”，选中后展开文字输入框，并重新操作一遍新功能检查用户旅程。
- 采用桌面内联渐进展开：自定义选项放在现有风格末尾，输入框只在选中时显示；切换预设时保留草稿，重新选择自定义时恢复。
- 自定义风格为空将阻止图片生成并给出中文就近提示；请求只进入模型生成链路，不扩展 3MF 或打印配置。
- 下一步审计现有 C++ UI、客户端 JSON 和 Sidecar 提示构造，先补测试再实现并构建正式 Windows 版本。
- 已完成纵向契约审计并新增设计文档；采用 `style=custom` + 独立 `custom_style`，不把自由文本塞入稳定风格 ID。
- C++ 页面已增加内联自定义输入、即时刷新、空值阻断、任务快照和恢复；客户端 JSON/Multipart 均传递独立字段。
- Sidecar v5 已增加自定义风格白名单、1000 字节校验、状态持久化和公开恢复字段；Image2/文本预处理提示将自定义内容限制为外观方向，打印与结构硬约束优先。
- 新增提示构造、空值/超长校验、状态恢复测试；首轮 OpenAI/Sidecar 定向回归 32/32 通过。
- 修正一个依赖倒数参数位置的旧测试后，自定义风格相关定向回归 36/36 通过。
- Windows 并行构建受当前页面文件余量限制；清理本轮孤立节点并临时以 `CL=/MP1` 串行构建后，`libslic3r_gui` 和完整 `OrcaSlicer.dll` 均成功。
- 新 DLL SHA-256 为 `14D99D840DC9D2FE9B71FEC3997D5EE4C5DBFA9F9EE3A770B5F8DFE1AFBAFB32`；下一步部署 DLL、重启 Sidecar v5 并进入正式 GUI 验证。

# 2026-08-17 阶段 52 完成：自定义风格与正式旅程验收

- 已把 Release DLL 部署到正式运行目录，Sidecar 升级为 v5；健康检查返回真实 `https://laotie.dev` 通路和包含 `custom` 的风格能力。
- 正式 GUI 验证自定义面板即时展开、空值阻断、输入即时启用、预设切换隐藏与草稿恢复全部通过。
- 用“机械猫圆形底座 + 复古木刻玩具风”完成 1 次真实 Image2：生成 AI 原图、严格色板、可打印清理、问题热力图、角色掩码和 3D 参考图；未创建 Tripo 任务。
- 任务 `9d038b5e-ee51-40ae-91e3-6c31b6707c24` 正确保存 `style=custom`、中文自定义描述、RGBW 色板与三层图片产物；原图 1254×1254，可打印门禁显示最小特征 7 px、清理后小区域 0.00%。
- 切到“准备”再返回时预览保留；强制关闭并重启正式 Orca 后，主体描述、自定义描述、可打印预览和 30 万面 3D 确认入口均恢复。
- 3D 付费确认框验证通过后选择“否”，本阶段 Tripo 新调用为 0。
- AI Python 全量回归 179/179，Windows Release 的 `libslic3r_gui` 与完整 `OrcaSlicer` 构建通过，`git diff --check` 通过。
- 阶段架构复核确认自由文本只穿过通用模型生成契约，不进入 Orca 核心、3MF 或打印配置；详见 `Docs/architecture/2026-08-17-phase52-custom-style-review.md`。

# 2026-08-17 阶段 53 启动：精简生成页并恢复显式图片对比

- 用户通过正式界面截图指出：左侧颜色角色和物理参数难理解、自定义风格框固定高度、右侧下拉只显示严格色板而没有原图对照。
- 采用渐进披露方案：首屏保留必要输入与四色摘要，角色/物理参数归入默认折叠高级设置；文字框限定范围内随内容增高。
- 对比区改为同屏双图：左侧基准原图，右侧当前处理阶段；无上传参考图时以 AI 原图作为基准。
- 下一步检查 `ModelGenerationPanel` 的表单 sizer、预览模式和位图布局，形成最小 C++ 改动，不改变 Sidecar 或付费链路。
- 代码确认原图产物没有丢失：纯文字任务的 `m_raw_preview_image` 已下载，但现有双栏条件只检查用户参考图 `m_reference_image`，因此处理阶段会退化为单图。
- 已冻结设计：高级设置集中收纳颜色角色和物理参数；两类文本框在有限行数内自适应；纯文字任务用 AI 原图作为左侧基准，含参考图任务继续使用用户参考图。
# 2026-08-17 阶段 53 完成：生成页精简、自适应输入与原图对比

- 精简 3D 生成左侧文案和颜色入口，把颜色用途、打印宽度、喷嘴、线宽和最小特征移入默认折叠的高级设置。
- 主体描述和自定义风格输入框已在限定行数内随内容即时增高/缩回。
- 修复纯文字任务的图片对照：严格色板、可打印清理和问题热力图均显示 AI 原图与当前结果，AI 原图阶段保持单图。
- 正式 GUI 验证了即时阶段切换、页面往返状态保持、高级设置展开/收起和文本框自适应；额外修复折叠面板导致 Orca 主窗口缩放的问题。
- Windows Release 构建通过并部署，AI Python 全量回归 179/179 通过，DLL SHA-256 为 `FA553148AD63302C7A7E37BEA070FE696B513DE84837A72481009EB0E05F34AF`；未产生新的 Image2/Tripo 付费调用。
- 架构复核确认改动只在独立 AI GUI 页面，不修改 Sidecar、Provider、3MF、打印配置或 Orca 核心模型。
# 2026-08-17 阶段 55 启动：3D 预览局部智能改色
- 用户确认实施第一版“点击部位 → 智能扩选 → 手动补选/擦除 → 四色改色”。
- 已恢复现有规划与工作树，确认 3D 预览由 `ModelGenerationPanel.cpp` 内独立 `ModelPreview3D` 承载，OBJ 通过顶点色进入 Orca 导入链路。
- 设计决定采用离线混合选区：射线拾取种子面，按连通色块与法线连续性扩选，补选/擦除使用小范围网格笔刷；不自动拆件。
- 为兼容现有未提交修改，本阶段仅增量修改 AI GUI 边界和新增独立选区模块/测试，不回退或覆盖其他工作树内容。

# 2026-08-17 阶段 55 完成：局部智能改色闭环
- 新增 `src/slic3r/GUI/AI/Model/VertexColorRegionEditor.*`：实现网格邻接、射线拾取、颜色/法线智能区域生长、局部补选/擦除、选区着色和安全 OBJ 副本写出。
- 3D 预览加入中文“局部改色（点击模型选区）”面板，支持智能选区、补选、擦除、精细/标准/宽松范围、实时三角面计数、清除选区和最多四种当前耗材目标色。
- 选区以青色覆盖层显示；短按点击选区、拖动仍旋转、滚轮仍缩放。应用后立即加载新 OBJ，保留原 OBJ，并写入历史模型元数据。
- 新增 `tests/slic3rutils/test_vertex_color_region_editor.cpp`，覆盖色块连通、补选/擦除、锐法线边界、射线拾取和 OBJ 结构保真；当前主构建关闭测试目标，未在该 build 目录运行。
- Windows Release `libslic3r_gui` 与完整 `OrcaSlicer` 构建通过，DLL 已部署到正式运行目录；源/目标 SHA-256 均为 `0AE8401808352DD22479B11B428DCCA2B6088AA26A45D483D9E1836E0F43EB6F`，`git diff --check` 通过。
- 正式 GUI 真实模型验证：智能选中 728 面，擦除为 611 面，应用红色后生成新的顶点色 OBJ；模型库新增“局部改色模型”，原模型仍保留。
- 收尾复核修正自然色源模型的元数据：未选区域保留自然顶点色时不再误标成四色；目标耗材色与目标四色板单独写入元数据。已受四色约束的源模型仍保持四色状态。
- 第一轮交互验证产物为 `generated_models/downloads/orcaslicer-ai-edit-e964108d-2655-413a-a094-2cdfd4aece35.obj`；元数据修正后的最终权威复验产物为 `generated_models/downloads/orcaslicer-ai-edit-67a3a299-cb87-46d5-914c-88dfea183a1a.obj` 与同名 JSON。
- 最终正式 GUI 复验在 290,438 面自然色模型上智能选中 36,472 面并应用 `#FF0000`；新模型仍在历史库显示“自然颜色”，JSON 为 `palette=[]`、`use_printable_colors=false`、`preserves_unselected_vertex_colors=true`，同时独立保存四个目标耗材候选色。
- 最终 OBJ SHA-256 为 `D0A32F439F50740D3965EC9A886857A2C5EECE5AAE41A831F31BCFBBC44CE5FA`；本轮未调用 Image2 或 Tripo。
- 完成架构复核 `Docs/architecture/2026-08-17-phase55-local-recolor-review.md`：功能未侵入 Orca 核心、3MF、打印配置或智能切片边界，语义分割可按面 ID 契约后续插拔。

# 2026-08-18 阶段 56 启动：修复绿色改色后变蓝
- 用户提供正式界面截图并报告点击“应用改色”后绿色变成蓝色。
- 已恢复规划文件和工作树，确认继续只增量修改局部改色模块，不覆盖其他大量未提交改动。
- 已审计当前颜色路径：UI 使用 `wxColour` 解析十六进制，编辑器以 RGBA 数组保存，OBJ 按 RGB 写出，Orca 解析器也按 RGB 读取；下一步从最新产物和正式 GUI 复现确定实际错位位置。
- 首次汇总最新 JSON 的 PowerShell 命令因 `foreach` 后直接接管道触发解析错误；将改用数组变量，不重复该写法。
- 已在用户当前正式 Orca 窗口选择 `耗材 2 · #00FF00` 并应用现有 8343 面选区；新 OBJ 和重新加载预览均为绿色，证明颜色通道本身正确。
- 已定位 UI 根因：选区固定显示青色，且重新加载后摘要仍显示旧选区数量。冻结修复方案为“目标色即时预览 + 加载后清零摘要 + RGB 三通道往返测试”，设计见 `Docs/plans/2026-08-18-local-recolor-color-consistency-design.md`。

# 2026-08-18 阶段 56 完成：局部改色颜色一致性

- `ModelPreview3D` 的选区高亮改为跟随当前目标耗材色；切换目标色后即时刷新，不再用固定青色遮住绿色并造成通道错觉。
- 模型加载成功后统一发送选区变化通知，保证内部零选区、界面摘要和“应用改色”按钮状态一致。
- 新增红、绿、蓝三种顶点色经 OBJ 写出并由 Orca `load_obj` 重新读取的往返测试源码；当前主构建关闭测试目标，未在该 build 目录执行。
- Windows Release `libslic3r_gui` 与完整 `OrcaSlicer` 构建通过，正式 DLL 已部署，SHA-256 为 `E2095733DECAE8CA0078E0EB400ADA87B47DC4D81EC2BFDA149A0AEB6C73C4AB`。
- 正式 GUI 复验：选择绿色目标后 41,605 面高亮为绿色；应用后模型仍为绿色、摘要显示“尚未选择区域”、应用按钮禁用。
- 最终 OBJ 为 `generated_models/downloads/orcaslicer-ai-edit-f9c868be-b3be-4790-ad95-1e95497e8cbc.obj`，SHA-256 为 `42AA459441B6CC91A23BE308102F9B3248FD0BC7BC0F4574476152E1A9D6D6E9`；同名 JSON 的 `recolor_target` 为 `#00FF00`。
- `git diff --check` 通过；本阶段未调用 Image2 或 Tripo。架构复核见 `Docs/architecture/2026-08-18-phase56-recolor-color-consistency-review.md`。

# 2026-08-18 阶段 57 启动：GitHub 提交前仓库整理

- 用户要求先整理仓库代码，再由用户提交到 GitHub；本阶段不暂存、不提交、不推送。
- 已恢复规划，确认会话恢复脚本给出的是早期 PPT 陈旧内容，继续以磁盘阶段 56 和实际工作树为准。
- 当前分支 `codex/model-generation`，HEAD 为 `ca5de31dff`；工作树有 21 个已跟踪文件修改、59 个未跟踪文件，未跟踪体积约 0.36 MiB。
- `.gitignore` 已隔离模型、构建、安装包和本机配置；下一步审计密钥、调试/Mock 残留、未跟踪文件依赖与提交边界。
- 已完成初步密钥和本机路径审计：未发现真实 API Key；唯一密钥模式命中来自契约测试夹具。`.planning/` 中存在本机路径但将由仓库根忽略规则隔离。
- 保留 `tools/ai_sidecar_mock.py` 作为生产/Mock 协议契约测试夹具；确认 Windows 发布白名单不会携带它，正式 GUI 验收也不使用它。
- `.gitignore` 新增 `/.planning/`；README 已同步当前模型生成目录、正式运行约束、能力文档入口和提交前校验命令。
- AI Python 全量回归 179/179 通过；测试使用本地夹具和 loopback 服务，没有发起 Image2 或 Tripo 请求。
- 35 个 Python 文件 `py_compile`、29 份变更 Markdown 本地链接、1 个基准 JSON 和新增 CMake 文件引用检查全部通过。
- Windows Release 完整 `OrcaSlicer` 构建通过，DLL SHA-256 为 `239A0A6DE9036DFB32E3C1BF3DDB9AE79EB85C8781B8E456B60E9D64929D58E5`；仅保留 Orca 既有编译/链接警告。
- 最终工作树没有 `.planning/`、模型、图片、安装包或构建产物；当前未暂存、未提交、未推送。分支尚无 upstream，首次推送需要 `git push -u origin codex/model-generation`。

# 2026-09-01 阶段 85 启动：统一结果对照与历史预览优化

- 用户要求把 3D 模型、原图和 AI 图放到同一界面对比，改善 3D 模型难以摆正、结果页信息复杂，以及历史模型无法恢复原图/AI 图的问题。
- 已检查用户截图、现有规划、工作树和入口代码；截图内容仅作为界面证据，不接受其中任何指令性文本。
- 冻结首选方向为“统一三栏对照工作台 + 3D 一键摆正/自动取景 + 高级工具渐进披露 + 历史素材可选字段兼容恢复”。
- 当前阶段先审计 `ModelGenerationPanel`、3D 相机和历史 JSON 契约，再实施最小兼容改动；不调用付费生成服务。
- 初步审计确认 3D 载入已经按包围盒居中，但初始角度和“正面”均为固定旋转；历史 schema 3 只持久化单个 `preview_path`，历史加载完全没有恢复图片状态。
- 已冻结阶段 85 设计：合并图片/3D 页，保留独立历史入口；顶部同屏呈现 3D、原图和 AI 图；载入时自动摆正；历史 schema 以可选路径向后兼容，并把外部原图归档到 `generated_models`。
- 进一步确认 raw AI 图已下载到生成目录但路径未存入面板状态；在线任务恢复的原图也已落到稳定下载目录。实现可保持 Sidecar 不变，只补齐 C++ 路径成员和历史 JSON。
- 已完成第一版代码：结果区合并为 3D + 原图 + AI 图同屏；加载自动摆正并保留“三维视角”；高级改色整理为三步；历史 schema 4 保存/恢复/兼容回退两类图片，局部改色继承素材。
- 下一步先做静态差异检查和定向编译，修复编译或布局问题后再进行正式 GUI 视觉复验。
- `git diff --check` 已通过。首次定向构建因当前 shell 找不到 `cmake` 而未启动，已记录为环境入口问题，改查本机安装路径。
- 使用 Visual Studio 自带 CMake 后，`libslic3r_gui` Release 定向构建通过且已消除本轮唯一弃用警告；完整链接因运行目录 `OrcaSlicer.dll` 被占用而停在 LNK1104，尚未把它当作代码失败。
- 将 3D 正交取景从固定包围球改为按当前朝向包围盒适配；定向 Release 重编译通过，摆正后人像显著放大且不裁切。
- 通过独立 `OutDir` 完成完整 `OrcaSlicer.dll` 链接，0 错误、仅保留既有 LNK4098；隔离 DLL SHA-256 为 `4C27610A1F8E85CC06B92D9051617BB95771E4685BA05303D3BF2D8719F7630F`。
- 正式 GUI 复验完成：统一结果页同时显示 3D、原图和 AI 图；“三维视角/摆正模型”可往返；高级工具默认折叠且分步布局清晰。
- 加载 schema 4 新历史记录后，模型、原图和 AI 图全部恢复；加载旧 schema 缺图记录后，模型正常恢复且两栏给出准确缺失占位。
- 实际历史 JSON 检查确认 schema 为 4，两类图片路径均为生成根目录内相对路径且文件存在。
- AI Python 全量回归 484/484 通过；`git diff --check` 通过。测试使用 Mock/loopback 夹具，没有发起新的 Image2 或 Tripo 付费任务。
- 阶段 85 完成；架构复核记录为 `Docs/architecture/2026-09-01-phase85-unified-result-workspace-review.md`。

# 2026-09-01 阶段 86 启动：统一 Windows 本地启动入口

- 用户确认继续处理正式启动入口。
- 已核对根启动器、README、发布包启动器和构建目录：根启动器仍指向运行树，但未同步最新 Release DLL，且 Sidecar 版本检查停留在 v4。
- 冻结方案：启动前按哈希把 `build/src/Release/OrcaSlicer.dll` 同步到 `build/OrcaSlicer`，然后用 v6 契约检查 Sidecar并启动现有运行树 EXE。
- 根启动器已增加 Release/运行树路径变量、DLL 内容同步、占用失败提示和 Sidecar v6 版本变量；Sidecar 窗口改为最小化启动。
- 首次使用 `Get-FileHash` 的同步实现因当前 Windows PowerShell 缺少该命令失败一次；已改用 `fc /b` + `copy /y`，避免模块依赖且保留“相同 DLL 不覆盖”的行为。
- Windows Release 完整链接通过；`--check` 成功启动并确认 Sidecar v6，源/运行树 DLL SHA-256 都是 `B8728D4D3E95D3B820FD1C3EA402F3165836714239CBDC558673DC3A8B5E3EB0`。
- 通过 `start_orcaslicer_with_ai.bat` 实际启动正式运行树 App，进程 PID 35472、路径为 `build/OrcaSlicer/orca-slicer.exe`。
- README 已补充 DLL 自动同步、Sidecar v6 和 `--check` 使用说明；阶段 86 完成。

# 2026-09-01 阶段 87 启动：轻量风格推荐

- 用户确认人像与非人像都采用风格推荐，并明确要求交互保持简单。
- 已读取代码工作流、UI/UX 和文件规划规范，恢复现有阶段记录并确认工作树包含既有未提交修改。
- 已定位当前四类风格、旧风格别名、图片质量指标和输入区布局；下一步冻结推荐契约并实现本地分析，不产生付费调用。
- 已冻结“现有风格下拉 + 单张推荐卡 + 两个备选按钮”的最简交互，并写入 `Docs/plans/2026-09-01-phase87-lightweight-style-recommendation-design.md`。
- 已实现 Pillow 本地推荐规则、六种正式风格提示、Sidecar v7 本机接口、C++ 客户端解析和紧凑推荐卡；推荐异步返回时尊重用户手动选择。
- 定向 Python：图片质量/推荐 9/9、提示与 OBJ 156/156、Sidecar 合同 42/42 通过；Windows Release `libslic3r_gui` 编译通过。
- 正式 Sidecar 已升级为 v7；用本地截图调用推荐接口返回 `relief + low_poly/sculpture`，结果标记 `local_only=true`，没有使用 API Key。
- 实机恢复真实人像后发现无文字语义不足；增加 Pillow 本地人脸式肤色连通域兜底及两条回归，真实原图现稳定返回“手办 + 单色雕塑/低多边形”。
- 历史恢复会重新计算推荐但不覆盖已选风格；推荐失败时卡片静默隐藏，成功态原因已压缩为一行，两个备选按钮实机切换正常。
- AI Python 全量回归 491/491、相关 `py_compile` 与 `git diff --check` 通过；Sidecar v7 健康检查和真实本机 multipart 推荐均通过，没有真实付费调用。
- Windows Release GUI 重编译与隔离完整 DLL 链接通过；最终 DLL SHA-256 为 `934DDA00CA48FDD7BB30E2E0D928CECF39127E8F3B60452654DE2FB5294FC306`，已放入 `build/src/Release` 等待日常启动器下次同步。
- 隔离 GUI 最终验收通过：历史原图与 AI 图同屏，推荐卡显示“推荐：手办 / 人像优先手办化，保留辨识度并减少恐怖谷感 / 单色雕塑 / 低多边形”；正式 PID 35472 全程未关闭。
- 阶段 87 完成；架构复核记录为 `Docs/architecture/2026-09-01-phase87-lightweight-style-recommendation-review.md`。

# 2026-09-01 阶段 89 启动：夜间非写实风格 Image2 优化

- 用户要求按约 10 小时继续优化除写实风格外的其他图像风格，允许持续使用 Image2，并在完成后把优化情况和图片资源写入指定飞书在线页面。
- 已冻结边界：本轮不改 `realistic`，不调用 Tripo；人物只参与手办、雕塑、低多边形、浮雕和微缩场景等非写实路线。
- 当前时间窗口按 2026-09-01 21:30 至 2026-09-02 07:30 组织为十个小时检查点；每轮都要更新清单、输出、评分、失败簇和下一步。
- 已确认 OpenAI Image2 兼容端点与密钥均已配置，模型默认 `gpt-image-2`；未输出任何凭据。
- 已确认目标链接可在当前已登录浏览器中打开，页面标题为“模型生成记录”，实际文档类型是在线表格；直接飞书 API 因缺少文档权限不能使用。
- 代码审计发现 `openai_preprocessor.py` 已具备低多边形、浮雕、微缩场景提示词，但 `image2_quality_benchmark.py` 仍只允许旧四风格；下一步先补齐评测契约和测试，再开始真实批量跑图。
- 既有 `image2-community-500-v1` 含 100 张 Wikimedia Commons 许可审计来源、20 个类别和 500 个历史输出，可作为今晚扩展的可追溯基线；不重复下载相同素材。
- 已把评测器公开风格扩展到 `low_poly/relief/diorama`，验证清单增加三类专属判定，扩展风格总览按“来源 + 每种活动风格”动态排布；旧三风格旅程联络表保持原布局兼容。
- 新增 `prepare_non_realistic_image2_benchmark.py`，从许可审计集冻结 20 个跨类别输入并加入 6 个文本压力用例；清单明确只有雕塑、手办、低多边形、浮雕、微缩场景，130 个候选中没有 `realistic`。
- 定向评测/提示词回归 55/55 通过；清单生成器与评测器回归 20/20 通过。首批 dry-run 确认 25 个调用、每风格 5 个、Tripo 0、阻塞候选 0。
- 首批真实 Image2 25/25 完成、0 失败、0 隐式重试；输入为木制关节人偶、双猫、孔雀、经典汽车、城堡，五种非写实风格均完成原始图、可打印参考图、评分和联络表。
- 首批质量门禁 23/25 通过，均分 79.48；浮雕 5/5、微缩场景 5/5、手办 5/5 通过，雕塑和低多边形各有 1 个阻塞。主要自动警告是耗材区域碎片化 19 次，下一轮需结合图片人工判断其是否过严。
- 已创建当前任务内每小时续跑的 ACTIVE 心跳 `image2`，共 10 次；提示词冻结写实风格、禁止 Tripo、要求每轮 dry-run/记录/回归，并在 07:30 最终总结后暂停。
- 首批总览位于 `generated_models/image2-non-realistic-overnight-v1/run/overview-sheets/primary/page-01.jpg`；飞书表格标签页已保留供最终汇总，但尚未提前写入未完成结论。
- 22:30 检查点先 dry-run 累计 `limit=50`：前 25 个复用、计划新增 25、每风格新增 5、Tripo 0、冻结不一致 0；随后执行第二批。
- 第二批新增茶壶、盆景、吉他、工业机械臂和牌楼五类输入，累计状态 49 complete / 1 failed / 80 pending，累计 Image2 50 次。唯一失败是 `plant_bonsai__low_poly__warm__r01` 的连接中断；候选已记录一次付费尝试，按契约不自动重试。
- 累计 49 张质量门禁 47/49 通过，均分 82.88；手办、浮雕、微缩场景已完成的 10 张均全部通过，雕塑 9/10、低多边形 8/9 通过。第二批结束后没有调用 Tripo。
- 更新后的第一页总览包含 10 个输入；本轮只记录重复失败证据，没有立即改生产提示词，以免破坏仍在执行的 v1 冻结提示词哈希。生产修正留到基线全部完成后的 v2 定向对照。
- 23:30 检查点用 `--skip-blocked` dry-run 累计 `limit=75`：49 个复用、跳过盆景低多边形失败候选、计划新增 25、Tripo 0、冻结不一致 0；随后执行第三批。
- 第三批新增章鱼、哈士奇、复古相机、无绳电钻和运动鞋，累计 73 complete / 2 failed / 55 pending，累计 Image2 75 次。新增失败为 `dog_husky__sculpture__natural__r01` 返回视觉全均匀图；该一次付费尝试已冻结，不自动重试。
- 累计质量门禁 71/73 通过，均分 85.27；手办、浮雕、微缩场景已完成的 15 张继续全部通过。雕塑 14 完成/1 服务输出失败，低多边形 14 完成/1 连接失败，二者完成样本的门禁通过率均为 13/14。
- 第二页总览已生成，覆盖章鱼、哈士奇、相机、电钻和双运动鞋。人物非写实、狮像、自行车、雨伞以及文本压力场景仍待后续检查点。
- 00:30 检查点 dry-run 累计 `limit=100`：73 个复用、跳过 2 个已付费失败候选、新增 25、Tripo 0、冻结不一致 0；随后执行第四批。
- 第四批新增石狮、爱因斯坦非写实、卓别林全身、侧面自行车、雨伞下的猫，累计 98 complete / 2 failed / 30 pending，累计 Image2 100 次；本批 25/25 完成、0 新失败。
- 累计质量门禁 96/98 通过，均分 85.88。低多边形累计 19 完成/1 服务连接失败，完成样本 18/19 通过、均分 82.21；手办、浮雕、微缩场景的 20 个完成样本仍分别全部通过。
- 第二页总览现完整覆盖 10 个图生图输入；低多边形在石狮、爱因斯坦、卓别林、自行车和伞下猫上都呈现明确大平面语言并保持核心轮廓/数量。
- 01:30 检查点 dry-run 累计 `limit=125`：98 个复用、跳过 2 个已付费失败候选、新增 25、Tripo 0、冻结不一致 0；随后生成早餐托盘、咖啡店角落、山地露营、水母台灯、深色棋组五组文本压力用例。
- 第五批 25/25 的候选状态均完成，累计 123 complete / 2 failed / 5 pending、Image2 125 次、Tripo 0。最终汇总首次被 Windows 对一个旧 `state.json` 的瞬时读锁打断，但候选状态与图片均已安全保存；随后 `--report-only` 完整恢复汇总和联络表，没有重发任何 Image2 请求。
- 恢复后累计质量评估 124 项、120 通过、4 阻塞、均分 83.0；手办、浮雕、微缩场景各 25 个完成候选仍全部通过。第三页总览已包含五组文本压力用例，家具阅读角 5 个候选待下一轮。
- 根据这次真实 Windows 读锁，评测器 `_read_json` 增加仅针对短暂 `PermissionError` 的本地指数退避；不改变提示词、候选哈希或付费调用逻辑，并新增定向回归。
- 02:30 检查点 dry-run 累计 `limit=130`：123 个复用、跳过 2 个已付费失败候选、只剩家具阅读角 5 个新候选、Tripo 0、冻结不一致 0；随后执行第六批。
- 家具阅读角五风格 5/5 完成，首轮基线最终为 128 complete / 2 failed，累计 Image2 130 次、Tripo 0；两个失败均保留一次付费尝试且从未自动重试。
- 基线最终质量评估 129 项、124 通过、5 阻塞、通过率 96.12%、均分 82.73。手办、浮雕、微缩场景各 26 个候选全部完成并通过自动门禁；雕塑 25 complete/1 failed，低多边形 25 complete/1 failed。
- 三页总览、256 份风格联络表（128 个完成候选 × raw/model-reference）、CSV/JSON 汇总和验证清单均已生成。下一阶段停止扩展 v1，转入失败簇修正和独立 v2 对照。
- 03:30–04:30 难例修正已进入生产提示词：`relief` 的实心背板和 `diorama` 的共享地台现在是最高优先级样式例外，分别覆盖通用人物展示底座与非人物无底座规则；所有非写实风格同时增加高优先级去文字/品牌/伪字形合同，`realistic` 路线保持原样。
- 新增三组提示词契约回归，验证图生图与文生图都获得浮雕/微缩支撑例外，普通手办仍保留旧无底座规则，写实提示中不出现任何新非写实合同。提示词与评测定向回归 59/59 通过，语法与 `git diff --check` 通过。
- 建立独立 `image2-non-realistic-targeted-v2` 对照集，对工业机械臂、茶壶、复古相机、无绳电钻、卓别林以及 6 个文本场景执行 55 次 Image2；55/55 完成、0 失败、Tripo 0，没有重试 v1 的两个已付费失败候选。
- v2 人工总览确认：11/11 浮雕都形成可见背板，工业机械臂/卓别林/露营地/水母灯/阅读角等 v1 失败簇均已纠正；器物和文本场景的微缩结果都获得共享托盘、棋盘、地面或最小地台；机械臂、相机、电钻没有再出现可读品牌或公司字样。
- v2 自动门禁评估 55 项、51 通过、4 阻塞、通过率 92.73%、均分 81.49。4 个阻塞全部是单色雕塑多物场景被 `fragmented_subject` 启发式判定，人工图中各元素已有托盘/棋盘/地面连接，继续按门禁校准问题而非 Image2 风格回退记录。
- 评测总览把无原图的文本用例从误导性的 `PENDING` 改为 `TEXT INPUT`；评测器回归 19/19 通过，v2 `--report-only` 已无付费重新生成总览和汇总。
- 04:30–05:30 资源检查点复核现有覆盖：20 个许可审计图片来源已经包含人物、动物、建筑、机械、器物、薄结构、植物和交通工具，6 个文本压力用例补齐食物、家具、室内外场景、透明与深色主体；继续下载相似网络素材的边际价值低，本轮没有新增下载或 Image2 调用。
- 新增通用资源目录生成器 `build_image2_resource_catalog.py` 及 2 条回归；合并 baseline-v1 与 targeted-v2 后导出 26 个唯一用例、185 个候选、183 个完成图和 2 个失败状态，所有完成项都能解析到原图、模型参考图、风格联络表和总览页。
- 资源包位于 `generated_models/image2-non-realistic-overnight-resources-v1/`，包含 JSON 总表、来源许可 CSV、候选图片 CSV、飞书精简 TSV 和 185 行飞书资源明细 TSV；目录生成器 2/2、语法与差异检查通过。
- 已生成本地报告 `Docs/reports/2026-09-02-non-realistic-image2-overnight-report.md` 资源审计稿，含结论、风格定位、覆盖、修正前后证据、图片路径、限制和飞书写入准备；12 个本地 Markdown 链接全部存在。最终检查点再补全量回归和飞书动作状态。
- 05:30–06:30 最终 dry-run：targeted-v2 的 55 个候选全部可复用、计划 Image2 0、Tripo 0、冻结不一致 0；baseline-v1 因生产提示词已修正而显示 130 个 `provider_prompt_sha256` 冻结不一致，但计划付费调用仍为 0，证明旧基线不会被误续跑。
- 全量 AI Python 回归 504/504 通过，所有 Tripo 相关测试均为 Mock/loopback 合同夹具；全量 Python `py_compile`、两个清单无 `realistic`、资源目录 183 个完成项路径零缺失及整个工作树 `git diff --check` 通过。
- 资源目录新增两页“baseline-v1 vs targeted-v2”同屏对照，覆盖 11 个共享难例的浮雕和微缩场景；人工复核能直接看到背板、连续地台和去品牌文字的修正效果。资源目录生成器回归增至 3/3 通过。
- 报告已更新为全量回归稿并加入两页前后对照；最终仅剩按外部写入规则请求确认后写入飞书，以及 07:30 停止心跳。
- 06:30 最终检查点通过专用飞书表格接口解析目标为 `模型生成记录` / `Sheet1`，读取 A1:H30 确认 240 个单元格全为空；30×8 最终矩阵 dry-run 通过，不会覆盖已有内容。
- 接口真实写入返回 Feishu `91403 Forbidden`，说明当前应用可解析/读取但没有写权限；未发生任何云端修改。已保留当前登录的飞书页面，浏览器粘贴 A1:H30 是可行兜底，但按动作时确认规则必须先得到用户确认。
- 本地报告已转为最终稿，线上状态明确记录为“等待动作时确认”；心跳当前继续保留以等待用户在 07:30 前确认，若未确认则按计划停止自动检查并保留可随时恢复的写入矩阵。
- 07:39 超过十小时窗口后，用户尚未回复浏览器写入确认；夜间 `image2` 心跳已删除并停止后续自动检查。所有本地成果和 30×8 飞书矩阵均已保留，后续收到“确认写入飞书”即可恢复一次性粘贴和读回，不需要重跑 Image2。
- 阶段 89 的本地优化、资源、报告和回归全部完成；仅飞书云端写入因接口权限及动作时确认保持待办，计划状态转为 `awaiting_user_confirmation`。
- 09:52 用户明确确认写入飞书；已通过当前登录的飞书页面将冻结的 30×8 汇总矩阵写入“模型生成记录”/`Sheet1!A1:H30`。为避免页面批量剪贴板被拦截，最终采用逐单元格定位、输入和提交，并在每批后确认已保存到云端。
- 最终使用独立只读接口读回 `A1:H30`，结果为 30 行 × 8 列、revision 211；对日期序列值和 URL 富文本做等价归一后，与冻结矩阵语义比对 0 处差异。阶段 89 状态转为 `complete`，无需重跑 Image2 或恢复夜间心跳。

# 2026-09-02 阶段 90：合并主线、内部版本与网站发布

- 用户明确要求把当前版本合并到主分支、同步主分支最新代码、生成内部版本、发布到网站并最后推送远端；这些外部写入动作均已获得本次请求授权。
- 已按 `Code`、`git-essentials` 和 `planning-with-files-zh` 恢复开发偏好与计划上下文。既有偏好要求模型生成只在 `codex/model-generation` 演进，正式集成/发布在 `codex/orca-integration-v2` 完成，并以完整验收 SHA 交付。
- 当前工作区位于 `codex/model-generation`，HEAD 为 `5081891766`，工作树包含本轮尚未提交的代码、测试和文档；`.tmp/`、`tmp/`、`validation/` 等本地生成/验证目录同时存在，必须从提交范围排除。
- 本地分支列表没有 `main`；`codex/orca-integration-v2` 位于独立工作树 `D:/Workspace/06_3DDY_orca_integration_v2`。下一步先抓取远端并解析实际主线、网站与发布脚本，再开始提交和合并。
- 已抓取 `origin` 与 `upstream`。`origin/HEAD` 明确指向 `origin/codex/orca-integration-v2`，因此 fork 的发布主线是集成分支；官方主线为 `upstream/main=b6ef6cf1be`。本地集成工作树干净但落后 origin 1 个提交 `bbdfc62618`。
- `origin/codex/orca-integration-v2` 相对 `upstream/main` 为官方侧 81 个提交、集成侧 29 个提交；最近共同基线为 `db29f570bd`。当前模型分支与集成线没有可直接合并的共享验收历史，发布应继续使用完整源 SHA + 增量移植，而不是把整条功能分支历史直接 merge 进集成线。
- 集成分支已有 `scripts/package_internal_fast.ps1` 和 `release/` 便携发布工具；网站不在该工作树中，发布工具按本机未提交配置定位独立网站工作树和受限上传目标。
- 已完整读取内部发布 runbook 与构建/上传/公网验证入口。权威流程要求干净的 `codex/orca-integration-v2`、外置 internal defaults、精确 source SHA、目标测试、安装包 SHA-256、原子上传、网站三文件定向更新与公网 Range/health 验证。
- 当前集成工作树没有 `release/config.local.ps1`；下一步只读定位既有操作员配置/网站工作树，绝不输出 SSH 目标、员工号或密钥。若本机确实缺少配置，构建仍可继续，上传发布则会成为需要用户补充外部状态的唯一阻塞。
- 当前模型分支正式未跟踪项已收敛为 10 份设计/架构文档、6 份报告（含 1 张 217 KiB 对照图）和 13 个生产/测试脚本；临时/验证目录不进入候选提交。
- 模型分支全量 AI Python 回归重新执行并以 504/504 通过，用时 149.127 秒；所有生成/付费文案均来自 Mock/loopback 测试，未创建真实 Image2 或 Tripo 调用。
- 两次候选凭据扫描分别因 PowerShell 条件语法和泛型列表参数展开失败，结果均明确作废且未进入暂存；第三次改用普通字符串数组后再决定是否提交。
- 普通数组扫描确认只有 `scripts/package_windows_ai_test.ps1` 和 `tools/ai/test_sidecar_contract.py` 命中 API Key“变量赋值”宽泛规则；分类结果均为环境传递、空值清理或测试夹具，没有命中 `sk-`、私钥头或 AWS Key 高置信度模式。后续用简单 `rg` 全树复核，不再扩展脆弱的 PowerShell 内联解析器。
- 简化后的全树 `rg` 高置信度扫描确认正式源码/文档中 `sk-`、私钥头和 AWS Key 模式 0 命中；`git diff --check` 通过。
- 已精确暂存 73 个正式文件，禁止目录暂存数为 0；`.tmp/`、`tmp/`、`validation/` 仍作为未跟踪本地产物保留，没有删除或提交。首轮 Markdown 尾随空格已改为等价段落结构，暂存差异检查通过。
