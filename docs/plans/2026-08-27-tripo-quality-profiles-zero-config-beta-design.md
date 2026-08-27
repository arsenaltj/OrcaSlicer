# Tripo 双档质量策略与零配置 Beta 设计

## 目标

模型生成页只保留两个用户可理解的策略，不再要求用户选择三角面数。高质量档最大化 Tripo H3.1 的几何与纹理能力；高性能档不牺牲基本外观，只减少昂贵的细节计算和生成阶段处理。内部 Beta 安装后应双击即用，不要求同事编辑批处理文件。

## 两档策略

| 用户选项 | Tripo 参数 | 内部面数上限 | 说明 |
|---|---|---:|---|
| 高质量（推荐） | `v3.1-20260211`, `geometry_quality=detailed`, `texture_quality=detailed`, `texture=true`, `pbr=true`, `export_uv=true` | 1,000,000 | Ultra 几何、高清纹理和完整 PBR；等待、下载和后处理成本更高 |
| 高性能 | `v3.1-20260211`, `geometry_quality=standard`, `texture_quality=standard`, `texture=true`, `pbr=true`, `export_uv=false` | 300,000 | 保留完整可见外观，减少几何、纹理和 UV 阶段成本 |

两档都使用 `texture_alignment=original_image` 保持原图颜色与外观。两档都不使用 `quad`，因为它会强制 FBX，与当前 OBJ 打印链路不兼容；也不默认启用 `smart_low_poly`，因为官方提示复杂模型可能失败。

`face_limit` 按官方语义仅作为上限。生成模型低于上限不再被判失败，结构完整性、薄壁、悬垂和可打印性继续由本地质量门禁判断。

## 数据流与兼容性

C++ 向 Sidecar 发送 `generation_profile=quality|performance`。Sidecar 将策略映射为受控面数上限，并在 Job 状态、磁盘恢复文件、公开状态和模型库元数据中保存该字段。Provider Gateway 校验策略与面数映射后，Tripo Client 生成最终请求。

旧客户端仍可只发送 `face_limit`：50 万及以上映射为高质量，其余映射为高性能。旧磁盘任务使用相同规则恢复，不修改已有远端任务引用，不产生自动付费重试。

## 零配置 Beta

优先方案是远端内部网关：安装包只携带可撤销、限额、限定来源的内部访问令牌，OpenAI/Tripo 主密钥只存在于网关。当前仓库没有可部署的统一网关，Tripo 仍由本地 Sidecar 直连官方 API。

若要求本次立即零配置出包，只能使用专门的内部 Beta Key 写入安装载荷。该 Key 必须单独创建、设置严格额度、可随时撤销，并在 Beta 后轮换；不得使用长期生产主密钥。无论采用哪种方案，安装器和日志都不显示 Key，覆盖升级保留本地配置。

## 验证

- Tripo 请求单元测试覆盖两档的几何、纹理、PBR、UV 和面数映射。
- Gateway/Sidecar 契约测试覆盖非法策略、策略与面数不一致、旧请求兼容和 Job 恢复。
- Python 全量回归、Windows Release 构建、真实 GUI 两档可见性验证。
- 最终安装包做安装、零配置启动、Sidecar 健康检查和卸载冒烟；不创建付费 Image2/Tripo 任务。
