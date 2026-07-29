# 分析基线与证据索引

## 1. 基线与方法

本资料的 ORCA 现状结论仅基于：

```text
repository: OrcaSlicer/OrcaSlicer
branch: main
commit: a62fb17e03d159d5b562cc6d64163346e454b5de
commit date: 2026-07-25
```

分析时使用独立的官方提交对象副本，没有以当前工作目录源码作为现状依据。目标 AI 架构是基于现状边界提出的设计建议，不代表官方已有实现，也不代表当前本地改动完成度。

## 2. 关键代码证据

下列路径和行号对应固定提交；后续分支变化可能使行号漂移，应优先按符号检索。

### 应用与 GUI

| 路径/符号 | 证据 |
|---|---|
| `src/OrcaSlicer.cpp` `CLI::run` | 应用入口和 CLI/GUI 分流 |
| `src/slic3r/GUI/GUI_App.cpp` `GUI_App::OnInit` | 应用初始化、服务和窗口创建 |
| `src/slic3r/GUI/MainFrame.cpp` `MainFrame` | 顶层页面、菜单和关闭流程 |
| `src/slic3r/GUI/Plater.cpp` `Plater::priv` | Model、plate、视图、后台切片和 worker 聚合 |
| `src/slic3r/GUI/GLCanvas3D.cpp` `do_move/do_rotate/do_scale` | 3D 交互提交到 Model |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | 对象树和 Canvas 选择同步 |

### Model 与 Plate

| 路径/符号 | 证据 |
|---|---|
| `src/libslic3r/Model.hpp` `Model` | 场景聚合根和对象所有权 |
| `src/libslic3r/Model.hpp` `ModelObject` | volumes、instances 和对象配置 |
| `src/libslic3r/Model.hpp` `ModelVolume` | mesh、volume transform 和配置 |
| `src/libslic3r/Model.hpp` `ModelInstance` | instance transform 和 printable 状态 |
| `src/slic3r/GUI/PartPlate.hpp` `PartPlate/PartPlateList` | 多 plate 状态和逐 plate Print/result |
| `src/slic3r/GUI/PartPlate.cpp` `select_plate/notify_instance_update` | plate 切换和对象归属更新 |

### 配置与预设

| 路径/符号 | 证据 |
|---|---|
| `src/libslic3r/PresetBundle.hpp/.cpp` | Printer/Filament/Process 和项目配置聚合 |
| `src/libslic3r/Preset.hpp/.cpp` | Preset、PresetCollection、继承和兼容性 |
| `src/libslic3r/PrintConfig.hpp` `DynamicPrintConfig` | UI/运行时动态切片配置 |
| `src/libslic3r/PrintConfig.cpp` | option 定义、默认值和配置规则 |
| `src/slic3r/GUI/Tab.cpp` `select_preset` | preset 切换、dirty 和兼容确认 |
| `src/slic3r/GUI/ProjectDirtyStateManager.*` | 项目、preset 和 project config dirty |

### 切片、并发与输出

| 路径/符号 | 证据 |
|---|---|
| `src/libslic3r/PrintBase.hpp` `PrintBase/PrintState` | Model 快照、步骤状态、取消和状态锁 |
| `src/libslic3r/Print.hpp` `Print/PrintObject/PrintRegion` | FFF 核心对象和步骤定义 |
| `src/libslic3r/Print.cpp` `Print::process` | FFF 顶层流水线 |
| `src/libslic3r/PrintApply.cpp` `Print::apply` | Model/config 增量同步和步骤失效 |
| `src/libslic3r/PrintObject.cpp` | 墙、填充、支撑等对象阶段和 TBB 并行 |
| `src/slic3r/GUI/BackgroundSlicingProcess.*` | 切片线程状态机、apply/start/stop |
| `src/slic3r/GUI/Jobs/Job.hpp` | `process` worker thread、`finalize` GUI thread |
| `src/slic3r/GUI/Jobs/Worker.hpp` | 队列、取消和等待协议 |
| `src/libslic3r/GCode/GCodeProcessor.hpp` `GCodeProcessorResult` | Preview 和调度使用的路径/统计结果 |
| `src/slic3r/GUI/GLCanvas3D.hpp` `load_gcode_preview` | result 进入 Preview/GCodeViewer |

### 格式、保存与恢复

| 路径/符号 | 证据 |
|---|---|
| `src/libslic3r/Model.*` `read_from_archive/file/step` | 3MF/STL/OBJ/STEP 等导入边界 |
| `src/slic3r/GUI/Plater.cpp` `load_files/load_model_objects` | 文件解析后的项目导入流程 |
| `src/slic3r/GUI/Plater.cpp` `save_project/export_3mf` | 项目保存和 StoreParams 组装 |
| `src/libslic3r/Format/bbs_3mf.hpp` `StoreParams` | 3MF 写入 DTO 和保存策略 |
| `src/libslic3r/Format/bbs_3mf.cpp` `store_bbs_3mf` | ZIP 内容、临时文件和原子 rename |
| `src/libslic3r/Format/bbs_3mf.cpp` backup manager | 定时备份和恢复目录 |

### 设备、网络与插件

| 路径/符号 | 证据 |
|---|---|
| `src/slic3r/Utils/PrintHost.*` | 传统文件上传抽象和 host adapter 工厂 |
| `src/slic3r/Utils/IPrinterAgent.hpp` | 发现、连接、发送和持续设备消息接口 |
| `src/slic3r/Utils/ICloudServiceAgent.hpp` | 认证、云 API、同步和 provider 回调 |
| `src/slic3r/Utils/NetworkAgent.*` | Printer/Cloud agent 组合 facade |
| `src/slic3r/Utils/NetworkAgentFactory.*` | agent registry、cache 和插件注册 |
| `src/slic3r/GUI/DeviceCore/DevManager.*` | 设备集合、发现和当前设备 |
| `src/slic3r/GUI/DeviceManager.*` `MachineObject::parse_json` | 设备状态解析和连接状态 |
| `src/slic3r/GUI/Monitor.*` | Device 页面轮询和状态投影 |
| `src/slic3r/GUI/PrinterWebView.*` | WebView 与 native bridge |

## 3. 已确认的关键实现事实

1. `Plater` 是应用工作流协调器，不是纯 View；
2. GUI 编辑 Model 与切片 Model 快照分离；
3. FFF 每 plate 拥有独立的 Print 和 G-code result；
4. 配置变更通过 `Print::apply` 计算步骤失效；
5. 切片线程与通用 Job Worker 是两套不同后台机制；
6. Job `process` 位于工作线程，`finalize` 位于 GUI 线程；
7. 3MF 保存先写 `.tmp`，成功后 rename；
8. 打印输出存在 PrintHost 和 IPrinterAgent 两套架构；
9. DeviceManager/MachineObject 是持续设备状态模型；
10. ORCA 已提供插件和 Python printer capability 注册边界。

## 4. 证据限制

- 固定提交副本最初采用 partial clone，个别几何、SLA 和执行策略 blob 在部分研究阶段未物化；
- 因此资料只对已经从可读源码核对的控制边界和符号作确定陈述；
- 没有把未核实的 SLA 内部步骤、所有 TBB 执行策略或每个几何算法细节写成事实；
- AI 目标模块名称是架构建议，不是官方现有符号；
- 进入详细设计时仍需对每个目标改动重新读取当前开发基线，尤其是 `PrintConfig`、3MF schema、`Print::apply` invalidation 和具体 GUI 事件。

## 5. 历史图说明

`diagrams/archive/` 中的文件是过程稿：

- `orcaslicer-orca-ai-increment-architecture.*`：早期将 AI 增量相对独立展示的版本，已被“ORCA 内嵌 AI 目标架构”替代；
- `orcaslicer-code-architecture-class-diagram-viewport.png`：早期视口截图，已被完整 PNG 替代。

当前评审应使用 `diagrams/01` 至 `diagrams/04`。
