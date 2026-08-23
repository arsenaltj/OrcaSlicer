# ADR-003：智能切片正式写入收口到 Orca Bridge

- 状态：Proposed
- 日期：2026-08-23
- 决策者：项目负责人、智能切片负责人

## 背景

ADR-002 的第一版主链路已经完成：候选绑定 `WorkspaceRevision`，试切运行在复制的 `Model`、`DynamicPrintConfig` 和独立 `Print` 上，用户确认后通过 `OrcaOfficialSliceGateway` 启动一次原生 Undo 事务和正式切片。

当前逻辑边界正确，但 `Plater.cpp` 仍包含候选目标解析、参数复检、事务写入、工作流对象装配和旧 Sidebar 投影，累计约 320 行。它们虽然只由 gateway 调用，物理位置仍扩大了共享文件的集成面，也使正式写入口不够直观。

## 决策

1. `OrcaOfficialSliceGateway` 提供面向 `Plater` 的原生构造路径；候选兼容复检、目标解析、单事务 Model/plate-config 写入、Preview 和 Undo 实现放在其 `.cpp` 中。
2. 新增 `OrcaSmartSlicingWorkbench`，集中拥有 workspace adapter、trial executor、official gateway、coordinator、runtime store、presenter 和 panel。
3. `Plater` 只保留三类薄接入：创建/停靠工作台、提供标准正式切片启动钩子、转发正式切片完成事件。
4. Domain/Application/Ports 不增加 wx、`Plater` 或 Provider 依赖；`OrcaSmartSlicingWorkbench` 和 gateway 实现只位于 `GUI/AI/Orca`。
5. 旧 Sidebar 继续作为同一 `SmartSlicingViewModel` 的只读兼容投影；ReadyToApply、Applying 和 OfficialSlicing 必须显示与右侧工作台一致的阶段状态。
6. 不增加持久配置、依赖、网络端口或 3MF/profile 字段。运行时元数据和临时 G-code 继续只位于系统临时目录。

## 备选方案

### 保持现状

运行风险最低，但不符合共享文件只做最薄接入的长期约束，后续候选类型会继续扩大 `Plater.cpp`。

### 只移动辅助函数

可减少一部分行数，但工作流生命周期和正式写入仍分散在 `Plater::priv` 与多个回调中，边界改善有限。

### 独立工作台与正式写入 Bridge（采用）

新增少量 Orca 适配层代码，换取唯一、可审计的正式写入口和更小的上游合并面。Domain/Application 接口保持不变，风险集中在 GUI 装配与生命周期，因此采用分步迁移和每步构建验证。

## 后果

- 正式工作区写入在调用路径和文件位置上都归属于 `OfficialSliceGateway`。
- `Plater.cpp` 的智能切片增量显著缩小，后续参数/方向候选无需继续修改共享文件。
- 工作台对象生命周期、后台取消和正式切片完成通知集中管理。
- 本轮会修改 `Plater.cpp`、`Plater.hpp` 和 `src/slic3r/CMakeLists.txt`；原因是减少既有共享接入，而非增加共享业务逻辑。

## 验收

- 现有正式应用、stale 拒绝、一次 Undo、Preview 合同测试保持通过。
- 新增 ReadyToApply/OfficialSlicing 的 legacy projection 测试。
- `Plater.cpp` 不再包含候选 transform/config 的具体写入循环。
- 功能关闭或工作台未打开时，原版切片路径不变。
- Windows RelWithDebInfo 测试与应用构建通过；GUI 只使用工作区内最新验收包。

