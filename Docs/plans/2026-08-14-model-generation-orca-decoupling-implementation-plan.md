# 模型生成与 Orca 解耦 Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** 在不改变现有演示行为的前提下，让模型生成页面不再直接持有 `Plater` 或实现 OBJ 导入、颜色映射、网格修复和切片编排。

**Architecture:** 模型生成侧通过稳定的 `GeneratedModelArtifact` 发布结果，通过 `IPrintablePaletteProvider` 只读获取打印色板，通过 `IModelArtifactConsumer` 提交用户确认后的导入请求。`OrcaWorkspaceAdapter` 是唯一理解 `Plater`、Preset、OBJ 颜色对话框和 CGAL 修复的实现；`MainFrame` 只负责构造并连接这些对象。

**Tech Stack:** C++17、wxWidgets、libslic3r、CMake、Catch2/现有回归、Windows Release。

---

## 实施说明

当前工作树包含本项目尚未提交的真实 AI 功能，不能创建一个缺失这些改动的独立 worktree。本计划在当前工作树小步执行，保留所有现有改动；每个任务均用 diff、构建和回归独立验证。本轮不执行 Git commit。

### Task 1：冻结模型生成边界契约

**Files:**

- Create: `src/slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp`
- Create: `src/slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp`
- Create: `src/slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp`
- Modify: `src/slic3r/CMakeLists.txt`

**Step 1:** 定义只含标准/Boost 数据类型的 `GeneratedModelArtifact`，包含本地路径、格式、颜色编码、job ID 和生成色板，不包含 wx、`Plater`、Model 或 provider SDK。

**Step 2:** 定义 `PrintablePaletteSnapshot`，同时提供有效槽位、温度兼容槽位和去重后的兼容颜色。

**Step 3:** 定义 `ModelImportRequest`、`ModelImportResult`、颜色策略枚举及 `IModelArtifactConsumer`；结果必须能表达导入失败、修复失败、手动修复、手动上色、映射退化和自动切片。

**Step 4:** 把头文件登记到 `src/slic3r/CMakeLists.txt`。

**Step 5:** 运行 `git diff --check`；预期无空白错误。

### Task 2：建立 OrcaWorkspaceAdapter

**Files:**

- Create: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp`
- Create: `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`
- Modify: `src/slic3r/CMakeLists.txt`

**Step 1:** 实现 `IPrintablePaletteProvider`，把原 `valid_project_slots()`、`compatible_project_slots()` 和 `project_palette()` 的行为等价迁移到 adapter。

**Step 2:** 实现 `IModelArtifactConsumer`，迁移自动颜色 mapper、原生 `ObjColorDialog`、snapshot/undo、开放边检测、CGAL 修复、手动导入降级和切片前配置收敛。

**Step 3:** Adapter 继续调用现有 Sidebar 六步展示和 MainFrame 导航回调，以保持本批用户行为不变；这些属于下一批要移除的兼容门面。

**Step 4:** 保证 adapter 的输入/输出不把 Orca 类型泄漏回契约头文件。

**Step 5:** 运行 `git diff --check`。

### Task 3：让 ModelGenerationPanel 去 Plater 化

**Files:**

- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

**Step 1:** 构造函数改为接收 `IModelArtifactConsumer` 与 `IPrintablePaletteProvider`，删除 `Plater*` 成员和 forward declaration。

**Step 2:** 下载完成后构造 `GeneratedModelArtifact`；用户点击导入时构造 typed `ModelImportRequest` 并交给 consumer。

**Step 3:** Panel 根据 typed result 更新现有中文状态、生成任务清理和按钮状态，不读取 Orca Model/Config/Sidebar。

**Step 4:** 色板 UI 攅读取 `PrintablePaletteSnapshot`，删除 Panel 内 Orca 温度兼容计算。

**Step 5:** 删除 Panel 中 `Plater`、Preset、ObjColorDialog、CGAL repair 等不再需要的 include/helper。

**Step 6:** 搜索验证：`rg -n "m_plater|Plater::|Sidebar::|ObjColorDialog|fix_model_with_cgal_gui|PresetBundle" src/slic3r/GUI/ModelGenerationPanel.*` 预期无匹配。

### Task 4：MainFrame 薄装配

**Files:**

- Modify: `src/slic3r/GUI/MainFrame.hpp`
- Modify: `src/slic3r/GUI/MainFrame.cpp`

**Step 1:** MainFrame 持有 `std::unique_ptr<OrcaWorkspaceAdapter>`，生命周期覆盖 ModelGenerationPanel。

**Step 2:** 使用现有导航/切片 lambda 构造 adapter，再把两个窄端口传给 Panel。

**Step 3:** 确认 MainFrame 不新增颜色、修复或业务状态判断。

**Step 4:** 运行 `git diff --check`。

### Task 5：编译与回归

**Files:**

- Modify if needed: `findings.md`
- Modify: `progress.md`
- Modify: `task_plan.md`

**Step 1:** 构建 `libslic3r_gui` Windows Release；预期成功。

**Step 2:** 构建完整 `OrcaSlicer` Windows Release；预期成功。

**Step 3:** 运行现有 Python AI 全量测试和 `py_compile`；预期与阶段 35 基线一致且不发起收费请求。

**Step 4:** 运行可用的 C++ 定向测试；若当前构建未生成测试 target，记录为现有构建限制，不伪造通过结论。

**Step 5:** 运行 `git diff --check` 和依赖搜索，确认模型生成 Panel 不再直接依赖 Orca 工作区类型。

**Step 6:** 若正式 GUI 可安全启动，使用已有模型库条目验证：色板显示、模型预览、手动/自动/单色导入、自动切片开关；不新建收费任务。

## 执行结果（2026-08-14）

- Tasks 1–4 已完成：稳定契约、`OrcaWorkspaceAdapter`、Panel 去 `Plater` 化和 MainFrame 薄装配均已落地。
- 三个关键 C++ 翻译单元完成 Windows Release 定点编译；`libslic3r_gui.lib` 已重新归档，完整 `OrcaSlicer.dll` 已链接成功。
- AI Python 回归 90/90、`py_compile`、静态边界搜索和 `git diff --check` 通过；未调用收费 API。
- 当前构建树没有 CTest 目标（0 项），因此不声称执行 C++ 单测。
- 日常 Orca 实例带未保存标记；为保护用户状态，本轮未覆盖运行目录 DLL，也未进行新 DLL 的正式 GUI 验收。新产物保留在 `build/src/Release/OrcaSlicer.dll`。
