# R0–R2 人物配色反向约束初步实现

日期：2026-09-18  
分支：`codex/exp/mobile-sam-local-validation`  
基线：`78c577a616170ef79d88dc2ecde838542cad9509`

## 已实现

- R0：`semantic_color_validation` 为每个材质区域输出稳定 ID、语义标签、置信度、原色 RGB/Oklab、面数、表面积、候选槽位、整面/子面支持计数和接受/拒绝原因。输出位于每个模型的 `region-diagnostics.json`，并嵌入 `result.json`。
- R1：区域覆盖键为 `analysis_signature + material_center_id`。覆盖同时作用于该区域的整面和子面；过期签名、未知槽位和重复区域事务性拒绝。槽位重排、停用和恢复保留 intended slot。映射持久化升级到 v4，同时读取 v2/v3。
- R2：普通候选排序增加可审计成本函数：原色兼容 0.35、语义兼容 0.30、多视角支持 0.15、几何回投 0.10、连续性 0.10；人像角色偏好最多提供 0.10 加分，硬拒绝候选为无穷成本。既有眼白、嘴唇、眉毛和 R3k 虹膜保护仍优先。
- GUI 控制器的请求结构已可携带区域覆盖，覆盖参与同一识别缓存和槽位映射链路；未提供覆盖时保持旧行为。

## 验证

- `SemanticColoring`：1041 断言／87 用例通过。
- `SemanticPaletteMapping`：745 断言／26 用例通过。
- `SemanticBoundaryRefinement` + `SemanticMaterialRegions`：207 断言／46 用例通过。
- `ModelSemanticColoring`：125 断言／7 用例通过。
- `scripts/verify_ai_integration.py --json`：`ok=true`，无 findings。
- Release 主程序和诊断程序已重新编译。

## 运行限制

固定 runtime 可正常加载 MediaPipe 和 MobileSAM。对 `right.obj` 的 R0 运行已生成 `analysis-baseline.json`、`analysis-candidate.json` 和 `region-diagnostics.json`；高面数材料映射继续运行时工作集达到约 2.7 GB，已停止进程，未生成完整六面视觉结果。这是资源限制记录，不作为视觉验收通过证据。

当前评分中的多视角、几何和连续性字段已接入统一接口，但在现有材质中心发现阶段使用中性证据值 `1`；后续需要把真实区域统计接入这些字段后，才能宣称完成完整的反向约束闭环。
