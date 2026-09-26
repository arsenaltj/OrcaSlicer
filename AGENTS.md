# 工程工作规则

## 入口与计划连续性

- 开始开发、恢复上下文或切换任务，读[短状态](docs/coordination/current-development.md)及[当前主计划](Docs/plans/2026-09-25-product-convergence-and-lightweight-plan.md)相关项。模块/命令查[工程导航](Docs/AI_ENGINEERING.md)与[快速开发](Docs/coordination/quick-development.md)，遵守目录内 AGENTS；不用通读历史文档。
- 新指令分为继续、补充、插队、正式变更、讨论。范围内补充挂原编号；插队先记恢复点，完成后在已有授权内返回；仅讨论不授权实施，明确的本轮范围限制优先。“继续”按短状态恢复，不从聊天末尾重猜。
- 最新明确决定优先，受影响旧任务不能静默消失。主计划保留稳定编号、目标、状态、验收、证据、负责会话/分支和修改范围；部分完成列剩余，延期列原因/恢复条件，取消或替代列决定/承接项。仅重大冲突且意图不明时询问。
- 可交付步骤结束或切换时增量更新主计划与短状态；短状态只留焦点、返回点、关键阻塞、下一步。完整状态仅维护于主计划，不新增 TODO/TASK/PLAN 台账。历史计划、报告及失败候选不是执行队列。
- 每轮交付附五行：主线、本轮、未结、下一步、计划变更。专项日志只作证据，不重复维护进度表。
- 两台 PC 按计划登记的会话/分支/文件范围协作。本机解耦工作须对应主计划目标，不默认接管另一台 UX。跨机结果注明版本、可取得证据及未同步部分；共享契约修改先记录双方范围和兼容要求。

## 开发与验证

- 当前交付是 Windows 内部开发版本。默认单代理完成实现、相关验证、失败修正与 diff 复核；按实际需要选技能，优先项目 `.agents/skills/` 同类版本。不自动增加代理、审批、提交、冷构建、全量测试或新报告。
- 复用本 checkout 的 `dev.ps1` 和 `.tmp/dev/{build,run,data}`；机器路径放忽略的 `.tmp/dev/settings.json`。同一构建目录单写者，不复制其他工作树缓存。构建期间不改源码；保留用户运行实例、工程、原始模型和旧工作区。
- 验证按行为选择：逻辑用定向自动测试；界面变化/明确主窗口验收用对应新版本的真实可见操作；持久化、撤销、导入、格式或共享接口覆盖相关消费者和失败路径。纯文档只查内容、引用和 diff，不启动应用。
- 编译、自动测试、GUI、视觉质量、实物打印分别记录，不能互相替代。记录源码/产物身份及验证范围；仅相关输入、依赖、条件变化或有新失败时补测。用户明确承接的视觉验收保持待反馈，不冒充通过。
- GUI 验证使用独立配置、模型副本和完整匹配的 EXE/DLL/资源/sidecar。真实生成经过软件主窗口的输入、确认、生成和历史恢复；离线 fixture 可测逻辑，不替代真实流程，不另建 mock 服务冒充验收。
- 查看/编辑/恢复复用已有资产；新生成沿用明确预算，不隐式付费重试。保留原件、真实任务 ID、历史引用和失败证据；配色的材质固有色、显示光照和实物偏差分别评价。
- 改架构边界、契约或 runtime 时用 `scripts/verify_ai_integration.py --json`，不弱化失败检查；阶段/能力/模块锚点变更更新 `Docs/architecture/review-map.json` 并用 `scripts/architecture_review.py --readme update` 同步生成区。文档整理不触发应用门禁。

## 产品与兼容边界

- AI 生成与智能切片在 Orca 主窗口内；Orca 管项目、配置、切片、G-code 与预览。导入不静默切片或改预设，智能切片建议须显式应用、可比较及撤销。AI 离线/关闭时保留普通 Orca 行为。
- C++17 / wxWidgets / CMake，保持可移植性、3MF、预设及手动流程兼容。`src/slic3r/AI/Contracts` 管跨模块契约，`src/slic3r/AI/SmartSlicing` 管决策，`src/slic3r/GUI/AI` 管宿主/适配器，`tools/ai` 管供应商；供应商策略不进入 libslic3r。共享边界另读 `docs/architecture/ai-integration-lock.json`。

## 按需交付与操作边界

- 本地开发/构建不以干净工作树、commit、push、PR、远程 CI 或团队通知为前置。内部包可用基线加完整哈希快照；明确需要时才读[交接流程](Docs/coordination/validation-handoff.md)和 `release/README.md`，使用 `release/build_internal.ps1`。
- 远端提交/集成仅在明确请求时按[团队 SOP](Docs/coordination/team-integration-sop.md)执行：特性分支 PR 指向 `codex/team/integration`，当前版本检查与非作者评审，维护者人工合入；PR 架构影响按[架构评审](Docs/coordination/architecture-review.md)生成。不会自动推送、合并、发布、联系测试者或购买生成。
- 工程规则不改变平台权限。区分脚本错误、未执行与平台拒绝；明确拒绝不得换工具、代理或入口绕过。历史 smart-slicing 最新源码对比的拒绝未解除，不重试该比较；旧拒绝记录见[续建来源](CONTINUATION.md)，不把旧机器的限制泛化为所有本地操作。
- 不将凭据、私人配置、照片或大型产物写入源码/普通日志。保留相关完整日志，交付摘要和路径；不将快照当作已集成提交，不把未核实写成完成。
