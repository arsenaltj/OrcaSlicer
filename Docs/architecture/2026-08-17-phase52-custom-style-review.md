# 阶段 52 架构复核：自定义图片生成风格

## 结论

本阶段保持了模型生成与 Orca 核心切片域的边界。自由风格文本只沿“页面 → 模型生成客户端 → Sidecar → 图片预处理器”传递，没有写入 3MF、打印配置、模型对象或 Orca 核心算法，也没有把 Image2/Tripo 供应商字段暴露到 C++ 页面。

## 依赖方向

```text
ModelGenerationPanel
  -> AIModelGenerationClient (通用 style/custom_style 契约)
    -> Loopback Sidecar
      -> openai_preprocessor (提示词策略与硬约束)
```

- `style=custom` 仍是稳定枚举值，自由文本使用独立 `custom_style`，没有形成不可解析的动态风格 ID。
- C++ 只负责交互、任务快照和状态恢复；1000 UTF-8 字节硬边界由 Sidecar 负责，UI 的 240 字符限制是更保守的前置约束。
- 预设风格发送空 `custom_style`，因此旧任务、旧调用方和预设提示模板保持兼容。
- 自定义文字只作为外观方向；四色、实体区域、最小特征、底座与可建模轮廓等打印硬约束仍由预处理器后置强调。

## 恢复与隔离

- `custom_style` 已进入任务 JSON 和公开任务状态，应用重启后可以恢复选项、文字和预览。
- 修改自定义文字会使当前输入与任务快照不匹配，旧预览不会被误认为新输入的结果。
- Mock 只更新能力枚举，不进入正式运行路径；正式 GUI 验收使用 Sidecar v5 和真实 Image2。

## 验证

- 自定义风格定向回归：36/36。
- AI Python 全量回归：179/179。
- Windows Release：`libslic3r_gui` 与完整 `OrcaSlicer` 构建通过。
- 正式 GUI：空值阻断、即时启用、预设往返草稿保留、四阶段图片预览、页面往返、3D 付费确认入口、应用重启恢复均通过。

## 保留债务

- Sidecar 仍直接调用现有供应商客户端；Provider Gateway 抽取是此前已记录的模型生成域债务，本阶段没有扩大该耦合。
- Image2 同步调用期间进度仍是粗粒度的 10% 等待，消息清楚但缺少流式进度；可在后续统一异步 Provider Gateway 时改善，不应为此侵入 Orca UI。
- Pillow `getdata()` 在测试中产生面向 2027 年的弃用警告，应作为独立维护项迁移到 `get_flattened_data()`，与本功能无行为关系。
