# 自动摆盘调优入口

本目录规划布局候选，产品 ArrangeJob 和智能候选适配器已经共用该入口。可先优化规划算法，应用/Undo/工程保存继续由 Orca 负责。

## 输入、输出和替换

`PlacementEngine.hpp` 的 `IPlacementEngine::plan(Request)` 接收 owned 轮廓、床面、固定/禁区、间距、方向与取消参数，返回对象ID对应的变换。`arrange()` 会剥离全部 live Model setter，只复制结果变换；不会提交原生工程。

`arrange(..., engine)`、`ArrangeJob(engine)` 和 `OrcaPlacementCandidateInput.engine` 可注入实现；默认 `baseline_engine()` 为 `native-arrange` / `native-arrange-v1`，继续调用原生 arrange。算法ID/版本随结果返回，调用期间实现固定。

## 可改范围

排列、方向和利用率策略改本目录及对应测试。新引擎必须保持对象ID集合、有限角度、床/固定/禁区、间距及允许旋转约束；当前智能候选只支持当前盘，不能假报多盘成功。坐标沿用 Orca 内核缩放单位，输入适配处才做mm换算。

失败/取消不能移动 live 工程，不能调用 snapshot 中的 setter。几何/方向改变后原切片需要重新验证；这是数据依赖，不需要重写页面。共享原生参数/适配器变更由统一接线任务处理。

## 单模块验证

在仓库根执行，只构建摆盘与原生几何依赖，不编译桌面 GUI：

```powershell
./dev.ps1 CppTest -TestSuite ai_placement_tests -TestLabel '.*' -Jobs 2
```

[接口样例](../../../../tests/ai_capabilities/test_placement_engine.cpp) 覆盖 setter 隔离、selected/fixed/禁区参数、未知对象ID和取消。原生几何/摆盘候选组合样例仍在 aggregate `ai_capabilities_tests` 的 `SmartSlicing` 标签内，修改布局语义后加跑该组合。

验收：固定布局对照基线，先看合法性与锁定保护，再看利用率、后续支撑/耗时。退回方式为移除 engine 注入，恢复默认 native-arrange。
