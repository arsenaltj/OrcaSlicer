# R3h 耳发边界与耳区提示回退

日期：2026-09-18  
分支：`codex/exp/mobile-sam-local-validation`  
基线：R3g 回退层（`local-semantic-validation-20260918-r3g-fallback`）

## 本轮修改

- 耳 ROI 不再因为眼睛中心落入扩张窗口就整块拒绝。只有明确的小型矩形 ROI 完整覆盖眼睛时继续阻断；MediaPipe 产生的 32 点耳椭圆交给像素级 `allowed()` 保护眼白、虹膜和眉毛。
- 耳区正提示在 Face Landmarker 没有输出 FaceSkin 时，允许使用无冲突、高置信度的身体 FaceSkin；Hair 负提示仍必须独立存在。
- 其它部位的提示规则、MobileSAM 权重、裁剪尺寸和固定八视角条件保持不变。

## 回归测试

- `[SemanticBoundaryRefinement]`：14 个测试、99 个断言通过。
- `[SemanticColoring][EyeColor],[HairColor],[BoundaryColor]`：30 个测试、246 个断言通过。
- `[SemanticMaterialRegions]`：32 个测试、108 个断言通过。

## 固定模型重跑

输入：`artifacts/local-semantic-validation-20260917/fixtures`  
输出：`artifacts/local-semantic-validation-20260918-r3j-ear-seeds`  
运行资源：`artifacts/local-semantic-validation-20260918/runtime-candidate-g/ai/portrait_semantics`  
诊断程序：`build-validation/tests/slic3rutils/Release/semantic_color_validation.exe`

耳 ROI 统计：

| 样本 | 耳 ROI 总数 | accepted | rejected | changed pixels 合计 |
|---|---:|---:|---:|---:|
| 方飞（left） | 6 | 4 | 2 | 6,940 |
| 刘亦菲（right） | 6 | 5 | 1 | 14,158 |
| 双人（couple） | 12 | 6 | 6 | 约 12,480 |
| 动物 | 0 | 0 | 0 | 0 |

`changed pixels` 是 MobileSAM 耳 ROI 内被接受的像素数，不是准确率。双人和部分侧背视角仍因没有可靠皮肤／头发种子而回退基线；动物没有触发人像自动覆盖。

## 结论与下一轮

- 已修复一条通用链路瓶颈：粗身体皮肤可作为耳区提示，且不会覆盖明确五官细节。
- 当前轮次只算工程和算法候选验证完成，未通过最终视觉验收；仍需用户查看六面图确认耳发边界是否改善。
- 下一轮按顺序处理衣服/皮肤、头发/皮肤边界，再处理三级子面轮廓和五官抗锯齿。失败 ROI 继续使用 R3g 的安全结果。
