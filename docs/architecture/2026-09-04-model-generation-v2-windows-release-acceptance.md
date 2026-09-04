# 模型生成 v2：Windows Release 与 GUI 验收

日期：2026-09-04。最新状态：完整 Windows Release、AI 运行包及真实 GUI 自动启动通过；二次限色与颜色状态回传两个 P1 已修复，四色/六色手动导入、自动映射、单色、取消和普通导入回归通过。新 1/4/5/6 色清单下载全链路及完整物理通道矩阵仍未全部验收，不等同于正式发布或实物打印通过。

## 范围和基线

- 分支：`codex/model-generation-v2`。
- 应用源码：`6e3c6e658dc964b831f9005f6a97785124d9d9a6`。
- 本轮允许修复本机环境、执行完整 Release 构建和真实 GUI 本地样例验收；不改智能切片、不合并或推送。
- 首轮测试修复涉及 `tools/ai/test_diagnostic_failure_flow.py`；用户手动启动后发现漏包，本次补修 `CMakeLists.txt` 运行清单并新增 `tools/ai/test_packaged_sidecar.py`，不改变应用生成或切片行为。

## 环境恢复

构建工具为 Visual Studio 2022 / MSVC 19.44.35227、Windows SDK 10.0.26100.0、VS 自带 CMake 3.31.6。

旧依赖缓存来自其他工作区，不能直接复用其构建路径。新建 `build/environment-repair/` 隔离修复工程，只把缺失依赖安装到当前 `deps/build/OrcaSlicer_dep/usr/local/`。

| 缺项 | 修复方式 | 结果 |
| --- | --- | --- |
| Python 3.12.13 | 复用 `deps/python3/python3.cmake`，官方源码、固定 SHA-256、x64 Release 与 include-dev staging | 解释器、头文件、导入库齐全；SSL/SQLite/ctypes 隔离导入通过 |
| wxInspector 1.0.0 | 仓库固定源码包及哈希，链接已有 wxWidgets 3.3.2 | 真实静态库与 CMake package 已安装 |
| Assimp 5.4.3 | 新 deps 构建树的 `dep_Assimp` 目标 | 安装成功 |
| FFmpeg 7.0.3 | `dep_FFMPEG` 的固定 Windows x64 包及哈希 | 导入库、DLL、头文件已安装 |
| Pillow 12.2.0 | 仓库固定 CPython 3.12 x64 wheel，校验固定 SHA-256，安装于项目捆绑运行时 | `-I` 隔离模式下版本、包路径及 PNG 原生读写通过 |

没有替换系统 Python、修改系统 PATH、使用 GUI 测试桩或关闭必需构建检查。主 CMake 完整配置和生成已通过。

完整构建使用 `cmake --build build --config Release --target ALL_BUILD -- /m:2 /verbosity:minimal`；仅对子进程设置 `_CL_=/MP4`，控制 32 GB 内存下的编译并行。

### 完整构建结果

- `ALL_BUILD Release` 返回 **0**，包括完整核心库、443 个 GUI 编译单元、主程序、Python bridge / stubgen、profile validator 及 dev-utils；没有绕过 CustomBuild 或关闭必需目标。
- 新主程序与主 DLL 于本机时间 11:06 生成，产物目录 `build/src/Release/`。这不是安装包构建，也不等同于 GUI 已验收。
- 程序目录中的 `python/python.exe -I tools/ai/verify_bundled_runtime.py` 验证通过：Python 3.12.13、Pillow 12.2.0、包路径与原生 PNG roundtrip 正常。
- 仍有 CMake 弃用、C5051、C4805、LNK4098 等警告；本轮没有为消除非阻断警告而修改无关生产代码。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `build/src/Release/orca-slicer.exe` | 271360 | `07706a3d5ef89309d9311d5e73f4a0e08518f3ba2b135e9cf9588081596099ad` |
| `build/src/Release/OrcaSlicer.dll` | 82644992 | `48c576344becf0ecca62db1c05ba2abb197bd3a7b4a14db85ba32fcf2b5091d7` |

## 自动验证

- Python：631 项全部通过，160.144 秒；日志 `build/environment-repair/python-regression-clean.log`。
- 新捆绑 Python 3.12.13 + Pillow 12.2.0：631 项全部通过，216.575 秒；使用 `-I` 并显式加入当前源码目录，日志 `build/environment-repair/python31213-regression.log`。
- 原生契约：136 断言 / 5 用例；presentation：145 断言 / 3 用例；色板快照：35 断言 / 3 用例，全部通过。
- 集成边界：`python scripts/verify_ai_integration.py --json --skip-git` 通过。
- 本地回放预检：1/4/5/6 色全部达到 ready，下载 OBJ SHA-256 与清单绑定一致，清单下载 SHA-256 与状态一致。

### 测试环境隔离修复

诊断失败测试原本只覆盖 legacy OpenAI 参数，继承的 `OPENAI_PRO_*` 会优先选中真实 Image2 服务。本轮新增环境过滤回归，并从测试包复制中排除内部凭据文件。诊断测试改用合成凭据和本机失败地址，不继承用户供应商配置。

按 security-auditor 技能的凭据隔离检查，新增用例验证供应商环境变量不会传入离线诊断子进程；不打印密钥或改变用户的实际供应商设置。

首轮日志显示该缺陷曾导致一次真实预处理服务访问尝试，输入为测试生成的 96×96 小图，未进入 3D 生成；是否计费无法从本地日志确认。修复后定向及全量测试通过。

## GUI 验收设计

原计划使用独立数据目录和 `build/environment-repair/gui-acceptance/generated_models/`，本地回放服务只监听 `127.0.0.1:18769`。该计划只完成 HTTP 预检；后续真实 GUI 续验通过资源管理器正常打开完整开发目录，使用默认用户数据目录、生产 sidecar 和新启动的空白未保存项目，复用现有打印预设。没有覆盖已保存项目、主动保存打印预设或修改供应商配置；导入过程中原生窗口确实向当前临时项目添加了耗材槽。两种验收环境的结果不得混记。

夹具以既有人像模型 `generated_models/4ae4d7e9-f511-4c39-8e93-fd181698eb70/model-vertex-color.obj` 的副本为输入，通过生产 OBJ 限色、图片限色及颜色意图清单生成函数构造。旧四色样例无清单，新 1/4/5/6 色样例带清单。艺术预设允许使用有意义的色板子集，因此配置颜色数不等于每个模型实际用色数。

另有一个 72 面六色试块，六种精确 RGB 均实际使用，专门检查六色导入容量；不把艺术样例的子色板用色结果当作六个实际色区的证明。

| 检查项 | 状态 |
| --- | --- |
| 新 Release 程序启动、窗口可用 | 通过；正常双击启动，自动拉起真实生产 sidecar |
| 1～6 色选择和两个艺术预设 | 控件检查通过；完整 1～6 菜单，实选 1/5/6，默认 4，肖像速写和水墨版画浮雕可选 |
| 文本/图片流程与造型/配色提示 | 输入控件和空输入禁用生成已检查；未提交新预处理/生成，不计端到端通过 |
| 旧四色无清单导入 | 预览通过；默认导入变 2 色，手动指定 4 色后可导入，但状态回传错误 |
| 新 1/4/5/6 色带清单预览及导入 | 待验收 |
| 六个实际色区的本地 OBJ 导入 | 默认被合并为 3 色；手动指定 6 后导入成功，不代表物理通道约束或清单链路通过 |
| 导入只进入准备工作区、无自动切片 | 本轮观察通过；导入停在准备页，切片和 G-code 均等待，未点击切片 |

本地回放检验的是实际 GUI、下载校验、三维预览和导入链路；不证明远端生成质量、新艺术风格肖像相似度或实物打印效果。

### 首轮 GUI 阻塞记录（后续已解除，保留历史）

构建完成后，带独立 `--datadir`、`--no-single-instance` 和 loopback sidecar 配置的启动命令被执行工具拒绝（`blocked by policy`）。按后台启动要求显式加入 `-WindowStyle Hidden` 后仍被拒绝，未继续换通道重试。只读检查确认没有新 OrcaSlicer 进程，独立 app-data 目录也未创建，因此没有 GUI 截图或交互结果。

该阻塞不是 Python/CMake 构建错误。恢复时需要先由用户协助启动允许的测试实例，再执行上表全部 GUI 项；不能用 loopback HTTP 预检代替真实 GUI 验收。测试夹具和所有日志保留在 `build/environment-repair/`，不改日常项目与打印配置。

收尾已核验并停止本轮夹具服务 PID 133484，未终止其他用户进程。后续本地回放验收需重新启动该服务。本轮改动未提交、合并或推送。

## AI 运行包修复与最新交付入口

用户随后手动打开 `build/src/Release/orca-slicer.exe`，11:52 启动日志记录 `python=true, bootstrap=false`。该目录只是普通构建树，resources 联接到源码；之前未执行 AI 安装目录组装，直接把这个 EXE 当作可运行 AI 程序交接不完整。

### 实际修复

- `CMakeLists.txt` 显式运行清单补入新依赖 `color_intent.py`。新测试先准确复现 `ModuleNotFoundError`，补入后通过。
- 新测试检查清单中每个 Python 文件的本地导入闭包，并且只复制实际打包文件到临时目录，在 `-I -B`、清除供应商环境和禁止网络连接条件下导入生产 sidecar；不再用完整源码目录掩盖漏包。
- 保持仓库 `ORCA_AI_WINDOWS_INSTALLER` 默认 OFF，仅在本机构建缓存显式启用 ON；internal 修订号为 `model-v2-runtimefix-dev`。
- 复用完整 Release 构建树，增量 `ALL_BUILD` 和 `cmake --install build --config Release --prefix D:/Workspace/06_3DDY_claude/build/model-generation-v2-app` 均返回 0。未调用集成线正式发布脚本，也未生成或发布安装器。

**本次应打开的程序：** `D:/Workspace/06_3DDY_claude/build/model-generation-v2-app/orca-slicer.exe`。

该目录包括主程序、原生 DLL、资源、捆绑 Python/Pillow、bootstrap、全部 sidecar 模块和构建身份；不包含 `orca_ai_internal_defaults.json`。不要把 EXE 单独拷走，也不要继续用 `build/src/Release/` 的裸构建入口验收 AI 自动启动。

### 最新验证

| 项目 | 结果 |
| --- | --- |
| 打包闭包 / 隔离导入、bootstrap、runtime 和 integration 定向回归 | 60 项通过 |
| 捆绑 Python 3.12.13 下全量回归 | 633 项通过，147.219 秒 |
| 完整安装目录 Python/Pillow/原生 PNG | 通过 |
| 真实安装 bootstrap 启动生产 sidecar v9 | 通过；使用独立数据目录、无供应商凭据 |
| 双向认证握手及健康检查 | 通过；协议 2、health schema 2、正确开发修订号 |
| 无认证健康请求 | 正确拒绝 |
| 本轮真实服务测试生成请求 | 0 |
| 测试子进程正常关闭 | 退出码 0 |
| GUI 自动连接 | 通过；后续正常双击完整开发目录，C++ 自动拉起服务，认证与健康检查成功 |

新目录的 EXE/DLL SHA-256 与上方完整构建产物一致；这里只修复运行包组装，未改 C++ 应用。`color_intent.py` SHA-256 为 `21f0aa1d205b7da5863f50d81f3e9f6968da950687c9f61e4b46972ca1dac5b0`。构建身份仍基于应用 SHA `6e3c6e658dc964b831f9005f6a97785124d9d9a6`，运行清单修复尚未提交，不是正式发布回执。

C++ 自动启动和部分真实 GUI 样例现已补验，详见下节。新 1/4/5/6 色清单完整 GUI 矩阵仍未完成，且已发现阻断整版验收的导入衔接问题。

## 真实 GUI 续验结果（本机时间 12:20 起）

本次按用户“验证这个版本”的请求执行，不继续修改生产代码、不切片、不调用新的预处理、配色推荐或模型生成。

### 已通过

- **真实自动启动：** 应用 PID 274932 于 12:20:50 启动，12:21:06 由应用拉起同目录捆绑 `pythonw.exe`（PID 361892，父进程 274932），12:21:09 监听 `127.0.0.1:18764`，12:21:17 双向认证挑战和健康检查均为 200。启动最初有一次服务未就绪的超时，随后自动恢复；不是需要用户手工启动服务。
- **真实模型预览：** 自动恢复历史模型 707eadc2（998866 面、24 个颜色组）；模型库历史四色 a4d1c1fc（1927448 面、90.3×93.0×100 mm）加载 9.09 秒，4 个渲染颜色组，`gl_error=0`。后者元数据为 schema 4，无颜色意图字段；服务任务查询 404 后，本地模型库仍能正常恢复。
- **控件与职责边界：** 两个艺术预设可选；1～6 色选项完整。没有“导入后自动切片”设置。四色模型导入后进入准备页，网格检查显示封闭、自动摆放完成，切片和 G-code 均保持等待。
- **六色基础容量：** 72 面六试块 OBJ 的原生窗口手动设为 6 并应用后，显示 6 个颜色映射并成功应用到模型体积，日志为 `got 6 clusters` 和 `painting applied to model volumes`。重算后的 RGB 部分分量有 1/255 偏移，因此只证明六个色区容量，不声称精确色板原样保留。

### 验收问题 1：已限色模型在交接时又被自动限色（P1）

复现：模型库四色模型 → “导入到准备页” → 默认“手动匹配打印机耗材” → 原生 Import Model 自动显示 **2 色**；同一通用导入窗口处理六色试块时默认显示 **3 色**。手动分别指定 4/6 并点“应用”能恢复对应颜色数量，但不能要求用户每次重新做已确认的配色决策。

代码依据：`src/libslic3r/Model.cpp:324` 起将顶点色/面色 OBJ 转为 `texture_mesh`；`src/slic3r/GUI/Plater.cpp:9366` 进入新纹理导入窗口；`src/slic3r/GUI/TextureImportDialog.cpp:1870` 默认 `start_computation(true, true)`，`:2400` 将目标色数设为 0（自动选择）。该路径未接收模型生成已确认的色数/色板意图。

验收要求：模型生成交接应保留已经确认的颜色结果；如果需要用户主动重算，应明确提示并由用户选择，不能默认再次减少色数。还须单独验证 1～6 物理通道兼容与槽位约束；原生通用窗口在两次测试导入后把临时项目耗材总数加到 13，六个颜色组不等于六个可用物理通道。

### 验收问题 2：颜色已应用，但流程仍报匹配未完成（P1）

四色模型手动应用并确认后，模型已经有颜色，日志 12:30:22 明确记录上色完成；12:30:25 模型生成适配器却记录 `source_colours=0, mapped_colours=0, applied=false`，侧栏显示“颜色匹配未完成”。

代码依据：`OrcaWorkspaceAdapter.cpp:253–297` 通过旧 `ObjImportColorFn` 回调更新结果，但 `Model.cpp` 的 `objFn` 当前只有形参声明，OBJ 已完全进入新纹理路径；`Plater.cpp:14897–14912` 的真实匹配结果留在 `TextureImportResult`，未回传到适配器。`:319–331` 因此按未上色展示错误状态。自动映射和单色模式也共享旧回调入口，需补真实回归；本次未实际操作这两种模式，不计通过。

验收要求：衔接实际导入结果，区分成功、取消、单色和降级，不能只改文案或把 `colors_applied` 无条件置真。

### 限制和现场状态

- 当前 GUI 验收未完成新 1/4/5/6 色清单的下载—预览—导入全链路；先前 HTTP 预检与自动测试仍单独保留。
- 本次服务日志共 10 个 HTTP 请求：7 个 GET、3 个本地 journey-events POST；没有预处理、推荐或生成 POST，也没有 provider 事件。此结论只适用于本次 GUI 续验；此前诊断测试的真实访问尝试仍按上文如实保留。
- 不验证新艺术预设的实际生成质量、肖像相似度、智能切片或实物打印。现有项目配置还显示擦拭塔部分越界警告，未修改其设置，也未把当前场景当作可打印项目交付。
- 应用和自动拉起的服务保持运行；窗口为未保存测试项目，含历史模型和六色试块，没有覆盖原始模型或已保存工程。
- 收尾再次使用完整目录捆绑 Python 执行 `test_packaged_sidecar.py`：2 项通过（1.388 秒）；`git diff --check` 通过。633 项全量和 316 个原生断言为本报告前述构建/补包阶段结果，本次 GUI 续验未重复整套运行。
- 结论：**AI 运行包修复验收通过；模型生成导入交接尚未通过。** 本轮只记录复现和根因，下一轮获准修复后再补全 GUI 矩阵。

## 导入衔接修复（用户批准后的实现与复验）

本节对应用户“好，帮我把这个修复一下”。上文两个 P1 为修复前的历史结果；本轮只处理模型生成的颜色交接，不改智能切片、配置格式或供应商设置。

### 实现边界

- 恢复 `Model::read_from_file` 的显式 OBJ 颜色回调；普通文件导入和重载不注入专用回调，继续原生纹理窗口。
- 生成模型手动匹配默认按原始离散 RGBA 分组，不再次自动聚类；只有用户主动指定更少颜色时才降色。
- 保色窗口将 OBJ 小数颜色四舍五入到字节，避免截断造成 1/255 偏移；不改变共享 GUI 颜色转换规则。
- 依据真实顶点/面色上色函数返回值填写应用状态和实际颜色数；取消不增加模型，也不显示导入失败。单色模式不弹配色窗口。
- 补充旧匹配窗口错误页空指针保护；Plater 的空模型提前返回仅限显式回调，保持普通配置导入边界。

### 已完成的自动验证

| 项目 | 结果 |
| --- | --- |
| 修复前真实导入红测 | 3 用例 / 19 断言，7 个失败均为回调未调用；普通路径通过 |
| 修复后原生 OBJ 回归 | **8 用例 / 854 断言全部通过**，覆盖 1～6 色、相近色、单色空回调、取消、面色、普通路径和显式降色 |
| 3MF 兼容回归 | **8 用例 / 80 断言全部通过** |
| 捆绑 Python 3.12.13 全量回归 | **634 项通过，198.164 秒** |
| 最终集成守卫复检 | **45 项通过，7.930 秒** |
| AI 集成边界脚本 | 通过，`errors=[]` |

首次全量 Python 命令在 `-I` 隔离模式下未加入源码根目录，导致 23 个 `tools.ai` 测试模块加载失败；修正测试命令后全量通过，未修改这些测试。测试进程清除了供应商环境变量，不继承真实供应商配置。

### 构建与 GUI 现场

完整生产 `ALL_BUILD Release` 和 CMake install 均返回 0，完整开发目录已更新为 `build/model-generation-v2-app`，修订号为 **`model-v2-colorhandoff-dev`**。原生测试单独启用、构建、运行后将本机 `BUILD_TESTS` 恢复原 OFF；没有关闭或跳过生产必需目标。OBJ 测试另以随机顺序、种子 6 重跑，仍为 8 用例 / 854 断言通过。

安装目录 Python 3.12.13 / Pillow 12.2.0 的隔离校验和原生 PNG roundtrip 通过；未打入内部凭据文件。新 DLL 于 14:02:45 生成，安装目录与构建目录 SHA-256 一致：

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `build/model-generation-v2-app/orca-slicer.exe` | 271360 | `07706a3d5ef89309d9311d5e73f4a0e08518f3ba2b135e9cf9588081596099ad` |
| `build/model-generation-v2-app/OrcaSlicer.dll` | 82646016 | `637affc080833555db235eb9dff99e050c4418b79fe9f1c7597627a2893d3691` |

EXE 为启动壳，哈希未变化；本轮原生修复位于新 DLL，不能只凭 EXE 时间判断版本。构建身份仍记录基线 SHA `6e3c6e658dc964b831f9005f6a97785124d9d9a6`，本地修改尚未提交，不是正式发布回执。

旧测试场景正常另存为 `build/environment-repair/gui-acceptance/pre-colorfix-test-scene.3mf`，随后关闭旧程序，应用与自己的生产 sidecar 均正常退出。为复验真实模型生成入口，将明确标记的六色试块四个文件复制到默认本地模型库，逐个预检目标不存在，没有覆盖历史文件；OBJ 和清单 SHA-256 匹配，实际包含六个颜色组。

新版 GUI 通过资源管理器正常启动：应用 PID 413228（14:09:02）自动拉起同目录捆绑 pythonw PID 369900（14:09:18）。服务 14:09:20 开始监听，14:09:31 的认证挑战、健康检查和历史任务查询均为 200；启动初期一次未就绪提示后自动恢复，无需手工启动服务。

| 真实 GUI 检查 | 结果与证据 |
| --- | --- |
| 六色带清单本地历史模型 | schema 6，72 面、6 色；预览、清单恢复和模型生成导入入口通过 |
| 六色手动默认值 | 默认与推荐均为 6，六行映射；未修改色数或点击重算 |
| 六色确认后的真实结果 | 14:13:45：`source_colours=6, mapped_colours=6, applied=true`；侧栏显示“完成·已确认模型颜色与耗材槽” |
| 原问题四色人像 | 历史 schema 4、1927448 面，预览 8.79 秒；默认与推荐均为 4，无需重算；14:21:09 为 `source_colours=4, mapped_colours=4, applied=true`，侧栏正确完成 |
| 自动映射当前耗材 | 不弹手动窗口；14:15:17 为 `source_colours=6, mapped_colours=5, applied=true`；侧栏“完成·已映射耗材颜色” |
| 单色模式 | 不弹配色窗口，几何正常导入；14:12:14 为 `mode=2, source_colours=0, mapped_colours=0, applied=false`，侧栏“完成·单色导入” |
| 取消手动导入 | 返回“已取消导入。”、可重试，准备页仍为空，无新增模型，13 个既有耗材槽未增加 |
| 普通文件导入 | “文件→导入”仍打开原生 `Import Model` 纹理窗口，六色试块仍按普通自动策略显示 3 色；本次仅观察并关闭窗口，没有应用或新增对象 |
| 无自动切片 | 各成功模式均进入准备页；切片与 G-code 保持等待，未点击切片或打印 |

自动模式的 6→5 是用户选择“自动匹配当前耗材”后的最近色槽合并，不是手动保色路径的二次自动聚类。对保存的 3MF 做只读检查：共有 4 个模型（三种模式的六色试块及四色人像）；单色试块没有 `paint_color`，手动六色试块使用 8～13 槽，自动试块使用 1、2、4、5、6 槽，自动映射未使用第 6 槽以后的耗材。此结论只针对当前配置，不替代完整物理通道矩阵。

本轮测试场景于 14:23:20 另存为 `build/environment-repair/gui-acceptance/post-colorfix-test-scene.3mf`（30,290,579 字节），没有覆盖旧场景或原始模型。应用保持运行，标题已无未保存标记。

### 收尾结论与未验边界

- **本轮两个 P1 修复通过。** 不把普通导入改成专用窗口，不改变智能切片或 3MF/profile 格式。
- 当前打印机预设含 13 个既有槽，手动窗口使用现有槽；保留源色数不表示现有耗材 RGB 与源色完全一致。未主动增加槽或修改打印预设。
- 六色 schema 6 的本地历史恢复通过；完整新 1/4/5/6 色清单下载—预览—导入 GUI 矩阵、全部物理通道配置仍待验。原生 1～6 色参数化测试不能代替这些 GUI 与设备验收。
- 本轮 GUI 服务共 16 个 HTTP 请求：10 个 GET、6 个本地 journey-events POST，无预处理、推荐、模型生成请求或 provider 事件；未使用付费服务。
- 取消后模型确实未导入，但生成页的进度条仍停留 98%，临时工程存在快照脏标记；这是剩余显示细节，不计为取消后界面完全复原。
- 验收场景存在模型重叠及既有擦拭塔越界警告，不是可直接打印项目；未测试新艺术风格相似度、实物打印或智能切片。
- `git diff --check` 通过；本轮未提交、合并或推送。
- 收尾时工作树另有本轮未编辑的图片预处理改动：`tools/ai/test_openai_preprocessor.py` 于 14:24、`tools/ai/openai_preprocessor.py` 于 14:26 更新，晚于本轮构建安装与全量测试。安装目录的预处理模块哈希仍为 `70b0574dc49a8c34151770dd1bd72cc98234778419f824bd07ac93253862017b`，与此后源码不同；未覆盖、打包或替它们宣称验收。上述 634 项结果对应本轮测试时的源码，不能作为此后并发改动的验证结果；本轮 C++ 修复文件均未在构建后再变更。

## 证据目录

- Python 构建：`build/environment-repair/python-build.log`。
- wxInspector：`build/environment-repair/wxinspector-build.log`。
- Assimp：`build/environment-repair/assimp-build.log`。
- 完整 Release：`build/environment-repair/release-build-ffmpeg.log`。
- GUI 夹具说明和哈希：`build/environment-repair/gui-acceptance/fixture-index.json`。
- GUI 回放日志：`build/environment-repair/gui-fixture-server.log`。
- AI 完整目录组装：`build/environment-repair/ai-runtime-install.log`。
- 修复后完整回归：`build/environment-repair/python-packaging-regression.log`。
- 真实生产服务启动与认证结果：`build/environment-repair/installed-sidecar-smoke/result.json`；同目录保留独立日志。
- 真实 GUI 截图：`build/environment-repair/gui-acceptance/evidence/`，含艺术预设、1/5/6 色控件、四色默认变二色、六色默认变三色、手动六色及准备页等待切片证据。
- 真实应用日志：`C:/Users/ltj/AppData/Roaming/OrcaSlicer/log/debug_Fri_Sep_04_12_20_50_274932.log.0`。
- 真实服务日志：`C:/Users/ltj/AppData/Roaming/OrcaSlicer/log/orca-ai-sidecar.log`，本次进程 PID 361892。
- 本轮修复红/绿原生测试：`build/environment-repair/color-handoff-red-test.log`、`color-handoff-green-test.log`、`color-handoff-3mf-test.log`。
- 本轮 Python 全量：`build/environment-repair/color-handoff-python-regression-complete.log`。
- 本轮完整 Release：`build/environment-repair/color-handoff-release-build.log`。
- 本轮安装：`build/environment-repair/color-handoff-install.log`。
- 修复后 GUI 截图：`build/environment-repair/gui-acceptance/evidence/color-handoff/01-six-default.png` 至 `10-saved-scene.png`，覆盖六色、取消、单色、自动映射、四色、普通导入及保存后的工程。
- 修复后真实应用日志：`C:/Users/ltj/AppData/Roaming/OrcaSlicer/log/debug_Fri_Sep_04_14_09_02_413228.log.0`；同一 sidecar 日志按 PID 369900 区分本轮。
