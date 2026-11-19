# Windows 原生人像识别依赖

更新：2026-11-16。当前主线保留原生 MediaPipe；FaRL／Torch 研究运行时已退役。

生产构建继续使用 `cmake/OrcaSemanticRuntime.cmake` 及 `OrcaStageSemanticRuntime.cmake`：显式开启 `ORCA_AI_PORTRAIT_RECOGNITION` 和 `ORCA_ENABLE_MEDIAPIPE_NATIVE`，以 `ORCA_SEMANTIC_RUNTIME_DIR` 指向已验证的原生目录。校验八个封存文件、模型、DLL、provider 及许可证后装配到 `ai/portrait_semantics`。没有识别能力的构建保持关闭；不可用时沿现有功能门禁反馈，不启用旧研究链。

`ORCA_AI_LEGACY_FARL_RUNTIME`、`ORCA_BEAUTY_RUNTIME_ROOT`、`ORCA_BEAUTY_RASTER_BINARY` 不再是有效启用入口，旧缓存明确要求这些能力时配置报错。旧依赖准备、Torch／FaRL 权重及 raster 装配脚本已删除。能力清单 `legacy_farl_enabled` 保持兼容并固定为 false；运行校验拒绝退役 payload。

不删除用户已有运行时、权重、历史外观、细节锁、模型、缓存或研究证据。历史来源与失败报告通过 Git 及已有 artifacts 追溯，不能据旧文档重新开启执行链。仍有实际消费者的离线几何、边界分析和样例校验脚本保留，只用于独立诊断，不装入生产运行时。

同事开发复用各自 checkout 的固定构建目录和匹配依赖，不复制其他源码的 CMakeCache／对象文件。构建／运行及交付规则见 [快速开发](quick-development.md)和 [发布说明](../../release/README.md)。
