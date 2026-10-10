# UX 分支：Windows 人像依赖与同事构建指南

更新：2026-10-09。适用分支：`codex/team/integration`。

当前默认版本使用自动分区、分区填色与画笔，不携带重型人像识别资源，普通 Windows AI 构建不需要下载下列约 1 GB 附件。`ORCA_AI_PORTRAIT_RECOGNITION` 默认 OFF；安装能力清单同时关闭识别，旧运行文件或历史配置不能自动重新启用。

以下仅用于显式研究这套实验人像算法。它在本机单人样本耗时约 22 分钟，尚未达到 ≤60 秒和 ≤100 MB 的产品约束；不作为默认交付。继续实验时须同时明确设置 `-DORCA_AI_PORTRAIT_RECOGNITION=ON` 并准备完整固定依赖，不能删减完整性检查。

此文解决首次构建报 `Offline portrait packaging requires verified weights and local CPU site-packages`、缺少人像权重或 CPU Python 包的问题。人像识别在本地运行，不需要配置 Tripo 等云端 Provider；云端生成的服务配置是另一项工作。

## 1. 下载什么

源码通过 Git 同步；大型依赖通过固定版本的 GitHub Release 附件分发。分支中的 [依赖描述文件](../../cmake/portrait_dependencies.json) 固定下载地址、字节数和 SHA-256。**不要使用其他开发者的 CMakeCache.txt 或硬编码的 `D:/TEST/...` 路径。**

- [依赖版本与附件](https://github.com/arsenaltj/OrcaSlicer/releases/tag/portrait-deps-win-x64-cp312-20261009-r1)
- [直接下载 ZIP](https://github.com/arsenaltj/OrcaSlicer/releases/download/portrait-deps-win-x64-cp312-20261009-r1/portrait-deps-win-x64-cp312-20261009-r1.zip)
- [附件 SHA-256](https://github.com/arsenaltj/OrcaSlicer/releases/download/portrait-deps-win-x64-cp312-20261009-r1/SHA256SUMS)

依赖包适用于 **Windows x64、CPython 3.12、CPU**，包含：

| 内容 | 说明 |
| --- | --- |
| 四个权重 | `mobilenet0.25_Final.pth`、`face_parsing.farl.celebm.main_ema_181500_jit.pt`、`face_landmarker.task`、`selfie_multiclass_256x256.tflite` |
| Python 与依赖 | Python 3.12.13、PyTorch 2.5.1 CPU、torchvision 0.20.1 CPU、pyfacer、MediaPipe、SciPy 及已验证的传递依赖 |
| 三个固定 wheel | NumPy 2.2.6、OpenCV headless 4.10.0.84、Pillow 12.2.0；初始化时复制到本构建目录的 wheel 缓存 |
| 原生识别组件 | `native-semantic/` 中的 MediaPipe DLL、模型和许可说明 |
| 文件清单 | `dependency-manifest.json` 记录每个文件的大小、哈希和 Python 包版本 |

包内保留第三方组件自带的许可证和 NOTICE。此依赖附件不是 Orca 安装包，也不包含个人配置、API Key、用户模型、历史资产或开发者构建缓存。**应用算法模块及 `local_semantic_raster.dll` 从当前源码构建并部署，不从此包复制。** 普通 `git pull` 不会下载这些大型附件。

## 2. 新电脑首次准备

准备 Git、Visual Studio 2022（C++ 桌面开发及 Windows SDK）、CMake（本次验证为 3.29.2）、项目所需 Perl/Gettext 和可从 `py -3.12` 调用的 Python 3.12。依赖准备脚本只用 Python 标准库，不需要先给系统 Python 安装 PyTorch 等包。

在 PowerShell 执行：

```powershell
git clone --branch codex/team/integration --recurse-submodules https://github.com/arsenaltj/OrcaSlicer.git
cd OrcaSlicer
py -3.12 .\scripts\portrait_dependencies.py prepare
```

已有源码的同事在自己的分支妥善提交或保存改动后同步远端，再运行 `prepare`；不要为执行本文而丢弃本地改动。没有 `py` 启动器时可用 Python 3.12 的完整可执行文件路径替代。

脚本默认下载到 `.tmp/dependency-downloads/`，校验压缩包后，在临时目录解压并校验全部文件，使用包内 Python 执行隔离 CPU 导入检查。全部成功才启用 `.tmp/portrait-dependencies/` 并生成其中的 `portrait-dependencies.cmake`。重新执行会检查现有目录，不自动覆盖损坏或不同版本的依赖。

如果浏览器下载附件更方便，或开发机离线，可把 ZIP 复制到本机并执行：

```powershell
py -3.12 .\scripts\portrait_dependencies.py prepare --archive "D:\Downloads\portrait-deps-win-x64-cp312-20261009-r1.zip"
```

也可用 `--destination D:\OrcaDeps\portrait-20261009-r1` 指定位置。下文的初始缓存路径相应改为该目录。上述离线方式只覆盖本包；**首次编译 Orca 的 C++ 依赖仍需其各自的下载或现有缓存。**

## 3. 首次编译与完整运行目录

继续在仓库根目录执行。以下命令使用当前仓库已有的 `build_win.bat`，为本 checkout 单独构建 C++ 依赖和应用：

```powershell
$portraitInit = (Resolve-Path .tmp/portrait-dependencies/portrait-dependencies.cmake).Path.Replace('\', '/')
$env:ORCA_SLICER_CMAKE_ARGS = '-C "' + $portraitInit + '" -DORCA_AI_WINDOWS_INSTALLER=ON -DORCA_AI_DISTRIBUTION_CHANNEL=internal -DORCA_AI_PORTRAIT_RECOGNITION=ON'
.\build_win.bat -ds -i --vs 2022 --arch x64 --config release --deps-dir "$PWD/.tmp/dev/deps-build" --build-dir "$PWD/.tmp/dev/build" -j 2
if ($LASTEXITCODE -ne 0) { throw '构建失败，请保留日志并查看最早的具体错误。' }
```

此环境变量仅作用于当前 PowerShell 会话；如果已有其他 `ORCA_SLICER_CMAKE_ARGS`，请合并保留所需参数。`$PWD` 将构建目录传为本机绝对路径，避免依赖子目录解析相对路径。路径应避开 `&` 和 `!` 等批处理特殊字符。`-j 2` 限制编译并行度，可按内存调整。不要给首次构建加 `--no-configure`。

初始缓存会设置本机的三个目录，并准备本构建目录的 wheel 缓存：

```text
ORCA_AI_WEIGHTS_DIR                 → <依赖目录>/weights
ORCA_AI_PORTRAIT_SITE_PACKAGES      → <依赖目录>/python/Lib/site-packages
ORCA_SEMANTIC_RUNTIME_DIR           → <依赖目录>/native-semantic
ORCA_AI_PYTHON_STABLE_ABI_DLL       → <依赖目录>/python/python3.dll
```

默认安装结果在 `.tmp/dev/build/OrcaSlicer/`；若已有缓存自定义了 `CMAKE_INSTALL_PREFIX`，以该路径为准。也可以显式安装到新的完整目录：

```powershell
cmake --install .tmp/dev/build --config Release --prefix .tmp/dev/team-run
& .\.tmp\dev\team-run\resources\beauty-runtime\python\python.exe -I -B .\.tmp\dev\team-run\resources\beauty-runtime\modules\bundled_portrait_runtime.py --root .\.tmp\dev\team-run\resources\beauty-runtime
```

成功后从完整目录启动 `orca-slicer.exe`。需要隔离测试配置时：

```powershell
& .\.tmp\dev\team-run\orca-slicer.exe --datadir "$PWD\.tmp\dev\team-data"
```

不要只复制 EXE/DLL；缺少资源、sidecar 或 `resources/beauty-runtime` 的目录不算完整候选。不要在同一路径的 Orca 或 Python 仍运行时覆盖其运行库。

## 4. 已经有 C++ 构建缓存的同事

下载并准备依赖后，给**自己的原构建目录**重新配置即可，无需重建其他同事的目录：

```powershell
cmake -S . -B .tmp/dev/build -C .tmp/portrait-dependencies/portrait-dependencies.cmake -DORCA_AI_WINDOWS_INSTALLER=ON -DORCA_AI_DISTRIBUTION_CHANNEL=internal
cmake --build .tmp/dev/build --config Release --target OrcaSlicer_app_gui --parallel 2
cmake --install .tmp/dev/build --config Release --prefix .tmp/dev/team-run
```

日常源码更新仍从 Git 获取；同一依赖版本可复用。分支更新了依赖描述文件后，下载新附件到独立版本目录，重新配置。旧依赖保留作为回退，不把损坏缓存当作可以跳过校验的理由。

`dev.ps1 Setup` 当前没有专门的人像依赖参数。新机器请先按上文配置，之后才能使用 `dev.ps1` 的现有增量工作流。内部安装包仍使用 [release/README.md](../../release/README.md) 的打包入口和检查规则；本指南不绕过其检查。

## 5. 常见错误

| 错误 | 处理 |
| --- | --- |
| `Offline portrait packaging requires...` | 确认运行过 `prepare`，且本次 CMake 配置带了生成的 `-C` 文件。仅创建两个空目录不够。 |
| 下载被代理阻断或超时 | 用浏览器下载同一版本 ZIP，再通过 `--archive` 校验准备；不要关闭 TLS 或哈希检查。 |
| `Dependency ... mismatch` | 文件与固定版本不一致。保留错误文件用于排查，换新下载目录或独立目标目录，不手改清单绕过检查。 |
| CPU 导入失败、DLL 找不到 | 检查 Windows x64、VC++ 运行库和安全软件隔离记录；保留具体模块名。不要混入 CUDA 或 ARM64 包。 |
| `model_hash_mismatch` | 四个权重需与当前分支固定哈希一致；相似名称或其他版本不能替代。 |
| `Could not find ORCA_AI_PYTHON_STABLE_ABI_DLL` | 更新本指南对应脚本并重新执行 `prepare` 和带 `-C` 的配置；初始缓存会指定包内已校验的 `python3.dll`。 |
| CMake 指向其他人的绝对路径 | 重新运行 `prepare` 生成本机初始缓存，再对自己的构建目录配置。 |
| `architecture_budget_exceeded` 等架构检查 | 与依赖缺失分开处理；遵守原有记录/审批规则，本脚本不会放宽检查。 |

## 6. 维护依赖版本

依赖和算法分开版本化。维护者从已验证运行库创建依赖包，执行解压校验、CPU 导入、CMake 配置与内容检查后上传到新的固定版本附件；把对应的 `cmake/portrait_dependencies.json`、本指南及准备脚本提交到 UX 分支。

不要覆盖旧 Release 的同名包。新版本使用新的标签和文件名。发布后核对远端附件的字节数和 SHA-256，再提供同事使用的文档链接。准备脚本支持 `pack --help`，其输入仅接受清单列出的运行库文件；用户配置和当前算法不进入依赖包。

本轮验证记录见依赖 Release 说明。脚本/依赖检查、完整应用构建、主窗口功能与人像视觉验收分别记录；依赖导入成功不代表生成、裁切或切片流程已经验收。
