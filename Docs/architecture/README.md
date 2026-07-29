# 架构资料总索引

## 1. 资料目的

这套资料用于两个阶段：

1. 建立不包含本地修改的 OrcaSlicer 官方现状架构基线；
2. 将 AI 模型生成、智能切片和交互优化嵌入现有架构，并进一步拆成开发工作包。

## 2. 分析基线

| 项目 | 值 |
|---|---|
| 官方仓库 | `OrcaSlicer/OrcaSlicer` |
| 分支 | `main` |
| 固定提交 | `a62fb17e03d159d5b562cc6d64163346e454b5de` |
| 提交日期 | 2026-07-25 |
| 本地改动 | 不纳入现状基线，也不作为目标架构依据 |

## 3. 推荐阅读顺序

### 面向产品和架构评审

1. [AI 模块级目标架构](03-ai-target-architecture.md)
2. [OrcaSlicer 现状架构基线](01-orcaslicer-current-baseline.md)
3. [开发任务拆解指南](04-development-decomposition-guide.md)

### 面向技术负责人和开发人员

1. [代码模块与修改入口](02-orcaslicer-code-map.md)
2. [ORCA 内嵌 AI 目标代码架构图](diagrams/03-orcaslicer-integrated-ai-target-architecture.svg)
3. [开发任务拆解指南](04-development-decomposition-guide.md)
4. [分析基线与证据索引](05-analysis-basis-and-evidence.md)

## 4. 图表目录

| 编号 | 图表 | 用途 | SVG | PNG |
|---|---|---|---|---|
| 01 | 现状用户旅程与技术架构 | 理解 ORCA 用户流程、分层、状态所有权与边界 | [SVG](diagrams/01-orcaslicer-current-architecture.svg) | [PNG](diagrams/01-orcaslicer-current-architecture.png) |
| 02 | 代码级类关系与扩展导航 | 定位现有关键类、所有权和修改入口 | [SVG](diagrams/02-orcaslicer-code-architecture-class-diagram.svg) | [PNG](diagrams/02-orcaslicer-code-architecture-class-diagram.png) |
| 03 | ORCA 内嵌 AI 目标代码架构 | 明确新增模块在 ORCA 包和代码层的位置 | [SVG](diagrams/03-orcaslicer-integrated-ai-target-architecture.svg) | [PNG](diagrams/03-orcaslicer-integrated-ai-target-architecture.png) |
| 04 | AI 模块级目标架构 | 用能力和模块视角进行方案评审、划分工作包 | [SVG](diagrams/04-orcaslicer-ai-module-target-architecture.svg) | [PNG](diagrams/04-orcaslicer-ai-module-target-architecture.png) |

`diagrams/archive/` 保存被后续方案替代的过程稿，不作为当前目标架构依据。

## 5. 颜色语义

- **蓝色实线**：ORCA 官方已有类、模块或业务流；
- **橙色虚线**：目标架构中拟新增的 AI 类、模块或能力流；
- **灰色**：外部系统、通用说明或非核心关系；
- 图中同时使用文字标签，不依赖颜色单独表达含义。

## 6. 核心架构原则

1. ORCA 的 `Model`、配置系统、`Print` 和 Preview 继续作为业务真值；
2. AI 产生建议、候选、诊断和生成资产，不建立第二套项目或切片核心；
3. 用户接受之前，不改变正式项目配置和正式切片结果；
4. 所有耗时工作复用 ORCA Job/Worker 或受控切片任务，不创建无生命周期管理的线程；
5. AI Provider 通过窄接口接入，应用代码不硬编码厂商协议；
6. 3MF、Profiles、默认切片行为和三平台兼容属于不可破坏边界。
