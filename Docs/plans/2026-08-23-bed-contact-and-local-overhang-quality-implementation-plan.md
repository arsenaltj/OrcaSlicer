# 阶段 66：接地面积与局部悬垂预检实施计划

日期：2026-08-23

1. 为 `printable_model_quality.py` 增加失败测试：宽跨度针脚接触必须触发 `weak_bed_contact`；局部平台底面必须触发 `localized_overhang_regions`；正常宽底座保持通过。
2. 在既有面遍历中保留向下标记和 Z 范围，复用面邻接计算接地投影面积与局部悬垂连通域。
3. 输出 `structural-v4`、新 thresholds、metrics 和 warning；保持 schema 1 及旧报告容错。
4. 扩展 `AIModelGenerationClient` 的可选指标解析，并在 `ModelGenerationPanel` 显示接地面积、局部悬垂数量和中文提示。
5. 运行质量门定向测试、Sidecar 契约测试、全量离线 AI 测试、Release 构建和仓库本地 GUI 复核。
6. 补充阶段验收文档，列明共享文件、Provider 调用、格式和默认行为均无变化，提交并保持工作树干净。
