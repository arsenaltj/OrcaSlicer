# R3 眼部、眉毛与唇色边界修复记录

日期：2026-09-18  
分支：`codex/exp/mobile-sam-local-validation`  
基线：`orca.semantic-coloring/v22-protect-facial-details`

## 本轮修改

- 在最终整面映射后增加眼部邻接保护。眼白、虹膜、眼线及其一圈邻接面不能使用唇色槽；真实 `Lips` 与 `MouthInterior` 保留原映射。
- 对未获得 `Eyebrow` 标签、但位于眼部邻接且原色为深色的 `FaceSkin/BodySkin/Unknown` 面，使用人像卡眉毛中性色作为保守兜底。
- 对 `BodySkin/Unknown` 的头发边界，只有在原色呈黑棕且当前结果借用了肤色或唇色槽时才回退到头发槽；避免覆盖真实棕色眼部细节。
- 人像唇色槽增加空间局部性保护：远离嘴唇的皮肤/未知面不能继续使用唇色槽；衣服、头发和明确嘴唇区域不受该规则影响。

## 回归结果

- `[EyeColor],[EyebrowColor],[HairColor]`：35 个测试、272 个断言通过。
- `[SemanticColoring]`：84 个测试、1025 个断言通过。
- `[BoundaryColor]`：3 个测试、10 个断言通过。
- 新增覆盖：眼缘红色泄漏、深色眉毛兜底、头发边界借用肤色、远离嘴唇的脸部红槽回退。

## 真实样本复核

- 使用同一方位、同一 512 像素和同一 MobileSAM 输入重新生成刘亦菲六面图。
- 输出目录：`artifacts/local-semantic-validation-20260918-r3g-fallback`（刘亦菲、双人）；四模型基础对照仍在 `artifacts/local-semantic-validation-20260918-r3f-final`。
- 正面眼部局部：眼白保持白色，虹膜保留蓝色，眉毛保留中性灰棕，嘴唇仍为唇色。
- 预览回退层同步保护后，正面唇色 RGB 像素：刘亦菲 408 → 249，双人 973 → 322；该统计只表示渲染图中唇色槽像素总数，不等同于全模型准确率。
- 动态诊断运行与之前相同的 MobileSAM ROI；未改变模型、提示或视角条件。
- 视觉上仍保留发际线和耳后细锯齿，作为下一轮几何边界/抗锯齿处理项；本轮不宣称最终视觉验收通过。

## 程序

- 单元测试：`build-validation/tests/slic3rutils/Release/slic3rutils_tests.exe`
- 诊断程序：`build-validation/tests/slic3rutils/Release/semantic_color_validation.exe`
- 主程序：`build-validation/src/Release/orca-slicer.exe`
