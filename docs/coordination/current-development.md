# 2026-10-27 本地语义模型轻量化（实现、GUI 与轻量安装包通过）

主线：正常交付只使用原生 `mediapipe.cpu.v1` 双模型和单一 `ModelSemanticColoring / ColorRegionMajorityV2` 路径；FaRL/Torch 降为默认关闭的研究能力，旧历史只读兼容，新请求不再调用 Python/FaRL 后备链。

本轮：完成 provider、固定大小和 SHA-256 能力校验，生产包只安装 `ai/portrait_semantics`；原生失败时荣耀套装明确回退全局近色、普通 AI 配色保持原预览，无可信牙齿证据不制造牙齿白。源码测试、集成守卫、完整 Release 构建、运行树校验和人像/非人物 GUI 关键路径均通过；用户运行中的 PID `3316` 未结束或覆盖。

未结：标准用户安装/卸载、125%/150% DPI、可见牙齿样本、真实六头打印机及实体打印色彩保真未验证；内部安装包未签名。代码正确性、屏幕观感和实体打印色准继续分别报告。

下一步：当前验证入口仍为 `D:/TEST/OrcaSlicer-upstream-v2.5.0-alpha/.tmp/dev/run/orca-slicer.exe`；轻量安装包位于 `D:/TEST/packages/current/OrcaSlicer_AI_Internal_Fast_V2.5.0-alpha_20261027-a02806d457-mediapipe_x64.exe`。后续局部色块问题优先修正 V2 投影、区域投票或颜色块后处理，不恢复大模型运行时。

计划变更：最终 EXE 为 `255,139,350` bytes、SHA-256 `D4C8E6D9FC4BBC27A38AA20E888EE6EB61FE9A3ADD997323A2C4779DED9524B5`，相对旧包缩小约 77.4%；EXE/ZIP 分别扫描 `19,394`/`19,385` 个成员且零 findings/零 gaps，均恰好含 8 个原生语义文件、共 `64,989,390` bytes，重型 FaRL/Torch 路径命中为零。旧 1.13 GB 二进制已删除，小型证据归档到 `.retired-20261027-a02806d457-heavy`；本轮保持未提交、未推送。

# 2026-10-27 当前版本纯净 Windows 安装包（已交付）

主线：将干净提交 `a02806d45786e3ce7a76e0aba14c88de22b7f6ca` 打包为仅供内部验证的纯净 x64 Windows 安装包；不制作最终便携包、不启动安装程序、不结束 PID `11140` 的旧验证实例，也不覆盖 `.tmp/dev/run`。

本轮：固定 `.tmp/dev/build` 已核对为本 checkout 的 VS 2022 x64 Release。首次候选暴露移动 SDK 合并后的打包依赖缺口：CPack 安装规则需要 `orca_mobile_device_sdk_c.dll`，而 `OrcaSlicer_app_gui` 未构建 `orca_mobile_device_sdk_ffi`；显式构建该目标后 DLL SHA-256 为 `CCD819363CA708D84913596170112479B0B3FF420612B2ADAB739C6C104CD3FF`。第二次候选因长输出目录令 NSIS 无法打开深层 `jedi` 类型桩文件；改用 227 字符以内的短候选路径后完整通过。两份失败候选仅保留小型诊断证据，所有 `_CPack_Packages` 已删除。

未结：安装包未执行标准用户安装旅程，未验证 `Program Files` 权限、100%/125%/150% DPI、真实六头打印机或实体打印色彩保真。内部安装包未签名，`SignatureStatus=NotSigned`。当前打包脚本仍不会自动构建移动设备 SDK 的 FFI DLL，未来打包需先显式构建该目标或另行修复依赖链。

下一步：安装包位于 `D:/TEST/packages/current/OrcaSlicer_AI_Internal_Fast_V2.5.0-alpha_20261027-a02806d457_x64.exe`；如需安装验收，应在标准用户环境使用独立数据目录，逐项验证启动、模型历史、荣耀套装、导入切片和卸载，不把归档校验当作 GUI 验收。

计划变更：安装包大小 `1,130,724,380` bytes，SHA-256 为 `04F0E4CFD4AEEE44A49A3EA730F54E5EE91A2BAABA17BDCCB7AFE8845A8F7E1B`。安装包扫描 `50,526` 文件、临时 ZIP 对照扫描 `50,517` 文件，均为 `NOT_DETECTED_WITHIN_SCOPE`、`0 findings / 0 gaps`；ZIP 仅作载荷验证后删除。7-Zip NSIS 完整性检查通过；发布集成守卫 `53/53`、`[ModelGenerationPresentation]` `65/65`（8879 assertions）、`[AI][SmartSlicing]` `259/259`（3622 assertions）通过。`packages/current` 仅保留本版 EXE 及小型哈希、清单和扫描证据，旧版大文件已删除。

# 2026-10-26 工作台配色分支同步与 Draft PR #45（合并、验证及推送完成）
# 2026-10-27 当前：本地智能切片与工作区修改提交远端 PR

主线：SMARTSLICE-CLOUD-20261024；已交付侧栏修复及最新远端合并版本，检查入口反馈保持仅分析。

本轮：已按用户授权推送 `codex/task/smart-slicing-local-integration-20261027` 并创建/挂载 [Draft PR #46](https://github.com/arsenaltj/OrcaSlicer/pull/46)，目标 `codex/team/integration`。远端基线 `502014b3b3`，受测产品源码 `ba94c318ee`，候选49文件包含全部本地差异；旧PR #40已关闭未合入，其本地相关修改已纳入。127项Python离线回归、AI/SDK边界、README及diff检查通过；提交模式架构报告4图均9/9，完整版本与证据见主计划。

未结：PR创建时GitHub判断无文本冲突，候选CI已启动但未完成；非作者复核、最终合并版GUI/用户视觉验收及人工合入待完成。检查入口触发遗漏与汇总展示问题仍未修复；历史未验边界和1项Windows符号链接跳过保留。

下一步：用户测试已构建版本，按PR候选CI结果及非作者复核处理；本轮只补交付记录，不直接推送integration或自动合入，不启动未授权的检查入口修复。用户配置、模型、产物及旧 `.planning/` 保留于本机。

计划变更：原“不推送/不创建PR”边界被本轮明确PR授权替代；不将检查分析转为实现，不启动历史额外任务。完整版本与验证范围见主计划。


并行 SDK PR #47 同步（2026-10-27）：按用户授权将 `47f91d1e1d` 合并到 `task/task-20261026073432344-584f56`；冲突保留两组 SmartSlice 测试并修正 DTO 字段名，Windows 共享核心编译及 5 用例/58 断言、AI/核心 manifest 门禁通过。原 b7544577 SDK/APK 验收历史保留；最终二进制和 Mock Android 全量验收尚未完成，PR 继续 Draft。完整记录见主计划 SDK-SMART-PR47。
