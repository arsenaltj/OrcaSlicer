# OrcaSlicer 架构资料

本目录汇总 OrcaSlicer 官方现状架构与 AI 目标架构分析成果。

## 快速入口

- [架构资料总索引](architecture/README.md)
- [OrcaSlicer 现状架构基线](architecture/01-orcaslicer-current-baseline.md)
- [代码模块与修改入口](architecture/02-orcaslicer-code-map.md)
- [AI 模块级目标架构](architecture/03-ai-target-architecture.md)
- [开发任务拆解指南](architecture/04-development-decomposition-guide.md)
- [分析基线与证据索引](architecture/05-analysis-basis-and-evidence.md)

## 智能切片交接

- [智能切片 P2 当前状态与后续开发交接](handoff/2026-08-25-smart-slicing-p2-status.md)
- [下一位 Codex 的智能切片开发提示词](handoff/2026-08-25-smart-slicing-codex-handoff-prompt.md)

## 核心图表

1. [现状用户旅程与技术架构](architecture/diagrams/01-orcaslicer-current-architecture.svg)
2. [代码级类关系与扩展导航](architecture/diagrams/02-orcaslicer-code-architecture-class-diagram.svg)
3. [ORCA 内嵌 AI 目标代码架构](architecture/diagrams/03-orcaslicer-integrated-ai-target-architecture.svg)
4. [AI 模块级目标架构](architecture/diagrams/04-orcaslicer-ai-module-target-architecture.svg)

现状架构基线类资料以 OrcaSlicer 官方 `main` 固定提交 `a62fb17e03d159d5b562cc6d64163346e454b5de` 为基线；功能交接资料以各文档明确记录的功能分支 SHA 为准。
