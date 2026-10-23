# OrcaSlicer MobileSDK 使用指南

`OrcaSlicer::MobileDeviceSDK` 是 OrcaSlicer 中面向移动端和其他宿主工程的无界面核心契约层。它位于 `src/orca_mobile_device_sdk`，只依赖 C++17、标准库和 `nlohmann_json`。

SDK 的目标是让桌面端、iOS、Android、Edge Connector 和云端客户端共享同一套数据模型与操作边界。桌面 UI、wxWidgets、Python、Bambu 专有 DLL、AI Provider Key、HTTP/WebSocket 实现和 WebRTC 实现都不属于这个目标。

## 架构边界

```text
移动端 Swift/Kotlin/C ABI 绑定
            │
            ▼
┌──────────────────────────┐
│ OrcaSlicer::MobileDeviceSDK     │  公共 DTO、状态机、命令契约、适配器接口
└──────────────┬───────────┘
               │
       ┌───────┴────────┐
       ▼                ▼
本地切片/协议适配器       AI Gateway / Relay 客户端
       │                │
       ▼                ▼
打印机或 libslic3r       云端服务
```

桌面端使用 `libslic3r_gui`、wxWidgets 或 Python 时，必须通过适配器实现 MobileSDK 定义的接口。MobileSDK 的公共头文件不能反向包含桌面类型。

## 构建和同步规则

1. `src/CMakeLists.txt` 始终加入 `orca_sdk`，桌面构建和移动 SDK 构建使用同一个 CMake 源码树。
2. `OrcaSlicer` 和 `libslic3r_gui` 都链接 `OrcaSlicer::MobileDeviceSDK`，因此桌面构建会编译并检查同一套 SDK 契约。
3. CMake 生成 `orca/sdk/MobileSDKBuildInfo.hpp`，其中包含源代码完整 commit 和短 commit。桌面端和移动 SDK 产物必须使用相同的 source identity。
4. 可移植的连接、任务和切片编排逻辑应进入 `src/orca_mobile_device_sdk` 或其他明确的无 UI Domain/Port；桌面 UI 只保留窗口、导航、线程调度和用户交互适配。
5. 商业 Gateway、厂商私有协议和专有 DLL 必须位于 SDK 外部适配器，不能成为开源 MobileSDK 的编译依赖。
6. `[MobileSDK]` 测试位于 `tests/mobile_sdk/test_mobile_sdk_contracts.cpp`，桌面分支和开源分支都应保留这组测试。

可以使用边界检查脚本验证依赖隔离：

```text
python scripts/verify_mobile_sdk_boundary.py --source-root . --json
```

在已配置的 CMake 环境中，`orca_mobile_device_sdk_boundary_check` 会作为构建目标执行同一检查。

## 最小示例

```cpp
#include <orca/mobile/device/DeviceSDK.hpp>

using namespace Orca::SDK;

ConnectionStateMachine connection;
connection.begin_discovery();
connection.begin_probe();

std::array<PathObservation, 3> paths{{
    {ConnectionPath::Lan, true, true, 4, 0},
    {ConnectionPath::RemoteBroker, false, false, 0, 0},
    {ConnectionPath::Relay, true, true, 120, 0},
}};

const PathSelection selected = connection.select_path(paths);
// selected.path == ConnectionPath::Lan
// connection.state() == ConnectionState::Connecting

CommandEnvelope command;
command.session_id = "session-id";
command.device_id = "printer-id";
command.command_id = "command-id";
command.sequence = 1;
command.expires_at_epoch_seconds = 2000000000;
command.type = "printer.pause";
command.payload = {{"reason", "user_request"}};

std::string error;
if (command.validate(/* now_epoch_seconds = */ 1999999999, &error) != CommandValidationError::None) {
    // reject the command before sending it
}
```

## 当前实现和后续适配器

当前核心提供：

- 连接路径和连接生命周期状态机；
- 打印机能力、端点、状态和协议适配器接口；
- 幂等控制命令信封；
- AI 任务、Provider 无关的模型产物和 Gateway 客户端接口；
- 本地/远程切片请求、结果、进度和引擎接口。

后续实现应分别放在独立 target：

- `orca_connectivity_moonraker`；
- `orca_connectivity_octoprint`；
- `orca_connectivity_prusalink`；
- `orca_ai_gateway_client`；
- `orca_slicing_local`；
- `orca_slicing_remote`；
- `orca_mobile_ios`；
- `orca_mobile_android`。

这些 target 可以依赖 MobileSDK，但 MobileSDK 不依赖它们。

## 开源分支覆盖

MobileSDK 的核心文件、契约测试和边界检查脚本都保留在公开源码树中。开源分支可以在没有商业 AI Gateway、厂商 DLL、真实打印机或云端账号的情况下构建契约层和运行离线测试。

新增商业适配器时应满足：

- 不修改公共 SDK 的开源默认行为；
- 不把私有凭据写入 MobileSDK 或公共测试；
- 通过独立 CMake target 注入；
- 保留没有商业适配器时的空实现、模拟实现或远程客户端边界；
- 让上游分支可以只合入 `src/orca_mobile_device_sdk`、测试和文档而不携带私有服务代码。

## 工程拓扑

移动端、桌面端、后端、SDK 产物和基础设施的仓库边界见 [PROJECT-TOPOLOGY.md](PROJECT-TOPOLOGY.md)。

## 需求分析报告

- [需求与三期开发规划 · Markdown](REQUIREMENTS-ANALYSIS.md)
- [需求与三期开发规划 · HTML](REQUIREMENTS-ANALYSIS.html)

以上文件是工程内同步阅读副本。用户审核的权威版本由工作区任务产物管理；本仓库文档不写入机器绝对路径。
