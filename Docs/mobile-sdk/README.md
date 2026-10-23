# OrcaSlicer 移动 SDK 使用指南

当前分为三个独立包：`orca-mobile-device`、`orca-mobile-slicing`、`orca-mobile-ai`。前两个由 OrcaSlicer 生产跨平台制品，移动公共仓消费锁定产物并提供 Dart、Android JNI/Kotlin 和 iOS 桥接；AI 当前只保留无实现结构。

## 边界与构建

- Device 位于 `src/orca_mobile_device_sdk`，目标 `OrcaSlicer::MobileDeviceSDK` / `OrcaSlicer::MobileDeviceSDKShared`。设备发现/连接状态、命令、任务和事件属于该 SDK；纯契约目标只依赖标准库和 nlohmann_json，具体网络由宿主适配器提供。
- Slicing 位于 `src/orca_mobile_slicing_sdk` 与 `mobile-slicing` 构建入口，链接公共 `OrcaSlicer::Core`。桌面保留 `libslic3r` alias，源清单统一，Android 平台替换显式隔离。
- Android JNI 全部位于移动仓，只包括 Device/Slicing 公开 C ABI。SDK 生产目标不包含 jni.h、Java 类名或 Android 回调实现。
- AI 占位位于移动公共包，没有模型生成接口、实现或可调用二进制。

公开 C 头：

```c
#include <orca/mobile/device_sdk_c.h>
#include <orca/mobile/slicing_sdk_c.h>
```

C++ 宿主可以使用 Device 契约，命名空间为 `Orca::MobileDevice`：

```cpp
#include <orca/mobile/device/DeviceSDK.hpp>
using namespace Orca::MobileDevice;

ConnectionStateMachine connection;
connection.begin_discovery();
connection.begin_probe();
```

CMake 生成 `orca/mobile/device/MobileDeviceSDKBuildInfo.hpp`，源码身份和 ABI 应与制品锁一致。最小 Device 构建可使用 `-DORCA_MOBILE_DEVICE_SDK_ONLY=ON -DBUILD_TESTS=ON`；这不执行完整桌面构建。Android 切片从 `mobile-slicing` 配置，使用锁定 NDK/API/依赖并链接共享核心目标。

运行 `python scripts/verify_mobile_sdk_boundary.py --json` 与 `python scripts/verify_core_source_manifest.py` 检查边界和源清单。SDK 主机契约测试位于 `tests/mobile_sdk`；可通过 `ORCA_PLATFORM_TEST_BOOST_SOURCE_DIR` 指定固定 Boost 源头文件运行隔离 MD5 平台回归，不将 Boost 加入 Device SDK 的正式依赖图。

完整接口见 [API.md](API.md)，核心目标见 [CORE-TARGET.md](CORE-TARGET.md)，跨仓布局见 [PROJECT-TOPOLOGY.md](PROJECT-TOPOLOGY.md)。历史需求分析保留设计来源，其过时接口不作为当前实现承诺。桌面完整构建与 iOS 验收本轮分别按用户决定跳过/搁置。
