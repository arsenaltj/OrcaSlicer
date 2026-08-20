# 阶段 63 架构复核：悬空分离部件打印门禁

日期：2026-08-21

## 结论

本阶段补上确定性结构门禁的一处空白：与主体同样大的悬空独立部件不会触发原有“微小脱离件”规则。新规则只把明确没有热床或模型接触的分离部件标记为 `review`，不新增 `reject`，不移动、合并或修复几何，也不进入 Orca 智能切片业务。

## 契约

- 报告 schema 保持为 `1`，门禁版本由 `structural-v2` 提升为 `structural-v3`。
- 旧报告和未知 metrics 继续按既有 Sidecar/C++ 容错规则读取。
- 新阈值：`component_contact_tolerance_mm = 0.2`。
- 新 metrics：`floating_component_count`、`minimum_floating_clearance_mm`。
- 新 warning：`floating_disconnected_components`；只产生 `review`。

## 算法与误报控制

1. 复用现有拓扑组件及其 AABB，不重复解析 OBJ。
2. 最低点位于现有接地带内的组件视为已支撑。
3. 其余组件按最低 Z 从下到上处理。
4. 组件 AABB 与已支撑几何聚合 AABB 的距离不超过接触容差时，视为接触并扩展已支撑范围。
5. 只有存在明确正间隙的组件计入悬空分离部件。

聚合 AABB 会把嵌套壳、重叠材质壳、重复 OBJ 接缝和可能接触主体的部件优先视为已支撑。这是有意的保守策略：warning 门禁宁可漏掉模糊案例，也不因简化的包围盒分析误报并干扰用户。

## GUI 与边界

模型生成质量卡为新 warning 增加中文解释：“检测到未接触热床或主体的悬空分离部件，请检查是否可打印。”GUI 不读取阈值、不重新计算组件，也不改变导入硬门禁。

本阶段没有改动 3MF、profile、打印参数、支撑生成、模型摆放、切片逻辑或 Provider。质量分析仍是 Sidecar 制品准备后的本地只读步骤。

## 验证

- 失败前基线确认：等尺寸悬空组件原报告为 `pass`，新阈值/metrics/`structural-v3` 尚不存在。
- `test_printable_model_quality`：17/17 通过，覆盖等尺寸悬空组件和容差内独立壳体。
- 质量/视图定向：23/23 通过。
- Sidecar/可打印管线定向：26/26 通过。
- OBJ 生成回归：62/62 通过。
- 除已知 PowerShell readiness 启动器超时外的离线 AI 回归：193/193 通过；付费校验脚本测试使用 mock，没有外部付费调用。
- Python 语法检查通过。
- 阶段 45 人像、机器人和本地 `0c85a1eb` 三个高面数模型重新分析后，新增 `floating_component_count` 均为 0。
- Windows Release `OrcaSlicer` 编译链接通过；仅有仓库既有 `LNK4098` 默认库警告。

## 限制

该规则不是精确碰撞、壁厚或支撑可达性分析。AABB 重叠的内部漂浮物可能被保守放行；复杂接触判断和自动支撑属于后续几何/智能切片主线，不在本阶段扩展。
