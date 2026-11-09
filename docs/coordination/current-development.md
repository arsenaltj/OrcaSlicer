# 2026-11-09 当前：MAINLINE-BUGFIX-001 合入主线并独立构建

主线：正式修复分支 task/mainline-bugfix，基于 8638fd3595 的人像 v12 与既有账号能力，同步团队主线 ea91b27324。

本轮：用户已授权拉取、合并解决冲突和独立编译验证入口；19 文件文本冲突已处理，正在检查接口与相关回归。

未结：合并后编译、运行树及入口尚未验证；用户视觉和打印不沿用为本次通过。

下一步：完成门禁和定向验证，在本 checkout 固定 .tmp/dev/build 首次配置/编译，准备独立 .tmp/dev/run 与 .tmp/dev/data 给用户。

计划变更：本轮正式承接构建和本地合并，原开发及模型库工作区保留；历史任务不自动续跑。详见主计划 MAINLINE-BUGFIX-001；以下为合入前主线历史状态。

# 2026-11-06 当前：Honor Forge 发现与连接已实机通过，只上传待人工

主线：UX/PRINTER-DISCOVERY-20261105 / 本会话 / `codex/team/integration` / `0f6891b3ab` 基线；本轮 46 个文件按用户授权形成一笔本地提交，准确提交 ID 以 Git HEAD 为准。

本轮：新 UX 打印确认已提供“导出到电脑／仅上传 G-code／上传并打印”三项，真实设备门禁要求 agent、设备 ID、在线、连接及 15 秒内状态上报；Honor Forge/Moonraker 发送强制使用当前盘 raw `.gcode`，缺失即失败，不回退 `.gcode.3mf`。发送继续复用 `SendJob`、`PrintJob` 和 Moonraker agent，PrintHost 保留原流程。定向回归 HonorForgeDiscovery 10/10、DeviceAccess 9/9、registry 3/3、PrinterWorkspace 12/12、MoonrakerPrinterAgent 16/16、PrinterDiscovery 5/5、UiRedesign 130/130、SmartSlicing/确认 334/334 通过；架构门禁和 diff 检查通过。最终 Windows `Run -NoLaunch` 五步通过，收据 `.tmp/dev/logs/20261106-113150-807/result.json`，源码身份 `c5e719e5458341e704b37ccb495561ee34dcf4ea87ba4a49d5dbc977ac4ce702`，EXE 为 `.tmp/dev/run/Honor Forge Studio.exe`。

未结：最新 EXE 已以 `.tmp/dev/data` 启动（PID 4868）。用户在当前窗口手动打开设备弹框后，`picker` 发现同时验证实际打印机 `10.80.218.120` 的 7125／80 端点并归并为 1 台，随后选择命中并调用 `DeviceManager::set_selected_machine()`；`server_info`、初始状态和 WebSocket 均成功，最终 `selected=true/online=true/connected=true/info_ready=true`。打印机正在上报已有作业 `RUNNING`，本轮没有产生 `[PrinterDispatch]` 或 `[PrinterTransfer]`，没有上传或启动新打印。Windows Computer Use 原生管道仍不可用，因此可见布局只能由用户手动观察。设备型号映射仍记录 `Unsupported printer type: Honor Forge` 及预设名更新失败，未阻断连接和状态上报，但机型专属消费者待继续观察。

下一步：用户确认设备弹框只显示归并后的 `10.80.218.120:7125` 且按钮／状态文案正确；待打印机当前作业完成或硬件同事确认上传不会干扰后，完成切片并只测试“仅上传 G-code”，不要点击“上传并打印”，回收 `[PrinterDispatch]`、`[PrinterTransfer]` 日志和打印机端文件结果。断线重连和进入打印页后再接 WiFi 继续人工验证。

计划变更：r5 ZIP 保留为上一发现阶段证据，不代表本轮新增三动作和 payload 修正；用户已明确本轮只交付开发 EXE，不新建安装包或 ZIP。实体发现、端点去重、选择、连接和状态上报已由实际 Honor Forge 设备日志通过；三按钮可见布局、只上传、断线重连和后接 WiFi 仍待用户反馈，上传并打印保持禁止验收。用户已授权本地提交；不推送或创建 PR。
