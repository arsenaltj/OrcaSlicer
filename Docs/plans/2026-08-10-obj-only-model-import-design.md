# OBJ-only 模型生成与导入设计

## 目标

3D Generate 工作流生成的三维产物必须以 OBJ 格式下载并导入当前盘面。非 OBJ 结果不能静默回退或进入导入流程。

## 方案

- sidecar 能力契约只声明 `obj`。
- Tripo 几何生成完成后只创建一次 OBJ 转换任务。
- OBJ 下载后继续执行文件大小、结构和顶点色校验。
- 转换或校验失败时任务进入 `failed`，保留具体错误，不尝试 3MF/STL。
- GUI capability discovery 只接受 OBJ-only 契约；下载前再次检查产物格式和顶点色编码。
- 底层客户端保留旧格式解析，避免破坏旧任务状态的诊断能力，但产品工作流不会导入旧格式。

## 验收

- health 返回 `artifact_formats: ["obj"]`。
- 成功任务仅调用 OBJ 转换并返回 `.obj`。
- OBJ 转换失败时不发生第二次格式转换。
- GUI 仅对合法的顶点色 OBJ 执行下载和导入。
- Python 回归、Windows Release 构建、安装同步和启动检查全部通过。
