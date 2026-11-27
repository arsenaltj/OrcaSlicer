# 本项目的打印与颜色边界

这份知识说明用于评审模型产物和颜色交接。执行步骤见 [模型评估 Skill](../../.agents/skills/model-generation-evaluation/SKILL.md)，产品导航见 [AI 工程入口](../AI_ENGINEERING.md)。这里的实现事实应随契约变更核对，硬件未知项不能由术语推断。

## 物理通道、目标色和配方

| 概念 | 本项目含义 | 不可据此推断 |
|---|---|---|
| 物理耗材通道 | 真实 filament input 对应的零基 slot；当前契约支持 1–6 个 | 六个界面色块不等于六个可用硬件通道 |
| 目标色 | 模型希望呈现的颜色和区域 | RGB 显示色不是实际材料光学特性或已校准打印色 |
| 虚拟混色配方 | 从物理池中选择组件及比例，供后续工艺表达 | 当前契约每个配方最多 3 个组件，不是任意六组分混合 |
| DiscreteFilament | 离散耗材选择/分区与兼容 fallback | 不是连续色域复现 |
| ProcessMix | 过程色意图及配方表达 | 不指定喷头拓扑，也不证明 CMYK 硬件执行已实现 |

依据：[ColorIntent.hpp](../../src/slic3r/AI/Contracts/ColorIntent.hpp)、[ADR-006](../architecture/ADR-006-six-channel-model-color-intent.md)、[Orca 配方能力](../../src/libslic3r/ColorDecomposeRecipe.hpp)。通道上限、单配方组件上限和 target palette 上限是不同约束。

六通道不能自动解释为 C/M/Y/K/W/透明或其他固定组合；具体材料、通道映射、逐层/空间/同喷嘴混色方式和标定数据需要设备证据。[人像路线提案](../architecture/realistic-beautified-portrait-printing.md) 已明确这些未知项，它不是硬件规格。

已加载工程的耗材/清洗矩阵标准化使用`update_multi_material_filament_presets`，允许硬件喷嘴闲置并保留实际装料。`update_filament_count`用于启动/切换时恢复保存的预设列表，会按原生最小数量补齐；不要将这个恢复入口混用于工作台运行态。两种边界由现有PresetBundle恢复测试和工作台耗材测试分别验证。

## 几何、颜色和文件交接

- 图像、纹理 UV/材质、顶点颜色和打印区域是不同表示；屏幕上有彩色纹理，不保证导入后有正确可切片颜色区域。
- 当前生成请求默认输出GLB，本地模型加载与后处理支持OBJ/GLB；具体格式以任务产物元数据和实际文件为准。`color-intent.v1.json`是OBJ交接链路的伴随契约，不能假定每个GLB都具有该文件。客户端schema/hash校验只证明检查覆盖的文件关联，不证明SmartSlicing已使用意图生成配方。
- 生成模型不等于Orca项目3MF。项目3MF包含项目/配置语义，生成模块不应自行拥有这些状态；已实现的GLB载入和候选导出，与制造母版的完整目标分别判断。
- 生成阶段结束于产物交付；导入、试切、正式切片/G-code 和机器执行分别需要证据。导入成功不应隐式改变预设或触发切片。

依据：[生成产物](../../src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp)、[请求默认格式](../../src/slic3r/GUI/AIModelGenerationClient.hpp)、[客户端校验](../../src/slic3r/GUI/AIModelGenerationClient.cpp)、[OBJ/GLB载入](../../src/slic3r/GUI/AI/Model/ModelArtifact.cpp)、[候选导出](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationBeautyExport.cpp)、[Orca适配器](../../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)。

## 从工作台更换打印机

从主窗口齿轮打开「设置 → 打印机」，选择目标机型及喷嘴直径。工作台耗材区显示当前配置的摘要。选择荣耀六色套装时，如果实体喷头不足，工作台给出容量和设置路径的文字提示。设置里的机型和喷嘴直径指向本机预设。喷头数是硬件能力，已装载实体耗材和叠色是工程配置。应用后载入目标预设的上次耗材和工艺，初次应用按原生默认配置。切回原规格时恢复其配置。

工程按目标槽位重新核对材料和颜色。若目标缺少已用槽位，提示对应盘及引用位置。成功换机返回原工作台模型。用户在耗材区显式应用荣耀套装，之后按原流程切片。绑定旧机的耗材撤销记录在切回原机后恢复。界面控制和工程规则分别见[设置窗口](../../src/slic3r/GUI/Redesign/RedesignSettingsDialog.cpp)及[预设切换服务](../../src/slic3r/GUI/Redesign/PrinterSettingsService.cpp)。

## 质量检查能证明什么

[结构分析器](../../tools/ai/printable_model_quality.py) 输入为已归一化的 Z-up OBJ；长度阈值按毫米使用。来源单位、打印尺寸或朝向不明时，先报告未知，不能把默认阈值的结果解释为制造合格。

分析器覆盖顶点/面合法性、边界和非流形边、连通分量/悬浮、接地、薄部件、[局部壁厚采样](../../tools/ai/sampled_local_thickness.py)、悬垂区域，以及顶点颜色与目标 palette 的覆盖。检查原始 status、errors、warnings、thresholds 和 available 标志；缺失测量不能按零风险处理。

边界边为零且未检出非流形边，不等于已经穷尽顶点非流形、自交或所有内部几何问题。有限壁厚采样不保证每个位置达标；悬垂阈值不能代替指定打印方向、材料和支持配置的验证。OBJ 分析也不自动验证所有 MTL/纹理文件的视觉效果。

工作台修复区分严格接收与固定官方版本的原生兼容接收。严格模式继续要求完整拓扑安全；原生兼容模式必须具有真实逐体修复执行及源／输出几何身份，可保留官方允许的交叠，并记录实际自交、拓扑及几何变化。保存、历史、美颜和切片导入核对同一证据；单个兼容标志不能放行。具体规则及版本见[ADR-009](../architecture/ADR-009-native-repair-acceptance.md)。

原始纹理／材质外观与打印分区分别保留。通用颜色后备只给已接受的几何生成确定性标签，不再次移动顶点；记录实际颜色数、后备方式和色差。多色分区可用不表示纹理无损，也不表示实体打印色已标定。原生修复成功还需外观、分区、发布、凭证和完整首帧安装全部通过，才能报告工作台检查完成。百万／二百万面链路使用容量及资源预算检查，资源不足单独报告，不自动压面、丢色或降低检测。

| 证据层 | 验收问题 |
|---|---|
| 输入与参考图 | 图像是否可用；多视图是否一致；身份/造型目标是否保留 |
| 几何与颜色预检 | 本次阈值下发现了什么风险，哪些测量不可用 |
| 导入与切片 | 导入后区域/颜色/尺寸是否正确；实际配置下是否可切片；预览与 G-code 是否符合预期 |
| 真实打印 | 身份/造型、表面缺陷、色差、强度和重复性是否符合约定标准 |

前三层不能替代最后一层。当前仓库有结构/视觉/切片测试与报告，不据此宣称已有经硬件标定的全链路打印资格。

## Mobile direct color SDK (2026-11-20)

The std-only preview palette, Direct matcher, CIEDE2000 function and quality evaluation now share source under src/slic3r/AI/Color; existing desktop headers are compatibility includes. Model SDK direct profile excludes GUI/layered recipe execution, preserves the desktop 1..6 direct limits, and marks trial-only drafts as nonmanufacturing. Source/project/SDK and material/process identity must remain current. New Slicing color export returns explicit native part/instance identity and requires source-project rebind; flattening transformed colored objects into a single generic volume is not a completed project round trip.
