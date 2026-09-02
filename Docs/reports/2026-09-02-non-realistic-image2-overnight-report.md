# 非写实风格 Image2 夜间优化报告

> 时间窗口：2026-09-01 21:30–2026-09-02 07:30（Asia/Shanghai）
>
> 当前状态：09:52 已完成；十小时自动检查已停止，飞书在线表格已写入并读回验证。
>
> 明确边界：冻结 `realistic` 和写实人像路线；只调用 `gpt-image-2`，未调用 Tripo 或任何 3D 付费服务。

## 1. 结论摘要

本轮围绕单色雕塑、手办、低多边形、浮雕和微缩场景五种非写实路线，完成 26 个用例的跨风格基线与 11 个高风险用例的定向复测。两个批次共冻结 185 个候选，Image2 实际调用 185 次，生成成功 183 张，2 个失败分别是一次连接中断和一次服务返回均匀图；失败候选都只保留一次付费尝试，没有自动重试。Tripo 调用为 0。

首轮基线揭示的最高置信度问题不是“Image2 不会做浮雕/微缩”，而是长提示词内部的优先级冲突：浮雕背板、微缩共享地台会被后面的通用无底座规则或人物展示底座规则覆盖；机械产品上的品牌字样清理也处于较低优先级。生产提示词修正后，独立 v2 的 11/11 浮雕都形成明确背板，器物与场景的微缩结果获得连续地台，机械臂、相机、电钻不再出现可读品牌式文字。

当前五种风格的建议定位：

| 风格 | 最适合 | 主要优势 | 暂不首推 |
|---|---|---|---|
| 手办 | 人物、宠物、吉祥物、产品拟人化 | 识别度高，主动避开恐怖谷，配色清晰 | 需要严格工业比例或单色材料表达的对象 |
| 单色雕塑 | 人物纪念像、动物、建筑、器物轮廓 | 材质统一、适合单耗材打印 | 多物场景的自动连通域评分仍容易误报 |
| 低多边形 | 动物、车辆、建筑、植物、家具、复杂轮廓 | 大切面语言稳定，能显著减少细碎纹理 | 需要柔和表情和圆润收藏感的人像 |
| 浮雕 | 正面轮廓、建筑立面、宠物、器物、场景压扁表达 | v2 背板规则稳定，单视图 3D 化明确 | 需要完整背面或自由观看的独立摆件 |
| 微缩场景 | 室内外空间、多物关系、食物托盘、建筑组合 | 共享地台能表达空间关系并帮助连接 | 没有环境关系的孤立单人物；视觉上会接近普通手办 |

## 2. 覆盖与数据来源

### 2.1 用例覆盖

- 20 个 Wikimedia Commons 许可审计图片输入：人物头像、人物全身姿态、猫、狗、鸟、章鱼、建筑、宝塔、汽车、自行车、工业机械臂、相机、厨具、电钻、运动鞋、雨伞薄结构、木吉他、关节玩具、盆景和石狮。
- 6 个可控文本压力用例：早餐托盘、咖啡店角落、山地露营、水母台灯、深色旅行棋组、阅读角家具组。
- 难例维度包括：多人/多物、遮挡、透明、深色、细杆、轮辐、毛发、植物枝叶、建筑层级、机械关节、重复元素、文字/品牌干扰、复杂背景和裁切。

所有网络输入均保留来源页、作者、许可证名称和许可证链接；完整审计见：

- [source-resources.csv](../../generated_models/image2-non-realistic-overnight-resources-v1/source-resources.csv)
- [resource-catalog.json](../../generated_models/image2-non-realistic-overnight-resources-v1/resource-catalog.json)

04:30 资源复核认为现有 20 个许可图片来源与 6 个文本压力用例已经覆盖本轮高价值缺口；继续下载相似素材只会增加重复样本，因此本检查点没有新增网络下载，也没有新增付费调用。

### 2.2 批次统计

| 批次 | 用途 | 候选 | 完成 | 失败 | Image2 | Tripo | 自动门禁 |
|---|---|---:|---:|---:|---:|---:|---|
| baseline-v1 | 26 用例 × 5 非写实风格基线 | 130 | 128 | 2 | 130 | 0 | 124/129 通过，均分 82.73 |
| targeted-v2 | 11 个失败簇/压力用例修正对照 | 55 | 55 | 0 | 55 | 0 | 51/55 通过，均分 81.49 |
| 合计 | 资源与修正证据 | 185 | 183 | 2 | 185 | 0 | — |

v2 的 4 个自动阻塞全部是单色雕塑多物场景被 `fragmented_subject` 启发式判定；人工总览中，物件已经通过托盘、基座、棋盘或地面接触。该结果继续说明自动门禁适合作为复核提示，不能代替样式语义和可打印连接的人工检查。

## 3. 生产优化

### 3.1 浮雕背板成为样式级硬规则

`relief` 现在明确要求一个简单实心背板，并声明其优先级高于人物展示底座和非人物无底座规则。完整主体必须压缩为正面浅浮雕并以大面积连接到背板，不能返回自由站立手办、器物或场景。

修正前重复失败：工业机械臂、卓别林、露营地、水母灯、阅读角。

修正后定向结果：11/11 用例均出现可见背板，并保留主体、数量和正面层级。

### 3.2 微缩场景共享地台成为样式级硬规则

`diorama` 现在要求一个具有平底的共享低地台，并声明其优先级高于非人物无底座规则。所有请求的主体和道具都必须与同一地台连接；孤立主体只允许最小、无装饰的接触平台，不再凭空增加岩石、植物、建筑或标牌。

修正前重复失败：茶壶/相机无地台、电钻随机岩石底座、人物退化为普通展示座。

修正后定向结果：机械臂、茶壶、相机、电钻获得连续低地台；早餐托盘、咖啡角、露营地、棋组和阅读角保留共享支撑面。孤立人物仍不具备充分场景语义，因此推荐逻辑保持谨慎。

### 3.3 非写实文字清理提升优先级

五种非写实风格在样式配置附近增加高优先级文字清理：移除可读文字、品牌、序列号、标签、水印和伪字形；面板或铭牌只保留为空白凹槽、宽沟槽或实色色块，不允许复制来源字形或生成替代拼写。

修正前工业机械臂和电钻重复生成品牌/公司式文字；v2 总览中的机械臂控制柜、相机面板和电钻机身只保留无字结构色块。

### 3.4 评测可靠性与展示

- Windows 瞬时占用 `state.json` 时，读取路径加入有限 `PermissionError` 退避；候选状态、提示词哈希和付费调用逻辑不变。
- 总览中无来源图的文本用例由误导性的 `PENDING` 改为 `TEXT INPUT`，明确区分“文本输入”与“尚未完成”。
- 新增资源目录生成器，合并批次、来源许可、提示词哈希、状态、评分、原始图、模型参考图、联络表和总览路径。

## 4. 视觉证据

### 4.1 基线总览

- [baseline-v1 第 1 页](../../generated_models/image2-non-realistic-overnight-v1/run/overview-sheets/primary/page-01.jpg)
- [baseline-v1 第 2 页](../../generated_models/image2-non-realistic-overnight-v1/run/overview-sheets/primary/page-02.jpg)
- [baseline-v1 第 3 页](../../generated_models/image2-non-realistic-overnight-v1/run/overview-sheets/primary/page-03.jpg)

### 4.2 修正后总览

- [targeted-v2 第 1 页](../../generated_models/image2-non-realistic-targeted-v2/run/overview-sheets/primary/page-01.jpg)
- [targeted-v2 第 2 页](../../generated_models/image2-non-realistic-targeted-v2/run/overview-sheets/primary/page-02.jpg)

### 4.3 修正前后同屏对照

- [浮雕/微缩修正前后第 1 页](../../generated_models/image2-non-realistic-overnight-resources-v1/comparison-sheets/page-01.jpg)：茶壶、工业机械臂、卓别林、相机、棋组和早餐托盘。
- [浮雕/微缩修正前后第 2 页](../../generated_models/image2-non-realistic-overnight-resources-v1/comparison-sheets/page-02.jpg)：阅读角、咖啡角、露营地、水母灯和无绳电钻。

对照页直接展示同一个用例的 baseline-v1 浮雕/微缩与 targeted-v2 浮雕/微缩。工业机械臂、卓别林、露营地、水母灯和阅读角从自由站立输出变成明确背板浮雕；茶壶、相机、电钻和场景组合获得连续地台；电钻上的品牌式字样被空白面板和宽槽替代。

### 4.4 推荐查看的单项联络表

- 低多边形动物：[石狮](../../generated_models/image2-non-realistic-overnight-v1/run/contact-sheets/model-reference/sculpture_lion__low_poly.jpg)
- 非写实人物手办：[爱因斯坦](../../generated_models/image2-non-realistic-overnight-v1/run/contact-sheets/model-reference/portrait_einstein__cartoon.jpg)
- 修正后器物浮雕：[工业机械臂](../../generated_models/image2-non-realistic-targeted-v2/run/contact-sheets/model-reference/mechanism_industrial_robot__relief.jpg)
- 修正后户外微缩：[山地露营](../../generated_models/image2-non-realistic-targeted-v2/run/contact-sheets/model-reference/text_outdoor_campsite__diorama.jpg)
- 修正后家具微缩：[阅读角](../../generated_models/image2-non-realistic-targeted-v2/run/contact-sheets/model-reference/text_furniture_reading_corner__diorama.jpg)

## 5. 图片资源包

统一资源包目录：`generated_models/image2-non-realistic-overnight-resources-v1/`

| 文件 | 用途 |
|---|---|
| `resource-catalog.json` | 两个运行批次、来源和 185 个候选的机器可读总表 |
| `source-resources.csv` | 26 个唯一用例的来源、作者、许可证、挑战和保留要素 |
| `image-resources.csv` | 每个候选的提示词哈希、状态、评分、原始图和模型参考图路径 |
| `feishu-summary.tsv` | 飞书表格首页的精简指标，可直接粘贴/导入 |
| `feishu-image-resources.tsv` | 185 个候选的飞书表格资源明细 |
| `comparison-sheets/` | 两页浮雕/微缩修正前后同屏证据 |
| `README.md` | 资源包说明与总数校验 |

资源完整性检查：183 个完成候选均能找到原始图、模型参考图、对应风格联络表和所在总览页；2 个失败候选保留错误状态与一次付费调用记录。

## 6. 已知限制与后续建议

1. `palette_material_is_fragmented` 在 v1 为 102/129，在 v2 为 43/55，比例都约 79%；该指标与人工可用性改善无明显同步关系，应先校准连通域/最小区域阈值，不应继续用更强 Image2 约束解决。
2. 现有自动门禁不验证“浮雕一定有背板”“微缩一定有共享地台”“图片内没有可读文字”等样式语义；本轮用冻结提示词契约和人工总览弥补，后续可增加独立视觉审阅字段。
3. 孤立人物的微缩场景难以在不捏造环境的前提下与普通手办明显区分；产品推荐应继续优先手办/雕塑/低多边形，只有用户明确需要环境时再建议微缩场景。
4. 两个 v1 失败是服务/传输异常，不是风格质量结论；本轮按成本安全原则未重试。
5. 本轮只优化 3D 生成前的参考图，没有验证下游 Tripo 3D 网格；这符合“只使用 Image2、不调用 Tripo”的授权边界。

## 7. 验证状态

- AI Python 全量回归：504/504 通过；涉及 Tripo 文案的合同测试均使用 Mock/loopback 夹具，没有创建真实 Tripo 任务。
- 提示词与评测定向回归：59/59 通过。
- 资源目录生成器回归：3/3 通过。
- Python 语法检查和相关 `git diff --check`：通过。
- 两份浮雕/微缩修正前后对照页已人工查看，源图、前后结果、文字清理和支撑面变化都可读。
- 飞书写入与收尾：`Sheet1!A1:H30` 已写入；只读接口按 revision 211 读回 30×8，语义比对 0 处差异。

## 8. 飞书写入结果

目标为用户指定的飞书 Wiki 页面，实际内容类型是在线表格“模型生成记录”。专用表格接口先完成解析、A1:H30 空值读取和 240 单元格 dry-run；真实接口写入因 Feishu `91403 Forbidden` 未修改云端，因此在用户明确确认后改用当前已登录的飞书页面完成写入。

09:52 已把冻结的 30×8 最终矩阵写入 `Sheet1!A1:H30`，内容只包含本轮总结、指标、风格建议、验证结果和本地图片/资源路径。页面批量剪贴板被拦截后，采用逐单元格定位、输入和提交；每批都确认“已经保存到云端”，没有修改 A1:H30 之外的区域。

最终通过独立只读接口读回 `A1:H30`，得到 30 行 × 8 列、revision 211。飞书原生把日期存为序列值、把 URL 存为富文本；按显示语义归一后，240 个单元格与冻结矩阵 0 处差异。两个 `11/11` 比值单元格已显式设为纯文本，避免被自动识别为日期。十小时夜间心跳保持已删除状态，不会继续触发自动检查。
