# OBJ 资源包、颜色与本地产物设计

## 目标

图生或文生 3D 完成后，Tripo 返回的 OBJ 资源包能够被正确识别、保留和导入。所有参考图、风格预览、原始下载、解压资源和最终导入文件都位于项目当前目录，不再写入系统临时目录或在退出时删除。

## 数据目录

启动器设置 `ORCASLICER_AI_OUTPUT_DIR=<项目根目录>\generated_models`。sidecar 为每个任务创建 `<job-id>` 子目录，保存输入图片、AI 预览、`artifact-raw.zip`、`package/` 原始 OBJ/MTL/贴图，以及 `model-vertex-color.obj`。GUI 下载副本写入 `generated_models/downloads/`。删除远端任务只清除内存状态，不删除磁盘产物。

## OBJ 处理

下载后先检查文件签名。纯顶点色 OBJ 沿用严格校验；ZIP 资源包则执行路径穿越、符号链接、文件数量、单文件大小和总解压大小检查，然后解压到任务目录。转换器读取 OBJ 的顶点、UV 和面索引，从 MTL 的 `map_Kd` 找到底色贴图，按每个顶点引用的 UV 采样并平均颜色，输出只有 XYZRGB 顶点与几何面的独立 OBJ。

## 颜色语义

原始 MTL/纹理资源完整保留，便于外部 DCC 工具使用。OrcaSlicer 当前未启用图片纹理 OBJ 导入，因此工作流导入的是顶点色 OBJ；进入 Orca 后继续使用现有颜色聚类与耗材映射对话框。该转换会把连续纹理近似为顶点插值颜色，适合当前多色打印演示，但不等同于渲染器中的高分辨率纹理。

## 验收

- Tripo ZIP 不再触发 UTF-8 报错。
- 非法 ZIP 路径、符号链接和超限内容被拒绝。
- 最终 OBJ 的每个几何顶点带 RGB，面索引不依赖 MTL/贴图。
- 真实失败产物可离线恢复，无新增付费调用。
- GUI 下载与退出后，`generated_models` 中的文件继续存在。
- Python 回归、Windows Release 构建和实际 Orca 导入通过。
