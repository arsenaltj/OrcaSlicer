# 阶段 70：采样式局部厚度预检设计

日期：2026-08-23

## 目标

识别阶段 67 无法覆盖的附着型薄壁和细连接：薄结构与大型主体属于同一个连通组件，因此组件整体主轴尺寸正常，但局部两侧表面距离已经低于 0.8 mm。

结果只提供复核建议，不自动加厚、删除几何或修改切片参数。

## 保守方案

1. 仅在模型无开放边、非流形边、绕序错误和退化面时启用。
2. 为三角面包围盒按中心 Morton 码排序，一次排序后构建固定叶容量的只读 BVH；不引入 NumPy、SciPy、trimesh 或新依赖。
3. 小模型检查全部面；高面数模型按 Morton 顺序做确定性空间分层采样，并补充一组最大面积面，采样总量有明确上限。
4. 从采样面重心沿法向和反法向各发射一条短射线，只搜索 0.8 mm 内的相对表面。
5. 排除自身、拓扑相邻面和其他连通组件；命中面法向必须与采样面明显相反。
6. 只有至少 2 个样本命中，且对应采样面积累计至少 1 mm² 时，才产生 `thin_local_wall_regions` 复核提示。

组件整体已被阶段 67 判定为薄型时不重复增加局部薄壁 warning，但仍保留采样 metrics 供诊断。

## 性能与失败模式

- 默认采样上限 4096，BVH 叶容量 24；射线搜索距离不超过薄壁阈值。
- BVH 采用 Morton 单次排序，避免 Python 递归中反复排序导致的高面数退化。
- 空间索引和采样结果只在单次分析内存在，不写入报告或制品。
- 无效拓扑、数值异常或索引构建失败时把 `local_thickness_available` 标为 false，不阻断已有门禁结果。
- 采样法可能漏掉极小薄区，因此“未发现”不等于切片级壁厚证明；但任何 warning 必须有明确的同组件对射证据。

## 契约

- 报告 schema 保持 `1`，门禁版本提升为 `structural-v6`。
- 新 thresholds：`min_local_wall_thickness_mm`、`local_thickness_sample_limit`、`local_thickness_bvh_leaf_size`、`min_thin_local_samples`、`min_thin_local_sample_area_mm2`、`max_opposing_normal_dot`。
- 新 metrics：`local_thickness_available`、`local_thickness_sample_count`、`thin_local_surface_sample_count`、`thin_local_sample_area_mm2`、`minimum_sampled_local_thickness_mm`。
- 新 warning：`thin_local_wall_regions`，只产生 `review`。

## 边界

实现只修改模型生成质量模块、Sidecar/C++ 模型生成契约、模型生成 GUI、测试和文档。不修改 Orca 壁厚、支撑或切片算法，不修改 3MF/profile，不调用 Provider。
