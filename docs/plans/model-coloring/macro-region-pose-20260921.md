# Pose 宏观区域分层检查点

## 范围

本检查点在 `codex/exp/mobile-sam-local-validation` 分支上，继承 `d57ff578f4`（B1a）。
B1a 保持“技术测试通过、视觉验收失败”状态，已知失败包括脸部灰带、颈部杂色、白衣肤色污染和发缘问题。

本轮接入 MediaPipe Pose Landmarker Lite 的可替换接口，建立 Face、Neck、LeftArm、RightArm、TorsoClothes、Hair、Accessory、Unknown
宏观区域和人物实例。Pose 结果加入分析签名、缓存和 JSON；缺失、损坏、取消或无可靠人物归属时回退 C1。

## 关键行为

- 未归属面继续使用 C1 的连通材料中心，不能借用已确认人物的皮肤中心。
- 皮肤缺口修复只在同一人物、同一宏观区域内传播；人物归属不明确时不传播。
- 半身像允许 Pose 关键点落在画面外；每个区域在使用前单独检查种子、置信度和范围。
- Pose 身份参与生产映射签名，预览、诊断工具和材料映射使用同一签名。
- 双人宏观候选分数接近时放弃该面归属，保留安全基线。

## 自动验证

相关 Catch2 测试：`175 test cases / 2088 assertions` 全部通过。

构建目标：

```text
cmake --build build-validation --config Release --target slic3rutils_tests semantic_color_validation --parallel 2
```

真实方飞诊断使用固定本地资源和四面/六面导出工具：

```text
build-validation\tests\slic3rutils\Release\semantic_color_validation.exe \
  --runtime .tmp\semantic-runtime \
  --fixtures artifacts\local-semantic-validation-20260917\fixtures \
  --output artifacts\macro-region-v27-20260921 \
  --model right --images-only --skip-palette-matrix
```

结果：本地 MediaPipe Body、Face、Pose 均实际推理；Pose 资源哈希为
`59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a`。方飞候选约 12.7 秒完成，
宏观区域已写入分析缓存，新增大面积白衣肤色污染消失。

## 当前限制与下一步

方飞正/侧/背对照仍显示部分侧背面保持安全基线，宏观区域图也表明未知面较多；这不是视觉验收通过。
刘亦菲、双人和动物尚未在本检查点完成完整六面回归。下一步按顺序验证四模型人物隔离、区域内 1～6 色映射，
再启用 MobileSAM 的耳发、眼白虹膜、眉毛和发际线三级子面边界。
