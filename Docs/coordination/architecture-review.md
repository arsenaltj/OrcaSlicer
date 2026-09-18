# PR 架构影响可视化

目标：每个 PR 都能从产品全貌下钻到模块、业务状态和源码改动。code-review-graph 负责本地代码关系与影响分析，Archify 负责经过布局校验的 architecture / lifecycle 图；同一个离线入口串起各层。PR 评论保留直接可见的 Mermaid 摘要。图是 Review 导航，不是验收结论。

## 同页业务画布

`index.html` 首页使用产品能力架构：顶部服务与编排，中间模型生成和智能切片两条显式主线，下接 Orca 原生能力，底部公共支撑。原有细分业务入口保留在对应分区；点击分区标题展开整条主线，点击业务卡片进入对应阶段，再展开状态、业务条件和代码模块。文件依据在右侧显示，不跳转层级页面，可用路径、“缩回上层”或 Esc 返回。已有 Archify 图表在本页弹层打开，独立图表文件仍保留用于导出。

`product_overview` 集中维护首页能力、目标占位与业务关系。按用户确认的目标图，YOYOClaw、积分计费、自动上色/摆盘等完整目标能力在所属模块内使用紫色虚线和“未做”占位；可点击查看目标输入和输出，不参与 PR 改动计数。已有部分实现仍在当前源码主线展示，不据此宣布目标闭环完成。原生能力中尚未细化的入口明确只做模块级定位。

`relations` 维护服务调用、规划调度、模型导入、工作区读取、候选应用、原生工作链和底座支撑的方向与条件。当前关系用实线，规划关系用虚线，均可点击查看说明。它们属于业务逻辑映射，不冒充静态分析自动识别出的函数调用；PR 颜色与未做状态分别表达。

橙色表示模块文件直接修改，业务层称为“关联源码改动”；蓝色表示潜在影响，灰色只表示无已识别改动。细化后的业务入口使用 `impact_scope: evidence`，只按自身锚点文件计算；代码模块仍汇总完整模块范围，避免把整个模块的改动传播为每个业务入口都变更。右侧显示实际文件依据；删除文件也参与标记。维护 `review-map.json` 中的 `business_flows` 和已有阶段锚点即可，不新增另一套分析器。

工作区预览明确标注含未提交修改；只有提交比较模式才标注 PR 提交范围。状态源码文件变动不证明所有状态或业务条件均发生变化。

## 图表保留的阅读层级

- **L0 产品全貌**：使用者、Orca 桌面、外部 AI 服务、本地项目资产及打印文件。属于维护的产品上下文，关联当前版本的源码锚点，不把它称为 CRG 自动发现的业务架构。
- **L1 模块与职责**：主窗口、模型生成、智能切片、契约、适配器、原生核心等；卡片显示职责、直接修改及潜在关联，可继续进入业务状态或相关文件。
- **L2 业务状态**：模型生成与智能切片两条主路径。点击阶段筛选状态，点击状态查看含义和源码位置，再下钻文件；异常、取消、失效与等待状态保留在图中支线清单、状态表和恢复条件中。支线清单不虚构转移箭头，不能当作完整状态转移图。
- **L3 源码与本次改动**：模块/路径过滤、修改清单、状态及规则的源码定位与文件哈希，并可打开完整静态依赖图。

状态名称从目标源码的 Python Job 状态引用/赋值及 C++ WorkflowState 枚举提取，与基线比较新增和移除。业务含义、阶段分组和关键条件由 `review-map.json` 维护，**不是自动控制流证明**。新状态没有说明时显示待补充；锚点消失显示缺失；实现文件有改动显示需复核。源码中有实现不等于实际验收通过；本工具不连接正在运行的 Orca 或服务，不展示伪造的实时进度。

## 团队需要同步什么

首次将以下源文件一起提交到团队 PR，目标为 `codex/team/integration`，审核合并后其他同事正常拉取/合并该分支即可。不要用 `git add .` 混入未完成业务改动；按下面的范围选择文件和已有文件中的相关 diff。

| 文件 | 用途 |
| --- | --- |
| `.github/workflows/architecture-pages.yml`、`scripts/architecture_site.py`、`scripts/test_architecture_site.py` | 成功报告的独立在线发布、版本归档与检查 |
| `.github/workflows/architecture-impact.yml` | 每个 PR 自动分析、上传交互制品并更新同仓 PR 评论 |
| `scripts/architecture_review.py` | Git 差异、code-review-graph、Archify 与 README 的统一生成入口 |
| `scripts/architecture-review-requirements.txt` | 固定解析工具版本 |
| `scripts/architecture_explorer.html`、`scripts/architecture_review.html` | 业务画布与代码关系页面模板 |
| `scripts/test_architecture_review.py` | 版本、删除关系、安全边界及 README 同步回归 |
| `Docs/architecture/review-map.json` | 业务语义、源码锚点、未做目标及关系的唯一维护源 |
| `docs/architecture/ai-integration-lock.json` | 已有模块 ownership 依赖；保留仓库版本，无需复制另一份 |
| `README.md`、`AGENTS.md`、`.github/pull_request_template.md` | 团队入口、AI 执行规则和 PR 提示 |
| `Docs/coordination/architecture-review.md`、`Docs/AI_ENGINEERING.md` | 使用说明及已有工程导航 |
| `dev.ps1` | Windows 本地快捷入口；Review 分支不调用编译/运行依赖 |
| `.gitignore` | 保留已有 `/.tmp/` 排除规则 |

不上传 `.tmp/`、虚拟环境、缓存、完整日志、下载的 Archify ZIP、个人 `.codex` / `.agents` 配置、密钥及业务资产。HTML 和 JSON 报告通过本次 Actions 制品交付，不作为每次 PR 的大文件提交。

**必需 Skill：无。** CI 运行固定版本 CLI，不依赖任何人的全局 Skill、MCP、Codex 模型或账户。Codex 同事读取仓库 `AGENTS.md` 与本说明即可；其他 AI 工具可将这两个文件作为任务上下文。`archify` Skill 仅用于修改图表布局时的可选设计辅助，`code` / `test-runner` 是个人开发辅助，不必复制到仓库；`graphify`、`planning-with-files-zh`、新插件或编排平台不是此流程依赖。

仓库管理员首次检查 Actions 已启用、组织允许 workflow 中的官方 Actions、同仓 PR 评论任务可使用 `pull-requests: write`。现有分析任务只有读权限；不为此添加个人访问令牌、不改用 `pull_request_target`，也不关闭保护。若组织禁止评论，报告仍在分析任务摘要和制品中，评论任务会明确报错。Fork PR 不发机器人评论。启用是否成功以首个真实 PR 的 Actions 结果为准，不能用本地测试代替。

## README 如何随 PR 同步

README 顶部的标记区域由同一份映射生成，保留原项目 README 其他内容。Windows 工作区 `./dev.ps1 Review` 在分析前自动更新这一区域；`-Committed` 只分析，不改文件。只更新 README 时无需安装图工具：

```text
python scripts/architecture_review.py --readme update
python scripts/architecture_review.py --readme check
```

需要维护业务阶段、状态说明、源码锚点、模块归属或未做目标时，只改相关映射条目并运行更新命令，将实际产生的 README 差异随 PR 提交。普通实现修改未改变总览时，README 内容保持稳定；**每次 PR 的具体变化仍会重新生成**在 PR 评论、检查摘要和交互制品中，通过 README 的固定链接进入。CI 用 `--readme check` 拒绝过期总览并提示修复命令，不自动向开发分支写回提交。

PR 分支的 README 在该分支可见；合并后目标分支的 README 更新。GitHub 仓库首页显示默认分支，因此只合入 integration 而尚未合入默认分支时，默认首页不会提前显示这些改动。本流程不切换默认分支，不替代审核合并。

GitHub README 展示 Mermaid 静态主线、未做目标及在线更新记录入口。交互图由 GitHub Pages 承载，无需下载；Actions 制品仍保存 14 天。在线归档在独立发布分支保存，不能把制品保留期误认为网页有效期。

## 本地环境与命令

使用完整 Python 3.12 和 Node.js 22；Orca 内嵌 Python 可能缺少 tree-sitter 扩展需要的 `python3.dll`，不要为此改动产品运行环境。工具环境独立于构建和 sidecar。

```powershell
uv venv --python 3.12 .tmp/architecture-review/tool-env
uv pip install --python .tmp/architecture-review/tool-env/Scripts/python.exe --only-binary :all: -r scripts/architecture-review-requirements.txt
.tmp/architecture-review/tool-env/Scripts/python.exe scripts/architecture_review.py --setup-archify
./dev.ps1 Review -BaseRef HEAD
```

以上显示当前未提交修改（包括已暂存、未暂存、未跟踪及删除文件）。按集成基线看整条开发线：

```powershell
./dev.ps1 Review
./dev.ps1 Review -Committed
```

`-Committed` 只比较提交，不混入本地业务修改。需要指定版本时传 `-BaseRef <SHA/ref>` 和 `-HeadRef <SHA/ref>`；基线使用两端 merge-base，并同时记录请求的 base tip。命令不会 fetch、提交、推送、编译、启动 Orca 或调用模型服务。

跨平台或 CI 直接使用安装了上述依赖的 Python：

```text
python scripts/architecture_review.py --base <base-sha> --head <head-sha>
```

首次 `--setup-archify` 仅下载官方 2.16.0 发行 ZIP，核对固定 SHA-256 后解压到当前工作树 `.tmp`；不安装全局 Skill 或依赖当前电脑的用户目录。日常 `Review` 不下载、不检查更新、不调用模型。版本及发行包哈希固定在脚本中，升级需要重新校验。

输出位于本工作树 `.tmp/architecture-review/report/`：

- `index.html`：离线业务画布，同页缩放展开阶段/状态/模块，按本次差异高亮，并在侧栏查看源码依据。入口本身不依赖网络；需要查看 Archify 弹层时，应将整个制品解压。
- `l0-context.html`、`l1-modules.html`、`l2-generation.html`、`l2-slicing.html`：四张独立 Archify 图，支持明暗主题、缩放、聚焦及导出；入口可单独展开每张图。
- `details.html`：完整模块关系、改动文件和搜索入口。
- 同名图表 JSON：确定性生成的 Archify 输入；`archify-receipts.json` 汇总四张图的布局检查与输入/HTML 的 SHA-256。单张校验及错误日志留在本地。
- `summary.md`：PR Mermaid 摘要；`report.json`：完整分析数据、关系变化、版本及覆盖范围。

完整日志在本地 `latest.log` 及报告目录的 Archify 日志。不同工作树不共用可写索引。Archify 校验或交付失败时命令返回失败；上次成功的 HTML 不代表本次通过。

## 每次 PR

[工作流](../../.github/workflows/architecture-impact.yml) 在 PR 创建、更新、重开或转为待审核时执行，不按路径过滤，因此文档和配置 PR 也能看到改动模块。无需全量编译。

- 只读分析任务运行检查，准备固定版本的 Archify，对本次输入执行 showcase validate / deliver，并上传 `architecture-impact` 制品，保留 14 天。下载解压后打开 `index.html`，完整关系及文件清单见 `details.html`。
- 同仓库 PR 的独立发布任务更新一条机器人评论，内含 Mermaid 图和本次运行链接，不重复刷评论；写权限仅在该任务，且不检出/执行 PR 源码。
- Fork PR 保留检查摘要和 HTML 制品；不使用 `pull_request_target` 执行外来代码获取写权限。
- 发布前重新核对 PR 的 base/head；版本已变化则不发布旧评论。生成或发布失败会显示失败，不吞掉错误。不自动修改分支保护或审核要求。
- 首次启用需要将本次脚本、模板、映射及 workflow 一起提交到 PR；本地通过不等于 GitHub Actions 已运行。

## 范围、维护与局限

[review-map.json](../architecture/review-map.json) 从现有 [集成锁](../../docs/architecture/ai-integration-lock.json) 引用 ownership，集中维护展示分类、索引范围、职责及业务语义/源码锚点，不另建协调文档。新增文件若没有模块归属，会出现在“待归属”；修改模块内部逻辑也会标记模块，不要求依赖关系发生变化。业务证据只读取指定的少量文件；PR 模式读取指定提交的 Git blob，工作区模式包含本地修改，并记录哈希。

试点索引 AI 业务层、GUI/AI、生成/sidecar 客户端、Python AI 代码和相关 C++ 测试；第三方依赖、完整 Orca 核心、素材和历史文档不进入代码图。所有 Git 改动仍进入清单，未解析文件明确标为“模块标记”。不要将此试点称为全仓完整架构。

Archify 节点用文字标明直接修改及潜在关联数量，颜色不表示验证结果；这里是源码模块，不是独立部署服务，因此隐藏服务类型图例。总览最多展示五条选定主干依赖，只有 CRG 实际解析到的关系才绘制；其他关系的数量及全部新增/移除数量在卡片中明确披露。完整关系不丢弃，均保留在 `details.html`、PR Mermaid 与 JSON 中。详情页黄色表示直接修改、蓝色表示潜在关联。箭头从使用方指向依赖方，不能当成运行时调用顺序。

关系来自静态分析，C++ include 的相对路径或唯一后缀补充解析标记为推测，重复头文件保持未解析。wxWidgets 事件、动态分派、跨进程 HTTP 和运行状态语义无法仅凭此图完整确认。Archify 的 9 项 showcase 布局检查不证明代码正确；首次/模板变化需实际检查页面，CI 不宣称已完成人工视觉验收。

同时分析基线和目标，保留删除文件的旧消费者。影响最多反向两跳，范围外依赖不会自动补齐。改名按删除＋新增显示。

结果缓存按源码内容、映射配置、分析器版本和依赖版本校验，输入相同时复用；当前试点在源码输入变化时重建该版本的限定范围图，不宣称已实现全仓逐文件增量。缓存位于 `.tmp`，不会作为团队真实源码提交。源码更新期间检测到输入变化会拒绝发布。

不安装全局 Skill/MCP 配置、Git hook、守护进程或 embeddings，不把代码发给模型服务。Archify 使用固定布局和分析数据自动生成，每次 PR 不需要 AI 重新绘图。新增模块或更改总览布局时，更新并实际校验 `archify_spec`；不能静默隐藏新模块来通过检查。

回归入口：`python -m unittest discover -s scripts -p test_architecture_review.py -q`。覆盖暂存/未暂存/删除差异、旧消费者、依赖方向、路径/HTML 安全及真实 C++/Python 解析。

## 直接在线查看与版本记录

入口：<https://arsenaltj.github.io/OrcaSlicer/>；每个 PR 的最新成功报告在 `pr/<number>/`，每份报告的固定地址为 `pr/<number>/<head-sha>/<run-id>/`。页面显示版本，更新记录保留旧提交。README 提供固定表格入口，在线记录随成功发布自动追加，不产生自动回写源码分支的提交。

Pages 使用 `codex/architecture-pages` 的根目录，分支只保存已公开的报告产物与索引，不是业务开发分支。首次由有权限的维护者启用该 Pages 来源。不要替换已有网站或更改默认分支。源码与新增发布 workflow 仍通过团队 PR 审核。

`architecture-pages.yml` 使用 `workflow_run` 接收成功的 `PR architecture impact`，因此须先合入默认分支才会自动触发。首次试用可由当前操作者使用同一 `architecture_site.py` 发布已经成功且来源对应的报告，并明确记录这次人工初始化；不能把初始化写成自动流程已验收。

发布任务固定检出默认分支的 workflow SHA，仅下载匹配 run 的 artifact，不检出/执行 PR 源码。校验同仓 PR、当前 head/base、报告身份、固定文件白名单和大小限制后，写入独立网页分支；Fork、失败或过期报告不作为当前预览发布。PR HTML 作为静态文件保存，在不授予同源和顶层跳转权限的 iframe 内显示，不在带凭据的 CI 中运行。

发布使用独立任务的 `contents: write`、`pages: write`、`pull-requests: write`，无需 PAT 或额外云服务。写入网页分支后显式请求 Pages build，等待对应归档提交构建完成才发布在线评论；失败时保留下载入口并报错。并发发布串行执行；GitHub 原生 concurrency 不保证待执行事件队列完整，必要时重跑被取消报告的分析。已有版本不覆盖，不自动清理归档；报告数量增大时再制定保留策略。

在线图只描述已提交源码与静态影响，不暴露本机未提交代码、配置、密钥或模型资产，也不证明业务验收通过。GitHub Pages 可用性以实际访问结果为准。
