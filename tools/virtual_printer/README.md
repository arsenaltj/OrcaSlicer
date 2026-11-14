# Honor Forge Virtual Printer 0.1.0

用于 Honor Forge / Moonraker 软件联调的独立虚拟 3D 打印机。Windows 10/11 x64 安装版和便携版均自带 Python 运行时，离线可用，同事电脑无需安装 Python、Node 或开发工具。

## 使用

1. 安装 `win64-setup.exe`，从桌面或开始菜单打开“Honor Forge 虚拟打印机”；或解压便携 ZIP 后运行 `HonorForgeVirtualPrinter.exe`，保留整个目录。
2. 浏览器自动打开中文中控。关闭网页后后台、打印和摄像头继续运行；托盘菜单可重新打开中控或退出后台。
3. 新版客户端在“Honor Forge”设备通道扫描本机设备。尚无本机发现补丁的客户端，可手动添加中控显示的 `127.0.0.1:端口`，无需密钥。使用匹配的 Klipper 打印预设，关闭 `use_3mf`，上传原始 G-code。
4. 在总览加载示例后，从“文件与任务”开始打印；也可以在客户端选择“仅上传”或“上传并打印”。默认 10×，可切换 1× / 50×。

默认本机 `http://127.0.0.1:7125`，占用时自动选择 7126–7135。重复启动只打开已有中控。监听声明在当前用户目录，客户端仍通过 `/server/info` 校验 UUID。运行配置、上传文件、历史和独立 UUID 均保存在 `%LOCALAPPDATA%\HonorForge\VirtualPrinter`。便携版也使用此目录，复制软件到另一台电脑会建立独立身份。

后台的 HTTP 就绪和已有实例探测始终直连声明中的本机端口，不使用系统／环境代理、不跟随重定向，同时验证设备标记和 UUID；不修改其他请求的代理设置。

局域网共享默认关闭。在“配置与诊断”启用后，用显示的局域网地址连接，也广播 `_moonraker._tcp`；Windows 防火墙需允许该程序的专用网络入站。局域网仅开放打印机协议和摄像头，中控管理保留本机访问。网络设置切换会短暂重连。开机启动需在发行版中显式启用，默认关闭。

HTTP 和两个 WebSocket 入口共用连接校验：中控要求本机连接及 loopback／`localhost` 地址；局域网协议额外接受数字 IP 和当前设备广播的 `hf-virtual-<UUID前12位>.local` 名称。Host 必须指向实际监听端口，浏览器 Origin 必须与获准地址同源；任意域名不能凭自身同值 Origin 获得访问权限。不带 Origin 的原生协议客户端仍可按网络模式连接，重复或畸形的 Host／Origin 会被拒绝。

## 硬件与仿真

- 默认 256×256×256 mm、1 物理喷头、4 料槽；支持 1–8 喷头、0–16 料槽、1–32 工具映射，喷头与料槽独立。
- 三个模板：单喷头直连、单喷头多色、多独立喷头。名称、尺寸、喷嘴直径、温度上限、槽位材料/颜色/余量与映射可配置。导入配置不会复制安装身份；只有空闲时能保存硬件配置。
- 槽位颜色仅接受 `#` 加六位 ASCII 十六进制字符，大小写均可。含符号、空白、下划线或非 ASCII 数字的颜色在校验／保存时拒绝，保留原内存配置和已保存文件。
- 实际保存 G-code；流式执行 G0/G1/G2/G3（XY 平面）、G28、G90/G91/G92、M82/M83、Tn、调温/等待、风扇、暂停/恢复/取消和虚拟调平。工具切换按配置映射。文件未支持的宏明确记入“unsupported”诊断，手工未知命令返回错误。
- 位置、温度、耗材、层数和画面由同一个仿真时钟推进。进度采用已读取字节，层数优先切片注释、其次挤出 Z 高度；剩余时间优先切片头估算。它们用于软件联调，不是机械运动学或实体打印时长预测。圆弧不含非 XY 平面和固件专属运动学。
- 后台以 640×360 / 5 FPS 生成等轴测画面，支持 MJPEG 与 JPEG 快照；长任务路径累积到固定大小画布。客户端显示活动喷头，中控展示全部喷头。
- 异常包括离线、上传失败、存储满、缺料、温度异常、摄像头中断。离线期间本机中控仍可恢复。清除缺料后需要手动恢复；温度异常终止任务。重启后台保留任务历史，将未完成任务标记为中断，由用户重新开始。

## 接口与所有权

`hf_virtual_printer/session.py` 中的 `VirtualPrinterSession` 唯一持有设备、任务、文件与仿真状态。FastAPI、WebSocket、浏览器与摄像头消费同一状态；桌面 `DeviceManager` / Job 仍走现有 Honor Forge / Moonraker 通道。

| 范围 | 接口 |
|---|---|
| 识别与状态 | `/server/info`、`/printer/info`、`/printer/objects/list`、`/printer/objects/query` |
| 订阅 | `/websocket` 的 `server.connection.identify`、`printer.objects.subscribe`、`notify_status_update` |
| 文件 | `/server/files/{roots,list,directory,metadata,upload}`、`/server/files/gcodes/{filename}` |
| 打印 | `/printer/print/{start,pause,resume,cancel}`、`/printer/gcode/script` |
| 耗材 | `/server/database/item?namespace=lane_data` |
| 摄像头 | `/server/webcams/list`、`/webcam/?action=snapshot`、`/webcam/?action=stream` |
| 中控 | `/sim/api/v1/{state,config,templates,action}`、`/sim/events` |

这是面向当前客户端调用的 Moonraker 子集，不是通用 Moonraker 固件替代品。上传只支持文本 `.gcode` / `.gco` / `.g`，单文件最多 1 GiB。加热、归零、移动和调平为仿真行为。

## 升级、卸载与诊断

升级或卸载前先从托盘退出。安装器按用户安装，不请求管理员权限；升级复用数据，卸载保留上述用户目录。需要备份时复制该目录；不同电脑请导入配置文件，保留各自独立的 `identity.json`。

中控可导出诊断，后台滚动日志是用户目录下 `virtual-printer.log`。端口范围耗尽会提示错误。摄像头黑屏先检查异常开关、连接地址和实际端口。发现失败可手动添加地址；旧客户端不会自动读取运行声明。不要同时启动多个指向相同数据目录的版本。

## 开发与打包

Python 3.12 x64，依赖固定在 `requirements-lock.txt`，包含运行和验证工具。源码入口：

```powershell
python -m pip install -r tools/virtual_printer/requirements-lock.txt
python tools/virtual_printer/run.py
python -m unittest discover -s tools/virtual_printer/tests -v
python tools/virtual_printer/build_package.py --output D:/path/to/empty-staging
```

打包需 PyInstaller 和官方 NSIS，输出 self-contained onedir、安装 EXE、便携 ZIP、源码清单、源码快照、SHA-256 和构建日志。脚本会以移除开发工具的 PATH 验证冻结程序的真实 HTTP/WebSocket；验证失败不交付。经核验后才按工作区规则替换 `packages/current`。

可用 `--headless --no-browser --data-dir <隔离目录> --runtime-file <声明路径>` 做隔离验证。客户端仅在 QA 时设置 `ORCASLICER_VIRTUAL_PRINTER_RUNTIME` 指向相同声明。`--check-running` 返回 2 表示该数据目录已有后台，否则为 0，供升级器使用。

源码回归覆盖颜色保存／重开／摄像头、HTTP／WebSocket 的可信地址和来源、局域网协议与中控隔离，以及有代理环境下的本机就绪、错误 UUID 和重定向拒绝。真实网络测试启动独立数据目录和 headless 进程，不控制用户实例；源码通过不代表旧安装包已含补修，冻结程序与干净系统仍需对应版本验证。

本机自动测试、浏览器、匹配版本主窗口分别记录。干净 Windows 10/11 标准用户安装和第二台实体电脑的安装/局域网仍需实际环境验收；本机构建与冻结程序测试不能替代这些结论。实体打印精度和色彩不在本仿真软件验收范围。
