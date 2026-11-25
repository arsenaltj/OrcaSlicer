# 高级模式2D描述与预览：详细开发方案

版本：V1.0。日期：2026-11-24。需求：`ADV-2D-01`～`06`。基线：`codex/team/integration`／`10b47141bb5521cb9faa56a48d197f79d8770815`。

输入：[已确认需求](2026-11-24-advanced-2d-description-preview-requirements.md)、[架构设计](2026-11-24-advanced-2d-description-preview-architecture.md)。验证：[112条测试用例](2026-11-24-advanced-2d-description-preview-test-cases.md)。本文规定本次实施的接口、字段和步骤；用户随后明确授权严格按本文实施及完成验收。实际进度、源码／产物身份及分层结果仅维护于主计划，设计条目本身不表示验收通过。

## 1. 交付边界及需求追踪

| 需求 | 实施结果 | 用例组 |
| --- | --- | --- |
| ADV-2D-01 | 移除左栏描述框内的视角标签，左栏作用范围取当前输入集合 | A2D-ALL、A2D-UX |
| ADV-2D-02 | 无图有效文字四图；有图只生成有对应原图的方向；清空全部后显式回文字流程 | A2D-ALL、A2D-UP |
| ADV-2D-03 | 点击／拖入统一主图门禁；删除／替换立即隐藏；缺主图时保留辅助并约束操作 | A2D-UP、A2D-SAFE |
| ADV-2D-04 | 2D不选择；首次3D选择保持；已有模型的原确认窗口承接选图，实际冻结与确认一致 | A2D-MODEL、A2D-ROUTE、A2D-SAFE |
| ADV-2D-05 | 固定四格／单大图、底部切换及缩略图导航、悬停眼睛和名称、明确预览记忆边界 | A2D-PRE、A2D-UX |
| ADV-2D-06 | 两种模式左下星芒、精简浮层、独立局部文字、严格本方向直接提交、记录与恢复 | A2D-LOC、A2D-SAFE、A2D-UX |
| 保留规则 | 一次性全目标成功导航；两家单多图；忙碌／只读／未决／停止／过期／历史及点云 | A2D-ALL、A2D-MODEL、A2D-ROUTE、A2D-SAFE |

不新增设计稿未圈工具、推荐语、图片直接建模、下载或右侧模型库，不改智能模式、3D主页面及原点选延迟。旧“回2D改选下次建模图”的规则由A替代；旧“局部发送后再次确认”由用户最新直接提交决定替代。左栏原2D确认和3D原确认保留。

## 2. 文件与函数修改清单

| 文件／既有接缝 | 拟修改内容 | 不扩张的边界 |
| --- | --- | --- |
| [AdvancedCreationTypes.hpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationTypes.hpp) | 高级专用设计意图、候选选择及其身份投影；沿ViewId四方向 | 不改公共AI Contracts、Job格式或相机方向身份 |
| [AdvancedCreationSession.hpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationSession.hpp)／[cpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationSession.cpp) | 全范围投影、上传变更资格、单图资格、3D候选事务与修订；保留原model偏好／seen／invalidated／guidance | 不在UI复制可用性；恢复缺主图中间态仍合法 |
| [AdvancedCreationFeatureHost.hpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationFeatureHost.hpp)／[cpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedCreationFeatureHost.cpp) | 分开全局确认和局部直提；新语义回显检查；候选计划异步准备、最终确认与原回执提交 | 不共用智能请求句柄；不新增自动付费重试 |
| [AdvancedMultiviewClient.cpp](../../../src/slic3r/GUI/AI/ModelGeneration/AdvancedMultiviewClient.cpp)及hpp | 序列化高级上下文、解析必要投影，延续身份与缓存验证 | HTTP只适配，不在Client重写目标规则 |
| [AdvancedCreationWorkspace.cpp](../../../src/slic3r/GUI/Redesign/AdvancedCreationWorkspace.cpp)及hpp（如需要） | 固定标签、2D事件隔离、两种预览、导航、星芒和浮层；高级方案A确认窗口适配 | 不改公共确认窗口在智能模式的行为 |
| [advanced_multiview_contracts.py](../../../tools/ai/advanced_multiview_contracts.py) | 解析高级设计上下文、合法范围与局部文字；保留design_targets作为目标边界 | 公共MAX_PROMPT_BYTES、style及相机契约不改 |
| [advanced_multiview_application.py](../../../tools/ai/advanced_multiview_application.py) | prepare区分all_inputs／single_view／legacy；execute隔离局部与全局基线；重试继承明确范围 | 原revision、fingerprint、能力、未决、停止与幂等保护不弱化 |
| [advanced_multiview_repository.py](../../../tools/ai/advanced_multiview_repository.py) | 输入变更主图约束的持久侧校验；保存／读取高级上下文，保留旧版本 | 不迁移／删除原件，不将缺主图草稿变成不可保存 |
| [advanced_multiview_design_workflow.py](../../../tools/ai/advanced_multiview_design_workflow.py) | DesignImageRequest增加可选局部字段；按目标模式准备参考；版本关联本次operation | 只发布冻结目标；其它方向pending和current不变 |
| [advanced_multiview_image_gateway.py](../../../tools/ai/advanced_multiview_image_gateway.py) | 仅高级提示词组合局部与全局优先级；保持有基底时的方向、身份及未要求细节 | 不变更GPT／YOYOClaw接口、参数、白名单或能力 |
| [advanced_multiview_models.py](../../../tools/ai/advanced_multiview_models.py)及model_contracts／model_workflow | 优先复用原selected_views／visible_versions协议；补A一致性回归；仅发现直接关联缺口才做必要校验 | 不预设供应商改造，不升级既有v1／v2冻结快照 |
| [RedesignMessageDialog.hpp](../../../src/slic3r/GUI/Redesign/RedesignMessageDialog.hpp)／[cpp](../../../src/slic3r/GUI/Redesign/RedesignMessageDialog.cpp) | 为方案A提供可选确认内容及最终确认guard；未设置时保持原行为 | 不改变普通／智能窗口的回调、按钮或确认语义 |
| `resources/images/figma-ux/` | 接入已核验且本需求使用的星芒／关闭／发送／模式／眼睛SVG；命名沿原资源规范 | 不使用整页截图、示例模型图或未圈素材 |
| 定向C++／Python测试及其既有构建登记 | 增加业务断言与异步场景，更新被新规则替代的旧断言 | 不删除旧保护测试，不把源码文本扫描当行为验证 |
| 维护文档 | 需求／三份设计用例、README索引；实施时同步平台映射、原子能力及导航 | 当前仅设计，不把拟实施状态写成已完成事实 |

`advanced_multiview_transport.py`现有JSON plans路由已把请求传给Application，不预设新增端点或multipart字段。公共契约、侧车全局runtime版本、供应商协议如确需变化，须先报告影响。

## 3. Session与命令接口设计

以下名称为拟新增，可在既有命名规范内调整，责任与校验不能变化。

| 接缝 | 输入／输出及约束 |
| --- | --- |
| `all_design_views()` | 从当前InputDraft投影T，返回规范顺序；不读取m_selected、model_selected、PreviewState；生成前必须通过input_rejection |
| `upload_change_rejection(view, replacement)` | 无主图时拒绝辅助新增／替换；删除允许；补主图允许；不替代图片格式及编辑锁校验 |
| `single_design_eligibility(view, local_text)` | 复用编辑锁、忙碌、未决、输入上限、图片方向域、可见基底／失败必要输入；不因其它方向普通pending强制扩图 |
| `begin_model_selection()` | 建立只在本次A窗口存活的候选；复制已有偏好、强制front；记录上下文和基准版本 |
| `toggle_candidate_view(view)` | front不可取消；可用辅助可切换；已选失效辅助允许取消；不可用未选方向不能加入 |
| `candidate_model_eligibility()` | 对候选而非旧偏好检查建模集；全局未决、草稿不匹配等保护仍生效 |
| `commit_model_selection(candidate_id, revision)` | 最终确认且快照／计划匹配才写入P3、增加选图修订并保存LocalDraft；取消不调用 |
| `cancel_model_selection()` | 丢弃候选，晚到准备回调不能恢复它 |

现有`effective_views()`不能继续供新左栏生成使用。旧`SelectionContext::DesignForNextModel`不再由2D卡片触发；只保留兼容所需的读取或旧测试夹具，不保留隐藏业务入口。原3D卡片单／双击处理和偏好初始化沿用，不借本次更改其点击延时。

### 3.1 主图上传校验的三层落点

1. Workspace的`choose`在打开辅助文件选择前检查；`accept`同时承接点击结果和ImageDrop，接受时再次检查。
2. Session提供统一领域原因，防止点击与拖入规则分叉；恢复旧草稿使用专用恢复路径，不被误当成新增上传。
3. Application／Repository在写入输入前比较旧关系、内容摘要及`replaced_views`：最终缺front时，仅允许保留未变辅助或删除，不允许新增／替换辅助；同内容显式替换也算替换。先校验再落盘，不产生半更新。

无主图的旧草稿、删除主图的中间态继续可读可保存。删除主图不级联删除辅助图；仅相应方向current清空。重新上传主图后恢复合法操作，不将旧失效图回填。

### 3.2 全局描述、润色和风格的消费者

左栏文字、风格和现有润色结果都只更新全局DesignDraft，最终生成始终作用于T，不读取Q或P3。现有`polish_prompt`本就使用全局文字与可选主原图作参考，继续保留该输入和“使用润色后的描述？”确认；本次不扩成新的多参考润色接口，不给局部浮层增加润色工具。供应商／风格等原控件和现有字节计数仅按上述全局语义联动，不能因删除动态标签丢失原校验或改变其它布局。

## 4. 2D全范围与严格单图计划

### 4.1 请求和计划字段

现有`command_id`、`kind`、`expected_group_revision`、全局draft字段及`base_versions`继续使用。新字段`design_request`：

| 字段 | 类型／默认 | 校验／用途 |
| --- | --- | --- |
| `version` | 整数1 | 新命令必须明确；未知版本拒绝 |
| `scope` | `all_inputs`／`single_view` | 缺整个上下文仅用于旧记录／旧调用legacy兼容 |
| `target_view` | 单图时必填ViewId字符串 | 与唯一target_views一致，属于当前T |
| `local_prompt` | 单图时字符串，可空 | 沿现有UTF-8字节上限；不写入全局draft |

字段完整冻结到plan、operation，参与指纹。version记录沿原operation_id关联已提交文字；不重复保存私人输入至普通日志。新增字段语义只在高级模块内解析。

单图计划必须满足：

```text
targets == [target_view]
base_versions只决定该图的可见编辑基底
target_modes只包含target_view
dependency_views为空
0 <= max_new_image_calls <= 1
max_new_model_tasks == 0
design_request与发送时的局部上下文一致
```

新局部编辑正常产生一个新图调用；显式恢复已取得候选时可以为零，不因下载失败另起生图。Host不能只检查张数，必须同时检查回显的scope、direction、局部文字和计划上下文，拒绝旧侧车忽略字段的响应。

### 4.2 Application.prepare分支

```text
读取并校验revision、组忙碌、主图和有效输入
allowed = design_targets(group)
解析design_request

all_inputs:
    selected = allowed
    冻结所有目标当前可见基底，沿原initial/edit与主图依赖规则

single_view:
    要求已提交创作、唯一合法目标以及原未决请求已解决
    selected = (target_view,)
    不合并pending_views
    有已校验可见设计 -> edit模式
    无设计且是合法失败方向 -> initial模式，检查本方向必要输入
    若生成辅助图需要尚不存在的主图 -> 拒绝；不增补主图
    图片模式无本方向上传 -> 拒绝

legacy:
    旧记录和旧调用沿原逻辑
    不把新单图请求降级为legacy

按原能力查询冻结供应商／模型
再次复查revision／忙碌，保存计划和指纹
```

严格单图不能直接调用现有retry后仅改变UI标题：当前初次主图retry可能扩为其它失败方向。新单图修复以明确single_view的新操作准备；若恢复／重试其既有operation，则继承single_view和原目标，不进入旧扩张分支。供应商身份、已取得候选及未知受理保护继续。

### 4.3 执行和草稿语义

| 动作 | 组级全局字段 | 方向字段与记录 |
| --- | --- | --- |
| 左栏保存 | 按原逻辑保存saved_draft／uploads；风格、输入失效产生原pending | 保留不属于输入变化的current |
| 全范围任务受理 | 沿原逻辑更新latest_submitted_draft；initial更新initial_input | 只创建本次T内attempt |
| 局部任务受理 | 不用局部内容更新任何全局字段，不推进全局提交基线 | operation保存global draft快照＋design_request，只有目标attempt |
| 局部成功发布 | 全局文字、风格、供应商草稿保持 | 目标current切换，清除目标pending／changed_upload；其它方向不变 |
| 局部明确失败 | 全局字段保持 | 沿原失败状态；没有源图失效的可用旧基底按原规则保留，不假装新图成功 |
| 原图删除／替换后的失败 | 全局草稿按实际保存 | 已清空current不能用失败回退复活旧图 |

全局文字已编辑但尚未形成匹配全范围结果时，局部重生成使用本次全局快照作为默认约束，仍保留原`draft_changed`等3D门禁。局部文字本身不能使全局草稿变脏，也不能把未完成的全局输入更新标记为全部完成。

### 4.4 提示词与参考图

给高级`DesignImageRequest`增加可选局部上下文，旧构造默认无局部。`design_prompt`按当前全局描述／风格产生原提示词，并独立附加“本方向局部要求优先于冲突的全局要求，未要求内容保持”。不改公共`DEFAULT_IMAGE_INSTRUCTION`或智能提示词。

有当前设计时，第一参考是被冻结并已验证的本方向可见设计，沿原编辑保持方向、主体、比例和构图。删除／替换已让current失效时，不复用旧图；无基底的失败方向走单视角初次生成。按本方向原图、主视角参考、纯文字依次选择可用输入。有当前图或失败的历史方向即使未上传本方向原图，也可独立发起局部请求。两段用户文字各自按2000字节上限，组合后的内部提示词不作为新的用户全局字段计数或截断。

GPT及YOYOClaw的高级2D适配都覆盖空局部／非空局部、有基底编辑／失败补图，不新增供应商模型或改变现有参考张数上限。

## 5. Host：确认、回执和异步生命周期

### 5.1 分离命令入口

- `generate_design`：固定全范围意图，左栏原Confirm继续。
- 拟新增`generate_single_design(view, text, callbacks)`：校验后prepare，响应匹配则直接run_plan；不调用Workspace的2D确认。
- 复用`run_plan`、pending-submissions和reconcile，增加高级scope用于核对和恢复；局部任务不arm新的首次引导、不用单目标替换原完整guidance_targets。
- 局部计划完成但尚未发出POST时，回执保存失败须清理该未发送回执，保留输入并允许存储恢复后的显式重试；已发出POST而受理不明的原回执继续保留，只核对原请求，不能静默重复创建。

单图命令捕获`CallbackContext`、draft／upload修订、group revision、目标、可见version／sha和浮层请求代次。预览Q或模式变化不作为业务输入变更；请求绑定原发送方向，不能按回调时显示的大图方向重新解释。

### 5.2 浮层状态

| 状态 | 行为 |
| --- | --- |
| Editing | 输入可编辑；空文字可发送；关闭清空；校验失败沿原反馈 |
| Preparing／Submitting | 冻结文本和方向，原命令锁防重复；不发第二个ID |
| 明确未受理失败 | 保留文字及原方向，允许修正后显式重试 |
| 受理不明 | 保留冻结请求、回执和文字；核对原请求，未解决前不放行新提交 |
| Accepted | 确认返回身份属于本请求后关闭并清空；结果继续原生成状态投影 |

“提交成功关闭”以operation受理为准，不能在本地点击／发出HTTP时就关闭，也不等供应商图完成。用户关闭浮层不等于停止已经受理的操作；停止仍走原停止入口。换Tab、换创作、关闭页面后，旧UI回调不得访问销毁控件或串入新浮层，业务回执由Host保留。

局部文字只存在输入框和本次冻结请求；未提交文本不写LocalDraft、不跨重启恢复。已受理文字随operation保存；错误日志仅保留脱敏ID／阶段／原因。

## 6. 方案A详细时序

### 6.1 保留首次路径，调整已有模型路径

原`generate_model(Confirm)`的首次卡片选择路径保持。已有模型“重新生成”调用高级专用选择事务，替换当前“先按旧选择资格准备plan、后bool Confirm”的顺序。可复用原确认组件样式，但高级选择内容和异步准备不改变智能确认回调类型。

候选建议结构：

```text
candidate_id、candidate_revision
callback_context、draft_revision、group_revision、input_epoch
original_model_selection_revision
selected_views、visible_versions
plan_request_sequence
ready_plan及其fingerprint／expires_at（仅当前候选有效）
```

Session拥有候选集合及规则，Host拥有计划请求、响应代次与提交生命周期，控件只展示并派发toggle／confirm／cancel。

### 6.2 打开与改选

入口仅绕开“旧选中辅助图失效”对打开候选窗口的阻塞，不能绕开只读、活动操作、未决受理、主图缺失及全局输入失配。原按钮可点击反馈规则保留；UI禁用状态不代替Host命令复查。

弹窗显示当前设计缩略图和原偏好。失效辅助图若原已选，应提供状态及取消能力；未选不可用图不允许加入。删除后没有current的方向保留状态，不回填旧图。主图不可取消。变更后即时更新张数／单多图说明，清除旧plan可确认状态。

每次合法候选都使用新command_id准备model-plan，继续发送原`selected_views`、`visible_versions`、`expected_input_epoch`、draft及原模型选项。相同候选的重复投递可复用其同一请求身份；候选已变更不能借旧ID改内容。

### 6.3 最终确认与取消

1. Host只接受最新候选代次且所有可见版本一致的plan；旧响应直接丢弃，不改变已选偏好。
2. 最终确认检查当前guard、组／输入／版本、候选修订、plan过期时间及供应商能力；非法时不POST model-jobs。
3. 本地确认记录须能恢复本次所选方向；与原pending receipt和LocalDraft保存衔接。保存失败不发任务，未确认候选不冒充已保存偏好。
4. 确认成功才将候选变为P3；更新选图修订后的提交上下文必须与自身变更一致，不能因commit导致自己的计划失效。
5. POST使用该候选plan的id、job_id和fingerprint，不重新查询旧effective_model_views拼第二份输入。
6. 明确取消不改P3／LocalDraft，不创建Job或供应商任务；准备文件没有费用，沿原有效期过期。
7. 最终确认后的传输未知沿原receipt核对，同一Job仅受理一次；确认后的选择偏好不会因HTTP超时回退成弹窗打开前的旧集合。

持久化顺序由Host承接：在最终确认的同一次GUI命令内复核身份，先原子保存含本次冻结选择身份的pending receipt，再按原保留旧检查点方式保存候选对应LocalDraft，二者成功后才提交Session内存偏好并POST。Session不直接执行文件IO。回执或草稿写失败时保留原偏好且不POST；已确定尚未发送的本地回执直接清理，不通过negative reconcile消耗未使用的计划，候选和plan保留，目录恢复后可在同窗再次显式确认。清理本地回执失败仍保留原保护，不能绕过未决门禁。若两份保存成功后进程退出，重启可读取用户已确认偏好并核对同一plan／Job，不另起新任务。POST发出后的传输未知继续使用原reconcile，回执里的选择仅是该次冻结请求的恢复证据，不成为另一个可编辑偏好所有者。

### 6.4 供应商路由断言

| 供应商／张数 | 原业务／适配入口 | 必须验证的载荷 |
| --- | --- | --- |
| Tripo，1 | `ModelTaskRequest.source=image` → `create_image_task` | 只上传冻结front；沿`/generation/image-to-model`，无多图inputs |
| Tripo，2～4 | `source=multiview` → `create_multiview_task` | 只上传所选方向；`/generation/multiview-to-model`，inputs按front／left／back／right过滤排序 |
| YOYOClaw，1 | `submit_3d` | 原`/yoyoclaw/model/chat/completions`；model＋主图image_base64，无multi_view_images |
| YOYOClaw，2～4 | `submit_multiview_3d` | 同原接口；主图image_base64＋所选辅助multi_view_images，按原left／right／back顺序；无面数字段 |

这里列的是本地基线源码适配，不把两个供应商不同的排序强行统一。两家都保持一个model-generation任务；现有OBJ格式转换属于原明确选项，单独计数，不误计为重复建模。不会自动新增2D请求。

## 7. 2D控件、布局与事件

### 7.1 ViewCard与页面投影

给共用卡片增加明确用途投影：2D预览、首次3D选图、大图弹窗。2D传入无选中装饰，单击不再进入Session延迟选择；3D保持原选择／双击规则。只为2D卡片绘制／命中星芒，不扩散到3D卡片或双击弹窗。

底部模式切换、缩略图及浮层都消费自身事件。卡片双击只查看，不能产生一次遗留选择；切Tab时清理旧2D待处理单击，防止被新上下文解释。

模式组与缩略图共用Workspace内的预览点击序列：首个点击立即响应；布局移动后，系统双击时间内同一屏幕位置的后续点击仍归属原手势，不能误中刚出现的缩略图。另一位置的明确点击立即响应；新建／换创作时清理该序列。该保护仅用于2D预览导航，不写入Session或持久化，不更改3D卡片事件或现有控件位置。

失败状态说明中“选中后补生成”须改为本次已确认的星芒入口说明，仅限该页面受影响文案。视角名继续主／左／后／右，保留现有名称位置，主图内部仍是front。

### 7.2 PreviewState

```text
mode = Grid
current_view = Front
has_entered_single = false
logical_creation_token
```

模式组点击只改mode；首次Single置Front，此后不覆盖current_view。缩略图点击只改current_view。结果更新和Tab往返保持；新建、打开另一创作、进程重启重置。

逻辑创作token不直接等同group revision或所有epoch变化。新空白首次创建服务组后应继续原展示状态；只有用户切换创作才重置。四格固定原顺序与空位；单图和缩略图使用同一当前结果来源，当前失图不跳到另一个方向。

保留现有ImagePreview／解码缓存，按实际图片身份更新；不在hover、切换焦点或重复render时做业务保存／同步文件哈希。这里仅避免新增预览带来额外同步工作，不实施原点选性能优化。

### 7.3 视觉参数与资源接入

以[需求第3节](2026-11-24-advanced-2d-description-preview-requirements.md#3-已读取的图标与参数)为尺寸与颜色的唯一详细基线。主要落实：

| 控件 | 逻辑参数 |
| --- | --- |
| 左下星芒 | 24×24、圆角6、黑65%；渐变`#3F52FF→#9E7CFF` |
| 页面内浮层 | 360×164、背景`#171717`、边框`#2F2F2F` 1、圆角8；删除推荐区后不保留原286高度 |
| 浮层标题／关闭 | 星芒28×28；关闭点击区24×24；关闭图形白80% |
| 输入／发送 | 输入344×100、圆角8、边框1.3白10%、黑40%；提示14／22白45%；发送20×20、白80% |
| 模式组 | 68×36、按钮32×32、组内2、圆角8、`#1F1F1F`；当前`#256FFF` |
| 缩略图／悬停 | 36×36×4、间距4、圆角8，当前边1／其它0.5；hover眼睛14×14及黑40%蒙层 |
| 名称气泡 | 52×28＋箭头10×5，背景`#121212`、字号12／20；仅hover |

保留现有中文字体，按FromDIP及原DPI重建机制缩放；浮点描边用现有绘制能力，不错误取整为逻辑0。布局根据可用中央区域放置，浮层不得越出窗口、遮掉自身发送／关闭；缩小／跨DPI时重新定位。沿既有最小窗口边界验收，不新增全局最小尺寸或整页布局改版。

12份源SVG及SHA清单已在忽略证据目录，实施时只接入需要的原生矢量资产。带阴影导出的74×42／373×299画布不作为逻辑点击区；浮层精简后的容器用客户端绘制或按已确认尺寸适配，不能缩放整张原推荐语背景充当新结构。

## 8. 异步、恢复与兼容检查表

| 触发 | 必须保留的身份或动作 |
| --- | --- |
| 上传／草稿保存期间再次编辑 | 对比捕获input、upload_revisions及当前草稿，不确认较新的同路径替换 |
| 图片下载／解码晚到 | 对比group、epoch、version／sha及invalidated，删除方向不复活 |
| 局部prepare期间切预览 | 仍绑定原目标；不能把Q变化映射为新业务方向 |
| 新建／换创作／只读变化 | 原Session／Host上下文使旧UI请求失效；受理记录单独收尾 |
| 取消A后旧plan返回 | candidate_id及request sequence不匹配，不能保存偏好或启用确认 |
| A内连续改选 | 每次使旧plan失效；只接受最终集合和版本 |
| 同组新任务接到旧回执 | 沿operation／job及request sequence，不能只比group_id |
| Stop早于execute受理 | 原stop-intent记账；晚POST不新增调用或导航 |
| 旧2D记录缺新字段 | legacy只读／原任务恢复；不补写虚构局部文字 |
| 新单图记录重启 | 局部文本和目标从原operation恢复，不自动发新图 |
| v1四图／v2可变模型 | approved_paths、Job持久化、粒子、历史、工作台共用原冻结身份 |

## 9. 实施顺序与每步完成条件

以下为已授权实施步骤，完成状态及验证证据只在主计划登记；不以阶段交付要求用户例行回复“继续”。

| 步骤 | 工作及依赖 | 完成条件／验证 |
| --- | --- | --- |
| D1 | 先完成Session目标／上传规则和高级命令上下文，保留旧记录读取 | 定向C++和Python证明全范围、主图门禁、strict-single及legacy边界 |
| D2 | 贯通局部prepare／execute、提示词、记录、回执与停止恢复 | 单图调用次数≤1，其它图／pending不变，全局基线不污染；失败／未知受理无重复 |
| D3 | 实现3D候选事务与同窗异步plan，接回原model-jobs | 取消零生成；旧选失效可取消；新选择、冻结及上传一致；两家全子集离线路由 |
| D4 | 接入2D模式、缩略图、星芒／浮层和限定资源，隔离3D事件及样式 | 主窗口核对红框参数、预览记忆、双击及无2D业务选中 |
| D5 | 同步相关架构／能力说明，构建匹配运行树，执行定向回归及授权实窗验收 | 分开记录逻辑、编译、GUI、真实任务及未验分支；形成可review diff |

同一构建目录单写者，编译期间不改源码。保留用户进程、工程、资产和其它未提交修改，不把提交／推送／PR当作本地交付前置。

## 10. 后续验证入口

以下为实施和验收入口，实际执行收据见主计划。依实际变更选择，不默认冷构建或全量测试。

```powershell
./dev.ps1 Test -TestPattern test_advanced_multiview.py
./dev.ps1 Test -TestPattern test_advanced_multiview_models.py
./dev.ps1 CppTest -TestSuite slic3rutils_tests -TestLabel AdvancedCreation -Jobs 2
./dev.ps1 CppTest -TestSuite slic3rutils_tests -TestLabel GenerationParticles -Jobs 2
```

供应商载荷沿现有`tools/ai/test_model_provider_gateway.py`、`test_generation_options.py`、`test_obj_generation.py`、`test_network_policy.py`及`test_yoyoclaw.py`补充并选跑：分别覆盖网关分派、Tripo实际客户端载荷和YOYOClaw网关／客户端；不虚列尚不存在的独立client测试程序。保持离线网络防护。新纯逻辑布局／候选测试优先并入原`test_advanced_creation.cpp`；若确需新增文件，按`tests/AGENTS.md`在原suite登记。

涉及高级架构／契约实现时：

```powershell
python scripts/verify_ai_integration.py --json
python scripts/architecture_review.py --check-map
python scripts/architecture_review.py --readme update
python scripts/architecture_review.py --readme check
python scripts/check_documentation_sync.py --base HEAD --worktree
git diff --check
```

文档同步脚本的worktree模式只提示范围，不能代替PR声明；本轮不建立PR。实际GUI使用本checkout的`dev.ps1`及完整`.tmp/dev/{build,run,data}`，独立配置和资产副本，记录EXE／DLL／资源／sidecar身份。

两家3D各1／2／3／4张真实任务的定义见用例，执行必须沿届时明确的生成授权和剩余预算，不沿历史计划自动新增调用或重置额度。只做离线验证的场景不能记真实服务通过。

## 11. 实施完成与验收出口

- 六项需求均有对应代码与定向测试，3D方案A没有未连接的旧入口；被替代的旧断言同步调整并保留其余保护。
- 全局／局部／预览／候选／已保存偏好分属明确接缝；单图、A的取消和失效、异步／停止／恢复均有行为证据。
- 新版本真实Windows主窗口核对两种预览、浮层、参数、首次／已有模型路径及导航；未完成的DPI／异常分支列为待执行，不用自动测试代替。
- Tripo和YOYOClaw的单图／多图载荷与实际冻结来源一致；真实任务按授权取得证据，缺少的分支保持未验。
- 交付源码／产物身份、实际测试结果、完整日志路径及diff复核；当前112条只是用例定义数，不代表112条已通过。

<a id="single-preview-and-thumbnail-development-20261125"></a>

## 12. 已确认的四格、单大图及缩略图视觉整改

本节适用于高级模式「生成2D设计图」的单大图预览，接续四格视觉整改。参考Figma画板 `1322:49631` 及子图层 `1322:50477` 至 `1322:50566`。现有Session、Host、生成记录和3D输入继续提供业务事实，`AdvancedCreationWorkspace` 管布局、预览方向、悬停和命中。用户于2026-11-25确认实施及Windows实窗验收。前序开发方案、用例和结果按各自基线追溯。

### 12.1 布局与视觉

| 控件 | 规格与布局 |
| --- | --- |
| 单大图 | 基准548×740 DIP，圆角16、无描边，中栏水平居中。先为现有页内状态和底部模式组留空间，再按可用宽高等比缩小，保持548∶740。图片等比铺满、居中裁剪，双击沿原查看入口展示原图。 |
| 标题 | 图内顶部常显，水平居中、距顶12，52×24、圆角8、黑底65%，白字12、行高18。采用现有Windows中文字体和「主视角／左视角／后视角／右视角」。成功态收起原底栏和重复「设计图」文字，等待、排队、生成中、失败和待重新生成仍使用原状态反馈。有图状态提示避开底部交互区。 |
| 四个缩略图 | 固定主、左、后、右，组内36×36×4、间距4、总156×36，圆角8，卡内水平居中、距底12。等比铺满。当前方向白色内描边1，其余白色内描边0.5。缺图方向沿用对应的空位和状态。 |
| 缩略图悬停 | 当前鼠标所指图片区域叠黑40%，中心眼睛14×14、白色80%。名称气泡在上方，间距2，主体52×28、圆角6、下箭头10×5，底色约 `#121212`、白字12、行高20。眼睛与当前方向描边可以分别出现。 |
| 大图悬停 | 有图且鼠标在卡片或图内控件时，显示同四格的六色标渐变和符合现有显示资格的星芒。0%、7.69%、16.35%、83.65%、91.82%、100%依次为黑60%、`#2A2A2A` 33.53%、透明、透明、`#131313` 29.97%、黑60%。移出后恢复常态，标题与四个缩略图常显。无当前图但可补生成的失败卡沿用可见星芒和现有反馈。 |
| 星芒 | 图内左下各距12，24×24、圆角6，黑底45%与图和蒙层直接合成。沿用可见图形12.86×13.71、原双色紫色渐变和小星60%不透明度。将已验证的空遮罩适配用于这份共用SVG。 |
| 模式组 | 中栏页底独立居中，与大图同轴。68×36、圆角8、`#1F1F1F`，两按钮各32×32、四周留2，当前蓝色 `#256FFF`，另一项白色。参考画板中的图底间距用于原尺寸预算，实际将预览余高分到上下。 |

计算时先得到中栏可用于图片区的宽W、高H，再取缩放 `min(1, W/548, H/740)`。常规尺寸下缩略条坐标为 `(卡左+(卡宽-156)/2, 卡底-48)`，星芒坐标为 `(卡左+12, 卡底-36)`。建议窄窗优先收缩大图与页底开关之间的空白。星芒与居中的缩略条发生碰撞时，星芒上移到卡内左侧的上一行。缩略条宽度不足时保持四项等宽、4 DIP间距、居中，按可用宽度统一缩小四个缩略图。以现有主窗口最小尺寸验证实际宽度及命中，跨DPI重新计算所有位置。窄窗按此规则适配。四格和单大图分别用原尺寸预算计算实际边界，再以完整预览区的余高居中。浮层按移动后的星芒位置重排。

四格固定为主／左、后／右。基准单卡260×350 DIP，四卡整体528×708 DIP，间距8，图角16。采用与单大图相同的居中裁剪、顶置标题及星芒绘制。

### 12.2 代码落点与实施顺序

1. 在 `AdvancedCreationWorkspace.cpp` 将 `preview_navigation` 收为独立模式开关，将四个缩略图交由单大图容器排版。在 `layout_grid()` 的2D单大图分支计算卡片、标题、星芒、缩略条和气泡边界，绘制与命中共用同一组坐标。已有模式切换入口继续使用 `set_preview_mode()`。四格使用确认后的布局。3D展示设计图时共用该视图及模式开关，任务视图接管中栏时一并隐藏设计图和模式开关。
2. 单大图按图片、渐变、标题及图内控件顺序合成。复用 `ViewCard`、`PreviewThumbnail` 的绘制及位图准备能力，按钮和缩略图的透明部分直接与卡片像素合成。模式按钮及缩略图继续消费现有 `AdvancedPreviewClickSequence`。星芒消费自己的点击，进入已有 `open_local_popover()`，依据星芒和中栏可见边界调整浮层，提交、校验、回执和关闭使用原流程。
3. 缩略图点击更新窗口内 `preview_view`，首次进入单大图显示主视角，后续模式往返及同创作的Tab往返继续记住所看方向。切换方向时关闭旧方向浮层，换创作时重置预览。普通卡面双击查看原图，快速双击模式或缩略图不得把第二下交给底下的图面或星芒。
4. `AsyncPreviewImage` 复用后台准备和有界任务。原铺满预览默认偏上，四格、单大图及其缩略图显式使用居中对齐，裁剪参数随路径、版本、像素尺寸和DPI一起参与请求匹配。准备任务按目标尺寸处理，切方向、换组、隐藏或DPI变化时只接收仍匹配的结果。调节尺寸时可暂用同创作、同方向、同当前版本的缓存位图。切换方向或创作后若缺少匹配位图，显示该方向占位，旧任务不得覆盖当前方向。悬停只触发局部重绘。
5. 3D四格复用 `Session::click()` 和 `double_click()` 区分选图与查看原图。单大图把每个缩略图的右上角留给独立绘制和命中的勾选框，展示上限18 DIP，紧凑时随缩略图缩小。勾选即时进入 `Session::toggle_model_view()`，与四格使用同一个资格判断，实际选择更新由 `Host::changed()` 保存和发布。3D输入角色关闭卡面渐变及缩略图暗色蒙层，减少悬停造成的卡面重绘。2D使用原渐变和星芒。任务视图与方案A使用原入口。
6. 将卡片星芒资源 `resources/images/figma-ux/advanced-2d-card-regenerate.svg` 按已复现的遮罩问题适配，四格与单大图共用修正后的图形。渐变、标题、星芒及模式按钮使用已读取的稿件参数，图内操作以现有星芒为本轮入口。

### 12.3 验收

| 场景 | 可观察的通过条件 |
| --- | --- |
| 标准尺寸、窄窗、可用DPI | 大图保持比例。四个缩略图全部处于图内且可点击，模式组在页底。标题、状态和星芒互不遮挡，浮层发送和关闭可达。 |
| 四个方向与缺图 | 依次点击四个缩略图，标题、图片与白色描边对应。缺图显示所选方向自己的占位。首次单图是主视角，四格往返、同创作Tab往返保持上次方向。 |
| 快速悬停与双击 | 在大图、缩略条和星芒间移动，整图渐变不闪烁。眼睛和气泡仅跟随悬停缩略图，双击缩略图只切图，双击卡面仍打开原图。已有 `AdvancedPreviewGesture` 回归通过。 |
| 星芒和浮层 | 大小星均为稿件紫色，按钮黑底能透出图。点击有效方向打开对应浮层，切方向后旧浮层关闭。使用已有样本检查失败和忙碌投影，并以定向回归覆盖原提交保护。 |
| 异步与业务回归 | 快速切方向、换历史组、调整DPI时，迟到位图不串方向。比对原全局目标、设计图版本和3D勾选，观看与切图没有产生生成操作。 |

实施后按变更跑 `AdvancedCreation` 定向回归，补有意义的布局与旧请求覆盖保护测试，再按既有Windows运行路径使用真实主窗口和套图副本验收。用同一组样本记录悬停及切图响应、源码与产物身份，逐项记录实际DPI和未执行环境。相关既有用例优先复用 `A2D-PRE`、`A2D-LOC-18/19`、`A2D-UX-01/04/05/06/08`，对应视觉参数在实施时按确认版同步。

### 2026-11-25 五项整改增量

- 全局描述长度提示及润色超限映射使用「2000 字符」，既有输入阈值用例继续验证 2000 与 2001 UTF-8 字节边界。
- 3D 已有模型确认窗的缩略图单行居中，说明回显触发内容重排。星芒局部使用 Windows 编辑框内部绘制占位，正文和系统输入法组合状态控制显隐，发送等待只停用编辑和重复发送。
- 大图详情绑定当时的设计路径及版本，最多缓存一张边长不超过 3072 像素的位图。滚轮和拖曳只消费已加载位图，拖曳边界保证图仍可见。
- 高级 2D 区分活动任务、可恢复结果及终态未知。用户再次通过原入口显式提交才生成新操作，已获得结果仍走「核对本次请求」，使用原发布保护。已结束但远端结果未知时，显式重试可能产生额外供应商用量。

### 2026-11-25 高级页关停和建模资格修复

- `AdvancedCreationWorkspace` 在页面及子控件存活时关停。解绑显示、DPI、大小、顶层窗口和计时器回调，撤销异步交付。通过隐藏浮层释放其窗口，避免在析构过程中修改描述输入框。普通隐藏、最小化恢复继续使用页面原有的显示事件。
- `AdvancedCreationSession` 将弹框进入资格和当前选图提交资格分开。用户可在原确认窗取消异常辅助图，取消窗口只丢弃候选。`advanced_multiview_models.py` 准备时检查所选集合，执行时复核冻结版本、文件校验值和当前资格。活动任务、未决回执、可恢复结果继续使用既有保护，重复确认沿用同一计划的幂等处理。
- 原生回归向页面投递晚到的隐藏、DPI 和计时事件，执行两次关停后处理事件。两家供应商的单、多图分派、缺图、旧四图调用、过期计划与幂等使用定向回归。Windows 结果详见[专项用例](2026-11-24-advanced-2d-description-preview-test-cases.md#a2d-exit-model-gate-results)。
