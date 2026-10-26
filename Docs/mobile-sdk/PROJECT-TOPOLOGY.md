# 移动 SDK 跨仓布局

当前实现以公开 Device/Slicing C ABI 和 SDK 锁文件为边界。历史混合 SDK 的模型生成/切片接口已拆分，不再作为公共入口。

```text
OrcaSlicer/
  src/libslic3r/                  # 桌面与SDK共用核心，Core target / source manifest
  src/orca_mobile_device_sdk/     # Device公开头、协议、命令、任务、事件C ABI
  src/orca_mobile_slicing_sdk/    # 本地切片封装、公开C ABI、platform适配
  mobile-slicing/                # Android核心/切片构建及gate
  tests/mobile_sdk/              # Device契约、C ABI、线程与平台摘要回归

orca-mobile/
  packages/orca_mobile_device/   # Dart契约/桥接/能力
  packages/orca_mobile_slicing/  # Dart切片契约、制品锁
  packages/orca_mobile_ai/       # 无实现包、构建占位、能力未实现
  android/                      # JNI、Kotlin生命周期、SDK制品消费
  ios/                          # Pod/Swift/Objective-C++桥接与XCFramework接入
  sdk.lock.json                 # Device制品锁
```

Device 公共 C++ 入口为 `orca/mobile/device/DeviceSDK.hpp`，命名空间 `Orca::MobileDevice`；Device/Slicing C 入口分别是 `orca/mobile/device_sdk_c.h`、`orca/mobile/slicing_sdk_c.h`。AI 不提供具体接口或可调用产物。

桌面优化进入核心，SDK通过同一公共目标和清单构建；平台源替换、依赖差异显式声明。移动仓只消费发布制品，不能重新编译 libslic3r，也不能让 JNI 包含核心 C++ 私有类型。原生可选网络传输应独立于纯协议契约目标及移动平台适配，不把旧架构候选直接合入。

Android/iOS分别适配；正式二进制通过Release/制品存储分发，版本、提交、架构、工具链和SHA由制品清单及锁追溯。iOS本轮验收搁置，不能用目录完整代替编译或实机结果。
