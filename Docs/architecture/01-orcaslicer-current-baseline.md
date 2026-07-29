# OrcaSlicer 现状架构基线

> 基线：官方 `main @ a62fb17e03d159d5b562cc6d64163346e454b5de`。

![现状架构](diagrams/01-orcaslicer-current-architecture.svg)

## 1. 总体判断

OrcaSlicer 是一个跨平台桌面模块化单体：

- `libslic3r` 聚合模型、配置、格式、几何、FFF/SLA 切片和 G-code；
- `libslic3r_gui` 聚合 wxWidgets GUI、设备、网络、WebView、插件与平台能力；
- `GUI_App` 是应用组合根和全局服务入口；
- `MainFrame` 管理顶层导航、菜单和窗口生命周期；
- `Plater` 是 Prepare/Preview 工作流的核心应用协调器；
- `Model` 是编辑场景的权威数据源；
- FFF 模式下，每个 plate 拥有独立的 `Print` 和 `GCodeProcessorResult`；
- 设备输出分为传统 `PrintHost` 和持续连接型 `IPrinterAgent` 两套体系。

## 2. 核心用户旅程

### 2.1 启动与首次配置

```text
OrcaSlicer main
→ CLI::run
→ GUI_App::OnInit
→ 加载 AppConfig、资源、语言和 profiles
→ 创建 MainFrame / Plater / Device 页面
→ 首次配置向导
```

本地项目编辑和切片不依赖云登录；网络插件失败时，本地能力仍可运行。

### 2.2 新建、打开与导入

```text
New / Open / Import
→ 未保存项目与 preset 确认
→ 文件格式解析
→ ModelObject 规范化
→ 加入 Model、ObjectList 和 PartPlate
→ Undo/dirty 更新
→ 相关切片结果失效
```

`.3mf` 同时具有“打开项目”“仅导入几何”“加载配置”等语义。当前项目打开不是事务式切换：正式解析前会 reset 旧项目，加载失败后没有完整旧项目回滚。

### 2.3 Prepare 编辑

```text
GLCanvas3D / Gizmo
→ 临时选择和变换
→ do_move / do_rotate / do_scale
→ 写回 ModelInstance 或 ModelVolume
→ 更新 plate 归属和 ObjectList
→ 创建 Undo snapshot
→ 使切片步骤失效
```

场景模型：

```text
Model
└─ ModelObject
   ├─ ModelVolume     几何、部件类型、volume 变换、配置
   └─ ModelInstance   实例变换、可打印状态、plate 位置
```

世界变换为：`instance transform × volume transform × mesh`。

### 2.4 参数与预设

有效切片配置由以下来源合成：

```text
Printer preset
+ Process preset
+ Filament presets
+ Project config
+ Plate config
+ Object / Volume / Layer-range overrides
→ DynamicPrintConfig
```

切换预设还会处理兼容性、未保存变更、FFF/SLA 技术切换、对象位置、dirty 和切片失效。

### 2.5 切片与 Preview

```text
用户 Slice
→ Plater 校验并选择当前 plate 的 Print
→ BackgroundSlicingProcess::apply(Model, config)
→ Print::apply 增量同步并计算步骤失效
→ Print::process
→ PrintObject 各阶段
→ G-code 与 GCodeProcessorResult
→ slicing-completed event
→ Preview / GCodeViewer
```

主要 FFF 阶段包括切层、墙、Surface 分类、填充、熨烫、支撑、擦料塔、Skirt/Brim、路径简化和 G-code。

后台切片使用独立 Model 快照，不直接读取 GUI 正在编辑的场景。顶层阶段由切片线程编排，阶段内部通过 TBB 并行。

### 2.6 导出、发送与设备监控

传统上传：

```text
Plater::send_gcode_legacy
→ PrintHostJob
→ PrintHost queue
→ OctoPrint / Moonraker / PrusaLink 等适配器
```

持续设备连接：

```text
PrintJob / SendJob
→ NetworkAgent
→ IPrinterAgent
→ LAN / Cloud printer
```

设备状态：

```text
IPrinterAgent callback
→ GUI_App::CallAfter
→ DeviceManager
→ MachineObject::parse_json
→ Monitor / Status / Storage / Firmware / HMS
```

### 2.7 保存、恢复与关闭

```text
Plater::save_project
→ export_3mf
→ StoreParams
→ store_bbs_3mf
→ target.3mf.tmp
→ 成功后原子 rename
→ 清 dirty 和旧恢复备份
```

自动备份写入临时恢复目录，不覆盖用户项目。关闭窗口会依次处理 Gizmo 编辑、项目保存、preset 保存、上传队列、切片停止和全局服务清理。

## 3. 架构分层

| 层 | 核心职责 | 主要组件 |
|---|---|---|
| 应用壳 | 生命周期、资源、语言、服务装配 | `OrcaSlicer.cpp`、`GUI_App` |
| 顶层导航 | 页面、菜单、快捷键、关闭流程 | `MainFrame` |
| 应用工作流 | 项目、编辑、切片、预览、导出编排 | `Plater` |
| 编辑交互 | 3D 场景、对象树、Gizmo、参数侧栏 | `GLCanvas3D`、`ObjectList`、`Sidebar` |
| 场景域 | 对象、部件、实例、plate | `Model*`、`PartPlate*` |
| 配置域 | profiles、兼容性、有效配置 | `PresetBundle`、`DynamicPrintConfig` |
| 切片域 | 步骤状态、几何处理、路径生成 | `Print`、`PrintObject`、`PrintRegion` |
| 输出域 | G-code、统计、Preview | `GCode*`、`GCodeProcessorResult`、`libvgcode` |
| 持久化 | 3MF 与模型格式 | `libslic3r/Format` |
| 设备网络 | 发现、状态、上传、打印 | `DeviceManager`、`PrintHost`、`NetworkAgent` |

## 4. 状态所有权

| 状态范围 | 权威所有者 |
|---|---|
| 应用级 | `GUI_App`、`AppConfig`、`PresetBundle` |
| 项目级 | `Plater::Model`、Project config |
| 对象级 | `ModelObject`、`ModelVolume`、`ModelInstance` |
| Plate 级 | `PartPlate`、该 plate 的 `Print` 与 G-code result |
| 任务级 | `BackgroundSlicingProcess`、`Job/Worker` |
| 设备级 | `DeviceManager`、`MachineObject` |

新状态必须先确定归属范围，再决定持久化、Undo、dirty、线程和生命周期。

## 5. 关键兼容边界

1. `.3mf` 项目必须向后兼容；
2. Profile key、枚举、默认值、继承和兼容表达式属于公共 schema；
3. 新功能关闭时不得改变既有默认切片行为；
4. wx 控件只能在 GUI 线程更新；
5. 新配置必须声明准确的 `PrintStep` / `PrintObjectStep` 失效范围；
6. FFF 多 plate 各自拥有独立切片上下文；
7. 所有修改需要 Windows、macOS、Linux 验证；
8. 厂商协议应进入 `PrintHost`、`IPrinterAgent` 或 `ICloudServiceAgent`，不应进入 `Plater`。

## 6. 主要结构风险

- `GUI_App` 既是组合根也是 Service Locator；
- `Plater` 同时承担视图、模型所有权和跨域编排；
- 项目打开不是事务式切换；
- dirty、关闭 freshness 和 backup freshness 存在多套账本；
- 切片 invalidation 与后台线程停止存在严格锁顺序；
- 切片线程、TBB、通用 Worker、网络线程和上传队列多套并发体系并存；
- `MachineObject` 和 `ICloudServiceAgent` 职责较宽。

因此，新业务能力应优先形成窄接口、领域服务或 Job，再由现有协调器接入。
