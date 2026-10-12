# 原图到 AI 处理图的独立调试

生产入口仍为 `openai_preprocessor.preprocess_image`，主窗口的图像任务传入现有 `print` 参数。`image_preprocessing_policy.py` 负责类别、结构特征、材质风险和主体文字策略；`nonportrait_reference.py` 负责只读诊断。独立命令使用相同的提示词与供应商调用，默认不调用远端，也不创建 3D 任务。

```powershell
python tools/ai/image_preprocessing_debug.py --input "D:/samples/excavator.png" --instruction "轮式挖掘机，保留部件和开孔" --style realistic --output .tmp/preprocessing-debug --print-settings '{"width_mm":150,"nozzle_mm":0.4,"line_width_mm":0.4,"minimum_feature_mm":0.8}'
```

输出 `prompt.txt`、`prompt-layers.json` 和 `preprocessing.json`。`prompt-layers.json` 记录实际选中的规则键、文本、字符区间、启用状态及提示词/规则文件哈希；拼接所有 `text` 等于真实生产提示词。加 `--prepared 已有处理图.png` 可比较现有原图与处理图；加 `--generate` 才通过已配置的生图服务发起一次图片编辑。输出不能覆盖原图，生产预览与几何参考复用供应商原始像素。

## 单独调优提示词

统一编辑文件为 [image_preprocessing_prompts.py](image_preprocessing_prompts.py)。它只保存提示词数据，题材判断仍在 `image_preprocessing_policy.py`，API/图片请求仍在 `openai_preprocessor.py`。无需复制一整份长提示词或修改供应商代码。

非人像的拼接顺序为：原图权威与用户要求 → 所选风格 → 构图/功能支撑/背景 → 主体文字 → 题材 → 匹配结构特征 → 材质风险/结果自检 → 打印尺度 → 颜色材质与灯光。人像/未知保留既有兼容顺序，包括身份锁、源图裁剪、脸部/手臂/衣物几何、无展示底座、表面/肤色和禁贴原图脸；浮雕/场景风格可覆盖默认支撑规则。此次拆分保持实际提示词文字不变。

| 要调的类型/能力 | 直接修改的键 | 适用范围 |
| --- | --- | --- |
| 人像身份与五官/手臂/衣物 | `PORTRAIT_RULES.identity_lock`、`identity_geometry` | 身份锁仅 realistic / portrait_sketch；几何规则沿既有兼容路径 |
| 人像肤色、光照、表面细节 | `PORTRAIT_RULES.surface` | realistic / portrait_sketch；保留年龄和身份细节 |
| 人像裁剪、底座、禁止贴脸 | `PORTRAIT_RULES.source_crop`、`display_base`、`no_photo_overlay` | 人像/未知兼容路径；不在 2D 补全被裁掉的人体 |
| 动物/宠物 | `SUBJECT_RULES.animal` | 品种、耳/肢/尾数量、花纹、姿态 |
| 建筑 | `SUBJECT_RULES.architecture` | 层数、门窗/拱洞、柱、功能基础 |
| 机械/产品/家具 | `SUBJECT_RULES.hard_surface`；机械连接另调 `FEATURE_RULES.mechanical` | 部件、开孔与已有连接，不臆造机构 |
| 植物/盆景/珊瑚/食物 | `SUBJECT_RULES.organic`；枝冠另调 `FEATURE_RULES.branching` | 主轮廓、枝叶层级、原有空隙 |
| Logo/文字/徽章 | `SUBJECT_RULES.flat_graphic`、`TEXT_RULES.preserve` | 字形、拼写、负空间、浅浮雕 |
| 多主体场景 | `SUBJECT_RULES.scene` | 数量、左右关系、间距；场景风格支撑见下方 |
| 烟火/水花等特效 | `SUBJECT_RULES.effects` | 连续实体/浅浮雕表达，避免漂浮粒子 |
| 薄片/镂空/柔性/精密件 | `FEATURE_RULES.thin_surface`、`hollow`、`soft_surface`、`precision` | 条件叠加；适用题材在 `FEATURE_SUBJECTS`，不是所有类型通用 |
| 风格 | `STYLE_PROFILES.<style>`；非人像四种短风格覆盖在 `NONPORTRAIT_STYLE_PROFILES.<style>` | realistic / cartoon / sculpture / portrait_sketch 等；风格与题材分别调 |
| 背板/底座/功能支撑 | `SUPPORT_RULES.relief`、`diorama`、`source_functional`；人像见 `display_base` | 风格要求的支撑优先，其他按源图和用户要求 |
| 通用构图/背景/灯光/自检 | `COMMON_RULES` | 会影响多个类型，修改前查看实际导出段 |
| 颜色/哑光代理/打印尺度 | `MATERIAL_RULES`、`PRINT_SCALE_TEMPLATE` | 默认自然色，哑光仅显式启用；打印尺度仅非人像已接线 |

例如仅调动物，把 `SUBJECT_RULES['animal']` 的字符串改掉；不会把该条规则加到盆景或机械提示词。调整 `FEATURE_RULES['thin_surface']` 则会同时影响匹配薄片特征的雨伞、翅膀、叶片等。要新增更细的独立类型，需要同时添加关键词、结构规则及 `FEATURE_SUBJECTS` 适用映射；不要通过共享薄片规则偷偷改变其他题材。

`STYLE_PROFILES` 与部分共用助手也服务文字生图。人像/未知仍使用 `LEGACY_RULES` 中既有跨题材兼容段，未知并不代表识别为人像；手动 `--category` 可指定有把握的题材。现有风格别名由入口映射到标准风格；`custom` 使用经验证的用户自定义风格，不在规则文件中伪装成固定风格。

保存规则后，先不带 `--generate` 重跑上面的命令，在 `prompt-layers.json` 查 `rule` 与 `active`，核对实际入选的段和打印尺寸；再按需用 `--generate` 单独生成一次。常驻 Sidecar 需要重启才加载修改，安装版需包含新的规则模块；已完成图片/历史不会自动重做。人像可以将参数换为 `--instruction '成人肖像，保留身份和原图裁剪' --category portrait`，照样导出身份、裁剪和表面段，但现阶段不会把非人像的 `PRINT_SCALE_TEMPLATE` 自动附到人像上。

独立调试自定义风格时，用 `--style custom --custom-style '手雕哑光陶土，保留原图姿态'`；缺少或不合法的风格描述会在远端请求前拒绝。实际传给生图服务的描述与导出段相同。

此次框架拆分的针对性验证见 `test_image_preprocessing_prompts.py`，覆盖单题材修改隔离、人像风格条件、结构条件、实际一次生产编辑与完整无远端调试导出。提示词修改后的 2D 保真、3D 拓扑/薄壁、切片和实物应分别验证；单个类别的一对样本不能代表总体覆盖率。

| 优化 | 当前行为 | 边界 |
| --- | --- | --- |
| 类别路由 | 7 类非人像；补挖掘机、雨伞、椅子、犬类、花瓶、首饰、精密零件等；描述优先，文件名辅助 | 关键词证据，不是视觉识别；未知/人像沿用原提示词 |
| 条件提示词 | 仅添加匹配的机械、分枝、薄片、镂空、柔性或精密特征规则；保留视角、部件、开孔 | 不能凭单图证明隐藏几何、装配公差或焊接 |
| 文字与 Logo | 主体字形和标志保留；背景水印清理；明确要求删除主体文字时才移除 | 小字忠实度需查看实际生成结果 |
| 打印尺度 | 实际宽度、喷嘴、线宽和最小特征进入非人像提示词，强调已有连接局部加固和保留开孔 | 2D 尺度诱导不是网格厚度测量；仍需 3D 检查和切片 |
| 材质代理 | `--matte-proxy` 显式生成中性哑光几何参考 | 默认关闭；源颜色保留在原图，灰色结果不自动替换正式彩色方案 |
| 一致性诊断 | 本地原图/结果质量、哈希、可靠 alpha 下的孔洞和比例变化 | 不透明背景的掩膜不能可靠比较；像素孔洞不是部件计数 |
| 语义复核 | `--semantic-review` 显式发起一次视觉请求，检查部件、开孔、比例/视角、可见连接、主体文字 | 默认关闭；无重试；低信心、缺字段、请求失败为 unavailable；不作为 3D 合格证据 |
| 多视图准备 | `--view front=原正面图.png --view left=原左视图.png` 记录提供的参考与哈希 | 来源和跨视图语义一致性未验证；不生成背面，不自动提交 Tripo |

自动路由不会改用户所选风格。浮雕/版画保留风格要求的背板，其他物件仅保留原有功能支撑或用户要求的底座。材质风险与物体类别分开，例如玻璃花瓶仍是硬表面物体，透明材质不把它改分为特效。

生产 Sidecar 默认只做本地诊断。只有显式设置 `ORCASLICER_AI_NONPORTRAIT_REVIEW=1` 才调用视觉复核，使用现有 `OPENAI_API_KEY`、`OPENAI_BASE_URL`、`OPENAI_TEXT_MODEL`。复核缓存绑定原图/处理图哈希、完整前处理策略和复核模型；没有额外生图重试或 3D 创建。已完成历史不会自动重做前处理。

离线回归复用 `scripts/run_ai_offline_tests.py`，相关文件为 `test_image_preprocessing_prompts.py`、`test_image_preprocessing_policy.py`、`test_nonportrait_reference.py`、`test_openai_preprocessor.py`、`test_model_input_image_quality.py`、`test_printable_sidecar_pipeline.py`，以及现有 Sidecar/设计流程消费者。定向回归不替代生产后端、真实主窗口、3D 泛化或实物打印验收。
