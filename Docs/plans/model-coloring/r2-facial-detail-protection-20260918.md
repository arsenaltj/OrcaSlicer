# R2 面部细节保护开发记录

日期：2026-09-18  
分支：`codex/exp/mobile-sam-local-validation`  
流水线版本：`orca.semantic-coloring/v22-protect-facial-details`

## 本轮修改

- 将眉毛、眼白、虹膜、嘴唇和口腔列为稳定面部细节。边界根面投票只能在同一细节族内细化，不能把已确认细节降为皮肤或头发。
- 三级边界叶子在追加前检查基线二级叶子；冲突时保留安全二级叶子，避免子面树出现重叠路径或覆盖眉毛/眼白。
- 合成阶段按现有色卡角色槽位恢复稳定面部根面，避免候选整面映射覆盖基线细节；不重新构建材质发现缓存。
- 语义流水线版本升到 v22，旧分析缓存不会混入本轮结果。
- 新增两个回归用例：边界头发候选不能覆盖稳定眉毛子面，不能覆盖稳定眼白根面。

## 验证结果

- `slic3rutils_tests.exe`：530 个用例通过，33 个 Python 环境相关用例跳过，0 失败。
- 定向语义测试：`[SemanticBoundaryRefinement]` 13 个用例通过；`[SemanticColoring]` 80 个用例通过；新增回归组通过。
- `semantic_color_validation.exe` 已用固定四模型、4 色、CPU、MobileSAM 重新生成六面 flat 对照。
- 动物：`person_detected=false`，边界 ROI 为 0，保持人像自动覆盖回退。

## 新一轮资产

目录：`D:\TEST\OrcaSlicer-mobile-sam-validation\artifacts\local-semantic-validation-20260918-r2`  
用户复核副本：`D:\测试验收\下一轮R2`

本轮 Release 程序：`D:\TEST\OrcaSlicer-mobile-sam-validation\build-validation\src\Release\orca-slicer.exe`  
对应诊断程序：`D:\TEST\OrcaSlicer-mobile-sam-validation\build-validation\tests\slic3rutils\Release\semantic_color_validation.exe`

每个模型目录包含原色/基线/候选 PPM、深度、面编号、分析 JSON 和局部 4 倍图；`contact-sheets` 下有四个模型的六视角 PNG。

本轮仍不宣称视觉验收通过。请重点复核：

1. 双人正面女方眉毛、双眼眼白及眉眼边界。
2. 刘亦菲左耳与头发交界，尤其是正面、左右侧面。
3. 方飞双耳、鼻孔、嘴唇是否保持原有材料。
4. 四模型后侧、顶部和底部是否出现新增色块。

抗锯齿和显示平滑尚未作为本轮主要修改，待上述语义保护确认后再进入下一轮。
