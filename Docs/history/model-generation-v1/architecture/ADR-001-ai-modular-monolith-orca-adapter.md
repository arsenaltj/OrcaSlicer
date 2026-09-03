# ADR-001：同仓模块化单体与 Orca 防腐适配层

- 状态：提议
- 日期：2026-08-14
- 决策者：项目负责人、模型生成负责人、智能切片负责人

## 背景

当前模型生成和自动切片链路已经可运行，但 C++ 的主要职责集中在 `ModelGenerationPanel`，智能流程状态进入 `Plater`，Python 的主要职责集中在 `orca_ai_sidecar.py`。两名开发者若直接按功能继续修改，会同时竞争三个高冲突文件，并增加同步 Orca 官方版本的成本。

项目近期只并行推进模型生成和智能切片，交互系统、账号、计费后置；仍需交付一个 Windows 桌面程序和一个本地 sidecar。

## 决策

1. 保持一个 Git 仓库、一个 Orca 桌面进程和一个 loopback sidecar 进程；
2. 在 C++ 内建立 ModelGeneration 与 SmartSlicing 两个 bounded context；
3. 两域只通过不可变 `GeneratedModelArtifact` 单向交接；
4. 建立 `IOrcaWorkspace`，由 `OrcaWorkspaceAdapter` 隔离 `Plater`、Model、Config、Print 和 Preview；
5. sidecar 保持部署单体，但内部拆为模型生成、切片建议、供应商、制品处理和存储模块；
6. 保留当前 `/v1/orcaslicer/*` 为兼容门面；
7. `MainFrame`/`GUI_App` 只做装配，`Plater` 不保存 AI 工作流状态；
8. AI job 和报告不进入 `.3mf`；用户确认后的 mesh/配置继续走 Orca 原生持久化。

## 理由

- 能让两名开发者按目录和契约并行；
- 保留当前可演示链路，可逐步迁移；
- 避免多仓库、多版本和多安装单元的过早成本；
- 最大限度减少对 Orca 高频文件的长期修改；
- 未来账号、计费或远程任务平台进入时，已有稳定端口可拆成独立服务。

## 放弃的方案

### 继续扩展 Panel/Plater

短期最省改动，但无法解决并行冲突、测试困难和上游合并成本。

### 立即拆成多个仓库/服务

隔离更强，但当前团队规模和交付形态不值得承担独立发布、协议治理、安装、日志和调试成本；智能切片也无法脱离 Orca 核心单独运行。

## 后果

正面：

- 目录所有权和测试边界清晰；
- provider、UI 和 Orca 可以替换或升级；
- 现有 HTTP 与打包流程可继续使用；
- 上游冲突集中到少量 composition/adapter 文件。

代价：

- 需要先投入两到五天进行等价抽取；
- 迁移期会暂时存在 facade 和新旧代码并存；
- 共享 DTO/端口变更必须由双方评审；
- `OrcaWorkspaceAdapter` 会成为需要严格控制大小的新边界模块。

## 约束与检查

- domain/application 目录禁止包含 wx 或 provider SDK；
- 只有 `GUI/AI/Orca` 可以直接依赖 `Plater`；
- 旧 sidecar contract tests 必须持续通过；
- AI 关闭回归、旧 3MF/profile、普通导入/切片为每次上游同步门禁；
- `libslic3r` 的 AI 相关修改必须能解释为通用 Orca 能力，并独立测试。
