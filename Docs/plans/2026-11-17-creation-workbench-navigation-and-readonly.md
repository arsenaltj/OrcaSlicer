# AI创作与工作台往返：修改边界与验收用例

日期：2026-11-17

状态：**已按授权实施；44项按下述分层方式验收通过，逐项结果与范围见第9节。按用户追加授权随代码形成本地Git提交，不推送远端。**

归属：MAINLINE-BUGFIX-001／workbench-navigation-and-initial-view-20261117。基线：`codex/team/integration`／`7221efa76054dcc1fbc313da07462c9c6842ce64`，当前会话单writer。进度统一维护于[主计划](2026-09-25-product-convergence-and-lightweight-plan.md)和[短状态](../../docs/coordination/current-development.md)。本稿只细化页面往返、创作只读及相关异步清理；“模型首次显示朝向”仍是独立待处理问题，不随本稿纳入实施。

## 1. 已对齐的产品规则

1. 生成模型完成后，用户明确点击“进入工作台”，成功交接资产才进入工作台流程，并锁定对应AI创作内容。
2. 左侧“工作台”是页面导航，空工作台、AI生成、模型加载、检查／修复／改色、原生导入和切片过程中均可进入，不能因创作只读、生成资格或后台忙碌被拒绝。
3. 工作台流程内，点击左侧“AI创作”只读查看；反复切页不解锁。只有明确执行“返回AI创作”，通过既有写入保护后才恢复编辑。
4. 智能／高级模式遵循同一规则。只读不妨碍查看图片、旋转／缩放预览和页面往返；修改输入、发起生成及恢复另一份草稿等写操作继续锁定。
5. 切页不重新导入模型、不创建生成任务、不清空工程、不重新切片，也不改变已有撤销记录和有效G-code。

“任意时间”覆盖上述业务等待状态。原生文件选择、保存确认等对话框仍在用户结束当前模态交互后接受切页；后台任务进度或加载遮罩不得借用这种例外长期封锁侧栏。

此前“侧栏切到AI创作即可编辑独立草稿”的建议已撤回，不属于本方案。

## 2. 操作边界与预期界面

| 操作／状态 | 应发生的行为 | 允许改变的状态 |
| --- | --- | --- |
| AI生成完成，尚未点击“进入工作台” | 显示现有完成结果，沿原生成操作规则；仅完成生成不建立新的工作台流程锁 | 当前生成结果与展示状态 |
| 首次点击“进入工作台” | 校验当前成果身份、文件和工作台替换资格；成功交接后建立对应流程锁，显示工作台 | 资产交接、工作台状态、已提交的创作返回上下文 |
| 首次交接失败／取消 | 保留原工程、旧资产和原流程归属；报告真实失败，允许继续导航或重新尝试交接 | 仅本次交接请求及错误状态，不残留新锁 |
| 点击侧栏“工作台” | 显示已有工作台及其最近有效子视图；没有工程／模型则显示正常空态；处理中显示当前进度 | 页面／子视图可见性 |
| 工作台流程内点击侧栏“AI创作” | 显示交接来源的创作内容，只读状态保持 | 页面可见性，不改变流程锁 |
| AI只读页底部操作 | 主动作为“返回工作台”，按导航资格启用；次动作为可见的“返回AI创作”，用于恢复编辑；不显示可执行的“重新生成” | 按各自独立资格决定按钮可用性 |
| 点击“返回AI创作” | 使用统一恢复动作，通过保护后回到原智能／高级草稿并解锁；不自动生成或替换工作台模型 | 已提交的流程锁、原草稿的编辑资格 |
| 恢复编辑时工作台仍在写入 | 保持恢复入口可见，禁用并显示具体原因，例如“模型处理中，完成后可返回编辑”；终态／取消排空后自动刷新资格 | 写操作资格；侧栏导航始终可用 |
| 恢复编辑遇到未保存修改／未接受候选 | 沿用现有确认；取消保留只读锁，确认后恢复草稿编辑；仅恢复编辑不丢弃当前工程 | 用户确认后的编辑资格，不立即替换工程 |

只读提示统一为“当前为只读查看，点击‘返回AI创作’恢复编辑”。按钮因真实处理暂不可用时，旁边显示原因，不能出现提示引用不存在按钮的情况。完成态、加载态、错误态及高级页布局刷新均不得覆盖恢复入口。

工作台最近子视图沿用现有页面协调器的记录：原生准备页返回准备页，已有有效切片预览返回该预览；结果失效时退回相应准备／空页面，不能为了恢复预览隐式重新切片。

## 3. 状态所有者与依赖边界

| 状态 | 所有者与本次约束 |
| --- | --- |
| 生成任务、输入版本、停止／新建语义 | 仍由现有生成面板／SmartGenerationFlow及AdvancedCreationSession／Host维护；不改变上轮“每次新建、停止不恢复” |
| 创作成果交接后的流程归属 | 在现有ModelGenerationFeatureHost的交接／返回上下文中维护唯一的“已成功交接的创作上下文”；绑定来源模式、草稿／成果身份，不由页面和busy推断 |
| 智能输入／配置备份 | 复用已有快照，仅作为原草稿恢复资料；不能再以`m_saved_creation_input.has_value()`作为只读锁的业务事实 |
| 页面、工作台子视图、导航代次 | 仍由RedesignPageCoordinator及现有PageTransition维护，不另建路由器或第二套页面栈 |
| 只读控件及按钮 | Shell／AdvancedCreationWorkspace消费同一个已提交上下文，投影编辑资格；事件处理也检查资格，不能只禁用控件 |
| 原生工程、切片、G-code、撤销／重做 | 仍由Plater及现有原生工作流拥有；页面往返不接管、复制或重建工程 |
| 历史加载的取消与排空 | 仍由当前ModelHistoryOpen请求及对应面板加载状态管理，按请求身份结算，不用全局清零替代 |

普通手动导入或模型库导航不凭空建立创作锁；已存在创作流程锁时，也不能通过这些入口或历史回调自动清除它。通用资产返回信息与“是否已经明确恢复创作编辑”分别表达，但在同一现有Host内维护，避免Shell和高级页各存一份业务真值。

成功交接才提交上下文。开始准备时可以保存本次候选上下文，但失败必须恢复原上下文；页面切换既不提交，也不回滚业务交接。已接受交接在后台完成时可提交有效业务结果，但不能覆盖用户后来选择的页面。

## 4. 按模块限定修改范围

### 4.1 侧栏与子视图返回

主要落点：[RedesignShell.cpp](../../src/slic3r/GUI/Redesign/RedesignShell.cpp)、[RedesignShell.hpp](../../src/slic3r/GUI/Redesign/RedesignShell.hpp)、[RedesignModelWorkflow.cpp](../../src/slic3r/GUI/Redesign/RedesignModelWorkflow.cpp)和[RedesignModelRoute.hpp](../../src/slic3r/GUI/Redesign/RedesignModelRoute.hpp)。

- 为侧栏和只读页“返回工作台”提供纯导航路径；不调用新资产的`request_open_generated_workbench`／`request_open_workbench`入场校验，不重新触发`begin_workbench_presentation`的资产准备。
- “AI创作”“工作台”的切页不再共用生成busy、`modal_blocking`或资产可替换性门禁；这些条件继续限制真实生成、导入、替换和恢复编辑等写操作。
- 恢复现有工作台最近有效子视图；没有模型仍能进入工作台。切页不执行颜色准备、自动导入或切片。
- 排查`apply_workbench_state`中待处理的自动打开及原生导入完成导航：业务可完成，但用户已选其他页面后，不得再强制拉回工作台。

### 4.2 交接、锁定和显式恢复

主要落点：[ModelGenerationFeatureHost.cpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.cpp)、[ModelGenerationFeatureHost.hpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp)、[RedesignCreationModel.cpp](../../src/slic3r/GUI/Redesign/RedesignCreationModel.cpp)和前述Shell／Workflow。

- 首次“进入工作台”保留文件、成果身份、当前请求及工程保护；只删除普通导航对这些校验的依赖，不放宽交接资格。
- 已交接后的“返回工作台”先按已提交上下文判断导航行为，不能先因创作面板`busy`／`can_open_workbench=false`退出。
- `m_image_page_view_only`成为已提交流程状态的UI投影；草稿快照不再隐式建立锁。智能与WorkbenchAssets两个面板的快照各管各自职责，不混用完成资格和导航资格。
- 完成双按钮布局保留恢复入口：锁定态显示“返回工作台”／“返回AI创作”，非锁定态沿现有生成操作布局。`refresh_inline_generation`不能再无条件隐藏恢复编辑按钮。
- 所有“返回AI创作”入口共用一个动作与资格；真实后台处理未排空时显示原因，排空后重算；未保存修改／候选沿现有确认，取消不解锁。

### 4.3 高级模式同步消费

主要落点：[AdvancedCreationWorkspace.cpp](../../src/slic3r/GUI/Redesign/AdvancedCreationWorkspace.cpp)、[AdvancedCreationWorkspace.hpp](../../src/slic3r/GUI/Redesign/AdvancedCreationWorkspace.hpp)，必要时接入既有[AdvancedCreationFeatureHost.hpp](../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationFeatureHost.hpp)。

- 将共同流程只读条件纳入高级编辑资格，保留其独占命令、停止收尾、回执及模型任务锁。
- 覆盖上传／替换四图、描述、风格、模型选项、新建／切换创作、历史恢复、2D／3D生成等改变当前草稿的入口；通过按钮、快捷键或迟到回调都不能绕过锁。
- 模式切换不能成为绕过只读的入口；锁定期间保持交接来源模式。普通侧栏页面切换不能被高级页`navigation_allowed`中的工作台业务busy条件拦截。
- 高级完成页及其他状态同样显示明确恢复入口；显式恢复绑定原组／草稿，不能换组、重画四图或重提旧模型任务。

### 4.4 呈现过渡、遮罩和后台完成

主要落点：[RedesignWorkbenchPresentation.cpp](../../src/slic3r/GUI/Redesign/RedesignWorkbenchPresentation.cpp)、[RedesignPageTransition.hpp](../../src/slic3r/GUI/Redesign/RedesignPageTransition.hpp)、[RedesignPageCoordinator.hpp](../../src/slic3r/GUI/Redesign/RedesignPageCoordinator.hpp)；必要时调整[PostGenerationWorkbenchPresentation.cpp](../../src/slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchPresentation.cpp)。

- 现有`RetainedPageInputFilter`以整个Shell为根，会吞掉侧栏输入。把后台加载期间的输入拦截限制到冲突内容区，遮罩／反馈卡也不得覆盖导航区。
- 页面独占过渡仅覆盖实际GUI挂载／布局提交；CPU准备、工作排空、等待有效首帧不能一直持有全局切页锁。保留现有业务请求身份和挂载租约，不建立竞争路由。
- 每次显式导航更新当前路由代次。后台完成可以更新所属资产状态，但旧完成、超时、失败回滚及自动导航只能影响仍匹配的页面请求，不能跳回或回滚到旧页面。
- GUI不可分割的提交在当前事件内完成，期间记录最后一次导航意图并于下一次GUI调度应用；不等待整个后台任务结束，也不使用同步等待或循环`Yield`。
- 保留GL上下文、尺寸／DPI、首帧身份及隐藏窗口保护。隐藏页面无需立即渲染才能让其他页面工作；重新显示时取得匹配首帧。不得因页面被用户切走而误判首帧超时，也不得播放别的资产旧帧。

### 4.5 历史加载取消后的状态结算

主要落点：[ModelGenerationArtifactFlow.cpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationArtifactFlow.cpp)、[ModelGenerationHistoryOpen.cpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationHistoryOpen.cpp)、[ModelHistoryOpen.hpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelHistoryOpen.hpp)及必要的[ModelGenerationPanel.cpp](../../src/slic3r/GUI/ModelGenerationPanel.cpp)投影。

现有运行中加载取消路径可在`worker_finished()`返回后提前退出，跳过`m_preview_loading`／`m_busy`清理；`drained`恢复又可能同步启动新加载。因此不能仅在函数末尾补一次无条件清零。

1. 已排队取消：不开始解析，按现有请求结束协议释放本次所有权。
2. 已运行取消：保留真实工作占用，等当前worker排空；取消按钮点击后不能立即把工作台视为可替换。
3. 排空后：先核实本次请求仍拥有对应加载状态，再结算该请求的loading／busy，随后执行可能启动恢复的`drained`回调。
4. 回调若启动恢复／新加载，其busy必须保持；旧请求返回时不能再清新请求的状态。
5. 迟到、重复、已替代请求的回调不能清新任务busy、解锁创作或改变当前页面。成功、失败、取消、超时和关闭均收敛到同一请求所有权规则。

### 4.6 按实际差异同步维护

- 若增加UI状态投影，检查[ModelGenerationHost.hpp](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationHost.hpp)的快照比较／通知消费者，避免只读或恢复资格变化未刷新。
- 保留[PostGenerationWorkbenchHost.cpp](../../src/slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchHost.cpp)中真实资产打开／导入保护；必要时补只读呈现能力，不把`request_open_workbench`直接改成永远成功。
- 在现有测试文件中增加行为回归，复用实际生产规则／请求生命周期，避免只检查源码字符串或把实现判断原样抄进测试。
- 实施时同步平台说明和受影响的状态所有者／模块锚点；只有实际契约、能力或所有者变化才更新[review-map.json](../architecture/review-map.json)并生成README区域。文档同步声明按[现有规范](../coordination/documentation-sync.md)填写，不新建架构报告。

## 5. 影响范围与风险

整体为**中等风险的GUI工作流修正**；原生工程保留、异步呈现回滚和输入过滤为重点。实现应按“导航与交接分开→共同锁定投影→请求清理→真实主窗口验证”分步落地，每步保留现有保护。

| 风险点 | 可能回归 | 限制与验收 |
| --- | --- | --- |
| 放开导航时仍有后台准备／首帧等待 | 旧回调抢页、挂载错位、闪旧模型 | 现有路由代次与挂载身份共同校验；NAV-08～09、ASYNC-03～07、WIN-08 |
| 创作快照与WorkbenchAssets快照混用 | 假锁、误解锁、按钮仍不可用 | Host提交上下文为唯一归属，Shell／高级页仅投影；FLOW-01～10 |
| busy清理顺序或所有权错误 | 永久busy，或新加载尚未完成却允许写入 | 排空前保持占用、回调前只清当前请求；ASYNC-01～05、09～10 |
| 原生导入／切片在隐藏页面完成 | 重复导入、G-code失效、丢撤销或未保存工程 | 保留Plater事务，不重走交接；NAV-03～07、NATIVE-01～04、WIN-03 |
| 全局输入拦截收窄 | 正在修改的内容被误操作 | 只开放导航和查看，内容写入门禁保持；NAV-06～08、FLOW-05 |
| 恢复按钮布局及高级消费者遗漏 | 有提示无出口，或另一入口绕过锁 | 完成／加载／错误／不同尺寸均检查；FLOW-05～10、WIN-01～02、08 |

本稿不修改供应商接口、计费／重试策略、模型生成能力、历史文件格式、粒子算法、相机／网格朝向、切片算法、预设或libslic3r。不新增跨重启的创作锁持久化；重启不凭`model_path`推断新锁，也不恢复停止的生成任务。已有工程保护与上轮每次新建／停止不恢复、质量非阻断行为必须保持。

## 6. 验收用例

以下保留审批时的验收定义，实际结果见第9节。NAV／FLOW／ASYNC／NATIVE共36项以定向逻辑测试和实际消费者测试取证；WIN共8项必须在对应新版本的Windows真实主窗口取证。参数化场景不另计总数，不把旧52项或已有测试的通过状态移植到本稿。

通用参数：凡涉及创作来源的用例同时覆盖智能／高级；涉及后台状态的用例覆盖该入口实际支持的排队、运行、成功、失败、停止／取消和迟到结果。不同状态可用生产状态机的定向fixture验证，但不能用mock服务代替WIN的真实流程。

### 6.1 页面导航（10项）

| 编号 | 前置／操作 | 通过标准 |
| --- | --- | --- |
| NAV-01 | 空应用，无当前生成成果；点击侧栏“工作台” | 显示正常空工作台，无生成资格／准备失败弹窗，无新增任务或导入 |
| NAV-02 | 已有工作台模型；工作台→AI创作→侧栏工作台 | 回到同一工作场景；不调用资产打开／重新解析，不改变流程锁 |
| NAV-03 | 原生准备页有未保存位置／配置修改；切AI再返回 | 仍在准备页，同一工程及未保存状态、模型变换不变 |
| NAV-04 | 已切片且G-code有效；切AI只读再返回 | 仍是原有效预览；同一切片结果，无重切片／重导入 |
| NAV-05 | AI处于2D准备、3D等待、下载或解析；切工作台及返回 | 工作台可显示已有场景／空态；原任务继续，同job／请求，无重复提交 |
| NAV-06 | 工作台检查、修复或改色进行中；往返两个页面 | 导航可用，进度和结果归属不变；模型替换／写入等冲突动作仍禁用 |
| NAV-07 | 原生导入或切片进行中；切AI再返回，期间完成或失败 | 页面可达；当前事务只结算一次，不取消／重复导入或切片，不被完成回调抢页 |
| NAV-08 | 历史加载／交接准备中，内容有遮罩／输入过滤；点击两侧栏入口 | 侧栏收到输入并切页；内容冲突写操作仍被挡住；无全Shell长期拦截 |
| NAV-09 | 等待期间快速连续选择AI／工作台；随后交付旧完成／回滚回调 | 最后一次用户选择生效；旧回调不跳页、不恢复旧挂载或页面 |
| NAV-10 | 最近子视图为预览，但结果已失效或工程已空；返回工作台 | 回到对应准备／空态，无过期G-code显示、隐式切片或资产替换 |

### 6.2 流程归属、只读和恢复（10项）

| 编号 | 前置／操作 | 通过标准 |
| --- | --- | --- |
| FLOW-01 | 生成完成但未交接；仅切页，或保留尚未提交的输入备份 | 不因完成、切页、`model_path`或备份存在建立新锁；既有真实任务锁继续有效 |
| FLOW-02 | 智能成果有效；点击“进入工作台”成功，再切AI并多次往返 | 已提交来源与成果身份匹配；始终只读，侧栏往返不解锁 |
| FLOW-03 | 高级确认套图的模型就绪；交接成功后返回AI | 保持原组／四图／版本／选项并只读；不能借切模式或切组绕过 |
| FLOW-04 | 首次交接文件／身份校验失败、准备失败或取消；已有旧流程时也执行 | 无新锁残留；旧工程／旧上下文完整；可导航，错误指向本次交接 |
| FLOW-05 | 已锁定；通过上传、描述／风格／供应商、模式切换、新建／历史恢复、生成及快捷键修改 | 所有改变当前草稿／任务的路径均拒绝，输入／版本不变，不新增任务；库／历史回调不自动解锁 |
| FLOW-06 | 已锁定；查看原图／设计图、旋转缩放预览、查看已有详情并往返页面 | 查看正常，无草稿／工作台资产替换；不为浏览另一历史条目隐式恢复草稿 |
| FLOW-07 | 在智能／高级完成、加载、错误及窗口较小状态刷新页脚 | “返回AI创作”持续可见；只读提示与实际按钮一致；“返回工作台”按导航资格可用 |
| FLOW-08 | 工作台无冲突写任务；从工作台及AI只读页分别显式“返回AI创作” | 恢复同一来源模式／原草稿并解锁；不提交生成、不替换工程、不复活旧任务 |
| FLOW-09 | 有未保存修改／未接受候选；点击恢复，先取消再确认 | 取消保持锁和原现场；确认后可编辑；仅恢复编辑不丢模型／候选／工程，后续替换仍受原保护 |
| FLOW-10 | 检查／修复／改色、导入／切片或当前加载排空未结束；尝试恢复编辑 | 入口可见并说明禁用原因，导航始终可用；真实终态／排空后恢复资格，不产生永久禁用；用户再次显式点击才解锁 |

### 6.3 请求取消与异步呈现（10项）

| 编号 | 前置／操作 | 通过标准 |
| --- | --- | --- |
| ASYNC-01 | 历史加载尚在队列时取消 | 不执行解析；本次请求所有权及对应loading／busy结算一次，可继续导航／后续加载 |
| ASYNC-02 | 解析已开始时取消，随后worker实际排空 | 排空前保持写入保护；排空后无残留`preview_loading`／busy；旧工程可用 |
| ASYNC-03 | 可见页面触发真实呈现超时；另测主动切走／最小化再返回 | 真超时回滚并结算；不能因隐藏而误判等待首帧失败；新页面可用，返回取得匹配首帧 |
| ASYNC-04 | 请求A取消后创建B，再到达A的完成／失败／超时 | A不清B的busy、不解锁或改变路由；B仍按自身生命周期完成 |
| ASYNC-05 | 旧请求排空的`drained`回调同步启动旧模型恢复／新加载 | 旧占用先结算；新请求busy保持到其完成；没有恢复后再无条件清零 |
| ASYNC-06 | 安装失败／GL首帧失败，存在可恢复的旧模型 | 旧有效资产／上下文保留，实际原因可追踪；当前用户页面不被旧回滚覆盖，不显示错资产帧 |
| ASYNC-07 | 加载／交接／原生导入在隐藏页面完成；用户已选其他页 | 正确提交仍有效的业务结果及交接锁；不强制导航，返回后展示同一结果 |
| ASYNC-08 | 源文件缺失／不可读／坏格式，或原生导入失败 | 本次请求结束，无新锁或永久busy；旧工程仍可达，错误不变成侧栏导航拒绝 |
| ASYNC-09 | 重复取消／完成／回滚通知，快速连续点击导航与交接 | 每个业务请求最多提交／恢复一次；导航不创建请求，无重复导入或任务 |
| ASYNC-10 | 加载排队／运行、呈现等待或排空时正常关闭验收实例 | 无晚到访问已销毁UI、悬挂或额外提交；按现有关闭协议排空并保留原资产 |

### 6.4 原生工程及既有功能兼容（6项）

| 编号 | 前置／操作 | 通过标准 |
| --- | --- | --- |
| NATIVE-01 | 保存有效切片结果身份／G-code摘要；多次页面往返 | 结果身份及文件摘要一致，无新增切片事件；仍可继续原预览／导出 |
| NATIVE-02 | 建立可撤销的工程修改；切页、显式恢复编辑，再撤销／重做 | 原撤销链、脏状态与工程内容正确，不因切页重建或清空 |
| NATIVE-03 | 有网格、颜色／材质、变换及用户预览视角；往返页面 | 模型／材质／变换与用户视角保留；不借导航重设首显相机 |
| NATIVE-04 | 原工作台有资产A，AI有成果B；先侧栏返回，再显式交接B | 侧栏仍为A；显式B交接才走完整文件／身份／工程保护，失败保留A |
| NATIVE-05 | 服务不可用，已有本地工程／成果及有效创作上下文 | 可导航、只读查看并在无冲突写任务时显式恢复编辑；不要求重连或提交生成 |
| NATIVE-06 | 旧历史、手动导入、已有3MF工程及应用重启 | 无新持久化格式；普通入口不造新锁，已有锁不被普通导航／资产回调清除；重启不复活生成，不凭文件路径推断锁 |

### 6.5 Windows真实主窗口验收（8项）

使用独立配置、原模型／工程副本及完整匹配的EXE／DLL／资源／sidecar，保留用户运行实例和原始资产。各用例须记录实际操作与证据，不能用离线截图或逻辑fixture替代。

| 编号 | 实际操作 | 通过标准与证据 |
| --- | --- | --- |
| WIN-01 | 智能模式复用已有完成成果：进入工作台→侧栏AI→侧栏工作台→显式返回AI | 正确交接、持续只读、可往返、显式后才编辑；截图／日志及同一模型身份，无新增生成 |
| WIN-02 | 高级模式复用既有确认套图／完成模型，执行同样旅程 | 原组／四图／选项不变；恢复按钮存在、恢复正确，无换组／重画四图／新任务 |
| WIN-03 | 复现用户完整旅程：原生编辑／切片成功→AI只读→侧栏返回工作台→继续原预览及工程操作 | 无“准备未完成”阻断；原预览、G-code、未保存状态和撤销可用，记录切片结果身份与往返前后摘要 |
| WIN-04 | 用已有较大历史模型副本，实际加载过程中取消，再连续切页／再次加载 | 日志证实取消发生于运行阶段及随后排空；无永久busy或无法返回；未命中该阶段必须记为未覆盖 |
| WIN-05 | 对已有模型执行真实检查／修复／改色，在实际运行期间切出和返回 | 不抢页、不丢进度／候选，写入保护有效，真实终态后恢复编辑资格；每种实际覆盖操作分别留证 |
| WIN-06 | 在主窗口真实发起或复用仍有效的YOYO生成，等待时切工作台并返回；成功／失败／停止后检查页面与任务 | 同一当前任务，无导航造成的新提交；模型就绪后须显式交接才锁定。记录真实job／task与实际终态，受理不当作生成成功 |
| WIN-07 | 独立实例断开真实本地服务，用已有缓存成果／工程往返并显式恢复编辑 | 页面可达、查看及可恢复的编辑入口正常，无自动生成／恢复旧任务；记录真实断服务与恢复 |
| WIN-08 | 同实例至少20轮往返，包含一次后台等待／迟到完成、调整窗口、最小化恢复及正常关闭 | 最后路由正确、侧栏始终可用、恢复出口可见，无旧帧／错误挂载或关闭悬挂；记录实际分辨率／DPI，不推定所有DPI已通过 |

WIN-06优先复用仍有效的任务；确需新生成时使用此前已授权的YOYO预算，并记录新增调用。Tripo累计**6/6已用完，本方案不新增Tripo调用**；高级生成中状态可通过生产状态机定向测试覆盖，不冒称新增Tripo实窗生成通过。本轮编写文档不调用任何生成服务。

## 7. 实施后的验证入口与放行条件

以下保留审批时的验证入口，实际执行收据见第9节。复用本checkout的`.tmp/dev/{build,run,data}`，同一构建目录顺序单写，构建期间冻结源码；不冷构建或复制其他工作树缓存。

```powershell
./dev.ps1 Check
./dev.ps1 CppTest -TestSuite ui_lifecycle_tests -TestLabel UiLifecycle -Jobs 2
./dev.ps1 CppTest -TestSuite slic3rutils_tests -TestLabel 'UiRedesign|ModelHistoryOpen|ModelPreviewLoad|SingleWorkbench|UnifiedWorkbench|PostGenerationWorkbench|InlineModelGeneration|AdvancedCreation' -Jobs 2
./dev.ps1 Build
./dev.ps1 -NoLaunch
```

现有标签及suite已在源码中核实；`CppTest`按脚本现有`--no-tests=error`执行。优先扩展现有`test_ui_redesign.cpp`、`test_unified_workbench.cpp`、`test_model_history_open.cpp`、`test_inline_model_generation.cpp`、`test_post_generation_workbench.cpp`、`test_advanced_creation.cpp`和`test_ui_lifecycle.cpp`。若实际只涉及部分模块，可进一步收窄定向选择；若修改切片／导入契约，则补受影响的原生消费者测试，不能只凭UI单测放行。

状态所有者／集成接缝变更须执行`scripts/verify_ai_integration.py --json`并保留Git检查；实际改变架构映射时按现有`scripts/architecture_review.py --readme update`同步并检查，文档同步用`scripts/check_documentation_sync.py --base HEAD --worktree`定位候选。候选检查不等于PR声明或远端检查通过。

验收记录须包含：源码HEAD及未提交diff身份、匹配运行产物身份、每项用例实际参数和通过／失败／未覆盖、日志／截图、真实任务ID，以及原工程／资产／G-code前后身份。编译、定向自动测试、主窗口操作分别报告；全部必需场景通过并修正失败后，才声明本方案验收完成。既有粒子视觉／性能、朝向及旧暂停任务继续按原记录保留，不与本44项混算。

## 8. 现场依据与本轮交付范围

现场日志：`C:/Users/mooreb/Downloads/HonorForge_Logs_20261116_201555/logs/debug_Mon_Nov_16_20_04_25_3436.log.0`。20:13:04切片成功，20:13:16仍复用有效G-code；20:13:21出现呈现回滚，20:13:46～20:15:34五次工作台入场均记录`busy=true`，相关页面／布局组件已初始化。代码中的加载取消提前返回缺陷与此时序吻合。

日志直接证明忙碌门禁拒绝，未逐次记录字段赋值、取消触发及worker排空；导出名义build `459c527`未与当前源码／二进制精确对应。因此根因修复除定向回归外，仍须通过WIN-03／04真实主窗口验证，不把源码推导写成已复现。

完整既有分析证据：[navigation-log-analysis.json](../../.tmp/dev/evidence/workbench-navigation-and-initial-view-20261117/navigation-log-analysis.json)。审批稿阶段仅新增本稿并同步进度，未改产品或执行产品验收；后续实施由用户明确授权，结果如下。

## 9. 实施与验收结果（2026-11-17）

本节为第6节44项的实际结果：NAV 10／10、FLOW 10／10、ASYNC 10／10、NATIVE 6／6、WIN 8／8通过。前36项由生产状态机／真实加载器及消费者定向测试验证，并辅以主窗口；后8项均有真实主窗口操作证据。GL故障、迟到回调、排队关闭等可控异常使用生产门禁与生命周期fixture，不声称注入了真实显卡驱动故障或每种异常都做过实窗。

### 9.1 实际修改与修正

- 侧栏导航复用已有工作场景，保留最近原生准备／预览视图；导航脱离展示等待时只释放页面租约和内容遮罩，业务请求继续排空。原生切片自动切到预览后，离页时同步实际Plater子视图。
- 成功的显式成果交接才由Host提交唯一`CreationWorkbenchContext`；智能／高级共同只读，模式／历史／生成命令和迟到确认均保留写入守卫。只读页显示“返回工作台”和“返回AI创作”，显式返回原草稿才解锁。
- 历史加载按当前请求身份先结算loading／busy，再执行可能同步启动恢复的`drained`；旧完成不清新请求占用。后台检查不因隐藏而取消，完成／导入／回滚不覆盖用户后来选择的页面。
- 首轮实窗发现只读按钮截字、自动预览返回丢失和ONNX静态析构退出崩溃，已分别修正按钮等宽153DIP、同步实际子视图、正常排空后显式释放前景运行时。第三版初始化真实前景推理后正常退出，exit code 0。大模型解析与隐藏／最小化不消耗首帧15秒预算。

### 9.2 证据索引、版本与自动检查

以下路径均相对于仓库。证据根`E = .tmp/dev/evidence/creation-workbench-navigation-20261117`，截图／可访问树`G = E/gui`；`G/events.jsonl`记录实际时间。三个实窗版本保留原始失败；涉及后续修正的行为以第三版重新验收，不把首轮截图当作修复后证据。

| 检查／版本 | 实际结果与证据 |
| --- | --- |
| 当前checkout检查 | `./dev.ps1 Check`：READY，复用原构建目录，无需重新配置 |
| 相关C++回归 | `20261117-184305-001/result.json`：386／386通过，含导航／交接／高级／历史／粒子前景；不是44项的替代计数 |
| 生命周期suite | `20261117-173626-085/result.json`：48／48通过；最终补测也包含48项UiLifecycle标签，交叉不累计 |
| 缺失／坏格式／Windows不可读补测 | `20261117-202025-754/result.json`：74／74通过；不可读使用真实独占文件句柄，真实加载器报错并保留旧资产。前轮`20261117-201403-131`为73通过／1个测试断言失败，保留；空PreparedModel的`installable()`允许旧同步消费者，故改验实际错误／空渲染数据／无凭据／未安装，未修改或放宽产品门禁 |
| 增量构建／完整安装 | `20261117-185113-215/result.json`、`20261117-185327-043/result.json`通过；NoLaunch安装后才启动独立QA |
| 第三版受测身份 | `E/runtime-verified.json`：19656份安装文件匹配；native identity `cc4ed934c57baa17317da21be7f8a708bcef830181ad094747c1bcea0e8cf322`；EXE `650068ff02444de85260d68e08ed3b77390348ef1dac2c2dbe3e2957087d81a3`；DLL `0335f6df75779f401b2a3b7598b1236c604cd75febb93147d0666f1202487b65` |
| 源码／测试身份 | 受测安装收据source identity `aabc904185bdd056c11dd06c5efcfa7b9da2bf45f0ad583e50d552b18cd9d927`；其后仅补测试和文档，产品源码／依赖不变。最终分开记录安装身份与当前测试／文档快照，见`E/validation.json` |
| 集成／架构／文档 | 集成锁、Git收据、review-map及README检查、文档同步候选和diff检查，最终收据见`E/validation.json`；不以候选范围提示冒充PR文档声明 |
| 原件保留 | `E/original-files-final-check.json`：2227份来源文件摘要一致，differences为空；最终工程另存`E/qa-native-final.3mf`，未覆盖原件／基线 |

测试简称：UW=`tests/slic3rutils/test_unified_workbench.cpp`；HR=`test_model_history_open.cpp`；UI=`test_ui_redesign.cpp`；LC=`test_ui_lifecycle.cpp`；PG=`test_post_generation_workbench.cpp`；AC=`test_advanced_creation.cpp`，后五份也在同目录。下表列出实际行为／证据主题，可按测试名称定位；测试参数化只用于覆盖，不追加44项总数。

### 9.3 逐项结果

| 编号 | 结果 | 实际覆盖及主要证据 |
| --- | --- | --- |
| NAV-01 | 通过 | 空工作台正常可达；`G/065-empty-workbench.png`及第三版空态，无生成／导入门禁弹窗 |
| NAV-02 | 通过 | UW“Page round trips…”三子视图20轮保锁；WIN-01／02／08真实往返不重新解析 |
| NAV-03 | 通过 | 第三版Y位置169.28→185.35、工程脏状态，round18往返保持；`G/175-native-position-dirty.png`及v3-round18树 |
| NAV-04 | 通过 | 第三版实际切片成功自动预览，round19／20回原预览；G-code前后同摘要，见WIN-03 |
| NAV-05 | 通过 | UI当前任务跨生成／加载／失败／停止状态仅刷新、不导航；WIN-06真实2D／3D等待往返保持同job及task，无重复提交 |
| NAV-06 | 通过 | PG所有pending操作保持替换保护；WIN-05实测检查／修复／改色运行时往返，结果仍归当前资产 |
| NAV-07 | 通过 | PG NativeSliceSession旧revision／成功／失败终态；UI切片完成只呈现一次；实窗原生导入和切片隐藏后继续完成，`G/051-NAV07-import-hidden.png`、WIN-03 |
| NAV-08 | 通过 | 内容过滤根收窄到content；实窗历史／交接／处理遮罩期间侧栏往返可用，`G/076-handoff-background-user-ai.png`、`G/115-v3-user-cancel-running.png`；冲突写入仍禁用 |
| NAV-09 | 通过 | UI旧route／首帧拒绝；LC旧mount／排队替换；第三版20轮包含后台完成，最后所选页不被抢回 |
| NAV-10 | 通过 | UI“Reentering the workbench…”覆盖失效预览退回Slicing、有效结果保留Preview及空态；不触发新切片 |
| FLOW-01 | 通过 | UW stage未commit不锁，智能／高级accepted真假参数；`G/066-unlocked-before-handoff.png`及第三版生成就绪未交接仍编辑 |
| FLOW-02 | 通过 | UW智能commit绑定job／generation；第三版企鹅显式交接后只读、20轮保锁，`G/195-rehandoff-complete.png`、`G/196-relocked-ai.png` |
| FLOW-03 | 通过 | AC共享锁命令测试；第二版原机器人第3组四图、172字描述和紫条纹选项往返不变；`G/071-advanced-restored.png`～`078-locked-history-change-rejected.png` |
| FLOW-04 | 通过 | UW两来源首次失败不锁、替换失败保旧anchor；HR文件／job／generation校验与取消；实窗交接取消后旧工程保留 |
| FLOW-05 | 通过 | AC真实Host拒绝new／open／rename／generate且原draft／selection不变；共同Host及UI事件守卫保护输入／模式／历史与迟到确认；实窗`G/077-locked-mode-switch-rejected.png`、`078-locked-history-change-rejected.png`、`138-v3-library-preserve-lock.png` |
| FLOW-06 | 通过 | 智能／高级原图／四图和3D查看、用户旋转缩放及侧栏往返；没有恢复其他草稿或替换工作台，WIN-01／02／03 |
| FLOW-07 | 通过 | 恢复入口由共享锁独立投影，不被完成／加载／错误动作覆盖；第三版153DIP按钮完整，真实1251×781小窗口`G/200-small-readonly-exit-visible.png` |
| FLOW-08 | 通过 | UW显式restore清锁、旧完成不重锁；智能／高级从工作台及只读页回原稿，无新任务；`G/082-offline-explicit-advanced-editing.png`、`G/202-small-window-restored-editing.png` |
| FLOW-09 | 通过 | PG dirty／candidate拒绝替换；第三版青绿企鹅真实未保存改色，否保锁、是解锁且模型仍在；`G/187-actual-color-edit.png`～`192-dirty-model-preserved-after-unlock.png`。未把自动接受的检查成果称为“未接受候选”实窗 |
| FLOW-10 | 通过 | UI恢复资格覆盖生成／保存／加载／处理；PG所有pending blocker及原生worker排空；WIN-04／05运行期间入口可见但禁用，终态后再次显式点击才解锁 |
| ASYNC-01 | 通过 | HR排队取消不解析，worker_finished／drained恰一次；LC superseded queue不解析，所有权正常释放 |
| ASYNC-02 | 通过 | HR真实parser同步点取消与排空；第三版运行worker取消后settled、rollback，随即request3成功，见WIN-04 |
| ASYNC-03 | 通过 | UI真实可见时间Deadline／stalled preview超时后可重试同G-code；第三版约50秒解析未耗首帧预算，主动离页及最小化恢复无误超时 |
| ASYNC-04 | 通过 | HR旧generation结果不安装、旧drain不清新busy；UI旧route／frame拒绝；LC旧mount不能释放新owner |
| ASYNC-05 | 通过 | HR“Canceled history work settles…”在drained同步设置新owner／busy后，旧完成不再清零，重复通知无二次恢复；WIN-04真实恢复／重载 |
| ASYNC-06 | 通过 | HR冻结取消／异常不暴露部分payload；UI错误首帧／超时与旧route门禁，LC回滚保留原source／session，PG失败替换保留有效动作。此项为可控消费者／呈现fixture，不宣称真实GPU驱动故障实窗 |
| ASYNC-07 | 通过 | UW交接锁与页面独立；UI／LC隐藏只保dirty、独立生成不取消；第二版高级交接、第三版YOYO／原生导入隐藏完成均不抢页 |
| ASYNC-08 | 通过 | HR真实加载器缺失／取消／坏OBJ、Windows独占锁不可读，均返回错误／空render／无proof／未安装且旧模型摘要不变；PG失败替换、原生slice/import终态门禁，仍可侧栏导航 |
| ASYNC-09 | 通过 | HR重复cancel／worker_finished仅一次drained；UW重复commit／restore不重锁；UI切片result仅呈现一次；真实20轮无新增提交或重复导入 |
| ASYNC-10 | 通过 | LC queued／running／transaction／retired worker关闭排空、销毁后回调和mount lease；HR取消后旧完成拒绝；第三版真实运行前景推理后Alt+F4正常退出，`E/exit-v3.json` |
| NATIVE-01 | 通过 | 第三版有效G-code长5431700字节，往返前后SHA一致`462BF33C…2A2E8650`；同一结果可导出。导出回调日志为skip slicing，不当作新增实际切片 |
| NATIVE-02 | 通过 | 第三版位置修改／脏状态跨页保持、Undo／Redo成功；真实显式恢复编辑后模型／原生工程未丢失；`G/181-native-undo-after-navigation.png`、`182-native-redo-after-return.png`、`192-dirty-model-preserved-after-unlock.png` |
| NATIVE-03 | 通过 | 原网格／材质、Y185.35变换、用户预览视角往返保留；青绿编辑及第二耗材仍可用；`G/179-native-user-view-before.png`、`187-actual-color-edit.png`、v3-round19／20；未修改首显相机 |
| NATIVE-04 | 通过 | 第二版原兔A与高级机器人B独立，侧栏仍为A，显式B才走交接；第三版旧兔与生成企鹅同样分离；UW／HR失败及身份校验保A |
| NATIVE-05 | 通过 | 第二版真实停止sidecar、18771无监听，仍可只读往返和显式恢复高级原稿；`E/offline-proof.json`及`G/082-offline-explicit-advanced-editing.png` |
| NATIVE-06 | 通过 | 真实打开旧历史／原生3MF并跨独立实例启动，普通入口不锁；UW新context无锁、history/library不改旧anchor、显式restore不复活任务；现有文件格式／生成状态测试保持 |
| WIN-01 | 通过 | 复用智能完成成果，实际交接→只读→工作台→显式恢复；第三版最终重交接及小窗口恢复复验，`G/194-rehandoff-after-clean-edit.png`～`202-small-window-restored-editing.png` |
| WIN-02 | 通过 | 第二版复用既有Tripo高级组`4cafa542-c827-4003-9120-32d5475fd222`，原四图／描述／选项与恢复一致；`G/070-advanced-existing-groups.png`～`084-local-rehandoff-no-generation.png`，无新Tripo调用 |
| WIN-03 | 通过 | 第三版真实打开qa-native-baseline.3mf、改位置、19:46:29切片→19:46:31.100完成自动预览→round19／20只读AI往返→继续预览／导出／Undo／Redo；`G/174-native-project-open.png`～`182-native-redo-after-return.png`、`E/v3-gcode-before.json`、`E/v3-gcode-after-navigation.json` |
| WIN-04 | 通过 | 第三版request2于19:03:28.507 started，19:03:42.072用户取消，42.077 worker_pending=true，42.105 settled，43.268 rollback；request3重载成功，无永久busy；`G/114-v3-history-request.png`～`119-v3-reload-after-cancel.png` |
| WIN-05 | 通过 | 第二版高级真实检查／修复114908ms；第三版蓝企鹅检查／修复及美颜改色实际运行时往返，结束后可恢复；`G/079-advanced-workbench-running.png`、第三版129～143及v3-round记录。自动接受成果与未保存手工编辑分别取证 |
| WIN-06 | 通过 | 第三版主窗口真实YOYO job`8097f158-52ad-44a3-b2d6-e2ea5951c5b0`／flow`687c9dfa-6138-48a4-9def-761e7519bf3d`／task`1514227138890186752`，1 attempt，ready／ready、GLB完成；等待往返旧兔保留、就绪未交接不锁、显式企鹅交接才锁。首版另1次YOYO成功保留在`E/real-generation-initial.json`，本轮合计2次3D调用，新增Tripo 0 |
| WIN-07 | 通过 | 第二版断开真实本地服务并记录无监听，缓存模型／组可查看、导航及显式恢复编辑；随后独立启动服务用于第三版；未恢复旧生成任务 |
| WIN-08 | 通过 | 第三版同PID32412完成20轮（v3-round01～20树／截图），含运行取消、真实生成／处理、后台完成、最小化恢复；最终缩至1251×781仍可见恢复按钮并实际解锁，正常关闭exit0。主屏3120×2080、日志dpi=2／dip100_px=200；不外推其他DPI或多屏 |

### 9.4 交付边界

最终测试数据位于独立`.tmp/dev/data/creation-workbench-navigation-20261117`，QA PID32412及sidecar24360均已结束，原用户实例／工程／2227份源资产保留。运行树`.tmp/dev/run/Honor Forge Studio.exe`为已验证内部版本；按用户追加授权，本稿与代码／测试／维护文档形成普通本地Git提交，两份进度文档继续排除；不推送、不打包发布。模型首次朝向、粒子主观视觉评审、旧性能目标及其他历史未验项仍按原记录保留，不能由这44项通过推出完成。

## 10. 高级只读操作区UX对齐补充方案（2026-11-17）

当前授权／状态：用户已追加“实施并验收全部用例通过”，本节方案获批；2026-11-18进一步明确“授权1次完整智能生成，完成第10项”。当前代码修改、223／223项定向测试及ADV-UX-01～10全部通过，追加一次2D设计＋YOYO 3D已用1／1、零Tripo。首轮9项实际证据见10.7，智能续验及最终结算见10.8。

归属：MAINLINE-BUGFIX-001／workbench-navigation-and-initial-view-20261117范围内补充，当前会话单writer，沿codex/team/integration／ad75c3bffb。用户先审批方案，后明确批准实施；10.1～10.6保留获批设计，10.7记录当前实测，前文44项验收不移作本次UX通过证据。

### 10.1 目标与范围

以用户认可的智能模式截图为标准，对齐“高级模式已有3D成果→进入工作台→侧栏AI创作”后的左侧底部操作区。左侧“返回AI创作”为带图标的灰色次按钮，右侧“返回工作台”为带图标的渐变主按钮及底部光晕；横向等宽、高44DIP，沿用共用组件现有字体、圆角、间距及交互样式。只读提示位于双按钮下方，采用智能模式相同字号、颜色、内边距和换行规则。

本次仅改高级页操作区的呈现和已有动作的绑定适配。ModelGenerationFeatureHost的CreationWorkbenchContext继续拥有只读归属，原Shell继续拥有返回资格／恢复／导航；高级Session继续拥有草稿／组／选项，Plater继续拥有工程／预览／G-code／撤销。供应商、下载、生成流程和历史格式均不在本次修改范围；验证复用已有真实模型，不新增生成或Tripo真实调用。

### 10.2 状态与行为

| 当前状态 | 左按钮 | 右按钮 | 提示／退出行为 |
| --- | --- | --- | --- |
| 已交接、只读、允许恢复 | 返回AI创作，启用 | 返回工作台，启用 | “当前为只读查看，点击“返回AI创作”恢复编辑”；左键走既有显式恢复及确认 |
| 已交接、只读、暂不允许恢复 | 返回AI创作，禁用 | 返回工作台，启用 | “当前为只读查看，工作台处理中，完成后可返回AI创作”；忙碌结束只更新资格，不自动解锁 |
| 未交接的普通完成态 | 重新生成，沿原资格 | 进入工作台，沿原资格 | 保留原完成态和原首次交接保护，无只读提示 |
| 显式恢复后的普通创作态 | 按原2D／3D阶段显示动作 | 按原2D／3D阶段显示动作 | 移除只读提示，恢复原草稿编辑，不自动发起生成 |

只读操作区的显示依据是现有创作锁，不依赖当前2D／3D页签或模型预览是否重新就绪。页签切换、输入禁用及历史保护沿用已有资格，不借本次UX修改放宽。

### 10.3 具体改动

1. 高级只读态复用现有GenerationCompletionActions，调用其set_read_only／set_enabled；不复制一套按钮绘制或修改共用组件样式。普通完成态继续使用同一组件原有行为。
2. 左按钮回调按当前只读状态分发：只读时重新校验can_return并调用return_creation；非只读时调用原高级生成入口。右按钮继续使用既有open_workbench回调，其只读路径恢复已有工作场景。不得把只读左键误接为重新生成，也不得为右键新增导入／交接路径。
3. 只读态不再显示旧的纵排view_model／return_button；移除仅服务该纵排恢复入口的冗余控件及引用，保留普通查看模型的既有语义。
4. 在高级action_panel内添加底部只读提示，文本及视觉规则与智能一致；中央status不重复显示相同只读提示，非只读阶段的状态／错误消息按原逻辑显示。
5. 操作区统一投影最终显示状态，按实际显隐／提示尺寸变化更新布局；避免apply／模型首帧回调重新覆盖只读按钮。两种只读状态之间刷新时更新启用和提示，退出只读时恢复普通完成态布局及标签。

### 10.4 预计文件与验证方式

- 生产代码原则上仅修改`src/slic3r/GUI/Redesign/AdvancedCreationWorkspace.cpp`。`InlineModelGenerationView`已有API足够，本方案不要求更改共用按钮类、Shell、Host或业务契约。
- 随实现同步`Docs/architecture/PLATFORM_ARCHITECTURE.md`中现有创作／工作台说明，补充两模式只读操作区共用呈现规则；无需新增ADR、架构模块或进度台账。
- 自动验证复用`slic3rutils_tests`中已有AdvancedCreation、CreationWorkflow、UiRedesign、InlineModelGeneration相关定向用例，覆盖恢复资格、锁保持、完成门禁和来源保留。只有发现现有行为测试缺口时补充必要用例，不以控件字符串／源码匹配测试代替UX验收。
- Windows复用本checkout的`dev.ps1`和`.tmp/dev/{build,run,data}`，审批后先改代码，再增量构建／定向测试、安装完整匹配运行文件，最后进行真实主窗口验收。使用独立配置及已有成功高级四图／模型副本，保留原件及用户实例，不新生成、不调用Tripo。

### 10.5 核心测试用例（10项）

| 编号 | 场景／操作 | 通过标准与验证层次 |
| --- | --- | --- |
| ADV-UX-01 | 高级已有3D成果→进入工作台→侧栏AI创作，查看3D阶段只读操作区 | 实窗：横向左“返回AI创作”／右“返回工作台”，等宽44DIP、图标／字体／圆角／渐变／光晕与智能一致；没有旧纵排按钮 |
| ADV-UX-02 | 沿现有允许的路径分别查看高级2D／3D阶段的只读页 | 实窗：两阶段底部返回入口和提示均可见，不依赖3D预览重新就绪；中央不重复只读提示，输入及页签资格保持原规则 |
| ADV-UX-03 | 真实工作台处理期间切到高级AI页，等待处理终态 | 定向测试＋实窗：左按钮可见但禁用，右按钮可用；底部显示处理中原因。终态后按原资格启用左按钮并更新提示，不自动解锁；点击时仍校验资格 |
| ADV-UX-04 | 只读页点击“返回工作台”，包括已有切片预览的场景 | 实窗：回到原工作场景／最近有效子视图，同一模型及有效G-code保留；不再次导入、不自动切片，创作锁保持 |
| ADV-UX-05 | 允许恢复时点击“返回AI创作” | 定向测试＋实窗：显式恢复原高级组／四图／描述／风格／建模选项，解除只读并隐藏提示；不新增任务或自动重新生成，工作台已有模型仍保留 |
| ADV-UX-06 | 工作台有未保存修改时，点返回AI创作，分别取消和确认 | 实窗：沿原确认流程；取消后仍只读、操作区保持；确认后才恢复编辑，保留既有工作台工程保护，不丢失或覆盖原模型 |
| ADV-UX-07 | 高级未交接完成态及普通创作状态回归 | 定向测试＋实窗：未交接仍为“重新生成／进入工作台”，恢复后按当前2D／3D阶段显示原动作；生成中／失败／停止的原控制规则由既有定向测试覆盖，不误套只读返回动作 |
| ADV-UX-08 | 连续5轮工作台／AI往返，伴随状态刷新并检查重复点击 | 实窗：锁、组／任务及选项保持；按钮不回退旧布局、不重复出现、不触发意外生成／导入或额外确认；迟到刷新不恢复只读页的生成动作 |
| ADV-UX-09 | 当前DPI下常用窗口及窄窗口，缩放、最小化恢复 | 实窗：两按钮文案完整，无遮挡／重叠；提示可换行且操作区可见，光晕不覆盖提示。记录实际窗口尺寸／DPI，窄窗优先复用可达的1251×781，不外推未测DPI |
| ADV-UX-10 | 智能模式相同只读旅程及普通完成态对照 | 实窗：用户认可的按钮UX、提示、忙碌禁用、显式恢复和普通完成态均保持；同DPI／相近侧栏宽度截取操作区对照 |

### 10.6 执行顺序与交付判定

审批后依次完成：高级操作区改动与维护说明→增量构建及相关定向测试→完整匹配Windows主窗口10项验收→diff复核与结果交付。实际失败在此范围修正后重验相关项；任何未执行／不具备条件的用例须明确列出，不记为通过。

证据沿忽略目录`.tmp/dev/evidence/advanced-readonly-actions-20261117/`保存实际源码／产物身份、定向测试结果、操作区截图、真实旅程及组／任务身份核对。原44项及Tripo下载验收沿其各自版本保留，本次不复用为新通过结论。审批时仅提交本稿，后续实施以10.7实际记录为准；本方案本身不授权Git提交、推送或发布。

### 10.7 实施与当前验收结果（2026-11-18）

本节保留追加生成授权前的9／10结果及当时限制，最终10／10结算见10.8。

实现限于`src/slic3r/GUI/Redesign/AdvancedCreationWorkspace.cpp`：只读时复用共用横向双按钮，左回调重新检查原恢复资格，右回调沿原工作台导航；底部提示采用智能字号／颜色／内边距和换行规则，中央不重复显示。2D／3D均按原Host创作锁投影，忙碌终态只更新资格；未交接完成态仍显示原生成／交接动作。未修改智能页、共用按钮样式、业务状态、供应商、下载或历史格式。维护文档同步`Docs/architecture/PLATFORM_ARCHITECTURE.md`“AI创作与工作台往返”段；工程导航及契约没有变化，无需新增ADR或修改review-map。

验证版本：HEAD `ad75c3bffb3a4d6250f790121b530bccfaf3d1a0`加上述未提交改动，源码文件SHA256 `3042db87dbfd48ee35dd218c35180e66e1cb01a37a154f0db9afb626ba55c549`。`dev.ps1 CppTest`按`AdvancedCreation|CreationWorkflow|UiRedesign|InlineModelGeneration`筛选，223／223通过（`.tmp/dev/logs/20261117-235153-297`）；`dev.ps1 Run -NoLaunch`五步PREPARED（`20261117-235454-479`），19656运行文件匹配，native identity `7a1e8678ce7cfc4b78c33ed39ec9aa726ee3e7e6a06eb643a22d5f4b4f7041b5`。EXE SHA256 `650068ff02444de85260d68e08ed3b77390348ef1dac2c2dbe3e2957087d81a3`，DLL SHA256 `709c59e87943632453067e722e9a9c455cc02f9eeec59531e9b084c2cb5a26ff`。构建／逻辑测试／实窗分别取证，不把223项累计成界面验收项数。

以下`E`指`.tmp/dev/evidence/advanced-readonly-actions-20261117`，`G`指`E/gui`。独立datadir为`.tmp/dev/data/advanced-readonly-actions-20261117`，使用原高级组`9f0a36ec-26f9-4206-bc0e-cfa29762b309`及真实任务`4cafa542-c827-4003-9120-32d5475fd222`副本；智能已复制原成功任务`8097f158-52ad-44a3-b2d6-e2ea5951c5b0`及其完整资产登记，但未将其伪装成新生成会话。

| 编号 | 当前结果 | 本次版本证据 |
| --- | --- | --- |
| ADV-UX-01 | 通过 | `G/009-advanced-readonly-3d-tab.png`、`021-advanced-readonly-normal-window.png`；实见横排等宽双按钮、图标、渐变、光晕及下方提示，复用原共用组件绘制；智能新版本完整对照另记10 |
| ADV-UX-02 | 通过 | `G/008-advanced-readonly-busy-3d.png`、`009-advanced-readonly-3d-tab.png`、`034-dirty-workbench-ai-navigation.png`；高级两阶段均有只读出口，输入禁用、中央无重复提示 |
| ADV-UX-03 | 通过 | `G/008`、`010-advanced-busy-restore-click-rejected.png`、`020-round05-ai.png`；真实检查／修复期间左禁用右可用，禁用左键重复点击无效，处理终态更新提示但保持锁；资格逻辑由223项相关回归覆盖 |
| ADV-UX-04 | 通过 | `G/058-native-preview-before-roundtrip.png`～`061-native-preview-verified-after-roundtrip.png`；真实原生兔模型工程本地切片后，高级只读页右键返回原有效预览。`E/native-preview-before.json`及`native-preview-after.json`中同一19997872字节G-code的SHA256 `9bc772067243745595e58bf378c50a5b7c985a843d074ec8cd42479fa790ba1d`、mtime及1次成功切片计数不变，往返增量日志无导入／切片事件 |
| ADV-UX-05 | 通过 | `G/025-advanced-explicit-return-to-editing.png`、`026-advanced-completed-after-unlock.png`、`039-unsaved-return-confirm-restores-draft.png`；显式恢复同组、四图、172字节描述及原建模选项，提示消失且控件启用，无新增Job／attempt |
| ADV-UX-06 | 通过 | `G/033-apply-real-unsaved-color-edit.png`实改592252个面；`035`确认弹窗、`037-unsaved-return-cancel-keeps-lock.png`取消保锁、`039`确认后编辑；`040-after-confirm-workbench-edit-preserved.png`原工作台改色仍在。随后将副本修改保存为独立美颜成果，没有覆盖原模型 |
| ADV-UX-07 | 通过 | `G/005-advanced-completed-unlocked.png`、`026`、`045-advanced-normal-complete-before-rehandoff.png`；未交接3D完成态仍为重新生成／进入工作台，2D恢复原动作；生成／失败／停止控制由已有定向自动测试覆盖，本轮没有新生成来冒充实窗覆盖 |
| ADV-UX-08 | 通过 | `G/011-round01-return-workbench-double.png`～`020-round05-ai.png`；5轮往返，其中第1／5轮右键双击，并经历真实处理终态刷新；锁、原组／描述保持，无旧纵排或额外生成／导入 |
| ADV-UX-09 | 通过 | `G/008`、`021`、`024-advanced-restored-rehydrated.png`；窗口截图坐标尺寸1251×781及1560×992，当前运行日志dpi=2，按钮／提示完整、可换行，最小化恢复后不变；不外推其他DPI／GPU |
| ADV-UX-10 | 未完成，等待额外授权 | `G/067-switch-smart-for-comparison.png`、`068-refresh-complete-smart-history-copies.png`只验证智能初始页。智能页挂载工作台历史，现有WorkbenchAssets过滤smart记录，且智能旧生成会话不会恢复；已有成功副本无法进入本次版本的智能普通完成态。未修改恢复业务或伪造会话，不借旧截图宣称本轮通过。已请求1次完整智能生成（2D＋YOYO 3D、零Tripo）授权，获准后仅补此旅程 |

原件与调用核对：`E/originals-and-jobs-check.json`中1786份来源文件摘要全部不变，QA仅原2份Job、attempt文件一致、无新Job。高级Job字节一致；智能Job副本有两处既有读取规范化差异：重算`smart_elapsed_seconds`，并补齐`image_metrics.material_fragmentation_ok=true`；任务／状态／attempt不变，不把该副本元数据说成字节相同。`E/service-calls-audit.json`当前为0个本地POST、0个供应商事件，新增生成／Tripo真实调用均0。原生工程另存`E/qa-native-session.3mf`，G-code另存`E/qa-native-preview.gcode`；原工程及模型保留。

QA PID42364及sidecar15216均正常退出，exit code均0（`E/qa-exit.json`），没有在等待授权时遗留运行任务。

Diff检查通过；文档同步候选检查没有错误，提示无PR声明的REVIEW_NEEDED，不能冒充PR检查通过。没有新提交／推送／打包。当前只结算9／10通过，第10项待上述授权与实际完成后另记；原44项、下载交付和其他未验／暂停目标保持。

### 10.8 智能对照续验（2026-11-18）

用户明确授权一次完整智能生成（2D设计＋YOYO 3D、零Tripo），取代10.1／10.4本次验收零新增生成限制，产品修改范围不扩大。沿10.7同一源码／EXE／DLL和独立配置，经真实主窗口输入原企鹅描述、选择YOYOClaw、提交一次完整智能生成；生成任务`749597de-a8e7-42d4-958f-ba72038ccb2c`、flow`bb17786d-464c-46a4-8cb4-3aa98ee7d60c`、YOYO task`1514441350593945600`，1次2D请求和1个3D attempt，ready／ready。最终GLB26349800字节，未追加第二次生成或调用Tripo。以下`S`为`E/smart-validation-20261118`，完整旧证据保留。

ADV-UX-10通过：`S/gui/012-smart-normal-completion-actions.png`显示普通完成态仍为重新生成／进入工作台；显式交接成功后`015-smart-readonly-idle.png`显示横排返回双按钮及底部提示，输入禁用。真实本地网格修复期间`029-smart-readonly-mesh-repair-busy.png`的左按钮禁用、右按钮启用，`030`双击禁用左键无动作，`031`返回原美颜场景且候选待确认保护仍在。恢复修改前并结束处理后，`034-smart-readonly-after-processing-no-auto-unlock.png`重新允许恢复但保持只读；`035-smart-explicit-restore-editing.png`才显式解锁，原64字节描述、多色写实风格及YOYO选择保留，提示消失、普通动作恢复。`036-smart-workbench-preserved-after-unlock.png`确认工作台成果仍保留，无额外生成或导入。

外观对照通过：`S/readonly-ux-comparison.png`由本次智能015／029和同一受测版本高级021／008的底部操作区原尺寸裁剪组成，未缩放。当前dpi=2、两页侧栏344像素，横向双按钮的等宽、图标、字体、圆角、渐变／光晕和底部提示一致；焦点／悬停状态不作逐像素相同要求，不外推其他DPI／GPU。完整源截图、裁剪坐标及人工核对见`S/ux-comparison.json`。

测试准备及证据限制：在独立配置中尝试按模型配色配置耗材，现有单槽耗材未满足配色，随后按现有入口适配并保存独立白色美颜副本用于真实网格修复；原生成GLB／图片保持。快速配色尝试及已有检查缓存没有被算作修复忙碌证据，忙碌判定仅使用029／030的实际禁用树和截图。修复候选选择恢复修改前，正常结束后才验证显式恢复；未修改智能恢复业务或伪造会话。

收尾核对：`S/assets-and-jobs-check.json`中1786份来源、原4份Job／attempt及新Job／attempt／GLB／preview往返前后摘要全部不变，仅新增上述一个Job及一个attempt。既有原生G-code SHA256仍为`9bc772067243745595e58bf378c50a5b7c985a843d074ec8cd42479fa790ba1d`。QA PID32460及sidecar44632均正常退出、exit0（`S/qa-exit.json`）。源码／EXE／DLL与10.7身份完全一致，本轮只补实窗，不重复构建和已通过测试。最终**ADV-UX-01～10共10／10通过，相关自动测试223／223通过**，总表及逐项映射为`E/validation.json`。本需求无未结用例，其他历史未验／暂停目标保持；没有Git提交／推送／打包。

### 10.9 本地Git交付（2026-11-18）

用户追加“提交代码，形成代码提交记录”，取代此前不提交限制。沿`codex/team/integration`，以`ad75c3bffb3a4d6250f790121b530bccfaf3d1a0`为父提交，将本次`AdvancedCreationWorkspace.cpp`、平台维护说明及本验收方案三文件形成普通本地提交；两份含历史改动的主计划／短状态继续保留本地。提交号、逐文件blob／SHA及原验收身份映射保存`E/git-commit.json`；验证沿用10.7／10.8的同一源码、223／223项定向测试及10／10项验收。此次仅增加Git交付，无新增构建、应用操作或供应商调用，不推送或打包发布。
