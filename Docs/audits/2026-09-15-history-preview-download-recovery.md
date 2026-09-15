# 历史模型切换后的预览下载状态恢复

本次修复基于 `a38cd3d56b`，针对“旧任务的 AI 设计图仍在下载时，打开历史模型后操作按钮一直不可用”的问题。

## 原因与修改

`load_library_entry()` 允许用户从下载中的图片预览切换到历史模型。历史模型成功加载后，会取消旧客户端请求、递增任务序号并清空旧任务 ID。旧下载回调因此被序号检查丢弃，但原先的 `m_preview_download_in_flight` 没有清空。`refresh_controls()` 持续将界面判定为忙碌，而“取消加载”又要求存在任务 ID，导致导入、重新开始等操作无法恢复。

修复仅在历史模型成功加载的回调中清理旧图片下载的运行标记、取消标记、可用输出标记、输出名称和下载路径。之后按历史记录恢复图片与模型；历史模型解析失败时，继续保留旧任务及下载状态。旧请求的成功或失败回调仍须通过任务序号检查，不能覆盖已经切换的历史记录。不删除旧图片、模型或服务端任务。

## 主窗口回归步骤

使用独立配置和历史模型副本，启动已 mock 图像及 3D 提供商的本地 sidecar。现有离线 GUI 启动器支持 `-SlowPreview`，可让图片下载按小块缓慢返回；不得用真实收费请求制造等待。

| 场景 | 操作 | 预期 |
|---|---|---|
| 下载中成功打开历史模型 | 在生成页触发离线图片预览；显示“取消加载”且图片仍下载时，打开历史资产并加载一个有效模型。 | 历史模型及其图片正常显示；“导入到准备页”“重新开始”和输入控件恢复可用，没有无法取消的忙碌状态。 |
| 旧下载迟到 | 完成上述切换后，等待旧图片响应的全部分块或取消响应到达。 | 历史模型、图片、标题和状态不被旧任务覆盖；仍能导入和重新开始。 |
| 历史模型解析失败 | 旧图片仍下载时，加载夹具中的损坏模型文件；不要破坏真实历史资产。 | 显示历史模型加载失败；原任务、输入与预览下载继续保留。原图片下载完成后可正常显示，也可取消下载。 |
| 缺图历史与再次生成 | 旧图片仍下载时，加载一个没有关联图片的有效历史模型；随后点击“重新开始”，再次触发离线图片预览。 | 历史模型可导入，图片区显示素材缺失提示；不显示上个任务的下载图。重新开始后能再次下载并确认图片，无遗留取消标记。 |

## 本轮实际验证（2026-09-15）

源版本为 `a38cd3d56bf5824ad36255f66c42af215fbbe512` 加本报告所在提交的修改。源文件 `ModelGenerationPanel.cpp` SHA-256 为 `FE6165BAB074A1911200BA29E335DF5736D4262BF401D5B2037C210F5E24CBE4`。

- 完整主程序 `OrcaSlicer_app_gui` 与 `slic3rutils_tests` Release 构建成功；链接器仍有既有的 LIBCMT 冲突警告。
- C++ `[ModelGenerationPresentation],[ModelPreviewState]`：23 个用例、381 个断言通过。它们覆盖历史记录、缩略图及保存的编辑状态，不替代面板并发操作验证。
- 集成 guardrails 51 项、team integration service 52 项、candidate 10 项、bootstrap 18 项，共 131 项 Python 回归通过；完整 `verify_ai_integration.py --json` 和差异检查通过。
- 经 CMake install 安装到独立 `build/pr10-review-preview`，Python 3.12.13 / Pillow 12.2.0 隔离导入成功；实际运行 DLL SHA-256 为 `020C4AD624C7374EA1644D9AC8DF7035C3CE753EA8E8C08E152D49C098405EFD`，EXE 为 `3703E416740B1B7B8113BD4C804FEC5025B00CBC461990FF87DAAFD8FCD1FA45`。
- 本机无固定验证 registry，本轮直接使用独立运行目录和 `D:/TEST/pr10-review-validation/gui/runtime/datadir` 执行主窗口验证；提供商被 mock，sidecar 网络限于回环，未发起真实收费请求。

主窗口实测：恢复设计记录 14，拦住其 `model-reference` 响应；在“取消加载”可见时切到历史库并加载模型 18。模型显示后导入与重新开始按钮恢复可用。随后放行旧响应，模型、标题和按钮没有回退。实际完成颜色确认和准备页导入，未隐式切片；打印机、耗材和工艺预设名称保持原选择。点击重新开始后回到可编辑输入状态，准备页工程保留。

失败实测：重新打开设计记录 14，再次拦住预览响应，加载仅在隔离夹具中去掉面的模型 17。显示“历史模型加载失败，保留当前模型与预览”，原下载仍可取消；放行后原 AI 设计图正常完成，生成 3D 和重新开始按钮恢复可用。

本地截图保存在 `D:/TEST/pr10-review-validation/`：`history-switch-before-response.png`、`history-switch-after-response.png`、`history-import-preparation.png`、`history-failure-keeps-download.png`、`history-failure-original-preview-completed.png`。请求与完成日志在该目录下 `gui/runtime/logs/`。未重新执行缺图历史的组合场景、完整 3MF 往返、基本切片、真实服务生成或跨平台 GUI；以上结果不表示全旅程无缺陷。

## F002：保留旧集成服务要求的候选检查

`.github/team-collaboration.json` 原来只保留 AI 架构检查、Windows 构建和 Windows 单元测试，但旧服务配置校验仍强制要求 `Team integration candidate`。恢复该检查，并同步 bootstrap 示例/校验常量和架构锁/验证器，保留已有 Windows 两项检查。跨组件测试确认真实配置一致，删除候选检查会被拒绝，生成的保护规则可由旧服务与扩展四检查服务读取，缺检查或 App ID 不符仍然拒绝。

本轮只修改仓库配置和校验代码，没有修改 GitHub 线上保护、机器人自动修复权限或生产服务私有配置。候选 job 只依赖 inspect 和 Windows 两项；但旧服务 `candidate_evidence()` 仍检查整个 workflow 的成功状态，不能据此宣称旧服务已完全忽略 Linux/macOS 结果。新提交须重新运行 CI 和非作者复核，本报告不是审核通过凭证。
