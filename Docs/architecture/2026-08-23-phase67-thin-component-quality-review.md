# 阶段 67：薄型连通部件预检验收

日期：2026-08-23

## 结论

阶段 67 已增加旋转不变的薄型连通部件预检。对于闭合、流形且无退化面的模型，质量门会按连通组件计算最小主轴投影尺寸；长度至少 2 mm、最薄尺寸低于 0.8 mm 的组件产生 `thin_structural_components` 复核提示。开放、非流形或绕序异常模型把厚度标为未知，不输出薄型结论。

该指标只描述“整个连通实体的最薄方向”，不声称定位附着于大型主体上的局部薄壁，也不自动加厚、删除或移动几何。

## 算法与契约

- 报告 schema 保持 `1`，门禁版本提升为 `structural-v5`。
- 复用既有并查集，按组件收集唯一顶点。
- 计算 3×3 对称协方差矩阵，使用固定 24 轮上限的 Jacobi 迭代求最小主轴。
- 把组件顶点投影到最小主轴，以投影跨度作为旋转不变的薄型尺寸代理。
- 新 thresholds：`min_component_thickness_mm = 0.8`、`min_thin_component_diagonal_mm = 2.0`。
- 新 metrics：`component_thickness_available`、`thin_component_count`、`minimum_component_thickness_mm`。
- 新 warning：`thin_structural_components`，只产生 `review`。

GUI 薄片验收同时修正了既有支撑信号误报：保留原 `downward_surface_ratio` 字段兼容旧报告，新增 `elevated_downward_surface_ratio`；支撑 warning 和新界面显示只统计接地带以上的向下表面，平放底面不再被当作悬垂。

## 自动化验证

- 红灯基线：实现前新增测试出现 4 个缺失 metrics 错误和 1 个版本断言失败。
- `test_printable_model_quality`：24/24 通过。
- 质量门与 Sidecar 契约定向：46/46 通过。
- 全量模型生成离线测试：226/226 通过，36.082 秒。
- 阶段 65 C++ 局部区域回归：13 个测试用例、125 个断言通过。
- Python 语法检查与 `git diff --check` 通过。
- Windows Release `OrcaSlicer` 构建与链接通过；仅有仓库既有 `LNK4098` 默认库警告。

新增测试覆盖：

- 20 × 10 × 0.4 mm 闭合薄片触发薄型提示。
- 同一薄片经过 Y/Z 双轴旋转后仍测得 0.4 mm。
- 2 × 2 × 2 mm 实心方块保持通过，不被体积/表面积代理误判。
- 开放薄片的厚度指标为未知，不产生薄型 warning。
- 平放薄片原始向下表面占比高于 35%，但离床向下表面为 0%，不再产生支撑 warning。

## 真实模型复核

对三个近期 282k–297k 面模型离线重算：

- 厚度指标均可用。
- 薄型组件数均为 0。
- 最薄连通组件尺寸分别为 63.75 mm、28.68 mm 和 47.92 mm。
- 状态保持 `review`，原因仍是既有局部悬垂和微小耗材色块；没有新增硬拒绝或薄型误报。
- 单次分析约 5.6–6.0 秒，没有观察到主轴计算导致的明显性能回退。

## 仓库本地 GUI 验收

验收应用为 `D:\Workspace\06_3DDY_claude\build\src\Release\orca-slicer.exe`，Sidecar 为本工作区 `tools/ai/orca_ai_sidecar.py`。

1. 真实 UUID 模型重新检查后显示“最薄组件：28.68 mm · 薄型组件：0 个”，没有薄型 warning。
2. 被忽略的 UUID 薄片样本包含 12 个三角面、8 个顶点，预览尺寸为 20.0 × 10.0 × 0.4 mm。
3. 重新检查后显示中文提示“检测到整体厚度较薄的连通部件，请检查是否需要加厚”。
4. 展开指标显示接地跨度 100%、接地面积 100%、离床向下表面 0%、显著局部悬垂 0 个、最薄组件 0.40 mm、薄型组件 1 个。
5. 修正前样本错误出现“向下表面较多”；改用离床指标后该支撑提示消失，只保留薄型和极端比例提示。

本次还观察到最小无顶点色 OBJ 无法进入 `ModelPreview3D`，添加合法顶点色后正常加载。正式模型生成制品均携带顶点色，因此不影响当前闭环；无顶点色预览兼容性应作为独立 GUI 修复处理。

Sidecar 日志只包含 `/health`、任务读取、制品读取和 `/recheck`，没有 Image2、Tripo、视觉复核或其他付费端点调用。

## 架构与兼容性

- 修改范围仅为模型生成 `AIModelGenerationClient`、`ModelGenerationPanel`、Sidecar 质量模块、测试和文档。
- 未修改 `MainFrame`、`Plater`、CMake 或其他共享文件；`MainFrame.cpp` 仅因头文件依赖被重编译。
- 未修改 3MF、profile、永久端口配置、依赖、输出目录约定或原版默认行为。
- 未引入智能切片业务，也未合并、反向移植或复制 `codex/orca-integration-v2` 的代码。
- 旧报告缺少厚度或离床指标时继续使用原显示路径；Sidecar 不可用时保持“尚未检查”。

## 下一步

阶段 68 应设计附着型局部薄壁/细连接的保守空间采样，仅在闭合流形模型和足够一致的表面对射结果下给出建议；同时可单独修复 `ModelPreview3D` 对无顶点色小型 OBJ 的兼容性。两者均保持在模型生成域，不接管 Orca 切片算法。
