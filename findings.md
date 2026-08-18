# 发现与决策

# 2026-08-17 阶段 54：用户截图中的排版问题

- 用户截图显示左侧表单字段之间仍使用过大的统一纵向留白，标签、文本框、风格、图片和颜色分组缺少紧凑层级，导致有限宽度里滚动距离过长。
- “当前耗材：4 种颜色”下方的箭头加空白长条是展开状态的 `wxCollapsiblePane` 标题区域；文字在该状态下没有可靠显示，用户无法判断它控制什么。
- 继续调整 `wxCollapsiblePane` 绘制细节收益低且跨主题不稳定；更稳妥的方案是明确的文字切换按钮，并由普通 `wxPanel::Show/Hide` 控制高级内容。
- 本轮不缩小正文和表单字体，主要压缩 sizer 边距、文本框内边距和分组间距，保留分组标题作为信息层级锚点。


# 2026-08-15 阶段 42：3D 生成页面渐进式交互

- 原页面把模型精度、打印约束、导入颜色和自动切片全部放在首屏，用户在尚未得到图片时就要理解后续参数；同时主按钮位于滚动区末尾，导致“下一步做什么”不稳定。
- 最合适的兼容方案不是拆成多个独立页面，而是在现有单页状态机上增加固定四步旅程、阶段设置前置和固定主操作区；这样保留图片对照、3D 预览、历史模型与页面状态恢复能力。
- 左栏现在由固定旅程区、可滚动设置区和固定操作区组成；主操作不会因展开色板或打印参数而离开视野。
- 模型精度只在等待图片/提示词确认时出现，并从图片预览一致性判断中移除；用户可在确认图片后选择 10/30/50/100 万面，不会把已生成图片误标为过期。
- 打印尺寸、喷嘴、线宽、最小特征和暗部承担色被保留在默认折叠的“高级打印约束”中；专业能力没有删除，但不再干扰首屏。
- 3D 模型就绪后，“导入与切片”会被提升到设置区首位；颜色处理与自动切片不会提前出现。历史模型同样进入第 4 步，不依赖远程任务 ID。
- 正式 GUI 复用 95,338 面、21 色的红熊猫历史 OBJ 验证：第 4 步显示 95%，导入设置位于首屏，底部只保留“导入并自动切片”和“重新开始”。
- 最终交互验收没有创建新的 Image2 或 Tripo 任务；Sidecar 仍为 v4、`https://laotie.dev`，只执行了健康和最新任务只读检查。
- 设计说明位于 `Docs/plans/2026-08-15-model-generation-ux-design.md`；初始和最终 GUI 证据位于 `generated_models/gui-validation-phase42/`。

# 2026-08-13 Windows AI 测试包发现

- 标准 ZIP 可被 `tar` 和 .NET 正确读取，不代表 Windows 资源管理器能快速解压；Orca 的 1.5 万个 profile/resource 小文件会让 `Expand-Archive` 超过 5 分钟。对外用一个仅含内部 ZIP、校验文件、说明和 `tar.exe` 解压脚本的 STORE 外层 ZIP，可将资源管理器第一步解压缩短到约 1 秒。
- 内部测试者更适合使用随包配置文件，而不是交互式脚本和持久用户环境变量；`setup/ai-config.bat` 只暴露两个必填 Key，其余端点和模型用已验证默认值。
- ZIP 内再包含同名根目录会导致用户解压到同名文件夹后出现双层路径；打包应以包目录内容作为 ZIP 根，而不是压入包目录自身。
- `orca-slicer.exe` 只有约 260 KB，不能脱离同目录 `OrcaSlicer.dll`、第三方 DLL 和约 220 MB resources 单独运行；对外测试必须发完整运行目录。
- 正式 AI sidecar 的额外 Python 依赖只有 Pillow；HTTP 客户端使用标准库，不需要 `requests/httpx`。Python 代码使用 `X | None` 类型语法，因此测试包要求 Python 3.10+。
- 外部配置包括 `OPENAI_BASE_URL/API_KEY/TEXT_MODEL/IMAGE_MODEL` 与 `TRIPO_API_KEY/API_BASE/MODEL`；只有两个 Key 必须由测试者安全取得，其余均可使用启动器默认值。
- API Key 不应嵌入 ZIP 或批处理。当前配置脚本写入当前 Windows 用户环境变量，便于原有启动器刷新配置；这适合内部测试，不应作为正式凭据存储方案。
- Windows PowerShell 5 对无 BOM 的中文脚本存在代码页兼容风险；测试包的 `.ps1/.bat` 采用 ASCII 提示和 CRLF，中文说明单独存放在 README。
- Windows `Compress-Archive` 对大量资源文件可能遇到读取占用；系统 `bsdtar` 创建 ZIP 在本次 15,100 文件、325.1 MB 未压缩内容上稳定完成。

## 2026-08-11 阶段 26 初步现象
- 用户截图显示真实 Q 版人物 OBJ 已成功下载并在生成页以自然色渲染：14,480 个三角面、15 种颜色、41.3 x 41.1 x 100.0 mm。
- 失败发生在用户确认生成 G-code 后的确定性修复阶段，而不是 AI 生成、下载、OBJ 解析或 3D 预览阶段；界面错误为 `Repair failed: mesh still open after hole filling.`。
- 本阶段采用本地保色闭合修复，不重新调用付费 Tripo，也不通过关闭流体门禁强行切片。
- 对应真实任务为 `4ae4d7e9-f511-4c39-8e93-fd181698eb70`，Tripo generation `950512d3-c3d2-4f73-b05e-765b4327877b`、conversion `f4c7239c-3767-4d9f-8a24-9b51400f2b3a`；完整原始包、规范 OBJ 和下载副本均已保留。
- `mesh-repair.json` 显示模型单连通、7,233 顶点、14,480 面，没有脱离件；但有 23 条边界边和 3 条非流形边。sidecar 返回 `topology_status=deferred`，没有删面或补洞，剩余异常边 26 条。
- 当前 sidecar 仅能在非流形缺陷范围足够局部、删面后边界可分解成小型有向闭环时修复；否则安全退出。CGAL 后续执行 polygon soup 清理、边界缝合、非流形顶点复制和逐环补洞，仍未得到闭合网格。
- 离线诊断确认 3 条非流形边只牵连 8 个面，远低于 144 面安全上限；失败原因是代码将多个独立缺陷合并计算包围盒，得到 23.75 mm / 115.73 mm = 20.5%，错误超过 5% 局部范围阈值。
- 删除这 8 个面后的网格已无非流形边，形成 30 条边界边、7 个局部边界组件；最大组件直径约 1.79 mm，各组件顶点度满足现有 Euler 环拆分条件。正确修复应按共享顶点的缺陷簇分别校验 5% 范围，而不是放宽阈值。
- 采用按缺陷簇校验后，仍保留总删面上限、单簇空间范围、单环 64 边、最终每边恰好两面、单连通和顶点色门禁；原始 attempt 产物保持不变，只生成可回滚的修复副本。
- 新算法在真实副本上识别出 2 个非流形缺陷簇，最大对角线比例 1.33%；删除 8 个关联面、1 个未引用顶点，拆分并补齐 9 个局部边界环，增加 9 个顶点和 30 个面。
- 修复结果为 7,241 顶点、14,502 面、1 个连通体、0 条异常边；模型最小/最大坐标完全不变，29 个原始顶点色字符串集合完全保留。界面显示的 15 色是预览颜色分组数，与原始色值数量口径不同。
- 新 sidecar 代码重启后，当前 Orca 仍保留旧 job ID，但 sidecar 不恢复内存 job，确认按钮返回 `Model job not found`；现有 `download_and_import()` 即使 `m_artifact_path` 已指向有效下载文件也会无条件再次请求 sidecar，并在失败时清空本地路径状态。
- 本地恢复应成为正式能力：刚下载的 OBJ 和模型库历史 OBJ 都可在用户明确点击“确认并生成 G-code”后直接进入现有导入、颜色映射、摆盘和切片回调；双击历史卡片本身仍只加载预览，不改变阶段 25 的安全交互约束。
- 本地恢复已落地：确认导入时优先使用当前预览路径或 `generated_models/downloads` 的非空 OBJ，仅在本地文件缺失时请求 sidecar；job ID 为空的模型库条目也可显式确认导入。
- 模型库载入现在恢复 OBJ 路径、`vertex_colors` 编码、自然色/可打印色模式和 ready 状态。自然色历史模型保持单耗材打印语义，不会被当前页面默认开启的可打印色复选框误转成 MMU 模型。
- 正式 GUI 从历史库载入任务 `4ae4d7e9...` 后显示 14,502 面、15 个预览颜色组和 41.3 x 41.1 x 100.0 mm；界面明确显示“确认并生成 G-code”，证明 sidecar 内存任务不再是恢复前提。
- 确认后日志直接记录单个本地 OBJ 的 `load_files`，没有进入 CGAL 修复分支；随后出现 `Exporting G-code finished` 与 `on_process_completed:finished`，没有 `Model job not found`、`Repair failed` 或 `mesh still open`。
- 修复后的规范 OBJ 与 downloads 副本均为 624,709 字节，SHA256 `62F6B9B38420D6AE07621E108F9D03D2F9B267ACDEDC4E3B9F9D5DE1943FD0AB`；原始文件仍保存在 `phase26-backups`，没有新增 Tripo 任务。
- 最新 Windows Release 构建与日常运行 DLL SHA256 均为 `99709019EA4A0005920517772D4F071269529CECB063A675BC72CA2E35D73388`；正式 sidecar 继续监听 `127.0.0.1:18764`。

## 2026-08-11 阶段 25 初步根因
- 最新正式日志中生成页 3D 预览成功解析出 10 个颜色组，说明 OBJ 和 `load_obj` 阶段仍有颜色；同次 render 记录 `gl_error=1282`，需要继续定位共享 GL 状态或 shader uniform 是否导致显示颜色异常。
- 成功导入并进入 G-code 后，`download_and_import()` 明确清空 `m_job_palette`、将 `m_model_preview_ready=false` 并调用 `m_model_preview->clear()`；这直接解释了去其他页面再回 3D 生成页时预览消失。
- 当前 `GeneratedModelEntry` 只有标题、详情和内存位图，没有 OBJ 路径、色板、颜色模式、来源、风格、时间或稳定 ID，无法双击恢复模型。
- `m_library_entries` 只存在于 `ModelGenerationPanel` 生命周期中，最多 12 条；软件重启后为空，也不会扫描 `generated_models`，因此不是用户要求的历史模型库。
- 目标应以项目 `generated_models` 为真值：新增持久元数据索引，同时对旧历史 OBJ 执行兼容扫描；条目双击时用保存色板或 OBJ 顶点色重新构建 `ModelPreview3D`。
- 最终确认生成页颜色文件和解析均正常；渲染前清理共享 OpenGL 历史错误并恢复 framebuffer、viewport 和颜色/深度状态后，正式模型渲染为 10 个颜色组且 `gl_error=0`。
- Orca OBJ 顶点色导入还存在独立逻辑错误：旧判断会漏掉 `{1,1,2}` 这类含非基础耗材的三角面；现改为仅当三个顶点都等于 `first_extruder_id` 时跳过，并由 Catch2 用例 5/5 断言覆盖。
- 显示状态已与远程任务状态分离。导入完成和页面切换不再清空 `m_displayed_model_path`、显示色板或 OpenGL 网格；`3D 生成 -> 准备 -> 3D 生成` 往返后仍显示 15,616 面、41.7 x 42.1 x 100.0 mm、10 色。
- 模型库改为第三个同级标签页，启动时扫描 `generated_models`，规范产物优先、downloads 兜底并按 job ID 去重；现有项目得到 15 个唯一历史模型，其中 5 个规范 OBJ、10 个下载兜底。
- 双击历史卡片会延迟解析 OBJ 并载入当前 3D 预览，不自动导入、摆盘或切片；最新真实模型双击加载成功并恢复 10 色。
- 高 DPI 复核发现单行统计会挤出颜色尾字和重置按钮，最终改为“面数 + 颜色数 / 尺寸”两行稳定布局。
- 最新 Release DLL SHA256 为 `9539B7A1E718AF6846990C354E906B72222D55516ACF863406594249C8F71499`；日常运行目录仍被 PID 58140 使用旧 DLL，待用户正常关闭后再同步，不强制终止未保存窗口。
- 用户正常关闭 PID 58140 后，最新 DLL 已同步到 `build/OrcaSlicer/OrcaSlicer.dll`，构建与运行副本 SHA256 均为 `9539B7A1E718AF6846990C354E906B72222D55516ACF863406594249C8F71499`；旧 DLL 保存在同目录 `OrcaSlicer.dll.phase24.bak`。
- 正式启动脚本已重新启动 Orca PID 47888；窗口响应正常、路径为项目 `build/OrcaSlicer/orca-slicer.exe`，且正式 AI sidecar 健康检查通过。

## 2026-08-11 阶段 24 正式链路与准备页性能
- 截图中的 `Could not connect to the preprocessing service.` 不是预览控件问题：机器级 `OPENAI_BASE_URL` 已为 `https://laotie.dev`，但旧启动父进程仍向 sidecar 传入 `https://104.194.93.250/v1`。
- 正式启动链现在从 Windows Machine/User 环境刷新 provider 配置，健康接口仅公开非敏感 base URL；启动器发现运行实例配置过期时，只会停止命令行已验证为正式 sidecar 的 loopback 进程。
- 正式 `18764` 已确认使用 `https://laotie.dev`，model generation capability 为 available，`18765` 未监听；日常启动器 `start_orcaslicer_with_ai.bat --check` 通过。
- 使用失败任务的同一张输入图完成一次真实付费图片预处理，任务 `6243d4d2-678b-42b2-85e7-e87a1c733ef1` 成功到达 `awaiting_confirmation`，生成 1,770,828 字节 Q 版预览；未调用 Tripo。
- 真实预览保存在 `generated_models/paid-image-validation-phase24/style-preview-6243d4d2-678b-42b2-85e7-e87a1c733ef1.png`，原图和 sidecar 任务文件均保存在项目 `generated_models` 下。
- “准备”页首开卡顿来自 OpenGL/`GLCanvas3D` 延迟初始化：旧日志在用户点击后约 6 秒才加载 GL；根因是 Windows 首次 native surface 未映射时只记录 postpone，却没有像 Linux 一样安排重试。
- 修复后隔离正式实例在首页期间于 13:14:05.635 开始初始化 OpenGL、13:14:06.246 完成 `GLCanvas3D` 初始化；13:16:56 点击“准备”时画布在 0.5 秒观察窗口内出现，且没有第二次 GL 初始化。
- Windows Release `OrcaSlicer` target 成功；构建与运行 DLL SHA256 均为 `DA752B07097A4A86CFAE6A6F4F08DFC848A9D18590567B6277645460435D4E`。AI Python 回归 67/67 通过，`py_compile` 和 `git diff --check` 通过。

## 2026-08-11 阶段 20 视觉复核补充
- `q-cartoon-palette-v4.png` 已保持自然肤色、黑发、白衣和单色背景，未使用的高饱和色集中到底座连续分区，当前可作为合格基线。
- `cyberpunk-palette.png` 不合格：全局暖色保护把盔甲中的米白、棕色和部分结构色错误限制为肤色候选，导致人物主体丢失红、绿、白等关键护甲色；末端“补齐全部色板颜色”只把缺失色放到底座，无法弥补主体语义。
- 修复方向：肤色约束必须是空间/连通区域约束，仅保护上半部候选面部的主要暖色连通区域；盔甲等其他暖色簇继续按 Lab 最近色映射。保持边缘连通单色背景和底座补色策略不变。
- 第三轮离线复核确认另一根因：旧算法在识别背景之前执行自适应聚类，赛博朋克原图的灰色渐变背景占用了绝大多数色簇，主体红绿甲片没有进入候选。先将边缘连通背景统一后再以 48 色聚类，主体的红、绿、白、黑结构色均已恢复。
- `cyberpunk-palette-v3.png` 的主体配色已明显改善，但面部仍有少量灰色高光；`q-cartoon-palette-v6.png` 的脸部、黑发和白衣均正常。后续仅扩展已识别面部区域，不改变已验证的主体聚类顺序。
- 最终定位面部灰斑为候选色分类错误：`#B8B3A7` 的轻微暖偏被通用暖色检测误认为打印肤色。拆分原图肤色检测与更严格的耗材肤色筛选后，`cyberpunk-palette-v7.png` 的脸部恢复自然肤色，同时保留黑、白、绿、红、棕护甲分区；16 色全覆盖且无色板外颜色。
- 三风格最终视觉基线为 `q-cartoon-palette-v9.png`、`cyberpunk-palette-v9.png` 和 `classical-palette-v3.png`。量化函数现在按 style 限制头部肤色保护范围：Q 版为主体高度 39%，正常比例风格为 17%，避免把肩部护甲或暖白西装映射成肤色。
- 古典风格真实 `gpt-image-2` 编辑通过 `https://laotie.dev/v1/images/edits` 成功 1 次，原图 `classical-raw.png` 保留人物身份、交叉手姿势、完整构图和一体底座；严格色板版恢复白色套装、绿色内搭和自然肤色。
- 本轮截至三风格图片验收：真实图片调用共 3 次成功（Q 版、赛博朋克、古典），真实 3D 调用 0 次；3D 预算仍为 20/20。
- 阶段 20 第 1 次真实 Tripo 低模验收通过：generation `4c907118-0e3e-45cb-8d18-b21f5c1dafef`，OBJ conversion `a17a0d56-4154-410a-96ae-09fc5a8f23ae`。最终 OBJ 为 7,574 顶点、15,148 三角面、1 个连通体、0 边界边、0 非流形边、0 退化三角，色板外颜色 0；未触发第 2 次调用，预算剩余 19/20。
- 通过产物位于 `generated_models/paid-tripo-validation-phase20-q/4c907118-0e3e-45cb-8d18-b21f5c1dafef/model-vertex-color.obj`，大小 654,423 字节，SHA256 `FE6D5EEAB43511062209D68C82F2595D088219117E172CBA19F0DE53CA08E5C2`。
- 最新 Release 在 200% DPI 下的 `3D Generate` 首屏无裁切或重叠：四步流程、统一文字/图片输入、风格选择和色板区均完整可见；当前空项目从 `filament_colour` 动态读到 8 色并显示 `8 colors synced from the current project`。

## 需求
- 用户的总体目标是在 OrcaSlicer 上构建四类 AI 能力：
  1. 模型生成：以 Tripo 为当前示例，从文字和图片生成可作为切片输入的 3D 模型。
  2. 模型可打印性检查与自动修复。
  3. 切片参数 AI 化。
  4. 简化的端到端 AI 交互：用户只描述目标或上传图片，系统自动完成生成、检查、修复、参数配置和切片，中间最多提出少量必要问题。
- 架构必须 provider-agnostic；具体模型、LLM、生成服务和修复服务应可替换。

## 路线图初步判断
- 推荐先实现受控的固定工作流：`需求澄清 → 模型生成/导入 → 可打印性检查 → 必要修复 → 试切 → 参数选择/比较 → 最终切片 → 结果摘要`。
- 第一版不应做“LLM 自由调用任意能力”的开放 Agent。LLM 只负责理解意图、提出少量澄清问题、选择候选方案和解释结果；几何检查、修复、配置验证、切片、指标计算和导入必须由 OrcaSlicer 的确定性代码执行。
- 当前四项能力成熟度的初步排序：模型生成已有可运行纵向切片；切片参数 AI 已有受控建议/应用入口但不是闭环优化；检查/修复需要统一服务化现有内核；端到端编排尚未实现，只具备部分组件。
- 端到端 MVP 应优先采用显式状态机和 typed tools，每一步有输入/输出 schema、前置条件、结果证据、可重试策略和审批级别；后续再允许模型在白名单工具上动态规划。

- 四项能力应遵循“独立服务先验收、编排层后组合”的依赖方向：模型生成、模型检查/修复、切片试验/比较都应能被普通 GUI 单独调用；端到端 AI 交互只编排这些已验证能力，不承载核心算法。
- 推荐的核心抽象不是“一个万能 AI Assistant”，而是三层：OrcaSlicer 确定性能力服务；provider-agnostic sidecar/provider adapters；持久化工作流编排与用户交互层。

## 分期路线图骨架
1. **基础治理与现有生成收尾**：恢复 Git；修复页面回归；增加 feature gate；统一 `/health`/版本/能力协商；让 sidecar 可跨平台安装；建立生产/mock 契约测试；完成真实 Tripo 文生/图生 smoke。
2. **模型检查 MVP**：把现有确定性几何/摆放/切片前检查收敛成统一 `PrintabilityReport`，先只检查和解释，不自动改模型。
3. **安全修复 MVP**：按“安全自动修复 / 需用户确认 / 不可自动修复”分级；始终保留原模型和可撤销副本；修复后重新检查并比较差异。
4. **切片参数闭环**：从当前单次参数建议升级为有约束的候选配置、隔离试切、结构化指标比较、项目级应用与撤销；LLM 负责生成候选/解释，OrcaSlicer 负责配置校验和切片评估。
5. **低交互端到端 MVP**：固定状态机串联输入澄清、生成/导入、检查、修复、试切、参数选择和最终切片；只在缺失关键约束、产生外部费用、执行高风险修复或最终输出前询问用户。
6. **受控 Agent 化**：在固定工作流稳定后，开放白名单 typed tools 的动态规划；所有调用仍经过前置条件、权限、预算、审计和恢复机制。

## 建议的跨能力数据契约
- `IntentSpec`：输入类型、用途、目标尺寸、打印机/材料/喷嘴、质量/速度/强度偏好和允许的用户问题数。
- `GeneratedArtifact`：来源、provider、任务 ID、格式、hash、单位、尺寸、临时位置和生成参数。
- `PrintabilityReport`：检查版本、几何指标、问题代码/严重度/区域、是否阻塞、可修复性和建议动作。
- `RepairPlan` / `RepairResult`：动作、风险、原模型快照、修复副本、前后报告和几何变化摘要。
- `SliceCandidate`：配置快照、切片结果、时间/耗材/支撑/层数/警告等指标、硬约束和评分。
- `WorkflowState`：状态、步骤、产物、尝试次数、预算、待确认事项、错误和恢复点。

## 建议的成熟度定义
- **L0 规划**：只有文档或 UI 占位，没有可调用能力。
- **L1 建议器**：AI 给出文本/结构化建议，用户手动执行。
- **L2 受控执行器**：AI 可调用白名单动作，但每项输入经过 schema 与领域校验，关键动作需确认。
- **L3 可度量闭环**：系统执行候选方案，以确定性指标评估并选择，支持快照、撤销和复现。
- **L4 低交互编排**：用户只给目标/图片，工作流自动串联 L2/L3 能力，只在关键缺口和风险点询问。
- **L5 自适应 Agent**：在预算、权限、状态机和审计约束内动态规划；不是当前 MVP 目标。

## 各阶段最低验收原则
- 每个能力先有普通 GUI/API 可独立调用，再接入编排。
- 每个外部付费调用都有费用确认、幂等键、任务 ID、取消语义和重试边界。
- 每个模型/配置变更都有原始快照、差异摘要、撤销路径和确定性复验。
- 每个自动决策都保存输入、候选、指标、选择理由和工具版本，保证可复现。
- 功能关闭或 sidecar 未配置时，不改变现有 OrcaSlicer 默认行为。
- Windows、macOS、Linux 使用同一契约和验收集；provider 适配器通过契约测试而不是 GUI 特判。

## 与既有架构文档的关系
- `Docs/architecture/03-ai-target-architecture.md` 已覆盖用户本次提出的四类能力，目标模块包括 `AIProviderGateway`、`GeneratedModelImporter`、`ModelPreflightService`、`ModelRepairWorkflow`、`SlicingContextBuilder`、`SliceTuningOrchestrator`、`AIWorkflowCoordinator`、`AIServiceManager` 和 `AIJobStore`。
- 当前实际实现顺序与文档建议不同：模型生成纵向切片已经先落地，而平台服务、统一状态机、检查/修复、隔离试切仍未落地。因此不应重做生成，而应先补平台骨架，再把当前 `ModelGenerationPanel`/sidecar 流程迁入统一任务模型。
- 既有文档的 `AIWorkspacePanel` 偏向多功能工作台；用户新提出的低交互目标需要在其上增加“Guided/Auto Workflow”模式。建议保留两种入口：专家模式可逐项操作 Generate/Inspect/Repair/Tune；简化模式只显示问题、进度、必要提问和最终结果。
- 文档中的状态机适合单个 Job，但端到端工作流还需要步骤级状态：每步产物、审批、重试、回滚点、预算和依赖。建议区分 `AIJob`（单个外部/计算任务）与 `AIWorkflowRun`（跨步骤编排）。

## 模型可打印性检查与修复现状
- 已有确定性检查基础：`TriangleMeshStats` 保存面数、包围盒、体积、壳数和开放边（`src/libslic3r/TriangleMesh.hpp:47-85`）；CGAL 有自交/闭合/体积检查（`src/libslic3r/MeshBoolean.hpp:64-75`）；`BuildVolume::object_state()` 判断越界/碰撞/低于床面（`src/libslic3r/BuildVolume.cpp:377-408`）；`Print::validate()` 与 `Plater::validate_current_plate()` 提供正式切片前阻塞检查（`src/libslic3r/Print.cpp:1262-1404`、`src/slic3r/GUI/Plater.cpp:17796-17885`）。
- 已有确定性修复基础：STL 导入时 admesh 会接边、删除孤立面、统一法线和翻转负体积，但不补洞（`src/libslic3r/TriangleMesh.cpp:79-178`）；手动 CGAL 修复包含 polygon soup 清理、退化面/孤立点删除、非流形顶点拆分、自并集、补洞和方向修正（`src/libslic3r/MeshBoolean.cpp:478-556`）。
- 已有正确回写链：`FixModelByCgal` 分壳并清理零体积薄片，经 `ModelVolume::set_mesh()` 写回、重建凸包和失效缓存，GUI 后续落床并刷新（`src/slic3r/Utils/FixModelByCgal.cpp:115-181`、`src/slic3r/GUI/GUI_ObjectList.cpp:6117-6164`）。
- 当前缺口：检查结果分散且没有统一 issue/severity/evidence 模型；自交、零体积和壳数没有统一进入 preflight；薄壁、小特征和悬垂没有稳定的模型级诊断；CGAL 修复缺少完整修改统计；缺少修复前后差异、风险分级、自动工作流、Undo 验收和端到端测试。
- 结论：检查/修复不是“从零开发算法”，而是先建立 `ModelPreflightService` 聚合现有事实，再建立 `ModelRepairWorkflow` 包装现有 repair adapter 和标准回写/撤销流程。

## 四项能力成熟度矩阵
| 能力 | 当前成熟度 | 已有基础 | 主要缺口 | 下一目标 |
|---|---|---|---|---|
| 文/图生 3D | L2 受控执行器（未产品化验收） | Tripo 流程、GPT 预处理、轮询、3MF/STL 下载、确认后导入 | UI 回归、真实双 smoke、产物深检、持久化/恢复、feature gate、三平台打包、provider registry | 稳定可发布的独立生成能力 |
| 可打印性检查/修复 | 底层算法成熟；统一 AI 能力约 L0-L1 | mesh stats、CGAL/admesh、越界、`Print::validate()`、标准 mesh 回写 | 统一 issue/report、证据定位、薄壁/悬垂模型、修复分级、before/after、Undo/E2E | 先只读 Preflight，再安全 Repair workflow |
| 切片参数 AI 化 | L2 受控执行器 | 小范围上下文、结构化 proposal、key/type/range 校验、用户选择、preset 应用和重切片 | 完整诊断/设备材料上下文、scope、baseline 指标、候选、隔离试切、评分比较、快照/撤销 | L3 可度量闭环优化 |
| 低交互端到端流程 | L0 设计 | 生成页、AI Assistant、目标架构文档 | 统一工具、协调器、状态持久化、审批策略、步骤恢复、问题预算、统一 UI | L4 固定状态机 Guided Workflow |

## 推荐目标架构
```text
AIWorkspacePanel
├─ Expert mode: Generate / Inspect / Repair / Tune
└─ Guided mode: Intent → Workflow progress → Questions → Review
                    │
            AIWorkflowCoordinator
            ├─ ApprovalPolicy / BudgetPolicy
            ├─ AIWorkflowRunStore
            └─ typed domain tools
                    │
    ┌───────────────┼──────────────────┐
GenerationService  ModelPreflight/Repair  SliceTuningOrchestrator
    │                  │                    │
GeneratedModelImporter │             TrialSliceJob / Comparison
    └────────────── Orca application facade ──────────────┘
                     │
       Model / Mesh / Config / Print / Preview

AI domain services → AIProviderGateway → provider adapters / sidecar
```
- **OrcaSlicer C++ 应用层**拥有工作流状态、项目修改、Undo/dirty、正式导入、检查/修复和切片真值。
- **sidecar**负责 provider registry、LLM 意图/候选生成和外部模型生成服务适配；它只能返回结构化计划/候选/产物，不能直接修改项目。
- **libslic3r**继续保持 AI/provider 无关，只暴露确定性几何、配置和切片能力。
- 第一版保留一个真实 Tripo adapter 和一个 mock adapter 即可；通过 capability/contract 验证 provider 无关，不需要同时接入多个真实供应商。

## Guided Workflow 建议状态机
```text
Intake
→ Clarifying?                      # 只收集阻塞执行的缺失信息
→ Generating / Importing
→ Preflight
→ RepairPlanning
→ RepairReview? → Repairing → Recheck
→ BaselineTrialSlice
→ ProposingCandidates
→ TrialSlicing → Comparing
→ FinalReview?
→ Applying → OfficialSlicing → Completed

任意活动态 → Pausing / Canceling / Failed
持久化步骤 → Resume / Retry / Rollback
```
- 推荐默认交互预算：一次集中澄清（尺寸/用途/打印机材料/质量速度强度偏好），一次付费或高风险动作授权，一次最终结果确认；已有当前 printer/material 时不重复询问。
- 可自动执行：只读检查、低风险且可撤销的变换、候选试切、指标比较。
- 必须确认：外部付费生成/用户数据上传（可会话级预授权）、中高风险几何修复、覆盖正式模型/配置、最终导出或打印。
- 高风险且无法证明意图保持的修复（例如自动加厚关键结构、删除不确定壳体）不自动执行，只解释并提供选项。

## 分期路线图与验收
### M0：工程基线与平台补洞
- 恢复可靠 Git；修复默认页抢占和导航图标；feature gate；本地化目录；统一 `/health`、协议版本和 capabilities；`AIServiceManager`/最小 `AIJobStore`；mock/production 契约一致；sidecar 三平台打包。
- **验收**：AI 关闭时现有行为不变；离线可正常切片；三平台可启动/关闭；崩溃后有明确恢复/清理；无凭据写入日志或 3MF。

### M1：模型生成产品化
- 复用当前生成链，抽出 `AIModelGenerationService` 和 `GeneratedModelImporter`；强化 3MF/STL、hash、单位、尺寸和 multipart 校验；完成真实 Tripo 文生/图生 smoke。
- **验收**：生成模型可导入、Undo、保存 3MF、重开、切片；失败/取消/退出无悬挂任务和临时文件；收费提交不因重试重复创建。

### M2：只读 Printability Preflight
- 定义稳定 `PrintabilityIssue/Report` schema；聚合 mesh stats、自交、build volume、`Print::validate()`；增加问题定位和严重度；先不自动修复。
- **验收**：固定坏网格语料有稳定报告；检查不修改项目；报告可版本化、可缓存、可在 GUI 定位；单测覆盖 issue 分类。

### M3：安全自动修复
- adapter 化 admesh/CGAL/摆放修复；生成 `RepairPlan`；按风险分级；在模型副本执行；输出 before/after；接受时统一 `set_mesh`、Undo、dirty、cache invalidation；随后复检。
- **验收**：拒绝/取消零修改；接受后可 Undo；保存重开一致；修复后 report 改善且不引入新 blocker；端到端测试覆盖开放边、反法线、退化面、补洞和回写。

### M4：切片参数 L3 闭环
- 扩充 `SlicingContextBuilder`；构造 2-3 个有边界的候选；创建隔离 Model/Print snapshot 试切；从 `GCodeProcessorResult` 采集时间、耗材、支撑、换料/冲刷、警告和质量代理；硬约束过滤后比较；用户接受才正式 apply。
- **验收**：候选不污染正式项目；同输入可复现；无效配置无法执行；有 baseline/candidate 差异和指标证据；应用后可撤销并正式重切片。
- 第一版只宣称优化“时间/耗材/支撑及已定义风险代理”，不要宣称能预测真实成品质量；真实打印反馈闭环后置。

### M5：低交互 Guided Workflow MVP
- 用固定状态机组合 M1-M4；IntentSpec 集中澄清；统一进度、问题、审批、取消、重试、恢复和最终摘要；专家模式仍可逐步干预。
- **验收**：文字或图片输入能在少量问题内走到可切片结果；每一步有证据与恢复点；中途关闭后可继续；费用和高风险动作不会静默执行。

### M6：受控 Agent 化
- 将已验收能力暴露为 typed tools；LLM 可在白名单、预算、权限和状态机约束内动态选择步骤；引入工具版本、审计和 eval 集。
- **验收**：自由规划不能绕过 schema、审批、项目快照或预算；失败可回放；同一目标有离线回归评估。

## 最短实施路径
1. 先完成 M0 中的 Git、两个 UI 回归、feature gate、协议/capability 和 mock 契约。
2. 立即封闭当前生成链的真实 Tripo 双 smoke，作为 M1 基线。
3. 下一段核心开发从 `PrintabilityIssue/Report` + `ModelPreflightService` 开始；不要先写 Agent UI。
4. Preflight 稳定后接现有 CGAL/admesh 为 Repair adapters。
5. 同时仅设计切片候选/指标 schema，等 Preflight issue 成为上下文后实现隔离试切。
6. M1-M4 均有独立验收后，再做 Guided Workflow；Agent 动态规划最后开放。

## 研究发现
- 项目根目录原先没有 `task_plan.md`、`progress.md` 或 `findings.md`。
- `session-catchup.py` 执行成功但没有输出，未发现可恢复的未同步会话。
- 当前构建配置证据：`build/CMakeCache.txt` 使用 `Visual Studio 17 2022` 多配置生成器，`CMAKE_BUILD_TYPE=Release`，安装前缀为 `build/OrcaSlicer`。
- 当前 `build/` 中存在多级 `CTestTestfile.cmake`（说明测试被配置），但没有找到 `Testing/Temporary/LastTest*.log`（没有可核验的 CTest 实际执行记录）。
- 当前 `build/` 未找到 `OrcaSlicer.exe`；因此可依赖历史会话确认上一轮完整 Release 构建/安装/运行成功，但不能声称当前磁盘仍保留可执行产物。
- 项目根目录不是 Git 仓库，且顶层没有 `.git`，因此暂时无法直接获取分支、提交或工作区 diff。
- 根目录时间戳显示近期工作与 AI/Tripo 集成有关：`start_orcaslicer_with_agnes.bat`、`start_orcaslicer_with_ai.bat`、`real-tripo-text.3mf` 以及多张 Tripo UI 验证截图。
- 找到嵌套 Git 仓库 `.claude/upstream-orcaslicer`，最初作为上游对比候选，后因版本晚于当前副本而排除为原始基线。
- 该基线记录为 `main` / `origin/main`，HEAD 为 `a62fb17e03d159d5b562cc6d64163346e454b5de`（2026-07-25 22:31:40 +0800，`Remove cloud deletion of owned plugins from the plugin dialog (#14946)`）。
- 基线仓库自身工作树已被清空，因此普通 `git status` 显示全量删除；尝试组合其对象库与当前根目录时，索引/工作树配置仍将同一批文件同时识别为删除和未跟踪，该全树结果不可采信。
- 版本文件进一步证明该仓库不是当前副本的原始基线：当前根目录为 `SLIC3R_VERSION 02.06.00.51`，嵌套仓库 HEAD 为 `02.08.01.55`。
- 当前源码最后一轮修改集中于 2026-07-28：`ModelGenerationPanel`、`AIAssistantPanel`、`MainFrame`、`GUI_App`、`Plater`、GUI CMake、图标与本地化清单；此前 2026-07-25 至 2026-07-27 还修改了 AI sidecar、Tripo 客户端、OpenAI 预处理和架构文档。
- 已从其他会话记录恢复用户原始目标：使用 Tripo 补齐文生 3D 和图生 3D。
- 历史会话末尾证实：导航和本地化调整后编译通过，并完成了 Release/安装及 mock 驱动的真实运行验证；顶部顺序为 `3D Generate → Prepare → Preview → Device → Project`，独立模型生成页面已成功显示。
- 运行验证发现两个待收尾问题：`3D Generate` 新页启动时意外抢占默认页；顶部立方体图标在青色选中态对比度不足。
- 当时决定在 `InsertPage` 后显式隐藏新页，并新增“立方体+闪光”的 active/inactive SVG。随后因上下文超限中断，因此必须以当前文件确认这两个修复是否落盘并重新验证。
- 当前截图 `model-generation-main-page.png` 证实独立页面整体布局已经运行：`3D Generate` 位于“准备”之前，左侧为输入/GPT 预处理，右侧为预览结果；截图也清楚显示选中态立方体图标为灰色、对比度不足。
- `resources/images/` 当前只找到 `tab_generate_3d_active.svg`，未找到配对的 inactive 资源，说明专用导航图标修复至少没有完整落盘。
- 当前 `MainFrame.cpp:1327-1332` 创建并插入 `ModelGenerationPanel` 后未调用 `Hide()`，且 active/inactive 图标仍都传入 `menu_obj_cube`；全 GUI 搜索没有 `tab_generate_3d` 引用。因此两个已知 UI 修复均未接入代码。
- 历史会话还保留了产品决策：生成结果优先 3MF；图生 3D 采用“图片+文字联合提示”，若 Tripo v3 不直接支持附加文字，则应走可验证的组合流程，不能静默丢弃 prompt。
- 可确认完整 Release 构建、安装和 mock 驱动运行曾成功；历史记录还明确表示 GUI 编译与 4 个 Python 模块语法检查通过，并将真实 Tripo 的两次付费 smoke（文生/图生）留作下一阶段。
- “界面问题已全部复测”的记录发生在独立主页面迁移前；迁移后新发现的默认页抢占和导航图标问题仍未完成，不能混为同一轮验收。

- 当前实现已形成两条用户流程：
  - AI 调参助手：菜单打开右侧停靠面板，调用 `/config-proposal`，校验后按勾选项写入 preset 并重新切片（`MainFrame.cpp:3111-3114`、`Plater.cpp:5139-5147`、`AIAssistantPanel.cpp:98-128,161-205`）。
  - AI 模型生成：独立 `3D Generate` 页支持 Text-to-3D 与 Image+Text-to-3D，经 GPT 预处理、用户审核、额度确认、任务轮询、3MF/STL 下载及二次确认后，通过 `Plater::add_model` 导入当前盘并切回编辑页（`ModelGenerationPanel.cpp:143-179,285-371,420-448,506-556`）。
- C++ 只依赖 sidecar HTTP 契约，供应商细节位于 Python；但 Python 当前直接绑定 AGNES、OpenAI 和 Tripo，尚无 provider registry。
- 明确未完成项：Model Library 仅为占位（`ModelGenerationPanel.cpp:244-254`）；GUI 不管理 sidecar 生命周期；取消操作不保证终止远端 Tripo；任务持久化/恢复、生成产物深度验证和 AI 定向自动测试尚缺失。
- 上线前技术缺口：任务仅在 sidecar 内存中且无 TTL；mock 与生产协议存在漂移；sidecar 启动集成目前仅有 Windows 批处理；AGNES 端点缺少 OpenAI/Tripo 已有的 HTTPS/无内嵌凭据校验；本地请求标记不是有效身份认证。
- Python sidecar 未被正常 CMake/install/package 规则包含；当前启动依赖源码目录、外部 Python 和 Windows 批处理，不满足 Windows/macOS/Linux 三平台交付要求。
- AI 功能未受配置或 feature gate 控制：模型生成页和 AI Assistant 面板无条件构造，sidecar 未配置时仍会改变导航并暴露不可用入口。
- 新面板已加入 gettext 抽取清单，但 POT 创建日期早于这轮接入，新增 AI/Tripo/GPT 文案尚未进入 POT/PO 目录。
- 当前启动方式是批处理分别启动 sidecar 和 OrcaSlicer，无 readiness、健康版本协商、崩溃重启或关闭联动。
- 7 月 28 日这一轮主要把此前独立的客户端/sidecar 能力接成可见产品 UI，包括新页签、停靠助手、导入回调、关闭清理和 CMake 接入；现状是首个可操作纵向切片，而非完整 AI Workspace。
## M0 AI 功能门控与能力发现实现（2026-07-30）
- 已新增 `enable_ai_features` AppConfig 布尔设置，默认 `false`；Preferences 的 Developer → Experimental Features 提供需重启生效的显式开关。默认状态不创建 AI page/menu/AUI pane，也不会请求 sidecar。
- production 与 mock 的 `GET /health` 已统一为 provider-neutral v1 文档：`ok`、整数 `protocol_version`、诊断性 `sidecar_version` 与 `capabilities.config_proposal/model_generation`。模型生成仅在 OpenAI 预处理与 Tripo 均配置时标记 available；协议不暴露 provider、密钥、模型或 endpoint。
- 新增 `tools/ai/test_sidecar_contract.py`，使用临时 loopback servers 验证 production/mock schema、未配置/配置 capability 和无凭据泄露；该测试不触发外部 provider 请求。
- 新增 `AIServiceManager`，只在用户启用功能后非阻塞请求 loopback `/health`，限制响应为 16 KiB，接受严格 v1 schema；请求取消和 `CallAfter` 生命周期由 weak token 保护。非 loopback endpoint 直接 fail closed，与模型生成路径的本地信任边界一致。
- `MainFrame` 将 3D Generate 改为 capability 成功后追加标签，不再占用 `TabPosition` 固定 index，因此默认 Prepare/Preview 等现有索引回到历史位置；新增 active/inactive 专用图标。AI Assistant 也仅在 config proposal capability 成功后延迟创建 AUI pane 和 View 菜单项。
- `Plater` 的 AI pane API 已做空指针安全处理，避免无 capability、reset layout 或关闭过程访问不存在的 pane。
- 验证已通过：`python tools/ai/test_sidecar_contract.py`（3 tests）、对三份 Python 文件的 `py_compile`、`git diff --check`。未完成：C++ GUI 编译、Catch2 AppConfig 测试和运行时 GUI E2E；本 shell 没有可用的 `cmake`、`MSBuild.exe`、`devenv.com` 或 `ninja.exe`，即使 `build/OrcaSlicer.sln` 存在也不能在当前会话执行。

- 验证更新：用户关闭占用实例后，Visual Studio CMake 成功构建 `libslic3r_gui` 与 `OrcaSlicer` Release target，并生成安装目录的 `orca-slicer.exe`。隔离 `--datadir` GUI E2E 证实默认值会持久化为 `enable_ai_features=false` 且不触发 mock discovery；启用后 mock 收到 `/health`，响应窗口实际出现并可打开运行时追加的 `3D Generate` 完整输入/预览页面，且该页没有抢占默认首页。当前 build 禁用 `BUILD_TESTING`/`BUILD_TESTS`，没有生成 Catch2 target；wx 自定义菜单未暴露给 Windows UI Automation，故 “Show AI Assistant” 菜单/停靠 pane 的互动验收待专用驱动补充。

- 验证补充：已在隔离 `.workbuddy/build-tests`（`BUILD_TESTS=ON`、`BUILD_TESTING=ON`）构建 `libslic3r_tests`，并随机顺序执行 `AppConfig AI feature gate`，3 项断言全部通过。AI Assistant 菜单与 AUI pane 的实际点击验证仍待专用 wx/DPI-aware 驱动；现有 Windows UI Automation 不暴露顶栏菜单项，坐标自动化会误触其他 GUI 控件，故未将其宣称为已验证。

## 技术决策
| 决策 | 理由 |
|------|------|
| 继续检查 Git 与本地构建状态 | 这是恢复实际开发进度最可靠的信息来源 |
| 续作顺序：Git 保护 → UI 收尾 → mock 复测 → 真实 Tripo 双 smoke | 降低无版本控制修改的风险，并优先封闭已经明确的功能缺口 |
| 模型库、sidecar 托管和完整 AI Workspace 后置 | 当前应先把已实现纵向切片验收到可稳定继续开发的状态 |

## 遇到的问题
| 问题 | 解决方案 |
|------|---------|
| 当前环境元数据显示目录不是 Git 仓库 | 检查顶层目录和可能的嵌套仓库 |

## 资源
- `CLAUDE.md` / `AGENTS.md`：OrcaSlicer 项目说明。

## 五大产品域状态复核（2026-08-08）

### 证据基线
- 已完整读取 `Docs/README.md`、`Docs/architecture/README.md` 及 `01` 至 `05` 架构文档。
- 已用 spreadsheet artifact runtime 读取并渲染 `Docs/开发进展.xlsx`；唯一工作表的有效数据范围为 `B3:F30`，共 27 项子能力。
- `Docs/README.zip` 内的文件名和大小与当前 Markdown/图表一致，是同一套资料的归档副本；两张 AI 目标架构图与文字说明一致，没有额外的当前实现声明。
- 当前源码基线为 `master@a1ef7204fe`；架构资料的官方现状基线为 `main@a62fb17e03d159d5b562cc6d64163346e454b5de`，两者用途不同。

### 关键状态修正
- 文生/图生 3D 已有可运行纵向链路，但真实付费 smoke、任务恢复、三平台打包和彩色保真尚未完成，不能标为生产完成。
- 生成预览当前主要是预处理参考图、状态和摘要，不是生成 mesh 的可旋转 3D 预览。
- 参数智能调优不是纯规划：现有 AI Assistant 已支持白名单参数、严格值校验、用户选择、应用和重切片；但没有隔离试切、候选比较和指标闭环。
- 会话级 Model Library 已真实实现：只保存在内存，最多 12 条，不保存模型副本。
- 彩色链路已实现 OBJ 顶点色协议与格式校验；未验证颜色到耗材映射、3MF round-trip 和切片保真。
- 自动摆盘、自动朝向、多材料绘制、网格检查/修复和 G-code 指标均是可复用的 OrcaSlicer 原生基础，不是现成 AI 工作流。
- C++ 侧保持 provider-neutral，但 Python sidecar 仍直接绑定 OpenAI-compatible 与 Tripo；未找到 provider registry 或荣耀 adapter。
- 未找到荣耀账号、荣耀 AI、权益、额度或计费实现。隐私已有上传告知、loopback 和 HTTPS/URL 校验等局部防线，尚无完整治理。
- 历史计划中的 `enable_ai_features` 默认关闭结论已过时：当前 `MainFrame` 总是创建 3D Generate 页面并执行 sidecar discovery，动作是否可用再由 capability 控制。

### 路线图决策
- 采用核心 AI 轨与商业平台轨并行：核心轨从生成产品化推进到检查/修复、智能摆盘/上色、参数闭环和 Guided Workflow；商业轨从荣耀身份推进到荣耀 AI Gateway、权益/额度/计费与隐私运营。
- 两条轨道共用 F0 工程基线，在公开 Beta 前汇合；账号或云 AI 失败不得影响本地编辑和切片。
- 账号、权益、额度和账单由服务端最终裁决；桌面只缓存短期状态，不保存长期供应商密钥。
- 继续坚持 OrcaSlicer `Model`、Config、Print、Preview 为业务真值；AI 只生成候选、诊断和解释。
- 详细矩阵、架构决策、依赖和发布门槛见 `Docs/AI能力状态与实施计划.md`。

## 当前软件基线验证（2026-08-08）

### 已验证通过
- 基线为 `master@a1ef7204febebfed36a69589ffb4da10e2c89002`，提交说明为 `fix AI model generation startup recovery`。
- Windows Release `ALL_BUILD` 成功，安装目录 `build/OrcaSlicer/orca-slicer.exe` 可启动；构建目录与安装目录 EXE/DLL 哈希一致。仅 `OrcaSlicer_profile_validator` 有既有 `LNK4098` 运行库冲突告警。
- 当前 HEAD 的 `libslic3r_tests` 完整执行 129/129 通过，覆盖 Arrange、3MF、Config、MeshBoolean、Preset 等核心能力。
- `tools/ai/test_sidecar_contract.py` 4/4 通过；production/mock `/health` 都返回 protocol v1，文生/图生与 OBJ/3MF/STL capability schema 一致；相关 Python 模块通过 `py_compile`。
- 隔离数据目录 GUI 首次启动成功。默认停留首页，`3D Generate` 入口存在但未抢占默认页；mock discovery 后页面显示 `Local 3D generation service is ready.`。
- mock 文生 3D 从输入、预处理、用户确认、生成轮询到 `Generated model is ready` 全部通过；结果为带顶点色的 OBJ。
- 彩色 OBJ 下载后进入 OrcaSlicer 原生颜色映射确认，确认后自动回到准备页；日志记录可撤销的 `Import Object` 快照和 `load_model 1`，证明结果已导入当前盘面。
- 正常关闭时出现未保存项目确认；选择不保存后 OrcaSlicer 进程完全退出，无残留进程，既有 production/mock sidecar 均保持运行。

### 当前缺口与边界
- 当前 HEAD 已移除 `enable_ai_features` 门控，历史 `AppConfig AI feature gate` 测试也已从重建后的测试清单消失；目前没有有效的 C++ AI capability/GUI 回归测试。
- AI Assistant 自定义菜单与 AUI pane 仍缺少可靠自动化交互覆盖；本轮未将其宣称为运行态通过。
- 本轮只使用 mock，不执行真实 Tripo/OpenAI/荣耀付费调用；真实文生/图生、额度、失败补偿与幂等性仍待验收。
- 彩色 OBJ 导入会进入原生颜色映射对话框，不能视为无交互自动导入；颜色到耗材、3MF round-trip、切片保真仍未验证。
- Windows Release 与核心测试已验证；macOS/Linux 构建、打包和运行未验证。

## 2026-08-10 核心演示版范围候选
- 用户明确要求下周一提供可演示版本，macOS/Linux 优先级最低，本轮不投入。
- 推荐黄金路径：文字或参考图片输入 → AI 预处理 → 用户审核 → 3D 生成 → 结果导入盘面 → 原生摆放/检查 → 切片预览。
- 两天内只复用已验证能力：模型生成 sidecar、彩色 OBJ 导入、OrcaSlicer 原生摆放和切片；不新增复杂 Agent、自动修复或计费系统。
- 建议非目标：荣耀账号/计费、AI Assistant、模型库持久化、自动上色、风格化滤镜、参数候选闭环、macOS/Linux。
- 必须准备失败降级：真实 provider 不可用时仍能用预生成产物完成“导入 → 摆放 → 切片”后半段演示，但不能把降级路径表述为真实在线生成成功。
- 用户确认真实 AI 通路此前已验证，2026-08-10 演示必须以真实 provider 为主链。
- 当前 production sidecar 以 `OPENAI_API_KEY` 和 `TRIPO_API_KEY` 同时存在作为模型生成 capability 就绪条件；OpenAI 负责文本/图片预处理，Tripo 负责 3D 生成。
- `start_orcaslicer_with_ai.bat` 已实现 Windows 一键启动：拉起 production sidecar，轮询 `/health` 最多 15 秒，然后启动安装目录 Release 程序。
- 两个启动脚本均未硬编码 OpenAI/Tripo key，只从父进程环境继承；因此重启后的凭据可用性必须单独验证。
- 凭据已具备重启持久性：OpenAI key 配置在机器级环境，Tripo key 配置在用户级环境；当前进程均已继承。检查仅记录配置状态和长度，没有输出密钥值。

## 真实模型生成 smoke 阻塞（2026-08-08）
- 真实文生 smoke 在 Tripo 任务创建前失败，sidecar 状态为 `failed`，用户可见消息为 `The preprocessing service is temporarily unavailable.`。
- 当前 OpenAI-compatible endpoint 为 `https://104.194.93.250/v1`；携带既有凭据请求 `/models` 返回 HTTP 502，说明阻塞来自代理可用性，而不是 prompt、模型名或 Tripo 参数校验。
- 本次失败没有创建 Tripo 任务，因此没有产生 3D 生成费用。
- 为保证周一演示主链可用，采用显式环境开关 `ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=1`：文本保留已校验原始 prompt，图片复制已校验原始 PNG/JPEG；状态必须明确说明预处理不可用并正在使用原始输入。
- 降级模式仍调用真实 Tripo，不得表述为 OpenAI 预处理成功；默认模式继续 fail closed。
- 本地回归验证通过：预处理降级、sidecar contract、readiness 和 smoke 共 15 项 unittest 全部通过；相关 Python 文件 `py_compile` 通过。
- Windows 后台重启排查发现，旧 `cmd /K start_orca_ai_sidecar.bat` 控制台仍可派生占用 `18764` 的 sidecar，导致探针命中旧环境。改在 `18766` 启动显式携带降级开关的受控实例后，免费文本探针进入 `awaiting_confirmation` 并显示使用原始 prompt。
- 真实文生 smoke 成功：任务 `191806d4-5b3b-4f51-a1a5-b5803049b0d5`，产物为 3MF，27,399,111 字节，耗时 283.6 秒，保存于 `.workbuddy/core-demo-real-20260808/text/`。
- 真实图生 smoke 成功：使用 `resources/web/model/img/p1.png` 单车侧视参考图；任务 `6a95b1b1-3e71-44da-83c1-5a5414a3aca2`，产物为 3MF，27,139,455 字节，耗时 173.0 秒，保存于 `.workbuddy/core-demo-real-20260808/image/`。
- 两次真实任务都完整经历 `preprocessing → awaiting_confirmation → queued → running → ready`，客户端下载产物后执行 DELETE 清理。OpenAI 预处理仍未成功，两次均明确使用原始输入后调用真实 Tripo。
- 两个 3MF 都是有效 ZIP/OPC 包，含 `3D/3dmodel.model`；文生模型 704,186 顶点、1,408,374 三角面、约 71.33×71.14×100 mm，图生模型 703,848 顶点、1,408,070 三角面、约 46.79×100×27.23 mm。
- Windows GUI 使用隔离 `--datadir` 成功载入文生产物；日志确认解析 1 个对象、将无 Orca 元数据的 3MF 作为 geometry-only 导入，并完成 `auto placement`。窗口标题为对应真实产物，进程保持响应。
- GUI 可见彩排时工作站进入锁屏，截图只捕获锁屏界面，无法安全驱动切片与保存对话框；后续先使用同一 OrcaSlicer CLI 内核验证摆盘/切片，解锁后再补可见交互。
- CLI 使用 `--arrange 1 --export-3mf` 成功将文生产物保存为 Orca 项目 `.workbuddy/core-demo-gui-20260808/roundtrip-text.3mf`（23,366,136 字节）。
- 随后以该项目执行 `--slice 0 --arrange 1 --mtcpp 2000000 --mstpp 900` 时没有生成 G-code；根目录 `00000.log` 在重载项目对象阶段截断，Windows Application Event 1000/1001 记录 `orca-slicer.exe` 在 `OrcaSlicer.dll+0x1195fe` 发生 `0xc0000005`。当前 round-trip 重开/切片仍是阻断项。
- PDB 符号化确认故障指令位于 `std::string` copy assignment，源指针无效；GUI 模式重开同一 round-trip 项目成功到 `IMPORT_STAGE_FINISH`，说明该崩溃限于 CLI 路径，GUI 主链不受此崩溃阻断。
- 通过窗口句柄确认自定义预设安全提示后，GUI 完成项目重开，标题更新为 `roundtrip-text - OrcaSlicer`；“切片单盘”动作也被实际触发。
- 切片未生成刀路的直接原因是隔离数据目录使用不可打印的 `Default Printer`：确定性校验提示相对挤出模式需要每层 `G92 E0`。模型本身 `model_fits=1` 且 `has_printable_instances=1`。本机正常配置已选择仓库自带的 `WonderMaker ZR 0.2 nozzle` 系统预设，下一轮应使用真实预设验证。
- 使用正常 Orca 配置和系统预设 `WonderMaker ZR 0.2 nozzle`、`0.08mm Optimal @WonderMaker ZR 0.2 nozzle`、`WonderMaker PLA Basic` 后，真实文生模型的 GUI 切片成功。切片从 18:44:05 开始，18:44:19 完成 G-code 导出，完成状态为 0，`psGCodeExport=1`，并进入预览。
- 生成的临时 G-code 为 45,019,094 字节；摘要为 1249 层、44.09 g PLA、预计打印时间 6 小时 47 分 40 秒。UI Automation 同时确认“导出G-code文件”控件已启用且可见。
- 阶段 11 的 Windows 核心演示黄金路径已经闭环。CLI 重开 round-trip 3MF 后切片的 `0xc0000005` 仍是独立已知问题，但 GUI 重开及切片均通过，不阻塞周一演示。

## 3D Generate 交互验收（2026-08-10）

- 用户标注的根因有两项：预览位图参与布局且只做单次裁切式适配；进度条直接使用 provider 内部数值，阶段切换与用户认知不一致。
- 最终预览采用 `wxScrolledWindow` 双缓冲自绘。1080×1620 参考图在预览区内按原始宽高比完整居中显示；125% 时位图放大且垂直滚动范围生效；Fit 后恢复完整适配。
- 预览控件提供 `- / Fit / + / 百分比`，选图后立即显示原图；放大只改变预览内部虚拟尺寸，不改变左右栏布局。
- 生成进度固定为 Input、Prepare、Review、Generate/Finalize、Import 五步。mock GUI 验证 Review 为 `Step 3 of 5` 和 35%，Ready 为 `Step 5 of 5` 和 95%。Generate 的同步入口固定从第四步 40% 开始。
- Windows 中文代码页会把未本地化包装的 UTF-8 `·` 和 `×` 显示成“路”和“脳”；相关字符串已统一通过 `_L(...)` 构造。
- 工作站锁屏时 `PrintWindow` 能捕获窗口和原生控件，但不能捕获 `wxStaticBitmap` 的像素内容；本轮以静态位图句柄、尺寸、缩放标签和 Win32 滚动信息完成确定性验收。
- 详细设计见 `Docs/plans/2026-08-10-model-generation-interaction-design.md`。

## 两段式图生 3D 需求复核（2026-08-10）

- 用户要求的目标流程是：参考图 + 文字指令 → AI 风格化二维预览 → 用户确认 → 3D 模型生成，风格化预览是强制确认点。
- 原始本机参考图为完整的 1080×1620 竖幅，头部、躯干和下方服装均存在；裁切不是源文件导致。
- production sidecar 已调用 OpenAI-compatible `/images/edits` 生成风格化图，但演示启动器默认打开预处理降级；图片服务失败时会复制原图并进入 `awaiting_confirmation`，这会让用户误以为已经生成风格化预览。
- GUI 当前也会在风格化预览尚未完成下载时启用 3D 生成按钮，确认门槛不够严格。
- 推荐修正：自绘 letterbox 预览保证 Fit 完整显示；区分“Reference image”和“AI style preview”；图片模式禁用原图降级，并在风格化预览成功下载后才允许生成 3D。

## 两段式图生 3D 最终验收（2026-08-10）

- GUI 已改为 `wxScrolledWindow` 双缓冲自绘，Fit 使用 `min(可用宽/原宽, 可用高/原高)` 等比缩放并居中留边，不再依赖子位图控件布局。
- 物理分辨率截图确认 1080×1620 的 `方飞总.png` 从头部到下方服装完整显示；预览标题为 `Reference image`，像素尺寸显示正确。
- 图片流程现在明确显示 `Input → Style preview → Review → Generate 3D → Import`；左侧长文案已换行或精简，长文件名使用尾部省略。
- mock GUI 验证参考图阶段 `Generate 3D from preview` 为禁用；128×128 风格预览下载并显示后，标题变为 `AI style preview`、进度进入 `Step 3 of 5`，按钮才启用。
- production sidecar 不再对图片预处理使用原图 fallback；缺少 `OPENAI_API_KEY` 时返回 `AI style preview generation is not configured.`，图片编辑失败时任务进入 `failed`。
- Windows Release `/m:1` 构建通过，安装目录 DLL SHA256 为 `D4230825137664051FBB2F050D6F93E3E90E79D064BDD84E82F508025EC497A9`；并行构建仅因系统分页文件不足失败，不是代码错误。
- AI Python 回归 15/15、`py_compile` 和 `git diff --check` 均通过；GUI 证据位于 `.workbuddy/ui-interaction-20260810/stage13-final-reference-fit.png` 与 `stage13-final-style-preview.png`。

## 真实 AI 风格预览代理迁移（2026-08-10）

- 机器级环境变量已改为 `OPENAI_BASE_URL=https://laotie.dev`，但当前 Codex 与旧 sidecar 仍继承 `https://104.194.93.250/v1`；此前页面继续失败的直接原因是 `18764` 上复用了旧进程。
- `https://laotie.dev/models` 与 `https://laotie.dev/v1/models` 均返回 HTTP 200，模型列表包含 `gpt-image-2`。代码现在将域名根地址规范化为标准 `/v1`，已有 `/v1` 或自定义兼容路径保持不变。
- 图片编辑不再只透传短指令；sidecar 会补充“明确重绘、保留人物身份/姿态/服装/关键颜色、保持完整构图、禁止裁切与原图直出”等约束，用户指定风格仍为最高优先。
- 使用 `方飞总.png` 完成一次真实图片编辑：原图 1080×1620、SHA256 `21662206C03CC7FBBF35E2E9F1E0AAD8E61FEA4F246D15C73959E688FFBA9E46`；输出 1024×1536、SHA256 `4EBAF176C3FE98438749D9F6342EDEA2EE1CF641097CFC96CD7F97B663450A58`，确认不是原图复制。
- 人工复核真实结果：已形成清晰 3D 动画卡通风格，人物身份、微笑、交叉双臂、白色西装与深绿上衣得到保留，头部、双臂和服装构图完整。
- 当前 production sidecar PID `39212` 监听 `127.0.0.1:18764`，health protocol v1 与 model generation capability 正常；OrcaSlicer PID `53840` 保持响应。

## 原图与 AI 结果对照预览（2026-08-10）

- 用户截图中的右侧图片已是 AI 返回的 `1024×1536` 结果，但页面只显示一张图，无法直观确认它与 `1080×1620` 原图的差异。
- 根因位于 GUI 状态模型：`download_preview()` 将 AI 结果写入唯一的 `m_preview_image`，直接覆盖 `show_selected_image_preview()` 保存的参考图；并非图片 API 或下载失败。
- 采用同一预览画布内左右并排方案：左侧固定 `Reference`，右侧固定 `AI result`，共享 Fit/缩放/滚动。该方案比标签页更符合“同时看到两张图”，比拖动对比条更适合周一演示和不同构图结果。
- AI 生成前右侧显示明确等待态；生成失败时左侧参考图继续可见，右侧显示可重试提示；成功后两侧各自按完整构图 Fit，不互相覆盖。
- GUI 已将单一预览缓存拆为 reference/style 两套 `wxImage` 与 `wxBitmap`；Model Library 缩略图优先使用 AI 结果，没有结果时才回退参考图。
- Windows Release 的 `libslic3r_gui` 与完整 `OrcaSlicer` 构建通过，仅保留既有 `LNK4098` 告警；AI Python 回归 21/21 与 `git diff --check` 通过。
- 使用 `18765` mock sidecar 和隔离数据目录完成 Review 验收：左侧完整显示 1080×1620 原图，右侧显示 128×128 mock AI 结果；摘要同时列出两者尺寸，3D 按钮已启用。
- 共享缩放验证达到 125%，Fit 正确恢复 100%。DPI-aware 完整截图保存于 `.workbuddy/stage15-compare-run/comparison-review-dpi.png`。
- 正式 DLL 已同步到 `build/OrcaSlicer/OrcaSlicer.dll`，与 `build/src/Release/OrcaSlicer.dll` SHA256 同为 `649254FE2E611B0695BE12398F4AFC99D2B0300B63F4DD881D6B3961431E28C0`；正式程序 PID `19100` 已重启且保持响应。

## OBJ-only 生成与导入（2026-08-10）

- 原实现按 `obj -> 3mf -> stl` 顺序转换；OBJ 转换或顶点色校验失败时会静默回退，因此 GUI 最终可能导入 3MF/STL。
- 新契约只声明 `artifact_formats: ["obj"]`。production sidecar 只请求一次 OBJ 转换，失败时任务明确进入 `failed`，不再用其他格式掩盖问题。
- GUI capability discovery、启动 readiness 检查和 smoke 客户端均要求 OBJ；GUI 下载入口再次校验格式，只允许带受支持顶点色的 OBJ 导入当前盘面。
- `AIModelGenerationClient` 仍保留旧格式的底层响应解析能力，用于兼容旧任务/诊断；产品工作流不会下载或导入非 OBJ 产物。
- 新增 `test_obj_generation.py`，覆盖 OBJ 成功路径与失败不回退路径。AI Python 回归 23/23 通过。

## Tripo OBJ 资源包与颜色根因（2026-08-10）

- 本次真实 Tripo 转换已经成功，下载产物为 25,695,128 字节的 ZIP，文件头为 `PK\x03\x04`，并非损坏的 UTF-8 OBJ。
- ZIP 内包含一个约 100 MB 的 OBJ、一个 MTL，以及 Base Color、Normal、Roughness、Metallic 四张 JPEG 贴图。
- sidecar 将整个 ZIP 误命名为 `artifact.obj` 后按 UTF-8 文本读取，因此报错 `The generated OBJ is not valid UTF-8 text.`。
- 真实 OBJ 的顶点只有 XYZ，颜色通过 UV、MTL 的 `map_Kd` 和 JPEG 底色贴图表达，不是顶点色。
- OrcaSlicer 的 OBJ 解析器能读取 MTL、UV 和贴图引用，但 `Model::read_from_file()` 对 `has_uv_png` 的导入分支仍被注释为 developing；因此用户手工解压并导入 OBJ 时只有几何，没有颜色。
- 演示版采用 provider 适配层转换：完整保留原始纹理资源包，并从 Base Color 贴图按 UV 采样生成单文件顶点色 OBJ，复用 OrcaSlicer 现有颜色聚类和耗材映射流程。

## OBJ 资源包修复验收（2026-08-10）
- sidecar 现在把每个任务固定到 `ORCASLICER_AI_OUTPUT_DIR/<job-id>/`；Windows 启动器将该目录设为项目根目录下的 `generated_models/`，直接启动 sidecar 时也以当前项目目录为默认根目录。
- ZIP 解包拒绝绝对路径、路径穿越、重复路径、符号链接、特殊文件、加密条目、超限文件数和超限解压体积；输入图、AI 预览、原始 ZIP、OBJ、MTL 和全部贴图均保留。
- 纹理转换按 OBJ 的 `(material, vertex, texture-coordinate)` 组合拆分纹理接缝，按 `map_Kd` 底色贴图采样 RGB，输出不依赖 `mtllib/vt/vn/usemtl` 的单文件顶点色 OBJ。
- 真实失败 ZIP 离线恢复成功：最终 OBJ 为 75,029,064 字节、742,065 个带 RGB 顶点、1,449,376 个面，抽样至少 1,000 种不同颜色；未发起新的付费 AI 调用。
- OrcaSlicer 实际加载恢复 OBJ 后出现 `Obj文件导入颜色` 对话框，证明顶点色进入现有颜色聚类/耗材映射分支，而不是只加载几何。
- Windows 允许多个 `ThreadingHTTPServer` 进程复用 18764；运行验收时发现两个监听者后已全部停止，并只通过 `laotie.dev` 启动脚本恢复一个 production sidecar。

## Tripo 彩色低模与色板约束（2026-08-10）
- 用户给出的 `/zh/docs/files` 页面只定义文件上传：图片 PNG/JPEG 最大 20 MB，模型支持 GLB/GLTF/FBX/OBJ/STL 最大 150 MB；它不包含生成质量参数。
- Tripo v3 的文本/图片生成 3D 接口都支持 `face_limit`、`texture`、`pbr`、`texture_quality`、`geometry_quality`、`quad`、`smart_low_poly` 与 `export_uv`。
- `smart_low_poly=true` 时三角面 `face_limit` 范围统一为 500–20,000，四边面为 500–10,000；官方提示该模式最适合简单输入，复杂模型可能偶发失败。
- `quad=true` 会强制输出 FBX，与当前 OBJ-only 契约冲突，因此本项目低模路径应保持 `quad=false`。
- 图片生成 3D 还支持 `texture_alignment=original_image|geometry`；要保持用户确认过的打印色板，推荐 `original_image`。
- Tripo 的图像编辑和 3D 生成接口均未提供“限定颜色数量”或“指定离散色板”字段；提示词只能提高遵循度，不能保证输出严格落在打印机可用颜色集合中。
- 可打印颜色需要确定性闭环：把当前打印机/耗材槽颜色写入风格化提示词，随后将 AI 预览本地量化到精确色板；以量化预览生成 3D，并在 Base Color 转顶点色时再次映射到同一色板，确保 Orca 最终只看到受支持颜色。
- Orca 已有权威色板入口：`Plater::get_extruder_colors_from_plater_config()` 从 `preset_bundle->project_config` 读取 `filament_colour`；OBJ 导入颜色对话框也使用该入口，因此 AI 页面应复用它，避免维护另一套打印机/AMS 色板状态。
- 演示版低模默认参数建议固定为 `smart_low_poly=true`、`face_limit=20000`、`texture=true`、`pbr=false`、`texture_quality=standard`、`geometry_quality=standard`、`quad=false`、`export_uv=true`；图生 3D 额外使用 `texture_alignment=original_image`。
- 低模失败不应静默生成超高面数模型。推荐将任务标记失败并允许用户明确选择一次“兼容模式”重试；兼容模式关闭 `smart_low_poly`、将 `face_limit` 提高到 50000，同时在生成确认框中提示面数与导入风险。
- 颜色量化应采用确定性的最近色映射，并在感知色彩空间中计算距离；透明像素保持为背景处理，重复/无效 HEX 颜色在进入 sidecar 前去重和拒绝。量化后的预览才是用户确认和 Tripo 图生 3D 的输入。
- 最终 OBJ 烘焙阶段再次映射到相同色板，使每个 `v x y z r g b` 的 RGB 必然属于耗材色集合；这也避免 Tripo 的纹理重建重新引入渐变和中间色。

## 彩色低模实现与真实预览验收（2026-08-10）
- Tripo 文生/图生请求现固定使用 `smart_low_poly=true`、`face_limit=20000`、`texture=true`、`pbr=false`、`texture_quality=standard`、`geometry_quality=standard`、`quad=false`、`export_uv=true`；图生额外使用 `texture_alignment=original_image`。
- C++ 从当前项目 `filament_colour` 读取、规范化、去重并展示最多 16 色；预览后色板若变化，3D 按钮保持禁用并要求重新生成预览。
- OBJ 烘焙改为复用原始几何顶点索引，UV 接缝仅参与颜色投票，不再复制拓扑顶点；导入前强制校验不超过 20,000 个三角面、单连通体、每条边恰好两个邻面以及顶点颜色严格属于色板。
- 旧 1,449,376 面真实 OBJ 通过新门禁离线检查时立即被拒绝并返回 `exceeds the 20000-triangle low-poly limit`，不会进入 Orca 修复后丢色的路径。
- 200% DPI GUI 验收发现 16 色单行/拉伸网格会被滚动条裁切；最终改为按内容宽度的固定 6 列网格，完整显示为 `6+6+4`，色板摘要使用明确两行文案。
- 第一次真实付费图片预览保持了人物身份和全身构图，但比例偏写实且背景有大量细碎色点；提示词随后增加 Q 版大头短肢、玩具表面、纯色无纹理背景约束，并加入局部众数净化。
- 第二次真实付费图片预览形成明确 Q 版盲盒比例和纯色背景；最终文件 `generated_models/paid-image-validation/palette-preview-v2-final.png` 为 1024x1536，只使用当前 16 色板中的 11 色，色板外像素为 0。
- 真实验收发现 RGB 分通道众数滤波可能拼出色板外颜色；实现已改为在索引色 `P` 模式上做众数滤波后再转 RGB，并增加多色图案回归，保证滤波不能创造新颜色。

## 真实 Tripo 彩色低模验收（2026-08-10）
- 经用户明确授权后只创建了一次付费图生 3D 任务 `9ba4255f-732c-430d-a381-0ae3a8e5507a`；OBJ 转换任务为 `ea204146-0e6f-46cb-af55-e696f34d0fec`，没有创建第二个 3D 生成任务。
- Tripo 返回的任务输入明确回显 `smart_low_poly=true`、`face_limit=20000`、`texture=true`、`pbr=false`、`quad=false`、`export_uv=true` 和 `texture_alignment=original_image`，说明本地参数已正确送达服务端。
- 生成任务成功并消耗 40 credits；OBJ 转换成功并消耗 5 credits。所有输入、响应、原始 ZIP、OBJ/MTL/贴图、顶点色 OBJ、渲染预览与状态均保存在 `generated_models/paid-tripo-validation/9ba4255f-732c-430d-a381-0ae3a8e5507a/`。
- 官方渲染预览与确认过的 Q 版参考一致，保留人物身份、白色服装、深色头发和大头短肢比例；风格生成不是本次失败原因。
- 最终顶点色 OBJ 为 1,203,991 字节、13,066 个顶点、26,147 个三角面，只有 1 个连通分量且无退化三角形；但超过 20,000 面上限，并包含 26 条边界边和 14 条非流体边。
- OBJ 使用 10 个耗材色，色板外颜色为 0，证明“预览精确量化 -> Tripo 贴图 -> OBJ CIE Lab 二次量化”的颜色闭环有效。
- 真实结果证明 `face_limit=20000` 对 Tripo 是目标约束而非可靠的客户端验收保证，文字中的 watertight 要求也不能保证图片生成 3D 的网格流体性；硬门禁仍然必要。
- 新增 `tools/ai/run_paid_tripo_validation.py`：付费 task ID 一返回即原子落盘，重跑只恢复原任务；脚本输出完整拓扑/色板诊断，失败时不导入 Orca、不删除产物、也不自动创建第二个付费任务。

## 阶段 20：三风格、颜色保真与自动切片发现（2026-08-10）
- 当前风格化图片链路同时施加三重强约束：提示词要求只使用耗材 HEX、生成后无抖动最近色色板量化、随后执行 `5x5` 索引众数滤波。面积较小的色块会被映射或滤除，视觉上容易退化为两种主色；这不是 provider 没收到全部颜色的充分证据，而是后处理主动压缩了颜色分布。
- 风格预览和打印色映射应分成两个明确阶段：风格图优先保证身份、材质、轮廓和层次；打印预览再映射到耗材色板，并显示实际使用颜色与未使用颜色。不能要求模型机械地平均使用每一个耗材色，也不能用大窗口众数滤波吞掉细节。
- `Camera Roll/方飞总.png` 是干净背景的半身人物，适合验证身份保持、Q 版和赛博朋克材质；`刘亦菲.jpg` 是完整站姿人物，最适合验证可打印全身构图、自动缩放和摆盘；`大雁塔.jpg` 适合古典建筑风格，但底部设备水印/色卡必须裁掉或明确排除，且画面中佛像与塔是两个独立主体，不适合作为“一体模型”黄金路径首个 3D 输入。
- 三风格应采用结构化 profile，而不是仅把“Q 版/赛博朋克/古典”追加到自由文本：每个 profile 需要独立的造型比例、表面材质、背景、打印结构和禁止项，并共享单主体、完整轮廓、稳定底座、无悬浮碎件等 3D 友好约束。

## 阶段 20 最终验收（2026-08-11）
- 三风格严格色板基线为 `q-cartoon-palette-v9.png`、`cyberpunk-palette-v9.png` 与 `classical-palette-v3.png`；三图均使用全部 16 个指定颜色，色板外像素为 0。
- 唯一真实 generation task `4c907118-0e3e-45cb-8d18-b21f5c1dafef` 及 conversion task `a17a0d56-4154-410a-96ae-09fc5a8f23ae` 均成功。最终 OBJ 为 15,148 面、单连通、封闭流体、无边界边、无非流形边、无退化三角且颜色严格属于耗材色板。
- GUI mock 黄金路径证明 AI OBJ 专用颜色回调不会弹出普通 OBJ 颜色对话框；成功导入后会自动落床、发起切片并切换 Preview。切片日志明确记录 `model_fits=1`、G-code 导出成功和 viewer 加载成功。
- 当前 WonderMaker 八色项目的多色 G-code 会触发热床路径检查提示，即使模型本身位于热床内。它属于打印机/冲刷路径配置层，不应被误判为 AI 生成模型越界；后续可单独优化多色切片参数模板。
- 正式运行态已用 `https://laotie.dev` 显式启动，sidecar protocol v1 同时声明 text、image 和 OBJ 能力可用。真实 3D 预算仍剩余 `19/20`。
- 当前 Python 回归会报告 Pillow `Image.getdata()` 将在 Pillow 14 移除；这是 2027 年兼容性维护项，当前 55 项测试均通过，不属于下周演示阻断项。

## 阶段 20 重新审计：自然色板与真实网格修复（2026-08-11）
- 耗材色板应是允许颜色集合，而不是必须全部使用的检查清单；强制补齐缺失颜色会在模型底座制造无语义彩虹条带。自然量化结果只需保证零色板外像素，并在色板允许时保留至少三种有意义颜色。
- 自然色板真实 generation `92c11c8e-e949-4902-9bc6-1566b4536846`、conversion `a873c5f1-95a5-4ecb-bf56-0a4a79c2e3fc` 证明服务端低模约束有效，但不能保证单体和流体：结果 15,613 面、2 个组件、45 条边界边、4 条非流形边。
- 第二组件仅 81 顶点/142 面，包围盒约 2-3 mm，而主体为 100 mm、15,471 面；可按“面数和包围盒对角线均不超过主体 5%”确定性删除。尺寸或面数超过阈值的独立组件不得静默删除。
- Orca 已有 `fix_model_with_cgal_gui(..., keep_painting=true)`：它会保存并重映射 `mmu_segmentation_facets`，适合 AI 导入路径自动修复少量开边。修复后必须再次严格检查开放边，失败则撤销导入并停止自动切片。
- 第二轮 CGAL 自动修复的 GUI 证据 `generated_models/gui-validation-phase20-natural/11-repair-error-detail.png` 明确显示 `Repair failed: mesh still open after hole filling.`；日志证明 OBJ 先成功导入，随后保护逻辑主动撤销，因此空白 Prepare 页是失败回滚而不是渲染丢失。
- 清理微小脱离件后的主体仍有 27 条边界边与 4 条非流形边；非流形缺陷集中在约 1 mm 局部。删除 11 个非流形关联面后无非流形边，但形成带两个四度顶点的小型 figure-eight 边界，超出当前 CGAL 逐环补洞路径的稳定处理范围。
- sidecar 局部预修复对同一真实主体成功：移除 11 个非流形关联面、1 个失去引用的旧顶点，拆分并补齐 11 个小边界环，新增 11 个继承边界主导耗材色的中心顶点和 38 个三角面。结果为 15,498 面、7,749 顶点、单连通、0 条异常边。
- 修复前后均为 11 种耗材颜色，新颜色集合与丢失颜色集合都为空；说明在 OBJ 顶点色阶段做局部拓扑修复可以闭合网格而不破坏 MMU 色板语义。
- 原始 `artifact-raw.zip` 的完整 sidecar 离线重放同样得到 15,498 面、单连通、0 异常边且严格色板有效，证明解包、贴图烘焙、微小组件删除、局部补洞和最终门禁是一条可复现链路。
- GUI 使用上述完整重放 OBJ 自动进入 G-code Preview，证据为 `generated_models/gui-validation-phase20-natural/14-repaired-flow-result.png`。日志记录自动摆盘完成、`model_fits=1`、单对象、`Exporting G-code finished` 与切片状态 0；没有再次进入 CGAL 修复或撤销导入。
- 验收 datadir 当前只有 5 个耗材槽，而历史真实 OBJ 来自 11 色自然色板，因此导入按当前项目颜色最近映射后实际使用 2 个槽；颜色压缩发生在明确的项目色板适配阶段，不是拓扑修复丢色。正式生产生成会在预览前冻结当前项目/手动配置的色板，避免该输入色板不一致。

## 2026-08-11 最终需求审计
- 用户标注截图对应旧版五段式侧栏，主要问题是模式选择、准备区和生成区同时常驻，导致滚动与信息密度过高。
- 当前实现已将导航顺序固定为 `Home -> 3D Generate -> Prepare -> Preview`，并将工作流压缩为 `Input -> Review -> Generate -> Import`。
- 输入状态机不再依赖文字/图片模式选择：文字非空或已选择图片任一条件成立即可启用预处理，两者可同时提交；空输入保持禁用。
- 当前项目色板通过 `Plater::get_extruder_colors_from_plater_config()` 读取，页面 `wxEVT_SHOW` 会调用 `refresh_controls()` 重新同步；自定义色板最多 16 色且不回写项目配置。
- 200% DPI 最终截图显示左侧首屏无控件重叠，参考图采用完整适配显示，并预留独立 AI 结果框。
- 三种自然映射结果分别使用 10、9、11 种允许色，说明“只剩两个颜色”的退化已消除；自然模式将色板作为允许集合，不再为用满全部颜色而向底座添加人工彩带。
- 最新生产 Q 版预览在当前项目 8 色配置下实际使用 4 种有意义颜色并保持完整人物和底座，适合作为本轮连续 3D/G-code 验收输入。
- 真实生产项目的 8 个颜色槽并非同类耗材：1 个 PLA 槽与 7 个 ABS 槽的推荐温区不重叠。为保证一键 G-code 的打印安全，自动色板必须同时考虑颜色和温度兼容性，而不能只读取 `filament_colour`。
- 关闭 Orca 的混合温度保护虽然能绕过验证，但可能造成堵头或设备损坏，因此最终方案选择最大兼容槽组并保留原始槽号；当前配置应自动保留 7 个 ABS 颜色、排除白色 PLA 槽。
- `generated_models/gui-validation-phase21-production/08-real-config-compatible-palette.png` 实际停留在 Home 页，没有展示兼容色板，因此不能作为阶段 21 验收证据；需要重新进入 `3D Generate` 并捕获真实的 7 色兼容组与 1 个排除槽位。
- `07-compatible-palette.png` 展示的是隔离 datadir 中的 5 色项目，只能作为页面布局证据，也不能替代真实 1 PLA + 7 ABS 配置的温度兼容筛选验收。
- 修正运行目录 DLL 并改为逐槽读取真实耗材预设后，`13-real-compatible-palette.png` 已证明 1 PLA + 7 ABS 项目会保留 7 个兼容色并排除白色 PLA；但单行状态文案在 200% DPI 下被右侧截断，需要改为两行后再作为最终证据。
- `14-final-compatible-palette.png` 是最终有效证据：页面显示 7 个温度兼容色，1 个不兼容槽另起一行且未裁切；白色 PLA 未进入自动生成色板，其余 ABS 槽保留原始顺序。
- `15-final-gcode-preview.png` 证明真实 OBJ mock 在真实 1 PLA + 7 ABS 配置下完成自动导入、摆盘、切片和 Preview；耗材统计实际使用原始槽位 2、4、5，槽位 1 的白色 PLA 未参与。日志同时记录单对象、`model_fits=1`、`Exporting G-code finished` 与流程完成，未再出现混合温度拒绝。
- `16-production-ready.png` 证明正式 Orca 已恢复连接 `127.0.0.1:18764` production sidecar，页面显示服务就绪和同一 7+1 兼容色板；临时 `18765` mock 已停止。

## 2026-08-11 生成页 3D 预览与中文化
- 当前 `GLCanvas3D` 承担完整 Plater、床板、选择、射线拾取和切片状态，直接嵌入生成页会引入不必要的双画布状态耦合。
- `GLModel` 已支持从 `TriangleMesh` / `indexed_triangle_set` 初始化，适合作为轻量只读 3D 预览的渲染基础；仍需核对项目中是否已有可复用的小型 wxGLCanvas 宿主和顶点色渲染路径。
- 用户要求的“3D 预览放到 3D 生成这步”与当前“模型 ready 后立即自动导入并跳到 G-code”存在交互冲突：如果不增加停顿点，用户几乎看不到生成页内预览。
- 中文化范围暂定为 `ModelGenerationPanel` 页面、相关状态/错误/确认框和本页模型库，不扩展到 OrcaSlicer 其他已有页面。

## 2026-08-11 阶段 22 最终发现
- “3D 生成”移动到“准备”之前后，生成页可能成为应用首次使用 OpenGL 的入口。轻量预览不能假设主 `GLCanvas3D` 已初始化着色器，必须在绑定共享上下文后调用幂等的 `GUI_App::init_opengl()`。
- 多个 `wxGLCanvas` 共用上下文时也会共享 framebuffer、scissor、blend、color-mask 和 depth-mask 等状态。轻量预览每帧绑定默认 framebuffer 并恢复最小渲染状态，避免被主画布状态污染。
- 模型 ready 后先下载并用 Orca OBJ 解析器构建只读 `GLModel`，停留在生成页；只有用户点击“确认并生成 G-code”才执行原有导入、保色修复、摆放和切片。
- 15,916 面真实彩色 OBJ 在生成页显示为 40.1 x 39.9 x 100.0 mm、3 个颜色分组；拖动旋转和滚轮缩放均有可见变化。
- 200% DPI 下自动截图必须启用 Per-Monitor DPI awareness，否则截图只包含物理画面的左半边，会把位于右侧画布中心的模型误判为空。
- 最终 GUI 证据为 `generated_models/gui-validation-phase22/15-model-preview-physical-pixels.png`、`16-model-preview-rotated-zoomed.png` 和 `17-gcode-after-confirmation.png`。

## 2026-08-11 阶段 27：手动导入与模型库切片
- 当前自动修复失败路径会撤销首次 OBJ 导入并结束流程，原文件仍在本地，但页面没有进入准备页的显式入口。
- 开放网格不能在失败后直接复用正常成功回调，否则会绕过现有流体门禁并自动开始切片；手动兜底必须使用“只进入准备页”的独立完成结果。
- 模型库双击已经能够加载本地 OBJ、恢复调色板并设置为可导入状态；缺口主要是按钮仍显示新生成模型的“确认并生成 G-code”，用户无法明确判断历史模型可以继续切片。
- 生成面板完成回调目前没有结果参数，主窗口始终触发切片并切换预览页；需要扩展为携带 `slice` 标志。
- 设计文档：`Docs/plans/2026-08-11-manual-import-library-slicing-design.md`。

## 2026-08-11 阶段 28：模型导入颜色策略
- 当前关闭“使用可打印颜色”时传入空的 `ObjImportColorFn`，这会跳过 Orca 原生 OBJ 颜色对话框并强制得到单色模型；用户截图中的“按当前单一耗材打印”由此产生。
- `Plater::load_files` 在未提供颜色回调时会复用 `ObjColorDialog`，这是正常 OBJ 导入及手动颜色映射入口。
- 生成色板约束和 OBJ 导入策略是两个独立决策：前者控制 AI 输出，后者控制 Orca 是保留/映射颜色还是显式单色。
- 自动映射回调即使未应用颜色，OBJ 几何仍可能已经成功导入；该情况应保留模型、停止自动切片并转到准备页手动上色，而不是撤销并报导入失败。
- 设计文档：`Docs/plans/2026-08-11-model-import-color-mode-design.md`。

## 2026-08-11 阶段 28 最终发现
- 默认“正常导入（保留颜色）”通过空 `std::function` 进入 Orca 原生 OBJ 颜色流程；显式“单色导入”才传入空操作回调忽略 OBJ 颜色。
- 可打印颜色自动映射未应用任何颜色时，模型保留在准备页且不自动切片，用户可继续使用 Orca 手动上色。
- 单色历史模型最初能够导出 G-code，但 `DynamicPrintConfig::normalize_fdm_2()` 随后把 `enable_prime_tower` 从 true 改为 false，导致刚生成的切片状态从 1 失效为 0。
- `enable_prime_tower` 的有效值来自当前打印预设，不是 `project_config`；必须在自动切片前更新 `prints.get_edited_preset().config`，再同步预设/项目脏状态与 Plater 完整配置。
- 修复后真实模型库单色流程的日志只有 `0 -> 1`，没有 `1 -> 0`；最终截图为 `generated_models/gui-validation-phase28/11-library-single-gcode-valid.png`。

## 2026-08-11 可打印颜色可选模式
- 当前色板不仅存在于 UI，还同时参与提示词、AI 图片量化、OBJ 贴图烘焙、顶点色校验和 Orca 多耗材映射；只隐藏色板控件不会改变实际效果。
- 关闭色板约束后仍需保留 OBJ 顶点色供生成页 3D 预览，但不能把任意自然色错误声明为打印机可用颜色。
- 因此关闭模式的打印语义应为：自然色图片与模型预览，导入 Orca 后按普通单耗材对象切片；开启模式继续执行严格色板和多耗材映射。
- sidecar 以空数组 `palette: []` 表示自然色模式；图片预览跳过严格色板量化，OBJ 仍烘焙为顶点色，并使用最多 216 个确定性自然色阶控制文件与预览复杂度。
- 生成页在空色板时从 OBJ 实际顶点色提取最多 64 个显示色组，避免为每个细微颜色创建独立 OpenGL draw call。
- 颜色模式与色板一起被冻结到任务状态；用户在预览后切换开关会使结果失效并要求重新生成，避免自然色预览与耗材色 OBJ 混用。
- 关闭模式向 Orca 传入显式空操作 OBJ 颜色回调，从而跳过普通导入颜色对话框和多耗材涂色；模型仍保留自然色文件与 3D 预览，但 G-code 明确按当前单耗材生成。

## 2026-08-11 正式图片预处理失败与准备页卡顿
- 用户截图中的“Could not connect to the preprocessing service”来自正式 sidecar 到外部 OpenAI-compatible 图片服务的连接阶段，不是 Orca GUI 到本地 sidecar 的连接失败。
- 截图仍显示“正在生成 AI 处理图”占位是失败态文案没有完全覆盖进行中提示，错误反馈需要同时更新状态、结果摘要和图片占位。
- 后续验收固定使用 `127.0.0.1:18764` 正式 sidecar，不再启动或依赖 `18765` mock。
# 2026-08-11 高精度 OBJ 与颜色分组发现

- 当前 `tools/ai/tripo_client.py` 固定发送 `smart_low_poly=true`、`face_limit=12000` 和标准几何质量；这是模型细节不足的直接原因。
- `tools/ai/orca_ai_sidecar.py` 又以 `MAX_MODEL_FACES=20000` 拒绝更高面数产物，因此 provider 与本地门禁都必须同时调整。
- 已抽查现有产物 `orcaslicer-ai-4ae4d7e9-f511-4c39-8e93-fd181698eb70.obj`：7,241 个顶点全部携带 RGB，问题不是“没有顶点色”。
- 自然色烘焙当前固定量化到 6x6x6 RGB 色立方体，并把同一位置顶点在所有 UV/材质角上的颜色做多数投票；这会损失渐变、UV 接缝、小色块和材质边界。
- 当前纹理烘焙输出丢弃原 OBJ 的 `o/g/usemtl` 结构，且拓扑门禁强制单连通。这与用户最新确认的“允许多个对象/部件并保留颜色结构”相冲突。
- 进一步核对 Orca OBJ 载入器后确认：按 `(位置, UV, 材质)` 拆点会把 UV 接缝变成拓扑开放边。兼容 OBJ 因此继续复用几何位置索引，但自然色改为全精度 RGB 累积平均；原始 OBJ/MTL/纹理和 `o/g` 分组仍完整保留，未来导入器可直接使用逐角颜色。
- Windows 200% DPI 的正式 Orca 页面验收确认：`模型精度` 控件位于风格与选图之间，默认 `30 万面（推荐）`，未与周边控件重叠。
- 展开下拉列表后四档 `10 万面（较快）`、`30 万面（推荐）`、`50 万面（精细）`、`100 万面（最高）` 均完整可见；证据为 `generated_models/gui-validation-phase29/04-quality-options.png`。

## 2026-08-11 质量优先付费预览发现

- Q 版真实预览 `c4f5af5d-88d3-4d05-ba53-3ccdb775e07d/preview.png` 没有新增人物、道具、底座、文字或背景装饰，保留黑发、肤色、白外套、绿色内搭、交叉手臂和手表；构图比原图放大，下半身可见范围减少。
- 低多边形真实预览 `phase30-paid-style-previews/low_poly/preview.png` 保留主要身份、姿态、色块和手表，面片清晰且没有新增支撑物；同样存在构图放大。
- 雕塑真实预览 `phase30-paid-style-previews/sculpture/preview.png` 保留身份、服装和姿态，没有添加基座；单色石材外观符合风格，但不适合作为彩色 OBJ 链路的首个验证输入。
- Q 版具有最连续的色块、最少脆弱尖角和最强打印友好轮廓，作为首个 30 万面彩色 3D 输入；低多边形作为几何风格对照，雕塑作为单色风格对照。
- 一次低多边形 sidecar 图片任务在外部预处理期间遇到服务进程被 Orca 管理器接管，内存 job 丢失且只留下输入文件；后续付费图片验证改为单进程保护脚本，已有输出时拒绝重复调用。
- 首个 Q 版 30 万面 Tripo 任务 `d960d74e-4801-4dd5-9d1f-af42982653b9` 实际产出 296,642 个三角面、148,323 个顶点，单连通，边界边/非流体边/退化面均为 0；本地修复状态为 `not_needed`。
- 自然色 OBJ `model-vertex-color.obj` 为 15,474,154 字节，所有 148,323 个引用顶点均携带 RGB；原始 ZIP、OBJ、MTL 和 826,419 字节底色贴图均保留在任务目录。
- 六步长期侧栏只由 AI 导入流程启动，并只在该流程活跃时消费切片完成事件；完成后面板保持显示但状态转为非活跃，因此后续普通切片不会污染黄金路径结果。
- 自动摆放状态必须等待 `ModelGenerationPanel` 的同步导入回调返回后再标记成功；切片状态随后进入进行中，避免主窗口尚未更新时侧栏提前宣布完成。
- 完全关闭 MSBuild 和编译器内部并行后，新增侧栏代码通过 `libslic3r_gui` 与完整 `OrcaSlicer` Release 构建，说明此前 `C1060` 为内存压力而非代码诊断。
- 当前正式 Orca PID `61376` 正占用日常运行 DLL，且 `LockApp`/`LogonUI` 证明桌面锁定；在用户解锁并正常关闭前不能证明新侧栏已由正式程序加载，GUI 验收必须继续保持未完成状态。
- 旧 Orca 正常退出后，新 DLL 已部署并由正式 Orca PID `32408` 加载；production sidecar v4 同时恢复，能力协商仍为自然色 OBJ 与 10/30/50/100 万面四档。
- 模型库目录中的 `preview.png` 是风格输入图封面，不是 3D 渲染，不能用它证明图到模型的一致性；必须使用 Tripo rendered image 或直接渲染最终 OBJ。
- 最终 OBJ 全三角面顶点色渲染与风格图并排复核后，人物身份、Q 版比例、发型、面部色块、服装层次、交叉手臂和手表均保留；3D 化的主要损失是细纹理平滑和肩部轻微加宽，不存在额外人物、道具或底座。
- OBJ 的 Z 范围为 0-100 mm，X/Y 范围约 40.0 x 43.2 mm；下装底部是输入图本身的画面截断，不是 Tripo 缩略图裁切或 OBJ 导出丢失。
- 2026-08-12 阶段 30 GUI 续验发现：模型库列表只在 `ModelGenerationPanel::load_library_entries()` 执行时扫描磁盘，页签切换仅重绘，不会重新扫描。当前界面首项 `AI 模型 a8a1eb8a` 已无对应磁盘文件，属于 Orca 启动时缓存的旧条目；按当前磁盘与代码的实际排序算法（优先元数据 `generated_at`，否则 OBJ `last_write_time`），`d960d74e-4801-4dd5-9d1f-af42982653b9` 是有效模型中的首个正式高质量条目，仅位于故障路径夹具 `phase28-no-colour` 之后。
- 2026-08-12 阶段 30 首次真实 GUI 导入确认 `d960d74e...` 在模型库预览中为 296,642 面、22 个显示颜色组、40.0 x 43.2 x 100.0 mm；OpenGL 日志为 22 groups、2118x1015 viewport、shader 28、`gl_error=0`。标准 OBJ 导入器仍弹出“OBJ 文件导入颜色”二次确认，说明“正常导入（保留颜色）”此前没有提供 `ObjImportColorFn`。
- 同一次导入中，模型导入、封闭网格检查、自然颜色处理和自动摆放均成功。首次切片在 G-code 导出阶段因 `independent_support_layer_height` 配置应用而被 Orca 内部取消，随后自动重切成功并出现 `Exporting G-code finished` 与 `on_process_completed:finished`；旧六步状态机却在第一次内部取消事件上永久结束为失败。修复策略是仅对活跃 AI 流程且 `background_process.is_internal_cancelled()` 的取消事件保持切片为运行状态，等待替代切片的最终事件。
- 延迟确认不能用堆上临时 `wxTimer` 在自身事件回调内 `delete`；真实复跑在更新第 6 步时发生访问冲突。由 `Plater::priv` 持有计时器、随窗口生命周期销毁并通过既有统一事件分发后，完成回调不再崩溃。
- `PartPlate::is_slice_result_valid()` 只说明切片结果存在，不代表可打印；`is_slice_result_ready_for_export()` 还会检查 `toolpath_outside`、G-code 错误和耗材可打印性，适合作为 AI 六步流程最终成功门禁。
- 当前 WonderMaker 多色预设的擦料塔坐标可超出热床；自动流程不能在这种结果上显示成功。关闭 AI 自动切片的擦料塔后仍保留 OBJ 的 22 个颜色组和耗材映射，同时消除热床外刀路。
- 阶段 30 最终真实流程生成 75,846,512 字节 G-code，Viewer 完成映射，打印按钮为可用状态；完成后的 `Print::apply` 返回 `apply_status=0`、`invalidated=0`，没有后续 `1 -> 0`。
- Windows 在窗口最大化/恢复后会延迟重绘硬件加速 OpenGL 子窗口；`PrintWindow` 也不捕获 GL 像素。刀路验收需结合实际屏幕截图、`GCodeWindow::load_gcode` 日志、G-code 文件大小和可导出门禁，不能只看 `PrintWindow` 灰色画布。
- 2026-08-12 的最终空项目复跑证明此前刀路冲突来自旧项目中两个重叠模型，而不是正式 OBJ 本身：干净日志段只有 1 个模型对象，`gcode path conflicts check takes 0 secs` 后没有 `gcode path conflicts found`，并成功产生 38,127,361 字节 G-code。
- 单模型最终状态在 12:46:07 从 `0 -> 1`，完成回调后保持 `invalidated=0`，没有后续反向失效、热床外刀路或崩溃；截图 `generated_models/gui-validation-phase30/36-clean-single-model-gcode-hwnd.png` 给出六步状态、单个缩略图、实际刀路数据和可打印入口的同屏证据。
- 2026-08-12 用户复测“正常导入”只有一个颜色。正式 `model-vertex-color.obj` 的 148,323 个顶点全部携带 RGB，生成页 3D 预览仍显示 22 个自然颜色组，因此源颜色没有丢失；但该 OBJ 实际只有 1 个 `o`、1 个 `g`，没有 `usemtl/mtllib`，说明本次 Tripo 产物本身没有可供 Orca 拆分的多对象/材质结构。
- “一个对象”和“一个颜色”是两个独立问题。当前 `ModelGenerationPanel.cpp` 将 `ImportColorMode::Normal` 与 `AutoMap` 一起交给 `make_ai_obj_color_mapper()`，两者都会把自然顶点 RGB 映射到 `compatible_project_slots()`；兼容槽位只剩一种或颜色接近时，正常导入会被压成单一耗材色。这与 UI 文案“正常导入（保留颜色）”不一致，是导入实现缺陷，而不是单对象直接导致颜色只能为一种。
- 正常导入的 4 色 MMU 面片和 G-code 已验证有效，但首次主色同步把 `preset_bundle->full_config()` 传给 `Plater::on_config_change()`；完整配置构造过程会在项目配置之后重新应用耗材预设，从而覆盖刚写入的 `filament_colour`。因此差异检测看不到 OBJ 主色，左侧色块仍显示旧的红、绿、青、白。应直接传播 `project_config` 并刷新耗材控件。
- Orca 自身的耗材颜色编辑路径同样用包含 `filament_colour` 的项目配置调用 `Plater::on_config_change()`；该函数随后把项目颜色同步到 Plater 完整配置，并刷新对象列表与动态耗材栏。阶段 31 最终实现沿用这条原生路径，并额外刷新耗材下拉控件、标记项目已修改，不改变 MMU 面片映射或对象结构。
- 2026-08-12 阶段 32：`ModelGenerationPanel` 的导入完成回调已经通过布尔参数区分自动切片和进入准备页，因此新增选项不需要改动 MainFrame 或切片核心。开关只参与最终 `slice_after_import` 门禁，既有手动修复和手动上色门禁保持优先；自动切片关闭时，擦料塔与独立支撑层高等自动流程专用配置不会被修改。
- 阶段 32 GUI 实测证明关闭自动切片后，模型导入、封闭网格检查、自然颜色映射和自动摆放仍完成；切片与 G-code 两步保持等待，Orca 停在准备页且没有可打印结果。开关开启路径保持原有 `m_on_import_succeeded(true)` 行为，默认值为开启，因此不改变既有自动流程。
- `start_orcaslicer_with_ai.bat` 始终启动 `build/OrcaSlicer/orca-slicer.exe`，该程序加载同目录 `OrcaSlicer.dll`；仅构建 `build/src/Release/OrcaSlicer.dll` 或更新隔离验证目录不会影响正式启动结果。每次 Release 构建后必须在 Orca 正常退出时同步 DLL，并比较构建目录与运行目录哈希。
# 2026-08-12 阶段 33：导入颜色与真实耗材匹配

- 用户截图中 AI 流程显示“已保留并映射自然颜色”，但准备页模型为灰色、对象行只显示耗材 1，且打印机实际耗材色与模型色不匹配。
- 根因是 AI “正常导入”主动传入 `make_ai_obj_color_mapper(..., preserve_source_palette=true)`，绕过 Orca 原生 `ObjColorDialog`，并把聚类出的模型主色写回 `project_config.filament_colour`。这会把模型色当成打印机实际耗材色，界面成功状态不能证明物理颜色匹配正确。
- 修复决策：导入策略显式拆成手动匹配、自动匹配当前耗材、单色导入；默认手动匹配并复用原生 `ObjColorDialog`。取消或只映射出单一耗材时不自动切片；任何模式都不再用模型自然色覆盖打印机耗材色块。
- 原生 `ObjColorDialog` 在打开时已计算颜色聚类并生成 MMU 分色预览；确认后 `filament_ids` 保留，取消则清空并恢复耗材 1。因此 AI 回调可以用确认后的唯一耗材 ID 数量可靠区分成功、多色退化和取消。
- 正式 Orca 当前存在未保存项目且占用日常 DLL；隔离副本也受全局单实例机制限制。为保护用户状态，本阶段不会强制关闭窗口或替换正在加载的 DLL。
# 2026-08-13 阶段 35：正式模型生成任务恢复审计

- 正式 sidecar 原先只将 `attempts.json`、预览和 OBJ 落盘，`_JOBS` 完全驻留内存；sidecar 重启后 GUI 的旧 job ID 必然返回 `Model job not found`。
- 只在 GUI 保存 job ID 无法解决服务端恢复，也无法安全判断收费请求是否已经创建；只把中断任务标失败则会浪费已付费且仍可继续查询的 Tripo 任务。
- 最小正式闭环需要把原始文字/图片、风格、冻结色板、质量档、prepared prompt、generation ID、conversion ID、过程状态和本地文件引用原子写入每个生成目录的 `job.json`。
- 启动恢复按 task ID 是否已经可靠落盘分流：存在 generation ID 时只续轮询/转换/下载；不存在时标记失败并要求手动重新生成，绝不猜测性重复付费。
- GUI 通过 loopback-only `/v1/orcaslicer/model-jobs/latest` 恢复最近活跃任务；图片任务必须串行下载原始参考图与风格预览，因为现有客户端同一时间只持有一个 HTTP 请求。
- 删除任务只删除 `job.json`/内存索引，原图、预览、远端响应、OBJ 和诊断仍作为用户过程产物保留。
- 新增恢复测试覆盖：状态往返、最近任务接口、输入/色板/质量恢复、模糊状态拒绝续跑、已有远端 ID 零次 `create_*_task`、删除后不复活。
- 持久化首轮 AI 全量回归为 80/80；完成真实恢复缺陷修复后的最终回归为 90/90。Windows Release `OrcaSlicer.dll` 于 2026-08-13 22:29:03 完成链接。
- 真实收费验证任务 `8267ded0-c96c-4c46-b263-cdc92d49891d` 只创建了一个 Tripo generation ID `c3e2c96a-2be8-411a-ab4f-24f7bcdff20f` 和一个 conversion ID `b740163c-c7e6-48be-9937-feb7504e88f2`；sidecar 多次重启后 ID 与尝试次数均未变化，证明恢复不会重复创建付费任务。
- 本机代理会把 Tripo 官方 CDN 映射到保留地址。产物 URL 校验现在先严格匹配 HTTPS 官方主机 `openapi.cdn.tripo3d.com`，相似域名和 HTTP 仍在 DNS/下载前被拒绝，避免为了兼容代理而放宽任意地址访问。
- Tripo 的 `face_limit` 是目标值而非精确保证值。真实 10 万面任务返回 95,338 面；验收改为每个档位各自目标的 90%–125%，因此 30 万面档仍要求至少 27 万面，不会退化为全局 9 万面门槛。
- 恢复流程必须对本地转换产物幂等：若 `attempt-XX/model-vertex-color.obj` 已完整生成，先重新校验并复用；若损坏，再在新的 `recovery-XX` 目录下载，不能向已有 ZIP 解压目录重复写入。
- 最终真实 OBJ 为 4,822,151 字节、47,671 顶点、95,338 三角面、2 个连通部件、0 个异常边，顶点 RGB 校验通过；产物接口下载结果与磁盘文件 SHA256 `CE8244A6B515E08CF12FD8D8D42A41C06C5C6717DA8B8F85052B0B33BB2FCDEE` 一致。
- 正式恢复前，真实任务位于隔离根 `generated_models/formal-recovery-validation/`，而生产 sidecar 只扫描 `generated_models/` 的直接子目录。保留隔离原件并复制任务到正式根目录后，`latest` 正确返回该 `ready` 任务；没有触发任何远端请求。
- 恢复成功时必须清空旧尝试的 `error` 字段，否则 `status=accepted` 与旧失败文本并存会误导审计。正式代码与回归已覆盖该状态归一化。
- 正式 Orca 重启后，生成页成功恢复原文字、Q 版卡通、10 万面档、自然颜色、95,338 面/21 个显示颜色和可旋转彩色 3D 预览；切到准备页再返回后预览保持。
- 模型库首项显示本次真实红熊猫 OBJ（4.6 MB、自然颜色），旧历史模型仍完整显示，证明正式任务恢复与持久模型库可以共存。

# 2026-08-14 阶段 36：双主线解耦架构审计

- 当前 AI 主链路集中度很高：`ModelGenerationPanel.cpp` 约 2,612 行，`orca_ai_sidecar.py` 约 2,496 行；两者已经同时承担界面、状态机、任务恢复、供应商调用、OBJ 处理和流程编排等多类职责。
- C++ 侧已有 `AIServiceManager`、`AIModelGenerationClient` 和 `AISidecarClient` 雏形，但尚未形成模型生成域、智能切片域和 Orca 适配层的明确目录/接口边界。
- 当前工作树存在大量跨 Orca 核心与 GUI 文件的未提交改动；架构迁移必须采用增量抽取和兼容门面，不能一次性搬迁或重写，否则难以区分用户现有改动和新架构改动。
- `ModelGenerationPanel` 直接持有 `Plater*`，并在 `import_local_artifact()` 内完成 OBJ 导入、颜色策略、网格检查/修复、自动摆盘、切片触发和六步 UI 状态更新。这是当前模型生成与智能切片最主要的反向耦合点。
- 智能流程状态又直接写入 Orca 高频文件 `Plater.cpp/.hpp`：侧栏 UI、稳定计时器、内部取消识别、切片完成回调和最终可打印门禁均散落其中。上游升级发生冲突的概率较高。
- `MainFrame` 的直接集成相对较薄：创建生成页、注册一个 `bool slice` 回调、发现 sidecar 能力。该位置适合保留为 Composition Root，但不应继续承载域逻辑。
- `AIModelGenerationClient` 已把模型任务 HTTP 契约从 wxPanel 中分出，是可继续演进的基础；但它仍是单活跃请求、GUI 命名空间内的具体客户端，尚缺独立领域模型和可替换接口。
- 当前 Orca 核心改动规模集中在 `ModelGenerationPanel`（新增约 2,258 行）与 `Plater`（新增约 203 行）；`libslic3r` 只涉及少量颜色/布尔修复变更。解耦应优先从 GUI 编排层抽取，而不是重写切片内核。
- Python sidecar 同样是高耦合单体：`tools/ai/orca_ai_sidecar.py` 约 2,496 行，同时承担 HTTP 路由、付费任务持久化、图片预处理、调色板、OBJ/MTL/纹理处理、网格修复与切片参数建议。适合保留为一个本地进程，但内部按模型生成、智能切片建议、制品处理、供应商适配拆成模块。
- `tools/ai/tripo_client.py` 已形成供应商传输边界，可继续收敛为模型供应商适配器；当前 `/v1/orcaslicer/...` 路由应保留为兼容门面，避免 C++、打包脚本和已发演示包一次性迁移。
- 仓库已有 `Docs/architecture/` 架构资料，但当前实现已经新增真实模型生成、OBJ 颜色/修复、模型库与自动切片流程。本轮应增补“双主线模块边界与协作规则”，避免另建一套互相冲突的架构说明。
- 当前 AI 源文件仅在 `src/slic3r/CMakeLists.txt` 集中登记，迁移到新目录只需要控制一个构建接入点；这有利于新增模块而尽量不改上游高频文件。
- 当前仓库已经配置 `origin`（团队 fork）与 `upstream`（OrcaSlicer 官方），具备持续同步上游的基础；当前分支为 `master@a1ef7204fe`，工作树改动很多，因此应先做结构化的小步抽取，不能把“架构迁移”和“上游升级”放进同一个合并请求。
- 两处 `libslic3r` 修改性质不同：`Model.cpp` 的顶点色映射修复是 OBJ 通用导入兼容问题；`MeshBoolean.cpp` 的 CGAL 开边修复是通用算法变化，风险显著更高。后者应独立成上游补丁、带专门回归，不应成为智能切片模块的隐式依赖。
- 现有 `Docs/architecture/03` 与 `04` 已提出 Provider 无关、GeneratedModelImporter、Preflight、Repair、Trial Slice 等方向；本轮不推翻它们，而是补充当前真实代码到目标模块的迁移映射、双人目录所有权、单向制品契约与上游补丁预算。
- 当前“智能切片”尚不是完整闭环：`AIAssistantConfig` 已有 26 个左右白名单参数、类型/范围校验和上下文构造，`AIAssistantPanel` 能人工勾选后写入 Print/Filament tab 并重切片；但仍直接依赖 `Plater`、`GUI_App`、`Tab`，没有模型 Preflight、隔离试切、baseline/candidate 指标比较、事务式应用与独立状态机。
- 因此两条主线的交接不能是“模型生成模块直接调用 Plater 切片”。正确边界应是模型生成只产出不可变 `GeneratedModelArtifact`，用户确认后由 UI/应用壳显式调用智能切片用例；智能切片只消费制品和当前 Orca 工作区快照，不依赖 Tripo/OpenAI 任务对象。

# 2026-08-14 阶段 37：模型生成线与 Orca 第一批等价解耦

- 用户确认负责模型生成主线，并要求同时完成与 Orca 的解耦；第一批按已确认架构做等价抽取，不增加新产品行为。
- `ModelGenerationPanel` 对 Orca 的直接依赖可分成两类：读取当前耗材颜色/温度兼容槽位，以及消费生成后的 OBJ。前者应抽为只读 `IPrintablePaletteProvider`，后者应抽为 `IModelArtifactConsumer`，二者由 `OrcaWorkspaceAdapter` 实现。
- 当前 `import_local_artifact()` 的同步部分可以通过 typed request/result 迁出：输入为 artifact、颜色策略、是否自动切片；输出至少区分导入失败、修复取消、手动修复、手动上色、映射退化和是否启动切片。Panel 继续负责生成任务清理和文案投影。
- MainFrame 当前已有导航/切片回调，适合继续作为组合根：构造 `OrcaWorkspaceAdapter`，将窄端口传给生成面板；不把导入算法搬进 MainFrame。
- `valid_project_slots()`、`compatible_project_slots()`、`project_palette()` 属于 Orca 打印上下文读取，不属于模型生成 UI；迁入 adapter 后可同时服务生成前色板约束和导入时自动颜色映射。
- `wxGetApp()` 在 Panel 中剩余的大部分调用属于 GUI 调度/OpenGL 渲染，可在本批保留；解耦目标是移除 Model/Print/Preset/Plater/CGAL 等 Orca 业务依赖，不是移除 wxWidgets。
- 第一批实现后，`ModelGenerationPanel` 的头文件和实现只通过 `IPrintablePaletteProvider` 读取 Orca 打印上下文、通过 `IModelArtifactConsumer` 提交不可变模型制品；静态搜索不再出现 `Plater`、`Sidebar`、`ObjColorDialog`、Preset 或 CGAL 修复依赖。
- `OrcaWorkspaceAdapter` 是当前唯一理解 OBJ 原生颜色对话框、项目耗材槽、snapshot/undo、网格修复、准备页导航和自动切片协调的反腐层；这些 Orca 高频演进点不再散落在模型生成页面。
- `MainFrame` 只在组合根创建一个 adapter，并把两个窄端口注入 Panel；颜色映射、网格诊断和切片条件没有进入 MainFrame。
- 本批仍有意保留 adapter 对现有 Sidebar 六步展示及 MainFrame 导航回调的兼容调用。下一批可把流程展示抽为独立 observer，但不应在尚未建立智能切片应用服务前提前删除现有行为。
- MSBuild 直接调用 `_Lib` 和 `_Link` 可在不重编 338 个未改动 GUI 文件的情况下完成真实静态库归档与 DLL 链接；这适合本轮等价迁移验证，但正式发布包仍应走标准完整构建流水线。
- 当前 CMake 构建树未生成 CTest 测试目标（`Total Tests: 0`），因此本轮 C++ 结论来自三个改动翻译单元编译、完整库归档、DLL 链接和静态边界扫描，不声称执行了不存在的 C++ 单测。

# 2026-08-14 阶段 38：双机协作 Git 基线审核

- 当前 `master@a1ef7204fe` 与 `origin/master` 一致，但有 27 个已跟踪文件修改和约 30,276 个未跟踪文件；未跟踪数量主要由 `output/` 的测试包展开目录造成，不能直接使用 `git add .`。
- 最大本地目录包括 `build/` 约 8.97 GB、`.git/` 约 2.97 GB、`output/` 约 1.27 GB、`generated_models/` 约 1.09 GB；其中构建、输出和真实生成产物都不应进入共享源码基线。
- `.gitignore` 已忽略 `build*`、Python bytecode、`.claude/upstream-orcaslicer/`、`.workbuddy/` 和 `generated_models/`，但尚未覆盖 `.codex-recovery/`、`output/`、`projects/`、`website/` 等本机过程目录，需要按目录内容审计后补齐。
- 规划文件原始字节前缀是正常 UTF-8；工具输出中的 `ä»»...` 是显示层解码问题，不是磁盘内容损坏。
- `.claude/settings.local.json` 是已跟踪的本机设置修改，原则上不得纳入团队基线；需要通过精确 staging 排除，不能为了清洁工作树而覆盖用户本地配置。
- 安全扫描只命中 `tools/ai/test_sidecar_contract.py` 中的 `test-openai` / `test-tripo` 固定假值；打包模板的两个 Key 为空，未发现真实 API 密钥。个人绝对路径只出现在本地 `.codex-recovery` 和历史规划记录。
- `website/` 自带独立 `.git`，属于 3dprint.beer 的另一个仓库；`projects/` 是 PPT 工程，二者都不能作为 Orca 未跟踪目录被纳入本次提交。
- `packaging/windows-ai-test/` 只有 8 个小型模板文件，Key 为空；`scripts/package_windows_ai_test.ps1` 以白名单复制正式 sidecar 文件，明确不携带 Mock、付费验证脚本和历史模型，属于可复现发布源码，应保留。
- `run_paid_style_preview_validation.py` 与 `run_paid_tripo_validation.py` 均要求显式 `--confirm-paid-call`；后者还持久化 task ID 并拒绝模糊状态下重复创建。它们可作为受控诊断工具提交，但不会被正式启动或打包路径调用。
- `MeshBoolean.cpp` 的 49 行变化位于通用 CGAL repair 内核，风险显著高于 AI GUI；当前真实自动修复依赖其逐边界重提取、填洞和闭合检查，不能无验证删除，适合在同一共享基线中保留但作为独立 commit 说明。
- `Plater.cpp/.hpp` 的新增主要分为三块：OBJ 自定义颜色回调端口、六步流程兼容 UI、切片完成稳定性门禁。前两者被 `OrcaWorkspaceAdapter` 直接消费，最后一块修复实际异步重切/预览完成竞态，均有现有真实流程依据，不是可直接删除的死代码。
- 根目录两张 PNG 是旧架构/启动恢复图且没有被文档引用，不应进入基线；`Docs/开发进展.xlsx` 只有约 12 KB，并被正式状态文档引用，可与 `Docs/AI能力状态与实施计划.md` 一起保留。
- Sidecar ZIP 解包没有调用 `extractall`：逐项拒绝绝对路径、`..`、重复路径、符号链接、特殊文件、加密包、文件数和解压总量超限，再以 `xb` 创建目标，已覆盖典型 Zip Slip/Zip Bomb 风险。
- Sidecar 只允许绑定 `127.0.0.1`、`localhost` 或 `::1`；C++ 客户端也拒绝非 loopback endpoint。Tripo 下载固定为精确 HTTPS 官方 CDN allowlist，重定向再次校验，并限制下载大小。
- Python AST 审计未发现模块级重复函数；报告的 `find/unite` 是两个局部并查集实现，`redirect_request` 是两个不同安全重定向类的方法。Sidecar 仍有 268 行拓扑修复等大函数，但本轮拆分会显著扩大回归面，应作为后续模型生成内部模块化任务，不在共享基线发布前重写。
- `scripts/create_windows_ai_distribution.ps1` 固定引用旧 `demo3` ZIP 和输出名，只是一次性外发包装器，已由通用 `package_windows_ai_test.ps1` 替代；前者不提交，后者保留。
- 原 `packaging/windows-ai-test/setup/ai-config.bat` 虽为空，但一旦开发者本地填写真实 Key 就会形成 tracked secret 风险。基线改存 `ai-config.example.bat`，通用打包脚本在输出目录复制生成真正的 `ai-config.bat`。
- 根 README 的协作说明固定共享基线、两条主线目录所有权、Orca adapter 共享边界、个人分支/PR/上游同步流程和禁止提交的本地数据，可作为另一台电脑 Codex 的首要入口。
- 共享基线最终使用 `codex/ai-integration-20260814`，不移动 `master`；核心 OBJ、核心 Mesh、正式 AI 和协作文档拆成独立 commit，便于同事审查及后续 Orca 上游冲突处理。
- Git 提交后工作树干净；被移除跟踪的 `.claude/settings.local.json` 仍保留本机并由 `.gitignore` 保护，大体积输出、模型、网站和 PPT 工程同样未删除且未上传。
# 阶段 39：四色可打印图像链路审计

- `Docs/FOUR_COLOR_IMAGE_PIPELINE.md` 明确要求三层约束：AI 构图约束、确定性固定色板量化、面向 FDM 的区域清理与输出；提示词限色不能作为最终保证。
- 当前正式 Sidecar 已有 `_quantize_image_to_palette`，实现 Lab 欧氏近似映射、无抖动和简单 3×3 众数滤波，但算法内嵌在约 2700 行 Sidecar 中，缺少按毫米推导最小像素、邻接小区合并、孔洞处理、热力图、蒙版、评分和完整元数据。
- 当前图片输入会保存 `style-preview-raw.png` 和覆盖后的 `preview.png`，但 API/GUI 只暴露最终 preview；用户无法比较 AI 原图、严格四色图与可打印清理图。
- 当前文字输入只整理提示词并直接走 Tripo 文生 3D，没有先生成四色图片，不符合文档定义的文生图四色流程。
- GUI 已有“使用可打印颜色”、项目/手动色板和预览对照，可在不侵入智能切片模块的前提下扩展打印参数与图片阶段。
- 推荐实现：新增独立 `printable_image_pipeline.py`，Sidecar 负责调用与持久化，C++ 客户端只传递打印配置并下载已声明产物；默认实体色块且保留关闭开关后的旧路径。
- 已按上述边界完成实现；旧 `_quantize_image_to_palette` 暂只为历史回归测试保留，正式任务已切换到独立管线，后续可在清理旧测试时一并删除，当前不承担新业务职责。
- 真实 Image2 样例的严格图和清理图均只有指定四种颜色；清理把区域总数从 2666 降为 132，把小区域从 2523 降为 1，变更像素约 0.4647%，主体轮廓和主要机械细节仍清晰。
- 方案文档中的多候选自动排序、HueForge TD、SVG/STL/3MF 浮雕导出和真实切片耗材统计属于后续阶段；本轮完成的是实体色块 MVP，并提前实现了热力图、评分和主体/背景遮罩。

# 阶段 40：正式 Orca GUI 验收发现

- 日常运行实例仍加载 2026-08-13 的旧 DLL；最新 DLL 不能直接覆盖正在运行的文件，因此建立了 `generated_models/gui-validation-phase40/runtime/` 隔离副本，源码/副本 DLL SHA256 均为 `16208AC274C424ECADD73A68FA6FA4836CF0C1ADBD26C72938811530506CA30C`。
- 正式 Sidecar 已从旧进程重启到当前源码，`/health` 明确返回 `printable_image_pipeline.available=true`，包含 raw/strict/clean/heatmap/masks/metadata 输出能力，仍使用 `https://laotie.dev`，未使用 mock。
- 隔离实例第一次加载最新 DLL 时在 `wxEvtHandler::DoBind` / `Plater::priv::priv` 崩溃。源文件时间戳显示 `Plater.obj`（8 月 12 日）早于 `MainFrame.hpp`/`MainFrame.obj`（8 月 14 日）；此前只编译 `ModelGenerationPanel.cpp` 和客户端并直接归档，导致依赖 `MainFrame` 类布局的旧对象进入新 DLL。需要串行执行完整 GUI 增量编译，不能把“两个修改文件单编译 + 链接”作为可启动性证明。
- 新增日志显示模型生成页面的 `build_page`、`refresh_palette`、`load_library_entries`、事件绑定和构造函数全部完成，预设页也完成创建；因此把崩溃归因于四色页面本身并不成立。
- 本轮修改过 `MainFrame.hpp` 的成员布局，而 Orca GUI 中大量翻译单元直接或间接依赖该头文件。单独补编 `Plater.cpp` 仍不足以保证 ABI 一致，必须让完整 `libslic3r_gui` 编译批次成功结束并更新增量记录。
- MSBuild `ClCompile` 批次若在完成前被终止，即使已生成大量有效 `.obj`，其 `.tlog` 增量记录仍不会提交；再次启动会从文件列表开头重编。因此当前后台全量批次必须自然完成，不能再次用更高并发重启。
- 完整 362 个 GUI 对象重编、归档和链接后，隔离 Orca 可稳定启动；最终验收 DLL 为 72,640,000 字节，SHA256 `FC03AD55F8AEFB35A77A50B4540F082699C14CA746EBB4B35D8D97A4FE7DCBC8`。此前两次启动崩溃确认是旧对象与新类布局混链，不是四色业务逻辑缺陷。
- 正式 GUI 中中文参数、项目红绿蓝白四色、文字输入和付费确认均正常；本轮只产生 1 次 Image2 调用，任务 `def4b6a3-d84b-4e16-b0c0-de9efe5de7a5`，没有调用 Tripo。
- 正式任务状态 GET 路由存在真实缺陷：`_job_route()` 返回 `status`，但 `do_GET()` 白名单未包含它，导致任务创建后轮询 404。加入 `status` 后旧任务可从磁盘恢复到 `awaiting_confirmation`，并新增契约测试防止回归。
- GUI 能切换 `AI 原图 / 严格色板 / 可打印清理 / 问题热力图`，四个 PNG 的 SHA256 均不同，证明不是同一文件换标签。清理后小区域占比约 0.01%，区域数由 2678 降到 146，小区域由 2558 降到 33。
- 从 3D 生成页切到准备页再返回，任务、打印参数、阶段选择和图片保持；只有确认可打印预览后才显示可用的“生成 3D 模型”入口。本轮未点击该按钮，避免无必要的 3D 付费调用。
- Python AI 全量回归为 104/104；`git diff --check` 通过。测试仅有 Pillow 未来版本弃用警告，不影响当前结果。

# 阶段 41：四色预览质量与即时刷新

- 当前用户界面任务为 `a0b0f0db-e097-4eaf-9717-105cfddf8944`，输入是一张彩色 Q 版人物肖像，但任务仍携带上一轮“机械麒麟”文字提示，GUI 风格为“雕塑（适合单色）”。AI 原图因此变成几乎全灰白的单色雕塑，只保留少量低饱和绿色衣服。
- 严格色板与清理图实际只包含 `#0080FF` 和 `#FFFFFF`；`mask_red.png`、`mask_green.png` 仅 1605 字节且无有效区域。任务平均色差 13.7177，说明当前结果即使满足“像素属于色板”，也不满足“四色构图可用”。
- 根因在上游构图与当前质量门禁：最近色量化无法凭空创造灰度原图中不存在的红绿语义区域。应让 Image2 先按耗材颜色生成大块实体色，并在量化后检查主体使用色数、各主色面积下限和平均色差，失败时提示重新生成，而不是把蓝白线稿当作可确认结果。
- GUI 复现表明选择“AI 原图”后下拉标签和画布标题立即变化，但位图保持清理图；点击“+”缩放后位图才变为 AI 原图。`apply_preview_stage()` 已更新 `m_style_preview_image`，缺口位于自绘画布的主动重绘/布局提交。
- 位图延迟的直接根因是 `update_preview_view()` 只在目标尺寸变化时重建 `m_style_preview_bitmap`；不同阶段图片尺寸相同，旧缓存因此被复用。阶段切换时显式清空 bitmap 并同步 `Update()` 后，无需缩放即可立即显示 AI 原图/严格色板/清理图。
- “设计师玩具风”不能只写风格名：需要把最亮色限定为背景/封闭脸部，将红、绿、蓝等非背景色分配给外套、内搭、头发/暗部，并禁止自然肤色、灰黑和白色衣服吞掉主体。
- 第一轮真实 Image2 原图已有玩具质感，但自然肤色与白衣导致清理图主体面积仅 12.51%；新增 `printable_subject_area_ratio >= 18%` 门禁后，此类结果不会进入 3D。
- 第二轮真实原图形成白脸、红外套、绿内搭、蓝头发；旧 32 色 median-cut 因白/红面积占优，把绿与蓝预聚类合并。提高到至少 64 个自适应簇后，四色面积分别约 27.55%、2.37%、9.13%、60.94%，四色均保留。
- 第三轮 Image2 的灰色影棚背景暴露中性灰误映射蓝色的问题；亮中性色先固定映射到最亮耗材色后，背景恢复白色，主体面积约 38.88%，红/绿/蓝/白四色均为有意义区域。
- 最终正式 GUI 中已验证选择“AI 原图”会在同一次选择事件内立即换图；没有点击生成 3D，因此本阶段只有 3 次 Image2 调优调用、0 次 Tripo 调用。

# 阶段 43：设计师玩具效果调优规划发现

- 用户授权验证阶段可强制关闭应用；已精确关闭项目构建目录中的 Orca PID `222500`，未停止 Sidecar 或其他进程。
- `C:\Users\ltj\Pictures\Camera Roll` 当前固定样图为 `方飞总.png`（1080×1620）、`刘亦菲.jpg`（1080×2400）和 `大雁塔.jpg`（1279×1967）。三图分别覆盖半身人物、全身人物和复杂多主体场景。
- `方飞总.png` 的优势是脸部清晰、服装有白/绿大区块，难点是原图只到大腿附近且双臂交叠；若严格禁止补全与重构，只能得到被裁切的半身玩具，不适合作为通用 3D 参考。
- `刘亦菲.jpg` 已有完整站姿和白背景，适合验证人物身份、Q 版比例和稳定底座；长发、织物纹理、首饰、手指和靴筒必须主动几何简化，不能依赖后置四色清理解决。
- `大雁塔.jpg` 同时包含前景佛像、远景塔、密集树木和底部相机水印/色卡。当前“保留背景和所有可见内容”的提示会让主角不明确，必须在生成阶段先选择一个主角并剥离摄影附属内容。
- 当前 `_style_preview_prompt()` 强制保持原 canvas、裁切、背景、主体数量和空间布局，同时禁止补全、移除、重定位或新增底座；这与“完整轮廓、单一主体、稳定底座、干净背景”的 image-to-3D 目标冲突，是本轮最优先的上游问题。
- 现有 `_designer_toy_palette_direction()` 和确定性四色管线已能约束大块耗材色、至少三种有意义颜色、主体面积和背景中性灰映射；它们适合作为第二层打印门禁，不应先于 AI 原图的构图/身份质量判断。
- 建议采用固定回归集和分层评分：先锁定 AI 原图是否像、是否是完整单体玩具、是否适合建模，再检查四色面积、最小区域和边界复杂度。这样可以区分提示词问题与量化问题。
- 第 1 次真实 Image2 结果 `fangfei-bust-raw.png` 已正确形成单一半身摆件：双臂交叠无重复、腰部自然收口、蓝色圆形底座稳定、背景干净且没有补全下肢，说明新的构图契约有效。
- 第 1 次结果的人脸仍被放大为较强 Q 版大眼，辨识度尚可但离“身份优先”仍有距离；原图还保留自然肤色和金属手表，Image2 未完全遵守严格色板文字约束。
- 离线确定性处理后四色均有意义，红/绿/蓝/白面积约 15.94%/1.50%/9.14%/73.42%，主体面积约 27.96%，区域 58→29，小区域 29→0，质量门禁通过。
- 清理图的白色脸部大面积留白，五官主要靠红/蓝少量色块表达；这说明打印色门禁虽通过，但需要第二轮降低 Q 版夸张，并让关键五官成为更稳定、稍粗的封闭几何色块。
- 第 2 次真实 Image2 将身份权重提高到 85%、玩具化降到 15%，并禁止大眼与金属附件。结果保留成人脸部比例、交叠手臂和专业气质，白脸/蓝发/红衣/绿内搭在 AI 原图阶段已形成清晰材料分区，优于第 1 版。
- 第 2 版离线评分由 0.106275 降至 0.091495，平均色差由 7.4978 降至 5.9134，主体面积由 27.96% 提升至 34.31%，区域 50→22、小区域 28→0；适合作为新的正式默认比例。
- 全身人物 AI 原图肉眼完整，但严格色版中白色四肢与白背景连通，人物被切为上半身和靴子/底座两大块；旧质量指标仍通过，说明只统计面积与颜色数不足以保障 image-to-3D 轮廓。
- 新增主体连通性指标后，真实结果的最大连通主体占比分别为：半身 0.927742、全身人物 0.724210、大雁塔 0.370680。大雁塔的白色层间装饰带也与背景相连，把塔分成七段。
- 对 image-to-3D 参考图，背景色不能出现在颈部、四肢、底座连接、承重连接或建筑层间外轮廓；最大主体连通占比门槛应提高到 98%，让背景色造成的结构断裂在进入 3D 前被拒绝。
- 最终半身版本用连续蓝色头发轮廓包围白脸，并连接绿色高领，最大主体连通占比由 0.924128 提升至 1.0；评分 0.078899、主体面积 44.44%、区域 49→15、小区域 34→0，四种耗材色均有效。
- 最终全身人物使用红色长袖、绿色连裤材质、蓝色头发/靴子/底座，避免白色四肢与背景融合；最大主体连通占比 1.0、主体面积 31.88%，四色均有效并通过门禁。
- 最终大雁塔取消白色层间外沿，采用红墙、绿屋面、蓝檐口/窗/底座；最大主体连通占比 1.0、主体面积 41.58%，佛像、树木和摄影水印均被移除。
- 本轮共完成 8 次真实 Image2 调用、0 次 Tripo 调用；一次中文路径失败发生在上传前，不计远端调用。三张最终结果均来自同一正式提示契约并通过 98% 连通门禁。
# 2026-08-15 阶段 44：无贴图顶点色打印风格滤镜

- 当前正式风格入口只有 `q_cartoon`、`low_poly` 和 `sculpture`；GUI 分别显示为“Q 版设计师玩具（彩色推荐）”“低多边形”“雕塑（适合单色）”。
- Orca 的 OBJ 读取器支持 `v x y z r g b [a]` 形式的顶点色；Sidecar 已能把 Tripo 的贴图/UV 结果烘焙为 `model-vertex-color.obj`，并继续执行色板量化与拓扑校验。
- 因最终丢弃贴图，适合打印的风格必须靠几何轮廓和大面积顶点色区成立。照片写实、织物/木纹、金属/玻璃、细描边、微小文字、密集斑点和连续渐变都属于高风险效果。
- 顶点色会在三角形内部插值；若一个三角形的三个顶点属于不同色区，Orca 中会出现并不存在于耗材色板的过渡色。减面还可能删除颜色边界顶点，因此仅做图片阶段的四色量化不足以保证最终 OBJ 颜色干净。
- 推荐采用两层方案：上游风格预设负责构图、几何语义和大色块布局；下游确定性顶点色处理负责色板量化、混色三角形检测、边界保护、小色岛合并和最终产物验收。
- 第一批建议覆盖五类：纯色搪胶玩具、低多边形拼色、赛璐璐色块、釉彩嵌色摆件、双色雕塑。MVP 优先前四类，双色雕塑作为最稳健兜底。
- 关键未决策是统一颜色上限：若目标是直接适配现有四耗材打印机，建议第一批把“最多 4 个实际耗材色”设为硬约束，而不是先生成多种自然顶点色再依赖导入映射压缩。
- `_bake_obj_texture_to_vertex_colors()` 当前以几何 `vertex_index` 作为唯一键，同一几何顶点即使位于不同 UV 岛、材质或色区边界，也会合并为一个输出顶点；打印色板模式取其所有采样的多数色，自然色模式取平均色。
- 该实现能保证“顶点值属于色板”，但不能保证“每个三角面是单色”。三个顶点颜色不同的面仍会产生视觉插值，因此当前 `_validate_obj_palette()` 通过并不等同于实际显示只出现耗材色。
- 当前 Sidecar 没有再次减面，面数由 Tripo 的 `face_limit` 控制；正式档位是 10/30/50/100 万面，且 `smart_low_poly=false`。因此“低多边形拼色”应被理解为视觉切面风格，而不是把模型降回早期 2 万面方案。
- `_repair_small_obj_topology_defects()` 保留既有顶点完整颜色字段；补小孔时中心点使用边界顶点的多数色。该策略不会系统性丢色，但仍需为补丁面纳入混色三角形与颜色岛指标。
- 第一版边界处理建议按风格区分：搪胶/赛璐璐/釉彩采用“UV 角点展开 + 边界顶点复制”，保留每个面角的实际取色；低多边形拼色再将每个面或连通面簇归为单色，获得真正清晰的切面效果。仅按原几何顶点投票不再作为彩色正式产物的最终算法。
- 深入核对 `Model::obj_import_vertex_color_deal()` 后确认：Orca 会把三个顶点的耗材 ID 转换为 `mmu_segmentation_facets`；两个颜色的三角形是合法的面内材料边界，三种颜色的三角形也有专用分割编码。因此不能把所有混色三角形都当成渲染插值错误。
- 复制不同色区的重合顶点虽然能让单面颜色一致，却会让基于索引的网格拓扑出现开缝，并破坏现有 `test_uv_seams_do_not_duplicate_geometry_vertices` 契约。正式方案改为保留共享顶点和闭合拓扑，优化顶点标签以消除三色面、压低双色边界带宽和小色岛。
- 用户已确认所有正式打印滤镜统一限制为最多 4 色，同时要求适配不同打印机各自不同的四种实际耗材颜色；因此“四色”必须解释为动态数量上限，而不是红绿蓝白常量。
- Orca 侧已有可复用基础：`OrcaWorkspaceAdapter::printable_palette()` 能读取项目槽位颜色并计算最大温度兼容槽位子集；生成页已有“当前项目（自动同步）/手动配置 AI 色板”和任务色板变更失效门禁。
- 当前上限尚未统一：Orca 适配器、GUI 手动色板、Sidecar 和确定性图片管线均允许最多 16 色，需要在正式打印滤镜入口收紧到 4 色，并定义兼容槽位超过 4 个时的选择规则。
- 当前动态色板存在硬编码缺口：`_palette_roles()` 在恰好四种颜色时强制把任意四个 HEX 分配为 `red/green/blue/white`；GUI 的阴影色选项和提示词也固定使用蓝/红/绿/白。遇到橙、紫、黑、黄等真实耗材组合时角色语义会错误。
- 动态四色应改用与具体色相无关的感知角色：最亮/背景候选、最暗/结构色、主色和强调色，并按 CIE Lab 明度、彩度和色间距离确定；若颜色相近，界面应提示可辨识度风险。
- 更稳健的 3D 参考图不应强迫某种耗材色承担背景。推荐生成透明背景或独立中性背景，严格四色约束只作用于主体；否则无白色耗材时会浪费一种主体色并降低 Tripo 的前景分割质量。
- 新任务应保存“生成色板 + 槽位/材料快照 + 感知角色”，旧任务仍按现有纯 HEX 数组兼容读取。跨打印机导入时保留原顶点色，通过手动匹配或显式重新配色适配新耗材，禁止静默覆盖项目耗材颜色。
- 动态四色现在使用 `primary/structure/light/accent` 四个语义角色，1 至 3 色时按角色优先级降级；角色保存进任务和模型库元数据，旧任务缺失角色字段时可从 HEX 色板自动推导。
- 实际 Image2 端点即使收到“transparent background”也可能返回 RGB 图片，并把棋盘格直接画进像素。透明性不能只相信文件格式或提示词，必须在确定性管线中从原始图边界识别背景，再把主体输出为真实 RGBA。
- 白色耗材主体与白/浅灰背景共用最近色板槽时，量化后再做背景洪泛会吞掉脸部、服装和建筑结构。背景分割必须发生在固定色板映射之前；当前真实 Q 版缓存结果重跑后主体面积从 13.72% 恢复到 31.24%。
- “正好使用 4 色”并不保证可打印质量，反之没有强制每张图必须用满 4 色。质量门应要求最多 4 色且至少 3 种有意义主体色，同时独立检查主体面积和连通性。
- Orca 的双色三角形不是错误，而是 `mmu_segmentation_facets` 的合法材料边界编码；正式 OBJ 指标应重点阻断三色面、异常小色岛和拓扑破坏。
# 阶段 45 多样本质量基准发现（2026-08-15）

- 本地 `Camera Roll` 固定三图分别覆盖：白底半身职业肖像、白底全身人物、含佛像/塔/树木/品牌水印和色卡的复杂建筑场景。
- 三图还不能覆盖以下高风险输入：宠物毛发、商品反光、非白背景、深色主体、Logo/文字、高频树叶或织物。扩充回归集应优先补这些失败模式，而不是收集大量相似人像。
- `方飞总.png` 的白西装和浅灰背景会持续检验“白色主体与背景分离”；`刘亦菲.jpg` 的长发、细手指、纹理裙装和靴子会检验结构简化；`大雁塔.jpg` 会检验唯一主角选择、背景剥离以及底部品牌/色卡删除。
- 用户所说“训练调优”在当前技术边界中应落实为：批量候选、提示词迭代、统一评分、人工目视复核和确定性后处理回归；当前项目没有微调 Image2/Tripo 权重的训练接口。
- Wikimedia 页面检索可用，但当前出口对 API 和直接文件下载均返回 403 限流。为保证可重复性，新扩充样本改用 Image2 文生图创建明确受控的宠物、商品、深色主体、复杂背景和玩具输入；本地三张真实照片继续承担真实分布验证。
- 5 张受控合成输入目视合格：金毛全身用于毛发/尾巴简化，黑猫用于暗部轮廓分离，工作台相机用于唯一主角剥离，白鞋用于浅色主体与白背景分离，复古机器人用于密集贴花和细碎色区收敛。
- 8 张集合已经形成“小而难”的首轮基准；继续增加同质人像的边际收益低。下一步应优先保证同一输入、同一色板下的提示变体可复现、可恢复、可评分，并保留完整请求审计。
- 第一轮 22 个 Image2 候选全部通过当前四色自动质量门，但目视证明自动分数只能评价色区/连通性，不能替代身份与语义判断。
- 人物结果的系统问题是成人面部被统一成大眼、幼态、通用玩偶脸；应把真人 Q 版调整为“面部身份约 92%、身体和材质玩具化”，显式禁止放大眼睛、缩小鼻口、改变成年年龄和脸宽。
- 黑猫 `print_first` 自动分最低，但凭空增加了白嘴和白胸；`identity_first` 才忠于纯黑毛色。动物提示必须显式禁止新增脸部、胸口和四肢花纹。
- 金毛使用暖色黑/红/黄/米白色板明显优于 RGBW；RGBW 会不可避免地生成红色犬身。这不是量化错误，而是实际耗材色板与主体语义不匹配，产品层需要保留“风格化配色”预期或提示用户换色板。
- 佛像 `print_first` 比 `identity_first` 更适合打印：保留轮廓和莲座，同时减少莲瓣纹样与细线。机器人暖色 `print_first` 也优于 RGBW，说明复杂装饰对象更受益于结构优先提示。
- 白鞋和相机暴露统一底座规则过强：本身可稳定落地的商品不需要额外展台。底座应只默认用于人物、动物、雕像和不稳定道具；完整稳定商品保持原物体落地结构。
- 动态色板提示中存在一句“不要使用 black/off-white”等颜色名，与实际色板可能包含黑或暖白相冲突；应改为仅禁止引入未列出的自然替代色，明确列出的颜色始终允许。
- 第二轮真人结果明显改善：半身人物保留了成年面部比例、原始笑容、交叉手臂、白西装和绿内搭，摆脱了首轮通用大眼娃娃脸；暖色黑/红/黄/米白比 RGBW 更适合黑发和人像。
- 第二轮纯黑猫严格保留全黑口鼻与胸口，证明动物花纹约束有效；自动门禁失败仅来自“至少三种大面积颜色”的旧假设，不是图像质量失败。
- 第二轮白鞋和相机都不再生成多余展台，且自身轮廓、鞋底/镜头接触结构完整，验证“稳定商品不强制底座”的规则有效。
- 第二轮金毛保持金黄色整身和黑色底座，未再出现首轮白胸/白爪等凭空花纹；暖色色板仍是该主体更合理的真实耗材组合。
- 当前最适合进入 3D 的人物候选是 `portrait_bust_v2__warm_toy__identity_first`；最适合验证四种顶点色和硬边界的非人物候选是首轮 `toy_robot__warm_toy__print_first`。
## 2026-08-15 阶段 45 最终发现

- Orca 的“非流形边”统计不只看无配对边或三面共边。`create_face_neighbors_index` 要求相邻三角形在共享边上方向相反；同向共享边会被计为两个开放边。
- 人像原 OBJ 的普通无向拓扑报告为零异常，但存在 3 条同向共享边，因此 Orca 稳定报告 6 条非流形边。对面邻接图做奇偶约束遍历后只需翻转 1 个三角面即可归零。
- 后处理现在同时校验边界边、三面以上共边、同向绕序边和非可定向矛盾；修复完成前不会把 OBJ 标记为可切片。
- 机器人包含 3 个有效连通部件，但拓扑合法且颜色正常。此前“强制合并成一个对象”的目标会破坏多色语义，应保留供应商输出的有效部件，只清理微小漂浮碎片。
- 三色三角面不能通过复制所有颜色边界顶点解决，否则容易破坏共享拓扑。当前局部邻接队列算法在保持流形共享边的同时把三色面降为零。
- 正式 GUI 复验确认：人像原先的 6 条错误提示消失，红/蓝/白映射保留，切片完成；机器人同样完成多色切片。
- 本轮 35 次 Image2 的收益主要来自系统性提示修正，而不是继续堆候选：身份年龄、动物固有标记、底座适用范围和动态色板合法性是最关键的四类规则。

# 2026-08-16 模型生成 Beta 1 与 3D 质量门禁发现

- 当前链路已证明“先确认可打印参考图，再付费生成 3D”的产品方向有效，但只能证明演示和内部试用，不能由 8 类图片、2 个 3D 模型外推到未知输入。
- “高质量”至少分为图像语义、3D 几何、可打印拓扑和真实打印四层；当前图片与拓扑较强，3D 语义仍主要依赖人工，真实打印尚未形成数据闭环。
- “快速”还没有量化。后续必须记录用户主要决策次数、各阶段 p50/p90 时间、付费重试次数和单个成功制品成本。
- 阶段 45 的机器人有 3 个连通部件但能正常多色切片，说明组件数只能结合尺寸/面数占比判断，不能恢复“只允许一个对象”的旧硬约束。
- 现有 Sidecar 已覆盖边界、非流形、绕序、退化面和面数，但缺少统一 `model-quality.json`，也没有把接地、悬垂、极端比例和微小组件风险投影给 GUI。
- 纯几何门禁看不出“脸不像”或“肢体语义错误”；第一版应先建立稳定本地结构契约，后续追加统一多视角渲染和视觉评分，不应把二者混成一个不透明分数。
- 50 万面级人像本地分析约 16 秒，机器人约 11 秒；相对远端 3D 生成耗时可接受，但不适合在 Sidecar 启动时同步扫描全部历史模型。
- 人像接触跨度约 0.999、向下表面积约 18.95%，结构门禁为 `pass`；机器人接触跨度约 0.954、向下表面积约 17.89%，仅因两个极小组件进入 `review`。
- 现有 Orca adapter 对少量开放/非流形异常具有明确修复与失败降级能力，因此结构门禁必须显式接收下游能力，而不能把所有开放边无条件视为同一产品失败。

# 2026-08-16 质量复核 UI 与历史重验发现

- `AIModelGenerationClient::JobStatus` 当前只解析图像质量和 artifact，没有模型质量字段；新增字段可以保持可选，旧 Sidecar 返回缺失时视为“尚未检查”。
- 生成页面已经把 3D 预览和模型库放在同一右栏，质量卡应绑定 `m_displayed_model_path`/当前 job，而不是放进输入或导入设置区。
- 模型库元数据已有稳定 `job_id`、模型路径、色板和预览路径；历史重验不需要新增客户端路径参数。
- `load_library_entry()` 当前会清空 `m_job_id`，这会让后续无法查询历史任务；阶段 47 必须显式传入并保留条目的 `job_id`，同时不恢复旧输入表单状态。
- Sidecar 已持久化 `model-quality.json` 并通过 `_public_job()` 返回；recheck 只需复用同一领域模块和已有任务目录安全边界。
# 2026-08-16 质量复核 UI 与历史重验最终发现

- 结构质量结果必须贴着当前 3D 预览常驻显示：绿色通过、橙色复核、红色阻断比状态栏或弹窗更容易形成正确决策。
- 历史模型不一定有 `job.json`。兼容方式不是开放文件路径接口，而是让服务端按已校验 UUID、固定目录和固定 OBJ 文件名完成一次性安全接管。
- `review` 的价值是把可修复绕序、微小游离件等风险交给用户和 Orca，而不是把有效多部件模型错误拒绝；`reject` 只用于退化面等确定性硬错误。
- 真实样本验证：`8267ded0` 的 95,338 面模型因可修复绕序与微小脱离件进入 `review`；`ec2d5d40` 的 15,006 面模型因退化三角面进入 `reject`。
- 模型库扫描不能复用目录迭代错误码做单文件检查，否则一个残缺旧条目会提前终止整次枚举。
- `boost::filesystem::current_path()/generated_models` 不是可靠的产品存储定位：Orca 启动后会改变当前目录。GUI、Sidecar 和打包启动脚本必须共享显式 `ORCASLICER_AI_OUTPUT_DIR`。
- 本地重新检查约 10 秒内完成时，应在检查期间临时禁用导入并保留模型画布；完成后按新状态恢复操作，无需模态等待框。

# 2026-08-16 多视角与视觉复核最终发现

- Orca 的 `GLCanvas3D::render_thumbnail` 依赖 OpenGL 上下文、打印盘和 GUI 生命周期，不适合作为独立、可恢复的模型质量服务；最终 OBJ 的软件渲染更符合当前 Orca 解耦方向。
- 30 万面级模型不需要额外 3D 引擎即可生成可判读视图。Pillow 的深度排序三角面渲染在 448 px、最多 24 万采样面下约 13 秒，足以放在用户显式操作后运行。
- 均匀抽样会在高密度表面留下孤立针孔；仅在发生抽样时做 3×3 中值处理，可显著清理针孔且保持主轮廓和大色块。
- Tripo 当前规范化产物的正面朝向为 X 轴方向；五视图标签已按真实模型校准，等轴视图单独提供高度信息。
- 视觉模型可以可靠指出明显底座/主体关系，但不能替代拓扑、壁厚、支撑和打印机兼容检查；把它限制为 advisory review 是必要的防误伤设计。
- 图生 3D 视觉复核必须同时看到原参考图与最终多视角，才能评价身份与主题保持；文生 3D 只能依据用户描述，不应声称精确身份一致。
- 缓存键至少需要最终 OBJ SHA-256、渲染版本、视觉复核版本和模型名；否则更换模型或提示契约后会错误复用旧结论。
- 打包脚本的显式 Python 文件白名单容易随模块拆分漏依赖；本阶段已补齐现有四个缺失运行时，后续应增加“在干净包目录导入 Sidecar”的自动打包测试。

# 2026-08-16 模型生成盲测校准批最终发现

- 4 个未知文字任务全部生成 26.6–29.7 万面、严格动态四色的顶点色 OBJ；颜色和拓扑链路已经明显强于语义链路。
- 四个模型的边界边、非流形边和退化面均为 0；狐狸、机器人和相机是单连通体，塔楼只多出 5 个四顶点微小组件。
- 单张三分之四参考图对正面特征约束较好，但无法可靠定义背面：机器人生成大圆盘和细条，相机背部结构过度膨胀。这是当前 3D 首次认可率的主要瓶颈。
- 狐狸的角色、背包、底座和配色完整，但指南针连接偏细；说明拓扑合法不等于局部连接足够适合打印。
- 塔楼视觉质量最高，多层屋檐和四色块清晰；微小组件被结构门禁正确降为 `review`，证明启发式风险不应升级成硬拒绝。
- 视觉模型在四例中都指出了人工可确认的问题，与人工三态结论 4/4 一致；但小样本和同轮人工复核不能替代独立盲评。
- 第一批严格口径下“无需复核首次通过”为 0/4，但四例都值得导入 Orca 继续检查。下一阶段应先解决多视图/背面约束，再扩到 50–100 例，否则只是扩大同一失败模式。
- 恢复框架必须对不同付费阶段分别防重：Image2 同步失败、Tripo 任务 ID、视觉 `unavailable` 都需要独立记账；仅依赖最终文件是否存在会造成隐性重复调用。

# 2026-08-16 多视图 3D 官方能力与架构发现

- Tripo v3 官方提供 `POST /generation/multiview-to-model`；推荐输入是命名视图对象数组，每项只有 `front / left / back / right` 之一，正面必填、至少两张，顺序由服务端规范化。
- 多视图请求可直接使用文件上传 token，或引用 Tripo `image-to-multiview` 任务；最新模型为 `v3.1-20260211`，支持 `geometry_quality`、`face_limit` 等现有高细节参数。
- 官方要求所有图片为同一对象且光照一致，推荐至少 256×256。项目必须在付费 3D 前验证视图身份、比例、配色、配件与方向，不能只验证像素尺寸。
- 现有 `tripo_client.py` 已使用 v3 和最新模型，单图任务的高细节参数可以复用；缺口只是命名视图校验和 `multiview-to-model` 映射。

## 2026-08-16 阶段 50 真实多视图对照

- Image2 单次生成四视图能稳定保持四色和大体身份，但首轮容易出现工具连接、凸出深度或左右命名疑问；严格正交契约和原图对照能改善复核，仍需人工批准处理软性歧义。
- 机器人多视图最终模型 282,616 面、单连通、零非法拓扑，视觉 88 分，相比同一参考图的单视图基线 78 分提升 10 分，背面遮挡和细长伪影消失。
- 相机多视图最终模型 279,578 面、零非法拓扑，视觉 82 分，相比单视图基线 71 分提升 11 分；唯一结构提示是一个 4 顶点微小组件。
- 2/2 样本都提升 10 分以上，足以证明继续校准多视图有价值，但不能据此宣称未知输入稳定；正式接入前至少扩到 20 例。
- 架构搜索确认 C++ GUI 没有新增 Tripo URL、file token 或 endpoint；阶段 50 的供应商依赖只在实验编排器和 `tripo_client`。正式 Sidecar 仍直接导入 Tripo 客户端，是下一阶段 Gateway 抽取的主要债务。

## 2026-08-16 阶段 51 第二批预检发现

- 狐狸首版四视图保持了帽子、背包、指南针、圆形底座和目标四色，正/左/后/右顺序明确，视觉预检 82 分。
- 与冻结单图相比，四视图没有明显身份漂移；主要不确定性是蓬松尾巴在侧后视与腿部重叠，以及爪子握持指南针的遮挡，而不是颜色或主体缺失。
- 狐狸首版已经达到可人工复核水平，不宜仅为追求 AI `pass` 盲目重生成；应与塔楼首版一起目视比较后决定是否消耗第二张 Image2。
- 塔楼首版四视图把冻结原图的三层墙体/三道屋檐压缩为两层，属于不可接受的结构漂移；AI 预检虽然指出侧/背视重复，却错误把身份判为通过，证明复杂建筑必须显式核对层数等离散几何不变量。
- 方形对称塔楼的左、右、背面本来就可能高度相似，不能仅凭“画面重复”判失败；真正硬问题是层数变化。后续复核需要同时知道“必须三层”和“非正面三侧对称”的样本专属契约。
- 塔楼第二版恢复三层，但底部视图触边且后视主体连通门禁失败；第三版解决裁切和连通，却再次退化成两层墙体/两道主要屋檐。说明当前 Image2 四宫格方案对复杂重复层级建筑的离散计数约束不可靠。
- 塔楼在 3 次 Image2 上限内没有得到合格四视图，因此必须停止在 3D 前；这一失败本身是有效校准数据，不能用人工批准覆盖明确的层数错误。
- 现有 Sidecar 直接导入 `create_image_task/create_text_task`，说明 Python 供应商 Gateway 尚未完成；本阶段应把新能力限制在 client + 独立实验编排器，避免继续扩大正式 Sidecar 的直接耦合。
- 用 Image2 一次生成 2×2 四视图板再本地切分，可以把每例 Tripo 生成控制为一次；若一致性不合格，可以只重做 Image2，不消耗 Tripo 3D。
- 直接把四视图拼图送单图 endpoint 会丢失视图角色语义，并可能生成四个对象或面板几何，不应进入正式路径。

## 2026-08-16 阶段 51 最终校准发现

- 狐狸多视图最终结构报告为 `pass`、自动视觉 83 分，比单图基线高 1 分；但人工五视图看到头顶明显悬浮圆环/横线，第二连通组件是异常几何，因此最终人工拒绝。
- 视觉模型把该异常解释为可能的渲染噪点，说明自动视觉分不能覆盖新增连通组件；最终汇总必须允许人工结论覆盖自动 `outcome`。
- 塔楼三次 Image2 都无法同时满足完整取景、四视图一致和精确三层结构；像素门禁与 AI 身份检查均不足以判断离散层数不变量。
- 累计四组配对中只有机器人、相机人工接受，狐狸和塔楼拒绝，固定四宫格路线的人工接受率为 2/4，未达到产品接入条件。
- 多视图仍然是有效能力，但应作为 `generated_turntable` 策略之一；复杂机械/建筑还需要供应商原生多视图或用户提供视图的对照，不应强制同一路线。
- 阶段 51 实际只新增 1 次 Tripo 生成和 1 次转换；塔楼在付费 3D 前停止，证明预检门禁确实节省了无效 3D 调用。

## 2026-08-17 自定义风格与用户旅程发现

- 现有风格选择是 C++ 固定下拉、原生客户端 `style` 字段、Sidecar 白名单和 OpenAI 提示模板的纵向契约；只增加 GUI 选项会被 Sidecar `invalid_style` 拒绝。
- 自由文本不应编码为 `custom:<prompt>` 风格 ID。独立 `custom_style` 字段能保持能力枚举稳定、支持任务恢复，并为未来 Provider Gateway 保留清晰映射边界。
- `refresh_controls()` 已经由提示词和风格事件驱动并负责统一 `Layout/FitInside`；让自定义输入的 `wxEVT_TEXT` 复用该入口，可以解决“需要点击别处才刷新”的同类问题。
- 自定义草稿应在预设/自定义切换时保留，但任务快照必须同时冻结 `style` 与去除首尾空白后的 `custom_style`，否则修改自定义文字后旧预览仍可能被当成有效。
- Sidecar 应同时验证必填、仅自定义模式可用和 UTF-8 字节上限；UI 的 240 字符软上限用于减少晚期服务端拒绝，服务端 1000 字节仍是最终安全边界。
- 正式 GUI 证明 `wxEVT_TEXT -> refresh_controls()` 能在输入最后一个字符后立即启用预览，不需要额外点击；这套模式应复用于后续所有会改变生成资格的输入。
- 自定义面板展开会增加左侧表单高度，但底部主操作保持固定可见；这比弹窗更符合连续输入旅程，颜色和高级打印约束仍可滚动查看。
- RGBW 打印机色板会让严格色板图显得比 AI 原图生硬，但本次 AI 原图已经在生成阶段使用同一四色并保持机械猫、圆底座和大色块，说明“生成时约束 + 本地严格清理”的两阶段职责成立。
- 页面往返和进程重启都能恢复自定义文字与预览；恢复后的默认查看层为“可打印清理”，不会因上次用户临时查看 AI 原图而改变后续 3D 输入。
- Image2 同步等待约一分钟时，界面长时间停在 10% 但有明确忙碌文案和停止按钮；这是可接受但不理想的粗进度，后续应由 Provider Gateway 提供阶段化异步状态，而不是在 GUI 伪造百分比。

## 2026-08-17 生成页精简与图片对照发现

- 用户截图显示颜色角色映射和打印宽度、喷嘴、线宽、最小特征同时暴露在主旅程里；这些是系统推导或高级校准信息，不应与“描述、风格、参考图”竞争注意力。
- 自定义风格输入采用固定高度，在短文本时留下大面积空白，在长文本时又需要内部滚动；更合适的是按行数增长，并设定约 2–6 行的高度边界。
- 当前“图片对照”只改变同一画布的数据源，下拉选中“严格色板”时原图完全消失；控件名称与实际行为不一致，是用户认为对比功能丢失的直接原因。
- 文生图任务没有用户上传的参考图，但仍有 `raw_preview_path`；对比基准应是 AI 原图。图生图任务则优先显示用户原参考图，旁边显示 AI 原图或当前处理阶段。
- 推荐采用双栏同屏对比，而不是继续要求用户在下拉框间记忆切换；窄窗口才退化为上下布局。
# 2026-08-17 阶段 53：生成页精简与对比恢复

- 右侧原图并未丢失：纯文字任务已经把 AI 原图下载到 `m_raw_preview_image`，但旧对比条件只认用户上传的 `m_reference_image`，因此处理阶段错误退化成单图。
- 对比基准应按任务类型选择：图生图使用用户参考图，文生图使用 AI 原图；选择 AI 原图阶段本身时使用单图，避免左右重复。
- 左侧最难理解的是颜色用途与物理打印参数，它们是调优项，不是多数用户完成首次生成所必需的输入；移入默认折叠高级设置后，首屏只需理解“是否限制颜色”和“使用哪组耗材”。
- `wxCollapsiblePane` 默认会尝试调整顶层窗口大小，嵌入 Orca 主框架时必须使用 `wxCP_NO_TLW_RESIZE`，否则展开高级设置会让整个应用窗口跳变。
- 文本控件按字体测量可视行数，并限定最小/最大行数，可以同时实现短文本紧凑、长文本可读和主操作区不被无限挤压。
# 2026-08-17 阶段 55：局部智能改色第一版
- 自动拆件不适合作为局部换色第一版：它会改变拓扑、对象语义和后续导入行为，而用户的直接目标只需要修改现有顶点色。第一版保持网格不变，在 3D 预览中选择三角面区域即可满足换色闭环。
- 稳定的离线选区可以由三层信号组成：三角面连通性负责阻止跨独立部件扩散，种子色差负责识别已有色块边界，局部法线连续性负责阻止跨锐边扩散；补选/擦除用局部半径修正语义误差。
- `VertexColorRegionEditor` 与 wxWidgets、Provider、Sidecar、Orca 工作区解耦，未来 SAM/语义服务只需返回面 ID 集合即可替换自动选区来源，不必重做 GUI、手工修边和 OBJ 写出。
- 真实 GUI 验证：290,438 面、11 色历史 OBJ 的一次智能点击选中 728 面，擦除后剩 611 面；选区高亮、范围与模式控件均即时生效。
- 应用红色后生成 16,871,469 字节的新 OBJ；元数据为 `source=local_recolor`、290,438 面，并记录具体目标耗材色。原始 OBJ 仍存在且 SHA-256 为 `970A347CD86007486084528DDA45BB9CFBDDE187F540E7C1B5B199297EFC7750`。
- 收尾复核修正了自然色源模型的标记：局部改色不量化未选区域，因此自然色模型改色后仍按自然颜色预览和入库；只有原先已限制到四色的模型才继续显示“4 种可打印颜色”。元数据另存 `recolor_target_palette`，避免把可选目标色误当成 OBJ 的完整实际色板。
- 修正后的正式 GUI 二次复验选择了 36,472 个躯干三角面并写入红色；预览从 11 个量化色增加到 16 个实际顶点色，历史模型明确显示“自然颜色”。新 JSON 同时满足 `palette=[]`、`use_printable_colors=false`、`recolor_target=#FF0000`、四个 `recolor_target_palette` 和 `preserves_unselected_vertex_colors=true`，说明自然色来源与目标耗材候选色已经解耦。
- 真实启动再次确认：Orca 会把当前目录改到日志目录，正式产物定位不能依赖 `current_path()`；必须由 `start_orcaslicer_with_ai.bat` 或打包启动器显式设置 `ORCASLICER_AI_OUTPUT_DIR`。本轮直接验证产生的 OBJ/JSON 已迁回项目 `generated_models/downloads/`。
- 主构建配置关闭了 Catch2 目标（`BUILD_TESTING=OFF`、`BUILD_TESTS=OFF`），因此新增测试源码暂不能在现有 build 目录执行；Release 构建和完整真实 GUI 旅程通过，后续 CI/测试构建可直接纳入该测试文件。

# 2026-08-18 阶段 56：局部改色颜色一致性
- 用户截图显示选区高亮仍是约定的青色，目标下拉当前显示红色；用户报告的是点击“应用改色”后原绿色区域变蓝，因此问题需要按“目标色选择 → OBJ 写出 → Orca OBJ 解析 → GL 重新预览”逐段核对，不能把青色选区覆盖层误判成最终模型颜色。
- 现有写出函数按 `color[0] color[1] color[2]` 输出标准 RGB，Orca `load_obj` 也按同一顺序解析；现有测试只断言绿色字符串被写入文件，尚未覆盖 wx 颜色转换、重新加载后的颜色统计和 UI 下拉选择稳定性。
- 正式 GUI 复现选择 `耗材 2 · #00FF00` 并应用后，最新 JSON 记录 `recolor_target=#00FF00`，OBJ SHA-256 为 `C26AB24D471B260A8592D5365329EACF2C9EC0DECB037755BC0AEFD761E9FC83`，顶点行明确写入 `0.000000 1.000000 0.000000 1.000000`；重新加载后的目标区域也呈绿色，排除真实 RGB/BGR 通道交换。
- 根因是固定青色不透明选区层与陈旧的“已选择 8343 个三角面”摘要共同造成误判：青色会遮住原绿色，应用后模型虽然重新加载且内部选区已清空，但 UI 摘要没有收到零选区通知。修复应让选区预览跟随目标耗材色，并在每次模型加载后统一同步零选区状态。
- 修复后的正式 GUI 以 `#00FF00` 选中 41,605 个三角面时直接显示绿色高亮；应用后摘要清零、按钮禁用且模型继续显示绿色。最终 OBJ SHA-256 为 `42AA459441B6CC91A23BE308102F9B3248FD0BC7BC0F4574476152E1A9D6D6E9`，顶点行保持标准 RGB。
- 颜色一致性修复只涉及 AI 预览层和 UI 状态同步；红、绿、蓝通道的文件契约由 `VertexColorRegionEditor` 写出后再经 Orca `load_obj` 读取的测试源码约束，没有向 Orca 核心引入颜色特例。

# 2026-08-18 阶段 57：GitHub 提交前仓库审计

- 当前 `codex/model-generation` 分支相对 `ca5de31dff` 有 21 个已跟踪文件修改和 59 个未跟踪文件；未跟踪内容总计约 0.36 MiB，主要是模型生成源码、测试和设计记录。
- `.gitignore` 已覆盖 `build*`、`generated_models/`、`output/`、`projects/`、本机 AI 配置和打包配置，因此真实模型、预览、安装包与本机密钥没有进入当前未跟踪清单。
- `.planning/` 是本地多任务规划状态，不是产品源码；应通过仓库忽略规则隔离，而根级 `task_plan.md/findings.md/progress.md` 当前已经被跟踪且记录本项目完整研发证据，是否长期保留可在后续文档策略中单独调整。
- `real-tripo-text.3mf` 是约 27.76 MiB 的既有已跟踪文件，不属于本轮差异；本次整理不直接删除或改写 Git 历史。
- 正式运行不依赖 `tools/ai_sidecar_mock.py`，但生产/Mock `/health` 契约测试仍直接加载它；Windows 打包脚本使用正式运行文件白名单且不复制该 Mock，因此保留测试工具不会进入发布包。
- 当前主要文件规模为 `ModelGenerationPanel.cpp` 3,777 行、`orca_ai_sidecar.py` 3,343 行。提交前强行拆分会扩大回归面；本版只做发布卫生整理，组件拆分应作为后续独立提交并保持现有测试基线。
- 变更文件密钥扫描只命中 `test_sidecar_contract.py` 的测试夹具；未发现真实 OpenAI/Tripo Token。正式 endpoint 通过环境变量注入，打包检查脚本的 `https://laotie.dev` 只是无凭据默认 URL。
- 最终验证通过：Python 单测 179/179、35 个 Python 文件语法检查、29 份变更 Markdown 本地链接、基准 JSON、`git diff --check` 和 Windows Release 完整构建均通过。
- 分支 `codex/model-generation` 当前没有 upstream tracking，且 HEAD 与集成基线相同；本地改动提交后首次推送必须显式设置 `origin/codex/model-generation` 上游。
- 完整单并发重编译出现的 C4172、C4005、C4101 与 LNK4098 均位于未修改的 Orca 上游文件或既有链接配置，不是模型生成新增告警。
