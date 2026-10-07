# 智能切片策略调优入口

本目录拥有现有协调器、建议及候选比较。优化参数搜索与评分时保留同一套试切、版本守卫和正式应用流程，UX 可以并行收尾。

## 输入、输出和替换

- 参数：`Ports/IParameterAdvisor.hpp` 的 `advise(WorkspaceContext)` → `ParameterProposal`。真实 OrcaSmartSlicingAdapter 已使用该端口；默认工厂在 `Application/BrimParameterAdvisor.hpp`。
- 评分：`Domain/CandidateComparison.hpp` 的 `CandidateScoringStrategy`，有效试切候选/目标 → 排序、推荐与证据。SmartSlicingCoordinator 构造时捕获评分函数/版本；默认空函数采用 `compare_candidates()`。
- 基线：`brim-stability-v1` 保留小底面/高细比时建议 brim5–10mm的原规则；`candidate-comparison-v1` 保留现有比较。新策略更改自己的ID/版本，退回默认工厂/空评分函数即可。

## 可改范围

参数建议、搜索、候选排序、时间/材料/质量目标改本目录的策略及对应测试。继续由 SmartSlicingCoordinator 拥有状态，试切/正式切片/Undo 留原端口及 Orca。优化引擎不得绕过失败指标、物理槽位兼容、已知候选ID、WorkspaceRevision 和显式应用守卫。

当前仅基线候选也可以是合法结果，但不能声称优化收益。更新工艺/方向后重验对应颜色资格和切片；不要用过期试切指标比较。共享策略字段或正式执行器的变更由统一接线任务处理。

## 单模块验证

在仓库根执行，只构建 SmartSlicing 中性算法，不构建网格/GUI，也不执行正式打印：

```powershell
./dev.ps1 CppTest -TestSuite ai_slicing_strategy_tests -TestLabel '.*' -Jobs 2
```

[接口样例](../../../../tests/ai_capabilities/test_smart_slicing_strategy.cpp) 使用真实协调器验证基线参数、替换评分、任务实现捕获、过期结果及不可用/物理不兼容候选拒绝。原生候选组合样例继续由 aggregate `ai_capabilities_tests` 验证；正式应用/取消/恢复另跑桌面 `SmartSlicing` 标签。

验收：与同输入的基线比较可解释试切指标、搜索成本和约束满足情况；必要的实物质量评价单列。旧正式交接复用完成结果的宿主修复已验证，算法调优无需修改该宿主回调。
