# 阶段 81：模型 Provider Gateway 设计

日期：2026-08-26

## 目标与边界

本阶段把最终模型生成中的供应商调用从 `orca_ai_sidecar.py` 抽到独立的 `ModelProviderGateway`，集中表达供应商顺序、付费任务授权、远端任务复用和错误分类。现有策略保持不变：GPT/Image2 负责理解与高质量设计参考，Tripo 负责最终几何；这是串行质量链路，不是失败后自动切换供应商。任何新的 Tripo 模型任务仍只能由用户在 GUI 中明确确认“生成 3D”后创建，失败后不自动创建第二个付费任务。

本阶段不改 Sidecar HTTP 路由、GUI 文案、任务状态名称、OBJ 后处理、Orca 导入契约、3MF/profile 或默认输出目录。OpenAI 预处理调用仍沿用现有实现；Gateway 先接管风险最高的 Tripo 任务创建、轮询、转换和下载边界，并在能力响应中只增加可选的供应商策略说明。后续阶段再把 GPT/Image2 的具体调用机械迁入同一边界，避免一次改动过大。

## 方案比较

1. **薄函数包装**：改动最小，但付费授权、恢复和错误判断仍散落在 Sidecar，无法形成真正的边界。
2. **中等粒度 Gateway（采用）**：以一个小型 Python 模块承接 Tripo adapter，Sidecar 只传 provider-neutral 请求、显式授权和已有远端 ID；网格处理继续留在模型生成域。
3. **动态 Provider 注册中心**：可支持任意供应商插件，但当前只有明确的 Image2/GPT→Tripo 链路，会过早引入配置、发现和选择复杂度。

采用方案 2。它能先解决付费调用与恢复的核心风险，同时保持现有测试夹具和发布结构可控。

## 组件与数据流

新增 `tools/ai/model_provider_gateway.py`：

- `ProviderPolicy`：声明 `design=gpt/image2`、`geometry=tripo`、`automatic_fallback=False`、单次确认最多创建一个模型任务。
- `PaidTaskAuthorization`：由 Sidecar 的 `/generate` 状态校验成功后创建；一次性消费，不能被重复使用。
- `ModelTaskRequest`：只包含来源、prompt、参考图和面数目标，不依赖 Sidecar `Job`。
- `ProviderTaskRef`：返回 provider、远端任务 ID、是否复用。
- `ProviderGatewayError`：提供稳定的 `code/category/retryable/ambiguous` 元数据和安全用户消息。
- `ModelProviderGateway`：负责配置可用性、创建或复用模型任务、创建或复用转换任务、轮询和下载。

新生成流程为：GUI 确认 → Sidecar 校验 job/palette/prompt → 创建一次性授权 → 先持久化 `creating` attempt 意图 → Gateway 消费授权并创建 Tripo 任务 → Sidecar 立即持久化远端 ID → 后续轮询、转换、下载均通过 Gateway。恢复流程不创建授权，只允许复用已保存的远端 ID；缺少 ID 时继续安全失败并要求用户手动重新开始。

## 错误、幂等与降级

Gateway 不重试任何任务创建，也不自动从 Tripo 切换到其他供应商。底层 Tripo 客户端对只读轮询的短暂状态重试保持不变。错误被分类为：未配置、未授权、无效请求、限流、超时/取消、暂时不可用、供应商拒绝、不安全或无效制品、未知供应商错误。创建阶段的连接中断或临时不可用标记为 `ambiguous=True`，表示供应商可能已收到请求；Sidecar 记录该证据但不自动重复收费请求。

幂等由三层共同保证：HTTP job 状态只允许一次 `/generate`；一次性授权只能消费一次；重启恢复必须已有远端任务 ID。Sidecar 在发起远端创建前先写入稳定 `provider_request_id`，如果进程在返回 ID 前中断，恢复时仍然失败为“引用未保存，请手动重新开始”，不会猜测或重发。

Gateway 或 Tripo 不可用时，模型生成能力标记不可用；Orca 原有编辑、导入、切片和历史模型浏览不受影响。未知的新增 health 字段由旧客户端自然忽略。

## 验证

- Gateway 纯单元测试覆盖策略、授权一次性、文本/图片创建、已有任务复用、无授权零调用、错误分类和无自动 fallback。
- Sidecar 回归覆盖新任务意图先持久化、成功后保存 task ID、恢复不创建付费任务、错误元数据记录、recheck/visual-review 零 Provider 调用。
- Python 全量离线测试与 `py_compile` 必须通过。
- Windows 测试包必须包含新模块，环境检查清单同步更新。
- 因无 C++/GUI 行为变化，本阶段只做 Sidecar health/任务契约 smoke 和 Windows Release 增量构建；若 C++ 文件没有变化，不重复做交互式 GUI 操作。
