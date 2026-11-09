"""Documentation policy regressions over real, isolated Git histories."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import check_documentation_sync as docs


ROOT = Path(__file__).resolve().parents[1]


def body(status=docs.UPDATED, files=("Docs/guide.md",), reason="已同步行为和失败恢复说明。"):
    return ("## 文档同步\n\n" +
            f"- [{'x' if status == docs.UPDATED else ' '}] {docs.UPDATED}\n" +
            f"- [{'x' if status == docs.NO_IMPACT else ' '}] {docs.NO_IMPACT}\n\n" +
            "文档路径：\n" + "".join(f"- `{p}`\n" for p in files) +
            "\n说明：" + reason + "\n\n## 验证与恢复\n说明其他验证。\n")


class DocumentationSyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="documentation-sync-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.environment = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                                GIT_TERMINAL_PROMPT="0")
        self.git("init", "-q", "--initial-branch=integration")
        for key, value in (("user.name", "Documentation Test"), ("user.email", "docs@example.invalid"),
                           ("core.autocrlf", "false"), ("commit.gpgsign", "false")):
            self.git("config", key, value)
        self.write("src/feature.py", "value = 1\n")
        self.write("Docs/guide.md", "# Guide\n\nExisting behavior.\n")
        self.write("Docs/plans/current.md", "# Progress\n")
        self.base = self.commit()

    def git(self, *args, input=None):
        result = subprocess.run(["git", *args], cwd=self.root, env=self.environment, input=input,
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", errors="replace"))
        return result.stdout.decode("utf-8").strip()

    def write(self, path, text):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-qm", "fixture change")
        return self.git("rev-parse", "HEAD")

    def review(self, text, *, base=None, head="HEAD", worktree=False):
        return docs.review(self.root, base or self.base, head, text, worktree=worktree)

    def code_change(self):
        self.write("src/feature.py", "value = 2\n")
        return self.commit()

    def test_missing_declaration_is_rejected_even_for_document_only_changes(self):
        self.write("Docs/guide.md", "# Changed guide\n"); self.commit()
        self.assertEqual("FAILED", self.review("## 影响与复核\n")['status'])

    def test_no_impact_requires_a_specific_explanation(self):
        self.code_change()
        for reason in ("", "无", "待补充", "N/A", "tbd"):
            with self.subTest(reason=reason):
                self.assertTrue(self.review(body(docs.NO_IMPACT, (), reason))['errors'])
        result = self.review(body(docs.NO_IMPACT, (), "仅调整内部缓存分配，接口和操作方式保持一致。"))
        self.assertEqual("PASSED", result['status'])

    def test_both_choices_and_neither_choice_are_rejected(self):
        self.code_change()
        for marker in ("[x]", "[ ]"):
            with self.subTest(marker=marker):
                text = body(docs.NO_IMPACT, ()).replace("[x]", marker).replace("[ ]", marker)
                self.assertTrue(self.review(text)['errors'])

    def test_updated_declaration_requires_real_document_content_changes(self):
        self.code_change()
        self.assertTrue(self.review(body())['errors'])
        self.write("Docs/guide.md", "# Guide\n\nUpdated behavior and recovery.\n"); self.commit()
        result = self.review(body())
        self.assertEqual("PASSED", result['status'])
        self.assertEqual(["Docs/guide.md"], result['changed_documents'])

    def test_progress_updates_do_not_substitute_for_maintained_documentation(self):
        self.write("Docs/plans/current.md", "# Progress\nTask completed.\n"); self.commit()
        self.assertTrue(self.review(body(files=("Docs/plans/current.md",)))['errors'])
        self.assertFalse(self.review(body(docs.NO_IMPACT, (), "仅维护进度记录，使用说明和接口没有变化。"))['errors'])

    def test_no_impact_does_not_hide_actual_maintained_doc_updates(self):
        self.write("Docs/guide.md", "# Updated\n"); self.commit()
        self.assertTrue(self.review(body(docs.NO_IMPACT, (), "仅更新文档。"))['errors'])

    def test_deleted_documents_cannot_be_declared_as_updated(self):
        (self.root/'Docs/guide.md').unlink(); self.commit()
        self.assertTrue(self.review(body())['errors'])

    def test_pure_renames_do_not_count_as_document_content_changes(self):
        self.git("mv", "Docs/guide.md", "Docs/renamed.md"); self.commit()
        result = self.review(body(files=("Docs/renamed.md",)))
        self.assertEqual([], result['changed_documents'])
        self.assertTrue(result['errors'])

    def test_renamed_and_edited_documents_are_valid_updates(self):
        self.git("mv", "Docs/guide.md", "Docs/renamed.md")
        self.write("Docs/renamed.md", "# Guide\n\nExisting behavior.\nNew recovery behavior.\n"); self.commit()
        self.assertFalse(self.review(body(files=("Docs/renamed.md",)))['errors'])

    def test_line_ending_only_changes_do_not_count_as_updates(self):
        self.write("Docs/guide.md", b"# Guide\r\n\r\nExisting behavior.\r\n"); self.commit()
        self.assertEqual([], self.review(body())['changed_documents'])

    def test_declaration_uses_exact_git_spelling(self):
        self.write("Docs/guide.md", "# Updated\n"); self.commit()
        self.assertTrue(self.review(body(files=("docs/guide.md",)))['errors'])

    def test_space_and_unicode_paths_survive_git_diff_and_pr_parsing(self):
        name = "Docs/配色 guide.md"
        self.write(name, "# 配色\n\n本次修改的说明。\n"); self.commit()
        result = self.review(body(files=(name,)))
        self.assertFalse(result['errors'])
        self.assertEqual([name], result['changed_documents'])

    def test_html_comments_and_fenced_examples_are_not_declarations(self):
        self.code_change()
        for wrapped in ("<!--\n" + body() + "\n-->", "```markdown\n" + body() + "\n```"):
            with self.subTest(wrapped=wrapped[:4]):
                self.assertTrue(self.review(wrapped)['errors'])

    def test_duplicate_documentation_sections_are_rejected(self):
        self.code_change()
        self.assertTrue(self.review(body() + "\n" + body())['errors'])

    def test_traversal_and_absolute_paths_are_not_document_proof(self):
        self.code_change()
        for name in ("../Docs/guide.md", "/Docs/guide.md", "C:/Docs/guide.md", "Docs\\guide.md"):
            with self.subTest(name=name):
                self.assertTrue(self.review(body(files=(name,)))['errors'])

    def test_symbolic_link_blobs_do_not_count_as_maintained_documents(self):
        blob = self.git("hash-object", "-w", "--stdin", input=b"another.md")
        self.git("update-index", "--cacheinfo", f"120000,{blob},Docs/guide.md")
        self.git("commit", "-qm", "symlink fixture")
        self.assertTrue(self.review(body())['errors'])

    def test_target_branch_doc_changes_do_not_count_as_feature_changes(self):
        self.git("checkout", "-qb", "feature")
        feature = self.code_change()
        self.git("checkout", "integration")
        self.write("Docs/guide.md", "# Target-only update\n"); target = self.commit()
        result = self.review(body(docs.NO_IMPACT, (), "本PR仅调整内部实现，接口和用法保持一致。"), base=target, head=feature)
        self.assertEqual([], result['changed_documents'])
        self.assertFalse(result['errors'])

    def test_worktree_advice_includes_staged_unstaged_and_untracked_files(self):
        self.write("src/feature.py", "value = 3\n"); self.git("add", "src/feature.py")
        self.write("Docs/guide.md", "# Edited\n")
        self.write("Docs/new.md", "# New\n")
        result = self.review(None, worktree=True)
        self.assertEqual("REVIEW_NEEDED", result['status'])
        self.assertEqual(["Docs/guide.md", "Docs/new.md"], result['changed_documents'])
        self.assertIn("src/feature.py", result['changed_files'])

    def test_bad_refs_fail_instead_of_silently_skipping(self):
        with self.assertRaises(ValueError):
            self.review(body(), base="missing-ref")

    def test_pr_text_is_only_data_and_never_executed(self):
        self.code_change()
        text = body(docs.NO_IMPACT, (), "仅内部实现；$(touch injected) 和 `touch injected` 是文本。")
        self.assertFalse(self.review(text)['errors'])
        self.assertFalse((self.root/'injected').exists())

    def test_cli_checks_edited_event_body_and_writes_a_summary(self):
        head = self.code_change()
        event = self.root/'event.json'; summary = self.root/'summary.md'
        event.write_text(json.dumps({'action':'edited', 'pull_request':{'base':{'sha':self.base},
            'head':{'sha':head},'body':body(docs.NO_IMPACT, (), "内部错误处理整理，没有接口、操作或配置变化。")}}),encoding='utf-8')
        result = subprocess.run([sys.executable, str(ROOT/'scripts/check_documentation_sync.py'),
            '--root', str(self.root), '--event', str(event), '--summary', str(summary)],
            capture_output=True, cwd=self.root, env=self.environment, timeout=20)
        self.assertEqual(0,result.returncode,result.stderr.decode('utf-8',errors='replace'))
        self.assertEqual('PASSED',json.loads(result.stdout)['status'])
        self.assertIn('Documentation sync: PASSED',summary.read_text(encoding='utf-8'))

    def test_empty_event_body_fails_without_a_traceback(self):
        event=self.root/'event.json'
        event.write_text(json.dumps({'pull_request':{'base':{'sha':self.base},'head':{'sha':self.base},'body':None}}),encoding='utf-8')
        result=subprocess.run([sys.executable,str(ROOT/'scripts/check_documentation_sync.py'),
            '--root',str(self.root),'--event',str(event)],capture_output=True,cwd=self.root,timeout=20)
        self.assertEqual(1,result.returncode)
        self.assertEqual('FAILED',json.loads(result.stdout)['status'])

    def test_summary_keeps_untrusted_markdown_inside_a_longer_code_fence(self):
        result={'status':'FAILED','changed_files':[], 'changed_documents':['Docs/```![image](x).md'],
                'suggested_documents':['<img src=x>'], 'errors':['```\n## injected']}
        rendered=docs.summary(result)
        self.assertIn('````text',rendered)
        self.assertIn('"Docs/```![image](x).md"',rendered)

    def test_workflow_is_read_only_and_rechecks_body_edits(self):
        workflow=(ROOT/'.github/workflows/documentation-sync.yml').read_text(encoding='utf-8')
        self.assertIn('ready_for_review, edited',workflow)
        self.assertIn('persist-credentials: false',workflow)
        self.assertIn('contents: read',workflow)
        self.assertIn('--event "$GITHUB_EVENT_PATH" --summary "$GITHUB_STEP_SUMMARY"',workflow)
        self.assertNotIn('pull_request_target',workflow)
        self.assertNotIn('pull-requests: write',workflow)
        self.assertNotIn('github.event.pull_request.body',workflow)

    def test_current_pr_template_accepts_a_completed_documentation_declaration(self):
        self.write('Docs/guide.md', '# Updated guide\n'); self.commit()
        template=(ROOT/'.github/pull_request_template.md').read_text(encoding='utf-8')
        text=template.replace('- [ ] '+docs.UPDATED,'- [x] '+docs.UPDATED)
        text=text.replace('文档路径：\n','文档路径：\n- `Docs/guide.md`\n')
        text=text.replace('说明：\n','说明：已同步本次行为与恢复说明。\n')
        self.assertFalse(self.review(text)['errors'])


if __name__ == '__main__':
    unittest.main()
