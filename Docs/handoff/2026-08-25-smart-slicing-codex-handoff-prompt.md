# 下一位 Codex 的智能切片开发提示词

下面的提示词可直接复制给新的 Codex 任务。它以本次文档提交前的功能提交 `e6808459f69290e5e1a45e0f2001fed252967d8f` 为代码基线；启动新任务后，应以实际 HEAD 和最新交接文档为准，不得重置或改写其后的合法提交。

```text
你将在以下工作区继续开发 OrcaSlicer 智能切片：

仓库：D:\Workspace\06_3DDY_smart_slicing
功能分支：codex/smart-slicing
已完成的 P2 功能基线：e6808459f69290e5e1a45e0f2001fed252967d8f
ADR-004 验收基线：8b77ad5b2f424fd95e45f1e7a26c03961dff0a89

开始前必须：

1. 读取仓库 AGENTS.md/CLAUDE.md，以及以下文档：
   - Docs/handoff/2026-08-25-smart-slicing-p2-status.md
   - Docs/architecture/ADR-002-smart-slicing-transactional-workbench.md
   - Docs/architecture/ADR-003-smart-slicing-orca-bridge-and-release-hardening.md
   - Docs/architecture/ADR-004-smart-slicing-p2-parameter-scopes-and-multicolor-policy.md
   - Docs/plans/2026-08-20-smart-slicing-final-goal-implementation.md
   - Docs/plans/2026-08-25-smart-slicing-p2-implementation.md
2. 运行 git branch --show-current、git rev-parse HEAD、git status --short。
3. 如果 HEAD 已领先交接基线，先审计新增提交，不要 reset、rebase 或覆盖用户改动。
4. 确认当前仍在 codex/smart-slicing，工作树中的既有未提交修改一律视为用户改动并予以保护。

当前状态：

- ADR-004 / P2 已完成。
- 已打通 WorkspaceRevision、可打印性检查、确定性候选、候选隔离试切、目标专属比较、一次确认、OfficialSliceGateway 单事务应用、官方切片和 Undo。
- 支持稳定、质量、速度、节省材料四种目标。
- 参数提案当前只允许 Plate/Process。
- 质量/速度可做有界层高建议；稳定可做原生摆放/朝向和保守 Brim；多色节省材料可做保持约束的工具顺序搜索。
- 多色顺序必须保持逻辑 ID、物理映射、材料分配、擦料塔、层范围和全部冲刷设置。
- 功能关闭时不得影响原版 Orca 切片。

必须遵守的边界：

1. 不要合并或反向移植 codex/orca-integration-v2，也不要复制模型生成业务代码。
2. 不要改写已验收提交历史；只在当前 HEAD 后提交。
3. 智能切片业务保留在 Domain、Application、Ports、SmartSlicing GUI 和 Orca 适配器中。
4. 正式工作区写入只经过 OfficialSliceGateway；候选配置和试切结果在用户确认前必须隔离。
5. MainFrame、Plater、CMake 等共享文件只做最薄接入；若必须修改，单独说明原因。
6. 不修改 3MF/profile 格式和原版默认行为；功能关闭时不得影响官方流程。
7. 不修改仓库根 task_plan.md、findings.md、progress.md。
8. 未经用户明确授权，不调用付费 API。若任务确实需要生成服务，优先 image2 GPT、其次 Tripo；不要把相关模型生成业务带入本分支。
9. 不引入 endpoint、凭据、网络端口、sidecar 或 provider 依赖，除非另有已批准 ADR 和明确授权。
10. 未经用户确认，不让集成线自动获取本分支最新 HEAD。

尚未开放的范围：

- Object/Process；
- Volume 和层范围目标；
- Material/Filament 参数；
- 温度、流量、压力提前和校准值；
- 直接冲刷调整、颜色重映射和擦料塔优化；
- 远程参数建议器。

不得在 Orca 适配器中把这些范围静默转写成 Plate/Process。任何扩展都必须先定义稳定 Domain 合同并通过 ADR 审批。

建议的下一步优先级：

A. 先完成剩余发布门禁：复杂多色人工 GUI、正式 Apply/Undo、取消、异常恢复、现有 3MF/profile、键盘/分辨率回归和 Release benchmark。
B. 在可用环境完成 macOS/Linux CI。
C. 如果产品负责人要求继续扩展能力，先起草下一份 ADR，建议聚焦 Object/Process 的稳定对象 ID、对象配置副本、修订哈希、多对象过期检测和一次 Undo 恢复，不要直接实现未批准范围。

开发工作流：

1. 先检查现状和测试，再给出小步、可独立验证的计划。
2. 每个行为变化先补失败测试；保持 Domain 不包含 wx 或 Orca 类型。
3. 试切和正式网关必须共享同一 Domain/原生校验合同。
4. 任一准备失败必须做到零正式写入、零 Undo、零正式切片副作用。
5. 用户选择和一次确认仍是唯一正式写入边界。
6. 使用 apply_patch 编辑文件，保护无关修改。
7. 每个逻辑增量独立提交；不要留下脏工作树。

Windows 构建注意：

- CMake：C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
- 使用 /m:1，避免并行 PDB C1090/C2471。
- 至少构建 Release 和 RelWithDebInfo 的 slic3rutils_tests、fff_print_tests、OrcaSlicer、OrcaSlicer_app_gui。

关键测试：

- build-p0/tests/slic3rutils/<Config>/slic3rutils_tests.exe "[AI][SmartSlicing][Parameters]" --reporter compact
- build-p0/tests/slic3rutils/<Config>/slic3rutils_tests.exe "[AI][SmartSlicing][Candidate]" --reporter compact
- build-p0/tests/slic3rutils/<Config>/slic3rutils_tests.exe "[AI][SmartSlicing][Apply]" --reporter compact
- build-p0/tests/slic3rutils/<Config>/slic3rutils_tests.exe "[AI][SmartSlicing]" --reporter compact
- build-p0/tests/fff_print/<Config>/fff_print_tests.exe "[MultiFilament]" --reporter compact
- Release 全量 slic3rutils_tests.exe

GUI 验收安全要求：

- 只启动 D:\Workspace\06_3DDY_smart_slicing\build-p0\src\Release\orca-slicer.exe。
- 使用独立 --datadir，例如 build-p0\smart-slicing-next-gui-data。
- 启动前列出所有 orca-slicer.exe；启动后核对 ExecutablePath 和 CommandLine。
- 不操作其他目录安装的 Orca。
- 在测试项目中验证；未经用户确认，不对用户正式项目应用候选。

准备交付时：

- 确保 git status --short 为空且全部改动已提交。
- 回复完整 40 位 SHA。
- 给出相对本交接提交的变更摘要。
- 列出修改过的 MainFrame、Plater、CMake 或其他共享文件及原因。
- 报告聚焦测试、全量测试、Release/RelWithDebInfo 构建和 GUI 结果。
- 明确配置、依赖、内部 Application Port、网络端口、产品数据目录、3MF/profile 是否变化。
- 明确 macOS/Linux、性能和人工 GUI 中仍未完成的门禁。
- 不要主动通知或触发集成线获取 HEAD，等待用户确认。
```
