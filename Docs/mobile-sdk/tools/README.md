# 需求文档验证工具

verify-requirements.cjs 从当前 worktree 读取 v1.7 文档，核对本地仓库目录、需求/边界、真实目录锚点，并可运行 1440/390 浏览器场景。所有检查从输入计算，失败不会被覆盖成通过。

~~~text
node verify-requirements.cjs --workspace <workspace> --output <new-evidence-directory> --browser --playwright-module <playwright-module>
node --test verify-requirements.test.cjs
~~~

每次输出新证据目录；旧 root 辅助脚本仅作为归档，不再作为当前执行入口。验证范围是文档/结构/页面，不替代原业务代码、真实设备或完整平台集成验收。
