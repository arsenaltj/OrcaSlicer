# 阶段 67：薄型连通部件预检实施计划

日期：2026-08-23

1. 新增失败测试：轴对齐薄片和旋转薄片触发提示，2 mm 立方体保持通过，开放网格厚度指标为未知。
2. 实现小型确定性 3×3 对称矩阵 Jacobi 最小特征向量求解，并复用组件顶点计算最薄主轴投影。
3. 输出 `structural-v5` 新 thresholds/metrics/warning，保持 schema 1 和旧报告容错。
4. 扩展 `AIModelGenerationClient` 可选字段解析，并在质量卡显示最薄组件尺寸和中文提示。
5. 运行质量门、Sidecar 契约、全量离线测试、真实高面数模型复核和 Windows Release 构建。
6. 使用仓库本地应用和本地 `/recheck` 完成 GUI 验收；记录结果、提交并保持工作树干净。
