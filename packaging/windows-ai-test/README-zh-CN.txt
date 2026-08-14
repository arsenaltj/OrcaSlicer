OrcaSlicer AI Windows 测试包
============================

适用范围
--------
- Windows 10/11 x64
- 当前为内部测试版，请不要用于正式生产打印
- 仅验证 Windows；macOS/Linux 不在本测试包范围内

首次使用
--------
1. 将整个压缩包解压到本地磁盘，路径尽量不要过长。
2. 安装 Python 3.10 或更高版本，并在安装时勾选“Add Python to PATH”。
3. 双击“01-configure-ai.bat”，它会用记事本打开 `setup\ai-config.bat`。
4. 只填写下面两行等号后的 Key，然后保存：
   - `set "OPENAI_API_KEY=这里填写Key"`
   - `set "TRIPO_API_KEY=这里填写Key"`
5. 其他地址和模型名称已经带默认值，通常不需要修改。
6. 双击“02-check-environment.bat”。
7. 检查通过后，双击“03-start-orcaslicer-ai.bat”。

日常启动
--------
- 必须使用“03-start-orcaslicer-ai.bat”，不要直接双击 OrcaSlicer 目录中的 exe。
- 启动器会先检查/启动本地 AI sidecar，再启动 OrcaSlicer。
- 生成结果和临时文件保存在本测试包的 generated_models 目录。
- 关闭 OrcaSlicer 后，可运行“04-stop-ai.bat”停止本地 AI 服务。

外部 API 配置
--------------
- OpenAI-compatible 默认 Base URL：https://laotie.dev
- 默认文本模型：gpt-5.4
- 默认图片模型：gpt-image-2
- Tripo 默认 API Base：https://openapi.tripo3d.com/v3
- Tripo 默认模型：v3.1-20260211
- API Key 不包含在测试包中，请通过安全渠道单独取得。
- Key 只写在当前解压目录的 `setup\ai-config.bat` 中，不修改 Windows 用户环境变量。
- 填写真实 Key 后，不要再次转发该配置文件或整个已解压目录。
- 不要把 API Key 发到群聊、截图、日志或问题报告中。

费用说明
--------
- “02-check-environment.bat”只检查本地依赖和 sidecar 健康状态，不创建图片或 3D 付费任务。
- 生成 AI 风格预览可能消耗图片 API 额度。
- 点击确认生成 3D 模型会消耗 Tripo 额度。
- 请只在明确需要时确认付费操作。

主要测试流程
------------
1. 打开“3D 生成”。
2. 输入文字、选择图片，或两者同时使用。
3. 先生成并检查原图/AI 处理图。
4. 确认后生成 3D 模型，并在本页旋转、缩放预览。
5. 导入时优先选择“手动匹配打印机耗材（推荐）”。
6. 检查打印机实际耗材颜色与映射是否一致。
7. 可选择自动切片，或停在准备页手动处理。

已知限制
--------
- 当前是开发快照，没有数字签名，Windows 可能显示未知发布者提示。
- 模型色不等于打印机实际颜色，导入时必须核对耗材槽。
- 自动修复无法保证修复所有非流体/开放网格；失败时应手动检查。
- 账号、权益和计费系统尚未接入。
- 模型生成任务的跨进程恢复能力仍有限。

反馈时请提供
------------
- 操作步骤和失败发生在哪一步
- OrcaSlicer 页面截图（务必遮挡个人信息）
- 模型任务目录名称
- 是否开启“使用可打印颜色”和“导入后自动切片”
- 导入颜色模式
- 请勿发送 API Key
