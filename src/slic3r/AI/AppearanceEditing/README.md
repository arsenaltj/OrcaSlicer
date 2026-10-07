# 3D 美颜调优入口

本目录负责只读输入快照到独立候选资产的计算。接受版本、草稿、预览和工程提交由现有应用层负责；新版 UX 保存入口的集成留到 UX 收尾。

## 输入、输出和替换

`AppearanceEngine.hpp` 的 `IAppearanceEngine::process(Request)` 接收操作、source、新 destination、`ModelFinishingOptions` 与取消回调，返回 `ModelFinishingResult`。同步调用内引用有效；异步消费者应先保留输入快照的生命周期。

通过 `ModelFinishingOptions.engine` 注入实现，空指针使用 `baseline_engine()`。默认装配和基线均在本目录；原 `finish_model_obj`、`finish_model_artifact`、`finish_beauty_artifact` 三个兼容入口统一经过该接口。当前 ID/版本为 `appearance-baseline` / `appearance-baseline-v1`。

## 编辑分区与局部配色精修

`BeautyEditRegions.hpp`现在由本模块拥有，原GUI路径保留转发头。编辑区可以包含多个真实打印色块：`assign_region`仅组织选择，`reshape`默认`PreserveColors`只调整编辑边界，`ExtendAdjacentColors`按接收边界的局部槽位延伸；旧`reshape_and_paint`是显式覆色兼容入口。原v1编辑区JSON格式及源身份校验保持，首次匹配保留匹配前编辑覆盖，旧保存记录不会自动分区。

显式局部去杂色/修边入口位于`BeautyPuzzleRefinement.cpp`，使用颜色模块的只读提议和原有边界扩散。`refinement_protection`根据绑定到当前模型的识别标签保护五官；`protect_color_intent`比对原始面色与区域目标，保护手工RGB、直接耗材、无法确认自动来源的旧颜色及已修整目标。没有识别时去杂色保护全部未知区域，边界修整仍使用手工/连通性/面积保护。纹理编辑保留几何和面序，实际面槽位进入既有保存/打印消费者。

UX接线需传递：原始源色/身份、独立编辑区和打印区、明确边界覆色模式、显式选区及五官保护。重新按识别整理编辑区仅更新逻辑分组；Ctrl建立编辑组保留内部颜色，操作进入同一撤销/草稿快照。新UX页面的接入仍需在其实际工作树上验证。

## 可改范围

`BeautyRegionPrecision.hpp::refine_selected_region_mesh`提供显式的面内精度候选：沿同一有界轮廓在焊接顶点采样连续侧别，只切开相交三角面，共享边使用相同切点，UV接缝仍保留独立索引。输出新网格、父面序号、原边顶点插值及真实编辑区；不变形、不全局加密、不自动判断人体语义。`remap_beauty_precision`核对实际重开网格，再按父面逐项继承材料；随后用原编辑区接口显式指定耗材，释放边缘也需明确赋色。

`assess_beauty_precision`按未改变归属的父面核对新旧语义连通块的一一对应；默认重绑定拒绝无来源的新块、拆断、合并或完全删除原分区。它保护语义分区，不保留每个旧打印色块ID，也不能替代材料岛/人体准确性验收。仅显式`allow_component_changes_for_review`可导出被拒绝的诊断候选，不应作为UX的默认应用参数。当前两旧人像的碎片化唇色代理仍无法稳定自动面内拟合，能力保持实验入口；认可六色和v4自动去杂色不再调参。

轮廓`Segment::source_orientation`来自原边链遍历方向，面级v2/面内v3侧别不再从拟合切线与旧切线夹角猜测。尖角回折时该夹角会变号而内外侧不变；新增回归覆盖此真实失败。显示轮廓位置和既有记录schema保持。

保存由独立ModelArtifacts的`write_glb_surface_refinement`承担，追加POSITION/NORMAL/TEXCOORD_0/COLOR_0/indices并保留原图片、材质、节点与旧二进制载荷。首版受限于同源面绕向、单静态primitive和密集float属性；量化UV/色、显式tangent等不支持项报错，不静默丢弃。新拓扑必须成为独立base版本，并用实际重开几何重新绑定新记录；旧草稿/五官面罩/旧面序不能直接复用。现有GUI操作与默认保存路径仍用原面级版本，人工控制点交互/版本接受及新UX另行集成，不自动重放此候选。

保存校验同时检查父面面积、子面内部有向边配对及三条原边的唯一完整覆盖；只有面积相等不能排除重叠加漏面。检查按父面局部焊接相同位置的角点，允许保留独立UV索引；异常或发布后取消均清理本次新版本，原件保持。

定向测试使用`BeautyRegionPrecision|GlbSurfaceRefinement`，覆盖椭圆轮廓误差/曲面覆盖、材料继承及显式赋色、同比尺度/UV接缝、保护孔/取消、纹理与UV保存及来源失败。显式离线探针`BeautyRegionPrecisionProbe`读取`ORCA_PRECISION_SOURCE`、`ORCA_PRECISION_INPUT`（同源面级基线记录/保护面罩）及全新`ORCA_PRECISION_OUTPUT`；槽4嘴唇/槽3释放肤边为该对照人工指定，同一意图同时生成面级和面内产物。没有自动识别、GUI或实物验收含义。

连通检查回归覆盖拆断、合并、删除及新旧连通块总数相同的失败；保存回归覆盖等面积重叠/漏面、旋转与非均匀缩放下的UV接缝及发布后取消。桌面隐藏探针`BeautyRegionPrecisionImportProbe`读取新候选及逐面槽位；显式设置`ORCA_PRECISION_IMPORT_3MF=1`还会写入全新`refined-materials.3mf`并通过原生保存/加载检查面序、顶点、耗材分面序列化和六色槽位。它验证文件消费者，不代表GUI导入、切片或实物，也不将被拒绝的诊断候选升级为可应用版本。

`BeautyBoundaryContours.hpp`的曲线计算由本模块拥有，GUI旧路径保留转发。`BeautyRegionBoundary.hpp::plan_selected_region_boundary`把有界曲线投影用于局部三角面中心分类；`BeautyEditRegions::smooth_curve_boundary`仅调整选中逻辑区，默认保留颜色。用户可随后给相邻分区指定耗材，或显式选择`ExtendAdjacentColors`同步移动面材料；每个接收块须连到未移动的本区面，不能凭空猜颜色。保护面、三方交汇、折角和远处表面保留，未知语义不会自动触发。原v1保存记录直接保存实际面归属；没有保存曲线控制点或切开三角形，粗网格面内锯齿仍是表示限制。手动校边和分区赋色继续用原入口，拟合不自动重放到历史/识别更新。

工作台在现有编辑区菜单添加“平滑选中分区轮廓”；颜色延伸复用原开关，页面布局仍由UX负责。测试标签`BeautyRegionCurves|BeautyBoundaryContours|BeautyEditRegions|BeautyRegionRefinement`可用独立`ai_appearance_tests`运行。真实局部探针可显式传`ORCA_COLOR_CURVE_REGION`、`ORCA_COLOR_CURVE_SLOT`并配套已绑定源身份的guidance/六色；这是手动选择代理，不是自动识别或实物验收。

`BeautyPuzzle::recover_source_color_details`显式按不可变逐面源色补回选区的连续颜色细节，保护手工物理/混色槽位、自定义RGB及来源不确定的旧目标。只修改选区真实配色，残余区域及新区源色目标各自重算，逻辑编辑层/几何/原色不变，候选经原撤销与草稿路径应用。先补细节再决定是否清理/修边，避免仅平滑已经丢失细节的色块。源色缺失、来源不符或取消不能改变当前结果。

外观、局部修饰、边界、纹理处理及几何计算改本目录和对应测试。共享格式/来源身份读写位于 `AI/ModelArtifacts`，色彩帮助类型复用 `AI/ColorMatching`；共享契约变更由统一接线任务处理。

只写新候选，不覆盖 source、历史、草稿或3MF。正确填写 `preserves_face_order`；拓扑/面序变化不能沿用旧 face/region 映射。保留取消、来源身份检查和失败时目标清理语义。算法返回成功也不等于版本已发布。

## 单模块验证

`constrain_selected_colors`使用颜色模块的显式分区材料约束。只处理选区内、原色来源明确且未手工修改的错误唇色，嘴唇及两圈边缘/口腔/未知/衣物保持；原网格/编辑层不变。校正的新区以原生物理或既有混色槽位保存明确材料意图（不再保存原均色为自动目标），后续原色恢复与同色板重开不撤回校正，仍走原撤销/草稿路径。余区自己的源色目标重算，事务末验证及取消之后才应用。识别/手工分区只提供角色，算法不会推断它们已准确；新UX传递同源标签及材料角色即可复用，不依赖现有菜单。

在仓库根执行，只构建美颜及格式/色彩依赖，不编译桌面 GUI：

```powershell
./dev.ps1 CppTest -TestSuite ai_appearance_tests -TestLabel '.*' -Jobs 2
```

[接口样例](../../../../tests/ai_capabilities/test_appearance_engine.cpp) 和复用的 ModelFinishing/BeautyAppearance 回归覆盖 OBJ、重复/退化面、多材质纹理、共享UV、局部选区、取消、过期来源及已有目标。草稿/接受/存储回归另用 `slic3rutils_tests` 的 `BeautyPersistence` 标签。

验收：固定视角对比候选与基线，记录原件SHA、几何/面身份、算法版本和耗时。原件保护、局部未选区、撤销/保存恢复规则必须继续成立。新 UX 的版本发布尚未验证，不能用离线候选测试替代该验收。
