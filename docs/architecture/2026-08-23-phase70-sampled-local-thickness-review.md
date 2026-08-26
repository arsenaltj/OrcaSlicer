# 阶段 70：采样式局部厚度预检验收

日期：2026-08-23

## 结论

阶段 70 已增加附着型局部薄壁/细连接预检。对于闭合、流形、绕序一致且无退化面的模型，质量门会对有上限的表面样本进行同组件短距离对射；至少 2 个样本、累计至少 1 mm² 的表面在 0.8 mm 内命中法向相反的非相邻面时，产生 `thin_local_wall_regions` 复核提示。

该指标补充阶段 67 的组件整体厚度：大型主体加薄片或细颈仍是一个组件，整体主轴尺寸可能正常，但局部对射可以发现真实的窄间距。结果不自动修改几何、支撑、摆放或切片参数。

## 算法与契约

- 报告 schema 保持 `1`，门禁版本提升为 `structural-v6`。
- 新模块 `sampled_local_thickness.py` 只使用 Python 标准库。
- 三角面包围盒按 10 位 Morton 码一次排序，构建固定叶容量的只读 BVH；避免递归层层重排。
- 小模型检查全部面；大模型按 Morton 顺序确定性分层采样 4096 个面，并补充最多 512 个最大面积面，去重后总量有上限。
- 射线从面重心沿法向和反法向发出，只搜索 0.8 mm；排除自身、拓扑邻面、其他组件，以及法向夹角不足的命中。
- 默认置信门槛为 2 个命中样本和 1 mm² 累计采样面积。
- 新 thresholds：`min_local_wall_thickness_mm`、`local_thickness_sample_limit`、`local_thickness_bvh_leaf_size`、`min_thin_local_samples`、`min_thin_local_sample_area_mm2`、`max_opposing_normal_dot`。
- 新 metrics：`local_thickness_available`、`local_thickness_sample_count`、`thin_local_surface_sample_count`、`thin_local_sample_area_mm2`、`minimum_sampled_local_thickness_mm`。
- 组件整体已被判薄时不重复增加局部薄壁 warning，但保留局部采样 metrics。

## 自动化验证

- 红灯基线：新增测试首先因缺少 `structural-v6` 和局部厚度 metrics 失败。
- `test_printable_model_quality`：28/28 通过。
- 质量门、Sidecar 契约与可打印流水线定向：54/54 通过。
- 全量模型生成离线测试：230/230 通过，33.365 秒。
- 模型生成 C++ 回归：16 个测试用例、140 个断言通过。
- Python 语法检查和 `git diff --check` 通过。
- Windows Release `OrcaSlicer` 构建与链接通过；仅有仓库既有 `LNK4098` 默认库警告。

新增测试覆盖：

- 单一闭合流形组件由 10 mm 主体和 5 × 10 × 0.4 mm 附着细颈组成；组件整体最薄尺寸为 10.0 mm，局部对射测得 0.4 mm，并触发新 warning。
- 同一细颈经过 Y/Z 双轴旋转后仍测得 0.4 mm。
- 10 mm 实心方盒没有 0.8 mm 内相对表面，不产生局部薄壁 warning。
- 开放细颈模型的局部厚度指标为未知，不输出结论。

## 真实高面数复核

对三个近期 290k–297k 面真实模型运行 `structural-v6`：

- 分析时间分别约 11.94、13.02、11.69 秒。
- 实际采样分别为 4604、4602、4604 个三角面，没有突破上限。
- 分别有 152、134、432 个样本在 0.8 mm 内命中同组件相对表面。
- 最小命中距离分别为 0.024、0.018、0.010 mm，累计薄面样本面积分别为 5.13、3.36、10.15 mm²。
- 三个模型原本均存在局部悬垂和大量微小色块；新增 warning 与其高细节、不可稳定打印的小几何风险一致，状态仍为 `review`，没有新增硬拒绝。

高面数分析约比 `structural-v5` 的 5.6–6.0 秒增加 6–7 秒。该成本来自一次性 BVH 建立和有上限射线采样，当前可接受；后续若进入频繁交互场景，应缓存同一制品报告而不是重复构建索引。

## 仓库本地 GUI 验收

验收应用为 `D:\Workspace\06_3DDY_claude\build\src\Release\orca-slicer.exe`，Sidecar 为本工作区 `tools/ai/orca_ai_sidecar.py`，独立端口为 `18764`。

1. UUID `00000000-0000-4000-8000-000000000070` 的闭合细颈样本含 20 个顶点、36 个三角面，尺寸 15 × 10 × 10 mm。
2. `/recheck` 后质量卡显示“检测到附着在主体上的局部薄壁或细连接，请检查是否需要加厚”。
3. 展开指标显示最薄组件 10.00 mm、薄型组件 0；局部厚度采样 36 个、最薄命中 0.40 mm、薄面样本 4 个。
4. 同一样本还正确保留已有局部悬垂提示，两个信号互不覆盖。
5. 验收发现并修复 `wxCollapsiblePane` 展开后父页未重新布局的问题；现在模型预览会缩小，指标与 AI 视觉复核区域不再重叠。
6. Sidecar 日志仅包含 `/health`、任务/制品读取和 `/recheck`，没有 Image2、Tripo、AI 视觉复核或其他付费端点调用。

## 架构与兼容性

- 修改范围为模型生成质量模块、Sidecar/C++ 模型生成契约、`ModelGenerationPanel`、模型生成测试、打包运行时清单和文档。
- 为确保已打包测试版包含新标准库模块，`scripts/package_windows_ai_test.ps1` 和 `packaging/windows-ai-test/setup/Check-Environment.ps1` 各增加一行 `sampled_local_thickness.py`；这是本阶段唯一打包接入变化。
- 未修改 `MainFrame`、`Plater`、CMake、3MF、profile、依赖、永久端口配置、输出目录约定或原版默认行为。
- 未引入智能切片业务，也未合并、反向移植或复制 `codex/orca-integration-v2` 代码。
- 旧报告缺少新字段时 C++ 默认为不可用；无效拓扑或索引失败时安全降级为未知。

## 下一步

阶段 71 可把局部薄壁证据映射回预览选区：Sidecar 输出有上限的命中面范围或 C++ 复算同一规则，让用户像定位悬垂面一样直接查看细颈位置。需要保持 face-order 契约、报告体积上限和旧报告兼容。
