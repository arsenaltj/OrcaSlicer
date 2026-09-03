# ADR-0001：多视图生成保持在模型供应商边界内

## 状态

Accepted

## 背景

阶段 49 的四色和拓扑链路稳定，但单张参考图无法定义背面与侧面。Tripo v3 提供原生多视图建模接口；项目同时要求兼容 OrcaSlicer 原版演进和未来多供应商替换，因此不能把 Tripo 的视图键、文件 token 或 endpoint 暴露给 GUI 和 Orca adapter。

## 决策

领域层使用供应商无关的命名视图集合 `front / left / back / right`。实验阶段只在独立编排工具中验证，`tripo_client` 将命名视图映射为 Tripo 请求。正式产品接入时由模型生成 Gateway 接收可选 `reference_views`，Sidecar 编排状态机选择单图或多视图能力；Orca GUI 只看到“参考图已通过/需要复核”和最终 `GeneratedModelArtifact`。

## 后果

### 正面

- Orca 与 GUI 不依赖 Tripo API 细节。
- 单图、多图和未来其他供应商可以共享最终 OBJ、质量和恢复链路。
- 实验失败时删除独立工具即可，不污染正式用户流程。
- 付费任务状态与供应商网络协议可以独立测试。

### 负面

- 实验编排和正式 Sidecar 暂时存在两层状态记录。
- 正式接入前仍需抽取 Python 模型生成 Gateway，现有 Sidecar 对 `tripo_client` 的直接导入尚未完全消除。

### 中性

- 四视图参考会增加本地磁盘制品和 Image2/视觉复核调用，但不增加默认 Orca 工程文件格式字段。

## 备选方案

- 直接在 C++ GUI 上传四张图：拒绝，会把供应商工作流和付费恢复耦合进 Orca。
- 在 Sidecar 请求中暴露 Tripo token：拒绝，破坏安全边界与多供应商适配。
- 将四视图拼成一张继续使用单图 endpoint：拒绝，供应商语义不明确且无法可靠恢复视图角色。

## 非功能要求

- 可靠性：远端任务 ID 必须在任何非关键文件操作前持久化。
- 成本：本轮最多 2 次常规多视图 3D 生成，必要时才增加第 3 次。
- 安全：客户端不得传入 URL、file token 或任务目录外路径；密钥只从环境读取。
- 兼容：不修改 3MF、打印机配置或 Orca 原生模型格式。
- 可维护：供应商协议测试不得发起真实网络调用。

## 参考

- Tripo v3 `generation/multiview-to-model` 官方文档
- `Docs/MODEL_GENERATION_BLIND_PILOT_V1_REPORT.md`
