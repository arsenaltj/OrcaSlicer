# 智能切片边界收口与 Release 硬化设计

## 目标

在不改变 ADR-002 行为和已验收历史的前提下，将智能切片正式写入和工作流装配从共享 `Plater.cpp` 收回 `GUI/AI/Orca`，修复兼容 Sidebar 的状态不一致，并基于当前 HEAD 生成可重复的本地 GUI 验收证据。

## 范围

- 允许修改智能切片 Domain/Application/Ports、`GUI/AI/SmartSlicing`、`GUI/AI/Orca` 和对应测试。
- 共享文件只允许为减少现有接入而修改 `Plater.cpp`、`Plater.hpp`、`MainFrame.cpp` 和 `src/slic3r/CMakeLists.txt`；每次提交说明原因。
- 不合并或反向移植 `codex/orca-integration-v2`，不复制或调整模型生成业务。
- 不修改 3MF/profile、默认切片行为、第三方依赖、网络端口或正式数据目录。
- GUI 验收只启动工作区内应用，并在启动前确认没有其他 Orca 进程。

## 架构

`SmartSlicingViewModel` 继续是右侧工作台和旧 Sidebar 的唯一状态来源。ReadyToApply 表示候选试切已经完成、等待用户决定；旧六步投影应显示模型/检查/颜色/摆盘完成，正式 Slice/G-code 等待。Applying 和 OfficialSlicing 则将正式 Slice 标记为运行中。

`OrcaOfficialSliceGateway` 新增一个接收 `Plater&` 的原生构造路径。候选目标解析、typed patch 复检、`Plater::TakeSnapshot`、正式 transform/config 写入、dirty/invalidation、Preview 与 Undo 都在 gateway 实现文件中完成。原有函数式构造保留，用于 Application 合同测试。

`OrcaSmartSlicingWorkbench` 随后集中拥有 adapter、executor、gateway、coordinator、runtime store、presenter 与 panel。`Plater` 只向它提供标准正式切片启动回调，并负责把 panel 加入现有 wxAUI 管理器；正式切片完成事件转发给 workbench。

## 数据流与失败处理

候选生成和试切仍只读取 GUI 线程捕获的副本。点击“确认并应用”后，Application 先校验 revision，gateway 再校验 revision 和 Orca 兼容性，随后创建一个 Undo snapshot 并执行正式写入。写入异常立即回滚；正式切片启动或完成失败保留一次 Undo。工作区变化、面板关闭、超时和取消继续清理 trial session，不改变正式工作区。

## 测试与发布验证

- ViewModel：ReadyToApply、Applying、OfficialSlicing、Completed、ApplyFailed 的四阶段和六步投影。
- Gateway：stale、兼容拒绝、无变化、transform/config 应用、异常回滚、一次 Undo、Preview。
- 结构：Domain/Application/Ports 不 include wx/Plater；`Plater.cpp` 不包含候选写循环。
- 构建：RelWithDebInfo `slic3rutils_tests`、`fff_print_tests`、`OrcaSlicer`、`OrcaSlicer_app_gui`。
- GUI：同步最新工作区验收包后，在可丢弃工程中验证关闭态、离线、候选、取消、stale、应用、Undo 和 Preview。

