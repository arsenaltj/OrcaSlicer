# Orca AI 模块解耦、固定集成与版本发布设计

**日期：** 2026-08-28
**状态：** 已确认，渐进实施
**适用分支：** `codex/orca-integration-v2`

## 1. 目标与约束

本次优化的目标不是重写 Orca，也不是把桌面端拆成多个微服务，而是在官方 Orca Git lineage 上建立一个可持续演进的模块化产品线。模型生成和智能切片继续独立开发；集成线只接收用户验收的完整 40 位 SHA，并承担共享 GUI、Orca 适配、上游同步和版本发布。所有阶段必须保持 Orca 原始功能在 AI 关闭、Sidecar 离线、授权失败和供应商故障时仍可正常使用。

硬约束包括：不改写已验收历史；不把集成线反向合并到功能分支；不修改 3MF/profile 格式或官方默认行为；不在仓库和商用安装包内放置供应商 Key；未经明确授权不发起付费任务；共享 `MainFrame`、`Plater` 与 CMake 只承担组合根职责。当前产品端口继续为 18764，Sidecar 保持 v8/protocol v2，开发端口只允许显式覆盖并隔离数据目录。

非功能目标是：功能团队可以并行提交且冲突集中在少量组合根；一次上游同步能自动给出共享热点和越界修改；内部包可以快速生成并带完整诊断身份；候选包和正式包使用不可变二进制晋级；故障能够沿 build/session/request/job/provider correlation chain 定位，同时默认不记录提示词、图片、模型和凭据。

## 2. 方案比较

### 方案 A：只写开发约定

优点是零改动、立即可用；缺点是约定无法阻止模块互相 include、共享文件继续膨胀或发布流程绕过固定 SHA。多人并行后，规则会逐渐退化为口头知识，因此不采用。

### 方案 B：一次性重写 GUI 与 Sidecar

优点是目录结构可以快速变整齐；缺点是 `ModelGenerationPanel.cpp` 和 `orca_ai_sidecar.py` 都超过 4,500 行，一次搬迁会同时影响 GUI 生命周期、任务恢复、安装打包和供应商调用，难以证明没有行为回归，也会让后续固定功能提交更难集成，因此不采用。

### 方案 C：受守护的渐进式模块化单体

先把当前耦合量写成机器可验证的上限，再引入中立契约和独立 CMake 目标，然后逐步迁移 GUI 编排与 Sidecar 子系统。每个阶段只允许预算下降，保持兼容入口，并以测试、Release 构建和 GUI 验收作为提交门槛。该方案能保留现有产品行为，同时把风险和冲突分散到可回退的小提交，是本项目采用的方案。

## 3. 目标架构

```text
Orca desktop executable
├── AI/Contracts                         # 中立 DTO/Ports，只由集成线治理
├── AI/ModelGeneration
│   ├── Domain
│   ├── Application
│   ├── Ports
│   └── Presentation/FeatureHost
├── AI/SmartSlicing
│   ├── Domain
│   ├── Application
│   ├── Ports
│   └── Presentation/FeatureHost
├── GUI/AI/Orca                          # OrcaWorkspaceAdapter 等具体适配器
└── MainFrame / Plater                   # 创建、挂载、导航、事件转发

Local Sidecar
├── runtime                              # server/auth/lifecycle/config/logging
├── jobs                                 # store/recovery/idempotency
├── model_generation
│   ├── routes
│   ├── service
│   ├── providers
│   └── pipelines
└── diagnostics                          # redacted doctor/support evidence
```

依赖方向固定为：GUI 组合根 → FeatureHost → Application → Domain/Contracts；Orca 具体类型只出现在适配器和组合根。模型生成与智能切片不得直接引用彼此的 GUI 或 Application，实现交接时只通过 `AI/Contracts`。Sidecar 保留 `tools/ai/orca_ai_sidecar.py` 兼容入口，内部模块拆分后仍由该入口装配，安装器、端口和既有 API 路由不变。

## 4. 集成数据流与失败处理

功能交付时，功能分支提供完整 SHA、相对上次验收摘要、共享文件清单和验证证据。集成线先读取锁文件，再验证该 SHA 属于指定分支历史、功能拥有路径的 Git object receipt 与验收提交一致，并拒绝移动 HEAD。固定顺序为：上游合并、模型生成摄入、智能切片摄入、组合根清理、发布身份提交。每一步独立提交，便于定位冲突来源和回退。

运行时由 FeatureHost 接收 UI 事件，通过 Port 请求 Sidecar 或 OrcaWorkspaceAdapter。Sidecar 为每次请求生成稳定错误码与 correlation id。供应商拒绝、代理错误、超时、任务恢复失败和协议不兼容只关闭对应 AI 操作，不改变当前 Orca 模型、配置或手工切片状态。付费创建采用幂等键；无法确认远端是否已创建任务时进入“需核对”状态，不自动重试产生第二笔费用。

发布失败同样 fail closed：缺少精确 SHA、签名、SBOM、来源证明、资格矩阵或受保护环境审批时不得创建正式发布。内部快速包明确标记为不可晋级；商用候选包生成不可变 manifest 和 hash；正式发布只提升已验收候选产物，不重新编译。

## 5. 测试与阶段验收

阶段 1 先建立架构预算：锁定两个超大文件的最大行数，以及相对锁定上游的 MainFrame、Plater 和 CMake 新增/净新增上限。迁出业务造成的删除不受阻；后续提交可以减少预算，若需要增加则必须显式修改机器可读锁并接受评审。阶段 2 用中立契约和独立 CMake 目标把依赖方向交给编译器检查。阶段 3/4 分别拆 GUI 与 Sidecar，每次只迁移一个职责并保留兼容入口。

每个阶段至少运行 `test_integration_guardrails.py`、全部 AI Python 测试和 `verify_ai_integration.py`。涉及 C++ 时增加目标 Catch2、Windows Release 增量构建和 AI-offline 回归；涉及 UI 时验证中文、首次配置向导、模型生成、智能切片和安全降级；涉及安装或发布时验证安装/升级/卸载、Sidecar 自动启动、日志/诊断目录、校验值、签名与隔离 Python。代表性旧 3MF/profile 在候选阶段必须完成打开、切片、保存对比。

商用生产仍受 ADR-004 的 P0 门槛约束：Provider Gateway、身份/权益/配额、无供应商 Key 安装包、付费任务对账、隐私/许可审查、签名/SBOM/来源证明、打印机矩阵、kill switch、回滚与支持流程缺一不可。
