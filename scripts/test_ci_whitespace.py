"""Exercise the actual workflow PowerShell against isolated Git histories."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


class WhitespaceWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pwsh = shutil.which("pwsh")
        if not cls.pwsh:
            raise RuntimeError("PowerShell 7 is required, as in the Windows guardrails job")
        workflow = Path(__file__).resolve().parents[1] / ".github/workflows/ai-integration-guardrails.yml"
        step = workflow.read_text(encoding="utf-8").split(
            "      - name: Check committed whitespace errors\n", 1)[1]
        cls.script = textwrap.dedent(step.split("        run: |\n", 1)[1].split("\n      - ", 1)[0])

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ci-whitespace-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.environment = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull,
                                GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0")
        self.git("init", "-q", "--initial-branch=integration")
        self.git("config", "user.name", "Whitespace Test")
        self.git("config", "user.email", "whitespace@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "commit.gpgsign", "false")
        self.script_path = self.root / "check.ps1"
        self.script_path.write_text("$ErrorActionPreference = 'Stop'\n" + self.script, encoding="utf-8")

    def git(self, *arguments):
        result = subprocess.run(["git", *arguments], cwd=self.repo, env=self.environment,
                                capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        return result.stdout.strip()

    def commit(self, path="file.txt", content=b"clean\n"):
        file = self.repo / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)
        self.git("add", "--", path)
        self.git("commit", "-qm", path)
        return self.git("rev-parse", "HEAD")

    def run_check(self, *, head, base="", source="codex/task/account", candidate=None,
                  event="pull_request", before=""):
        environment = dict(self.environment, EVENT_NAME=event, BASE_SHA=base,
                           PR_HEAD_SHA=head if event == "pull_request" else "",
                           PR_HEAD_REF=source, HEAD_SHA=candidate or head, BEFORE_SHA=before)
        return subprocess.run([self.pwsh, "-NoLogo", "-NoProfile", "-NonInteractive",
                               "-File", str(self.script_path)], cwd=self.repo, env=environment,
                              capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

    def synced_history(self, feature_content=b"account\n"):
        start = self.commit()
        self.git("checkout", "-qb", "feature")
        self.commit("account.cpp", feature_content)
        self.git("checkout", "integration")
        self.commit("old-integration.txt", b"old baseline\n")
        self.git("checkout", "feature")
        self.git("merge", "--no-ff", "--no-edit", "integration")
        old_merge = self.git("rev-parse", "HEAD")
        self.git("checkout", "integration")
        target = self.commit("upstream.cpp", b"upstream content \n")
        self.git("checkout", "feature")
        self.git("merge", "--no-ff", "--no-edit", "integration")
        return start, old_merge, target, self.git("rev-parse", "HEAD")

    def test_syncing_beta_does_not_recheck_its_existing_whitespace(self):
        _, old_merge, target, head = self.synced_history()
        old_check = subprocess.run(["git", "diff", "--check", old_merge, head],
                                   cwd=self.repo, env=self.environment, capture_output=True)
        self.assertNotEqual(0, old_check.returncode, "Fixture must reproduce the old false failure")
        result = self.run_check(head=head, base=target)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_syncing_beta_still_rejects_feature_whitespace(self):
        _, _, target, head = self.synced_history(b"new account whitespace \n")
        result = self.run_check(head=head, base=target)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("account.cpp", result.stdout + result.stderr)

    def test_advanced_target_uses_common_base(self):
        self.commit(content=b"existing whitespace \n")
        self.git("checkout", "-qb", "feature")
        head = self.commit("account.cpp", b"account\n")
        self.git("checkout", "integration")
        target = self.commit(content=b"existing whitespace\n")
        result = self.run_check(head=head, base=target)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_whitespace_added_by_synthetic_merge_is_rejected(self):
        self.commit()
        self.git("checkout", "-qb", "feature")
        head = self.commit("account.cpp", b"account\n")
        self.git("checkout", "integration")
        base = self.commit("integration.txt", b"target change\n")
        self.git("merge", "--no-ff", "--no-edit", "feature")
        (self.repo / "resolution.cpp").write_bytes(b"merge resolution \n")
        self.git("add", "resolution.cpp")
        tree = self.git("write-tree")
        candidate = self.git("commit-tree", tree, "-p", base, "-p", head, "-m", "merge resolution")
        result = self.run_check(head=head, base=base, candidate=candidate)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Merge-resolution whitespace validation failed", result.stdout + result.stderr)

    def upstream_history(self):
        base = self.commit()
        self.git("checkout", "-qb", "upstream")
        self.commit("upstream.cpp", b"official whitespace \n")
        self.git("checkout", "integration")
        self.git("merge", "--no-ff", "--no-edit", "upstream")
        return base

    def test_dedicated_upstream_import_retains_official_whitespace_exception(self):
        base = self.upstream_history()
        head = self.commit("compatibility.cpp", b"compatibility fix\n")
        result = self.run_check(head=head, base=base, source="codex/upstream-v2.5.0-beta")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_upstream_fixes_after_import_still_reject_new_whitespace(self):
        base = self.upstream_history()
        head = self.commit("compatibility.cpp", b"new whitespace \n")
        result = self.run_check(head=head, base=base, source="codex/upstream-sync-beta")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("compatibility.cpp", result.stdout + result.stderr)

    def test_regular_feature_merge_cannot_use_upstream_exception(self):
        base = self.upstream_history()
        head = self.git("rev-parse", "HEAD")
        result = self.run_check(head=head, base=base)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("upstream.cpp", result.stdout + result.stderr)

    def test_push_still_rejects_new_whitespace(self):
        before = self.commit()
        head = self.commit("new.cpp", b"new whitespace \n")
        result = self.run_check(head=head, event="push", before=before)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("new.cpp", result.stdout + result.stderr)

    def test_initial_push_still_rejects_new_whitespace(self):
        head = self.commit(content=b"new whitespace \n")
        result = self.run_check(head=head, event="push", before="0" * 40)
        self.assertNotEqual(0, result.returncode)

    def test_merge_queue_still_checks_candidate_delta(self):
        base = self.commit()
        head = self.commit("new.cpp", b"new whitespace \n")
        result = self.run_check(head=head, base=base, event="merge_group")
        self.assertNotEqual(0, result.returncode)

    def test_invalid_pr_baseline_fails(self):
        head = self.commit()
        result = self.run_check(head=head, base="f" * 40)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Cannot resolve PR whitespace baseline", result.stdout + result.stderr)

    def test_clean_crlf_lines_pass(self):
        base = self.commit()
        head = self.commit("windows.cpp", b"first\r\nsecond\r\n")
        result = self.run_check(head=head, base=base)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_spaces_before_crlf_still_fail(self):
        base = self.commit()
        head = self.commit("windows.cpp", b"first \r\n")
        result = self.run_check(head=head, base=base)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("windows.cpp", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
