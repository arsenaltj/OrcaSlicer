# OrcaSlicer UI Redesign Migration

日期：2026-09-22

## 目标

在不改变 OrcaSlicer 现有项目、模型、预设、切片、预览、打印机、AI 和撤销重做业务行为的前提下，逐步替换桌面 UI 的视觉风格和交互方式。

本计划采用绞杀式迁移：新界面作为展示层逐步接管旧界面，业务逻辑继续由现有 Orca 模块提供。每个阶段都必须可以独立编译、单独回退，并保留旧 UI 作为页面级回退路径。

## 当前基线

- 源码基线：`decf64fab128422f3666e71fe034fc411566bd6a`
- 当前分支：`codex/team/integration`
- 构建基线：Windows x64 Release，Visual Studio 17 2022
- 当前 UI 组合根：`src/slic3r/GUI/MainFrame.cpp`、`src/slic3r/GUI/MainFrame.hpp`
- 当前业务状态中心：`src/slic3r/GUI/Plater.cpp`、`src/slic3r/GUI/Plater.hpp`
- 现有可复用边界：`src/slic3r/GUI/AI/AIDesktopFeatureHost.*`、AI FeatureHost、Coordinator 和现有 ActionRegistry

## 风险控制原则

1. 不在 UI 重构中修改 `libslic3r`、项目格式、预设格式、后台切片线程、Provider 协议或 Undo/Redo 数据结构。
2. 新 UI 不直接复制模型、项目或切片状态；所有业务状态只有一个来源。
3. 新旧界面调用相同的稳定命令，使用相同的业务适配器。
4. 先建立命令和状态边界，再替换页面；先替换外壳，再替换深层控件。
5. 保留运行时 Feature Flag 和页面级回退，默认始终使用旧 UI。
6. 视觉验证、业务回归和真实主窗口验收分别记录，不用编译结果代替 GUI 验收。

## 分阶段路线

### UI-001：基线和边界（当前阶段）

- 固定旧 UI 的业务流程基线和输入资产。
- 新增独立的 `src/slic3r/GUI/Redesign/` 基础层。
- 新增默认关闭的 Feature Flag。
- 新增稳定 Command Registry 骨架。
- 新增只读 State Snapshot/Store 骨架。
- 新增不接管现有页面的空 Shell。
- 不改变 `MainFrame`、`Plater` 的现有行为。

### UI-002：业务适配器

- 将项目、模型、预设、切片、导出、打印和撤销重做操作包装成稳定命令。
- 将 `Plater` 内部状态映射为只读状态快照。
- 为命令的 `CanExecute`、成功结果和失败原因建立契约测试。

### UI-003：新 Shell 和导航

- 接入新 Shell，但默认仍走旧页面。
- 支持页面级开关和旧页面回退。
- 新旧页面共享全局命令、快捷键、状态和错误处理。

### UI-004：逐页替换

推荐顺序：

1. 首页和工作区入口
2. 全局导航和顶部工具栏
3. Preset 选择外围布局
4. Prepare 外壳
5. Sidebar
6. Preview 外壳
7. Device 页面
8. AI 页面
9. 设置、弹窗和异常提示

3D Canvas、Gizmo、对象树、选择同步和 Undo/Redo 最后迁移，初期继续嵌入旧控件。

### UI-005：旧 UI 收缩

- 新旧业务结果对照通过后，逐页关闭旧 UI 默认入口。
- 保留至少一个版本周期的回退开关。
- 删除旧页面前完成源码、测试、构建和实际主窗口验收记录。

## 必须覆盖的行为基线

- 新建、打开、保存和另存项目
- 导入 STL、OBJ、3MF 等模型
- 选择、移动、旋转、缩放、复制和删除
- Undo/Redo 与项目 dirty 状态
- 打印机、材料和工艺预设切换
- 开始、取消、失败恢复和后台切片
- Preview、G-code 导出和发送到打印机
- 关闭确认、自动恢复和重启恢复
- AI 生成、导入、取消、恢复和切页

## 验收门槛

- 命令契约测试：相同初始状态和命令序列产生相同业务状态。
- 新旧 UI 对照：项目、模型、预设、切片状态、G-code 和 Undo/Redo 结果一致。
- 真实主窗口：成功路径、取消路径、失败恢复、切页、导入和恢复均按本轮范围实测。
- 每个阶段记录源码 HEAD、构建配置、修改文件、测试日志、EXE/运行时身份和未验证范围。

## 当前实施记录

### 2026-09-22

- 已创建本计划文档。
- 已新增 `RedesignFeatureFlags`、`RedesignCommandRegistry`、`RedesignStateStore` 和 `RedesignShell` 的最小基础层。
- Feature Flag 默认关闭，当前未接管 `MainFrame` 或任何旧页面。
- 已将基础层加入 `libslic3r_gui` 构建，并为命令注册表和状态快照增加 `[UiRedesign]` 自动测试。
- Release 增量编译通过；`slic3rutils_tests.exe [UiRedesign]` 通过 4 个用例、24 个断言。
- 运行时 GUI 验收尚未执行；本轮没有启动主窗口，也没有改变旧 UI 的默认路径。
- 已新增 `OrcaBusinessAdapter`，将项目、导入、保存、删除、撤销重做、重新切片和 G-code 导出映射到现有 `Plater` 公共 API。
- 适配器不会注册打印机/材料/工艺选择、发送打印或后台切片取消，直到这些操作具备与旧 UI 等价的校验和生命周期语义。
- 已完成 Windows x64 Release 增量构建；`OrcaSlicer.dll` 和 `slic3rutils_tests.exe` 已生成，构建同时通过了隔离 AI runtime 的 Pillow 校验。
- 本轮验证源代码基线为提交 `7016255522` 及当前未提交适配器改动；主程序目标构建成功，但尚未启动主窗口，未执行 GUI 验收。
- 下一步是在不改变旧路径的前提下接入一个可回退的 Shell 入口。
