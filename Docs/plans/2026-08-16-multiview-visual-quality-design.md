# 多视角渲染与 AI 视觉复核设计

## 目标

在现有 OBJ 确定性结构门禁之后，增加一层可解释的视觉复核，帮助发现纯网格统计无法识别的问题：主体缺失、肢体/结构语义异常、底座关系不合理、明显漂浮物、轮廓难读和色块过碎。

第一版不承诺身份完全一致、精确壁厚、无需支撑或必然可打印；这些结论分别需要参考图对比、体素/壁厚分析和真实打印闭环。

## 方案选择

### 方案 A：Sidecar 本地确定性渲染 + 可选视觉模型（采用）

- 从最终 `model-vertex-color.obj` 生成前、后、左、右和等轴五视图。
- 视图与报告保存在任务目录，可用于历史模型复查和问题追溯。
- 用户显式点击“AI 视觉复核”后才调用视觉模型。
- 图生 3D 任务同时提供原参考图和五视图拼图；文生 3D 提供用户描述和拼图。
- Orca 只消费结构化结果，不承载渲染和模型供应商逻辑。

优点：与 Orca 核心切片代码解耦、可测试、可在无 GUI 环境运行、历史模型可复用。缺点：软件渲染不等同于 Orca 最终着色，需要保守解释。

### 方案 B：Orca OpenGL 离屏截图（暂不采用）

优点是视觉表现最接近当前 3D 画布；缺点是依赖 OpenGL 上下文、当前打印盘和 GUI 生命周期，自动测试和历史批量复核脆弱。

### 方案 C：直接使用 3D 供应商截图（暂不采用）

实现简单，但无法覆盖本地后处理后的最终 OBJ，也会绑定 Tripo 的任务和计费状态。

## 产物与契约

每个任务目录新增：

- `model-views/front.png`
- `model-views/back.png`
- `model-views/left.png`
- `model-views/right.png`
- `model-views/isometric.png`
- `model-view-sheet.png`
- `visual-quality.json`

`visual-quality.json` 至少包含：

- `schema_version`、`review_version`、OBJ SHA-256、生成时间；
- `status`: `pass`、`review` 或 `unavailable`；
- 总分、置信度、中文摘要；
- `subject_complete`、`semantic_coherence`、`base_relationship`、`detached_artifacts`、`silhouette_readability`、`color_region_clarity` 六项检查；
- 警告代码和视图清单；
- provider/model 信息，不保存密钥。

视觉模型第一版不能返回 `reject`，也不能覆盖 `model-quality.json`。只有确定性结构门禁的 `reject` 可以阻断导入。

## 调用、缓存与失败策略

- 只有用户显式点击时才发送图片，避免启动扫描历史库时产生费用或上传数据。
- 用最终 OBJ SHA-256、视觉提示版本和评分模型组成缓存键；同一结果重复点击优先复用。
- API 不可用、响应格式异常或渲染失败时记录 `unavailable`，保留模型和结构质量状态。
- 不自动创建新的 Image2 或 Tripo 任务。

## 首批验收

1. 合成彩色网格能稳定输出五视图和拼图，颜色来自 OBJ 顶点色。
2. 真实 30 至 50 万面模型渲染耗时可接受，文件全部位于对应任务目录。
3. API 合约测试覆盖显式调用、缓存、历史任务、安全路径和失败降级。
4. 正式 Orca 中能发起视觉复核、显示中文结果；结构 `reject` 仍阻断，视觉 `review/unavailable` 不阻断。
5. Python 全量回归、Windows Release 构建和正式 GUI 验证通过。

## 实施结果

- 已按方案 A 完成，阶段状态为 complete。
- 29.6 万面真实模型首次五视图约 12.8 秒；视图接口下载结果与落盘原件 SHA-256 一致。
- 完成一次真实视觉模型复核，得到 86 分 `review`，正确指出半身收尾缺少独立底座；未调用 Image2 或 Tripo。
- 正式 Orca 中文卡片、缓存复用、历史恢复和 advisory-only 导入规则均完成 GUI 验证。
- Python AI 回归 148/148，Windows Release 完整链接通过。
