# 颜色匹配调优入口

本目录包含独立算法及当前默认实现，已有 UX 通过兼容入口调用它。可以先优化并验证本目录，UX 收尾后再核验新版页面。

## 输入、输出和替换

- 区域路径：`ColorMatchingEngine.hpp` 的 `IColorMatchingEngine`，`Input` → `Computation`。输入为面/区域颜色、面积、用户锁定、实际材料槽位和身份快照，输出是未确认的配色候选。
- 贴图路径：`TextureColorMatchingEngine.hpp` 的 `ITextureColorMatchingEngine`，只读贴图网格/设置 → `TextureColorResult`，另提供实际耗材匹配。两条路径保留各自算法，不强制合并。
- `Input.engine` 与贴图调用的 engine 参数可注入实现；空指针采用 `baseline_engine()` / `baseline_texture_engine()`。默认装配函数在本目录，任务启动时捕获实现。
- 当前基线：`region-matching`，直接色 `region-direct-v5`、叠色 `region-layered-v2`；贴图 `native-texture-color-v1`。新算法更换自己的 ID/版本，旧已接受结果不重算覆盖。
- **3D美颜实际路径**：`BeautyWorkbenchControls → beauty_match_feature_filaments → BeautyPuzzle::match_filaments → refine_automatic_color_regions`。细节保留算法位于 `AutomaticColorRegions.hpp/.cpp`，版本 `automatic-color-regions-v1`；只读面邻接、面积、原色、微分区、可自动处理的区域和真实槽位，输出连续色块候选。美颜模块负责分配编辑ID、验证和应用，GUI继续负责确认、撤销与保存。优化这条路径不需要改 UX。

## 首次自动匹配的细节保留

原大区先做面积均色，微分区只有在另一实际材料改善至少 6 ΔE00 时才提议细分；逐面复核不比冻结基线更偏色，随后按连通性、模型/原区域面积阈值和保存目标的可重匹配性筛选。若拆分留下过小原色残片，局部撤回最短连接路径上的提议，不涂掉残片。阈值在 `ColorRegionOptions`，它们是软件外观约束，不代表喷嘴/打印分辨率。

默认仅对**首次匹配、未手涂、有原色面数据**的区域启用；已有耗材配色、旧历史和显式手工颜色不重新分区。同色物理槽位保留各自身份，混色只允许项目中已有、校验通过的均匀原生混色槽位。每块新区及残余原色区保存自己的源色均值；同槽位色板换序不触发重匹配，新增材料按各区目标色重新选择。单独调用 `match_filaments(..., source_faces, false)` 可回到原均色基线，持久化格式不变。已保存的旧配色不会因升级自动改变。

美颜绑定实现在 `AI/AppearanceEditing/BeautyPuzzleColor.cpp`，大头文件只保留声明；后续算法调优优先修改颜色模块的 `.cpp`，减少桌面头文件依赖导致的重编译。现有 `BeautyEditRegions` 逻辑编辑分组保持原有覆盖；未使用该编辑层的旧入口会显示更多打印分区。

逐面误差约束属于此算法；后续已有的语义五官对比度修正是独立策略，不能把无识别回放指标当作带识别的整条链路保证。色板缺色、源模型本身灰度、皮肤目标色和实物标定应分别优化。

## 美颜工作台的显式局部精修

`ColorIslandCleanup.hpp/.cpp` 提供 `find_enclosed_color_islands`，版本 `enclosed-color-islands-v1`。输入是冻结的逐面真实槽位、选区/保护蒙版、邻接、面积和折角屏障，输出已有槽位的局部覆色提议；不持有GUI、草稿、纹理或识别器。仅清理被同一槽位完全包围、面积不超过该独立选区0.3%、周围色区面积至少为其20倍的色岛；开放边、选区外、折角、保护细节和多色邻域均保留。固定输入不级联重算。

`BeautyPuzzle::clean_color_islands`负责原生槽位校验和事务应用；`refinement_protection`保护已识别五官，未知区域在杂色清理中保持锁定。GUI另通过原始面色和`protect_color_intent`保护手工RGB目标、手工耗材及无法确认自动来源的旧目标。原始色差可以因用户明确去杂色而增加，不以更小原图色差冒充清理质量。

`BeautyPuzzle::smooth_selected_boundaries`复用原有曲面扩散/面积预算/连通性约束，只在选区及相邻三圈中修改实际面归属，保护手工颜色、五官内部及折角；保存与打印沿用同一逐面槽位。该版本仍按原三角面表达，显示曲线不替代真实填色，也未增加面内切分或拓扑修改。

工作台首次匹配前保留逻辑编辑区；颜色细分可以产生多个打印色块，但不改编辑对象。默认拖边界只改逻辑分区，显式启用“拖边界时延伸相邻颜色”才沿局部相邻颜色延伸，避免眼白被整个眼睛的主色吞掉。选区清理和修边均经现有草稿/撤销/保存路径提交。另一台UX可复用这些入口，不依赖本机菜单布局。

定向检查：`./dev.ps1 CppTest -TestSuite ai_color_matching_tests -TestLabel 'ColorIslandCleanup|AutomaticColorRegions|ColorMatchingEngine' -Jobs 4`，及美颜的`BeautyRegionRefinement|BeautyEditRegions|BeautyColorRegions|BeautyPuzzle`标签。离线隐藏`BeautyRefinementVisualProbe`需要新的`ORCA_REFINEMENT_OUTPUT`目录，导出人工面罩夹具的实际面色；该夹具不是真实人像。已有模型的`AutomaticColorModelProbe`另设`ORCA_COLOR_REPAIR=1`可比较当前匹配与显式选中全模型的真实面边界修整，`ORCA_COLOR_SAVE=1`检查保存重开；无识别回放不代表真实五官识别或实物验收。

## 可改范围

`SourceColorDetails.hpp/.cpp`的`selected-source-color-details-v1`是首次匹配后显式恢复选区源色细节的独立入口：不再用原有微patch均色提出候选，而读取不可变逐面源色，与该面的当前真实槽位比较。复用连续区域、面积过滤、源色逐面不退化及残余小岛撤回；选区外及保护面不参与提议/面积预算。Appearance的`recover_source_color_details`保护手工/未知旧目标，事务应用并更新新区及剩余区自己的源色均值；逻辑编辑区不变。本机现有菜单增加“按原纹理补回选区颜色细节”，新UX可直接复用此接口。没有合适的耗材颜色时仍不能恢复该颜色，不臆造新槽位或配方。

测试标签`SourceColorDetails|BeautyRegionRefinement`。真实探针设`ORCA_COLOR_DETAILS=1`，可另设`ORCA_COLOR_SELECTION_TOP_FRACTION`为模型Z范围顶部比例；这是明确的几何选区诊断，不是语义识别。与修边探针互为独立操作，保留固定色板、原件和首次匹配输出；现有纹理保存范围不变。

需要单独评估材料色域时，探针可设`ORCA_COLOR_PALETTE_FILE`为UTF-8 JSON文件，内容是1–6个`#RRGGBB`字符串的数组，顺序映射到零基槽位。默认仍用原六色板；输入校验及哈希记入`probe.json`。这是离线软件色板模拟，不修改工程材料配置或证明实际耗材兼容。对照算法收益须固定色板；换色板收益另列，不能混用两者结论。

权重、色差、降色、区域边界、patch 与质量评分改本目录及对应测试。维护稳定 `ColorMatchingTypes.hpp` 输入输出语义。GUI 的采集/取消/确认、Orca 的正式应用和保存继续由原消费者负责。

`AI/AppearanceEditing` 会复用本目录的色彩帮助类型；修改公共类型或数值语义时加跑它的相关回归。只替换匹配引擎无需改美颜页面。`AI/ModelArtifacts` 等共享目录由统一接线任务处理，五个调优任务避免同时编辑这些文件。

## 人像六色的起始建议

用户已于2026-10-05认可软件对照并要求制作耗材，冻结版见[人像六色v1：色卡/CSV/JSON](../../../../Docs/printing/palettes/portrait-six-v1/README.md)。后续固定此版目标色做算法前后对照，实物测量与材料配置另存，不改变本版目标。

用户保留自己的六色耗材板；算法始终消费实际槽位/材料，不把建议写成默认工程配色。当前两个真实人像的软件起始目标如下，不是已测量或保证能买到的六种材料：

| 角色 | 参考屏幕色 | 用途 |
|---|---|---|
| 浅肤色 | `#F7E2DA` | 皮肤亮部 |
| 暖肤色 | `#E8B49A` | 主体肤色 |
| 暖棕色 | `#70533E` | 眉毛、棕发、部分暗部 |
| 低饱和砖红 | `#B9514A` | 唇色 |
| 柔黑 | `#282629` | 黑发、瞳孔 |
| 暖白 | `#F6F7F9` | 眼白、牙齿、白衣 |

这是针对已有样本的起始组合，不是通用最优。需要保留蓝/绿服装或更深肤色时，应重配角色并以实际材料复核。六槽离散材料不能连续还原所有光照/肤色；不自动生成混色配方。

[固定色板算法与独立色板对照](../../../../.tmp/color-details-20261005/summary.md)：同算法换此软件目标色，两例顶部38%几何选区平均ΔE00为7.183→4.607、7.524→5.848；该收益属于色板变化，实际六色值未提供，不能据此判定用户色板或打印改善。另试厂商公开HEX组合时整体软件误差上升，保留失败候选、未推广。眼周/耳部红褐阴影的误配由下述显式分区约束继续处理，不能仅靠原图最近色或平滑保证正确。

## 分区材料约束

`PortraitColorConstraints.hpp/.cpp`的`selected-portrait-material-constraints-v4`提供显式分区用色校正。纯算法消费只读面角色、真实槽位/材料、原色、选区/保护与邻接；只把已知皮肤/眼睛/头发中误用唇色槽位的面改到已有非唇色候选，逐面源色损失默认最多增加8ΔE00。嘴唇/口腔及相邻两圈、未知/衣物、选区外和手工保护面保持；返回按源区域/目标槽位连通的提议，不改GUI或历史。不承诺原图ΔE下降，这次用户意图是移除不合语义的红色。

每个提议还须保持残余原色区连通；需要涂改未知/保护面才能连接时撤回提议。邻接搜索最多检查4096个面，无法在预算内证明连通也保守保留，叶端移除后重试到稳定结果。此保护避免为消红制造大量原色碎块，但分区接缝或连接点可能仍留少量红色；不能以扩大选区或覆盖未知面绕过保护。

v2在已接受的面罩内增加空间一致性：以逐面源色ΔE和相邻面材料不同的边数为代价，默认每条材料边罚4，交替面序最多8轮严格降低代价。可选择附近已有的非唇色槽，但仍逐面遵守原8ΔE00损失上限；不扩大校正面罩，不改几何、保护或原色残余连通性。`boundary_edge_penalty=0`或`coherence_passes=0`回到v1最近色分配；上限32轮，参数是软件代价而非已标定喷嘴分辨率/切片费用。实际材料连通分量与边数应单独统计，色块ID数量不能直接当成打印色岛数量；没有满足误差上限的连接色时仍可能保留细碎色岛。

唇色槽位可以由调用方显式提供；缺少材料角色时，`suggest_lip_material_slots`仅按屏幕色的红/粉色相、饱和度和亮度给出保守建议，不是实物或用途标定。接受六色测试中只选择砖红槽4，浅肤/暖肤/棕/柔黑/柔白不选择。特殊材料及深肤色必须复核/提供明确角色，不能把该启发式称作通用识别。

v3在同一代价/预算内补整块更新：逐面更新后，尝试把已接受面罩内同色连通块一起改为其边缘已有的非唇色材料；每个面均须满足原损失上限，整块总代价严格下降才应用。没有按“两面小岛”等面数阈值消色，也不按样本调参数；`component_coherence=false`保留v2逐面策略，用于消融对照。分区缺失/未知、缺少可用非唇色时继续不改；实际槽位和同RGB不同槽保留身份。合成回归覆盖深浅/非自然色、缺色、不同面数/面积尺度和排列，但不是不同人像的效果验收；当前边数代价及保护圈仍依赖网格离散，尚不保证任意重网格后的同一物理结果。泛化视觉效果仍需独立人像/肤色与实测材料，不能用两个手工ROI证明。

v4增加可选只读`PortraitColorGeometry`：与邻接同序的真实三角边长及参考长度L。几何代价为Σ(ΔE × 面积/L²) + boundary_edge_penalty × Σ(不同材料边长/L)，内部边只计一次；对同比缩放不变，对同一连续区域细分时面积/边长总和保持同一含义。逐面源色损失上限、原校正面罩、连通保护和嘴唇两圈保持。使用对称平均边长避免允许的数值舍入造成不对称代价，拒绝缺项/负值/非有限值/不一致邻边；0代价/0轮仍有效。

Appearance默认由只读BeautySurface面边长及引用三角形的包围盒对角线确定L，比例`boundary_scale_fraction=.001`先固定再验证，不是标定喷嘴毫米数。未引用顶点不影响范围。新增面边长为每面3个float（12字节/面），不准备完整顶点距离图，不写入保存或geometry ID；需重编译消费者，不改UX调用参数。`geometric_coherence=false`或无几何数据时保留v3计数代价，隐藏探针可设`ORCA_COLOR_GEOMETRIC_COHERENCE=0`作消融。

几何代价修复计数尺度问题，不保证任意曲面/网格的全局最优或完整算子重网格不变：局部求解顺序、硬色差、原色连通搜索及嘴唇保护圈仍有限。平面同区域细分/同比缩放回归只覆盖给定区域，不能当作独立人像识别/实物验收；新人物、材料角色和实际打印仍需独立数据。

Appearance的`constrain_selected_colors`负责识别名称转为中性角色、原色/手工保护和事务应用。明确校正后的新色块保存为材料意图，后续补源色细节不重新引入红色，原v1格式不变。工作台现有“更多操作 → 按五官分区校正用色（可撤销）”需已取得分区；更新识别本身不自动覆色。未知分区不猜成皮肤，几何选区不能冒充识别。

测试标签`PortraitColorConstraints|BeautyRegionRefinement`。真实隐藏探针同时设`ORCA_COLOR_DETAILS=1`、`ORCA_COLOR_CONSTRAINTS=1`和`ORCA_COLOR_GUIDANCE_FILE`，文件须绑定source SHA/geometry ID/面数并包含名称与RLE标签；两分支都运行当前匹配与补细节，仅候选显式校正。标注来源写入探针，手工ROI只能证明给定分区下的校正，不能当作自动识别验收。

## 单模块验证

在仓库根执行，只构建颜色依赖，不编译桌面 GUI：

```powershell
./dev.ps1 CppTest -TestSuite ai_color_matching_tests -TestLabel '.*' -Jobs 2
```

美颜绑定和真实消费者的定向回归：

```powershell
./dev.ps1 CppTest -TestSuite ai_appearance_tests -TestLabel 'BeautyColorRegions|BeautyPuzzle' -Jobs 2
./dev.ps1 CppTest -TestSuite slic3rutils_tests -TestLabel 'BeautyColorRegions|BeautyPuzzle|BeautyTarget|BeautyPersistence|BeautyEditRegions|ModelPreviewPuzzle' -Jobs 2
```

已有本地模型可用隐藏的 `[AutomaticColorModelProbe]` 做同输入、同六色板的原生基线/默认候选导出。构建 `ai_appearance_tests` 后，设 `ORCA_COLOR_SOURCE` 为模型路径、`ORCA_COLOR_OUTPUT` 为**尚不存在**的证据目录，再运行该 exe 的 `[AutomaticColorModelProbe]`。它记录原件哈希、匹配耗时与面数据，检查连通性、记录重开、目标源色和同槽位色板换序；导出的两个 `manifest.json` 可分别交给仓库 `scripts/beauty_color_quality.py --manifest ... --output ...`。另设 `ORCA_COLOR_SAVE=1` 可对 GLB 工作副本执行现有保存与几何重开，报告基线/候选各自的能力与错误；现有保存器的嵌入贴图要求不变。这是离线诊断，不提交生成任务或改原件，也不代替 GUI/切片/实物验收。

样例在 [test_color_matching_engine.cpp](../../../../tests/ai_capabilities/test_color_matching_engine.cpp)、[test_texture_color_engine.cpp](../../../../tests/ai_capabilities/test_texture_color_engine.cpp) 和现有边界回归：非连续槽位3/11、锁定颜色、拒绝面数不符、取消、贴图候选及局部边界。原生提交/旧历史另按 [工程导航](../../../../Docs/AI_ENGINEERING.md) 的桌面标签验证。

验收：固定输入先和基线比较，记录输入身份、算法版本、色差/关键区域保留与耗时；候选仍需用户确认，失败不能改原件或 live 工程。屏幕配色和实物颜色分别评价。
