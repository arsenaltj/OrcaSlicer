# 阶段 87 轻量风格推荐架构复核

## 结论

本阶段把风格推荐限制在模型生成边界内：GUI 只负责呈现和用户选择，C++ 客户端只访问回环 Sidecar，Sidecar 使用 Pillow 和既有图片质量指标做确定性分析。没有把语义规则写入 Orca 切片核心、3MF、打印配置或 printer profile。

## 数据流

1. 用户选择图片，或历史任务恢复已保存原图。
2. `ModelGenerationPanel` 向 `AIModelGenerationClient` 提交图片路径和可选文字。
3. 客户端仅向 `127.0.0.1` 的 `/v1/orcaslicer/model-style-recommendation` 发送 multipart 请求。
4. Sidecar 在本机返回一个主推荐、两个备选、原因码、主体类别和 `local_only=true`。
5. GUI 将原因码翻译为短中文文案；新图片可自动选中主推荐，历史或用户手动选择不会被异步结果覆盖。

## 兼容与安全边界

- 六种正式风格使用稳定 ID：`cartoon`、`sculpture`、`low_poly`、`relief`、`realistic`、`diorama`；`custom` 保持独立。
- 旧 `q_cartoon`、`cel_shaded`、`enamel_inlay` 等别名继续由既有恢复边界归一，不修改历史 schema 或 3MF。
- 推荐接口只接受原生客户端和回环端点；不读取 API Key，不调用 Image2、Tripo 或远端视觉模型。
- 推荐失败不弹窗、不阻断生成，卡片直接隐藏；现有风格下拉始终可用。
- 推荐结果不持久化为新的项目格式字段，避免扩大向后兼容面。

## 识别策略

- 文字关键词优先覆盖人像、动物、平面图形、特效、建筑、硬表面、植物/食物和多主体场景。
- 无文字时使用主体连通性、颜色数、边缘密度和输入质量门禁。
- 人像兜底只依赖 Pillow：肤色范围、连通域面积、形状比例和上部中央构图必须同时成立；大面积棕色产品夹具用于防止单凭颜色误判。
- 低置信度保持可见的两个备选，不把推荐包装成强制结论。

## 验证

- Python 全量回归 491/491；真实无文字人像和非人像类别均有覆盖。
- Sidecar v7 本机真实 multipart 请求返回 `local_only=true`。
- Windows Release GUI 编译与完整 DLL 链接通过，最终 SHA-256 为 `934DDA00CA48FDD7BB30E2E0D928CECF39127E8F3B60452654DE2FB5294FC306`。
- 隔离 GUI 实机确认历史恢复、紧凑推荐卡、短原因文案、两个备选按钮和手动切换均正常。

## 后续约束

若未来引入更强的本地语义模型，应继续保持可选依赖、离线执行和同一返回契约；不得因推荐而静默触发付费服务，也不应把模型依赖扩散到 Orca 核心。
