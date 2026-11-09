# 2026-11-11 当前：高级四视角 Tripo 错误融合已定位，整改待授权

主线：UX/ADVANCED-MULTIVIEW-20261107 / 本会话 / codex/team/integration；完整授权、任务与证据见主计划同编号。

本轮：Tripo官网确认当前命名对象数组及`front/left/back/right`顺序正确；鼠标和萌宠两份真实收据均含四个不同输入哈希，排除只传主图／右图。官网未明确`subject-left`／`camera-left`措辞，但白杯真实任务输入与返回模型的左右把手关系逐项一致，排除当前固定左右反标。独立五视图确认重复正面已经存在于供应商返回几何。可控主因是高级链路把任意主参考直接当语义`front`，且缺少共同相机、画布／尺度和跨视角刚体一致性归一／门禁；供应商在矛盾输入下的融合鲁棒性是共同风险。

未结：待授权实施Tripo canonical四视图归一和一致性门禁；官网未列出的`enable_image_autofix=false`及`.com`／`.ai`端点漂移需同时收敛但不是已证实根因。既有MV-UPL-02跨窗拖入、MV-LAY-06其他真实DPI、YOYOClaw协议、图像ambiguous对账、sidecar认证限制和4项集成diff预算基线保持。

下一步：用户确认整改范围后，在现有套图内增加自动相机／尺度归一、跨视角一致性门禁和失败视角待重新生成；复用当前禁用3D及单项重试，不新增隐式付费重试。

计划变更：新增只读根因记录及证据`.tmp/dev/review/20261110-tripo-multiview-root-cause/{mouse,pet}`和白杯左右语义对照`.tmp/dev/review/20261110-tripo-left-right-semantics/cup/model-view-sheet.png`；离线契约回归126／126、16／16、26／26通过。根因分析未改产品代码、未启动GUI或调用GPT／Tripo；2026-11-11按用户授权将前轮同组编辑实现、测试及协调记录通过`git commit --amend --no-edit`合入最近提交`feat(ai): add advanced multiview creation workflow`，未新增提交记录、未推送或创建PR，Tripo整改仍待授权。
