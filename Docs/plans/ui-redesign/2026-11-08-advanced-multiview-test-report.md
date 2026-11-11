# 高级模式多视角实施与测试报告

- 任务：`UX/ADVANCED-MULTIVIEW-20261107`；执行日期：2026-11-08～11。
- 基线：[需求](2026-11-07-advanced-multiview-requirements.md)、[原76条验收定义](2026-11-07-advanced-multiview-acceptance.md)、[架构](2026-11-07-advanced-multiview-architecture.md)、[详细设计](2026-11-07-advanced-multiview-detailed-design.md)。
- 交付决定（2026-11-09）：用户明确选择“先保留两项未验，交付当前报告”。本报告按该决定交付；原76条保留 **74通过、2未完整执行**，不宣称全部验收通过。
- 原基线结论：高级独立流程已实现，已取得真实GPT四视角生成／定向修改、Tripo四图共同建模、历史恢复和智能隔离证据。下方R2结果独立统计，不以旧供应商成功替代新代码端到端验收。

<a id="mv-r2-results"></a>

## 2026-11-11 失败交互整改：实现与分层验收

用户已确认开发和42条用例验收，并批准最多12次YOYOClaw真实调用。本机实现、定向自动回归、完整Windows构建、历史副本实窗及下方V1～V5端到端执行已完成；42条按定义所需的A／W／V分别判断，当前为 **16条通过、25条部分覆盖、1条未执行**。部分覆盖表示已有有效证据，但尚缺该条要求的某一层级或参数场景，不等同整条通过。真实供应商新增 **9／12次**：Doubao1、Hy2、Qwen6；7张正常发布、1张停止后仅归档、1次上游错误。已验证失败后解锁、换模型多图、空位补图、在途新建停止后续、组内串行及新四图／历史恢复；跨套图排队窗口未捕获，完整Host竞态等缺口保留，不能宣称42条全过。Qwen视角质量另记未通过，不用请求成功替代内容验收。

### 实际实现与本轮修正

- 删除设计卡片左上恢复／重试控件及相应创建、布局、事件；目标本轮结束未成功时右下显示“生成失败”，旧图保留，首次无图失败空位也可选择。统一从主按钮按当前模型准备计划，单张、多张、全部均可更换模型；旧失败不再全局锁住新2D意图，配置、输入与真实在途保护保持。
- Application统一处理旧图编辑、缺图补生成及必要主图依赖；确认列当前模型、实际方向、依赖和调用上限。有主上传参考可用时不额外补主设计图；两张缺图共享必要主图一次调用。可见编辑基图与服务端current分别校验，发布继续使用版本比较保护。
- 新建立即清工作区并回2D顶部，停止旧未提交视角；epoch、命令归属和任务级回执隔离旧回调。全局单执行器保持，真实排队投影与执行分开；单资产缓存失败不阻止整组终态和其它图片刷新。已得响应／版本保留，不自动新增付费请求。
- 实窗发现上传缓存下载完成后路径未变，上传区仍显示空位。`UploadSlot::display`现会在同一路径由不可解码变为可用时重新加载，并以无异常文件状态检查处理；清空一份隔离缓存后通过真实资产下载复验。旧失败截图保留于`w-before-fix-upload-placeholder`，修复证据为`w-fixed-cold-reference-download`。
- GPT全部参数保持原实现，包括编辑`size=auto`、`input_fidelity=high`。YOYOClaw三模型协议、尺寸及参考顺序保持；默认精确Qwen，标签去除`-PC-YOYO`。未增加并发、版本回退界面或智能／3D业务。

### 源码、构建与自动证据

本会话沿用`codex/team/integration`，构建／验收基线HEAD为`40680c85f71ef5bdb0fc577b2765dc9d30dad56a`，当时运行的源码包含工作树修改，具体身份见下表。用户随后明确要求排除主计划与短状态，将其余26个修改文件记录为一次本地Git提交，本报告随代码提交；不推送、修改PR或发布。完整运行目录为`.tmp/dev/run`，入口`Honor Forge Studio.exe`；GUI独立数据为`.tmp/dev/data/advanced-r2-20261111`。

| 证据代号 | 范围与结果 | 收据／身份 |
| --- | --- | --- |
| A1 | Python高级2D 75／75；含混合补图、共享主图预算、停止先于受理、串行队列、废弃排队B不阻塞C、已得结果损坏和版本保护 | `.tmp/dev/logs/20261111-214018-433/result.json`，source `1dc002e792c5c569307182f357ac5bbe791cbaa942ec5d6b9782c891ca5afa33` |
| A2 | C++ `AdvancedCreation` 22／22；含失败空位选择、可见旧图、旧失败解锁、新建epoch及点击清理 | `.tmp/dev/logs/20261111-205410-691/result.json`，source `0731d5923e0b8040c0cf14dc08a93f258216835feada1cea2de2736783f59de0` |
| A3 | Python四图建模17／17 | `.tmp/dev/logs/20261111-204330-222/result.json` |
| A4 | Python旧Sidecar HTTP合同80／80 | 同A3收据；原非高级入口与合同回归 |
| A5 | Python YOYOClaw 34／34；目录、三协议0～3有序参考、尺寸、错误与脱敏 | 同A3收据 |
| B1 | `Run -NoLaunch -Jobs 2`的catalogs、build、install、runtime-check、python-check五步全部exit 0 | `.tmp/dev/logs/20261111-212957-935/result.json`；source `21e4656c129e801e2415ce129a97261deaff33428dd4a4764e952f5d4c03cd5e`；native `5819cf105a0ac1145a6c3e319c19cdb5ab882f8c7e2201984fd9e02523dae685` |

Python合计206／206，C++22／22。A2至B1的生产源码差异仅上述Workspace缓存刷新修正，已编译和实窗复验，Session／Host测试源未变。B1之后仅为A1增加两条共享主图／废弃队列用例及收尾文档，生产源码未变；身份差异与哈希核对记录在`.tmp/dev/advanced-r2-20261111/test-source-continuity.json`，不将不同完整source identity写成相同。

架构映射为`MAP_CURRENT`（111锚点、29能力），生成说明为`README_CURRENT`；`git diff --check`通过。文档同步工作树候选检查无errors、状态`REVIEW_NEEDED`（advisory），列出平台／review-map／能力说明的实际更新，不等同已提交PR的声明通过。`scripts/verify_ai_integration.py --json`仍有原基线MainFrame.cpp/.hpp、Plater.cpp/.hpp四项diff预算错误，未放宽规则，详见`.tmp/dev/advanced-r2-integration.json`；不宣称工程集成门禁全绿。

### Windows真实主窗口证据

截图和UI树统一保存在`.tmp/dev/advanced-r2-20261111/`，下表给出文件名前缀，每项有同名`.txt`及`-0.png`等图片。初始历史与选择操作使用整改初版，缓存故障、离线新建及完整成功组全选确认已在B1最终构建复验；最终实例PID23436，配套侧车PID39036。W阶段窗口截图为1560×992，V阶段为1544×981；本轮未独立记录DPI，未完成其它窗口／DPI矩阵，不能引用旧版192DPI作为本轮证据。

| 代号 | 真实操作与结果 | 证据前缀 |
| --- | --- | --- |
| W1 | 旧502组四图保留，失败卡单选／取消／多选，有图双击预览保留选择；没有卡片恢复／重试按钮 | `w-failed-multiselect`、`w-failed-old-image-preview`、`w-preview-preserves-selection` |
| W2 | YOYOClaw默认Qwen、三标签无尾缀；Qwen左单图最多1张、Hy两个旧图最多2张，均取消且保留选择／旧图；GPT未配置按原规则禁用 | `w-whitelist-labels`、`w-failed-left-qwen-confirm`、`w-failed-two-hy-confirm`、`w-cancel-retains-failed-selection` |
| W3 | 首次部分失败空位可选，空后视角补1张；主旧图＋后空位混合2张；Doubao全部4张，取消未发请求；双击空位不打开预览 | `w-first-partial-failure`、`w-failed-empty-selected`、`w-empty-back-one-confirm`、`w-mixed-front-back-confirm`、`w-all-doubao-confirm`、`w-empty-doubleclick-no-preview` |
| W4 | 新建立即清空输入／选择／图像，连续新建可用，未保存描述清除；空位等待生成，主按钮因缺输入禁用，历史仍3组；智能往返不串草稿 | `w-new-empty-immediate`、`w-new-history-preserved`、`w-new-clears-unsaved-description`、`w-smart-isolated` |
| W5 | 收展右栏、2D／3D往返保留失败选择；缺图3D禁用并提示原因 | `w-failed-layout-collapsed`、`w-roundtrip-2d-keeps-failure-selection`、`w-missing-view-blocks-3d` |
| W6 | 完整成功组重开四图／缩略图正常；无选择时确认明确Qwen、全部四方向、最多4张，取消后旧四图和按钮保持 | `w-full-success-reopened`、`w-full-success-unselected-all-confirm`、`w-full-success-cancel-all-retains-images` |
| W7 | 清一份上传cache，真实下载后同路径上传图显示恢复，修正上传区不刷新的问题 | `w-before-fix-upload-placeholder`、`w-fixed-cold-reference-download` |
| W8 | 在隔离left缓存目标建立非空目录造成桌面写入失败：旧图保留、其余三图正常、失败右下和安全原因可见，主按钮可再次打开当前Qwen单图确认；恢复cache重开后失败清除 | `w-cache-failure-retains-image-and-unlocks`、`w-cache-failure-one-current-model-confirm`、`w-cache-restored-clears-failure`；`cache-fault.json`为restored=true |
| W9 | 仅停止自己的验证侧车后，新建仍立即空白；重启配套应用／服务仍空白、历史3组保留，无旧图回填 | `w-service-offline-before-new`、`w-service-offline-new-immediate`、`w-service-restarted-empty-history-preserved`；`offline-sidecar-fault.json` |

W8是桌面资产缓存写入故障，资产GET实际200；不是新的供应商下载失败或生图成功。W9为无在途供应商请求时的离线新建；不是在途停止／受理竞态验证。双击空位仅确认无预览，不增加“空位双击保留选择”的未约定结论。

真实历史副本：F2为`2f0814a8-4b9f-4d2e-a2e5-ad65dd14f6bd`，F3为`341d50f8-11b8-43c1-bd3b-de5016de2237`，F1为`3b9916bf-bf77-4081-a75f-7fd99481f3a7`。F2旧operation `88a6d5d5-ef03-4c31-99b1-99969ace3a63`的HTTP502／`UPSTREAM_GRPC_ERROR`、requestId `c921e904-3b75-4ec1-8ee9-3ecbef1d13ca`仍可定位；本轮没有把未知受理改写为确定拒绝。

`.tmp/dev/advanced-r2-20261111/preservation-and-no-generation.json`是获得真实调用授权前的检查点：当时三组原来源87文件hash全部不变，验证副本仅各自group.json因模型选择／草稿保存变化，新增7份未执行plan，没有新增operation／图像文件，仍为3组；当时侧车生成受理与供应商调用均0。该检查点不代表V执行后的调用数。V结束再次核对的结果在`supplier-validation.json`：原来源87／87文件hash仍不变，副本84个旧文件hash不变，仅3份group.json正常更新，已有旧版本／操作／响应文件保留；新增5份operation、实际9次调用，现为4组。原始hash清单为`copied-history-sha256.json`，原件、原日志、用户运行实例未用于故障注入。

### 42条逐项结果

A／W／V定义沿用[整改验收定义](2026-11-07-advanced-multiview-acceptance.md#mv-r2-acceptance)。下表“通过”仅用于该条要求层级均已取得证据；代码复核不替代Host竞态实测。

| 用例 | 结果 | 已取得证据与未覆盖范围 |
| --- | --- | --- |
| MV-R2-UI-01 | 部分覆盖 | W1／W3／W6确认卡片无按钮，源码已删除控件；完整Tab焦点遍历未做。 |
| MV-R2-UI-02 | 通过 | A1／A2、W4／W9：空态、输入门禁、立即新建及无空历史。 |
| MV-R2-UI-03 | 通过 | A2、W1／W2：失败旧图选择、多选、双击及确认取消，0调用。 |
| MV-R2-UI-04 | 通过 | A2、W3：失败空位选择／取消／混选，无成功预览或新增调用。 |
| MV-R2-UI-05 | 部分覆盖 | A1真实单执行器跨组控制测试通过；V2／V5实窗已见组内排队→生成。V4首请求在新组确认前已返回，未捕获跨套图等待及原因，不冒充该场景通过。 |
| MV-R2-UI-06 | 部分覆盖 | A1／A2、V2确认两张旧失败图成功更新并清错、未选图不变；V3空位单张成功。严格的“仅一张有旧图失败目标成功”组合未取得V（V1单张实际失败），不合并成整条通过。 |
| MV-R2-UI-07 | 部分覆盖 | A1部分失败、A2旧图保留通过；V1新502失败后旧图保留／解锁，V2两张成功。单次任务一成功一失败的W＋V未取得，不将不同任务混装。 |
| MV-R2-UI-08 | 部分覆盖 | W5及V5覆盖当前两种截图尺寸、右栏开合和2D／3D往返，新结果／勾选保持且无控件复活；其它支持窗口／DPI矩阵未验。 |
| MV-R2-GEN-01 | 部分覆盖 | A1首次文字／上传、方向顺序和预算通过；V5纯文字首次真实四方向、每方向1次成功。已有参考图首次四方向的本版V未执行，质量另记。 |
| MV-R2-GEN-02 | 部分覆盖 | A1当前模型单目标通过，W2换Qwen左单图确认1次；V1实际Doubao左单张1次但仍是原模型且失败，不能替代“换B的有旧图单张”V。 |
| MV-R2-GEN-03 | 通过 | A1、W2、V2：原失败组换Hy，后／右两个旧图各1次成功；front／left current与哈希不变，未因换模型扩大目标。 |
| MV-R2-GEN-04 | 部分覆盖 | A1新模型全四图通过；V4同组改Qwen确认全部四图，首请求真实发出，随后按新建场景停止其余3图。未实际完成该次全部四图重生，不用V5新组首次四图替代。 |
| MV-R2-GEN-05 | 通过 | A1／A2、W6：未选＝全部四图，取消0调用并保持当前图／选择。 |
| MV-R2-GEN-06 | 通过 | A1／A2、W3、V3：空后图按initial补图，Qwen实际1次，未增加主图；其它front／left／right current不变。图片可用与后视质量分别判断。 |
| MV-R2-GEN-07 | 部分覆盖 | A1两个缺图共享front依赖且分别覆盖front成功／失败，最大3／实际失败1次；纯文字缺主图的确认及V未验。 |
| MV-R2-GEN-08 | 通过 | A1已有主上传参考足够时不增主图，最多1次；此条仅要求A。 |
| MV-R2-GEN-09 | 部分覆盖 | A1混合编辑／补图通过，W3主旧图＋后空图确认2张；本版V未执行。 |
| MV-R2-GEN-10 | 通过 | A1／A2、W2／W3：取消无调用，保留图／正文／失败选择，可立即再次准备当前模型。 |
| MV-R2-GEN-11 | 部分覆盖 | A1／A5能力／目录不回落通过，W2 GPT未配置门禁；白名单失败／空／下架及确认间失效W矩阵未全验。 |
| MV-R2-GEN-12 | 部分覆盖 | A1／A2覆盖无效／超限／必需主参考，W4仅空输入门禁；其余无效输入实窗矩阵未全验。 |
| MV-R2-ASY-01 | 通过 | A1、W1／W2、V1／V2：旧ambiguous及本次新502均不锁新意图，随后Hy两图实际成功；原错误／requestId和费用不确定事实保留。 |
| MV-R2-ASY-02 | 部分覆盖 | A1／A2通过；V4实窗新建停止、V5真实在途禁用并于成功后恢复。明确失败且其它目标仍在执行、再使用停止入口的组合未取得W＋V。 |
| MV-R2-ASY-03 | 部分覆盖 | A1供应商响应后下载／损坏／校验逻辑通过；W8仅桌面写cache故障，非该条完整W失败矩阵。 |
| MV-R2-ASY-04 | 通过 | A1原子写／候选和版本回执保存失败、取回不新增调用、过期结果保护通过；此条仅要求A。 |
| MV-R2-ASY-05 | 通过 | A2逐图加载和终态，W8真实单cache故障仍保留旧图／刷新其它三图、主按钮可继续。 |
| MV-R2-ASY-06 | 部分覆盖 | A1重启不自动生成、W9离线新建及重连空白通过；运行中轮询失败与未决回执重连未演练。 |
| MV-R2-ASY-07 | 部分覆盖 | A1／A2、V4确认首请求submitting时新建立即清空；实际只调用1次，另外3图停止0次，晚到结果仅旧组归档且current不变；V5新组四图／busy无污染。跨套图排队窗口未捕获，故仍非整条通过。 |
| MV-R2-ASY-08 | 部分覆盖 | A1 stop先／后受理和迟到提交收敛通过；完整Host丢受理响应／新建时序与回执归档未有自动演练。 |
| MV-R2-ASY-09 | 部分覆盖 | A1 A／B／C真实单执行器中B废弃0调用且C正常通过；W队列期间再次新建未演练。 |
| MV-R2-ASY-10 | 部分覆盖 | A2旧快照／新建点击epoch，W1／W4选择与新建；全部Host成功／错误／下载／上传保存／润色延迟组合未参数化演练。 |
| MV-R2-ASY-11 | 部分覆盖 | A1相同command仅一次受理，W2／W4取消再准备／新建连点通过；实窗提交连点幂等未执行。 |
| MV-R2-ASY-12 | 未执行 | 代码已实现任务级与旧单回执兼容；旧／新回执并存、迁移写失败、逐项归档和重启的Host完整A＋W未执行。 |
| MV-R2-REG-01 | 通过 | A1 GPT纯文字／原图／编辑／补图请求参数与参考角色回归，A4旧合同通过；编辑size=auto及input_fidelity=high未改。 |
| MV-R2-REG-02 | 通过（A＋W） | A1／A5三协议、0～3参考和无回落，W2默认Qwen／三标签／GPT隐藏；V1～V5另记：Hy／Qwen实际返回，Doubao上游502，不能宣称三模型本版都生图成功。 |
| MV-R2-REG-03 | 部分覆盖 | A4旧入口回归，W4智能／高级往返；本轮未以原生工程副本完整验证AI离线普通Orca／预设。 |
| MV-R2-REG-04 | 部分覆盖 | A2／A3四图／pending／草稿／忙碌与Job-v2保护通过；W5缺图3D禁用，V5新四图可用时3D入口恢复，单选不缩减四图。未提交Tripo，其余pending／草稿门禁W矩阵未全验。 |
| MV-R2-REG-05 | 部分覆盖 | A1／A2同组上传／风格pending扩围通过，W2／W3只换模型不扩围；本版上传／风格扩围W未重验。 |
| MV-R2-REG-06 | 部分覆盖 | A1缺文件／重启；W1／W3／W6及V4／V5重开完整、失败、ambiguous、首次缺图和最新成功组，状态准确且无自动请求；缺文件组W未验。 |
| MV-R2-REG-07 | 通过 | A1确认可见旧base、服务端归属和canonical发布CAS、过期回执不覆盖，A2保留可见基图；此条仅要求A。 |
| MV-R2-REG-08 | 通过 | A1／A2、W4／W8、V1～V5：原来源87／87文件hash不变，副本仅3个旧group.json更新，旧文件84／84保留；新current与目标／版本／文件hash一致，停止晚到结果仅归旧组。 |
| MV-R2-LOG-01 | 部分覆盖 | A1／A5、W1／W8、V1新502：UI统一失败且可继续，结构化日志／operation的HTTP502、code、限长message、requestId及关联一致，中文message无替换乱码。内容审核及结果保存失败完整W矩阵未验。 |
| MV-R2-LOG-02 | 通过 | A1／A5假认证／Prompt／Data URL哨兵、长message脱敏与合法code／requestId保留通过。 |

### 真实供应商验收（最多12次，实际9次，已结束）

用户明确回复“批准，最多12次，继续端到端验收”后，按下表使用隔离历史机器人套图副本及无私人信息文字，从B1匹配Windows主窗口输入、确认、生成并重开历史。每次先核对确认中的实际方向、模型及上限；只调用YOYOClaw，GPT／Tripo新增0。失败同样计入次数，无自动重试、无追加购买。未上传人物历史组；人物组仅本地历史显示验证。

| 阶段 | 模型／操作 | 最多新增调用 |
| --- | --- | --- |
| V1 | Doubao：旧失败组只选一张旧失败图，用当前模型生成 | 1 |
| V2 | Hy：同组选择两张有旧图的失败目标，确认两张生成 | 2 |
| V3 | Qwen：首次部分失败组仅补一个无图方向 | 1 |
| V4 | Qwen：旧组全部四方向生成，观察首个请求已提交后立即新建，停止旧剩余方向；为操作时序最坏情况保留四次上限 | 4 |
| V5 | Qwen：新工作区有效纯文字首次四图；若V4仍在途，验证串行排队→执行和旧回调隔离；完成后历史恢复 | 4 |
| 合计上限 | Doubao 1、Hy 2、Qwen 9；不是每次必须用满 | **12** |

实际完成5份新operation、9次提交，已遍历隔离目录中授权后全部operation核对，无遗漏或重复操作；7张正常发布，1张停止后仅归档，1次HTTP502。完整脱敏凭据、requestId、attempt／版本ID、PNG实际尺寸、SHA256、目标外current不变及原件保护在[调用证据](../../../.tmp/dev/advanced-r2-20261111/supplier-validation.json)。所有本轮返回图片均为768×1024，PNG头尺寸与版本元数据一致且不超过1024×1024；没有用GPT作对照调用。

| 阶段 | 实际调用／结果 | operation | 实窗证据前缀 |
| --- | --- | --- | --- |
| V1 | Doubao 1次，上游502；旧左图保留，主按钮／模型选择恢复可用 | `5a840b5a-cb18-4452-ab94-b310bedae0aa` | `v1-doubao-left-confirm`、`v1-doubao-left-failed-unlocked` |
| V2 | Hy 2次，后／右均成功；清除两目标失败，未选主／左不变，缩略图更新 | `6dabbc03-459d-4aec-96b0-dd95d460ef7f` | `v2-hy-two-old-views-running`、`v2-hy-two-targets-success` |
| V3 | Qwen 1次，空后图补生成成功；其它三方向不变，后图错误清除；视角质量未通过 | `f715296b-b761-4e88-803d-3fb39e83ed02` | `v3-qwen-empty-back-running`、`v3-qwen-empty-back-success` |
| V4 | Qwen仅1次，主请求已发时新建立即空白；其余3方向停止、0调用；晚到主图仅旧组归档，旧四个current不变，重开旧图仍可用 | `9fdac8d5-5dfc-4219-8d1a-588a13ce717c` | `v4-new-immediate-while-old-request-running`、`v4-old-stopped-history-preserves-four` |
| V5 | Qwen 4次，新组纯文字首次四图逐张成功；历史重开、按钮恢复、右栏开合及2D／3D返回保留新图／选择，无3D提交 | `3e3b8ed6-e068-4e66-afd5-4840f260097a` | `v5-new-qwen-four-confirm`、`v5-new-qwen-four-running`、`v5-new-qwen-four-success`、`v5-new-qwen-history-reopened`、`v5-new-results-collapsed-selection-retained`、`v5-new-results-roundtrip-expanded`、`v5-four-usable-3d-eligible-no-submit` |

V1错误为`HTTP502 / UPSTREAM_GRPC_ERROR`，requestId为`cc956965-7b77-41e0-942a-e470366589cb`；安全message为上游gRPC `UNAVAILABLE`／连接被对方重置，耗时1219ms（传输844ms）。这次不是`OutputImageSensitiveContentDetected.PolicyViolation`。日志的`yoyoclaw.request.failed`和`advanced_multiview.image.failed`与operation关联一致，中文message已核对无替换字符；按原事实保留ambiguous／recovery_required，不将失败伪写为确定拒绝，也不再全局锁住用户。

V4主请求在新组确认前已经返回，因此V5捕获的是组内三个方向“排队中”→依次“生成中”，没有捕获跨套图等待前一个请求的UI窗口／原因；没有为了补画面再跑V4，剩余3次额度未使用。V4返回版本`8ee88a02-5981-4f4d-8442-e66f313dc5b6`仅保存在旧组，未替换旧主图；V5新组为`18b0e0d6-df74-4f1c-9680-ab9c2dedb8fa`，四current、模型、输入和忙碌状态均归新组。

内容质量单列：V3要求后视但实际接近正面；V5四图主体完整、黄色机器人及蓝色胸口按钮符合描述，但左右图均朝画面右方，后图机位偏高，**视角质量未通过**。没有修改提示词／参考规则或购买质量重试。传输、保存和交互成功不能证明四视角语义正确，也不能据此宣称YOYOClaw三模型质量验收通过。

本次明确调用计划已执行结束；D6仍因上表逐条剩余的窗口／DPI、完整Host回执／异步延迟、错误矩阵及未取得的特定W＋V组合保持部分完成。原76条两项未完整执行、旧质量问题和原集成diff预算失败保留，不扩大本轮调用或规格。

## 2026-11-08～10 历史实施与验证（原统计保留）

以下为当时实现与授权，不覆盖上方R2当前规格；其中卡片恢复／重试和旧失败全局锁的旧说明已被R2替代，原76条74通过／2未完整执行及质量失败证据不改写。
- 2026-11-10同组输入与风格重新生成补充：用户最终确认只有首次或点击“新建创作”创建新组；已有套图补充／替换／移除上传图、修改描述或风格均更新原组。上传变化视角强制进入“待重新生成”，变化目标以新上传图优先、当前设计图其次；上传未变的描述编辑仍以当前设计图优先、原上传图其次。主图替换后，用户额外选择的侧视角可读取新主图；风格变化在同组强制四图重新生成。待处理视角不可取消，成功后逐项清除，失败保留旧图和左上角单项重试；本地上传刚变化时即先以草稿失配禁止3D，服务保存后再由待处理集合继续阻止，避免自动保存窗口内使用旧四图。首次缺图后再改风格时，显式重试使用当前组输入和当前风格补缺图，其它风格待处理视角保持。Sidecar新增同组multipart输入更新，旧组缺新字段时按空集合兼容，上传原件与设计版本保持不可变。Python高级多视角75／75、C++ `AdvancedCreation` 17／17及`OrcaSlicer_app_gui`纯构建通过，收据`.tmp/dev/logs/20261110-200425-552/result.json`、`.tmp/dev/logs/20261110-200445-669/result.json`、`.tmp/dev/logs/20261110-200602-898/result.json`；实现阶段统一源码身份`296b4328ebbe1ffdf3ab89ecc48120d37bbb1cc6145c7475053107cd38e50c39`，架构映射102锚点／29能力通过。旧实例关闭后，最终`./dev.ps1 -NoLaunch`完成五步构建、安装和运行时校验（`.tmp/dev/logs/20261110-201222-046/result.json`，源码身份`0b698222cc87b278c8ab8e5069e0007a88701d02b10824925b7dd2a313cb624f`）。真实主窗口加载有主上传图且已有四设计图的组`01070a0c-f69d-4927-b362-a5d9b17f5e74`：替换右视角后历史仍为11组、同一`group_id` revision 11→12、`pending_views=right`／`changed_upload_views=right`，右卡显示待处理并锁定选中；切到3D时“生成3D模型”禁用并提示先重新生成设计图。风格改为雕塑后同组revision 13、`pending_views=front,left,back,right`，四卡全部显示“待重新生成”、全部自动选中且点击不能取消；3D继续禁用。另用无原上传主图的历史白杯设计组确认辅助图单独更新返回`missing_front`且后端未变，符合主图必填规则。Sidecar日志只有组读取、输入更新与draft保存，没有图像或Tripo生成请求；本轮GPT／Tripo均为0。
- 2026-11-10高级3D跳转与阶段隔离补充：确认高级“生成3D模型”后，界面约2秒内打开同一已受理Job的既有3D等待页；进度从“正在生成3D模型”开始，全程没有智能模式的2D设计准备／生图阶段。高级提交、停止、恢复、结果、返回创作和打开工作台均由高级专属宿主与`m_advanced_model_state`／`m_advanced_model_route`投影，智能模式只读往返未覆盖高级页面。实窗首次发现正常对账把受理阶段临时消息显示为`Four-view model accepted; no supplier task has started.`，现已改为正常提交静默对账并清除临时消息；手动恢复提示保留。真实组`2b4401d7-4db5-4ec6-b90b-c1667bd44ef0`创建Job`09a516ab-5bff-4b21-a2be-31cbe4d64a65`／Tripo task`6fcab64f-6390-4aa3-a7bf-dde65933cbff`，记录`workflow_mode=advanced`、`creation_kind=advanced_multiview_v1`，四个具名输入哈希互异，仅1个attempt，最终`ready/100%`，产物`model.glb`为31,044,012字节。完成页、原3D工作台、返回高级创作后的套图／四图／ready状态均通过真实GUI核对。本轮Tripo 1／3、GPT 0；`AdvancedCreation` 15／15、`UiRedesign` 132／132和最终Windows构建／安装／运行时校验／启动通过，收据`.tmp/dev/logs/20261110-173933-701/result.json`、`.tmp/dev/logs/20261110-174121-258/result.json`、`.tmp/dev/logs/20261110-174253-583/result.json`，源码身份`3c1612f6e6ebd49aa5e1b696107eb7cbcdae83d95ee40466d405966ccac7e4ae`。
- 2026-11-10中央标题精简补充：中央四张卡片只显示“主视角／左视角／后视角／右视角”，删除原标题中的括号及括号内相机说明；每张卡片右侧“设计图”及相机0°／左转90°／反向180°／右转90°业务语义均保持。中央使用独立短名称映射，避免改变其它区域继续使用的“正视角”。首轮`./dev.ps1 -NoLaunch`编译、安装和运行时校验通过后，静态复核发现首项复用通用名称会显示成“正视角”，已在交付前纠正；最终构建收据`.tmp/dev/logs/20261110-163420-678/result.json`，源码身份`333d6cf90873d06ee3a32815f2a9ffffa7df7479afb7d68dafc491ef0b9caa2f`。用户明确自行完成GUI验证，本报告不将本次视觉结果写为已通过。
- 2026-11-10单次生成与右栏刷新解耦补充：用户以响应时间、失败率和操作成本为由正式替换此前的自动视觉复核／纠偏试验。当前每个目标最多一次图像调用；对应方向有上传图时只用该图，缺图时最多使用主参考上传图与主设计图，局部编辑以当前目标设计图为主。用户补充指出生成图仍保留来源背景；核对确认原单图不是透明alpha，而是删除原背景并换为统一不透明纯色棚拍背景，当前高级缩水提示确实丢失了这套核心。现四条路径直接包含原`build_geometry_reference_prompt`／`build_text_geometry_reference_prompt`完整合同，并只对缺图路径增加相机角度和不可见区域推理的窄覆盖；multipart编辑补齐原`background=opaque`字段。右栏卡片刷新只跟踪套图id／名称和四个设计版本，描述、风格、普通revision和选择变化不重建卡片。Python51／51、C++15／15、完整Windows构建及配置化Sidecar启动通过，收据`.tmp/dev/logs/20261110-150530-382/result.json`、`.tmp/dev/logs/20261110-152547-689/result.json`、`.tmp/dev/logs/20261110-161013-624/result.json`。真实组`2b4401d7-4db5-4ec6-b90b-c1667bd44ef0`局部操作`47536917-1bb9-45c7-bccf-086786a48fc9`仅1次`/images/edits`，18.844秒成功，无`/chat/completions`或第二次图像调用；右图更新、其它三图不变，右栏对应缩略图刷新。连续输入描述、风格切换／恢复及生成前后均未见右栏重绘闪烁。恢复原背景合同后，同组主视角操作`4c142ae7-1375-41a7-9c3e-73b03df4223a`仅1次`/images/edits`，24.609秒成功；版本`41352218-7470-4f27-8cf0-63b6e071e444`把来源照片场景替换为均匀中灰色棚拍背景，主体轮廓和四周留白清晰，左／后／右版本ID保持不变，未发出文字复核或第二次图像请求。更换上传图规则按用户决定延期；智能、上传区、套图业务和3D未改。
- 2026-11-09视觉整改补充：用户指出原上传区只满足功能布局、未严格按Figma实现。现已以实际节点`172:5630`／`172:5666`替换为高级专用312×240上传组件，使用八个本地Figma SVG并去掉空态外置移除按钮／说明。最终运行收据`.tmp/dev/logs/20261109-102518-297/result.json`五步通过，真实主窗口核对空态、点击上传、预览、槽内移除及右栏收展，`AdvancedCreation` 12/12通过（`20261109-102713-618`）。本补充承接MV-UPL-01原视觉证据；原74／2统计、跨窗拖入和其他DPI边界不变，智能业务未改且未新增供应商调用。
- 2026-11-09左栏阶段UX补充：高级左栏新增2D／3D页签并按需求重整页签以下控件；上传组件、中央四图和智能模式源码／业务均未改。2D页使用智能同款描述／润色／风格视觉，GPT／YOYOClaw状态显示在模型列表中，常态仅2D主按钮；3D页只显示Tripo、100万／200万面、三项只读设置及3D主按钮。增补8条实窗用例全部通过，详见下节；未触发生成或增加调用数。
- 2026-11-09默认值补充：高级`DesignDraft`新建默认从“雕塑”改为“多色写实（realistic）”，GPT默认保持不变；历史快照仍恢复其保存风格。定向C++测试通过（`.tmp/dev/logs/20261109-140608-632/result.json`），完整运行五步通过并启动PID36848（`.tmp/dev/logs/20261109-141021-733/result.json`），两者源码身份`9d3e01b9ba0b1c592e1281c873971ae5a831070970df82b66937146592878c22`。真实主窗口点击“新建创作”后确认“多色写实”卡片选中、图像模型为GPT；未触发生成。
- 2026-11-09相对主参考相机补充：中央标题已改为“正视角（主视角）／左视角（左转90°）／后视角（反向180°）／右视角（右转90°）”，高级专用提示只移动相机并明确冻结头对躯干角度、物理解剖左右、接触关系及非对称／可动部件。Python 45／45、C++ `AdvancedCreation` 14／14通过（`.tmp/dev/logs/20261109-163435-231/result.json`），五步运行通过并启动PID18756（`.tmp/dev/logs/20261109-163729-710/result.json`），源码身份均为`02ab6108cc35342e48dadb28bd201b19680a86806e387eb6eae5d94beeea93fc`。真实组`79f55c4c-96f6-4354-8c36-6c78020dd1f4`／操作`bf302cb0-8252-464d-84ca-0c7dd2ebd15a`使用指定原图SHA256 `77310b486269b7e3c4916ac6f870d05155394e1ada399bf8b4441d293bfda8d4`提交4次GPT且均成功；主视图身份、裁切、服装和动作保持明显改善，但左／右90°仍近似同一侧，180°仍把头对躯干转角归正，故严格同姿态多视角质量判定为**未通过**。接口成功不等于模型质量通过，本轮不隐式重试。实窗确认右栏展开／收起时四标题完整且四格自适应；智能模式只读回归未见变化，返回高级仍恢复该组。
- 2026-11-09模式与历史套图UX补充：智能／高级顶部模式统一为黄色选中态；智能右栏只把标题改为“历史模型”，原卡片和业务不变；高级右栏改为“历史套图”，保留“新建创作”、搜索、刷新、分页和收展，以两列2×2缩略图卡片恢复整组上传图、四张设计图及描述／风格／供应商。旧高级单图记录继续保留但不在套图栏展示。活动设计卡显示黄色圆形加载，中央不再显示“等待生成”，底部“生成中…”及逐格更新保持。`AdvancedCreation` 14／14通过（`.tmp/dev/logs/20261109-172017-725/result.json`），五步运行通过（`.tmp/dev/logs/20261109-173503-529/result.json`），最新匹配运行源码身份`b0d16e2f43c4bca4238adeef55ad26fe36844500a42c88a01ed69976c4883a9c`。8条增补实窗用例全部通过：标题、两列套图、整组恢复、黄色当前标记、空搜索、收展和智能／高级往返均符合规格。真实加载组`a36d91c3-31e1-4a16-bc81-550f9d1c77fb`提交4次GPT且4次成功，实窗确认生成中只有圆形加载并逐格更新；未提交Tripo。
- 2026-11-09历史套图四项整改补充：标题字体匹配智能“历史模型”，页码与四个翻页按钮垂直居中；当前组revision／名称／四个version ID驱动卡片刷新并在操作结束后同步持久历史；单击加载、双击名称原地重命名，名称与描述隔离且两者都可搜索。实窗发现Enter保存会继续向父卡传播并触发加载，最终版本由编辑框消费Enter／小键盘Enter后通过复验。Python 48／48（`.tmp/dev/logs/20261109-193132-139/result.json`）、C++ 15／15（`.tmp/dev/logs/20261109-195317-319/result.json`）、Windows五步运行（`.tmp/dev/logs/20261109-195511-061/result.json`）通过，源码身份`583d183d9cd7c2d8dac032e5175fd5cf156ae1dd37bae3dfe3ddb277bbc3d3f7`。真实杯子组重启后以revision 16持久化“杯子套图正式验收”，四缩略图、四中央图和描述恢复；单击切组、Enter／失焦／Esc、名称／描述搜索、分页及智能标题只读回归通过。未调用供应商。
- 2026-11-09高级右栏四排布局补充：只调整高级右栏呈现，“历史套图”与右侧“新建创作”同行，搜索区上移，卡片及纵向间距收紧；每页数量仍为8。Windows五步运行`.tmp/dev/logs/20261109-201225-173/result.json`通过，产品匹配源码身份`3e37c230ba3e08ca6023f744b0f1adbf3561f5774c1189fee9d94cda23068c86`。独立12组数据实窗确认第一页两列×四排完整显示且不与分页重叠，第二页4组正常；单击杯子组恢复四图和描述，双击标题进入原地编辑，Esc保留原名；智能“历史模型”回归无变化。未改业务逻辑、未调用供应商。
- 2026-11-10上传区UX补充：只重排高级2D上传区为四个等大2×2槽位，标题行右侧放黄色“新建创作”，并以自绘深色浮层完整承载主参考语义、操作和格式提示；没有修改Session、FeatureHost、提示词、供应商、中央四图、3D或智能业务。Windows五步运行`.tmp/dev/logs/20261110-093216-731/result.json`通过，源码身份`c85e890ea996000e40b4344b52c3f589635828d1e0a68c0174a3c6018326a84b`，实窗PID39424。MV-UPLUX-01～06均通过：2D／3D显隐、提示浮层、四图cover、右槽移除／恢复、主槽替换／恢复、右栏两态下上传区稳定及智能只读回归均符合规格；未点击生成、未新增供应商调用。验收期间曾捕获一次高级右栏空白截图；用户手动操作未复现，随后使用当前匹配版本、包含8组历史套图的真实主窗口连续3轮执行收起／展开也均正常。此前“稳定复现／独立未结项”判定撤回，该截图视为自动化截图捕获、坐标或界面稳定时序的可疑状态，高级右栏当前可用且无需修复。
- 2026-11-10提示浮层配色补充：用户要求上传区问号提示与“生成模型”问号的浅色系统提示保持一致。`UploadInfoPopup`只将边框改为系统式细边框，并把背景／正文／次要文字改为`#F8F8F8`／`#202020`／`#585858`；内容、尺寸、悬停触发和上传业务未改。Windows五步运行`.tmp/dev/logs/20261110-095537-624/result.json`通过，源码身份`c914eb4cb5f5ceda00060bd814e158adab0eda345fbbe285cf85f9a08dc72f42`，实窗PID41892。真实悬停确认浅底、深字、清晰边界及三段内容完整可读；未点击生成、未新增供应商调用。
- 2026-11-10历史套图右栏收窄与卡片居中补充：高级历史面板由352收窄为320 DIP；2×2图片区保持约110 DIP、4 DIP间距和卡片164 DIP高度，改为按卡片实际宽度水平居中。`AdvancedCreation` 15/15及Windows五步运行通过，收据`.tmp/dev/logs/20261110-103009-853/result.json`、`.tmp/dev/logs/20261110-103153-622/result.json`，源码身份`4bd4877f7457e0c47216528b6d38c85286e359505df0bd14264b93295de4df13`。现有2组数据的真实主窗口确认普通／选中卡片左右留白一致、右栏视觉宽度低于左栏、收展后选中套图与中央四图恢复；本次没有伪造8组数据，卡片高度、纵向间距和每页8组逻辑未改，不以2组截图重复声明四排数据验收。未触发生成。

## 结果统计与适用范围

| 通过 | 失败 | 阻塞 | 未执行 | 总数 |
| --- | --- | --- | --- | --- |
| 74 | 0 | 0 | 2 | 76 |

“未执行”可包含已完成的局部条件或离线覆盖；只有满足该条全部必要实窗条件才记通过。GUI结论适用于本机Windows、192 DPI／200%缩放及实际记录的窗口，不外推其他DPI、跨平台或发布资格。UX是否符合用户最终美观期望仍待用户反馈。

2026-11-09新增左栏阶段UX单独统计为 **8通过、0失败、0未执行**，不回写原76条的历史口径。实窗确认进入高级默认2D、2D模型状态、3D页结构、100／200万面及“精细”联动、右栏收展、智能往返和再次进入高级复位。

2026-11-09新增模式与历史套图UX单独统计为 **8通过、0失败、0未执行**，不回写原76条的历史口径。旧高级单图数据未迁移、未删除，智能模式仅改标题与选中态呈现，业务逻辑未改。

2026-11-09新增历史套图四项整改单独统计为 **5通过、0失败、0未执行**（MV-HUX-09～13），不回写原76条的历史口径。

2026-11-09新增高级右栏四排布局单独统计为 **1通过、0失败、0未执行**（MV-HUX-14），不回写原76条的历史口径。

2026-11-10新增上传区UX单独统计为 **6通过、0失败、0未执行**（MV-UPLUX-01～06），不回写原76条的历史口径。高级右栏空白截图经用户手动操作和当前匹配版本连续3轮实窗复核均未复现，撤回范围外未结问题记录。

2026-11-10新增同组输入与风格重新生成规则共 **9通过、0失败、0未执行**（MV-INPLACE-01～09）；自动覆盖全部规则，真实主窗口补验同组换图、主图必填负向保护、待处理锁选、风格四图待处理和3D禁用，不回写原76条历史口径。

2026-11-10新增历史套图宽度与卡片居中单独统计为 **1通过、0失败、0未执行**（MV-HUX-15），不回写原76条的历史口径。本次实窗数据仅2组，四排布局不重复计数。

## 版本与证据身份

- 同组输入与风格重新生成实现阶段源码身份`296b4328ebbe1ffdf3ab89ecc48120d37bbb1cc6145c7475053107cd38e50c39`；Python 75／75、C++ 17／17和GUI目标编译收据分别为`.tmp/dev/logs/20261110-200425-552/result.json`、`.tmp/dev/logs/20261110-200445-669/result.json`、`.tmp/dev/logs/20261110-200602-898/result.json`。最终运行目录刷新收据`.tmp/dev/logs/20261110-201222-046/result.json`五步通过，源码身份`0b698222cc87b278c8ab8e5069e0007a88701d02b10824925b7dd2a313cb624f`；匹配程序`.tmp/dev/run/Honor Forge Studio.exe`、PID6544保留供用户查看。
- 分支 `codex/team/integration`；本次高级多视角连续修改已按用户要求通过`git commit --amend --no-edit`合入当前`HEAD`同一条提交，提交数量和说明不变。最新右栏收窄匹配运行源码身份`4bd4877f7457e0c47216528b6d38c85286e359505df0bd14264b93295de4df13`，收据`.tmp/dev/logs/20261110-103153-622/result.json`；此前相对主参考相机运行身份`02ab6108cc35342e48dadb28bd201b19680a86806e387eb6eae5d94beeea93fc`，完整EXE／DLL／资源哈希仍见[运行身份](../../../.tmp/dev/advanced-multiview-20261108/runtime-identity.json)。
- 左栏阶段UX最新源码身份`d277d7e7276e30e31c0787728e1ccf0da908289a68d4c85267b54a832eb73c74`；构建收据`.tmp/dev/logs/20261109-124217-477/result.json`，`AdvancedCreation`定向C++测试收据`.tmp/dev/logs/20261109-124459-351/result.json`，五步运行收据`.tmp/dev/logs/20261109-124656-519/result.json`，程序PID12448。三份收据均匹配同一源码身份。
- 独立运行目录 `.tmp/dev/run`；独立数据 `.tmp/dev/data/advanced-multiview-20261108`；本地端口18764。凭据只从用户指定provisioner读取并注入测试子进程，不在本文、源码或普通日志记录。
- 样本为本轮真实GPT机器人结果的副本，包含非对称红星／蓝工具及橙色背包；来源见[provenance](../../../.tmp/dev/advanced-multiview-20261108/fixtures/provenance.json)。测试和生成不改原件，不自动提交、推送、发布、切片或打印。

## 自动验证与真实调用

| 层级 | 结果 | 证据／限制 |
| --- | --- | --- |
| C++高级Session | 12/12通过 | `.tmp/dev/logs/20261108-220200-287/cpp-tests.log`；选择、双击、旧回调、草稿、布局、四图资格 |
| 高级2D离线 | 38/38通过 | [日志](../../../.tmp/dev/advanced-multiview-20261108/final-advanced-design-offline.log)；包含draft完整投影、multipart MIME、失败／恢复／幂等 |
| 高级3D离线 | 16/16通过 | [日志](../../../.tmp/dev/advanced-multiview-20261108/final-advanced-offline.log)；冻结四图、同一task恢复、旧路由拒绝新高级来源 |
| 智能旧流程离线 | 9/9通过 | [日志](../../../.tmp/dev/advanced-multiview-20261108/final-smart-offline.log)；另有真实GPT→Tripo连续流程及原历史恢复证据 |
| 旧HTTP合约 | 80/80通过 | [日志](../../../.tmp/dev/advanced-multiview-20261108/final-legacy-contract.log) |
| 架构映射 | 102锚点／29能力通过 | [结果](../../../.tmp/dev/advanced-multiview-20261108/final-architecture-map.json)，README_CURRENT |
| AI集成门禁 | 失败：4项既有基线diff预算 | [完整结果](../../../.tmp/dev/advanced-multiview-20261108/final-integration.json)；MainFrame.cpp/hpp、Plater.cpp/hpp，本轮未弱化检查，未修改这四文件 |
| 2026-11-09左栏阶段UX | 8/8实窗通过 | YOYOClaw显示“已连接”；Tripo显示与智能模式一致的“已配置”；200万联动“精细”；右栏收展及智能往返通过；未点击生成 |
| 2026-11-09相对主参考相机 | 自动与GUI通过；真实质量未通过 | Python 45／45、C++ 14／14、Windows五步运行及长标题收展／智能隔离通过；指定样本4次GPT均成功，但左右方向和头对躯干角度不满足冻结规则 |
| 2026-11-09模式与历史套图UX | 8/8实窗通过 | C++ 14／14及Windows五步运行通过；智能标题／选中态、高级套图搜索／分页／整组恢复／收展通过；真实4次GPT确认圆形加载、无中央“等待生成”及逐格成功更新 |
| 2026-11-09历史套图四项整改 | 5/5实窗通过 | Python 48／48、C++ 15／15、Windows五步运行；标题／分页、四缩略图刷新、单击加载、双击名称、Enter不切组、重启持久化及智能只读回归通过；未新增供应商调用 |
| 2026-11-09高级右栏四排布局 | 1/1实窗通过 | Windows五步运行；12组独立数据下标题／新建同行、搜索上移、首屏两列×四排、第二页、分页不重叠、单击加载、双击编辑及智能只读回归通过；未新增供应商调用 |
| 2026-11-10上传区UX | 6/6实窗通过 | Windows五步运行；等大2×2、标题同行新建、完整信息浮层、四图cover、移除／恢复、替换／恢复、2D／3D显隐、右栏两态上传区稳定及智能隔离通过；未点击生成 |
| 2026-11-10单次生成与右栏刷新解耦 | 自动、构建和实窗通过 | Python 51／51、C++ 15／15、Windows完整构建及配置化Sidecar通过；描述／风格不重建右栏，单个右视角真实操作仅1次图像调用，无视觉复核或自动二次生图，成功后只更新对应图和缩略图 |
| 2026-11-10高级3D跳转与阶段隔离 | 自动、构建、真实Tripo和实窗通过 | `AdvancedCreation` 15／15、`UiRedesign` 132／132；确认后进入既有3D等待页且不显示智能2D阶段，一个Job／一个Tripo task／一个attempt最终ready，原工作台和返回高级创作状态保留 |
| 2026-11-10同组输入与风格重新生成 | 自动、构建安装和实窗通过 | Python 75／75、C++ 17／17、GUI目标编译及最终五步刷新通过；真实组`01070a0c-f69d-4927-b362-a5d9b17f5e74`保持同一组和11组历史，右图换图后revision 12／单项待处理，风格改雕塑后revision 13／四图待处理且锁选，两个阶段3D均禁用；无GPT／Tripo生成 |

真实图像模型GPT `gpt-image-2`累计 **81次**；2D次数不限（用户最新明确授权）。其中原报告58次、旧标准elevation复现4次、相对主参考相机复验4次、加载状态实窗验收4次、方向／姿态纠偏试验9次、简化后单视角回归1次、背景合同恢复回归1次；本轮高级3D跳转验收未调用GPT。Tripo `v3.1-20260211`累计 **4次**；本轮新增1次并低于用户本轮最多3次的限制，早期总体最多20次的授权也未超过。统计包括失败及停止前已发出的创建请求，恢复下载／查询／复用候选不算新创建；文字润色和已移除的视觉复核请求不计入图像创建。原58次GPT明细见[调用账本](../../../.tmp/dev/advanced-multiview-20261108/supplier-budget.json)，后续23次以对应组／操作JSON为准。

YOYOClaw图像多参考／当前图编辑协议仍未核实，产品阻止该能力的生成，不退回单图伪装通过。用户确认YOYOClaw 3D实际使用腾讯混元，因暂无协议文档，本轮明确延期，3D只测试Tripo。

原58次图像调用包括高级57次和智能回归1次；后续三组各4次均为高级复验／实窗验收。Tripo包括高级2次和智能回归1次。高级第二个任务中断／重启后继续原task，查询、恢复及工作台本地检查没有新增供应商创建。

## 76条逐项执行结果

| 编号 | 状态 | 验证层级 | 实际结果与边界 | 证据 |
| --- | --- | --- | --- | --- |
| MV-ISO-01 | 通过 | GUI／审查 | 高级入口可点击，默认智能入口保留；高级分步显示。 | [keyboard-advanced-empty.png](../../../.tmp/dev/advanced-multiview-20261108/keyboard-advanced-empty.png) |
| MV-ISO-02 | 通过 | GUI | 从智能页 Tab 聚焦高级，Space 激活。 | [keyboard-advanced-empty.png](../../../.tmp/dev/advanced-multiview-20261108/keyboard-advanced-empty.png) |
| MV-ISO-03 | 通过 | GUI／C++ | 智能原草稿返回后保留；高级输入和独立 Session 不复制到智能。 | [smart-draft-retained.png](../../../.tmp/dev/advanced-multiview-20261108/smart-draft-retained.png) |
| MV-ISO-04 | 通过 | 真实GPT→Tripo／GUI／任务证据 | 智能原上传入口→生成3D模型→原连续生图及建模→模型完成，Job-v1／workflow_mode=smart，无高级快照或四图选择；1次GPT＋1次Tripo。 | [smart-real-generation-complete.png](../../../.tmp/dev/advanced-multiview-20261108/smart-real-generation-complete.png) |
| MV-ISO-05 | 通过 | GUI／离线 | 高级建模中切智能，再返回高级，原模型任务归属和引用保留，无新提交。 | [advanced-return-during-model.png](../../../.tmp/dev/advanced-multiview-20261108/advanced-return-during-model.png) |
| MV-ISO-06 | 通过 | 真实智能／高级历史GUI | 智能真实模型历史打开原工作台并保留原草稿；再切高级，从创作历史恢复上传正视、最新四图和177字节描述。高级组未投影为智能连续Job，3D结果仍沿共享模型库语义。 | [advanced-group-restored-after-smart-history.png](../../../.tmp/dev/advanced-multiview-20261108/advanced-group-restored-after-smart-history.png) |
| MV-ISO-07 | 通过 | diff审查 | 智能业务没有变更；共享接缝只增加高级类型守卫与独立宿主，旧路径默认值不变。条件性停止规则未触发。 | [final-smart-offline.log](../../../.tmp/dev/advanced-multiview-20261108/final-smart-offline.log) |
| MV-UPL-01 | 通过 | GUI | 标题、正视大槽及左后右小槽、必填标识均可见。 | [four-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-four-view-success.png) |
| MV-UPL-02 | 未执行 | 局部GUI／工具限制 | 点击选择四槽及对应方向已验；尝试文件管理器拖入时，Computer Use限制拖动终点必须位于源窗口内，跨窗拖入未完成，不能写为通过。 | [four-reference-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-confirmation.png) |
| MV-UPL-03 | 通过 | GUI／哈希 | 单槽替换、移除辅助图不会改变其他槽；原图副本及历史保持。 | [two-reference-right-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/two-reference-right-confirmation.png) |
| MV-UPL-04 | 通过 | GUI／离线 | 仅辅助图时明确缺少主视，生成禁用。 | [side-without-front-blocked.png](../../../.tmp/dev/advanced-multiview-20261108/side-without-front-blocked.png) |
| MV-UPL-05 | 通过 | 真实GPT／GUI | 无图有效文字可确认并生成固定四图。 | [initial-four-view-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/initial-four-view-confirmation.png) |
| MV-UPL-06 | 通过 | GUI／离线 | 空输入禁用，辅助图缺主视不绕过校验。 | [keyboard-advanced-empty.png](../../../.tmp/dev/advanced-multiview-20261108/keyboard-advanced-empty.png) |
| MV-UPL-07 | 通过 | 真实GPT／GUI | 仅正视WebP且描述为空，首次四图成功。 | [one-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/one-reference-four-view-success.png) |
| MV-UPL-08 | 通过 | 真实GPT／GUI | PNG、JPEG和静态透明WebP均预览／生成可用；20,709,023字节、3000×2300合法PNG被接受，未为大小边界额外生图。 | [near-limit-valid-png.png](../../../.tmp/dev/advanced-multiview-20261108/near-limit-valid-png.png) |
| MV-UPL-09 | 通过 | GUI／离线 | 取消文件选择以及损坏PNG、超限、动画WebP、不支持GIF和32×32小图替换均被拒绝；已有有效正视、描述及四图保持，未提交生成。 | [unsupported-gif-rejected-preserves-input.png](../../../.tmp/dev/advanced-multiview-20261108/unsupported-gif-rejected-preserves-input.png) |
| MV-GEN-01 | 通过 | 真实GPT／GUI／哈希 | 保留旧方向失败；相同1600×1024透明正视WebP重验，辅助槽为空，四图完成，左右朝向相反且非对称附件可辨，无自动3D。 | [single-webp-direction-retest-success.png](../../../.tmp/dev/advanced-multiview-20261108/single-webp-direction-retest-success.png) |
| MV-GEN-02 | 通过 | 真实GPT／GUI／操作收据 | 正+左、正+后、正+右三种组合均生成四个独立视角；上传哈希及四图版本逐组登记。 | [two-reference-back-success.png](../../../.tmp/dev/advanced-multiview-20261108/two-reference-back-success.png) |
| MV-GEN-03 | 通过 | 真实GPT／GUI／操作收据 | 正+左后、正+左右、正+后右三种组合均完成四视角，缺少的辅助方向被补齐。 | [three-reference-back-right-success.png](../../../.tmp/dev/advanced-multiview-20261108/three-reference-back-right-success.png) |
| MV-GEN-04 | 通过 | 真实GPT／GUI | 四个具名上传参考共同约束同一机器人，输出四个独立方向，非四个对象。 | [four-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-four-view-success.png) |
| MV-GEN-05 | 通过 | 真实GPT／GUI | 纯文字机器人最终形成正左后右独立图，支持相同编辑与Tripo建模。首次3张MIME失败及补齐证据保留。 | [four-views-expanded.png](../../../.tmp/dev/advanced-multiview-20261108/four-views-expanded.png) |
| MV-GEN-06 | 通过 | GUI／操作收据 | 首次四图确认可取消；取消无操作任务、无供应商新调用，草稿保留。 | [single-image-confirmation-cancel.png](../../../.tmp/dev/advanced-multiview-20261108/single-image-confirmation-cancel.png) |
| MV-GEN-07 | 通过 | 真实GPT／GUI | 成功图逐张更新到具名固定格，其他保持占位，最终停留2D。 | [three-reference-partial-expanded.png](../../../.tmp/dev/advanced-multiview-20261108/three-reference-partial-expanded.png) |
| MV-GEN-08 | 通过 | 真实GPT／视觉 | 加强提示后的双参考、三参考及四参考各产出四份独立完整图片，左右方向和非对称附件可辨；旧单正视方向失败另记GEN-01。 | [four-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-four-view-success.png) |
| MV-LAY-01 | 通过 | GUI／视觉 | 四张等大2×2，主体contain完整、间距一致；用户最终美观接受仍待反馈。 | [four-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-four-view-success.png) |
| MV-LAY-02 | 通过 | GUI／C++ | 右栏收起释放宽度，卡片扩大；图片在高度约束下保持比例。 | [drawer-collapsed.png](../../../.tmp/dev/advanced-multiview-20261108/drawer-collapsed.png) |
| MV-LAY-03 | 通过 | GUI | 展开后按剩余空间缩放，无遮挡和越界。 | [layout-1320x840-expanded-partial.png](../../../.tmp/dev/advanced-multiview-20261108/layout-1320x840-expanded-partial.png) |
| MV-LAY-04 | 通过 | GUI／C++ | 已有左后选择及描述时收展保持选择、范围、描述和结果；无新生成。 | [back-edit-success-collapsed.png](../../../.tmp/dev/advanced-multiview-20261108/back-edit-success-collapsed.png) |
| MV-LAY-05 | 通过 | GUI／C++ | 在1544×981、1560×992及1320×840逻辑窗口检查；较小窗口展开／收起均2×2、无变形，输入栏可滚动，固定动作可达。 | [layout-1320x840-collapsed-partial.png](../../../.tmp/dev/advanced-multiview-20261108/layout-1320x840-collapsed-partial.png) |
| MV-LAY-06 | 未执行 | 单条件GUI | 本机只取得192 DPI／200%缩放；其他DPI、跨屏未验证，不外推。 | [runtime-identity.json](../../../.tmp/dev/advanced-multiview-20261108/runtime-identity.json) |
| MV-LAY-07 | 通过 | GUI／真实结果／C++ | 已检查1199×1312与1086×1448两种实际结果宽高比：等大四格和双击大图均contain显示完整主体，未拉伸裁切；此结论限已验比例，不外推极端宽高比。 | [portrait-ratio-popup.png](../../../.tmp/dev/advanced-multiview-20261108/portrait-ratio-popup.png) |
| MV-LAY-08 | 通过 | 真实GPT／本地输入故障／GUI | 首次正视成功其余失败，与局部左成功后失败保留旧图均经实窗收展右栏检查；四格尺寸、主体与状态稳定。故障为测试副本短暂不可读，原字节已恢复。 | [partial-edit-stable-collapsed.png](../../../.tmp/dev/advanced-multiview-20261108/partial-edit-stable-collapsed.png) |
| MV-SEL-01 | 通过 | GUI／C++ | 后视单击选中、再单击取消，黄色框勾对应切换。 | [back-edit-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/back-edit-confirmation.png) |
| MV-SEL-02 | 通过 | GUI／C++ | 无需Ctrl累积左后多选，单项取消不清空其他。 | [multi-selection-left-back.png](../../../.tmp/dev/advanced-multiview-20261108/multi-selection-left-back.png) |
| MV-SEL-03 | 通过 | GUI／C++ | 按右后左正倒序全选四图，全部标签归并；逐项取消至无选中，范围再次显示全部，无新生成。 | [all-deselected-default-all.png](../../../.tmp/dev/advanced-multiview-20261108/all-deselected-default-all.png) |
| MV-SEL-04 | 通过 | GUI／视觉 | 现有黄色细框与角部勾选，悬停无选中标识；原色及卡片位置保持。 | [multi-selection-left-back.png](../../../.tmp/dev/advanced-multiview-20261108/multi-selection-left-back.png) |
| MV-SEL-05 | 通过 | GUI／C++ | 双击未选图片只查看，关闭后保留原先其他选择。 | [selected-popup-preserved.png](../../../.tmp/dev/advanced-multiview-20261108/selected-popup-preserved.png) |
| MV-SEL-06 | 通过 | GUI／C++ | 双击已选图片保留原选择及其他选择。 | [selected-popup-preserved.png](../../../.tmp/dev/advanced-multiview-20261108/selected-popup-preserved.png) |
| MV-SEL-07 | 通过 | GUI／C++ | 当前系统双击条件下多次单击／双击验证，无误生成、重复弹窗；快速跨图点击另有定向回归。 | [selected-popup-preserved.png](../../../.tmp/dev/advanced-multiview-20261108/selected-popup-preserved.png) |
| MV-SEL-08 | 通过 | GUI／视觉 | 大图主体完整、方向可辨，修复后的关闭按钮可见可点；返回状态保留。 | [large-popup-close-visible.png](../../../.tmp/dev/advanced-multiview-20261108/large-popup-close-visible.png) |
| MV-SEL-09 | 通过 | GUI／离线 | 缺图占位不打开大图、不被选为可编辑结果；单项补齐可恢复。 | [back-retry-success.png](../../../.tmp/dev/advanced-multiview-20261108/back-retry-success.png) |
| MV-TAG-01 | 通过 | GUI | 空选时框内开头“全部视角”与按钮下“全部视角设计图生成”同步。 | [two-reference-four-view-success.png](../../../.tmp/dev/advanced-multiview-20261108/two-reference-four-view-success.png) |
| MV-TAG-02 | 通过 | GUI | 后视单选及左右多选，中央框、标签、按钮范围同步。 | [left-right-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/left-right-confirmation.png) |
| MV-TAG-03 | 通过 | GUI／C++ | 按右后左正倒序选择时标签仍按正左后右固定顺序排列，部分集合与先前正序一致，全选四图归并全部。 | [all-selected-fixed-order.png](../../../.tmp/dev/advanced-multiview-20261108/all-selected-fixed-order.png) |
| MV-TAG-04 | 通过 | GUI／持久化 | 289字节中文多行正文连续选择／取消后保持；滚动位置、末尾内容与标签分离，自动保存不清空四图。 | [multiline-tags-counter.png](../../../.tmp/dev/advanced-multiview-20261108/multiline-tags-counter.png) |
| MV-TAG-05 | 通过 | GUI | 2000／2001 ASCII字节边界：标签不计入，超限红提示并禁用，删除一字恢复；沿用既有字节限制。 | [prompt-over-limit.png](../../../.tmp/dev/advanced-multiview-20261108/prompt-over-limit.png) |
| MV-TAG-06 | 通过 | 真实润色／GUI | 透明WebP正视参考可润色；澄清反馈可见，取消保留78字节原文；接受替换为177字节正文，左后标签与选择和四图保持。 | [polish-accepted-selection-preserved.png](../../../.tmp/dev/advanced-multiview-20261108/polish-accepted-selection-preserved.png) |
| MV-TAG-07 | 通过 | GUI | 1320×840及1560×992、200%下两标签／多行正文，收展右栏和窗口变化均保留正文、滚动及字数；无标签覆盖。 | [multiline-tags-maximized.png](../../../.tmp/dev/advanced-multiview-20261108/multiline-tags-maximized.png) |
| MV-TAG-08 | 通过 | GUI／C++ | 双击未选图关闭后，标签和按钮范围仍属于原选中集合。 | [selected-popup-preserved.png](../../../.tmp/dev/advanced-multiview-20261108/selected-popup-preserved.png) |
| MV-EDIT-01 | 通过 | 真实GPT／哈希／GUI | 只修改后视，其他三图引用和哈希保持。 | [first-back-edit-version-proof.json](../../../.tmp/dev/advanced-multiview-20261108/first-back-edit-version-proof.json) |
| MV-EDIT-02 | 通过 | 真实GPT／GUI | 左／右编辑只处理两个目标，正／后不变；右图停止后保留旧图，明确重试复用候选。 | [after-left-right-edit-group.json](../../../.tmp/dev/advanced-multiview-20261108/after-left-right-edit-group.json) |
| MV-EDIT-03 | 通过 | 真实GPT／哈希／GUI | 无选中全部编辑4调用，各视角基于自己的当前图。 | [all-edit-proof.json](../../../.tmp/dev/advanced-multiview-20261108/all-edit-proof.json) |
| MV-EDIT-04 | 通过 | 真实GPT／哈希 | 后视第二次编辑使用第一次成功版本，其他不变。 | [second-back-edit-version-proof.json](../../../.tmp/dev/advanced-multiview-20261108/second-back-edit-version-proof.json) |
| MV-EDIT-05 | 通过 | 真实GPT／GUI／哈希 | 仅上传正视WebP，后视无原图仍以当前后视设计图修改成功；原件和其他三图不变。 | [uploaded-group-edit-proof.json](../../../.tmp/dev/advanced-multiview-20261108/uploaded-group-edit-proof.json) |
| MV-EDIT-06 | 通过 | 真实GPT／本地输入故障／GUI／哈希 | 左后两目标中左真实成功；后因冻结旧左参考短暂不可读而失败，当前后旧图保持可读，正右版本不变，左后选择和格位不变；未伪称供应商报错。 | [real-partial-edit-proof.json](../../../.tmp/dev/advanced-multiview-20261108/real-partial-edit-proof.json) |
| MV-EDIT-07 | 通过 | 真实GPT／GUI确认／哈希 | 失败后视单项确认后仅调用GPT一次；后视更新成功，左视成功新图与正右版本和哈希保持，无隐式批量重生。 | [partial-back-only-retry-proof.json](../../../.tmp/dev/advanced-multiview-20261108/partial-back-only-retry-proof.json) |
| MV-EDIT-08 | 通过 | 真实GPT／GUI／离线 | 首次三图MIME失败逐项修复补齐，未重复正视；未齐备时3D禁用。冻结输入／锚点回归已通过。 | [back-retry-success.png](../../../.tmp/dev/advanced-multiview-20261108/back-retry-success.png) |
| MV-MODEL-01 | 通过 | GUI／可逆文件故障／哈希 | 逐一暂移测试组正左后右当前图，重开历史后对应卡片为空，3D禁用；点击不新建任务。四图均逐份恢复原哈希，未改历史记录。 | [missing-right-blocked.png](../../../.tmp/dev/advanced-multiview-20261108/missing-right-blocked.png) |
| MV-MODEL-02 | 通过 | 真实Tripo／GUI | 取消未提交；显式确认后一个逻辑Job、一个远端task，四份冻结输入。 | [tripo-confirmation-cancel.png](../../../.tmp/dev/advanced-multiview-20261108/tripo-confirmation-cancel.png) |
| MV-MODEL-03 | 通过 | 真实Tripo／GUI／哈希 | 仅后视选中仍确认并提交正左后右四个具名冻结输入，哈希逐项匹配且互异；最终ready，原task仅一次创建。 | [tripo-recovery-final-proof.json](../../../.tmp/dev/advanced-multiview-20261108/tripo-recovery-final-proof.json) |
| MV-MODEL-04 | 通过 | 真实结果／GUI确认／哈希 | 局部编辑结束后，新左和保留旧后及正右四图可进入原Tripo确认；四个当前文件哈希有效，无失败新版本混入。取消确认未新增3D调用。 | [partial-edit-stable-model-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/partial-edit-stable-model-confirmation.png) |
| MV-MODEL-05 | 通过 | 真实Tripo／GUI／任务收据 | 四个具名冻结输入哈希互异且匹配，一个远端task和一个机器人模型；质量review及薄壁／悬垂警告保留，未证明相对单图质量改善。 | [tripo-workbench-back.png](../../../.tmp/dev/advanced-multiview-20261108/tripo-workbench-back.png) |
| MV-MODEL-06 | 通过 | GUI负向保护／离线版本守卫 | 四图有效时选择YoYoclaw，建模禁用且点击不提交、不换供应商；恢复Tripo即恢复资格。确认版本变化守卫有既有定向离线覆盖；此前真实sidecar中断未新增task。仅保护用例通过，YoYoclaw实际建模能力仍未知且按用户决定延期。 | [unsupported-model-provider-blocked.png](../../../.tmp/dev/advanced-multiview-20261108/unsupported-model-provider-blocked.png) |
| MV-MODEL-07 | 通过 | 真实模型／GUI | 模型经原工作台配色保护、显式映射及显式导入普通工作台；无自动切片／打印，原件保留。未执行实物打印。 | [advanced-import-no-autoslice.png](../../../.tmp/dev/advanced-multiview-20261108/advanced-import-no-autoslice.png) |
| MV-HIS-01 | 通过 | GUI／持久化 | 一组恢复上传图、最新四图和正文，四视角正确关联。 | [uploaded-history-restart.png](../../../.tmp/dev/advanced-multiview-20261108/uploaded-history-restart.png) |
| MV-HIS-02 | 通过 | GUI／哈希 | 仅正视上传，空辅助槽恢复为空；最新后视编辑及其他当前图保留。 | [uploaded-group-edit-proof.json](../../../.tmp/dev/advanced-multiview-20261108/uploaded-group-edit-proof.json) |
| MV-HIS-03 | 通过 | 真实GPT／本地输入故障／历史GUI | 分别切离再从历史打开首次部分失败组及局部失败组：缺图／新左／保留旧后、重试入口和描述真实；首次3D禁用，四可用图组可确认建模。 | [partial-edit-history-restored.png](../../../.tmp/dev/advanced-multiview-20261108/partial-edit-history-restored.png) |
| MV-HIS-04 | 通过 | GUI／操作收据 | 正常关闭重启后打开上传组，原件、四图及描述恢复；无新图像／模型调用。 | [uploaded-history-restart.png](../../../.tmp/dev/advanced-multiview-20261108/uploaded-history-restart.png) |
| MV-HIS-05 | 通过 | 真实旧历史／GUI | 旧v1单图历史打开原图／设计图及原3D设置，返回多视角入口可见可点；未补四图、未调用供应商，原件未改。旧素材涉及私人照片，仅本地查看。 | [legacy-history-copy-proof.json](../../../.tmp/dev/advanced-multiview-20261108/legacy-history-copy-proof.json) |
| MV-HIS-06 | 通过 | GUI／C++ | 高级组历史恢复后返回智能，智能草稿保留，无高级标签与图污染。 | [smart-draft-retained.png](../../../.tmp/dev/advanced-multiview-20261108/smart-draft-retained.png) |
| MV-ERR-01 | 通过 | GUI／Python | 重复点击准备只得到一个确认／操作；提交收据持久、重复确认只排队一次的离线回归通过。 | [four-reference-confirmation.png](../../../.tmp/dev/advanced-multiview-20261108/four-reference-confirmation.png) |
| MV-ERR-02 | 通过 | 真实GPT／GUI | 左先成功、右进行中停止，成功和旧图保留；晚到候选不自动发布；明确重试复用，无额外付费调用。 | [stopped-preserves-result.png](../../../.tmp/dev/advanced-multiview-20261108/stopped-preserves-result.png) |
| MV-ERR-03 | 通过 | 真实服务中断／GUI／任务收据 | 远端task持久化后中断sidecar，原图／四图／任务引用保留；正常重启应用再显式恢复，同一task最终ready且仅一次创建。服务单独重启不能自动恢复认证，需应用重启，此限制保留。 | [tripo-recovery-final-proof.json](../../../.tmp/dev/advanced-multiview-20261108/tripo-recovery-final-proof.json) |
| MV-ERR-04 | 通过 | 真实GPT／GUI／C++ | 三参考组右图仍生成时切到四参考历史；旧任务完成不覆盖当前四参考组，重开旧组取得自己的结果。 | [late-result-other-group-after.png](../../../.tmp/dev/advanced-multiview-20261108/late-result-other-group-after.png) |
| MV-ERR-05 | 通过 | GUI／C++／Python | 四图已有时编辑忙碌，3D动作禁用；Session和服务再次校验busy／版本。 | [partial-update-before-stop.png](../../../.tmp/dev/advanced-multiview-20261108/partial-update-before-stop.png) |
| MV-ERR-06 | 通过 | AI离线／原生GUI | sidecar停止期间打开普通3MF工程副本，手动层高0.2→0.24再恢复0.2；未自动切片／打印。保存仅作用于测试副本，原件保留；不外推全部Orca功能。 | [offline-native-original-restored.png](../../../.tmp/dev/advanced-multiview-20261108/offline-native-original-restored.png) |

## 缺陷修复与保留问题

| 问题 | 处理与验证边界 |
| --- | --- |
| 首次参考请求3张image_rejected | 修复高级multipart MIME与扩展名顺序；新增真实multipart字段回归，真实逐项补齐与后续多参考生成成功。 |
| 高级空POST被服务拒绝 | stop／recover／reconcile使用JSON对象；真实原Tripo任务恢复成功且无新task。 |
| 首次编辑描述崩溃 | 高级JSON PUT改为Http::put2；共享Http及智能客户端保持。WER保留，后续多次真实编辑及重启恢复未复现。 |
| 自动保存短暂清空四图 | draft返回runtime完整快照而非原始group，新增回归；最终实窗复验状态在对应TAG项记录。 |
| WebP润色不接受及空结果静默 | 高级本地解码、缩至最长边≤2048并转临时PNG，异步结束释放；空优化结果显示warnings／补充描述提示。最终实窗结果见TAG-06。 |
| 侧栏滚动、禁用按钮刷新、drawer箭头、大图关闭尺寸 | 已修复并在真实主窗口观察；较小窗口固定生成动作可达。 |
| 左右方向相同 | 保留旧单正视失败结果，加强高级专属正交视角／画布方向／非对称附件约束；不得自动镜像图伪造正确结果。各条件复验见GEN项。 |
| 空闲点击偶尔被轮询挡住 | 高级宿主定时只轮询进行中图像／模型任务；显式打开／恢复仍主动刷新。最终构建后复验记录在对应GUI证据。 |
| 旧单图返回入口不可见 | 高级专用legacy_back补最小宽度；最新匹配Windows运行树中按钮完整可见可点击，返回多视角未生成新图。 |
| 服务单独重启后认证失效 | 旧客户端nonce失效，未改共享认证或智能业务。正常重启应用后显式恢复原Tripo任务成功；需要应用重启的限制保留，不称为无缝重连。 |

## 真实任务与质量结论

- 主文字组 `9f0a36ec-26f9-4206-bc0e-cfa29762b309`；后视连续两次编辑、左右多选停止／候选复用、全选四图编辑均保留引用与哈希证据。
- 单正视WebP组 `9edc52cd-c0a2-49e8-bb7c-cb25fafdfdc3`；后视编辑 `9c6a9065-13f4-430a-9ad2-3a974b16c8e8` 仅后视变化，其余和上传原件哈希一致；此组旧左右方向问题仍保留。
- 初次Tripo Job `d89a8cef-78b9-493a-b64f-931ddbe693ce`，task `14a4b687-a539-47d3-84b4-3fb3edbb4774`；四个具名输入冻结且互异，原task恢复不新建。
- 第二次高级Tripo Job `c92bd493-7cf0-4148-b156-47edf7238ddc`，task `d0fd6435-7eaa-430b-a4a8-b9326f4b8e72`；仅选后视仍提交完整四图。服务中断后应用重启并显式恢复，最终ready且一个attempt，见[恢复证据](../../../.tmp/dev/advanced-multiview-20261108/tripo-recovery-final-proof.json)。
- 智能原连续流程Job `a1ec5064-3fcb-4f18-bba8-48656c035883`，task `3ac56b50-c951-4158-bb81-e44ff88675a6`；原入口生图→建模→可见结果／原模型历史，Job-v1且无高级快照，见[最终身份](../../../.tmp/dev/advanced-multiview-20261108/smart-real-final-proof.json)。随后原工作台本地检查／修复完成，原始模型保留。
- 机器人3D已从正侧背观察并显式导入；模型诊断为review，含薄壁／悬垂警告。四图接入成功不等于可打印性通过；没有同条件单图对照、实物打印或泛化质量提升结论。
- 首次上传组合、操作ID与四图版本见[组合证据](../../../.tmp/dev/advanced-multiview-20261108/initial-combination-evidence.json)。全部原图、候选、失败收据和真实任务保存在独立测试数据目录。
- 首次部分失败组 `341d50f8-11b8-43c1-bd3b-de5016de2237`：真实GPT正视成功后，测试上传参考短暂不可读，其余三向失败；仅调用1次。原字节已恢复，保留缺图现场及历史恢复，见[首次部分失败证据](../../../.tmp/dev/advanced-multiview-20261108/real-initial-partial-proof.json)。
- 局部修改组 `b9ed67b3-344d-4984-b16c-dfcc87b200df`：操作 `3ccf00ba-7f25-4460-953a-4b9fe1382cab` 左视真实成功，后视因冻结旧左参考短暂不可读而失败；新左／旧后可用，正右不变。故障发生在本地参考准备阶段，不称为供应商失败；没有mock或改写任务状态，见[局部失败证据](../../../.tmp/dev/advanced-multiview-20261108/real-partial-edit-proof.json)。两种右栏状态及历史重开均已实窗检查。
- 上述后视经明确单项确认重试，操作 `c32453b4-a956-40cd-8a59-1d139922bb70` 仅一次GPT调用成功，其他三图版本和哈希不变，见[单项重试证据](../../../.tmp/dev/advanced-multiview-20261108/partial-back-only-retry-proof.json)。部分失败后进入Tripo确认并取消，未新增3D调用。
- 逐一暂移测试组当前正／左／后／右文件，分别重开历史检查对应空格及3D禁用；所有原文件均已按原哈希恢复：[正视](../../../.tmp/dev/advanced-multiview-20261108/missing-front-proof.json)、[左视](../../../.tmp/dev/advanced-multiview-20261108/missing-left-proof.json)、[后视](../../../.tmp/dev/advanced-multiview-20261108/missing-back-proof.json)、[右视](../../../.tmp/dev/advanced-multiview-20261108/missing-right-proof.json)。没有篡改历史元数据或生成结果。
- 另一组 `fb5dee02-057f-4e78-af26-7a50b3a80635` 的真实GPT请求收到 `image_service_unavailable`，状态仍为 `recovery_required`／`ambiguous`；旧四图及原字节保持，未当作未提交直接重发，见[原始不确定结果](../../../.tmp/dev/advanced-multiview-20261108/partial-second-ambiguous-proof.json)。保留真实失败及待对账状态，不以不限2D额度解除重复提交保护。

## 未结与交付边界

原76条中74条通过、2条未完整执行；YOYOClaw能力缺口、用户最终视觉接受、真实GPT不确定请求及既有集成门禁失败均保留，不能将本报告称为全部验收通过。MODEL-06通过仅指不支持供应商／版本守卫的负向保护，YOYOClaw实际多视角建模仍未知且按用户决定延期，不计入建模功能通过。只维护原76条，不改变需求以消除失败。未执行安装包标准用户Program Files写入、跨平台、远程CI或实物打印；本次为Windows本地内部开发验证。

剩余原用例为MV-UPL-02和MV-LAY-06，用户已于2026-11-09明确决定保留未验并先交付当前报告。前者点击上传已验，但跨窗口拖入受Computer Use拖动终点边界限制；后者仅取得192 DPI／200%，其他真实缩放未验。本轮不再等待补验答复，不改系统缩放或追加生成；两条仍保留在原用例中，日后需要时沿原编号补验。局部成功／失败、单项重试、缺图资格及对应历史条件已经补验。

后续补验方法（保留为恢复参考，本轮不执行）：可从 `.tmp/dev/advanced-multiview-20261108/fixtures` 将 `front.png`、`left.png`、`back.png`、`right.png` 分别拖到正／左／后／右槽，检查对应预览、一槽一图、总计四图及没有隐式生成。MV-LAY-06须在另一真实Windows缩放比例下打开已有四图组，记录缩放及窗口尺寸，检查右栏收展、黄色选择／文字及双击大图；仅缩放截图或改变窗口尺寸不能代替。原b9ed67b3组四图及故障／恢复组均保留。

维护文档已同步：Docs/AI_ENGINEERING.md补高级入口及状态所有者；Docs/architecture/PLATFORM_ARCHITECTURE.md和review-map.json维护隔离边界与实际锚点；tools/ai/README_atomic_capabilities.md维护路由、Job-v2、恢复和供应商限制。文档同步worktree检查为advisory范围提示，errors=0，不等同于PR声明通过；本轮未创建PR。
