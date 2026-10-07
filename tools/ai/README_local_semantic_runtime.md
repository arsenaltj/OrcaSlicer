# 本地人像语义运行时

识别运行时与颜色算法分开准备。应用只消费已准备的本地 CPU 环境，不安装依赖、不下载权重、不调用生成供应商。普通 Orca Python 无需 Torch；缺失、禁用或校验失败时继续使用已有颜色分区回退。

## 可复用环境

Windows x64 / CPython 3.12 使用 [锁文件](beauty_runtime_win_py312_cpu.lock)，包含官方 PyPI 发布版 `pyfacer==0.0.4` 及其漏声明的 `timm` 依赖，Torch/torchvision 固定 CPU 轮子。哈希只适用于该平台；更换平台或算法应另建并验证环境，不能去掉哈希后沿用验收。

在独立临时目录显式准备 wheel 和 site-packages，避免修改系统 Python：

```powershell
python -m pip download --only-binary=:all: --require-hashes --extra-index-url https://download.pytorch.org/whl/cpu -r tools/ai/beauty_runtime_win_py312_cpu.lock -d .tmp/beauty-wheels
python -m pip install --no-index --find-links .tmp/beauty-wheels --require-hashes -r tools/ai/beauty_runtime_win_py312_cpu.lock --target .tmp/beauty-site-packages --no-compile
```

权重的名称、大小与 SHA256 由 [worker](local_semantic_worker.py) 和 [body helper](local_body_regions.py) 固定。使用官方 [FACER 模型](https://github.com/FacePerceiver/facer)及 MediaPipe 模型；准备实际权重文件后，运行 `scripts/stage_beauty_runtime.py --python-root <完整Python3.12目录> --site-packages <准备好的site-packages> --weights <权重目录> --output <新的暂存目录>`。不要携带机器路径 `.pth`、用户资产或供应商配置。

构建环境完整时，将 `ORCA_BEAUTY_RUNTIME_ROOT` 配置为该暂存目录，再用现有 `dev.ps1 Run -NoLaunch` 安装。工作台自动读取 `resources/beauty-runtime`；数据目录已有 `local_semantic_runtime.json` 时尊重其显式配置，包括禁用决定。机器路径只写忽略的本地配置。

## 必须区分的验证

1. staging 的导入检查只证明依赖能加载。用 [原生宿主探针](../../tests/slic3rutils/test_local_semantic_worker_client.cpp) 的 `[LocalSemanticWorkerRealProbe]` 验证实际 CPU 推理、固定权重和精确标签顺序；`--identity-only` 不证明推理可用。
2. `[LocalSemanticWorkerRealMesh]` 用真实 GLB 验证 worker 输出与原生面顺序、来源和运行时身份。未知面允许保持未知，不以扩大标签覆盖率代替准确性。
3. GUI 单独验证原始模型、已有版本、识别更新、保色重分区、局部修色及保存恢复；自动识别和材料选择分别看图。原生探针不能替代 GUI、人工分区真值或实物打印。

官方 PyPI 版在推理结果中返回标签，无 `parser.label_names` 属性；未发现人脸时返回 `{}`。适配器仅统一这个空结果，完整探针和每个接受的网格视图仍检查实际标签、形状和有限值，不能给解析器填入预设标签来伪造通过。

## 与颜色和美颜的边界

原始纹理 → 有面对应证明的语义指导 → 编辑区域 → 材料候选，分别保持来源与版本。`更新五官识别（保留当前区域）` 保留既有编辑；`按识别整理编辑分区（保留颜色，可撤销）` 调整逻辑区域；`按五官分区校正用色（可撤销）` 仅处理选区。它们不应在识别更新时隐式重涂历史。

识别出银发并不保证六色最近色会选白色，识别出皮肤也不保证初始匹配拒绝唇红。继续演进材料用途约束和人工区域校验，保持已认可的色板目标及手工颜色；六色缺少的服装颜色和实物色差另行记录。
