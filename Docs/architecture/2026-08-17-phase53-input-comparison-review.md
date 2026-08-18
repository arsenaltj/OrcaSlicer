# 阶段 53 架构复核：输入精简与图片对比

## 结论

本阶段仅调整 `ModelGenerationPanel` 的展示层、布局和既有图片状态选择，没有扩展模型生成契约，也没有让 Orca 核心对象感知 Image2、Tripo 或供应商字段。现有解耦边界保持不变。

## 变更边界

- 自适应文本高度由面板内部的 wxWidgets 布局辅助函数计算，使用控件字体、可用宽度和限定行数，不写入任务或项目配置。
- 高级设置只是把既有颜色角色和物理打印参数移入折叠容器；字段、事件和请求映射保持原实现。
- 图片对比复用已经下载的 `m_reference_image`、`m_raw_preview_image` 和当前阶段图片，没有新增文件格式、缓存类型或网络请求。
- 文生图以 AI 原图作为左侧基准，图生图仍以用户参考图作为左侧基准；该选择只发生在 GUI 位图渲染之前。

## Orca 演进兼容性

- 不修改 `.3mf`、打印机/耗材配置、模型对象和切片入口。
- 不修改 Sidecar HTTP 路由、任务 JSON 或 Provider Gateway。
- 折叠面板启用 `wxCP_NO_TLW_RESIZE`，避免 wxWidgets 默认行为影响 Orca 顶层窗口；仍使用跨平台 wxWidgets API。
- 改动集中在 `ModelGenerationPanel.cpp/.hpp`，未来跟随 Orca 上游演进时冲突面保持在独立 AI 页面。

## 验证

- Windows Release：`libslic3r_gui` 与完整 `OrcaSlicer` 构建通过。
- 正式 GUI：短/长文本高度即时变化；高级设置默认收起且展开不缩放主窗口；AI 原图、严格色板、可打印清理即时切换；页面往返后对比保持。
- 复用既有生成任务完成验证，没有新增付费调用。
