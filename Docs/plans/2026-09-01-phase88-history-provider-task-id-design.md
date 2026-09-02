# Phase 88：历史模型保留并复制 3D Task ID

## 目标

每个由远端 3D 服务生成并进入模型库的历史模型，都保留实际生成该模型的 Tripo task ID。历史卡片直接显示完整 ID，并提供一个紧邻的小号“复制”按钮。界面不新增页面、弹窗或高级设置。

## 数据流

Sidecar 已把每次远端调用的 `generation_task_id` 和 `conversion_task_id` 持久化在任务 attempts 中。本阶段只把最终有效尝试映射为公开状态中的 `provider_tasks`，由 `AIModelGenerationClient::JobStatus` 安全解析。模型下载并验证成功后，GUI 将 ID 与现有模型、原图、AI 图一起写入版本化历史元数据。转换 task ID 仅持久化用于排障，历史卡片默认展示生成 task ID。

旧历史元数据保持兼容：字段缺失时不显示复制行，也不伪造 ID。用户加载旧历史模型时，GUI 已经会查询本地 Sidecar 状态；若还能恢复该任务，就把 task ID 回填到历史元数据并刷新模型库。

## 交互与验证

历史卡片在尺寸、时间、颜色和素材信息下方增加一行 `3D Task ID：<完整 ID>`，右侧为“复制”。复制只写入本机剪贴板，并在页面底部显示轻量成功提示。需要验证 Sidecar 契约测试、Release 构建、历史元数据内容，以及正式 GUI 中完整 ID 的显示和复制行为。整个流程不得创建新的 Image2 或 Tripo 付费任务。
