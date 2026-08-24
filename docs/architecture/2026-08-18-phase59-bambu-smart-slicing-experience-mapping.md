# 阶段 59：Bambu 打印经验与智能切片能力映射

> 日期：2026-08-18
> 范围：智能切片主线的资料研究、现状审计和实施建议；本阶段不修改业务代码。
> 结论：优先复用 Orca 已有确定性算法和真实切片指标，AI 只负责目标理解、候选解释和排序辅助。

## 1. 执行结论

Bambu Lab 官方 Wiki 中最值得迁移到本项目的，不是某几个固定参数值，而是以下工程模式：

1. 把方向、支撑、附着、冷却、多色等问题拆为可观测事实和有副作用的动作；
2. 先用确定性算法生成少量候选，再用真实切片数据比较；
3. 对材料、喷嘴、层高、擦料塔、支撑和打印顺序建立兼容约束，冲突时明确阻断；
4. 保留锁定对象、plate、对象/零件作用范围和用户手工覆盖；
5. 校准、耗材状态和硬件问题不能用切片参数掩盖；
6. 多色优化以逐层换料序列和真实冲刷量为准，而不是只看颜色数量；
7. 修复、简化、闭孔和冲刷到模型等可能改变语义或外观的动作必须先预览并确认。

本仓库已经继承了 Bambu/Orca 的多数底层能力。当前缺口主要是：稳定领域模型、非破坏候选、隔离试切、结构化指标、事务式应用和面向普通用户的中文解释层。

## 2. 当前智能切片基线

当前 `src/slic3r/AI/SmartSlicing/` 只有 `IModelArtifactConsumer.hpp`。`OrcaWorkspaceAdapter.cpp` 仍在一次同步调用中完成：

- 真实耗材色板读取与温度兼容槽筛选；
- 手动、自动、单色导入；
- 开放边布尔检查；
- CGAL 自动修复；
- 摆放到打印板；
- 正式 Process preset 修改；
- 自动切片触发和 Sidebar 六步状态更新。

已有能力值得保留，但当前结构无法表达候选、证据、作用范围、取消点和事务边界。特别是适配器会在用户确认前直接关闭 `independent_support_layer_height` 与 `enable_prime_tower`；这与目标架构冲突，也与 Bambu 官方把擦料塔作为多色摆盘/换料约束的经验相反。

## 3. 可迁移能力矩阵

| Bambu 经验 | 适用性 | 本项目落法 | Orca 可复用入口 | 优先级 |
|---|---|---|---|---|
| 多指标自动朝向 | 高 | 网格快速生成 3–5 个方向候选；试切后按支撑、桥接、多色废料、时间和外观风险重排 | `libslic3r/Orient.cpp`、`GUI/Jobs/OrientJob.cpp` | P1 |
| 锁定 plate/对象不参与自动变换 | 高 | `ArrangementContext` 显式携带 locked target 和 scope | `PartPlate`、`OrientJob`、`ArrangeJob` | P0 |
| 材料化自动摆盘 | 高 | 纳入间距、打印顺序、工具头避让、床温、排除区和擦料塔占位 | `GUI/Jobs/ArrangeJob.cpp`、`libslic3r/Arrange.*` | P1 |
| 自动 brim | 高 | 建立高度/底面积/材料收缩/速度驱动的 `BedAdhesionRisk`，试切比较 brim 代价 | 现有 brim 配置和生成逻辑 | P1 |
| 局部冷却与最小层时间 | 中高 | 从 G-code 统计层时间、风扇、外墙速度变化和悬垂路径；只生成受控候选 | `GCodeProcessorResult::moves` | P2 |
| 自适应层高 | 中高 | 作为质量/时间候选；先校验喷嘴范围、擦料塔和有机树兼容性 | layer height profile、`Print::validate()` | P2 |
| 支撑类型/样式分型 | 高 | 区分大平坦悬垂、复杂小悬垂、关键悬臂/尖尾和可桥接区域 | 支撑生成、`Print::validate()` | P1 |
| 异材支撑界面 | 高 | 从当前真实槽中选择兼容材料；比较表面质量、换料、冲刷和堵塞风险 | filament preset、support config、compatibility check | P2 |
| 特殊切片模式 | 高 | 将“奇偶/闭孔”作为非破坏切片候选，不把所有开放边自动修复 | slicing mode config、`Print::validate()` | P0/P1 |
| 接缝策略 | 中 | 需要主要可见面/背面输入；比较接缝距离和外观代理 | seam config、`GCodeProcessorResult` | P2 |
| 预览统计 | 很高 | 作为 `SlicingMetrics` 权威来源，不由 AI 猜测 | `PrintStatistics`、`GCodeProcessorResult` | P1 |
| 流量比例/动态流量校准 | 高，但设备相关 | 只做校准就绪度和上下文有效性检查；不自行替代硬件校准 | printer/filament calibration state（需适配） | P1/P2 |
| AMS 类型+颜色映射 | 很高 | source color → physical slot 显示材料兼容、色差、退化和手工覆盖 | 当前 palette provider、`ObjColorDialog` | P0 |
| 逐层换料与最小冲刷顺序 | 很高 | 直接采用现有序列/矩阵做确定性评分和排序 | `FilamentGroup.cpp`、`ToolOrderUtils.*`、flush matrix | P1 |
| 冲刷到填充/支撑/对象 | 中高 | 冲刷到支撑可作为低风险候选；填充/指定对象需透色或杂色确认 | wipe/flush configs、G-code stats | P2 |
| 高面数简化 | 有条件 | 只报告性能风险和可预览简化提案；绝不静默应用 | simplify gizmo/mesh simplification | P2 |
| 3MF Production Extension | 高 | 正式模型/配置继续走 Orca 原生保存；AI 报告和候选外置 | 现有 3MF implementation | P0 |

## 4. 第一版应建立的结构化事实

### 4.1 `PrintabilityReport`

第一版不追求一次覆盖所有算法，先稳定 issue 契约：

```cpp
struct PrintabilityIssue {
    std::string issue_code;
    IssueCategory category;
    IssueSeverity severity;
    StableTargetRef target;
    Evidence evidence;
    std::vector<SuggestedAction> suggested_actions;
    bool blocks_automatic_flow;
    bool may_change_geometry;
    bool may_change_color_semantics;
};
```

优先 issue code：

- `mesh.open_edges`
- `mesh.non_manifold`
- `mesh.degenerate_triangles`
- `mesh.multiple_disconnected_shells`
- `mesh.high_triangle_count`
- `geometry.possible_intentional_open_structure`
- `build_volume.out_of_bounds`
- `build_volume.height_exceeded`
- `placement.small_contact_area`
- `placement.high_tip_risk`
- `feature.thin_wall_risk`
- `overhang.critical_cantilever`
- `overhang.long_bridge`
- `color.mapping_collapsed`
- `color.material_incompatible`
- `config.prime_tower_compatibility`
- `config.variable_layer_tree_support_conflict`
- `calibration.context_missing_or_stale`

`Evidence` 必须保存数值和定位，例如开放边数量、三角面数量、bbox、接触面积、模型高度、最长桥接、对象/volume ID、option key；用户可见文本由 Presenter 本地化。

### 4.2 `ArrangementPlan`

```cpp
struct ArrangementCandidate {
    std::vector<ObjectTransform> transforms;
    double estimated_contact_area_mm2;
    double stability_risk;
    double estimated_overhang_area_mm2;
    double appearance_support_risk;
    bool respects_locks;
    bool fits_build_volume;
};
```

快速阶段只计算网格代理。最终推荐必须附带试切指标，特别是：

- 实际支撑体积和支撑接触代理；
- 桥接路径长度与最长跨度；
- 总层数和打印时间；
- 换料次数、冲刷量和擦料塔耗材；
- 因方向变化造成的强度/外观风险说明。

### 4.3 `SlicingMetrics`

第一版直接归一化现有结果：

```cpp
struct SlicingMetrics {
    double print_time_s;
    double prepare_time_s;
    double model_volume_mm3;
    double support_volume_mm3;
    double flush_volume_mm3;
    double wipe_tower_volume_mm3;
    double total_weight_g;
    uint32_t filament_changes;
    double tool_change_time_s;
    double travel_distance_mm;
    double seam_gap_distance_mm;
    double seam_scarf_distance_mm;
    std::vector<SlicingWarning> warnings;
    std::vector<LayerFilamentUsage> layer_filaments;
};
```

后续再从 `moves` 聚合最小层时间、体积流量利用率、外墙速度方差、悬垂/桥接路径和风扇覆盖等质量代理。

### 4.4 `ParameterProposal`

```cpp
struct ParameterChange {
    ScopeKind scope_kind;
    StableTargetRef target;
    PresetOwner owner;
    std::string key;
    ConfigValue old_value;
    ConfigValue new_value;
    std::string reason_code;
};
```

验证顺序保持：key → 类型 → 范围/枚举 → scope → printer/filament 兼容 → 禁止项 → 变更预算 → `Print::validate()` → 隔离试切。

## 5. 候选评分原则

不采用一个不透明的“AI 总分”。先保留硬门禁，再按用户目标显示 Pareto 对比：

### 硬门禁

- 构建空间、碰撞和不可切片错误；
- 颜色映射退化或材料不兼容；
- 修复丢失顶点色/多材料；
- 擦料塔、层高、支撑、喷嘴等组合不兼容；
- 未通过 schema、scope 和配置范围校验。

### 可比较目标

- 稳定性：接触面积、倾倒风险、首层/翘曲风险；
- 质量：支撑接触、桥接、接缝、曲面层纹、外墙速度一致性；
- 时间：总时间、准备时间、换料时间；
- 材料：模型、支撑、冲刷、擦料塔分别统计；
- 多色：逐层换料、颜色切换方向和外观污染风险；
- 可拆除性：brim、同材/异材支撑界面和 Z/XY 间隙。

默认最多生成 baseline + 2 个候选，避免 CPU、内存和用户认知负担失控。

## 6. 不应直接照搬的能力

1. **硬件自动校准算法**：激光雷达、涡流传感器和固件行为是设备能力；智能切片只读取状态和适用上下文。
2. **自动简化高面数模型**：可能丢失几何、顶点色和材料边界，必须由用户确认并保存新副本。
3. **统一自动闭孔或 CGAL 修复**：航模交叠薄壁、开放设计和功能孔可能是有意结构。
4. **用项目颜色覆盖真实耗材配置**：只允许 source → physical slot 映射，真实槽位是设备事实。
5. **自动降低冲刷乘数**：默认值偏保守是为了颜色/材料安全；没有测试证据不能静默降低。
6. **让 LLM 直接调流量比、K 值、硬件上限或自定义 G-code**：这些属于校准、安全或硬件边界。

## 7. 建议实施顺序

### P0：里程碑 1 + 最小结构化 Preflight

新增独占文件：

- `src/slic3r/AI/SmartSlicing/Domain/PrintabilityReport.hpp`
- `src/slic3r/AI/SmartSlicing/Domain/SmartSlicingRequest.hpp`
- `src/slic3r/AI/SmartSlicing/Application/IOrcaWorkspace.hpp`
- `src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.*`
- `tests/slic3rutils/test_smart_slicing_domain.cpp` 或等价独立测试目标

适配器内按步骤实现：import、color mapping、inspect、repair、arrange、official slice；第一轮保持现有入口可构建，但不再新增业务特判。最小 Preflight 先接开放边、三角面/组件统计、build volume、颜色退化和 `Print::validate()` 翻译。

当前“自动关闭独立支撑层高/擦料塔”应先封装为显式 legacy compatibility decision，并列为待确认迁移；在候选/事务机制建立后删除直接正式配置修改。若第一阶段就移除会改变现有自动切片行为，需要单独确认兼容策略。

### P1：方向/摆盘候选 + 隔离试切

- 建立 `ArrangementPlan` 与 locked/scope 约束；
- 复用 Orca Orient/Arrange 生成候选但不正式写回；
- 建立 baseline + 最多 2 个候选的 `ISliceExecutor`；
- 从 `GCodeProcessorResult` 提取第一版 `SlicingMetrics`；
- 先支持“稳定优先 / 质量优先 / 多色省料”三个明确目标；
- 用户确认后在一个 snapshot 中应用变换、配置并正式切片。

### P2：参数建议深化

- 把现有 26 项白名单校验抽到 provider-agnostic application/domain；
- 增加 object/part/modifier scope 和兼容图；
- 加入自动 brim、支撑类型、可变层高、冷却、接缝、多色减废候选；
- 增加校准就绪度，不自动执行硬件校准；
- Sidecar 只生成有 schema 的少量候选和中文解释，不接触正式 `DynamicPrintConfig`。

## 8. 文件所有权与冲突边界

### 智能切片独占修改

- `src/slic3r/AI/SmartSlicing/**`
- `src/slic3r/GUI/AI/SmartSlicing/**`
- `src/slic3r/GUI/AI/Orca/**`
- 智能切片测试和本设计后续文档

### 共享高冲突文件，只做薄接入

- `src/slic3r/CMakeLists.txt`
- `MainFrame.*`
- `GUI_App.*`
- `Plater.*`
- Shared DTO 和跨域交接接口

### 需要独立通用补丁评审

- `src/libslic3r/Orient.*` 候选结果 API
- `src/libslic3r/Print.*` 或 `GCodeProcessor*` 新统计字段
- 3MF/profile schema

优先用现有公开接口完成第一版。只有稳定 DTO 确实无法从 adapter 取得时，才向 `libslic3r` 增加通用、非 AI、可测试的查询能力。

## 9. 回归与验收重点

- AI 关闭时普通导入、切片、Preview、CLI、旧 3MF/profile 行为不变；
- 手动/自动/单色颜色策略、取消和映射退化全部保留；
- Preflight 只读，报告生成前后模型/config/Undo/dirty 不变；
- 特殊开放结构不会被统一自动修复；
- 候选试切不覆盖正式 `Print`、配置或 Preview；
- 锁定 plate/对象不会被方向或摆盘修改；
- 多色候选使用真实冲刷矩阵，擦料塔不会被隐式关闭；
- 修复/简化前后验证颜色、对象/组件、拓扑和体积变化；
- 正式应用只有一次 snapshot，失败/取消可完整回滚；
- Windows Release 为当前门禁，Domain/Application 保持三平台纯 C++ 边界。

## 10. 官方资料

- [Bambu Lab 软件目录](https://wiki.bambulab.com/zh/software)
- [自动朝向](https://wiki.bambulab.com/zh/software/bambu-studio/auto-orientation)
- [自动摆盘](https://wiki.bambulab.com/zh/software/bambu-studio/auto-arranging)
- [Brim](https://wiki.bambulab.com/zh/software/bambu-studio/auto-brim)
- [冷却模式](https://wiki.bambulab.com/zh/software/bambu-studio/auto-cooling)
- [可变层高](https://wiki.bambulab.com/zh/software/bambu-studio/adaptive-layer-height)
- [支撑功能](https://wiki.bambulab.com/zh/software/bambu-studio/support)
- [特殊切片模式](https://wiki.bambulab.com/zh/software/bambu-studio/special-slicing-modes)
- [接缝设置](https://wiki.bambulab.com/zh/software/bambu-studio/Seam)
- [查看切片信息](https://wiki.bambulab.com/zh/software/bambu-studio/view-slicing-information)
- [切片参数设置指南](https://wiki.bambulab.com/zh/software/bambu-studio/how-to-set-slicing-parameters)
- [流量比例校准](https://wiki.bambulab.com/zh/software/bambu-studio/calibration_flow_rate)
- [动态流量校准](https://wiki.bambulab.com/zh/software/bambu-studio/calibration_pa)
- [多色打印](https://wiki.bambulab.com/zh/software/bambu-studio/multi-color-printing)
- [减少多色打印材料浪费](https://wiki.bambulab.com/zh/software/bambu-studio/reduce-wasting-during-filament-change)
- [修复模型](https://wiki.bambulab.com/zh/software/bambu-studio/fix-model)
- [简化模型](https://wiki.bambulab.com/zh/software/bambu-studio/simplify-model)
- [3MF 兼容性说明](https://wiki.bambulab.com/zh/software/bambu-studio/3mf-compatibility)
