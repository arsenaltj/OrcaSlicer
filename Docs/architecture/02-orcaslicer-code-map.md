# 代码模块与修改入口

> 现有类关系详见：[代码级类图](diagrams/02-orcaslicer-code-architecture-class-diagram.svg)。
>
> AI 新增代码位置详见：[ORCA 内嵌 AI 目标代码架构](diagrams/03-orcaslicer-integrated-ai-target-architecture.svg)。

## 1. GUI 与应用编排

| 类/模块 | 现有职责 | 适合修改或扩展 | 不应放入 |
|---|---|---|---|
| `GUI_App` | 启动、全局服务、配置和网络生命周期 | 应用级服务装配、退出清理 | 场景编辑、AI 业务逻辑、几何算法 |
| `MainFrame` | 顶层页面、菜单、快捷键、命令可用性 | 新页面入口、导航和命令路由 | Model 变换、Provider 协议 |
| `Plater` | 项目、编辑、plate、切片和输出用例编排 | AI 用例的 ORCA facade、正式 apply 接入 | AI Provider、模型生成协议、修复算法 |
| `Sidebar` | 预设、对象和 plate 参数 UI | 轻量入口、选择范围和结果摘要 | AI 状态机和持久状态 |
| `GLCanvas3D` | 3D 渲染、选择、Gizmo、Preview | 诊断标记、修复差异、候选预览 | 项目真值、网络调用 |
| `ObjectList` | Model 的树形投影与选择同步 | AI 问题标记、对象范围选择 | 独立业务状态和切片调度 |

## 2. 项目与场景域

| 类 | 所有权/职责 | 修改规则 |
|---|---|---|
| `Model` | 场景聚合根，拥有 `ModelObject` | 新项目级持久状态需要复制、清理、3MF、Undo/dirty 联动 |
| `ModelObject` | 拥有 volumes 和 instances | 对象级诊断/修复目标应以对象 ID 或稳定引用表示 |
| `ModelVolume` | 几何、部件类型、volume 配置和变换 | 修复 mesh 后必须失效 bbox、hull 和切片缓存 |
| `ModelInstance` | 实例位置、旋转、缩放、可打印状态 | 自动摆放和方向建议应在用户确认后写回 |
| `PartPlateList` | 多 plate、当前 plate、逐 plate Print/result | 新 plate 级状态需明确复制、删除和切换规则 |
| `PartPlate` | plate 成员、配置、锁定和切片有效性 | AI 参数作用范围若为 plate，应在这里或 plate config 表达 |

世界变换：

```text
world = ModelInstance transform × ModelVolume transform × mesh
```

## 3. 配置与预设

| 类/模块 | 现有职责 | AI 接入方式 |
|---|---|---|
| `PresetBundle` | Printer/Filament/Process preset 和项目配置 | 提供当前有效配置与兼容性，不保存 Provider 原始输出 |
| `PresetCollection` | preset 选择、可见和兼容集合 | AI 若建议切换 preset，必须走既有选择确认 |
| `Preset` | 单个 preset 的配置、继承、厂商和 dirty | 不直接写系统 preset；建议形成可比较候选 |
| `DynamicPrintConfig` | UI 和运行时有效配置 | 只接收通过 key/type/range/scope 校验的候选 |
| `PrintConfigDef` / `PrintConfig.cpp` | option 定义、默认值和类型 | 新正式参数必须在此定义，并同步 profile schema 和 UI |
| `ModelConfig` | 对象/volume 配置和时间戳 | 对象级调参需通过它进入对象 override |

AI 参数应用必须经过：

```text
AI 原始建议
→ ConfigProposal 校验
→ CandidateConfig
→ 用户比较和接受
→ 目标 scope 的标准配置写入
→ dirty / Undo
→ Print::apply 失效计算
→ 正式切片
```

## 4. 切片与输出

| 类 | 现有职责 | 修改/扩展入口 |
|---|---|---|
| `PrintBase` | Model 快照、状态锁、取消和 status callback | 不直接加入 AI；保持切片任务协议稳定 |
| `Print` | Print 级配置、对象、区域和步骤状态 | 新正式切片行为、PrintStep 失效规则 |
| `PrintObject` | 切层、墙、填充、支撑等对象步骤 | 模型相关新算法和 PrintObjectStep 失效规则 |
| `PrintRegion` | 共享区域配置 | 影响区域配置的参数必须处理 region 重建/复用 |
| `PrintState<Step>` | INVALID/STARTED/DONE | 新步骤或失效关系需要同步状态机和取消行为 |
| `Layer/Surface` | 中间几何和 ExtrusionEntity | 并行修改需审查相邻层共享写 |
| `GCode` | 路径转 G-code | AI 通常不直接改写；正式新策略才进入此层 |
| `GCodeProcessorResult` | 路径、时间、耗材、警告和冲突 | AI 试切评分与 Preview 比较的主要数据来源 |

## 5. 后台任务

### 长期切片任务

`BackgroundSlicingProcess` 管理正式 FFF/SLA 切片线程、进度、取消和完成事件。

### 通用后台任务

```text
Job::process(Ctl&)   worker thread
Job::finalize()      GUI thread
Worker               queue / cancel / status
```

AI 模型生成、远端分析、模型检查和试切编排应使用受控 Job。`process()` 不得操作 wx 控件；UI 更新放 `finalize()` 或 `CallAfter`。

## 6. 持久化与格式

### 模型导入

```text
Model::read_from_archive/file/step
→ Model / ModelObject
→ Plater::load_model_objects
```

新增格式应在 `libslic3r/Format` 实现，Plater 只负责文件选择、加载策略、进度和错误反馈。

### 3MF 保存

```text
Plater::export_3mf
→ StoreParams
→ store_bbs_3mf
→ .tmp
→ atomic rename
```

新项目字段必须同时处理：

- StoreParams 穿透；
- 3MF writer；
- 3MF reader；
- 缺失字段默认值；
- 旧版本迁移；
- forward compatibility；
- Save/Load round-trip；
- backup/restore；
- dirty 和 Undo。

AI 任务历史、凭据和远端 job ID 默认不写入 3MF；只在产品明确要求跨设备继续任务时定义可移植 schema。

## 7. 设备与网络

### 传统文件上传协议

```text
PrintHostJob → PrintHost → host adapter
```

适用于上传文件和可选 StartPrint，不承担持续设备状态。

### 状态型打印机连接

```text
PrintJob / SendJob
→ NetworkAgent
→ IPrinterAgent
→ DeviceManager / MachineObject
```

适用于发现、连接、LAN/云消息和持续状态。新打印机协议通过 `NetworkAgentFactory` 注册，不应在 Plater 中按厂商分支。

### AI 服务

AI Provider 应使用独立的 provider-agnostic 接口和 `AIServiceManager`；不建议复用打印机 `NetworkAgent`，因为生命周期、认证、任务和错误语义不同。

## 8. 需求到修改入口

| 需求 | 首要修改位置 | 必须联动 |
|---|---|---|
| 新 AI 页面/交互 | `MainFrame`、AI workspace、`Plater` facade | 本地化、CMake、状态恢复、选择同步 |
| 模型生成 | Generation service、Provider gateway、Importer | Job、Model 导入、Undo/dirty、错误取消 |
| 模型检查 | `libslic3r/AI` preflight service | Model ID、诊断渲染、测试 |
| 模型修复 | Repair workflow + 现有 mesh/CGAL adapter | Undo、缓存失效、plate、重新切片 |
| AI 调参 | Context builder、Proposal service | Config 校验、scope、失效、dirty、正式切片 |
| 候选试切 | Tuning orchestrator、TrialSliceJob | 隔离 Print、资源限制、取消、result 比较 |
| 新预览指标 | `GCodeProcessorResult`、Viewer | 正式切片/试切、序列化需求、UI 对比 |
| 新项目字段 | Model/Project config | 3MF、migration、Undo、backup、round-trip |
