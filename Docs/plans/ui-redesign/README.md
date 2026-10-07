# OrcaSlicer UI Redesign Migration

日期：2026-09-22

## 专项需求基线

- [3D 模型新 UX 需求与交互方案基线](2026-09-30-3d-model-ux-requirements.md)：固化生成结果导入、模型检查、3D 美颜、颜色匹配、正式导入、AI/Orca 切片和 Preview 的已确认需求，并包含后续架构、详细设计与实施提示词。

## 目标

在不改变 OrcaSlicer 现有项目、模型、预设、切片、预览、打印机、AI 和撤销重做业务行为的前提下，逐步替换桌面 UI 的视觉风格和交互方式。

本计划采用绞杀式迁移：新界面作为展示层逐步接管旧界面，业务逻辑继续由现有 Orca 模块提供。每个阶段都必须可以独立编译、单独验证；旧 UI 只作为迁移期间的启动兼容路径，新 UI 激活后不得再返回旧 UI。

## 当前基线

- 源码基线：`decf64fab128422f3666e71fe034fc411566bd6a`
- 当前分支：`codex/team/integration`
- 构建基线：Windows x64 Release，Visual Studio 17 2022
- 当前 UI 组合根：`src/slic3r/GUI/MainFrame.cpp`、`src/slic3r/GUI/MainFrame.hpp`
- 当前业务状态中心：`src/slic3r/GUI/Plater.cpp`、`src/slic3r/GUI/Plater.hpp`
- 现有可复用边界：`src/slic3r/GUI/AI/AIDesktopFeatureHost.*`、AI FeatureHost、Coordinator 和现有 ActionRegistry

## 风险控制原则

1. 不在 UI 重构中修改 `libslic3r`、项目格式、预设格式、后台切片线程、Provider 协议或 Undo/Redo 数据结构。
2. 新 UI 不直接复制模型、项目或切片状态；所有业务状态只有一个来源。
3. 新旧界面调用相同的稳定命令，使用相同的业务适配器。
4. 先建立命令和状态边界，再替换页面；先替换外壳，再替换深层控件。
5. 保留运行时 Feature Flag 用于分阶段启用和启动兼容；图像首页已改为默认入口。旧 UI 可以在进入新 UI 前作为兼容启动路径，但新 UI 激活后不提供返回旧 UI 的运行时路径。
6. 视觉验证、业务回归和真实主窗口验收分别记录，不用编译结果代替 GUI 验收。

## 分阶段路线

### 正常启动默认入口（2026-10-07）

- 按用户要求，启动窗、初始化缓冲页、首次配置和完整 3D 工作台／切片流程默认启用；正常双击 `orca-slicer.exe` 即可进入，无需审核脚本。
- 正常启动使用 Orca 默认用户数据目录，复用已有配置、历史资产和本地 AI 服务发现。隔离审核脚本继续用于独立验收配置。
- 携带输入文件、默认 Prepare 和工程恢复继续使用既有启动兼容规则。已有有效打印机配置跳过首次配置，不重置用户预设。
- 页面开关 `ORCASLICER_UI_REDESIGN_STARTUP_SPLASH`、`STARTUP_BUFFER`、`STARTUP_SETUP` 和 `MODEL_WORKFLOW` 可分别显式关闭；后三项同样使用完整 `ORCASLICER_UI_REDESIGN_` 前缀。全局 `ORCASLICER_UI_REDESIGN=0` 关闭未被页面开关覆盖的新版入口。原 `ORCASLICER_MODEL_WORKFLOW_REVIEW` 继续作为完整模型流程的优先覆盖开关。
- 默认启用不代表统一视觉或实体打印验收通过。四项工作台修正的 Windows 候选与未验证范围保留在本地验收记录。

### UI-001：基线和边界（当前阶段）

- 固定旧 UI 的业务流程基线和输入资产。
- 新增独立的 `src/slic3r/GUI/Redesign/` 基础层。
- 新增默认关闭的 Feature Flag。
- 新增稳定 Command Registry 骨架。
- 新增只读 State Snapshot/Store 骨架。
- 新增不接管现有页面的空 Shell。
- 不改变 `MainFrame`、`Plater` 的现有行为。

### UI-002：业务适配器

- 将项目、模型、预设、切片、导出、打印和撤销重做操作包装成稳定命令。
- 将 `Plater` 内部状态映射为只读状态快照。
- 为命令的 `CanExecute`、成功结果和失败原因建立契约测试。

### UI-003：新 Shell 和导航

- 接入新 Shell，并按启动意图选择初始页面；无输入文件、默认 Prepare 或恢复数据时进入新 Shell。
- 旧 UI 可以进入新 UI；新 UI 激活后，旧 Tab 请求、通知和快捷键统一翻译为新页面，不再直接切换旧 Notebook/Plater。
- 新旧页面共享全局命令、快捷键、状态和错误处理；尚未迁移的业务页面先使用新 Shell 的占位 host。

### UI-004：逐页替换

推荐顺序：

1. 首页和工作区入口
2. 全局导航和顶部工具栏
3. Preset 选择外围布局
4. Prepare 外壳
5. Sidebar
6. Preview 外壳
7. Device 页面
8. AI 页面
9. 设置、弹窗和异常提示

3D Canvas、Gizmo、对象树、选择同步和 Undo/Redo 最后迁移，初期继续嵌入旧控件。

### UI-005：旧 UI 收缩

- 新旧业务结果对照通过后，逐页关闭旧 UI 默认入口。
- 保留迁移期间的启动兼容开关和旧 UI 入口；不新增“新 UI → 旧 UI”的运行时回退。
- 删除旧页面前完成源码、测试、构建和实际主窗口验收记录。

## 必须覆盖的行为基线

- 新建、打开、保存和另存项目
- 导入 STL、OBJ、3MF 等模型
- 选择、移动、旋转、缩放、复制和删除
- Undo/Redo 与项目 dirty 状态
- 打印机、材料和工艺预设切换
- 开始、取消、失败恢复和后台切片
- Preview、G-code 导出和发送到打印机
- 关闭确认、自动恢复和重启恢复
- AI 生成、导入、取消、恢复和切页

## 验收门槛

- 命令契约测试：相同初始状态和命令序列产生相同业务状态。
- 新旧 UI 对照：项目、模型、预设、切片状态、G-code 和 Undo/Redo 结果一致。
- 真实主窗口：成功路径、取消路径、失败恢复、切页、导入和恢复均按本轮范围实测。
- 每个阶段记录源码 HEAD、构建配置、修改文件、测试日志、EXE/运行时身份和未验证范围。

## 当前实施记录

### 2026-09-22

- 已创建本计划文档。
- 已新增 `RedesignFeatureFlags`、`RedesignCommandRegistry`、`RedesignStateStore` 和 `RedesignShell` 的最小基础层。
- Feature Flag 默认关闭，当前未接管 `MainFrame` 或任何旧页面。
- 已将基础层加入 `libslic3r_gui` 构建，并为命令注册表和状态快照增加 `[UiRedesign]` 自动测试。
- Release 增量编译通过；`slic3rutils_tests.exe [UiRedesign]` 通过 4 个用例、24 个断言。
- 运行时 GUI 验收尚未执行；本轮没有启动主窗口，也没有改变旧 UI 的默认路径。
- 已新增 `OrcaBusinessAdapter`，将项目、导入、保存、删除、撤销重做、重新切片和 G-code 导出映射到现有 `Plater` 公共 API。
- 适配器不会注册打印机/材料/工艺选择、发送打印或后台切片取消，直到这些操作具备与旧 UI 等价的校验和生命周期语义。
- 已完成 Windows x64 Release 增量构建；`OrcaSlicer.dll` 和 `slic3rutils_tests.exe` 已生成，构建同时通过了隔离 AI runtime 的 Pillow 校验。
- 本轮验证源代码基线为提交 `7016255522` 及当前未提交适配器改动；主程序目标构建成功，但尚未启动主窗口，未执行 GUI 验收。
- 下一步是在不改变旧路径的前提下接入一个可回退的 Shell 入口。

### 2026-09-22 主界面 Shell 第一刀

- 已新增基于 Figma 主界面的静态 `RedesignShell`，当前只呈现 AI 创作首页中的“图像”Tab。
- 已在 `MainFrame::update_layout()` 增加最小路由：仅当 `ORCASLICER_UI_REDESIGN` 或 `ORCASLICER_UI_REDESIGN_IMAGE_HOME` 开启且处于编辑器模式时显示新 Shell；默认仍显示旧 Notebook/Plater。
- 新 Shell 当前包含左侧导航、上传图片面板、描述输入、模型选择、技能按钮、禁用的生成按钮和三步引导区；未接入 AI 请求，也不会自动导入 Prepare。
- `orca-slicer.exe` 已通过 `dev.ps1 Run -BuildDir build -Jobs 4` 使用独立运行目录启动，启动时保持进程响应并拥有有效主窗口句柄；隔离运行时和 Pillow 校验通过。
- `slic3rutils_tests.exe [UiRedesign]` 通过 4 个用例、24 个断言。
- 由于当前会话的 Computer Use 仅暴露浏览器接口，无法读取或操作原生 Orca 窗口；本轮 GUI 视觉和控件交互验收未完成，不将进程启动结果等同于 GUI 通过。
- 下一步先完成一次可用的原生窗口视觉/交互走查，再根据反馈进入图像 Tab 的本地交互和 AI 流程接入。

### 2026-09-23 图像页本地选择

- 在现有启动路由和图像 Shell 工作树上加入点击、拖拽选择一张本地图片、加载提示、缩略图、中央预览、移除和失败后重试。复用 `ModelGenerationPresentation::is_supported_image` 的 PNG/JPEG magic、20 MiB、可解码和最短边 64 px 校验；不向服务端上传，不触发生成、导入或切片。生成按钮保持禁用。
- 使用设计稿导出的导航、模型和引导图资源；缩略图、预览使用用户选择的本地图片。字体、原生控件边框、图标和位置仍以用户的真实窗口截图为最终视觉验收依据，不将构建通过视为像素级验收。
- 源码基线 `da3e79dd76c164c2ea78905c587cdb312cd18e62` 加当前未提交改动；运行时检查的源码快照标识 `46f73928b5cad29db23c72885417d19500e52cd3698119dd10ec4f307798c2b2`。Windows VS2022 x64 Release 增量编译、完整开发运行目录安装及隔离 Python/Pillow 检查通过；记录在 `.tmp/dev/logs/20260923-161354-980/result.json`。
- `slic3rutils_tests.exe '[UiRedesign],[ImageSelection]'` 通过 6 例、38 断言，日志 `build/ui-redesign-tests-20260923.log`。运行目录为 `.tmp/dev/run`，其中 `orca-slicer.exe` SHA256 为 `9D1046F3AE5734E3595FBA64C4E7A05F13F0BCB9CAC5112698CE84FED6D1FDA3`，`OrcaSlicer.dll` SHA256 为 `8B82C8C014D977220A069F234B4B2D66C418903397AC3C03E73987831C7B467F`。
- 本轮按用户分工未启动真实主窗口；点击、拖拽、取消、失败恢复以及视觉对照尚待用户验收，未调用付费服务。

### 2026-09-23 默认入口与拖动卡顿排查

- 编辑器在没有待打开文件、默认 Prepare 或恢复数据时，默认显示新图像首页；`ORCASLICER_UI_REDESIGN_IMAGE_HOME=0` 或全局 `ORCASLICER_UI_REDESIGN=0` 可以显式退回旧界面，页面级设置优先。其他页面不随之自动启用。
- Windows 标题栏使用原有系统 `WM_NCLBUTTONDOWN/HTCAPTION` 拖动，普通移动本身不会触发图像预览的 `wxEVT_SIZE`；目前没有实际拖动采样，不能将缩放直接认定为长按拖动延迟的根因。排查发现启动完成阶段曾重新开启隐藏的 3D 画布渲染标志；未初始化的画布会跳过渲染及 idle 工作，因此无法断定它造成当前卡顿，但现在只在进入旧 3D 页面时启用。图片预览在窗口连续改变尺寸时曾同步执行高质量缩放和 Layout，现延后至尺寸稳定 150 ms 后更新。
- Release 增量构建和定向测试通过（7 例、43 断言），构建日志 `build/ui-redesign-build-20260923.log`、测试日志 `build/ui-redesign-tests-20260923-default.log`。原 `.tmp/dev/run` 正被现有进程使用，未覆盖；候选安装于 `.tmp/ui-redesign-trial-20260923/run`，资源、安装版 sidecar 文件和隔离 Python/Pillow 检查通过。基线为 `da3e79dd76` 加当前未提交工作树；候选 `orca-slicer.exe` SHA256 `9D1046F3AE5734E3595FBA64C4E7A05F13F0BCB9CAC5112698CE84FED6D1FDA3`，`OrcaSlicer.dll` SHA256 `4DE169A2173A33E44C76C7A016B2EB492753D5B3128C5D4A60D233AE9D3DCFEA`。
- 未启动新候选进行真实鼠标拖动测量；新窗口拖动、窗口尺寸变化和旧 3D 页面切换的体感效果由用户验证，未把代码路径分析或编译通过当作性能验收。


### 2026-09-23 单向迁移边界与拖动诊断补充

- 当前 UI 整改按“旧 UI → 新 UI，禁止新 UI → 旧 UI”的单向边界执行。`MainFrame::select_tab()` 是旧业务 Tab 请求的统一路由：`Home` 映射到 `Image`，`Generate 3D` 映射到 `Model`，`Prepare`、`Preview`、`Monitor` 映射到 `Print`；未完成映射的请求只记录并忽略，不直接唤回旧 Notebook/Plater。
- `EVT_SELECT_TAB`、切片快捷键、切片完成后的 Preview 跳转、Monitor/多设备跳转以及网络插件热重载前的离开 Monitor 操作已改为经过统一路由。新 Shell 激活后，旧 UI 的业务通知、快捷键和事件不得直接调用 `m_tabpanel->SelectPageByName()`。
- 启动兼容例外仅发生在进入 Shell 之前：携带输入文件、配置要求默认 Prepare 或存在恢复数据时，首屏直接从旧工作区启动，避免出现可见的“新 UI → 旧 UI”跳转；这不是新 UI 的回退路径。新 Shell 激活后，`show_redesign_shell(false)` 请求会被拒绝并记录日志。
- 当前 `Assets`、`Model`、`Print` 是新 UI 的占位 host，尚未承载完整的模型、切片、Preview、打印机和多设备业务；它们只能证明导航边界，不能宣称旧业务已经迁移完成。下一阶段应逐页把业务 host、状态快照、命令、通知和失败恢复接入新页面，再关闭对应旧页面入口。
- 标题栏诊断通过 `ORCASLICER_UI_REDESIGN_DRAG_TRACE=1` 开启，日志前缀为 `[UiRedesignDrag]`，覆盖 Topbar 鼠标按下、`WM_NCLBUTTONDOWN`、`WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE`、窗口位置变化和 `wxEVT_SIZE` 分段耗时。Windows 自定义标题栏拖动调用已从异步 `PostMessage` 改为同步 `SendMessage`，以排除消息队列等待造成的启动延迟；新 Shell 下旧 Plater resize 通知和 `fit_tab_labels()` 已跳过。
- 已自行启动候选程序并采集启动/resize 日志；日志显示新 Shell 的 `legacy_resize_work=false`，但当前会话无法把真实鼠标输入注入到 Orca 原生窗口，因此尚未采集到完整的 `left down → WM_NCLBUTTONDOWN → WM_ENTERSIZEMOVE` 链路。标题栏“长按拖动严重延迟”仍需在真实桌面上用该环境变量复现后，结合日志完成最终定位。
- 本轮最新完整主程序构建因 GUI 预编译头重建导致 `cl.exe` 内存持续增长，在约 12 GB 工作集时主动中止；此前的成功候选和定向 C++ 测试记录仍保留，但不能把此次未完成构建写成最新修改已通过完整构建。后续应在资源充足时重新构建，再运行真实窗口验收。

### 2026-09-23 业务访问边界收紧

- 新 Shell 激活后，当前页状态统一由 `MainFrame::selected_tab_id()` 提供，旧 Notebook 不再作为业务判断的唯一来源；启动禁用/启用、网络插件热重载、菜单可用性、打印机视图判断以及 AMS 刷新均经过该边界。
- `Plater` 中切片完成后的 Preview 跳转已改为 `MainFrame::select_tab()`；旧面板选择请求在新 Shell 激活时只映射到新页面或记录忽略，不再直接操作旧 Notebook。
- 这一步只收紧导航和状态读取边界，不等同于业务页面已经迁移。`GUI_App::tab_panel()`、旧 Tab 创建和旧 3D 业务仍保留为启动兼容/迁移实现；后续每接入一个新业务 host，都要同步移除对应的旧控件依赖和旧页面入口。
- 本次针对修改后主程序的增量构建在 GUI 预编译头阶段因 `cl.exe` 工作集达到约 12 GB 被主动停止，未形成新的可运行候选；因此本轮代码改动尚未完成主程序构建验证。

### 本轮复核：单向路由收紧、构建与启动日志

- 继续按“旧 UI 请求只允许进入新 Shell；新 Shell 不回退旧 Notebook”的边界收紧业务入口。`SelectMachineDialog::navigate_to_timelapse_page()` 已改为调用 `MainFrame::jump_to_monitor_media()`，不再直接访问旧 `MonitorPanel` 的 `m_tabpanel`；打印完成、校准完成、打印错误也继续统一经过 `MainFrame` 路由。旧控件访问仅保留在 MainFrame 的 legacy 分支或旧控件自身内部。
- 本轮修改后的主程序增量构建已完成，结果记录：`.tmp/dev/logs/20260924-070206-854/result.json`，构建步骤 exit code `0`，耗时约 `1894.95 s`，目标为 `OrcaSlicer_app_gui`。这条记录更正此前“本轮尚未完成主程序构建验证”的过时描述。
- 定向测试已通过：`slic3rutils_tests.exe '[UiRedesign],[ImageSelection]'`，7 个用例、43 个断言；未执行与本次 UI 边界无关的全量测试。
- 已用当前构建启动隔离实例，PID `35064`，数据目录 `.tmp/ui-redesign-drag-diagnosis-current`，启动后保持存活并正常生成日志。日志：`.tmp/ui-redesign-drag-diagnosis-current/log/debug_Thu_Sep_24_07_34_17_35064.log.0`。
- 启动日志中，新 Shell 的 `wxEVT_SIZE` 处理为约 `40–185 us`，`legacy_resize_work=false`，`fit_labels_us=0`；旧 UI 初始化阶段的一次 `fit_tab_labels()` 约 `999 us`。这些数据说明当前已观测到的 resize/layout 路径不是“非常大的”拖动延迟来源。
- 本轮仍未获得真实鼠标长按标题栏拖动的完整事件链：日志没有 `CenteredTitle/BBLTopbar left down`、`WM_NCLBUTTONDOWN`、`WM_ENTERSIZEMOVE` 和 `WM_EXITSIZEMOVE`。因此不能把隔离实例启动成功或启动 resize 日志写成标题栏拖动已验收；下一步必须在可操作的真实桌面上复现并采集该链路。

### 2026-09-24 图像首页视觉差异修正（进行中）

- 问题与依据：用户提供的当前窗口截图对照 Figma `208:27221`，右侧引导内容贴顶、图标/控件露出方角、描述输入后文字过暗。当前 `RedesignShell` 在引导区外保留隐藏预览的弹性空白，PNG 资源四角为不透明背景，部分矩形控件使用原生方形背景；输入框指定了半透明白字但实际窗口未达到可读性要求。
- 修改假设：将未选图引导区独占内容区，按状态切换居中的预览 host；为需要圆角的容器显式绘制圆角，修复图标透明角并替换技能占位符；输入正文改用不透明浅色。构建/代码测试后提交完整候选路径与文件哈希，由用户负责最终视觉及主窗口验收。若 Windows 原生输入框仍覆盖文字颜色，保留为未解决项继续定位。

### 2026-09-24 单向迁移边界复核、最新构建与拖动日志分析

- 按“逐步替换、旧 UI 可以进入新 UI、新 UI 不再进入旧 UI”的目标复核当前改动。新 Shell 激活后，`MainFrame::select_tab()`、设备/多设备/校准/监控/HMS/升级/LiveView/Rack/Media 跳转、菜单命令以及 Ctrl+R/G/Shift+G/J/N/O/S/Shift+S/I/F 等快捷键均经过 `MainFrame` 语义路由；不再把新 UI 的请求直接投递到旧 Notebook/Plater。旧控件访问仅保留在 MainFrame 的 legacy 分支、迁移期间的状态兼容逻辑和旧控件自身内部。
- 新 UI 激活后调用 `show_redesign_shell(false)` 会被拒绝并记录诊断日志；旧 UI 仍可在进入新 Shell 之前作为输入文件、恢复数据和默认 Prepare 等启动兼容入口。当前 `Assets`、`Model`、`Print` 仍是占位 host，因此本轮完成的是单向边界和业务入口收紧，不是完整业务迁移；下一阶段应把真实命令、状态快照、通知、错误恢复和项目/模型/切片业务逐页接入新 host，再逐步移除对应旧控件依赖。
- Windows 自定义标题栏拖动已移除 `CaptureMouse()`/`ReleaseMouse()`，保留同步 `SendMessage(WM_NCLBUTTONDOWN, HTCAPTION, ...)`；这样先排除 wx 鼠标捕获与 Windows 原生移动循环叠加的风险。`ORCASLICER_UI_REDESIGN_DRAG_TRACE=1` 仍可输出 `[UiRedesignDrag]` 诊断链路。
- 最新源码运行时 identity：`344b8f8115f815f0ad45aa9669942934200acbc71386f6c8be15f4a47573ea62`。Windows x64 Release 完整构建通过，结果 `.tmp/dev/logs/20260924-093500-749/result.json`，构建日志 `.tmp/dev/logs/20260924-093500-749/build.log`；`orca-slicer.exe` SHA256 `9D1046F3AE5734E3595FBA64C4E7A05F13F0BCB9CAC5112698CE84FED6D1FDA3`，`OrcaSlicer.dll` SHA256 `7299E6511568DFE06C04DE650013434BF1AE7C446B92FE1D6129F2750609A643`。构建过程有既有 `LNK4098` 和 `/LTCG` 警告，但最终 exit code 为 `0`。
- 定向测试 `slic3rutils_tests.exe '[UiRedesign],[ImageSelection]'` 通过 7 个用例、43 个断言。
- 已自行启动隔离实例并读取日志：`.tmp/ui-redesign-drag-diagnosis-current/log/debug_Thu_Sep_24_07_34_17_35064.log.0`。新 Shell 的 `wxEVT_SIZE` 约 `40–185 us`，`legacy_resize_work=false`，`fit_labels_us=0`；旧 UI 初始化阶段曾有一次约 `999 us` 的 `fit_tab_labels()`。当前采样没有显示 resize/layout 是“非常大的”标题栏拖动延迟来源；一次合成消息链路中同步 `SendMessage` 约 `5.7 ms`，但不等同于真实鼠标拖动验收。
- 真实桌面上的完整 `left down → WM_NCLBUTTONDOWN → WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE` 链路仍未采集；一次鼠标注入命中了 Windows 锁屏界面，另一次最新隔离启动很快退出且未生成有效日志，不能据此判断程序功能失败。随后再次启动尝试被平台拒绝，未通过更换入口、拆分命令或其他方式绕过。标题栏长按拖动问题因此仍标记为“已排除一处双重捕获风险、尚未完成真实体验定位”，需要在可操作桌面上复现并反馈带 `[UiRedesignDrag]` 的日志。
### 2026-09-24 图像首页视觉差异修正（候选交付，待用户验收）

- 右侧引导区不再与隐藏预览共享弹性空白；空态引导区独占内容高度，预览改由独立 host 上下居中，并在选图后重新布局和缩放。上传/描述/模型/技能等卡片及导航容器绘制圆角；上传加号和技能图标采用内缩圆角 tile，技能占位符替换为透明资源。Logo、模型图标、头像和导航图标补齐透明角，安装资源的 11 张相关图像均检查到透明角与非透明主体。描述正文使用不透明浅色 `RGB(230,230,233)`，不再沿用半透明文字色。
- 构建基线：HEAD `da3e79dd76` 加现有未提交工作树（非干净集成提交）；Windows x64 Release，`build` 目录增量 `OrcaSlicer_app_gui` 通过，构建记录 `.tmp/dev/logs/20260924-104657-531/result.json`，其中源码 identity `4368c621118a49654ca9e1a426509ceeaca3d9297d307ea307dc5dd5532a0388`。定向 `slic3rutils_tests.exe '[UiRedesign],[ImageSelection]'` 通过 7 例／43 断言；日志 `.tmp/dev/logs/20260924-ui-redesign-tests-build.log` 和 `.tmp/dev/logs/20260924-ui-redesign-tests.log`。
- 完整候选目录 `.tmp/ui-redesign-visual-20260924/run`（未覆盖已有运行实例），安装命令退出码 0，日志 `.tmp/dev/logs/20260924-ui-redesign-install.log`。EXE SHA256 `9D1046F3AE5734E3595FBA64C4E7A05F13F0BCB9CAC5112698CE84FED6D1FDA3`；`OrcaSlicer.dll` SHA256 `3866FF9E290F6BF4D1473631517EAF913F503A9F4B78734FF068D9AEB416993B`。基础资源、新技能资源、安装版 sidecar/bootstrap 与 Python 均存在，隔离 Python 3.12.13/Pillow 12.2.0 的 PNG round-trip 检查通过。完整相关输入与产物哈希见候选目录旁的 `source-snapshot.json`。
- **验收边界**：本轮不启动候选程序、不宣称真实窗口视觉已通过；用户自行检查默认引导垂直位置、角部透明/圆角、描述中英文输入/焦点状态，以及选图后的预览切换和移除恢复。若原生控件或 DPI 环境仍有差异，以用户实测截图回报为后续修正依据。回退基线是先前 `.tmp/dev/logs/20260924-093500-749/result.json` 所对应的 DLL SHA256 `7299E6511568DFE06C04DE650013434BF1AE7C446B92FE1D6129F2750609A643`；不要混装旧运行目录与新资源。

### 2026-09-24 描述框输入文字颜色修复（用户验收通过）

- 用户反馈候选中描述框输入后仍为黑色、难以辨认。根因在多行 `wxTextCtrl` 的 `SetHint()` 回退实现：占位提示显示时保存了当时的默认黑色前景；输入或获取焦点时恢复该颜色，覆盖随后设定的浅色正文。
- 将描述框背景、长度、字体和正文前景色 `RGB(230,230,233)` 设定在 `SetHint()` 之前；仅调整这一处初始化顺序，避免扩大视觉修改范围。
- HEAD `da3e79dd76` 加现有未提交工作树；Windows x64 Release `OrcaSlicer_app_gui` 增量构建通过，记录 `.tmp/dev/logs/20260924-113119-273/result.json`。定向 `[UiRedesign],[ImageSelection]` 测试通过 7 例／43 断言，日志 `build/ui-redesign-prompt-hint-tests-20260924.log`。
- 按用户最新要求，确认旧候选未运行、原 DLL 与上轮记录哈希一致后，仅用新构建 DLL 覆盖 `.tmp/ui-redesign-visual-20260924/run/OrcaSlicer.dll`；EXE 和资源保持原样。新 DLL SHA256 `B6EBCB793A32CD34DEBA3E6F9365F7F32919DE8F9887D2D70D712B20DBE4511F`。新一轮输入／产物哈希见 `.tmp/ui-redesign-visual-20260924/source-snapshot-prompt-hint.json`，旧 `source-snapshot.json` 保留为覆盖前基线，不能当作当前运行目录的哈希。
- 未启动真实窗口，也未宣称视觉验收通过；用户自行检查提示、首次输入、失焦再聚焦、清空后再输入的颜色。保留上轮 DLL 哈希 `3866FF9E290F6BF4D1473631517EAF913F503A9F4B78734FF068D9AEB416993B` 作为回退身份，旧 DLL 实体未另行备份。
- 2026-09-24 用户反馈：描述框文字颜色测试效果 OK，**文字颜色验收通过**。这一反馈仅覆盖文字颜色，不推断其他页面或交互已验收。

### 2026-09-24 上传缩略图布局与关闭标记（待用户验收）

- 用户截图：选图后缩略图被挤到下方，上方保留空态灰色方块；关闭控件呈矩形且与图片相互遮挡。原因是原先只隐藏加号文字、没有移除 68 DIP 的空态 tile；另用原生矩形按钮在布局后手动移动、叠加在独立的静态位图上。
- 已上传状态隐藏整个空态 tile；缩略图使用固定 150 DIP 居中画布、最大 134 DIP 的圆角图片，并在同一画布内绘制圆形 × 标记。关闭命中区域清空图片，其他图片区域仍可重选；保留拖拽入口和原有校验。空态恢复原 tile。
- HEAD `5bb5f06b11` 加当前两文件未提交修改，Windows x64 Release 增量 `OrcaSlicer_app_gui` 通过，记录 `.tmp/dev/logs/20260924-121927-351/result.json`；定向 `[UiRedesign],[ImageSelection]` 通过 7 例／43 断言，日志 `build/ui-redesign-upload-tests-20260924.log`。
- 用户要求继续使用旧候选路径。覆盖前确认其 EXE 未运行且 DLL 与先前记录匹配，仅将新构建 DLL 覆盖 `.tmp/ui-redesign-visual-20260924/run/OrcaSlicer.dll`；SHA256 `2F06269843DAE780A4FCD59563499232E0C32CABAADBD60CCC29A8FDBF788EC5`，EXE、资源和 Python 未改变。候选未由 Codex 启动，图片居中、角部、关闭/重选/拖入及清空后恢复等待用户真实窗口验收。
