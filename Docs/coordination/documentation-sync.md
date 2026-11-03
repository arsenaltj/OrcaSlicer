# PR文档同步

文档与相关代码在同一PR中维护。`AGENTS.md`保留长期执行原则，工程导航负责定位，平台说明/review-map负责架构，模块文档负责契约与用法，主计划/短状态负责进度。进度或历史记录不能代替维护文档。

## 提交者怎样做

1. 明确本次改变了哪些行为、状态所有者、接口、格式、配置或开发命令，沿[工程导航](../AI_ENGINEERING.md)和[平台入口](../architecture/PLATFORM_ARCHITECTURE.md)找对应文档。
2. 更新已有说明；涉及架构语义/路径时维护review-map，并运行`python scripts/architecture_review.py --readme update`。新增长期架构取舍用已有ADR体系，普通实现调整不增加一份报告。
3. 填写PR模板的“文档同步”，选择一项、列出本PR实际修改的维护文档，并说明与改动的关联。没有文档影响时说明理由。
4. 复核人确认文档内容与代码一致，特别关注选择“无需更新”的接口、状态与运行配置变更。CI核对声明和文件差异，不能代替这一步。

已更新的填写示例：

```markdown
## 文档同步

- [x] 已同步相关文档
- [ ] 无需更新维护文档

文档路径：
- `Docs/architecture/PLATFORM_ARCHITECTURE.md`

说明：候选的接受/放弃检查点由会话拥有，已更新职责、恢复条件及验证入口。
```

无需更新的填写示例：

```markdown
## 文档同步

- [ ] 已同步相关文档
- [x] 无需更新维护文档

文档路径：

说明：仅替换内部临时缓存实现，公共接口、用户操作、配置及开发命令保持一致。
```

路径按Git实际大小写，用反引号逐项列出。说明不能只填“无”“不适用”“待补充”等占位文字。

## 本地检查

从仓库根执行，只需Python标准库和Git：

```text
python scripts/check_documentation_sync.py --base HEAD --worktree
```

该命令读取暂存、未暂存及未跟踪变更，列出实际更新和候选文档；结果`REVIEW_NEEDED`是范围提示，不表示PR声明通过。它不会修改文档或Git、调用应用/模型服务。

检查一条提交线及准备好的PR正文：

```text
python scripts/check_documentation_sync.py --base origin/codex/team/integration --head HEAD --pr-body-file .tmp/pr-body.md
```

基于两端merge-base计算本PR自身差异，排除目标分支单独前进的文档变更。返回`PASSED`才表示声明与本次Git差异一致；错误基点/缺历史对象会失败，不自动fetch或跳过。

## CI检查什么

[只读工作流](../../.github/workflows/documentation-sync.yml)运行`Documentation update check`，监听PR创建、追加提交、重新打开、转为待审及修改说明事件。显式包含`edited`，使补填说明后也能重验；事件语义见[GitHub官方说明](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request)。

每个PR都检查声明，不按文件路径跳过：

- 必须有且只有一个“文档同步”段落，勾选且只勾选一项，并填写说明；注释或代码块中的示例不算声明。
- 选择已同步时，列出的每个文档必须在该PR中有实际内容变化；纯改名、删除、换行符转换不算内容更新。
- Markdown/rST/AsciiDoc及review-map可作为维护文档；主计划、短状态、历史、审计和报告只作进度/证据，不作为该声明的内容更新证明。
- 选择无需更新时，保留具体理由；若PR已有维护文档内容变更，则应选择已同步并列出路径。
- 候选文档只帮助定位，不要求机械修改所有候选；检查不会推断所有业务影响或自动生成准确说明。复核人负责相关性和语义。

检查读取PR事件JSON和Git对象，不把PR正文拼进Shell执行；只写Actions日志/步骤摘要，不向分支提交、不发PR评论，也不需要仓库写权限或业务凭据。Fork PR使用同一只读路径，不使用`pull_request_target`执行PR代码。

## 启用与维护

脚本、测试、工作流、PR模板与本说明应一起纳入团队分支；已有PR需按团队SOP同步新基线并补填声明。首次远端生效以真实PR的Actions结果为准，本地通过不表示已在GitHub运行。若要强制作为合入门槛，维护者在现有分支保护/规则集中将`Documentation update check`设为必需；添加工作流文件本身不等于修改了分支保护。

维护源为[scripts/check_documentation_sync.py](../../scripts/check_documentation_sync.py)，回归为[test_documentation_sync.py](../../scripts/test_documentation_sync.py)。已有架构映射/README、集成与发布门禁继续各自验证；本检查不替换它们。

```text
python -m unittest discover -s scripts -p test_documentation_sync.py -v
```
