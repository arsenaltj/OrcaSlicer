# 阶段 81：模型 Provider Gateway 验收

日期：2026-08-26

## 结论

阶段 81 已把最终 Tripo 模型任务的创建、复用、轮询、转换和制品下载从 `orca_ai_sidecar.py` 抽到独立 `ModelProviderGateway`。Sidecar 继续拥有任务状态、持久化和 OBJ 后处理，但不再直接调用 Tripo adapter 函数。

供应商质量策略现在被明确表达为 GPT/Image2 负责理解和设计参考、Tripo 负责最终几何。该顺序是串行质量链路，不是自动 fallback；任一失败都不会隐式创建第二个付费模型任务或切换供应商。

## 付费授权与幂等

- `/generate` 只有在 job 状态、色板、prompt、面数目标和 Provider 配置全部校验后，才创建一次性 `PaidTaskAuthorization`。
- 授权限定 `tripo/model_generation`，只能消费一次；错误 provider、错误 operation 或重复消费均在任何 Provider 调用前拒绝。
- Sidecar 在创建远端任务前先持久化 `provider_request_id`、provider、operation 和 `creating` 状态，远端 ID 返回后立即写入 `generation_task_id`。
- 重启恢复不创建授权，只复用已保存的 generation/conversion task ID。若模型任务 ID 未保存，继续按旧安全行为失败并要求用户手动重新开始。
- 连接中断等可能已经到达供应商的创建错误标记为 `ambiguous=true`，记录证据但不自动重试。
- 底层只读任务轮询保留 Tripo 客户端原有短暂状态重试；Gateway 自身没有任务创建重试循环。

## 错误与降级

`ProviderGatewayError` 提供稳定的 `code`、`category`、`provider`、`operation`、`retryable` 和 `ambiguous` 字段。当前覆盖未配置、未授权、授权已消费、无效请求、限流、超时、取消、暂时不可用、供应商拒绝、不安全制品和无效供应商结果。

Sidecar 将结构化错误元数据保存到对应 attempt，同时继续向旧客户端提供既有安全 message/state。未知 attempt 字段和新增 health 字段均为可选，旧任务与旧 GUI 可安全忽略。

无 Tripo 凭据时，`model_generation.available=false`；Orca 原有编辑、导入、切片和历史模型浏览不受影响。

## 自动化验证

- Provider Gateway 定向测试：17/17 通过。
- Gateway、OBJ 生成和 Sidecar 契约组合回归：112/112 通过。
- 模型生成 Python 全量离线回归：275/275 通过，用时 36.197 秒。
- Python `py_compile` 通过。
- Windows Release `OrcaSlicer` 构建与链接通过。
- `git diff --check` 通过。
- 测试日志中的“paid Tripo”文本来自 mock 测试，没有外部 Provider 请求。

## 本地 capability smoke

使用本工作区 `tools/ai/orca_ai_sidecar.py`，显式清空 `OPENAI_API_KEY` 和 `TRIPO_API_KEY`，只请求一次 `/health`：

```json
{
  "available": false,
  "design_providers": ["gpt", "image2"],
  "geometry_provider": "tripo",
  "automatic_fallback": false,
  "max_paid_model_tasks_per_confirmation": 1
}
```

最初选择的临时端口 `18765` 已被另一个本机服务占用，请求命中对方 404；本阶段 Sidecar 随即停止，未触碰对方进程。改用事先确认空闲的临时端口 `28765` 后验证成功，Sidecar 日志只有 `GET /health`。结束后 `28765` 无监听，临时输出目录未被创建。

## 打包与变更范围

- Windows AI 测试包运行时清单和环境检查清单新增 `model_provider_gateway.py`。
- Sidecar health 的 `model_generation` 增加可选 `provider_policy`，协议版本和既有字段不变。
- 未修改 C++、GUI、MainFrame、Plater、CMake 或 Orca 工作区共享接口，因此本阶段无需重复交互式 GUI 验收。
- 未增加第三方依赖，未修改凭据格式、永久端口、默认输出目录、任务目录结构、3MF、profile 或原版默认行为。
- 未引入智能切片业务，也未合并、反向移植或复制 `codex/orca-integration-v2` 代码。
- 本阶段未调用真实 GPT、Image2、Tripo 或其他付费 API。

## 下一步

下一阶段把 Sidecar 中仍然直接调用的 GPT/Image2 预处理、色板推荐和设计参考生成机械迁入同一 Gateway，并为每个用户动作附带明确 operation context。完成后再运行 8–12 个用户批准的真实样本质量基准，对结构质量、视觉一致性、四色保持度和打印友好性进行联合评分。
