# R0 区域诊断快照：2026-09-18

本轮只增强离线诊断程序，不改变语义识别、耗材映射或边界算法。

## 输出

`semantic_color_validation` 为每个六色候选模型写出
`<output>/<model>/region-diagnostics.json`，同时将相同数组嵌入
`result.json` 的 `region_diagnostics` 和 `six_color_candidate.region_diagnostics`。

每个区域记录：

- 稳定材质中心 ID、语义标签及可读标签名；
- 区域原始 RGB/Oklab、面数和表面积；
- 由当前分析面置信度汇总的诊断置信度；
- 当前选中槽位以及面级、子面级支持数量；
- 每个候选槽位的 RGB、Oklab 距离、支持数量、是否选中和原因。

`selected_by_region_mapping` 表示该候选在当前映射中获得最多区域支持；
`alternative_candidate` 表示候选存在但没有成为当前选择；
`no_region_assignment_evidence` 表示该中心没有可归属的整面或子面分配；
`boundary_budget_fallback_without_new_assignment` 表示边界预算回退时未产生新的区域分配。

## 解释边界

材质中心的生产 ID 与面级分配并未在旧公共结构中直接暴露。诊断程序因此按
“同语义标签 + 原色 Oklab 最近中心”重建面到中心的关系，仅用于解释和回溯，
不会参与实际配色。用户验收时仍应以六面图和程序预览为准。
