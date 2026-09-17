# AGENTS.md

This is the continuation repository for the OrcaSlicer AI project. It preserves the latest useful model-generation working tree while keeping the former workspaces intact.

## Current scope: fast internal development (2026-09-13)

- The current deliverable is a Windows internal test build and its actual Orca main-window journey. Public releases, commercial release promotion, website uploads, signing/publication qualification and deployment are outside the current workflow. Do not restore those steps from historical plans.
- Local full application builds, startup and functional validation do not require a Git commit, push, PR, remote CI, team notification or a clean worktree first. "Full build" means building the required application and runtime targets; it does not require deleting an existing incremental build.
- Default to one agent: locate affected code, edit, run the minimum sufficient checks, review the diff, then finish. Use the risk-based loop below and [quick development](Docs/coordination/quick-development.md); a routine edit does not automatically require a main-window trial, handoff or new source worktree.
- Internal test packages may use a verified base commit plus a complete file-hashed snapshot, including uncommitted changes and a detached validation HEAD. Record the source identity, build configuration, artifact hashes and actual verification limits. Do not label a snapshot as a clean integrated commit.
- Use `release/build_internal.ps1 -SourceManifest <handoff/manifest.json>` for internal snapshot packaging. Its `-ValidateOnly` checks prerequisites without building, copying the runtime or starting Orca. Package/configuration failures are technical conditions to resolve within existing authorization, not evidence of a platform denial.
- The team submission section below applies only when the user requests remote submission or integration. Internal packaging does not require pushing or fetching; an unavailable integration baseline is recorded as unknown.

## Start here

Use the current request to choose the necessary context:

- Read `CONTINUATION.md` when taking over the project or when current scope, provenance or completion status is unclear. Its dated corrections supersede older product descriptions.
- Use `Docs/AI_ENGINEERING.md` to locate an affected module and its verification commands.
- Consult the relevant sections of `Docs/plans/2026-09-07-ai-product-rebuild-execution-prompt.md` only when a product/UX decision is unresolved by current code and requirements, including its latest dated corrections.
- Read `docs/architecture/ai-integration-lock.json` when changing integration boundaries, contracts, runtime pins, shared touchpoints or architecture budgets. Historical role names in this lock are provenance, not instructions to recreate old tasks.

Do not reread the whole document stack for a small, understood edit. Follow nested `AGENTS.md` when working in its directory.

Root `task_plan.md`, `findings.md`, `progress.md`, dated reports, and `archive/*` branches are retained evidence. They are not the current task list and do not authorize old work.

## Small-task loop and skill selection (2026-09-15)

- Select verification by behavior, risk and impact, not file count. Cross-module behavior, shared interfaces, credentials, paid calls, uploads, deletion, persistence/format migration and security changes require checks of affected consumers, failure paths and authorization boundaries; they are not ordinary small edits.
- Logic changes start with targeted automated tests. GUI changes additionally check the affected real visible state or interaction; navigation, import, recovery and undo changes retain their relevant regressions. Full GUI journeys are for changes affecting that journey or an explicit full acceptance request. Documentation and development-entry checks do not launch Orca merely to satisfy a generic verification rule.
- Local development does not automatically trigger clean/full rebuilds, packaging, website publication, remote CI or independent acceptance. Build affected targets incrementally in the checkout's own configured directory. Reuse its complete development runtime, isolated test configuration and existing offline test fixtures; never let different worktrees write one build directory. Confirm source/configuration and tested binary or sidecar identity before accepting results.
- Keep full logs locally, without credentials or unrelated private payloads; return status, counts/timing and log paths, reading relevant excerpts on failure. Let scripts/tools wait for completion instead of frequent polling. For GUI work, request the affected controls/state or one useful screenshot; do not repeatedly return full window trees and unchanged screenshots. After two equivalent failures without new evidence, diagnose or change the permitted strategy; never route around a platform denial or report blocked work as passed.
- Reuse recorded checks only while their relevant source inputs, configuration, dependencies and test conditions are unchanged. Invalidate affected results after changes or new failure evidence. Keep one local result/log record when needed; routine edits do not update multiple plans, coordination documents or acceptance reports.
- Skills are selected for a concrete missing capability, not keywords or the mere presence of code. `code` is sufficient for routine implementation; use `test-runner` for test work, `debug-pro` for a failure needing structured diagnosis, and `brainstorming`/`writing-plans`/`planning-with-files-zh` only for unresolved choices or substantial dependencies/recovery needs. `architecture-designer` requires an actual architecture decision; mentioning a module or interface does not trigger an architecture audit. The repository `model-generation-evaluation` skill applies to requested artifact/batch quality evaluation, not every generation-related edit.
- Simplification follows the existing project conventions without a default persona or persistent skill mode. No skill automatically adds agents, approval checkpoints, commits, full suites or reports. Explicit user skill requests still apply within their requested scope.
- Shared skills live in `.agents/skills/`: `code`, `planning-with-files-zh`, and `model-generation-evaluation`. Prefer these project versions over same-purpose personal copies; do not load both. Git sync carries these instructions, not personal model settings, credentials, plugin state or build caches.
- Finish when the requested behavior is implemented, relevant checks pass, the diff is reviewed and no new failure evidence remains. Briefly note unrelated findings without extending the repair scope. A real blocker leaves a specific unverified item, not an invitation to retry indefinitely.

## Work model

- Work on one bounded change at a time with one agent by default. Delegate only when explicitly requested or a substantial independent subtask justifies it; routine location, edits, tests and diff review stay with the principal agent. Do not create new sidebar tasks as a development convention.
- ADR-007 is accepted. The new team branches are `codex/team/model-generation`, `codex/team/smart-slicing`, `codex/team/maintenance`, and `codex/team/integration`. The common source snapshot has been prepared; `codex/continue` remains a provenance reference. New work uses the new team branches; old branches must not be force-updated or deleted during migration.
- Developers synchronize from `codex/team/integration` with ordinary merge commits. Target feature PRs at this integration branch; retain shared history and require current-version CI and non-author review. The initial automation phase is notification and candidate checking with manual merging; automatic merge and release are not enabled by adopting this ADR.
- `archive/model-generation-head-20260908`, `archive/smart-slicing-20260908`, and `archive/orca-integration-20260908` remain provenance references, not merge instructions.
- A platform check blocked comparison of the latest smart-slicing source with this snapshot. Do not retry that comparison through another task, agent, tool, or entry point. Keep the archive ref until the platform review is actually resolved.
- Never reset, clean, delete, or modify the former workspaces as part of work here.

## Team submission and integration (only for requested remote submission)

Follow [the team SOP](Docs/coordination/team-integration-sop.md) for commands, notification templates, build records and rollout status. These repository instructions guide developers and agents; branch protection, CI and the service enforce the remote workflow.

The confirmed GitHub owners are `arsenaltj` for model generation, `tony20160206` for smart slicing, and `tangjiajie15191661723-web` for maintenance and integration coordination. Keep `.github/team-collaboration.json` and generated `.github/CODEOWNERS` aligned with these roles. Account assignment does not by itself grant repository access; effective ownership requires collaborator write access and the CODEOWNERS version on the protected PR base branch.

- Before submitting work for integration, announce the task, branch and shared files in the agreed team channel. Fetch `origin`, incorporate updates to your own remote branch if any, then merge `origin/codex/team/integration` into your own branch. Preserve unfinished work before merging; never resolve conflicts by blindly replacing another developer's changes.
- Report the sync outcome, including conflicts or failure, and record the fetched integration SHA. Resolve conflicts, inspect the combined behavior, compile affected code and run applicable checks from `Docs/AI_ENGINEERING.md`. Documentation-only edits need documentation checks. Record the exact tested source SHA and any remaining verification limits.
- Push your own branch and open/update a PR targeting `codex/team/integration`; do not push directly to integration. Incomplete work may be saved as a clearly marked draft but must not enter the ready queue. Keep one independently reviewable change per PR and list dependencies as `Depends-On: #12, #13` when present.
- Require current-version CI, resolved discussions and non-author review, including the relevant owner for shared changes. A source or integration HEAD change invalidates the previous candidate; rebuild/recheck against the new pair before merging. Chat announcements are coordination notices, not a global lock or evidence that checks passed.
- Only the designated maintainer merges approved PRs during the current manual phase. The future bot must serialize merges, obey branch protection and recheck both HEADs immediately before merging. Automatic merging requires its separate implementation and real PR acceptance; the current service cannot perform it.
- Record CI/team-package attempts and retain successful internal packages with source snapshot identities, platform/toolchain, verification results and artifact hashes as specified in the SOP. Keep snapshot and PR candidates distinct from builds of the actual merged integration SHA. Binary packages and private configuration do not belong in Git source history; pending remote archive automation does not block local validation.
- After merging, verify the resulting integration SHA and link its build record in the PR/team card. A failed integration baseline pauses ordinary merges; use a reviewed recovery/revert PR with the required checks, then resume. Notification retries must not repeat a merge, and archive retries must not silently replace an existing version.

## Product and architecture boundaries

- Model generation and smart slicing are logical modules inside one Orca desktop product. Their principal workflows belong in the Orca main window.
- Orca owns projects, models, profiles, slicing, G-code, and preview. `src/slic3r/AI/Contracts` owns cross-feature contracts; `src/slic3r/AI/SmartSlicing` owns smart-slicing decisions; `src/slic3r/GUI/AI` owns desktop feature hosts and Orca adapters; `tools/ai` owns the Python generation sidecar and provider adapters.
- Keep provider policy out of `libslic3r`. Provider changes must not rewrite the user workflow or Orca core.
- Model import must not silently slice or change presets. Smart-slicing proposals must be comparable, explicitly applied, and undoable.
- Preserve ordinary Orca behavior when AI is disabled, offline, or unavailable.

## Compatibility and delivery

- C++17, wxWidgets, and CMake; keep Windows, macOS, and Linux compatibility.
- Preserve `.3mf`, printer profile, material profile, and manual Orca behavior. Format/profile changes require migration handling.
- Internal EXE and portable ZIP packages contain no provider credentials. Private tester configuration remains outside shared artifacts, source control, and ordinary logs.
- Do not publish, deploy, push, call paid generation services, or contact testers unless the user explicitly requests that concrete action.

## Verification

### 模型配色优化回溯（2026-09-15 用户要求）

- 2026-09-16 用户更新本配色任务分工：“验收交给我来执行”“你修改后告诉我，我负责测试”。Codex 负责实现、构建、代码测试、候选版本核验和清单／对照交付；实际视觉和主窗口流程由用户测试，未反馈前记为待用户验收。此具体分工优先于下方一般的代理 GUI 验收要求，不因尚未自行演示而扣住已可测试的候选；仍不得虚报通过或降低最终门槛。

- 配色优化按 [持续计划与迭代记录](Docs/plans/model-coloring/README.md) 执行。每轮生产改动开始时登记问题、依据和修改假设；交付前补齐源版本／补丁快照、同模型同视角色卡对照、测试与实际窗口结果、遗留问题和回退基线。
- 失败、部分改善和被用户否定的方案也保留记录。代码测试通过、屏幕区域效果合格与实体打印配色准确分别评价；不得以“无新增变化”代替修复原有问题，或用新指标覆盖原有失败区域。
- 配色表达材质固有色，光照明暗由显示和真实环境产生。识别、材质归并、边界表示、预览法线和物理耗材校准分项登记；阈值调整写明依据，不将未验证推断记成根因。

### 日常验证与按需独立交接

- 日常验证按上方小任务规则执行。构建期间不要继续修改源码；记录 HEAD、相关修改文件哈希及实际测试产物。验证请求已授权必要的本地构建、运行环境准备、离线检查和相关主窗口操作，不重复索要；外部动作仍按兼容与交付条款处理。
- 仅在明确要求独立验收或需要复核的团队交付时读取并执行[交接流程](Docs/coordination/validation-handoff.md)：复用登记的固定任务，使用独立源码/构建/运行目录与测试配置，固定提交或完整补丁快照，使结果和产物对应同一版本。完整交接快照不套用于日常修改。
- 用户已授权开发与验证任务之间发送交接、缺陷和结果消息。必须实际发送并核对回执，原开发任务负责修复闭环；固定验证任务不派给自己，结果回执不当成新验证请求。具体授权、已用预算及平台拒绝的原始记录随交接传递，任务分工不改变它们。

- When architecture boundaries, shared contracts, runtime pins or integration behavior change, use `python scripts/verify_ai_integration.py --json` and report all findings rather than weakening checks. This is not a gate for every small edit. Instruction-only or prose-only edits need document/skill validation; they do not require a full application build or unrelated suites.
- Verify that a selected offline test actually mocks provider access; do not infer isolation from its filename or from available credentials. Paid benchmarks and real provider calls remain separately authorized operations.
- Python AI tests are under `tools/ai/test_*.py`. C++ test placement and commands are in `tests/AGENTS.md`.
- Match verification to the affected module. Compilation or unit tests alone do not prove the main-window journey, provider stability, color fidelity, real slicing behavior, or print quality.

### Windows 本地执行与审批（2026-09-11）

- AGENTS.md 说明项目授权和工作方式，不覆盖工具执行策略。用户通过客户端支持的方式修改审批设置后，遇到权限问题时核对当前会话的有效设置；配置文件已保存不等于运行中的任务已经加载。不把日常已允许的操作变成额外权限排查或确认流程。
- 命令应清楚呈现目标路径、修改范围、启动程序及网络用途，复用可审查的现有项目脚本并传入明确参数。不得通过编码、隐藏字符串、间接执行或改变工具入口掩盖实际动作；不以规避检测为目的调整命令写法。回环 HTTP 地址也应说明用途，不默认免于审核。
- 新的测试环境准备优先保持完整、版本相符的独立运行目录；启动前只读核对 EXE、DLL、基础资源、Python 和安装版 sidecar 启动文件。不要把资源不完整的开发 EXE 当作已修复测试包交给用户，也不要为了方便修改已有运行实例或共享开发目录的资源联接。此原则不授权替代执行已经被拒绝的运行准备或启动操作。
- 复杂 PowerShell 命令可先做静态语法与路径检查，用于定位技术错误；只解析不等于执行成功或策略已批准。拒绝诊断须区分实际命中证据、公开实现推断和仍未知的原因，不把语法正确或离线规则复现写成平台放行。
- 拒绝记录保留时间、工具、具体命令/动作和原始输出；区分平台拒绝、脚本前置条件失败与尚未执行。仅有 `blocked by policy` 不得自行归因为 Auto-review、账户限制或某个风险规则，也不能把组合命令拒绝扩大为所有编译被禁止。
- 已有明确拒绝只能按平台提供的正式审批或复核路径处理。用户修改审批模式、再次表达任务授权或编辑项目指令，不自动等于该具体动作已获平台批准；取得适用于该动作的正式批准且当前工具允许后，才按批准范围继续。未解除前不拆分、换脚本、换任务或换入口执行同一被拒操作；不依赖它的已授权工作继续完成，准确保留验收缺口。

### 真实 3D 生成与历史资产（2026-09-10 用户要求）

- 2026-09-15 用户修正：后续实际验收使用软件真实主窗口及真实服务，不再准备、启动或修复独立 mock 服务作为验收环境。已有离线自动测试中的 mock/fixture 保留，用于逻辑验证，不冒充真实流程验收。只验证查看、编辑、导入或恢复时复用软件内已有真实资产，不额外生成；涉及新生成时复用具体外发授权和剩余预算，缺失范围须补齐，不能把本条视为无限付费或解除历史拒绝的授权。
- 后续真实 3D 生成须从软件自身主窗口的完整用户旅程发起，经过输入、设计图确认、生成及结果查看；不要用独立脚本或直接提供商／sidecar API 提交新生成任务来替代软件旅程。隔离且确实 mock 提供商的离线测试不受此限制，但不能作为主窗口验收证据。
- 生成的输入、设计图、模型及版本记录应保留在软件实际使用的历史资产中。验证时检查历史列表可见、切页后可重新打开，以及重启后仍能恢复；仅在 `outputs` 或临时测试目录保留文件不算已进入历史资产。保留原文件和真实任务标识，不覆盖已有资产或伪造生成记录。
- 继续遵守当前已授权的调用预算，不能因改用主窗口而重置计数。已有模型的查看、导入和恢复不应隐式提交新的收费任务。

### 实际界面体验与明确主窗口验收时，必须实际启动程序

- 用户明确要求实际体验、主窗口验收，或修改影响真实界面行为时，必须实际启动包含待验收修改的程序，并操作受影响流程。“检查/验证”按请求对象解释：逻辑、文档、脚本入口检查不自动升级为 GUI 验收。不能用代码阅读、编译或模拟接口测试宣称实际界面验收完成；仅启动成功也不等于功能验证通过。
- 使用独立测试配置和模型副本，保留用户已有工程与运行实例。界面验收已获授权时，不以一般技能建议、预计耗时或假设性风险跳过；纯文案只检查受影响可见状态，不扩大为完整旅程。
- 核对实际运行的可执行文件、DLL、资源和 sidecar 对应当前待验收修改，记录版本或文件哈希。旧实例只能作为问题复现或对照依据，不能替代新修正版的实际验收。
- 按本次修改范围走查成功路径、必要的取消／失败恢复和相关普通 Orca 行为；涉及切页、编辑、版本恢复或导入时，实际检查状态保留、撤销／重做及准备页交接。发现问题应修正、重建并重新启动修正版复测，直至相关问题解决或遇到真实阻塞。
- 保存足以复核的界面截图、日志或结果文件，明确哪些流程实际通过、哪些仍未执行。未完成实际程序验证时，不得写“没有问题”“全流程通过”或把该验收项标记完成。
