# 人像/模型生成调优入口

本目录定义不依赖 GUI 的生成 DTO、服务端口和薄客户端。供应商算法及预处理在现有 Python 网关内演进，继续使用统一 Job/模型资产输出。

## 输入、输出和替换

- `ModelGenerationTypes.hpp`：确认过的 `GenerationOptions`、描述/照片引用和请求/已有 Job ID → JobStatus、Task ID、产物路径与诊断。
- `IModelGenerationService.hpp` 保留现有22个操作，`ModelGenerationClient.hpp` 持有一个服务实现。构造时注入服务，客户端生命周期内固定该实现；实现 ID/版本与供应商身份分开。
- 桌面默认 `SidecarModelGenerationService` 位于 GUI 传输适配层，保留原 HTTP/线程/回调语义。算法任务通常改 `tools/ai/model_provider_gateway.py`、相应 provider/preprocessor 文件；无需修改页面或下游导入消费者。
- `GenerationOptions.provider` 仍走已有 Sidecar 路由。测试 RecordingGenerationService 只验证替换契约，不代表真实生成效果。

## 可改范围

可以优化供应商适配、提示预处理、生成和后处理策略。稳定 Job ID、真实 provider Task ID、产物格式/单位、确认/幂等及恢复规则。未知提交先查询，不能自动再提交；本地取消订阅和远端停止分别保留。共享协议及桌面传输改动由统一接线任务处理。

## 单模块验证

在仓库根执行，不构建网格/美颜/桌面 GUI，不发网络生成请求：

```powershell
./dev.ps1 CppTest -TestSuite ai_generation_tests -TestLabel '.*' -Jobs 2
./dev.ps1 Test -TestPattern test_model_provider_gateway.py
```

[接口样例](../../../../tests/ai_capabilities/test_generation_service.cpp) 覆盖确认参数传递、未知提交、已有任务恢复、下载、客户端销毁后的回调、本地取消/远端停止。先复用已有真实 Job 与模型评价相似度、完整性、耗时和成本；离线样本不等于新生成验收。新的付费生成需沿用明确授权，不因效果调优自动触发。

退回方式：使用原 provider 配置与默认 Sidecar 服务构造；不改写旧 Job 或已保存模型。完整 UX 输入/确认/生成旅程在 UX 收尾时合并验证。
