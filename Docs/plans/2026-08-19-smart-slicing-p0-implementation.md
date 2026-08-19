# Smart Slicing P0 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 建立无正式工程副作用的智能切片领域/应用/端口骨架、最小结构化预检和可停靠工作台。

**Architecture:** Domain 只包含值对象和稳定枚举；Application 通过窄 Ports 捕获只读工作区并驱动状态机；GUI/AI/Orca 适配器是唯一读取 Plater 与 Orca 配置的新增层。P0 不创建候选 Print，也不应用 Model/Config；P1 端口只保留扩展边界。

**Tech Stack:** C++17、wxWidgets/wxAUI、Catch2、CMake

---

### Task 1: Domain、Ports 与 Coordinator

**Files:**
- Create: `src/slic3r/AI/SmartSlicing/Domain/*.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Ports/*.hpp`
- Create: `src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp/.cpp`
- Test: `tests/slic3rutils/test_smart_slicing_coordinator.cpp`

1. 先编写 fake workspace 的失败测试，覆盖 start、阻断问题、cancel、revision stale。
2. 在 `WorkspaceRevision` 上实现完整相等比较；候选和报告始终携带 base revision。
3. 实现同步 P0 协调器；所有状态变化发布不可变 snapshot。
4. 运行 `slic3rutils_tests [AI][SmartSlicing]`，预期全部通过。

### Task 2: 只读 Orca Adapter

**Files:**
- Create: `src/slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp/.cpp`

1. 读取当前 plate、对象/实例/mesh 统计、printer/process/material preset 与有效配置标识。
2. 分别生成 model/config/plate 摘要，再组合为 fingerprint；不得触发 dirty、invalidation 或正式切片。
3. 最小预检输出空 plate、开放边、plate 外对象与已有 `Print::validate()` 结果；不可安全调用 validate 时降级到可用的确定性检查。

### Task 3: ViewModel、Presenter 与工作台壳

**Files:**
- Create: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp/.cpp`
- Create: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp/.cpp`
- Create: `src/slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp/.cpp`

1. 将内部状态投影为模型与材料、健康与准备、优化方案、检查并切片四阶段。
2. 提供开始、取消、重新检查和 stale 提示；P0 未实现的候选/应用操作保持禁用并有明确说明。
3. 用只读 revision 定时复核在途工作流，检测用户编辑造成的 stale。

### Task 4: Plater 集成与兼容清理

**Files:**
- Modify: `src/slic3r/GUI/Plater.hpp/.cpp`
- Modify: `src/slic3r/GUI/MainFrame.cpp`
- Modify: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`
- Modify: `src/slic3r/CMakeLists.txt`

1. 注册右侧 wxAUI pane 与 View 菜单入口，默认隐藏且 AI/Sidecar 离线时仍可用。
2. 让旧六步 Sidebar 只消费同一 ViewModel 的兼容投影。
3. 不接管或改变现有模型生成的导入并切片兼容路径；智能切片工作台自身不得写入 `Model`、`DynamicPrintConfig` 或正式切片结果。

### Task 5: Verification

1. 运行定向 Catch2 测试。
2. 编译 `slic3rutils_tests` 或可用的最小 Windows target；若本机缺少已配置 build，执行 CMake/source 静态校验并明确记录限制。
3. 检查 `git diff --check`、新增依赖方向、用户确认前零正式 Model/Config/Preview 写入。
4. 确认根目录 `task_plan.md`、`findings.md`、`progress.md` 未变化。

## Verification (2026-08-20)

- Configured an isolated Windows build in `build` using the existing Orca dependency install tree.
- Built `slic3rutils_tests`, `OrcaSlicer`, and `OrcaSlicer_app_gui` in Release mode with MSVC 19.44.
- Smart-slicing tests: 14 test cases, 73 assertions, all passed.
- Full `slic3rutils_tests`: 25 test cases, 180 assertions, all passed.
- Runtime smoke test reached the `Untitled - OrcaSlicer` editor window without a new crash log. Windows UI automation could read the editor tree, but the desktop session was locked and Windows refused to activate the wx/OpenGL window, so menu-level GUI journeys remain pending on an unlocked session.
- Read-only audit found no Model, DynamicPrintConfig, project-dirty, background-slice, or preview mutation call in the P0 smart-slicing adapter/application path.
- Existing model-generation import behavior remains unchanged and outside the smart-slicing coordinator path.
- The root `task_plan.md`, `findings.md`, and `progress.md` remained unmodified.
