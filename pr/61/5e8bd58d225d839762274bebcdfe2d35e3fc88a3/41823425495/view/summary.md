<!-- orca-architecture-impact -->
### 架构影响图

版本：`abb26f03848d` → `5e8bd58d225d`；模式：PR 提交。

直接修改 **36** 个文件，潜在关联 **5** 个文件；其中 **12** 个改动文件未做代码关系解析。

黄色＝直接修改，蓝色＝潜在关联；箭头由使用方指向依赖方。

```mermaid
flowchart LR
  slicing_sdk["独立切片 SDK · 修改 5 / 关联 4"]
  model_sdk["独立模型 SDK · 修改 23 / 关联 0"]
  honor_account["荣耀账号 · 修改 0 / 关联 0"]
  desktop["桌面与运行时 · 修改 0 / 关联 0"]
  generation["模型生成 · 修改 1 / 关联 0"]
  slicing["智能切片 · 修改 0 / 关联 0"]
  contracts["共享契约 · 修改 0 / 关联 0"]
  adapter["Orca 适配 · 修改 0 / 关联 0"]
  core["Orca 核心 · 修改 0 / 关联 0"]
  tests["测试 · 修改 0 / 关联 1"]
  engineering["开发与协作 · 修改 4 / 关联 0"]
  assets["资源与依赖 · 修改 0 / 关联 0"]
  unknown["待归属 · 修改 3 / 关联 0"]
  adapter -->|依赖| contracts
  adapter -->|依赖| core
  adapter -->|依赖| desktop
  adapter -->|依赖| generation
  adapter -->|依赖| slicing
  desktop -->|依赖| adapter
  desktop -->|依赖| contracts
  desktop -->|依赖| core
  desktop -->|依赖| generation
  desktop -->|依赖| honor_account
  desktop -->|依赖| slicing
  generation -->|依赖| adapter
  generation -->|依赖| contracts
  generation -->|依赖| core
  generation -->|依赖| desktop
  generation -->|依赖| slicing
  honor_account -->|依赖| desktop
  model_sdk -->|新增| generation
  model_sdk -->|新增| slicing_sdk
  slicing -->|依赖| adapter
  slicing -->|依赖| contracts
  slicing -->|依赖| core
  slicing -->|依赖| desktop
  slicing -->|依赖| generation
  slicing_sdk -->|新增| adapter
  slicing_sdk -->|依赖| core
  slicing_sdk -->|依赖| slicing
  tests -->|依赖| adapter
  tests -->|依赖| contracts
  tests -->|依赖| core
  tests -->|依赖| desktop
  tests -->|依赖| generation
  tests -->|依赖| honor_account
  tests -->|依赖| slicing
  tests -->|依赖| slicing_sdk
  classDef changed fill:#fff0c2,stroke:#b87800,color:#332600
  classDef potential fill:#e3ecff,stroke:#4678cf,color:#162e55
  class slicing_sdk changed
  class model_sdk changed
  class generation changed
  class tests potential
  class engineering changed
  class unknown changed
```

- 按版本化 index_scope/index_exclude 解析 C/C++/Python；目录清单单独显示未解析文件，不据此推断无影响。
- 潜在影响最多沿静态依赖反向追踪两跳，同时保留基线中的删除关系。
- C++ include 按相对路径或唯一后缀推测；重复头文件不强行连接。未解析端点含外部库。
- wxWidgets 事件、动态调用及 HTTP 跨进程关系可能遗漏；图不代替代码审查或测试。
- 红色表示关系移除，不表示失败；绿色表示新增关系，不表示验证通过。

下载本次 Actions 的 `architecture-impact` 制品，解压后打开 `index.html`：L0 产品全貌 → L1 模块职责 → L2 业务状态 → L3 源码改动。`details.html` 保留完整静态关系，`report.json` 供 AI 阅读。Archify 总览展示选定主干；业务语义为维护的说明，状态名称从源码提取。这里没有实时运行或业务验收状态，布局校验不等于代码验证。
