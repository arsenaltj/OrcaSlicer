# 阶段 60 架构复核：微小耗材颜色孤岛门禁

日期：2026-08-20

## 结论

本阶段在现有确定性 OBJ 结构门禁中增加颜色区域分析，用于提示会造成碎片化换色的微小耗材色块。新规则只产生 `review`，不新增 `reject`，不修改模型、调色板、打印配置或切片行为。

## 数据契约

- 报告 schema 继续为 `1`，门禁规则版本从 `structural-v1` 提升到 `structural-v2`。
- 旧 `structural-v1` 报告仍通过原有 Sidecar 与 C++ 客户端解析；客户端只消费已知 metrics，并保留 warnings/errors 数组。
- OBJ 只有在所有顶点都包含有效 RGB 时才启用颜色区域分析；无色、混合颜色字段或越界颜色只关闭颜色指标，不改变原结构结论。
- 同时支持归一化 `0..1` 和字节值 `0..255` 顶点色，并归一为稳定的 8 位颜色键。

新增阈值：

- `tiny_color_region_face_ratio = 0.0005`
- `tiny_color_region_area_ratio = 0.0001`

一个颜色区域必须同时低于面数占比和表面积占比才视为微小，避免把低面数但面积较大的风格化色块误报为噪声。

## 算法

1. 在既有 OBJ 解析中可选读取顶点 RGB。
2. 每个三角面以多数顶点色作为稳定标签；三种颜色各占一个顶点的混合面不强行分类。
3. 复用结构门禁的共享边数据建立面邻接。
4. 对精确重合但索引分离的两个边界半边补充几何邻接，避免 UV/材质接缝把同一色块拆碎；三面以上歧义边不连接。
5. 对相同颜色标签执行 BFS，统计区域面数与表面积占比。
6. 仅当模型实际使用两种以上颜色时启用微小色块 warning。

新增 metrics：

- `has_complete_vertex_colors`
- `printable_color_count`
- `color_region_count`
- `tiny_color_region_count`
- `smallest_color_region_face_ratio`
- `smallest_color_region_area_ratio`

## GUI 与边界

`ModelGenerationPanel` 只为 `tiny_printable_color_regions` 增加中文解释。它不读取新阈值、不重新分析网格，也不改变导入按钮的硬阻断条件。领域逻辑仍位于 `tools/ai/printable_model_quality.py`，Sidecar 无需复制实现。

## 测试与验证

新增 Python 测试源码覆盖：

- 两个面积接近的红/蓝大色区保持 `pass`。
- 极小蓝色连通区域触发 `tiny_printable_color_regions` 和 `review`。
- 无顶点色 OBJ 保持原结构行为且颜色 metrics 为零。
- `0..255` 顶点色归一为稳定颜色键；缺失或越界颜色关闭颜色指标但保持结构结论。
- 精确重复顶点接缝两侧的同色面仍统计为一个颜色区域。
- 报告写出使用 `structural-v2`，旧 Sidecar 契约 fixture 继续使用 `structural-v1`。

使用 Codex 随附的 Python 3.12 完成了以下离线验证：

- `python -m unittest tools.ai.test_printable_model_quality -v`：15/15 通过。
- `python -m unittest tools.ai.test_sidecar_contract -v`：16/16 通过。
- `python -m unittest tools.ai.test_obj_generation -v`：62/62 通过。
- `python -m unittest discover -s tools/ai -p 'test_*.py'`：185/185 通过；Provider 场景均为 localhost/mock 契约，没有真实付费请求。
- `python -m py_compile`：质量门实现和测试源码通过。

C++ 侧使用可读的 `D:\Windows Kits\10\10.0.26100.0` SDK 完成 Release `libslic3r_gui` 和 `OrcaSlicer` 编译/链接；质量卡新增 warning 文案已进入实际 GUI 编译。`git diff --check`、Python 全量回归、C++ 定向回归和静态契约审查均已完成。
