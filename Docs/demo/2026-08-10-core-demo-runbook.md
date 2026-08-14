# OrcaSlicer AI 核心演示 Runbook

## 演示目标

在 Windows Release 版本中完成一条真实主链：

`文字或图片 → 审核输入 → 真实 Tripo 生成 3D → 导入盘面 → 自动摆放 → 切片预览 → 保存/重开 3MF`

本次不演示 macOS/Linux、账号计费、AI Assistant、自动修复、自动上色或参数优化闭环。

## 演示前检查

1. 确认 `build\OrcaSlicer\orca-slicer.exe` 存在。
2. 确认 Python 3 可由 `python` 启动。
3. 确认 `OPENAI_API_KEY` 与 `TRIPO_API_KEY` 已通过环境变量配置。不要在终端或投屏中打印密钥。
4. 在仓库根目录运行：

   ```bat
   start_orcaslicer_with_ai.bat --check
   ```

   看到 `AI sidecar is ready for real text and image generation.` 才进入演示。
5. 正式演示前关闭残留的 OrcaSlicer 和旧 sidecar 窗口，再执行一次检查，避免旧进程占用 `127.0.0.1:18764`。

## 一键启动

在仓库根目录双击或运行：

```bat
start_orcaslicer_with_ai.bat
```

启动器会复用已就绪的 sidecar；若服务未启动，则拉起 sidecar、等待最多 30 秒，再启动 Windows Release 程序。

演示启动脚本显式启用 `ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=1`。当前 OpenAI-compatible 代理可能返回 HTTP 502；此时界面必须如实显示预处理不可用，并用原始文本或原图继续调用真实 Tripo。不得表述为 OpenAI 预处理成功。

## 现场操作

### 文生 3D 主路径

1. 打开 `3D Generate`，选择文字生成。
2. 使用固定提示词：`A compact stylized desk mascot with a flat circular base, one solid watertight object, no thin floating parts, suitable for FDM printing.`
3. 等待预处理完成，在确认页审核提示词。若显示原始输入降级，口头说明“预处理服务暂不可用，当前保留原始输入，后续 3D 生成仍为真实 Tripo 调用”。
4. 确认付费生成，等待状态进入 `ready`。历史实测约 4 分 44 秒，现场按 3 到 6 分钟预留。
5. 导入生成结果，执行自动摆放，确认模型位于盘内。
6. 选择以下已验证预设：
   - 打印机：`WonderMaker ZR 0.2 nozzle`
   - 工艺：`0.08mm Optimal @WonderMaker ZR 0.2 nozzle`
   - 材料：`WonderMaker PLA Basic`
7. 点击“切片单盘”，进入预览并确认“导出 G-code 文件”可用。
8. 保存 Orca 3MF 项目；重开项目，确认模型和配置保留。

### 图生 3D 备选路径

1. 选择图片生成。
2. 参考图：`resources\web\model\img\p1.png`。
3. 固定指令：`Create one clean, watertight and printable model with stable proportions and no thin floating details.`
4. 审核后确认生成。历史实测约 2 分 53 秒。
5. 后续导入、摆放、切片与保存步骤同文生路径。

## 已验证基线

- 文生产物：`.workbuddy\core-demo-real-20260808\text\text-191806d4-5b3b-4f51-a1a5-b5803049b0d5.3mf`
- 图生产物：`.workbuddy\core-demo-real-20260808\image\image-6a95b1b1-3e71-44da-83c1-5a5414a3aca2.3mf`
- 可重开 Orca 项目：`.workbuddy\core-demo-gui-20260808\roundtrip-text.3mf`
- 文生模型 GUI 切片结果：1249 层，44.09 g PLA，预计打印时间 6 小时 47 分 40 秒。
- GUI 切片实测约 14 秒完成 G-code 导出；状态码为 0，`psGCodeExport=1`。

## 灾备策略

1. 如果 readiness 检查失败，不创建付费任务；先确认 sidecar 端口和环境变量配置。
2. 如果现场网络或 provider 不可用，明确说明“在线生成当前不可用”，直接导入上面的已验证真实产物，继续演示摆放、切片、预览和项目保存。
3. 如果文生等待过久，切换到已验证文生产物；若需要继续展示在线能力，可改走历史更快的图生路径。
4. 不使用 `Default Printer` 切片。该配置的相对挤出校验会因缺少每层 `G92 E0` 而失败。
5. 不用 CLI 重开 round-trip 3MF 后切片。该路径存在已知 `0xc0000005`；GUI 重开和切片已验证通过，不受影响。
6. 不重复点击“生成”。所有真实调用都必须由操作人明确确认，避免重复计费。

## 演示后验收

- 在线任务显示真实 job 状态并到达 `ready`。
- 产物成功导入且存在一个可打印实例。
- 自动摆放完成，`model_fits=1`。
- 切片成功进入预览，“导出 G-code 文件”启用。
- 3MF 项目保存并可在 GUI 中重开。
- 若发生降级，现场表述与界面状态一致，不把原始输入降级描述为 OpenAI 成功。
